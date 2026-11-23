/*
 *  rsrc_patches.cpp - Resource patches
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "audio.h"
#include "audio_defs.h"
#include "rsrc_patches.h"

#if ENABLE_MON
#include "mon.h"
#endif

#define DEBUG 0
#include "debug.h"


// Sound input driver (replaces the native .AppleSoundInput, traps to audio.cpp)
static const uint8 sound_input_driver[] = {	// .AppleSoundInput driver header
	// Driver header
	0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x24,			// Open() offset
	0x00, 0x28,			// Prime() offset
	0x00, 0x2c,			// Control() offset
	0x00, 0x38,			// Status() offset
	0x00, 0x5e,			// Close() offset
	0x10, 0x2e, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x53, 0x6f, 0x75, 0x6e, 0x64, 0x49, 0x6e, 0x70, 0x75, 0x74, 0x00,	// ".AppleSoundInput"

	// Open()
	M68K_EMUL_OP_SOUNDIN_OPEN >> 8, M68K_EMUL_OP_SOUNDIN_OPEN & 0xff,
	0x4e, 0x75,							//	rts

	// Prime()
	M68K_EMUL_OP_SOUNDIN_PRIME >> 8, M68K_EMUL_OP_SOUNDIN_PRIME & 0xff,
	0x60, 0x0e,							//	bra		IOReturn

	// Control()
	M68K_EMUL_OP_SOUNDIN_CONTROL >> 8, M68K_EMUL_OP_SOUNDIN_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//	cmp.w	#1,$1a(a0)
	0x66, 0x04,							//	bne		IOReturn
	0x4e, 0x75,							//	rts

	// Status()
	M68K_EMUL_OP_SOUNDIN_STATUS >> 8, M68K_EMUL_OP_SOUNDIN_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	M68K_EMUL_OP_SOUNDIN_CLOSE >> 8, M68K_EMUL_OP_SOUNDIN_CLOSE & 0xff,
	0x4e, 0x75,							//	rts
};


// Driver resource named ".AppleSoundInput"? (name is a Pascal string at offset 18)
static bool is_sound_input_driver(const uint8 *p, uint32 size)
{
	static const char name[] = ".AppleSoundInput";
	const uint32 len = sizeof(name) - 1;
	return size > 19 + len && p[18] == len && memcmp(p + 19, name, len) == 0;
}


/*
 *  Search resource for byte string, return offset (or 0)
 */

static uint32 find_rsrc_data(const uint8 *rsrc, uint32 max, const uint8 *search, uint32 search_len, uint32 ofs = 0)
{
	while (ofs < max - search_len) {
		if (!memcmp(rsrc + ofs, search, search_len))
			return ofs;
		ofs++;
	}
	return 0;
}


/*
 *  Install SynchIdleTime() patch
 */

static void patch_idle_time(uint8 *p, uint32 size, int n = 1)
{
	if (!PrefsFindBool("idlewait"))
		return;

	static const uint8 dat[] = {0x70, 0x03, 0xa0, 0x9f};
	uint32 base = find_rsrc_data(p, size, dat, sizeof(dat));
	if (base) {
		uint8 *pbase = p + base - 0x80;
		static const uint8 dat2[] = {0x20, 0x78, 0x02, 0xb6, 0x41, 0xe8, 0x00, 0x80};
		base = find_rsrc_data(pbase, 0x80, dat2, sizeof(dat2));
		if (base) {
			uint16 *p16 = (uint16 *)(pbase + base);
			*p16++ = htons(M68K_EMUL_OP_IDLE_TIME);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(pbase + base, 4);
			D(bug("  patch %d applied\n", n));
		}
	}
}


/*
 *  Resource patches via vCheckLoad
 */

