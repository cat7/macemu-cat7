/*
 *  VMSettingsController.mm - Preferences editing in Cocoa on Mac OS X
 *
 *  Copyright (C) 2006-2010 Alexei Svitkine
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
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#import "sysdeps.h"
#import "prefs.h"

// The _UINT64 define is needed to guard against a typedef mismatch with Snow Leopard headers.
#define _UINT64

#import "VMSettingsController.h"

#import "DiskType.h"

#include <unistd.h>

#ifdef USE_SDL_VIDEO
#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#endif

// NSInteger was added in 10.5 SDK.
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1050
  #if __LP64__ || NS_BUILD_32_LIKE_64
    typedef long NSInteger;
  #else
    typedef int NSInteger;
  #endif
#endif

const int CDROMRefNum = -62;			// RefNum of driver

#ifdef STANDALONE_PREFS
void prefs_init()
{
}

void prefs_exit()
{
}
#endif

@implementation VMSettingsController

+ (id) sharedInstance
{
  static VMSettingsController *_sharedInstance = nil;
  if (!_sharedInstance) {
    _sharedInstance = [[VMSettingsController allocWithZone:[self zone]] init];
  }
  return _sharedInstance;
}

- (id) init
{
  self = [super initWithWindowNibName:@"VMSettingsWindow"];

  cancelWasClicked = NO;

  return self;
}

- (NSInteger) numberOfRowsInTableView: (NSTableView *) table
{
  if (table == usbTable)
    return [usbDevices count];
  return [diskArray count];
}

- (id) tableView: (NSTableView *) table objectValueForTableColumn: (NSTableColumn *) col row: (NSInteger) row
{
  if (table == usbTable)
    return [[usbDevices objectAtIndex:row] objectForKey:[col identifier]];

  DiskType *d = (DiskType*)[diskArray objectAtIndex:row];

  if ([[col identifier] isEqualTo:@"isCDROMcol"]) {
    return [NSString stringWithFormat:@"%d", [d isCDROM]];
  }

  return [d path];
}

- (void) tableView: (NSTableView *) table setObjectValue: (id) object forTableColumn: (NSTableColumn *) tableColumn row: (NSInteger) row
{
  if (table == usbTable) {
    NSMutableDictionary *d = [usbDevices objectAtIndex:row];
    if (![[tableColumn identifier] isEqual:@"pass"])
      return;
    // A device that must stay with the host can be unticked, not ticked
    if ([object boolValue] && [[d objectForKey:@"reason"] length]) {
      NSBeep();
      return;
    }
    [d setObject:[NSNumber numberWithBool:[object boolValue]] forKey:@"pass"];
    return;
  }
  if ([[tableColumn identifier] isEqual:@"isCDROMcol"]) {
    DiskType *d = (DiskType*)[diskArray objectAtIndex:row];
    [d setIsCDROM:![d isCDROM]];
  }
}

static NSString *getStringFromPrefs(const char *key)
{
  const char *value = PrefsFindString(key);
  if (value == NULL)
    return @"";
  return [NSString stringWithUTF8String: value];
}

/*
 *  USB tab: host USB devices to pass through ("usbuim", "usbdevice" prefs)
 *
 *  Devices are listed from ioreg, without opening them. SheepShaver takes a
 *  device from macOS only when it runs as root, and as root it can take
 *  anything, so the list leaves out what must stay with the host (as the
 *  QEMU G4/G5 launchers do): keyboards and mice, devices holding a mounted
 *  volume that is not optical media, devices made only of HID interfaces,
 *  and hubs. A device already in the prefs is always shown.
 */

static NSString *const USB_KEYBOARD_OR_MOUSE = @"keyboard or mouse";
static NSString *const USB_MOUNTED_DISK = @"mounted disk";
static NSString *const USB_HID_ONLY = @"HID only";

static NSData *runTool(NSString *path, NSArray *args)
{
  NSTask *task = [[[NSTask alloc] init] autorelease];
  NSPipe *pipe = [NSPipe pipe];
  [task setLaunchPath:path];
  [task setArguments:args];
  [task setStandardOutput:pipe];
  [task setStandardError:[NSFileHandle fileHandleWithNullDevice]];
  @try {
    [task launch];
  } @catch (NSException *e) {
    return nil;
  }
  NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
  [task waitUntilExit];
  return [task terminationStatus] == 0 ? data : nil;
}

