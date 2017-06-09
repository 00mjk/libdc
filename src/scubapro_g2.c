/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *           (C) 2017 Linus Torvalds
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include "scubapro_g2.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "usbhid.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &scubapro_g2_device_vtable)

typedef struct scubapro_g2_device_t {
	dc_device_t base;
	dc_usbhid_t *usbhid;
	unsigned int address;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} scubapro_g2_device_t;

static dc_status_t scubapro_g2_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t scubapro_g2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t scubapro_g2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t scubapro_g2_device_close (dc_device_t *abstract);

static const dc_device_vtable_t scubapro_g2_device_vtable = {
	sizeof(scubapro_g2_device_t),
	DC_FAMILY_SCUBAPRO_G2,
	scubapro_g2_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	scubapro_g2_device_dump, /* dump */
	scubapro_g2_device_foreach, /* foreach */
	scubapro_g2_device_close /* close */
};

static dc_status_t
scubapro_g2_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

#define PACKET_SIZE 64
static int receive_data(scubapro_g2_device_t *g2, unsigned char *buffer, int size)
{
	while (size) {
		unsigned char buf[PACKET_SIZE];
		size_t transferred = 0;
		dc_status_t rc;
		int len;

		rc = dc_usbhid_read(g2->usbhid, buf, PACKET_SIZE, &transferred);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR(g2->base.context, "read interrupt transfer failed");
			return -1;
		}
		if (transferred != PACKET_SIZE) {
			ERROR(g2->base.context, "incomplete read interrupt transfer (got %zu, expected %d)", transferred, PACKET_SIZE);
			return -1;
		}
		len = buf[0];
		if (len >= PACKET_SIZE) {
			ERROR(g2->base.context, "read interrupt transfer returns impossible packet size (%d)", len);
			return -1;
		}
		HEXDUMP (g2->base.context, DC_LOGLEVEL_DEBUG, "rcv", buf+1, len);
		if (len > size) {
			ERROR(g2->base.context, "receive result buffer too small - truncating");
			len = size;
		}
		memcpy(buffer, buf+1, len);
		size -= len;
		buffer += len;
	}
	return 0;
}

static dc_status_t
scubapro_g2_transfer(scubapro_g2_device_t *g2, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned char buf[PACKET_SIZE];
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t transferred = 0;

	if (csize >= PACKET_SIZE) {
		ERROR(g2->base.context, "command too big (%d)", csize);
		return DC_STATUS_INVALIDARGS;
	}

	buf[0] = csize;
	memcpy(buf+1, command, csize);
	status = dc_usbhid_write(g2->usbhid, buf, csize+1, &transferred);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(g2->base.context, "Failed to send the command.");
		return status;
	}

	if (receive_data(g2, answer, asize) < 0) {
		ERROR(g2->base.context, "Failed to receive the answer.");
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
scubapro_g2_device_open(dc_device_t **out, dc_context_t *context)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	scubapro_g2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (scubapro_g2_device_t *) dc_device_allocate (context, &scubapro_g2_device_vtable);
	if (device == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->usbhid = NULL;
	device->address = 0;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Open the irda socket.
	status = dc_usbhid_open(&device->usbhid, context, 0x2e6c, 0x3201);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open USB device");
		goto error_free;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
scubapro_g2_device_close (dc_device_t *abstract)
{
	scubapro_g2_device_t *device = (scubapro_g2_device_t*) abstract;

	dc_usbhid_close(device->usbhid);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	scubapro_g2_device_t *device = (scubapro_g2_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	scubapro_g2_device_t *device = (scubapro_g2_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Read the model number.
	unsigned char cmd_model[1] = {0x10};
	unsigned char model[1] = {0};
	rc = scubapro_g2_transfer (device, cmd_model, sizeof (cmd_model), model, sizeof (model));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the serial number.
	unsigned char cmd_serial[1] = {0x14};
	unsigned char serial[4] = {0};
	rc = scubapro_g2_transfer (device, cmd_serial, sizeof (cmd_serial), serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the device clock.
	unsigned char cmd_devtime[1] = {0x1A};
	unsigned char devtime[4] = {0};
	rc = scubapro_g2_transfer (device, cmd_devtime, sizeof (cmd_devtime), devtime, sizeof (devtime));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (devtime);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Command template.
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	command[0] = 0xC6;
	unsigned char answer[4] = {0};
	rc = scubapro_g2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

  	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = scubapro_g2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	if (receive_data(device, data, length)) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_IO;
	}

	// Update and emit a progress event.
	progress.current += length;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = scubapro_g2_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = scubapro_g2_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
scubapro_g2_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
