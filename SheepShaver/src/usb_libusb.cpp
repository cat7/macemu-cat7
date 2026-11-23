/*
 *  usb_libusb.cpp - Host USB devices through libusb (experimental)
 *
 *  SheepShaver (C) Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

/*
 *  The devices named by "usbdevice VVVV:PPPP" prefs are passed through, in
 *  pref order. A device libusb can open (on Windows: bound to WinUSB, e.g.
 *  with winusb-switch, or a HID device) gets its control requests forwarded.
 *  A device it cannot open is served in "descriptors only" mode: the Mac
 *  gets its real device and configuration descriptors, which the host has
 *  cached, and simple requests are accepted, so it can be enumerated
 *  without changing its host driver.
 */

#include "sysdeps.h"

#ifdef HAVE_LIBUSB

#include <libusb.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>

#include "main.h"
#include "prefs.h"
#include "usb_host.h"

class LibUSBHost : public USBHostBackend {
public:
	LibUSBHost() : ctx(NULL), started(false), quit(false) {}
	virtual ~LibUSBHost();

	virtual int count(void) { start(); return (int)devs.size(); }
	virtual bool low_speed(int dev) { return valid(dev) && devs[dev].low_speed; }
	virtual const char *name(int dev) { return valid(dev) ? devs[dev].label.c_str() : "?"; }
	virtual int control(int dev, const uint8 *setup, uint8 *data, int length);
	virtual bool submit(int dev, uint8 ep, int type, uint8 *data, int length, uint32 tag);
	virtual bool reap(uint32 *tag, int *result, bool *cancelled);
	virtual void cancel(int dev, uint8 ep);
	virtual void clear_halt(int dev, uint8 ep);

private:
	struct Device {
		libusb_device *dev;
		libusb_device_handle *handle;	// NULL: descriptors only
		bool low_speed;
		bool high_speed;			// faster than the Mac's USB 1.1 bus
		std::string label;
	};

	void start(void);
	bool valid(int dev) { return dev >= 0 && dev < (int)devs.size(); }
	int cached_request(Device &d, const uint8 *setup, uint8 *data, int length);
	static int config_bytes(libusb_device *dev, int index, std::vector<uint8> &out);
	static void clamp_bulk_packets(std::vector<uint8> &config);
	static int map_error(int err) { return err == LIBUSB_ERROR_PIPE ? USB_HOST_STALL : USB_HOST_ERROR; }

	libusb_context *ctx;
	bool started;
	std::vector<Device> devs;

	// Asynchronous transfers: libusb events are handled in their own thread
	// (Windows has no pollable libusb file descriptors); finished transfers
	// wait in a queue until the emulator reaps them.
	struct Pending {
		libusb_transfer *xfer;
		int dev;
		uint8 ep;
		uint32 tag;
	};
	struct Done {
		uint32 tag;
		int result;
		bool cancelled;
	};
	std::mutex lock;
	std::vector<Pending> pending;
	std::deque<Done> done;
	std::thread events;
	std::atomic<bool> quit;
	void event_loop(void);
	static void LIBUSB_CALL transfer_done(libusb_transfer *xfer);
};

LibUSBHost::~LibUSBHost()
{
	if (events.joinable()) {
		quit = true;
		events.detach();		// at exit: do not wait for libusb
		return;
	}
	for (size_t i = 0; i < devs.size(); i++) {
		if (devs[i].handle)
			libusb_close(devs[i].handle);
		libusb_unref_device(devs[i].dev);
	}
	if (ctx)
		libusb_exit(ctx);
}