static BOOL isHIDInput(NSDictionary *node)
{
  // Generic Desktop pointer, mouse, keyboard, keypad
  static const int usages[] = {1, 2, 6, 7};
  NSNumber *cls = [node objectForKey:@"bInterfaceClass"];
  NSNumber *proto = [node objectForKey:@"bInterfaceProtocol"];
  if (cls && [cls intValue] == 3 && proto && ([proto intValue] == 1 || [proto intValue] == 2))
    return YES;
  NSMutableArray *pairs = [NSMutableArray array];
  if ([node objectForKey:@"PrimaryUsagePage"] && [node objectForKey:@"PrimaryUsage"])
    [pairs addObject:[NSArray arrayWithObjects:[node objectForKey:@"PrimaryUsagePage"], [node objectForKey:@"PrimaryUsage"], nil]];
  id list = [node objectForKey:@"DeviceUsagePairs"];
  if ([list isKindOfClass:[NSArray class]])
    for (id p in list)
      if ([p isKindOfClass:[NSDictionary class]] && [p objectForKey:@"DeviceUsagePage"] && [p objectForKey:@"DeviceUsage"])
        [pairs addObject:[NSArray arrayWithObjects:[p objectForKey:@"DeviceUsagePage"], [p objectForKey:@"DeviceUsage"], nil]];
  for (NSArray *p in pairs) {
    if (![[p objectAtIndex:0] isKindOfClass:[NSNumber class]] || ![[p objectAtIndex:1] isKindOfClass:[NSNumber class]])
      continue;
    if ([[p objectAtIndex:0] intValue] != 1)
      continue;
    for (int i = 0; i < 4; i++)
      if ([[p objectAtIndex:1] intValue] == usages[i])
        return YES;
  }
  return NO;
}

// Walk ioreg's service plane: each device with its interfaces and the disks below it
static void usbWalk(id node, NSMutableDictionary *dev, BOOL optical, NSMutableDictionary *found, NSMutableArray *order)
{
  if (![node isKindOfClass:[NSDictionary class]])
    return;
  NSString *cls = [node objectForKey:@"IOObjectClass"];
  NSNumber *vid = [node objectForKey:@"idVendor"], *pid = [node objectForKey:@"idProduct"];
  if ([cls isEqual:@"IOUSBHostDevice"] && [vid isKindOfClass:[NSNumber class]] && [pid isKindOfClass:[NSNumber class]]) {
    NSNumber *devClass = [node objectForKey:@"bDeviceClass"];
    if (devClass && [devClass intValue] == 9)
      dev = nil;		// a hub: its devices are handled on their own
    else {
      NSNumber *loc = [node objectForKey:@"locationID"];
      if (!loc) loc = [NSNumber numberWithInt:0];
      if ([found objectForKey:loc])
        return;
      NSString *name = [node objectForKey:@"USB Product Name"];
      if (![name isKindOfClass:[NSString class]]) name = [node objectForKey:@"kUSBProductString"];
      if (![name isKindOfClass:[NSString class]]) name = @"";
      dev = [NSMutableDictionary dictionaryWithObjectsAndKeys:
        [NSString stringWithFormat:@"%04x:%04x", [vid intValue] & 0xffff, [pid intValue] & 0xffff], @"id",
        [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]], @"name",
        [NSMutableArray array], @"ifaces",
        [NSMutableArray array], @"media", nil];
      [found setObject:dev forKey:loc];
      [order addObject:loc];
    }
    optical = NO;
  } else if (dev) {
    NSNumber *ifClass = [node objectForKey:@"bInterfaceClass"];
    if ([cls isEqual:@"IOUSBHostInterface"] && [ifClass isKindOfClass:[NSNumber class]])
      [[dev objectForKey:@"ifaces"] addObject:ifClass];
    if (isHIDInput(node))
      [dev setObject:[NSNumber numberWithBool:YES] forKey:@"hidInput"];
    if ([cls isEqual:@"IOCDMedia"] || [cls isEqual:@"IODVDMedia"] || [cls isEqual:@"IOBDMedia"])
      optical = YES;
    NSString *bsd = [node objectForKey:@"BSD Name"];
    if ([bsd isKindOfClass:[NSString class]] && [node objectForKey:@"Whole"] && [node objectForKey:@"Content"])
      [[dev objectForKey:@"media"] addObject:[NSArray arrayWithObjects:bsd, [NSNumber numberWithBool:optical], nil]];
  }
  id children = [node objectForKey:@"IORegistryEntryChildren"];
  if ([children isKindOfClass:[NSArray class]])
    for (id child in children)
      usbWalk(child, dev, optical, found, order);
}

