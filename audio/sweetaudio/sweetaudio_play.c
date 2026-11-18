/* SPDX-License-Identifier: MIT */
/*
 * sweetaudio_play [hal|aq|both]: the CoreAudio side of doc 54 (run after the kernel /dev/sweetaudio is up).
 *   1. lists the HAL devices mediaserverd publishes (id, name, UID, transport, output streams) and the default output device;
 *   2. hal: waits (<= 30 s) for SweetAudio.driver's device "PixOSSweetAudio:0", then plays 4 beeps (660 Hz, 0.3 s on / 0.3 s off, -10 dBFS
 *      float) through an IOProc on it (HAL client API: mediaserverd -> plug-in -> /dev/sweetaudio);
 *   3. aq: plays 5 beeps (1320 Hz) through an AudioQueue on the default output (the app-level path: needs VirtualAudio's routing).
 * Every step prints one "sweetaudio_play: ..." line with the OSStatus. Bounded: every wait has a limit, at most ~12 s in all.
 */
#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UID_WANT "PixOSSweetAudio:0"
#define RATE 48000.0

static void
cfstr(CFStringRef s, char *buf, size_t n)
{
	buf[0] = 0;
	if (s == NULL || !CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8))
		snprintf(buf, n, "?");
}

static OSStatus
get(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 *size, void *data)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	return AudioObjectGetPropertyData(o, &a, 0, NULL, size, data);
}

/* list devices; returns the SweetAudio device id (0 = not found) */
static AudioObjectID
list(int verbose)
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	AudioObjectID ids[32], found = 0, def = 0;
	OSStatus e = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
	if (e == 0) {
		if (size > sizeof ids)
			size = sizeof ids;
		e = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
	}
	if (e != 0) {
		printf("sweetaudio_play: device list error %d\n", (int)e);
		return 0;
	}
	for (UInt32 i = 0; i < size / sizeof ids[0]; i++) {
		CFStringRef name = NULL, uid = NULL;
		UInt32 tr = 0, sz = sizeof name, ns = 0;
		char nb[128], ub[128];
		get(ids[i], kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &sz, &name);
		sz = sizeof uid;
		get(ids[i], kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, &sz, &uid);
		sz = sizeof tr;
		get(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &sz, &tr);
		AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
		AudioObjectGetPropertyDataSize(ids[i], &sa, 0, NULL, &ns);
		cfstr(name, nb, sizeof nb);
		cfstr(uid, ub, sizeof ub);
		if (verbose)
			printf("sweetaudio_play: device %u name '%s' uid '%s' transport '%c%c%c%c' out streams %u\n", (unsigned)ids[i], nb, ub,
			    (char)(tr >> 24), (char)(tr >> 16), (char)(tr >> 8), (char)tr, (unsigned)(ns / sizeof(AudioStreamID)));
		if (strcmp(ub, UID_WANT) == 0)
			found = ids[i];
		if (name)
			CFRelease(name);
		if (uid)
			CFRelease(uid);
	}
	if (verbose) {
		UInt32 sz = sizeof def;
		e = get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, &sz, &def);
		printf("sweetaudio_play: %u devices, default output %u (err %d), SweetAudio device %u\n", (unsigned)(size / sizeof ids[0]),
		    (unsigned)def, (int)e, (unsigned)found);
	}
	return found;
}

struct beeper { double hz; unsigned beeps; uint64_t frame; uint64_t total; uint64_t rendered; };

static float
beep_sample(struct beeper *b, uint64_t f)
{
	const uint64_t on = (uint64_t)(0.3 * RATE), per = 2 * on;
	if (f >= b->total || f % per >= on)
		return 0.0f;
	return (float)(0.316 * sin(2.0 * M_PI * b->hz * (double)f / RATE));
}