void LibUSBHost::start(void)
{
	if (started)
		return;
	started = true;

	std::vector<std::pair<unsigned, unsigned> > wanted;
	const char *str;
	for (int i = 0; (str = PrefsFindString("usbdevice", i)) != NULL; i++) {
		unsigned vid, pid;
		if (sscanf(str, "%x:%x", &vid, &pid) == 2)
			wanted.push_back(std::make_pair(vid, pid));
	}
	if (wanted.empty() || libusb_init(&ctx) != 0)
		return;

	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	for (size_t w = 0; w < wanted.size(); w++) {
		for (ssize_t i = 0; i < n; i++) {
			libusb_device_descriptor dd;
			if (libusb_get_device_descriptor(list[i], &dd) != 0
			 || dd.idVendor != wanted[w].first || dd.idProduct != wanted[w].second)
				continue;
			Device d;
			d.dev = libusb_ref_device(list[i]);
			d.handle = NULL;
			int speed = libusb_get_device_speed(list[i]);
			d.low_speed = speed == LIBUSB_SPEED_LOW;
			d.high_speed = speed >= LIBUSB_SPEED_HIGH;
			int err = libusb_open(list[i], &d.handle);
			if (err != 0)
				d.handle = NULL;
			char buf[64];
			snprintf(buf, sizeof(buf), "%04x:%04x %s%s", dd.idVendor, dd.idProduct,
			         d.low_speed ? "low speed" : d.high_speed ? "high speed" : "full speed",
			         d.handle ? "" : ", descriptors only");
			d.label = buf;
			devs.push_back(d);
			break;
		}
	}
	libusb_free_device_list(list, 1);

	for (size_t i = 0; i < devs.size(); i++)
		if (devs[i].handle) {
			events = std::thread(&LibUSBHost::event_loop, this);
			break;
		}
}

void LibUSBHost::event_loop(void)
{
	while (!quit) {
		struct timeval tv = {0, 100000};
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
	}
}

void LIBUSB_CALL LibUSBHost::transfer_done(libusb_transfer *xfer)
{
	LibUSBHost *self = (LibUSBHost *)xfer->user_data;
	Done d;
	d.cancelled = xfer->status == LIBUSB_TRANSFER_CANCELLED;
	switch (xfer->status) {
		case LIBUSB_TRANSFER_COMPLETED: d.result = xfer->actual_length; break;
		case LIBUSB_TRANSFER_STALL: d.result = USB_HOST_STALL; break;
		case LIBUSB_TRANSFER_CANCELLED: d.result = xfer->actual_length; break;
		default: d.result = USB_HOST_ERROR; break;
	}
	std::lock_guard<std::mutex> g(self->lock);
	for (size_t i = 0; i < self->pending.size(); i++)
		if (self->pending[i].xfer == xfer) {
			d.tag = self->pending[i].tag;
			self->pending.erase(self->pending.begin() + i);
			self->done.push_back(d);
			SetInterruptFlag(INTFLAG_USB);		// like a host controller interrupt
			TriggerInterrupt();
			break;
		}
	libusb_free_transfer(xfer);
}

bool LibUSBHost::submit(int dev, uint8 ep, int type, uint8 *data, int length, uint32 tag)
{
	start();
	if (!valid(dev) || devs[dev].handle == NULL)
		return false;
	libusb_transfer *xfer = libusb_alloc_transfer(0);
	if (xfer == NULL)
		return false;
	if (type == INTERRUPT)
		libusb_fill_interrupt_transfer(xfer, devs[dev].handle, ep, data, length, transfer_done, this, 0);
	else
		libusb_fill_bulk_transfer(xfer, devs[dev].handle, ep, data, length, transfer_done, this, 0);
	Pending p = {xfer, dev, ep, tag};
	{
		std::lock_guard<std::mutex> g(lock);
		pending.push_back(p);
	}
	if (libusb_submit_transfer(xfer) != 0) {
		std::lock_guard<std::mutex> g(lock);
		pending.pop_back();
		libusb_free_transfer(xfer);
		return false;
	}
	return true;
}

bool LibUSBHost::reap(uint32 *tag, int *result, bool *cancelled)
{
	std::lock_guard<std::mutex> g(lock);
	if (done.empty())
		return false;
	*tag = done.front().tag;
	*result = done.front().result;
	*cancelled = done.front().cancelled;
	done.pop_front();
	return true;
}

void LibUSBHost::cancel(int dev, uint8 ep)
{
	std::lock_guard<std::mutex> g(lock);
	for (size_t i = 0; i < pending.size(); i++)
		if (pending[i].dev == dev && pending[i].ep == ep)
			libusb_cancel_transfer(pending[i].xfer);
}