// What is plugged in now: dictionaries with "id", "name" and "reason" (why
// it may never be passed through; empty if it may)
static NSArray *usbHostDevices(void)
{
  NSData *tree = runTool(@"/usr/sbin/ioreg", [NSArray arrayWithObjects:@"-a", @"-r", @"-c", @"IOUSBHostDevice", @"-l", @"-w0", nil]);
  if (tree == nil)
    return [NSArray array];
  id roots = [NSPropertyListSerialization propertyListWithData:tree options:0 format:NULL error:NULL];
  NSMutableDictionary *found = [NSMutableDictionary dictionary];
  NSMutableArray *order = [NSMutableArray array];
  for (id root in ([roots isKindOfClass:[NSArray class]] ? roots : [NSArray arrayWithObjects:roots, nil]))
    usbWalk(root, nil, NO, found, order);

  // Mounted /dev/ volumes
  NSMutableSet *mounted = [NSMutableSet set];
  NSData *mounts = runTool(@"/sbin/mount", [NSArray array]);
  NSString *text = mounts ? [[[NSString alloc] initWithData:mounts encoding:NSUTF8StringEncoding] autorelease] : @"";
  for (NSString *line in [text componentsSeparatedByString:@"\n"])
    if ([line hasPrefix:@"/dev/"]) {
      NSRange sp = [line rangeOfString:@" "];
      if (sp.location != NSNotFound)
        [mounted addObject:[line substringWithRange:NSMakeRange(5, sp.location - 5)]];
    }

  NSMutableArray *out = [NSMutableArray array];
  for (NSNumber *loc in order) {
    NSMutableDictionary *d = [found objectForKey:loc];
    NSString *reason = @"";
    BOOL mountedDisk = NO;
    for (NSArray *m in [d objectForKey:@"media"])
      if (![[m objectAtIndex:1] boolValue] && [mounted containsObject:[m objectAtIndex:0]])
        mountedDisk = YES;
    NSArray *ifaces = [d objectForKey:@"ifaces"];
    BOOL hidOnly = [ifaces count] > 0;
    for (NSNumber *c in ifaces)
      if ([c intValue] != 3)
        hidOnly = NO;
    if ([[d objectForKey:@"hidInput"] boolValue])
      reason = USB_KEYBOARD_OR_MOUSE;
    else if (mountedDisk)
      reason = USB_MOUNTED_DISK;
    else if (hidOnly)
      reason = USB_HID_ONLY;
    [out addObject:[NSDictionary dictionaryWithObjectsAndKeys:
      [d objectForKey:@"id"], @"id", [d objectForKey:@"name"], @"name", reason, @"reason", nil]];
  }
  return out;
}

