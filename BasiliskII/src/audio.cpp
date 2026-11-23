/*
 *  audio.cpp - Audio support
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  Portions written by Marc Hellwig
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

/*
 *  SEE ALSO
 *    Inside Macintosh: Sound, chapter 5 "Sound Components"
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "emul_op.h"
#include "main.h"
#include "audio.h"
#include "audio_defs.h"
#include "user_strings.h"
#include "cdrom.h"

#define DEBUG 0
#include "debug.h"


// Supported sample rates, sizes and channels
vector<uint32> audio_sample_rates;
vector<uint16> audio_sample_sizes;
vector<uint8> audio_channel_counts;

// Global variables
struct audio_status AudioStatus;	// Current audio status (sample rate etc.)
bool audio_open = false;			// Flag: audio is initialized and ready
int audio_frames_per_block;			// Number of audio frames per block
uint32 audio_component_flags;		// Component feature flags
uint32 audio_data = 0;				// Mac address of global data area
static int open_count = 0;			// Open/close nesting count

bool AudioAvailable = false;		// Flag: audio output available (from the software point of view)

int SoundInSource = 2;
int SoundInPlaythrough = 7;
int SoundInGain = 65536; // FIXED 4-byte from 0.5 to 1.5; this is middle value (1) as int

/*
 *  Reset audio emulation
 */

static void SoundInReset(void);

void AudioReset(void)
{
	audio_data = 0;
	SoundInReset();
}


/*
 *  Get audio info
 */

static int32 AudioGetInfo(uint32 infoPtr, uint32 selector, uint32 sourceID)
{
	D(bug(" AudioGetInfo %c%c%c%c, infoPtr %08lx, source ID %08lx\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff, infoPtr, sourceID));
	M68kRegisters r;

	switch (selector) {
		case siSampleSize:
			WriteMacInt16(infoPtr, AudioStatus.sample_size);
			break;

		case siSampleSizeAvailable: {
			r.d[0] = audio_sample_sizes.size() * 2;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_sample_sizes.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 sp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_sample_sizes.size(); i++)
				WriteMacInt16(sp + i*2, audio_sample_sizes[i]);
			break;
		}

		case siNumberChannels:
			WriteMacInt16(infoPtr, AudioStatus.channels);
			break;

		case siChannelAvailable: {
			r.d[0] = audio_channel_counts.size() * 2;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_channel_counts.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 sp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_channel_counts.size(); i++)
				WriteMacInt16(sp + i*2, audio_channel_counts[i]);
			break;
		}

		case siSampleRate:
			WriteMacInt32(infoPtr, AudioStatus.sample_rate);
			break;

		case siSampleRateAvailable: {
			r.d[0] = audio_sample_rates.size() * 4;
			Execute68kTrap(0xa122, &r);	// NewHandle()
			uint32 h = r.a[0];
			if (h == 0)
				return memFullErr;
			WriteMacInt16(infoPtr + sil_count, audio_sample_rates.size());
			WriteMacInt32(infoPtr + sil_infoHandle, h);
			uint32 lp = ReadMacInt32(h);
			for (unsigned i=0; i<audio_sample_rates.size(); i++)
				WriteMacInt32(lp + i*4, audio_sample_rates[i]);
			break;
		}

		case siSpeakerMute:
			WriteMacInt16(infoPtr, audio_get_speaker_mute());
			break;

		case siSpeakerVolume:
			WriteMacInt32(infoPtr, audio_get_speaker_volume());
			break;

		case siHardwareMute:
			WriteMacInt16(infoPtr, audio_get_main_mute());
			break;

		case siHardwareVolume:
			WriteMacInt32(infoPtr, audio_get_main_volume());
			break;

		case siHardwareVolumeSteps:
			WriteMacInt16(infoPtr, 7);
			break;

		case siHardwareBusy:
			WriteMacInt16(infoPtr, AudioStatus.num_sources != 0);
			break;

		case siHardwareFormat:
			WriteMacInt32(infoPtr + scd_flags, 0);
			WriteMacInt32(infoPtr + scd_format, AudioStatus.sample_size == 16 ? FOURCC('t','w','o','s') : FOURCC('r','a','w',' '));
			WriteMacInt16(infoPtr + scd_numChannels, AudioStatus.channels);
			WriteMacInt16(infoPtr + scd_sampleSize, AudioStatus.sample_size);
			WriteMacInt32(infoPtr + scd_sampleRate, AudioStatus.sample_rate);
			WriteMacInt32(infoPtr + scd_sampleCount, audio_frames_per_block);
			WriteMacInt32(infoPtr + scd_buffer, 0);
			WriteMacInt32(infoPtr + scd_reserved, 0);
			break;

		default:	// Delegate to Apple Mixer
			if (AudioStatus.mixer == 0)
				return badComponentSelector;
			M68kRegisters r;
			r.a[0] = infoPtr;
			r.d[0] = selector;
			r.a[1] = sourceID;
			r.a[2] = AudioStatus.mixer;
			Execute68k(audio_data + adatGetInfo, &r);
			D(bug("  delegated to Apple Mixer, returns %08lx\n", r.d[0]));
			return r.d[0];
	}
	return noErr;
}


