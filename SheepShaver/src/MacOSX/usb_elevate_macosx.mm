/*
 *  usb_elevate_macosx.mm - Restart as root for USB passthrough (macOS)
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
 *  macOS gives a USB device that has a macOS driver (a disk, a camera,
 *  audio, HID) to another process only when that process runs as root.
 *  So when devices are to be passed through ("usbuim" and "usbdevice"
 *  prefs) and one of them is plugged in, SheepShaver asks for an
 *  administrator password once (sudo -A, with SheepShaver itself as the
 *  askpass helper) and starts itself again as root through sudo, with the
 *  same arguments, working directory and home folder; the first copy then
 *  quits. Cancelling the prompt keeps the first copy running without root,
 *  as before: devices without a macOS driver still work.
 *
 *  The root copy hands the prefs and XPRAM files back to the user when it
 *  quits, in case it created them.
 */

#include "sysdeps.h"

#if defined(HAVE_LIBUSB)

#import <Cocoa/Cocoa.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <libusb.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "prefs.h"
#include "usb_elevate_macosx.h"

static const char ELEVATED_ENV[] = "SHEEPSHAVER_USB_ELEVATED";	// "uid:gid" of the user

// Is one of the devices to pass through plugged in now?
static bool wanted_device_present(void)
{
	std::vector<std::pair<unsigned, unsigned> > wanted;
	const char *str;
	for (int i = 0; (str = PrefsFindString("usbdevice", i)) != NULL; i++) {
		unsigned vid, pid;
		if (sscanf(str, "%x:%x", &vid, &pid) == 2)
			wanted.push_back(std::make_pair(vid, pid));
	}
	if (wanted.empty())
		return false;

	libusb_context *ctx;
	if (libusb_init(&ctx) != 0)
		return false;
	bool found = false;
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	for (ssize_t i = 0; i < n && !found; i++) {
		libusb_device_descriptor dd;
		if (libusb_get_device_descriptor(list[i], &dd) != 0)
			continue;
		for (size_t w = 0; w < wanted.size() && !found; w++)
			found = dd.idVendor == wanted[w].first && dd.idProduct == wanted[w].second;
	}
	if (n >= 0)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return found;
}

// Run a program and wait for it; its exit status, or -1
static int run_and_wait(const char *path, const std::vector<std::string> &args, const std::vector<std::string> &env)
{
	std::vector<char *> argv, envp;
	for (size_t i = 0; i < args.size(); i++)
		argv.push_back(const_cast<char *>(args[i].c_str()));
	argv.push_back(NULL);
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back(const_cast<char *>(env[i].c_str()));
	envp.push_back(NULL);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	pid_t pid;
	int err = posix_spawn(&pid, path, &fa, NULL, &argv[0], &envp[0]);
	posix_spawn_file_actions_destroy(&fa);
	if (err != 0)
		return -1;
	int status;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static const char ASKPASS_ENV[] = "SHEEPSHAVER_ASKPASS";

/*
 *  Password prompt for "sudo -A": SheepShaver runs itself as sudo's askpass
 *  helper. It prints the password on stdout, or exits with 1 on Cancel.
 */

void USBAskpassMainIfRequested(void)
{
	if (getenv(ASKPASS_ENV) == NULL)
		return;
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		[NSApp finishLaunching];
		[NSApp activateIgnoringOtherApps:YES];
		NSAlert *alert = [[[NSAlert alloc] init] autorelease];
		[alert setMessageText:@"SheepShaver wants to pass USB devices through to Mac OS"];
		[alert setInformativeText:@"macOS gives devices it has drivers for (disks, cameras, audio) only to a program "
		                          @"running as administrator. Enter your password to start SheepShaver that way."];
		[alert addButtonWithTitle:@"OK"];
		[alert addButtonWithTitle:@"Cancel"];
		NSSecureTextField *field = [[[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)] autorelease];
		[field setPlaceholderString:@"Password"];
		[alert setAccessoryView:field];
		[[alert window] setInitialFirstResponder:field];
		[[alert window] setLevel:NSModalPanelWindowLevel];
		if ([alert runModal] != NSAlertFirstButtonReturn)
			exit(1);
		printf("%s\n", [[field stringValue] UTF8String]);
		fflush(stdout);
	}
	exit(0);
}

