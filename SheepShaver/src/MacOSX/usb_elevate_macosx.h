/*
 *  usb_elevate_macosx.h - Restart as root for USB passthrough (macOS)
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

#ifndef USB_ELEVATE_MACOSX_H
#define USB_ELEVATE_MACOSX_H

#include <string>
#include <vector>

// At the very start of main(): when started as sudo's askpass helper, ask
// for the password, print it and exit
extern void USBAskpassMainIfRequested(void);

// After the prefs are read: if devices are to be passed through and one is
// plugged in, ask for an administrator password and start a root copy with
// the same arguments. True if that copy runs (the caller then quits).
extern bool USBRelaunchAsRootIfNeeded(const std::vector<std::string> &args);

// In the root copy: hand these files back to the user at exit if root owns them
extern void USBElevatedOwnFiles(const std::vector<std::string> &files);

#endif