/*
 *  Set audio info
 */

static int32 AudioSetInfo(uint32 infoPtr, uint32 selector, uint32 sourceID)
{
	D(bug(" AudioSetInfo %c%c%c%c, infoPtr %08lx, source ID %08lx\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff, infoPtr, sourceID));
	M68kRegisters r;

	switch (selector) {
		case siSampleSize:
			D(bug("  set sample size %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.sample_size)
				return noErr;
			for (unsigned i=0; i<audio_sample_sizes.size(); i++)
				if (audio_sample_sizes[i] == infoPtr) {
					if (audio_set_sample_size(i))
						return noErr;
					else
						return siInvalidSampleSize;
				}
			return siInvalidSampleSize;

		case siSampleRate:
			D(bug("  set sample rate %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.sample_rate)
				return noErr;
			for (unsigned i=0; i<audio_sample_rates.size(); i++)
				if (audio_sample_rates[i] == infoPtr) {
					if (audio_set_sample_rate(i))
						return noErr;
					else
						return siInvalidSampleRate;
				}
			return siInvalidSampleRate;

		case siNumberChannels:
			D(bug("  set number of channels %08lx\n", infoPtr));
			if (AudioStatus.num_sources)
				return siDeviceBusyErr;
			if (infoPtr == AudioStatus.channels)
				return noErr;
			for (unsigned i=0; i<audio_channel_counts.size(); i++)
				if (audio_channel_counts[i] == infoPtr) {
					if (audio_set_channels(i))
						return noErr;
					else
						return badChannel;
				}
			return badChannel;

		case siSpeakerMute:
			audio_set_speaker_mute(uint16(infoPtr) != 0);
			break;

		case siSpeakerVolume:
			D(bug("  set speaker volume %08lx\n", infoPtr));
			audio_set_speaker_volume(infoPtr);
			break;

		case siHardwareMute:
			audio_set_main_mute(uint16(infoPtr) != 0);
			break;

		case siHardwareVolume:
			D(bug("  set hardware volume %08lx\n", infoPtr));
			audio_set_main_volume(infoPtr);
			break;

		default:	// Delegate to Apple Mixer
			if (AudioStatus.mixer == 0)
				return badComponentSelector;
			r.a[0] = infoPtr;
			r.d[0] = selector;
			r.a[1] = sourceID;
			r.a[2] = AudioStatus.mixer;
			Execute68k(audio_data + adatSetInfo, &r);
			D(bug("  delegated to Apple Mixer, returns %08lx\n", r.d[0]));
			return r.d[0];
	}
	return noErr;
}


/*
 *  Sound output component dispatch
 */

int32 AudioDispatch(uint32 params, uint32 globals)
{
	D(bug("AudioDispatch params %08lx (size %d), what %d\n", params, ReadMacInt8(params + cp_paramSize), (int16)ReadMacInt16(params + cp_what)));
	M68kRegisters r;
	uint32 p = params + cp_params;
	int16 selector = (int16)ReadMacInt16(params + cp_what);

	switch (selector) {

		// General component functions
		case kComponentOpenSelect:
			if (audio_data == 0) {

				// Allocate global data area
				r.d[0] = SIZEOF_adat;
				Execute68kTrap(0xa040, &r);	// ResrvMem()
				r.d[0] = SIZEOF_adat;
				Execute68kTrap(0xa31e, &r);	// NewPtrClear()
				if (r.a[0] == 0)
					return memFullErr;
				audio_data = r.a[0];
				D(bug(" global data at %08lx\n", audio_data));

				// Put in 68k routines
				int p = audio_data + adatDelegateCall;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x7024); p += 2;	// moveq	#$24,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatOpenMixer)
					goto adat_error;
				WriteMacInt16(p, 0x558f); p += 2;	// subq.l	#2,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x203c); p += 2;	// move.l	#$06140018,d0
				WriteMacInt32(p, 0x06140018); p+= 4;
				WriteMacInt16(p, 0xa800); p += 2;	// SoundDispatch
				WriteMacInt16(p, 0x301f); p += 2;	// move.w	(sp)+,d0
				WriteMacInt16(p, 0x48c0); p += 2;	// ext.l	d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatCloseMixer)
					goto adat_error;
				WriteMacInt16(p, 0x558f); p += 2;	// subq.l	#2,sp
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x203c); p += 2;	// move.l	#$02180018,d0
				WriteMacInt32(p, 0x02180018); p+= 4;
				WriteMacInt16(p, 0xa800); p += 2;	// SoundDispatch
				WriteMacInt16(p, 0x301f); p += 2;	// move.w	(sp)+,d0
				WriteMacInt16(p, 0x48c0); p += 2;	// ext.l	d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatGetInfo)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0103,-(sp)
				WriteMacInt32(p, 0x000c0103); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatSetInfo)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0104,-(sp)
				WriteMacInt32(p, 0x000c0104); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatPlaySourceBuffer)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f0a); p += 2;	// move.l	a2,-(sp)
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f00); p += 2;	// move.l	d0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$000c0108,-(sp)
				WriteMacInt32(p, 0x000c0108); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatGetSourceData)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$00040004,-(sp)
				WriteMacInt32(p, 0x00040004); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatStartSource)
					goto adat_error;
				WriteMacInt16(p, 0x598f); p += 2;	// subq.l	#4,sp
				WriteMacInt16(p, 0x2f09); p += 2;	// move.l	a1,-(sp)
				WriteMacInt16(p, 0x3f00); p += 2;	// move.w	d0,-(sp)
				WriteMacInt16(p, 0x2f08); p += 2;	// move.l	a0,-(sp)
				WriteMacInt16(p, 0x2f3c); p += 2;	// move.l	#$00060105,-(sp)
				WriteMacInt32(p, 0x00060105); p+= 4;
				WriteMacInt16(p, 0x7000); p += 2;	// moveq	#0,d0
				WriteMacInt16(p, 0xa82a); p += 2;	// ComponentDispatch
				WriteMacInt16(p, 0x201f); p += 2;	// move.l	(sp)+,d0
				WriteMacInt16(p, M68K_RTS); p += 2;	// rts
				if (p - audio_data != adatData)
					goto adat_error;
			}
			AudioAvailable = true;
			if (open_count == 0)
				audio_enter_stream();
			open_count++;
			return noErr;