static NSTextField *makeLabel(NSRect frame, NSString *text)
{
  NSTextField *label = [[[NSTextField alloc] initWithFrame:frame] autorelease];
  [label setStringValue:text];
  [label setEditable:NO];
  [label setSelectable:NO];
  [label setBordered:NO];
  [label setDrawsBackground:NO];
  [label setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
  [[label cell] setWraps:YES];
  return label;
}

static NSTableColumn *makeColumn(NSString *ident, NSString *title, CGFloat width)
{
  NSTableColumn *col = [[[NSTableColumn alloc] initWithIdentifier:ident] autorelease];
  [[col headerCell] setStringValue:title];
  [col setWidth:width];
  [col setEditable:NO];
  return col;
}

// Add the USB tab to the settings window (once)
- (void) addUSBTab
{
  if (usbTable)
    return;
  NSView *v = disks;
  while (v && ![v isKindOfClass:[NSTabView class]])
    v = [v superview];
  if (!v)
    return;
  NSTabView *tabs = (NSTabView *)v;

  NSView *pane = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 534, 349)] autorelease];
  [pane setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  usbEnable = [[[NSButton alloc] initWithFrame:NSMakeRect(15, 323, 500, 18)] autorelease];
  [usbEnable setButtonType:NSButtonTypeSwitch];
  [usbEnable setTitle:@"Pass USB devices through to Mac OS (experimental)"];
  [usbEnable setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [pane addSubview:usbEnable];

  NSTextField *help = makeLabel(NSMakeRect(17, 279, 500, 40),
    @"Needs USB 1.4 or later in Mac OS. Ticked devices are taken from macOS while SheepShaver runs "
    @"and given back when it quits. Keyboards, mice, hubs, mounted disks and HID-only devices are not listed.");
  [help setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [pane addSubview:help];

  NSScrollView *scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(17, 52, 500, 222)] autorelease];
  [scroll setHasVerticalScroller:YES];
  [scroll setBorderType:NSBezelBorder];
  [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  usbTable = [[[NSTableView alloc] initWithFrame:[[scroll contentView] bounds]] autorelease];
  NSTableColumn *pass = makeColumn(@"pass", @"Pass through", 80);
  NSButtonCell *check = [[[NSButtonCell alloc] init] autorelease];
  [check setButtonType:NSButtonTypeSwitch];
  [check setTitle:@""];
  [pass setDataCell:check];
  [pass setEditable:YES];
  [usbTable addTableColumn:pass];
  [usbTable addTableColumn:makeColumn(@"name", @"Device", 210)];
  [usbTable addTableColumn:makeColumn(@"id", @"ID", 80)];
  [usbTable addTableColumn:makeColumn(@"status", @"Status", 110)];
  [usbTable setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];
  [usbTable setDataSource:self];
  [scroll setDocumentView:usbTable];
  [pane addSubview:scroll];

  usbNote = makeLabel(NSMakeRect(17, 8, 400, 40), @"");
  [usbNote setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
  [pane addSubview:usbNote];

  NSButton *refresh = [[[NSButton alloc] initWithFrame:NSMakeRect(424, 12, 96, 32)] autorelease];
  [refresh setButtonType:NSButtonTypeMomentaryPushIn];
  [refresh setBezelStyle:NSBezelStyleRounded];
  [refresh setTitle:@"Refresh"];
  [refresh setTarget:self];
  [refresh setAction:@selector(refreshUSBClicked:)];
  [refresh setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
  [pane addSubview:refresh];

  NSTabViewItem *item = [[[NSTabViewItem alloc] initWithIdentifier:@"usb"] autorelease];
  [item setLabel:@"USB"];
  [item setView:pane];
  [tabs addTabViewItem:item];

  usbDevices = [[NSMutableArray alloc] init];
}

// Fill the list: what is plugged in, plus the wanted devices that are not
- (void) loadUSB: (NSArray *) wanted
{
  if (!usbTable)
    return;
  [usbDevices removeAllObjects];
  NSMutableSet *listed = [NSMutableSet set];
  int hidden = 0;
  for (NSDictionary *d in usbHostDevices()) {
    NSString *ident = [d objectForKey:@"id"];
    NSString *reason = [d objectForKey:@"reason"];
    BOOL want = [wanted containsObject:ident];
    if ([listed containsObject:ident])
      continue;			// the same model twice: one pref covers both
    if ([reason length] && !want) {
      hidden++;
      continue;
    }
    [listed addObject:ident];
    NSString *name = [[d objectForKey:@"name"] length] ? [d objectForKey:@"name"] : [NSString stringWithFormat:@"USB device %@", ident];
    [usbDevices addObject:[NSMutableDictionary dictionaryWithObjectsAndKeys:
      [NSNumber numberWithBool:want], @"pass", name, @"name", ident, @"id", reason, @"reason", reason, @"status", nil]];
  }
  for (NSString *ident in wanted)
    if (![listed containsObject:ident]) {
      [listed addObject:ident];
      [usbDevices addObject:[NSMutableDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithBool:YES], @"pass", @"(not plugged in)", @"name", ident, @"id", @"", @"reason", @"", @"status", nil]];
    }
  [usbTable reloadData];

  NSMutableArray *notes = [NSMutableArray array];
  if (hidden)
    [notes addObject:[NSString stringWithFormat:@"%d device%s hidden.", hidden, hidden == 1 ? "" : "s"]];
  if (geteuid() != 0)
    [notes addObject:@"Devices that macOS has drivers for (disks, cameras, audio) need root: "
                     @"SheepShaver asks for an administrator password at start when one of them is plugged in."];
  [usbNote setStringValue:[notes componentsJoinedByString:@" "]];
}

// The ticked IDs, in list order
- (NSArray *) usbWanted
{
  NSMutableArray *out = [NSMutableArray array];
  for (NSDictionary *d in usbDevices)
    if ([[d objectForKey:@"pass"] boolValue])
      [out addObject:[d objectForKey:@"id"]];
  return out;
}

- (IBAction) refreshUSBClicked: (id) sender
{
  [self loadUSB:[self usbWanted]];
}

- (void) setupGUI
{
  diskArray = [[NSMutableArray alloc] init];

  const char *dsk;
  int index = 0;
  while ((dsk = PrefsFindString("disk", index++)) != NULL) {
    DiskType *disk = [[[DiskType alloc] init] autorelease];
    [disk setPath:[NSString stringWithUTF8String: dsk ]];
    [disk setIsCDROM:NO];

    [diskArray addObject:disk];
  }

  /* Fetch all CDROMs */
  index = 0;
  while ((dsk = PrefsFindString("cdrom", index++)) != NULL) {
    NSString *path = [NSString stringWithUTF8String: dsk ];
    if (![path hasPrefix:@"/dev/"]) {
        DiskType *disk = [[[DiskType alloc] init] autorelease];
        [disk setPath:[NSString stringWithUTF8String: dsk ]];
        [disk setIsCDROM:YES];

        [diskArray addObject:disk];
    }
  }

  [disks setDataSource: self];
  [disks reloadData];

  int bootdriver = PrefsFindInt32("bootdriver"), active = 0;
  switch (bootdriver) {
    case 0: active = 0; break;
    case CDROMRefNum: active = 1; break;
  }
  [bootFrom selectItemAtIndex: active ];

  [romFile setStringValue: getStringFromPrefs("rom") ];
  [unixRoot setStringValue: getStringFromPrefs("extfs") ];
  [disableCdrom setIntValue: PrefsFindBool("nocdrom") ];
  int ramsize = PrefsFindInt32("ramsize");
  if (ramsize > 1000) ramsize >>= 20;
  [ramSize setIntValue: ramsize ];
  [ramSizeStepper setIntValue: ramsize ];

  int display_type = 0;
  int dis_width = 640;
  int dis_height = 480;

  const char *str = PrefsFindString("screen");
  if (str != NULL) {
    if (sscanf(str, "win/%d/%d", &dis_width, &dis_height) == 2)
      display_type = 0;
    else if (sscanf(str, "dga/%d/%d", &dis_width, &dis_height) == 2)
      display_type = 1;
  }

  [videoType selectItemAtIndex: display_type ];
  [width setStringValue:[NSString stringWithFormat:@"%d", dis_width]];
  [height setStringValue:[NSString stringWithFormat:@"%d", dis_height]];

  int frameskip = PrefsFindInt32("frameskip");
  int item = -1;
  switch (frameskip) {
    case 12: item = 0; break;
    case 8: item = 1; break;
    case 6: item = 2; break;
    case 4: item = 3; break;
    case 2: item = 4; break;
    case 1: item = 5; break;
    case 0: item = 6; break;
  }
  if (item >= 0)
    [refreshRate selectItemAtIndex: item ];

  [qdAccel setIntValue: PrefsFindBool("gfxaccel") ];

  [disableSound setIntValue: PrefsFindBool("nosound") ];
  [outDevice setStringValue: getStringFromPrefs("dsp") ];
  [mixDevice setStringValue: getStringFromPrefs("mixer") ];

  [useRawKeyCodes setIntValue: PrefsFindBool("keycodes") ];
  [rawKeyCodes setStringValue: getStringFromPrefs("keycodefile") ];
  [rawKeyCodes setEnabled:[useRawKeyCodes intValue]];
  [browseRawKeyCodesButton setEnabled:[useRawKeyCodes intValue]];

  int wheelmode = PrefsFindInt32("mousewheelmode"), wheel = 0;
  switch (wheelmode) {
    case 0: wheel = 0; break;
    case 1: wheel = 1; break;
  }
  [mouseWheel selectItemAtIndex: wheel ];

  [scrollLines setIntValue: PrefsFindInt32("mousewheellines") ];
  [scrollLinesStepper setIntValue: PrefsFindInt32("mousewheellines") ];

  [ignoreIllegalMemoryAccesses setIntValue: PrefsFindBool("ignoresegv") ];
  [ignoreIllegalInstructions setIntValue: PrefsFindBool("ignoreillegal") ];
  [dontUseCPUWhenIdle setIntValue: PrefsFindBool("idlewait") ];
  [enableJIT setIntValue: PrefsFindBool("jit") ];
  [enable68kDREmulator setIntValue: PrefsFindBool("jit68k") ];

  [modemPort setStringValue: getStringFromPrefs("seriala") ];
  [printerPort setStringValue: getStringFromPrefs("serialb") ];
  [ethernetInterface setStringValue: getStringFromPrefs("ether") ];

  [self addUSBTab];
  [usbEnable setState: PrefsFindBool("usbuim") ? NSControlStateValueOn : NSControlStateValueOff];
  NSMutableArray *wanted = [NSMutableArray array];
  const char *dev;
  for (int i = 0; (dev = PrefsFindString("usbdevice", i)) != NULL; i++) {
    NSString *ident = [[NSString stringWithUTF8String:dev] lowercaseString];
    if (![wanted containsObject:ident])
      [wanted addObject:ident];
  }
  [self loadUSB:wanted];
}

- (void) editSettingsFor: (NSString *) vmdir sender: (id) sender
{
  chdir([vmdir fileSystemRepresentation]);
  AddPrefsDefaults();
  AddPlatformPrefsDefaults();
  LoadPrefs([vmdir fileSystemRepresentation]);
  NSWindow *window = [self window];
  [self setupGUI];
  [NSApp runModalForWindow:window];
}

- (void) editSettingsForNewVM: (NSString *) vmdir sender: (id) sender
{
  chdir([vmdir fileSystemRepresentation]);
  AddPrefsDefaults();
  AddPlatformPrefsDefaults();
  LoadPrefs([vmdir fileSystemRepresentation]);
  PrefsReplaceString("screen", "win/800/600");
  PrefsReplaceString("extfs", "");
  PrefsReplaceString("ether", "slirp");
  PrefsReplaceInt32("ramsize", 64 << 20);
  PrefsReplaceInt32("frameskip", 2);
  PrefsReplaceBool("jit", true);
  NSWindow *window = [self window];
  [self setupGUI];
  [NSApp runModalForWindow:window];
}

static NSString *makeRelativeIfNecessary(NSString *path)
{
  char cwd[1024], filename[1024];
  int cwdlen;
  strlcpy(filename, [path fileSystemRepresentation], sizeof(filename));
  getcwd(cwd, sizeof(cwd));
  cwdlen = strlen(cwd);
  if (!strncmp(cwd, filename, cwdlen)) {
    if (cwdlen >= 0 && cwd[cwdlen-1] != '/')
      cwdlen++;
    return [NSString stringWithUTF8String: filename + cwdlen];
  }
  return path;
}

- (IBAction) addDisk: (id) sender
{
  NSOpenPanel *open = [NSOpenPanel openPanel];
  [open setCanChooseDirectories:YES];
  [open setAccessoryView:isCDROM];
  [open setAllowsMultipleSelection:NO];
  [open setTreatsFilePackagesAsDirectories:YES];
  [open beginSheetForDirectory: [[NSFileManager defaultManager] currentDirectoryPath]
                          file: @"Unknown"
                modalForWindow: [self window]
                 modalDelegate: self
                didEndSelector: @selector(_addDiskEnd: returnCode: contextInfo:)
                   contextInfo: nil];
}

- (void) _addDiskEnd: (NSOpenPanel *) open returnCode: (int) theReturnCode contextInfo: (void *) theContextInfo
{
  if (theReturnCode == NSOKButton) {
    DiskType *d = [[[DiskType alloc] init] autorelease];
    [d setPath:makeRelativeIfNecessary([open filename])];

    [d setIsCDROM:([isCDROMcheckbox state] == NSOnState)];

    [diskArray addObject: d]; 
    [disks reloadData];
  }
}

- (IBAction) removeDisk: (id) sender
{
  int selectedRow = [disks selectedRow];
  if (selectedRow >= 0) {
    [diskArray removeObjectAtIndex: selectedRow];
    [disks reloadData];
  }
}

- (IBAction) createDisk: (id) sender
{
  NSSavePanel *save = [NSSavePanel savePanel];
  [save setAccessoryView: diskSaveSize];
  [save setTreatsFilePackagesAsDirectories:YES];
  [save beginSheetForDirectory: [[NSFileManager defaultManager] currentDirectoryPath]
                          file: @"New.dsk"
                modalForWindow: [self window]
                 modalDelegate: self
                didEndSelector: @selector(_createDiskEnd: returnCode: contextInfo:)
                   contextInfo: nil];
}

- (void) _createDiskEnd: (NSSavePanel *) save returnCode: (int) theReturnCode contextInfo: (void *) theContextInfo
{
  if (theReturnCode == NSOKButton) {
    int size = [diskSaveSizeField intValue];
    if (size >= 0 && size <= 10000) {
      char cmd[1024];
      snprintf(cmd, sizeof(cmd), "dd if=/dev/zero \"of=%s\" bs=1024k count=%d", [[save filename] UTF8String], [diskSaveSizeField intValue]);
      int ret = system(cmd);
      if (ret == 0) {
        DiskType *d = [[[DiskType alloc] init] autorelease];
        [d setPath:makeRelativeIfNecessary([save filename])];
        [d setIsCDROM:NO];

        [diskArray addObject: d];
        [disks reloadData];
      }
    }
  }
  [(NSData *)theContextInfo release];
}

- (IBAction) useRawKeyCodesClicked: (id) sender
{
  [rawKeyCodes setEnabled:[useRawKeyCodes intValue]];
  [browseRawKeyCodesButton setEnabled:[useRawKeyCodes intValue]];
}

- (IBAction) browseForROMFileClicked: (id) sender
{
  NSOpenPanel *open = [NSOpenPanel openPanel];
  [open setCanChooseDirectories:NO];
  [open setAllowsMultipleSelection:NO];
  [open setTreatsFilePackagesAsDirectories:YES];
  [open beginSheetForDirectory: @""
                          file: [romFile stringValue]
                modalForWindow: [self window]
                 modalDelegate: self
                didEndSelector: @selector(_browseForROMFileEnd: returnCode: contextInfo:)
                   contextInfo: nil];
}

- (void) _browseForROMFileEnd: (NSOpenPanel *) open returnCode: (int) theReturnCode contextInfo: (void *) theContextInfo
{
  if (theReturnCode == NSOKButton) {
    [romFile setStringValue: makeRelativeIfNecessary([open filename])];
  }
}

- (IBAction) browseForUnixRootClicked: (id) sender
{
  NSOpenPanel *open = [NSOpenPanel openPanel];
  [open setCanChooseDirectories:YES];
  [open setCanChooseFiles:NO];
  [open setAllowsMultipleSelection:NO];
  [open beginSheetForDirectory: @""
                          file: [unixRoot stringValue]
                modalForWindow: [self window]
                 modalDelegate: self
                didEndSelector: @selector(_browseForUnixRootEnd: returnCode: contextInfo:)
                   contextInfo: nil];
}

- (void) _browseForUnixRootEnd: (NSOpenPanel *) open returnCode: (int) theReturnCode contextInfo: (void *) theContextInfo
{
  if (theReturnCode == NSOKButton) {
    [unixRoot setStringValue: makeRelativeIfNecessary([open filename])];
  }
}

- (IBAction) browseForKeyCodesFileClicked: (id) sender
{
  NSOpenPanel *open = [NSOpenPanel openPanel];
  [open setCanChooseDirectories:NO];
  [open setAllowsMultipleSelection:NO];
  [open setTreatsFilePackagesAsDirectories:YES];
  [open beginSheetForDirectory: @""
                          file: [unixRoot stringValue]
                modalForWindow: [self window]
                 modalDelegate: self
                didEndSelector: @selector(_browseForKeyCodesFileEnd: returnCode: contextInfo:)
                   contextInfo: nil];
}

- (void) _browseForKeyCodesFileEnd: (NSOpenPanel *) open returnCode: (int) theReturnCode contextInfo: (void *) theContextInfo
{
  if (theReturnCode == NSOKButton) {
    [rawKeyCodes setStringValue: makeRelativeIfNecessary([open filename])];
  }
}

- (void) cancelEdit: (id) sender
{
#ifdef STANDALONE_PREFS
  PrefsExit();
#endif
  [[self window] close];
  [NSApp stopModal];
  cancelWasClicked = YES;
}

- (void) saveChanges: (id) sender
{
  // Remove all disks
  while (PrefsFindString("disk"))
    PrefsRemoveItem("disk");
  // Remove all cdroms (but keep the ones in /dev/)
  const char *path;
  int index = 0;
  while ((path = PrefsFindString("cdrom", index)) != NULL) {
    NSString *p = [NSString stringWithUTF8String: path];
    if (![p hasPrefix:@"/dev/"]) {
      PrefsRemoveItem("cdrom", index);
    } else {
      // only increase the index if the current entry has not been deleted
      // if it has been deleted, the next entry is on the current entrys index
      index++;
    }
  }

  // Write all disks
  for (int i = 0; i < [diskArray count]; i++) {
    DiskType *d = [diskArray objectAtIndex:i];
    PrefsAddString([d isCDROM] ? "cdrom" : "disk", [[d path] UTF8String]);
  }

  PrefsReplaceInt32("bootdriver", ([bootFrom indexOfSelectedItem] == 1 ? CDROMRefNum : 0));
  PrefsReplaceString("rom", [[romFile stringValue] UTF8String]);
  PrefsReplaceString("extfs", [[unixRoot stringValue] UTF8String]);
  PrefsReplaceBool("nocdrom", [disableCdrom intValue]);
  PrefsReplaceInt32("ramsize", [ramSize intValue] << 20);

  char pref[256];
  snprintf(pref, sizeof(pref), "%s/%d/%d", [videoType indexOfSelectedItem] == 0 ? "win" : "dga", [width intValue], [height intValue]);
  PrefsReplaceString("screen", pref);

  int rate = 8;
  switch ([refreshRate indexOfSelectedItem]) {
    case 0: rate = 12; break;
    case 1: rate = 8; break;
    case 2: rate = 6; break;
    case 3: rate = 4; break;
    case 4: rate = 2; break;
    case 5: rate = 1; break;
    case 6: rate = 0; break;
  }
  PrefsReplaceInt32("frameskip", rate);
  PrefsReplaceBool("gfxaccel", [qdAccel intValue]);

  PrefsReplaceBool("nosound", [disableSound intValue]);
  PrefsReplaceString("dsp", [[outDevice stringValue] UTF8String]);
  PrefsReplaceString("mixer", [[mixDevice stringValue] UTF8String]);

  PrefsReplaceBool("keycodes", [useRawKeyCodes intValue]);
  PrefsReplaceString("keycodefile", [[rawKeyCodes stringValue] UTF8String]);

  PrefsReplaceInt32("mousewheelmode", [mouseWheel indexOfSelectedItem]);
  PrefsReplaceInt32("mousewheellines", [scrollLines intValue]);

  PrefsReplaceBool("ignoresegv", [ignoreIllegalMemoryAccesses intValue]);
  PrefsReplaceBool("ignoreillegal", [ignoreIllegalInstructions intValue]);
  PrefsReplaceBool("idlewait", [dontUseCPUWhenIdle intValue]);
  PrefsReplaceBool("jit", [enableJIT intValue]);
  PrefsReplaceBool("jit68k", [enable68kDREmulator intValue]);

  PrefsReplaceString("seriala", [[modemPort stringValue] UTF8String]);
  PrefsReplaceString("serialb", [[printerPort stringValue] UTF8String]);
  PrefsReplaceString("ether", [[ethernetInterface stringValue] UTF8String]);

  if (usbTable) {
    PrefsReplaceBool("usbuim", [usbEnable state] == NSControlStateValueOn);
    while (PrefsFindString("usbdevice"))
      PrefsRemoveItem("usbdevice");
    for (NSString *ident in [self usbWanted])
      PrefsAddString("usbdevice", [ident UTF8String]);
  }
  SavePrefs();
#ifdef STANDALONE_PREFS
  PrefsExit();
#endif

  [[self window] close];
  [NSApp stopModal];
  cancelWasClicked = NO;

  // quit
#ifdef USE_SDL_VIDEO
  SDL_Event event;
#if SDL_VERSION_ATLEAST(3, 0, 0)
  event.type = SDL_EVENT_QUIT;
#else
  event.type = SDL_QUIT;
#endif
  SDL_PushEvent(&event);
#endif
}

- (BOOL) cancelWasClicked
{
  return cancelWasClicked;
}

- (void) dealloc
{
  [super dealloc];
}

@end