bool USBRelaunchAsRootIfNeeded(const std::vector<std::string> &args)
{
	if (geteuid() == 0 || getenv(ELEVATED_ENV) != NULL)
		return false;
	if (!PrefsFindBool("usbuim") || !wanted_device_present())
		return false;

	char exe[PATH_MAX], real_exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0 || realpath(exe, real_exe) == NULL)
		return false;

	// sudo, not an authorization dialog: a process started by sudo stays in
	// the user's login session, so macOS still applies the privacy settings
	// the user gave SheepShaver (external disks, microphone). A root process
	// started through Authorization Services has no session and is refused.
	std::vector<std::string> env;
	env.push_back(std::string("SUDO_ASKPASS=") + real_exe);
	env.push_back(std::string(ASKPASS_ENV) + "=1");
	const char *keep[] = {"HOME", "USER", "LOGNAME", "XDG_CONFIG_HOME", "LANG", "TMPDIR", NULL};
	for (int i = 0; keep[i]; i++) {
		const char *v = getenv(keep[i]);
		if (v)
			env.push_back(std::string(keep[i]) + "=" + v);
	}

	// 1. Ask for the password (sudo remembers it for a few minutes)
	std::vector<std::string> validate;
	validate.push_back("sudo");
	validate.push_back("-A");
	validate.push_back("-v");
	int r = run_and_wait("/usr/bin/sudo", validate, env);
	if (r != 0) {
		fprintf(stderr, "USB: not running as root, devices with macOS drivers stay with macOS\n");
		return false;
	}

	// 2. Start the root copy: sudo -n env HOME=... SHEEPSHAVER_USB_ELEVATED=uid:gid <exe> <args>
	std::vector<std::string> cmd;
	cmd.push_back("sudo");
	cmd.push_back("-n");
	cmd.push_back("/usr/bin/env");
	for (int i = 0; keep[i]; i++) {
		const char *v = getenv(keep[i]);
		if (v)
			cmd.push_back(std::string(keep[i]) + "=" + v);
	}
	char ids[48];
	snprintf(ids, sizeof(ids), "%s=%u:%u", ELEVATED_ENV, (unsigned)getuid(), (unsigned)getgid());
	cmd.push_back(ids);
	cmd.push_back(real_exe);
	for (size_t i = 1; i < args.size(); i++)
		if (args[i].compare(0, 5, "-psn_") != 0)	// Finder's process serial number
			cmd.push_back(args[i]);

	std::vector<char *> argv, envp;
	for (size_t i = 0; i < cmd.size(); i++)
		argv.push_back(const_cast<char *>(cmd[i].c_str()));
	argv.push_back(NULL);
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back(const_cast<char *>(env[i].c_str()));
	envp.push_back(NULL);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	pid_t pid;
	int err = posix_spawn(&pid, "/usr/bin/sudo", &fa, NULL, &argv[0], &envp[0]);
	posix_spawn_file_actions_destroy(&fa);
	if (err != 0) {
		fprintf(stderr, "USB: could not start sudo: %s\n", strerror(err));
		return false;
	}

	// sudo -n fails at once if it has no credentials; otherwise it runs the copy
	for (int i = 0; i < 20; i++) {
		int status;
		if (waitpid(pid, &status, WNOHANG) == pid) {
			fprintf(stderr, "USB: could not start SheepShaver as root\n");
			return false;
		}
		usleep(50000);
	}
	return true;
}

// In the root copy: give files we created back to the user
static std::vector<std::string> owned_files;
static uid_t user_uid;
static gid_t user_gid;

static void give_back_files(void)
{
	for (size_t i = 0; i < owned_files.size(); i++) {
		struct stat st;
		if (stat(owned_files[i].c_str(), &st) == 0 && st.st_uid == 0)
			chown(owned_files[i].c_str(), user_uid, user_gid);
	}
}

void USBElevatedOwnFiles(const std::vector<std::string> &files)
{
	const char *ids = getenv(ELEVATED_ENV);
	unsigned uid, gid;
	if (geteuid() != 0 || ids == NULL || sscanf(ids, "%u:%u", &uid, &gid) != 2)
		return;
	user_uid = uid;
	user_gid = gid;
	owned_files = files;
	atexit(give_back_files);
}

#endif // HAVE_LIBUSB