adat_error:	printf("FATAL: audio component data block initialization error\n");
			QuitEmulator();
			return openErr;

		case kComponentCloseSelect:
			open_count--;
			if (open_count == 0) {
				if (audio_data) {
					if (AudioStatus.mixer) {
						// Close Apple Mixer
						r.a[0] = AudioStatus.mixer;
						Execute68k(audio_data + adatCloseMixer, &r);
						D(bug(" CloseMixer() returns %08lx, mixer %08lx\n", r.d[0], AudioStatus.mixer));
						AudioStatus.mixer = 0;
					}
					r.a[0] = audio_data;
					Execute68kTrap(0xa01f, &r);	// DisposePtr()
					audio_data = 0;
				}
				AudioStatus.num_sources = 0;
				audio_exit_stream();
			}
			return noErr;

		case kComponentCanDoSelect:
			switch ((int16)ReadMacInt16(p)) {
				case kComponentOpenSelect:
				case kComponentCloseSelect:
				case kComponentCanDoSelect:
				case kComponentVersionSelect:
				case kComponentRegisterSelect:
				case kSoundComponentInitOutputDeviceSelect:
				case kSoundComponentGetSourceSelect:
				case kSoundComponentGetInfoSelect:
				case kSoundComponentSetInfoSelect:
				case kSoundComponentStartSourceSelect:
					return 1;
				default:
					return 0;
			}

		case kComponentVersionSelect:
			return 0x00010003;

		case kComponentRegisterSelect:
			return noErr;

		// Sound component functions (not delegated)
		case kSoundComponentInitOutputDeviceSelect:
			D(bug(" InitOutputDevice\n"));
			if (!audio_open)
				return noHardwareErr;
			if (AudioStatus.mixer)
				return noErr;

			// Init sound component data
			WriteMacInt32(audio_data + adatData + scd_flags, 0);
			WriteMacInt32(audio_data + adatData + scd_format, AudioStatus.sample_size == 16 ? FOURCC('t','w','o','s') : FOURCC('r','a','w',' '));
			WriteMacInt16(audio_data + adatData + scd_numChannels, AudioStatus.channels);
			WriteMacInt16(audio_data + adatData + scd_sampleSize, AudioStatus.sample_size);
			WriteMacInt32(audio_data + adatData + scd_sampleRate, AudioStatus.sample_rate);
			WriteMacInt32(audio_data + adatData + scd_sampleCount, audio_frames_per_block);
			WriteMacInt32(audio_data + adatData + scd_buffer, 0);
			WriteMacInt32(audio_data + adatData + scd_reserved, 0);
			WriteMacInt32(audio_data + adatStreamInfo, 0);

			// Open Apple Mixer
			r.a[0] = audio_data + adatMixer;
			r.d[0] = 0;
			r.a[1] = audio_data + adatData;
			Execute68k(audio_data + adatOpenMixer, &r);
			AudioStatus.mixer = ReadMacInt32(audio_data + adatMixer);
			D(bug(" OpenMixer() returns %08lx, mixer %08lx\n", r.d[0], AudioStatus.mixer));
			return r.d[0];

		case kSoundComponentGetSourceSelect:
			D(bug(" GetSource source %08lx\n", ReadMacInt32(p)));
			WriteMacInt32(ReadMacInt32(p), AudioStatus.mixer);
			return noErr;

		// Sound component functions (delegated)
		case kSoundComponentAddSourceSelect:
			D(bug(" AddSource\n"));
			AudioStatus.num_sources++;
			goto delegate;

		case kSoundComponentRemoveSourceSelect:
			D(bug(" RemoveSource\n"));
			AudioStatus.num_sources--;
			goto delegate;

		case kSoundComponentGetInfoSelect:
			return AudioGetInfo(ReadMacInt32(p), ReadMacInt32(p + 4), ReadMacInt32(p + 8));

		case kSoundComponentSetInfoSelect:
			return AudioSetInfo(ReadMacInt32(p), ReadMacInt32(p + 4), ReadMacInt32(p + 8));

		case kSoundComponentStartSourceSelect:
			D(bug(" StartSource count %d\n", ReadMacInt16(p + 4)));
			D(bug(" starting Apple Mixer\n"));
			r.d[0] = ReadMacInt16(p + 4);
			r.a[0] = ReadMacInt32(p);
			r.a[1] = AudioStatus.mixer;
			Execute68k(audio_data + adatStartSource, &r);
			D(bug(" returns %08lx\n", r.d[0]));
			return noErr;

		case kSoundComponentStopSourceSelect:
			D(bug(" StopSource\n"));
			goto delegate;

		case kSoundComponentPauseSourceSelect:
			D(bug(" PauseSource\n"));
