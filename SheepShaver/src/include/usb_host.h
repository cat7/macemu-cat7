/*
 *  usb_host.h - Host USB devices passed through to the Mac (experimental)
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

#ifndef USB_HOST_H
#define USB_HOST_H

// Result codes of USBHostBackend::control()
enum {
	USB_HOST_STALL = -1,		// the device stalled the request
	USB_HOST_ERROR = -2			// any other failure (no response, ...)
};

// Host side of USB passthrough: the devices handed to the Mac, one per
// root hub port. The UIM (name_registry.cpp) uses this interface only, so
// it does not depend on the host USB library.
class USBHostBackend {
public:
	virtual ~USBHostBackend() {}

	// Number of passed-through device slots (ports 1..count on the root hub);
	// a slot's device may come and go (hotplug)
	virtual int count(void) = 0;

	// Device is plugged in (and can be enumerated)
	virtual bool present(int dev) = 0;

	// Apply plug/unplug events found by the host; returns a bit mask of the
	// slots whose presence changed since the last call. Call from the
	// emulator thread only (INTFLAG_USB is raised when there is news).
	virtual uint32 changes(void) = 0;

	// Device is low speed (1.5 Mbit/s)
	virtual bool low_speed(int dev) = 0;

	// Short description for logs
	virtual const char *name(int dev) = 0;

	// Run a control request on endpoint 0. setup is the 8-byte request;
	// data holds wLength bytes (IN: filled in, OUT: sent). Returns the
	// number of data bytes transferred, or USB_HOST_STALL / USB_HOST_ERROR.
	virtual int control(int dev, const uint8 *setup, uint8 *data, int length) = 0;

	// Start a bulk or interrupt transfer on endpoint ep (bit 7 set: IN).
	// data (length bytes) must stay valid until the transfer is reaped.
	// Returns false if it could not be started. Never blocks.
	enum { BULK = 2, INTERRUPT = 3 };
	virtual bool submit(int dev, uint8 ep, int type, uint8 *data, int length, uint32 tag) = 0;

	// Fetch one finished transfer: its tag, result (bytes transferred or
	// USB_HOST_STALL / USB_HOST_ERROR) and whether it was cancelled.
	// Returns false when none is waiting. Safe to call from the emulator.
	virtual bool reap(uint32 *tag, int *result, bool *cancelled) = 0;

	// Cancel the transfers on an endpoint (they are reaped as cancelled)
	virtual void cancel(int dev, uint8 ep) = 0;

	// Clear a halt (stall) on an endpoint
	virtual void clear_halt(int dev, uint8 ep) = 0;
};

// The backend in use, or NULL when the build has none
extern USBHostBackend *USBHost;

#endif