static OSStatus
ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it, AudioBufferList *out,
    const AudioTimeStamp *ot, void *ctx)
{
	(void)dev; (void)now; (void)in; (void)it; (void)ot;
	struct beeper *b = ctx;
	UInt32 frames = 0;
	for (UInt32 k = 0; k < out->mNumberBuffers; k++) {
		AudioBuffer *ab = &out->mBuffers[k];
		UInt32 ch = ab->mNumberChannels ? ab->mNumberChannels : 1, n = ab->mDataByteSize / (UInt32)sizeof(float) / ch;
		float *p = ab->mData;
		for (UInt32 i = 0; p && i < n; i++)
			for (UInt32 c = 0; c < ch; c++)
				p[i * ch + c] = beep_sample(b, b->frame + i);
		frames = n;
	}
	b->frame += frames;
	b->rendered += frames;
	return 0;
}

static int
play_hal(void)
{
	AudioObjectID dev = 0;
	for (int i = 0; i < 30 && (dev = list(0)) == 0; i++)
		sleep(1);
	if (dev == 0) {
		printf("sweetaudio_play: hal: FAIL no device %s after 30 s\n", UID_WANT);
		return 1;
	}
	struct beeper b = { 660.0, 4, 0, (uint64_t)(4 * 0.6 * RATE), 0 };
	AudioDeviceIOProcID pid = NULL;
	OSStatus e = AudioDeviceCreateIOProcID(dev, ioproc, &b, &pid);
	printf("sweetaudio_play: hal: device %u create ioproc err %d\n", (unsigned)dev, (int)e);
	if (e != 0)
		return 1;
	e = AudioDeviceStart(dev, pid);
	printf("sweetaudio_play: hal: start err %d (4 beeps 660 Hz now)\n", (int)e);
	for (int i = 0; e == 0 && i < 40 && b.frame < b.total + (uint64_t)(0.3 * RATE); i++)
		usleep(100000);
	OSStatus s = AudioDeviceStop(dev, pid);
	AudioDeviceDestroyIOProcID(dev, pid);
	printf("sweetaudio_play: hal: done rendered %llu frames, stop err %d\n", (unsigned long long)b.rendered, (int)s);
	return e != 0 || b.rendered == 0;
}

static void
aq_cb(void *ctx, AudioQueueRef q, AudioQueueBufferRef buf)
{
	struct beeper *b = ctx;
	UInt32 n = buf->mAudioDataBytesCapacity / 8u;
	float *p = buf->mAudioData;
	for (UInt32 i = 0; i < n; i++)
		p[2 * i] = p[2 * i + 1] = beep_sample(b, b->frame + i);
	b->frame += n;
	b->rendered += n;
	buf->mAudioDataByteSize = n * 8u;
	AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static int
play_aq(void)
{
	AudioStreamBasicDescription f = { RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
	struct beeper b = { 1320.0, 5, 0, (uint64_t)(5 * 0.6 * RATE), 0 };
	AudioQueueRef q = NULL;
	OSStatus e = AudioQueueNewOutput(&f, aq_cb, &b, NULL, NULL, 0, &q);
	printf("sweetaudio_play: aq: new output err %d\n", (int)e);
	if (e != 0)
		return 1;
	for (int i = 0; i < 3; i++) {
		AudioQueueBufferRef buf = NULL;
		if (AudioQueueAllocateBuffer(q, 4800 * 8, &buf) == 0)
			aq_cb(&b, q, buf);
	}
	e = AudioQueueStart(q, NULL);
	printf("sweetaudio_play: aq: start err %d (5 beeps 1320 Hz now)\n", (int)e);
	for (int i = 0; e == 0 && i < 50 && b.frame < b.total + (uint64_t)(0.6 * RATE); i++)
		usleep(100000);
	OSStatus s = AudioQueueStop(q, true);
	AudioQueueDispose(q, true);
	printf("sweetaudio_play: aq: done rendered %llu frames, stop err %d\n", (unsigned long long)b.rendered, (int)s);
	return e != 0;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *m = argc > 1 ? argv[1] : "both";
	int rc = 0;
	list(1);
	if (strcmp(m, "hal") == 0 || strcmp(m, "both") == 0)
		rc |= play_hal();
	if (strcmp(m, "both") == 0)
		sleep(1);
	if (strcmp(m, "aq") == 0 || strcmp(m, "both") == 0)
		rc |= play_aq();
	list(1);
	printf("sweetaudio_play: end rc %d\n", rc);
	return rc;
}