void CheckLoad(uint32 type, int16 id, uint8 *p, uint32 size)
{
	uint16 *p16;
	uint32 base;
	D(bug("vCheckLoad %c%c%c%c (%08x) ID %d, data %p, size %d\n", (char)(type >> 24), (char)((type >> 16) & 0xff), (char )((type >> 8) & 0xff), (char )(type & 0xff), type, id, p, size));
	
	if (type == FOURCC('b','o','o','t') && id == 3) {
		D(bug(" boot 3 found\n"));

		// Set boot stack pointer (7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0x22, 0x00, 0xe4, 0x89, 0x90, 0x81, 0x22, 0x40};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 6);
			*p16 = htons(M68K_EMUL_OP_FIX_BOOTSTACK);
			FlushCodeCache(p + base + 6, 2);
			D(bug("  patch 1 applied\n"));
		}

#if !ROM_IS_WRITE_PROTECTED
		// Set fake handle at 0x0000 to some safe place (so broken Mac programs won't write into Mac ROM) (7.1, 7.5, 8.0)
		static const uint8 dat2[] = {0x20, 0x78, 0x02, 0xae, 0xd1, 0xfc, 0x00, 0x01, 0x00, 0x00, 0x21, 0xc8, 0x00, 0x00};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);

#if defined(USE_SCRATCHMEM_SUBTERFUGE)
			// Set 0x0000 to scratch memory area
			extern uint8 *ScratchMem;
			const uint32 ScratchMemBase = Host2MacAddr(ScratchMem);
			*p16++ = htons(0x207c);			// move.l	#ScratchMem,a0
			*p16++ = htons(ScratchMemBase >> 16);
			*p16++ = htons(ScratchMemBase);
			*p16++ = htons(M68K_NOP);
			*p16 = htons(M68K_NOP);
#else
#error System specific handling for writable ROM is required here
#endif
			FlushCodeCache(p + base, 14);
			D(bug("  patch 2 applied\n"));
		}

	} else if (type == FOURCC('b','o','o','t') && id == 2) {
		D(bug(" boot 2 found\n"));

		// Set fake handle at 0x0000 to some safe place (so broken Mac programs won't write into Mac ROM) (7.1, 7.5, 8.0)
		static const uint8 dat[] = {0x20, 0x78, 0x02, 0xae, 0xd1, 0xfc, 0x00, 0x01, 0x00, 0x00, 0x21, 0xc8, 0x00, 0x00};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);

#if defined(USE_SCRATCHMEM_SUBTERFUGE)
			// Set 0x0000 to scratch memory area
			extern uint8 *ScratchMem;
			const uint32 ScratchMemBase = Host2MacAddr(ScratchMem);
			*p16++ = htons(0x207c);			// move.l	#ScratchMem,a0
			*p16++ = htons(ScratchMemBase >> 16);
			*p16++ = htons(ScratchMemBase);
			*p16++ = htons(M68K_NOP);
			*p16 = htons(M68K_NOP);
#else
#error System specific handling for writable ROM is required here
#endif
			FlushCodeCache(p + base, 14);
			D(bug("  patch 1 applied\n"));
		}