delegate:	// Delegate call to Apple Mixer
			D(bug(" delegating call to Apple Mixer\n"));
			r.a[0] = AudioStatus.mixer;
			r.a[1] = params;
			Execute68k(audio_data + adatDelegateCall, &r);
			D(bug(" returns %08lx\n", r.d[0]));
			return r.d[0];

		case kSoundComponentPlaySourceBufferSelect:
			D(bug(" PlaySourceBuffer flags %08lx\n", ReadMacInt32(p)));
			r.d[0] = ReadMacInt32(p);
			r.a[0] = ReadMacInt32(p + 4);
			r.a[1] = ReadMacInt32(p + 8);
			r.a[2] = AudioStatus.mixer;
			Execute68k(audio_data + adatPlaySourceBuffer, &r);
			D(bug(" returns %08lx\n", r.d[0]));
			return r.d[0];

		default:
			if (selector >= 0x100)
				goto delegate;
			else
				return badComponentSelector;
	}
}

/*
 *  Sound input driver (.AppleSoundInput replacement)
 *
 *  SEE ALSO
 *    Inside Macintosh: Sound, chapter 3 "Writing a Sound Input Device Driver"
 *
 *  The Mac side issues Status/Control calls with csCode 2 and a selector
 *  in csParam[0..3]. Control data follows the selector; Status answers of
 *  up to 18 bytes go back inline (byte count in csParam[0..3], data from
 *  csParam+4), longer ones are copied to the buffer whose address is in
 *  csParam+4 (and the count is 0).
 *
 *  A Read (Prime) records into ioBuffer. It stays pending (Prime returns 1)
 *  and is filled incrementally from SoundInInterrupt(), which the host
 *  backend triggers whenever new data arrives; ioActCount is kept current so
 *  a KillIO (SPBStop) leaves the recorded length in place. When the buffer
 *  is full the request is completed with IODone from a deferred task.
 */

const audio_in_backend *AudioInBackend = NULL;

static const int16 siUnknownInfoType = -231;
static const int16 siDeviceBusyErrIn = -227;

// Deferred task that calls IODone (same layout as the serial driver's)
enum {
	sidtCode = 20,		// 68k code
	sidtResult = 30,	// result code for IODone
	sidtDCE = 34,		// DCE for IODone
	SIZEOF_sidt = 38
};