void LibUSBHost::clear_halt(int dev, uint8 ep)
{
	if (valid(dev) && devs[dev].handle)
		libusb_clear_halt(devs[dev].handle, ep);
}

// Configuration descriptor as the device sends it, rebuilt from libusb's
// parsed copy (available without opening the device)
int LibUSBHost::config_bytes(libusb_device *dev, int index, std::vector<uint8> &out)
{
	libusb_config_descriptor *cd;
	if (libusb_get_config_descriptor(dev, index, &cd) != 0)
		return USB_HOST_STALL;
	out.clear();
	uint8 head[9] = {9, LIBUSB_DT_CONFIG, 0, 0, cd->bNumInterfaces, cd->bConfigurationValue,
	                 cd->iConfiguration, cd->bmAttributes, cd->MaxPower};
	out.insert(out.end(), head, head + 9);
	out.insert(out.end(), cd->extra, cd->extra + cd->extra_length);
	for (int i = 0; i < cd->bNumInterfaces; i++) {
		const libusb_interface &itf = cd->interface[i];
		for (int a = 0; a < itf.num_altsetting; a++) {
			const libusb_interface_descriptor &id = itf.altsetting[a];
			uint8 ib[9] = {9, LIBUSB_DT_INTERFACE, id.bInterfaceNumber, id.bAlternateSetting,
			               id.bNumEndpoints, id.bInterfaceClass, id.bInterfaceSubClass,
			               id.bInterfaceProtocol, id.iInterface};
			out.insert(out.end(), ib, ib + 9);
			out.insert(out.end(), id.extra, id.extra + id.extra_length);
			for (int e = 0; e < id.bNumEndpoints; e++) {
				const libusb_endpoint_descriptor &ed = id.endpoint[e];
				uint8 eb[9] = {ed.bLength, LIBUSB_DT_ENDPOINT, ed.bEndpointAddress, ed.bmAttributes,
				               (uint8)(ed.wMaxPacketSize & 0xff), (uint8)(ed.wMaxPacketSize >> 8),
				               ed.bInterval, ed.bRefresh, ed.bSynchAddress};
				out.insert(out.end(), eb, eb + (ed.bLength >= 9 ? 9 : 7));
				out.insert(out.end(), ed.extra, ed.extra + ed.extra_length);
			}
		}
	}
	out[2] = out.size() & 0xff;
	out[3] = out.size() >> 8;
	libusb_free_config_descriptor(cd);
	return (int)out.size();
}

// A high-speed device on the Mac's full-speed bus: its configuration lists
// bulk endpoints with 512-byte packets, which full speed does not allow
// (Mac OS's OHCI stack builds them with a max packet size of 0). Clamp
// them to 64, as in QEMU's host-libusb.
void LibUSBHost::clamp_bulk_packets(std::vector<uint8> &config)
{
	for (size_t i = 0; i + 6 < config.size() && config[i]; i += config[i]) {
		if (config[i + 1] == LIBUSB_DT_ENDPOINT && (config[i + 3] & 3) == LIBUSB_TRANSFER_TYPE_BULK
		 && (config[i + 4] | (config[i + 5] << 8)) > 64) {
			config[i + 4] = 64;
			config[i + 5] = 0;
		}
	}
}