#endif

	} else if (type == FOURCC('P','T','C','H') && id == 630) {
		D(bug("PTCH 630 found\n"));

		// Don't replace Time Manager (Classic ROM, 6.0.3)
		static const uint8 dat[] = {0x30, 0x3c, 0x00, 0x58, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			p16[2] = htons(M68K_NOP);
			p16[7] = htons(M68K_NOP);
			p16[12] = htons(M68K_NOP);
			FlushCodeCache(p + base, 26);
			D(bug("  patch 1 applied\n"));
		}

		// Don't replace Time Manager (Classic ROM, 6.0.8)
		static const uint8 dat2[] = {0x70, 0x58, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			p16[1] = htons(M68K_NOP);
			p16[5] = htons(M68K_NOP);
			p16[9] = htons(M68K_NOP);
			FlushCodeCache(p + base, 20);
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('p','t','c','h') && id == 26) {
		D(bug(" ptch 26 found\n"));

		// Trap ABC4 is initialized with absolute ROM address (7.1, 7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0x40, 0x83, 0x36, 0x10};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons((ROMBaseMac + 0x33610) >> 16);
			*p16 = htons((ROMBaseMac + 0x33610) & 0xffff);
			FlushCodeCache(p + base, 4);
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('p','t','c','h') && id == 34) {
		D(bug(" ptch 34 found\n"));

		// Don't wait for VIA (Classic ROM, 6.0.8)
		static const uint8 dat[] = {0x22, 0x78, 0x01, 0xd4, 0x10, 0x11, 0x02, 0x00, 0x00, 0x30};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 14);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 14, 2);
			D(bug("  patch 1 applied\n"));
		}

		// Don't replace ADBOp() (Classic ROM, 6.0.8)
		static const uint8 dat2[] = {0x21, 0xc0, 0x05, 0xf0};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons(M68K_NOP);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base, 4);
			D(bug("  patch 2 applied\n"));
		}

	} else if (type == FOURCC('g','p','c','h') && id == 750) {
		D(bug(" gpch 750 found\n"));

		// Don't use PTEST instruction in BlockMove() (7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0x20, 0x5f, 0x22, 0x5f, 0x0c, 0x38, 0x00, 0x04, 0x01, 0x2f};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 4);
			*p16++ = htons(M68K_EMUL_OP_BLOCK_MOVE);
			*p16++ = htons(0x7000);
			*p16 = htons(M68K_RTS);
			FlushCodeCache(p + base + 4, 6);
			D(bug("  patch 1 applied\n"));
		}

		// Patch SynchIdleTime()
		patch_idle_time(p, size, 2);

	} else if (type == FOURCC('l','p','c','h') && id == 24) {
		D(bug(" lpch 24 found\n"));

		// Don't replace Time Manager (7.0.1, 7.1, 7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0x70, 0x59, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 2);
			*p16++ = htons(M68K_NOP);
			p16 += 3;
			*p16++ = htons(M68K_NOP);
			p16 += 7;
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 2, 28);
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('l','p','c','h') && id == 31) {
		D(bug(" lpch 31 found\n"));

		// Don't write to VIA in vSoundDead() (7.0.1, 7.1, 7.5, 7.6, 7.6.1, 8.0)
		static const uint8 dat[] = {0x20, 0x78, 0x01, 0xd4, 0x08, 0xd0, 0x00, 0x07, 0x4e, 0x75};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16 = htons(M68K_RTS);
			FlushCodeCache(p + base, 2);
			D(bug("  patch 1 applied\n"));
		}

		// Don't replace SCSI manager (7.1, 7.5, 7.6.1, 8.0)
		static const uint8 dat2[] = {0x0c, 0x6f, 0x00, 0x0e, 0x00, 0x04, 0x66, 0x0c};
		base = find_rsrc_data(p, size, dat2, sizeof(dat2));
		if (base) {
			p16 = (uint16 *)(p + base);
			*p16++ = htons(M68K_EMUL_OP_SCSI_DISPATCH);
			*p16++ = htons(0x2e49);		// move.l	a1,a7
			*p16 = htons(M68K_JMP_A0);
			FlushCodeCache(p + base, 6);
			D(bug("  patch 2 applied\n"));
		}

		// Patch SynchIdleTime()
		patch_idle_time(p, size, 3);

	} else if (type == FOURCC('t','h','n','g') && id == -16563) {
		D(bug(" thng -16563 found\n"));

		// Set audio component flags (7.5, 7.6, 7.6.1, 8.0)
		*(uint32 *)(p + componentFlags) = htonl(audio_component_flags);
		D(bug("  patch 1 applied\n"));

	} else if (type == FOURCC('s','i','f','t') && id == -16563) {
		D(bug(" sift -16563 found\n"));

		// Replace audio component (7.5, 7.6, 7.6.1, 8.0)
		p16 = (uint16 *)p;
		*p16++ = htons(0x4e56); *p16++ = htons(0x0000);	// link		a6,#0
		*p16++ = htons(0x48e7); *p16++ = htons(0x8018);	// movem.l	d0/a3-a4,-(sp)
		*p16++ = htons(0x266e); *p16++ = htons(0x000c);	// movea.l	12(a6),a3
		*p16++ = htons(0x286e); *p16++ = htons(0x0008);	// movea.l	8(a6),a4
		*p16++ = htons(M68K_EMUL_OP_AUDIO);
		*p16++ = htons(0x2d40); *p16++ = htons(0x0010);	// move.l	d0,16(a6)
		*p16++ = htons(0x4cdf); *p16++ = htons(0x1801);	// movem.l	(sp)+,d0/a3-a4
		*p16++ = htons(0x4e5e);							// unlk		a6
		*p16++ = htons(0x4e74); *p16++ = htons(0x0008);	// rtd		#8
		FlushCodeCache(p, 32);
		D(bug("  patch 1 applied\n"));

	} else if (type == FOURCC('i','n','s','t') && id == -19069) {
		D(bug(" inst -19069 found\n"));

		// Don't replace Microseconds (QuickTime 2.0)
		static const uint8 dat[] = {0x30, 0x3c, 0xa1, 0x93, 0xa2, 0x47};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 4);
			*p16 = htons(M68K_NOP);
			FlushCodeCache(p + base + 4, 2);
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('D','R','V','R') && id == -20066) {
		D(bug("DRVR -20066 found\n"));

		// Don't access SCC in .Infra driver
		static const uint8 dat[] = {0x28, 0x78, 0x01, 0xd8, 0x48, 0xc7, 0x20, 0x0c, 0xd0, 0x87, 0x20, 0x40, 0x1c, 0x10};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 12);
			*p16 = htons(0x7a00);	// moveq #0,d6
			FlushCodeCache(p + base + 12, 2);
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('D','R','V','R') && is_sound_input_driver(p, size)) {
		D(bug("DRVR %d (.AppleSoundInput) found, size %d\n", id, size));

		// Replace the native sound input driver with one that traps to audio.cpp.
		// Matched by name: the ID differs by machine (-16499 on Quadra/8.1,
		// -16501/-16500 elsewhere)
		if (size >= sizeof(sound_input_driver)) {
			memcpy(p, sound_input_driver, sizeof(sound_input_driver));
			FlushCodeCache(p, sizeof(sound_input_driver));
			D(bug("  patch 1 applied\n"));
		}

	} else if (type == FOURCC('l','t','l','k') && id == 0) {
		D(bug(" ltlk 0 found\n"));

		// Disable LocalTalk (7.0.1, 7.5, 7.6, 7.6.1, 8.0)
		p16 = (uint16 *)p;
		*p16++ = htons(M68K_JMP_A0);
		*p16++ = htons(0x7000);
		*p16 = htons(M68K_RTS);
		FlushCodeCache(p, 6);
		D(bug("  patch 1 applied\n"));

	} else if (type == FOURCC('D','R','V','R') && id == 41) {
		D(bug(" DRVR 41 found\n"));
		
		// Don't access ROM85 as it it was a pointer to a ROM version number (8.0, 8.1)
		static const uint8 dat[] = {0x3a, 0x2e, 0x00, 0x0a, 0x55, 0x4f, 0x3e, 0xb8, 0x02, 0x8e, 0x30, 0x1f, 0x48, 0xc0, 0x24, 0x40, 0x20, 0x40};
		base = find_rsrc_data(p, size, dat, sizeof(dat));
		if (base) {
			p16 = (uint16 *)(p + base + 4);
			*p16++ = htons(0x303c);		// move.l	#ROM85,%d0
			*p16++ = htons(0x028e);
			*p16++ = htons(M68K_NOP);
			*p16++ = htons(M68K_NOP);
			FlushCodeCache(p + base + 4, 8);
			D(bug("  patch 1 applied\n"));
		}
	}
}