// Rates offered to the Mac (16.16 fixed); the backend converts from the host
static const uint32 soundin_rates[] = {
	0x1f400000,		//  8000 Hz
	0x2b110000,		// 11025 Hz
	0x2b7745d1,		// 11127 Hz (Mac 11 kHz)
	0x56220000,		// 22050 Hz
	0x56ee8ba3,		// 22254 Hz (Mac 22 kHz)
	0xac440000,		// 44100 Hz
	0xbb800000		// 48000 Hz
};
static const int NUM_SOUNDIN_RATES = sizeof(soundin_rates) / sizeof(soundin_rates[0]);

static struct {
	// Driver settings
	uint32 sample_rate;			// 16.16 fixed
	int16 sample_size;			// 8 or 16
	int16 channels;				// 1 or 2
	bool twos_complement;		// 8-bit samples signed
	bool continuous;
	bool paused;
	bool agc;
	bool meter_on;
	uint32 quality;
	uint32 gain;				// 16.16 fixed, 0.5 .. 1.5
	uint32 user_proc;			// Sound Input Manager interrupt routine (not called)
	int16 source;
	int16 playthrough;

	// State
	uint32 dt;					// deferred task (Mac address)
	bool device_running;		// host capture is open
	bool recording;				// a recording session is active
	volatile bool read_pending;	// a Read is waiting for data
	uint32 read_pb, read_dce;
	uint32 read_done;			// bytes stored so far
	uint8 level;				// last peak level, 0..255
} si;

static void soundin_set_defaults(void)
{
	si.sample_rate = 0x56ee8ba3;	// "if possible: 22 kHz, 8 bit, mono, no compression, AGC on"
	si.sample_size = 8;
	si.channels = 1;
	si.twos_complement = false;
	si.continuous = false;
	si.paused = false;
	si.agc = true;
	si.meter_on = false;
	si.quality = siBestQuality;
	si.gain = 0x10000;
	si.user_proc = 0;
	si.source = 1;
	si.playthrough = 0;
}

static int soundin_rate_hz(void)
{
	return (int)((si.sample_rate + 0x8000) >> 16);
}

static int soundin_frame_size(void)
{
	return (si.sample_size / 8) * si.channels;
}

// Open host capture, or apply the current format to it
static bool soundin_start_device(void)
{
	if (AudioInBackend == NULL)
		return false;
	if (!AudioInBackend->start(soundin_rate_hz(), si.sample_size, si.channels, si.sample_size == 16 || si.twos_complement)) {
		D(bug(" sound input: host capture failed to start\n"));
		si.device_running = false;
		return false;
	}
	AudioInBackend->set_gain(si.gain / 65536.0f);
	si.device_running = true;
	return true;
}

static void soundin_stop_device(void)
{
	if (si.device_running && AudioInBackend)
		AudioInBackend->stop();
	si.device_running = false;
}

// Format changed by the Mac: reformat capture if it is running
static void soundin_format_changed(void)
{
	if (si.device_running)
		soundin_start_device();
}

// Peak level of a block of samples in the current format, 0..255
static uint8 soundin_peak(const uint8 *p, uint32 len)
{
	int peak = 0;
	if (si.sample_size == 16) {
		for (uint32 i = 0; i + 1 < len; i += 2) {
			int v = (int16)((p[i] << 8) | p[i + 1]);
			if (v < 0) v = -v;
			if (v > peak) peak = v;
		}
		peak >>= 7;
	} else {
		for (uint32 i = 0; i < len; i++) {
			int v = si.twos_complement ? (int8)p[i] : (int)p[i] - 128;
			if (v < 0) v = -v;
			if (v > peak) peak = v;
		}
		peak <<= 1;
	}
	return peak > 255 ? 255 : peak;
}

// Read data from the host, only whole frames
static uint32 soundin_host_read(uint8 *buf, uint32 len)
{
	uint32 frame = soundin_frame_size();
	len -= len % frame;
	if (len == 0)
		return 0;
	uint32 n = AudioInBackend->read(buf, len);
	if (n)
		si.level = soundin_peak(buf, n);
	return n;
}

// Complete the pending Read via IODone, from a deferred task
static void soundin_complete_read(int16 result)
{
	si.read_pending = false;
	WriteMacInt32(si.read_pb + ioActCount, si.read_done);
	WriteMacInt32(si.dt + sidtResult, result);
	WriteMacInt32(si.dt + sidtDCE, si.read_dce);
	M68kRegisters r;
	r.a[0] = si.dt;
	Execute68kTrap(0xa082, &r);		// DTInstall()
	D(bug(" sound input read complete, %u bytes\n", si.read_done));
}

// Called by the host backend from its own thread
void SoundInDataArrived(void)
{
	if (si.read_pending) {
		SetInterruptFlag(INTFLAG_SOUNDIN);
		TriggerInterrupt();
	}
}