// Descriptors-only mode: answer from the host's cached descriptors
int LibUSBHost::cached_request(Device &d, const uint8 *setup, uint8 *data, int length)
{
	uint8 type = setup[0], req = setup[1];
	uint16 value = setup[2] | (setup[3] << 8);

	if (type == 0x80 && req == LIBUSB_REQUEST_GET_DESCRIPTOR) {
		std::vector<uint8> bytes;
		if ((value >> 8) == LIBUSB_DT_DEVICE) {
			libusb_device_descriptor dd;
			if (libusb_get_device_descriptor(d.dev, &dd) != 0)
				return USB_HOST_ERROR;
			uint8 b[18] = {18, LIBUSB_DT_DEVICE, (uint8)(dd.bcdUSB & 0xff), (uint8)(dd.bcdUSB >> 8),
			               dd.bDeviceClass, dd.bDeviceSubClass, dd.bDeviceProtocol, dd.bMaxPacketSize0,
			               (uint8)(dd.idVendor & 0xff), (uint8)(dd.idVendor >> 8),
			               (uint8)(dd.idProduct & 0xff), (uint8)(dd.idProduct >> 8),
			               (uint8)(dd.bcdDevice & 0xff), (uint8)(dd.bcdDevice >> 8),
			               0, 0, 0,		// no string descriptors in this mode
			               dd.bNumConfigurations};
			bytes.assign(b, b + 18);
		} else if ((value >> 8) == LIBUSB_DT_CONFIG) {
			int r = config_bytes(d.dev, value & 0xff, bytes);
			if (r < 0)
				return r;
			if (d.high_speed)
				clamp_bulk_packets(bytes);
			for (size_t i = 0; i + 1 < bytes.size(); i += bytes[i] ? bytes[i] : bytes.size()) {
				if (bytes[i + 1] == LIBUSB_DT_CONFIG) bytes[i + 6] = 0;		// iConfiguration
				if (bytes[i + 1] == LIBUSB_DT_INTERFACE) bytes[i + 8] = 0;	// iInterface
			}
		} else
			return USB_HOST_STALL;		// strings and class descriptors are not cached
		int n = (int)bytes.size() < length ? (int)bytes.size() : length;
		memcpy(data, &bytes[0], n);
		return n;
	}
	if (type == 0x80 && req == LIBUSB_REQUEST_GET_STATUS) {
		if (length >= 2) { data[0] = data[1] = 0; return 2; }
		return 0;
	}
	if (type & 0x80)				// other IN requests have no cached answer
		return USB_HOST_STALL;
	return 0;						// OUT requests (configuration, features, ...) are accepted
}

int LibUSBHost::control(int dev, const uint8 *setup, uint8 *data, int length)
{
	start();
	if (!valid(dev))
		return USB_HOST_ERROR;
	Device &d = devs[dev];
	if (d.handle == NULL)
		return cached_request(d, setup, data, length);

	uint8 type = setup[0], req = setup[1];
	uint16 value = setup[2] | (setup[3] << 8), index = setup[4] | (setup[5] << 8);

	if (type == 0x00 && req == LIBUSB_REQUEST_SET_CONFIGURATION) {
		// Let libusb select the configuration and claim its interfaces, so
		// that transfers to their endpoints work later
		int err = libusb_set_configuration(d.handle, value);
		if (err != 0 && err != LIBUSB_ERROR_NOT_SUPPORTED)
			return map_error(err);
		libusb_config_descriptor *cd;
		if (value && libusb_get_active_config_descriptor(d.dev, &cd) == 0) {
			for (int i = 0; i < cd->bNumInterfaces; i++)
				libusb_claim_interface(d.handle, cd->interface[i].altsetting[0].bInterfaceNumber);
			libusb_free_config_descriptor(cd);
		}
		return 0;
	}
	if (type == 0x01 && req == LIBUSB_REQUEST_SET_INTERFACE) {
		int err = libusb_set_interface_alt_setting(d.handle, index, value);
		return err == 0 ? 0 : map_error(err);
	}
	if (type == 0x02 && req == LIBUSB_REQUEST_CLEAR_FEATURE && value == 0) {
		int err = libusb_clear_halt(d.handle, index & 0xff);
		return err == 0 ? 0 : map_error(err);
	}
	int r = libusb_control_transfer(d.handle, type, req, value, index, data, length, 2000);
	if (r > 0 && d.high_speed && type == 0x80 && req == LIBUSB_REQUEST_GET_DESCRIPTOR && (value >> 8) == LIBUSB_DT_CONFIG) {
		std::vector<uint8> bytes(data, data + r);
		clamp_bulk_packets(bytes);
		memcpy(data, &bytes[0], r);
	}
	return r >= 0 ? r : map_error(r);
}

// Register as the backend (USBHost itself lives in name_registry.cpp, so
// builds without libusb link too)
static LibUSBHost the_libusb_host;
static struct LibUSBHostRegister {
	LibUSBHostRegister() { USBHost = &the_libusb_host; }
} the_libusb_host_register;

#endif