// Interrupt: move recorded data into the pending Read
void SoundInInterrupt(void)
{
	if (!si.read_pending || !si.device_running)
		return;

	uint32 buffer = ReadMacInt32(si.read_pb + ioBuffer);
	uint32 wanted = ReadMacInt32(si.read_pb + ioReqCount);
	uint8 tmp[4096];

	while (si.read_done < wanted) {
		uint32 n = wanted - si.read_done;
		if (n > sizeof(tmp))
			n = sizeof(tmp);
		n = soundin_host_read(tmp, n);
		if (n == 0)
			break;
		if (si.paused)
			continue;			// recording continues but is not stored
		if (buffer)
			Host2Mac_memcpy(buffer + si.read_done, tmp, n);
		si.read_done += n;		// NIL buffer: record without saving
	}
	WriteMacInt32(si.read_pb + ioActCount, si.read_done);

	if (si.read_done >= wanted) {
		soundin_complete_read(noErr);
		if (!si.continuous) {
			// Next Read starts a new recording; stop capturing until then
			si.recording = false;
			if (!si.meter_on)
				soundin_stop_device();
		}
	}
}

// Stop recording (KillIO, siCloseDriver, Close)
static void soundin_stop_recording(void)
{
	if (si.read_pending) {
		SoundInInterrupt();		// take what has arrived
		si.read_pending = false;
		WriteMacInt32(si.read_pb + ioActCount, si.read_done);
		D(bug(" sound input stopped, %u bytes recorded\n", si.read_done));
	}
	si.recording = false;
	if (!si.meter_on)
		soundin_stop_device();
}


// Mac restarted: the deferred task and any pending Read are gone
static void SoundInReset(void)
{
	soundin_stop_device();
	si.dt = 0;
	si.read_pending = false;
	si.recording = false;
	si.meter_on = false;
}


/*
 *  Sound input driver Open() routine
 */

int16 SoundInOpen(uint32 pb, uint32 dce)
{
	D(bug("SoundInOpen\n"));

	if (si.dt == 0) {
		M68kRegisters r;
		r.d[0] = SIZEOF_sidt;
		Execute68kTrap(0xa71e, &r);	// NewPtrSysClear()
		if (r.a[0] == 0)
			return memFullErr;
		uint32 dt = si.dt = r.a[0];
		WriteMacInt16(dt + qType, dtQType);
		WriteMacInt32(dt + dtAddr, dt + sidtCode);
		WriteMacInt32(dt + dtParam, dt + sidtResult);
		WriteMacInt16(dt + sidtCode, 0x2019);			// move.l	(a1)+,d0	(result)
		WriteMacInt16(dt + sidtCode + 2, 0x2251);		// move.l	(a1),a1		(dce)
		WriteMacInt32(dt + sidtCode + 4, 0x207808fc);	// move.l	JIODone,a0
		WriteMacInt16(dt + sidtCode + 8, 0x4ed0);		// jmp		(a0)
	}

	soundin_set_defaults();
	si.recording = si.read_pending = false;
	si.level = 0;
	return noErr;
}


/*
 *  Sound input driver Prime() routine (Read: record into ioBuffer)
 */

int16 SoundInPrime(uint32 pb, uint32 dce)
{
	uint32 wanted = ReadMacInt32(pb + ioReqCount);
	D(bug("SoundInPrime %u bytes\n", wanted));

	if (AudioInBackend == NULL)
		return noHardwareErr;
	if (si.read_pending)
		return siDeviceBusyErrIn;

	if (!si.device_running && !soundin_start_device())
		return noHardwareErr;
	if (!si.recording) {
		// New recording: drop anything captured before it (e.g. while metering)
		AudioInBackend->clear();
		si.recording = true;
	}

	si.read_pb = pb;
	si.read_dce = dce;
	si.read_done = 0;
	WriteMacInt32(pb + ioActCount, 0);
	if (wanted == 0) {
		si.read_pending = true;
		soundin_complete_read(noErr);
		return 1;
	}
	si.read_pending = true;

	// Data may already be waiting (continuous recording)
	SetInterruptFlag(INTFLAG_SOUNDIN);
	TriggerInterrupt();
	return 1;	// in progress, completed via IODone
}


/*
 *  Sound input driver Control() routine
 */

int16 SoundInControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	D(bug("SoundInControl %d\n", code));

	if (code == 1) {	// KillIO: stop copying, ioActCount holds the recorded length
		D(bug(" SoundInKillIO\n"));
		soundin_stop_recording();
		return noErr;
	}

	if (code != 2)
		return siUnknownInfoType;

	uint32 selector = ReadMacInt32(pb + csParam);
	uint32 data = pb + csParam + 4;
	D(bug(" selector %c%c%c%c\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff));

	switch (selector) {
		case siInitializeDriver:
			soundin_stop_recording();
			soundin_set_defaults();
			return noErr;

		case siCloseDriver:
			si.meter_on = false;
			soundin_stop_recording();
			soundin_stop_device();
			soundin_set_defaults();
			return noErr;

		case siSampleRate: {
			uint32 rate = ReadMacInt32(data);
			if (rate < 0x0fa00000 || rate > 0xbb800000)	// 4000 .. 48000 Hz
				return paramErr;
			si.sample_rate = rate;
			soundin_format_changed();
			return noErr;
		}

		case siSampleSize: {
			int16 size = ReadMacInt16(data);
			if (size != 8 && size != 16)
				return paramErr;
			si.sample_size = size;
			soundin_format_changed();
			return noErr;
		}

		case siNumberChannels: {
			int16 n = ReadMacInt16(data);
			if (n != 1 && n != 2)
				return paramErr;
			si.channels = n;
			soundin_format_changed();
			return noErr;
		}

		case siTwosComplementOnOff:
			si.twos_complement = ReadMacInt16(data) != 0;
			soundin_format_changed();
			return noErr;

		case siCompressionType: {
			uint32 type = ReadMacInt32(data);
			if (type != 0 && type != FOURCC('N','O','N','E') && type != FOURCC('r','a','w',' ') && type != FOURCC('t','w','o','s'))
				return paramErr;	// samples are always delivered uncompressed
			return noErr;
		}

		case siRecordingQuality:
			si.quality = ReadMacInt32(data);	// all qualities record uncompressed
			return noErr;

		case siContinuous:
			si.continuous = ReadMacInt16(data) != 0;
			return noErr;

		case siPauseRecording:
			si.paused = ReadMacInt16(data) != 0;
			return noErr;

		case siAGCOnOff:
			si.agc = ReadMacInt16(data) != 0;
			return noErr;

		case siLevelMeterOnOff:
			si.meter_on = ReadMacInt16(data) != 0;
			if (si.meter_on) {
				if (!si.device_running)
					soundin_start_device();
			} else if (!si.recording)
				soundin_stop_device();
			return noErr;

		case siInputGain:
			si.gain = ReadMacInt32(data);
			if (si.gain < 0x8000) si.gain = 0x8000;
			if (si.gain > 0x18000) si.gain = 0x18000;
			if (si.device_running)
				AudioInBackend->set_gain(si.gain / 65536.0f);
			return noErr;

		case siInputSource: {
			int16 s = ReadMacInt16(data);
			if (s < 1 || s > 2)
				return paramErr;
			si.source = s;		// both sources record from the host default device
			return noErr;
		}

		case siPlayThruOnOff:
			si.playthrough = ReadMacInt16(data);	// accepted, not implemented
			return noErr;

		case siUserInterruptProc:
			si.user_proc = ReadMacInt32(data);	// stored, not called
			return noErr;

		case siOptionsDialog:
			return siUnknownInfoType;	// no options dialog

		default:
			return siUnknownInfoType;
	}
}


/*
 *  Sound input driver Status() routine
 */

// Answer inline: byte count in csParam[0..3], data from csParam+4 (<= 18 bytes)
static void status_int16(uint32 pb, int16 v)
{
	WriteMacInt32(pb + csParam, 2);
	WriteMacInt16(pb + csParam + 4, v);
}

static void status_int32(uint32 pb, uint32 v)
{
	WriteMacInt32(pb + csParam, 4);
	WriteMacInt32(pb + csParam + 4, v);
}

static void status_count_handle(uint32 pb, int16 count, uint32 h)
{
	WriteMacInt32(pb + csParam, 6);
	WriteMacInt16(pb + csParam + 4, count);
	WriteMacInt32(pb + csParam + 6, h);
}

static uint32 new_handle(uint32 size)
{
	M68kRegisters r;
	r.d[0] = size;
	Execute68kTrap(0xa122, &r);	// NewHandle()
	return r.a[0];
}

int16 SoundInStatus(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	D(bug("SoundInStatus %d\n", code));
	if (code != 2)
		return siUnknownInfoType;

	uint32 selector = ReadMacInt32(pb + csParam);
	D(bug(" selector %c%c%c%c\n", selector >> 24, (selector >> 16) & 0xff, (selector >> 8) & 0xff, selector & 0xff));

	switch (selector) {
		case siDeviceName: {	// Str255, longer than 18 bytes: copy to the caller's buffer
			uint32 bufferptr = ReadMacInt32(pb + csParam + 4);
			static const uint8 str[] = { 8, 'B', 'u', 'i', 'l', 't', '-', 'i', 'n' };
			Host2Mac_memcpy(bufferptr, str, sizeof(str));
			WriteMacInt32(pb + csParam, 0);
			return noErr;
		}

		case siDeviceIcon: {	// handle to 'ICN#' data; borrow the CD-ROM icon
			uint32 h = new_handle(sizeof(CDROMIcon));
			if (h == 0)
				return memFullErr;
			Host2Mac_memcpy(ReadMacInt32(h), CDROMIcon, sizeof(CDROMIcon));
			status_int32(pb, h);
			return noErr;
		}

		case siDeviceConnected:
			status_int16(pb, AudioInBackend ? 1 : 0);	// siDeviceIsConnected / NotConnected
			return noErr;

		case siAsync:
			status_int16(pb, 1);
			return noErr;

		case siContinuous:
			status_int16(pb, si.continuous);
			return noErr;

		case siPauseRecording:
			status_int16(pb, si.paused);
			return noErr;

		case siAGCOnOff:
			status_int16(pb, si.agc);
			return noErr;

		case siTwosComplementOnOff:
			status_int16(pb, si.twos_complement);
			return noErr;

		case siDeviceBufferInfo:
			status_int32(pb, 4096);
			return noErr;

		case siNumberChannels:
			status_int16(pb, si.channels);
			return noErr;

		case siChannelAvailable:
			status_int16(pb, 2);
			return noErr;

		case siActiveChannels:
			status_int32(pb, (1 << si.channels) - 1);
			return noErr;

		case siSampleSize:
			status_int16(pb, si.sample_size);
			return noErr;

		case siSampleSizeAvailable: {
			uint32 h = new_handle(2 * 2);
			if (h == 0)
				return memFullErr;
			WriteMacInt16(ReadMacInt32(h), 8);
			WriteMacInt16(ReadMacInt32(h) + 2, 16);
			status_count_handle(pb, 2, h);
			return noErr;
		}

		case siSampleRate:
			status_int32(pb, si.sample_rate);
			return noErr;

		case siSampleRateAvailable: {
			uint32 h = new_handle(NUM_SOUNDIN_RATES * 4);
			if (h == 0)
				return memFullErr;
			for (int i = 0; i < NUM_SOUNDIN_RATES; i++)
				WriteMacInt32(ReadMacInt32(h) + i * 4, soundin_rates[i]);
			status_count_handle(pb, NUM_SOUNDIN_RATES, h);
			return noErr;
		}

		case siCompressionType:
			status_int32(pb, FOURCC('N','O','N','E'));
			return noErr;

		case siCompressionFactor:
			status_int16(pb, 1);
			return noErr;

		case siCompressionAvailable: {
			uint32 h = new_handle(4);
			if (h == 0)
				return memFullErr;
			WriteMacInt32(ReadMacInt32(h), FOURCC('N','O','N','E'));
			status_count_handle(pb, 1, h);
			return noErr;
		}

		case siRecordingQuality:
			status_int32(pb, si.quality);
			return noErr;

		case siLevelMeterOnOff: {
			// While not recording, drain capture to measure the current level
			if (si.meter_on && si.device_running && !si.read_pending) {
				uint8 tmp[4096];
				for (int i = 0; i < 64 && soundin_host_read(tmp, sizeof(tmp)); i++)
					;
			}
			WriteMacInt32(pb + csParam, 4);
			WriteMacInt16(pb + csParam + 4, si.meter_on);
			WriteMacInt16(pb + csParam + 6, si.meter_on ? si.level : 0);
			return noErr;
		}

		case siInputGain:
			status_int32(pb, si.gain);
			return noErr;

		case siInputAvailable:
			status_int16(pb, 2);
			return noErr;

		case siInputSource:
			status_int16(pb, si.source);
			return noErr;

		case siInputSourceNames: {	// handle to 'STR#' data
			static const uint8 str[] = {
				0x00, 0x02,
				10, 'M', 'i', 'c', 'r', 'o', 'p', 'h', 'o', 'n', 'e',
				11, 'I', 'n', 't', 'e', 'r', 'n', 'a', 'l', ' ', 'C', 'D'
			};
			uint32 h = new_handle(sizeof(str));
			if (h == 0)
				return memFullErr;
			Host2Mac_memcpy(ReadMacInt32(h), str, sizeof(str));
			status_int32(pb, h);
			return noErr;
		}

		case siOptionsDialog:
			status_int16(pb, 0);	// no options dialog
			return noErr;

		case siPlayThruOnOff:
			status_int16(pb, si.playthrough);
			return noErr;

		default:
			return siUnknownInfoType;
	}
}


/*
 *  Sound input driver Close() routine
 */

int16 SoundInClose(uint32 pb, uint32 dce)
{
	D(bug("SoundInClose\n"));
	si.meter_on = false;
	soundin_stop_recording();
	soundin_stop_device();
	return noErr;
}
