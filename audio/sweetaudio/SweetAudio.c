/* SPDX-License-Identifier: MIT */
/*
 * SweetAudio.driver: an AudioServerPlugIn (public API, AudioServerPlugIn.h of the macOS SDK) for PixOS.
 * One output device, one stereo output stream at 48 kHz. The HAL (HALS_PlugInManager in mediaserverd, iOS 16) mixes in
 * Float32; WriteMix converts to S16LE and write()s it to /dev/sweetaudio (contract: sweetaudio_dev.h, doc 54).
 * Without the device node (kernel path not up) the device still works as a silent sink, so routing can be tested first.
 *
 * Objects: 1 = plug-in (kAudioObjectPlugInObject), 2 = device, 3 = output stream. No controls (volume is the HAL's /
 * VirtualAudio's software volume; the kernel clamps gain, plan rule A-3).
 * Info.plist keys read at Initialize (all optional): SweetAudioTransportType (4 chars, default "usb "), SweetAudioName.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sa_core.h"
#include "sweetaudio_dev.h"

enum { kObjPlugIn = kAudioObjectPlugInObject, kObjDevice = 2, kObjStream = 3 };

#define SA_BUNDLE_ID   "org.pixelios.audio.SweetAudio"
#define SA_DEVICE_UID  "PixOSSweetAudio:0"
#define SA_MODEL_UID   "PixOSSweetAudio:model"
#define SA_PERIOD      16384u     /* ZeroTimeStampPeriod in frames */
#define SA_LATENCY     480u       /* frames: kernel start threshold (960) is the worst case; report the typical half */
#define SA_SAFETY      240u       /* frames */
#define SA_MAX_FRAMES  4096u      /* largest IO buffer we convert in one go (the HAL default is 512) */

static pthread_mutex_t sa_lock = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef sa_host;
static _Atomic uint32_t sa_refs = 1;
static UInt32 sa_transport = kAudioDeviceTransportTypeUSB;
static CFStringRef sa_name;
static uint32_t sa_running;       /* StartIO count */
static int sa_fd = -1;
static struct sa_clock sa_clk;
static uint64_t sa_dropped, sa_written, sa_ioerr;
static int16_t sa_buf[SA_MAX_FRAMES * SWAU_CHANNELS];

static void
sa_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
sa_log(const char *fmt, ...)
{
	/* stderr of mediaserverd: launchd points it at /dev/null; boot-arg sweetnull=...,mediaserverd tees it to the console (0048) */
	va_list ap;
	va_start(ap, fmt);
	fputs("sweetaudio: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static AudioStreamBasicDescription
sa_format(void)
{
	AudioStreamBasicDescription f = { 0 };
	f.mSampleRate = SWAU_RATE;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
	f.mBytesPerPacket = 4 * SWAU_CHANNELS;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4 * SWAU_CHANNELS;
	f.mChannelsPerFrame = SWAU_CHANNELS;
	f.mBitsPerChannel = 32;
	return f;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* IUnknown + factory */

static AudioServerPlugInDriverInterface sa_iface;
static AudioServerPlugInDriverInterface *sa_iface_ptr = &sa_iface;
static AudioServerPlugInDriverRef sa_ref = &sa_iface_ptr;

static HRESULT
sa_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
	if (inDriver != sa_ref || outInterface == NULL)
		return kAudioHardwareBadObjectError;
	CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
	if (req == NULL)
		return kAudioHardwareIllegalOperationError;
	HRESULT r = E_NOINTERFACE;
	if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
		atomic_fetch_add(&sa_refs, 1);
		*outInterface = sa_ref;
		r = S_OK;
	}
	CFRelease(req);
	return r;
}

static ULONG
sa_AddRef(void *inDriver)
{
	return inDriver == sa_ref ? atomic_fetch_add(&sa_refs, 1) + 1 : 0;
}

static ULONG
sa_Release(void *inDriver)
{
	if (inDriver != sa_ref)
		return 0;
	uint32_t v = atomic_load(&sa_refs);
	while (v > 0 && !atomic_compare_exchange_weak(&sa_refs, &v, v - 1))
		;
	return v > 0 ? v - 1 : 0;   /* the object is static: nothing to free */
}

static void
sa_read_config(void)
{
	CFBundleRef b = CFBundleGetBundleWithIdentifier(CFSTR(SA_BUNDLE_ID));
	CFStringRef t = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("SweetAudioTransportType")) : NULL;
	char s[8] = { 0 };
	if (t && CFGetTypeID(t) == CFStringGetTypeID() && CFStringGetCString(t, s, sizeof s, kCFStringEncodingASCII) && strlen(s) == 4)
		sa_transport = ((UInt32)(uint8_t)s[0] << 24) | ((UInt32)(uint8_t)s[1] << 16) | ((UInt32)(uint8_t)s[2] << 8) | (uint8_t)s[3];
	CFStringRef n = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("SweetAudioName")) : NULL;
	sa_name = (n && CFGetTypeID(n) == CFStringGetTypeID()) ? CFRetain(n) : CFSTR("PixOS Speaker");
	sa_log("initialize: transport 0x%08x, bundle %s", (unsigned)sa_transport, b ? "found" : "NOT found (defaults)");
}

static OSStatus
sa_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
	if (inDriver != sa_ref)
		return kAudioHardwareBadObjectError;
	sa_host = inHost;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	/* ticks per frame = (1e9 ns / rate) / (numer/denom ns per tick) */
	sa_clk.ticks_per_frame = (1e9 / (double)SWAU_RATE) * (double)tb.denom / (double)tb.numer;
	sa_clk.period = SA_PERIOD;
	sa_read_config();
	return kAudioHardwareNoError;
}

static OSStatus
sa_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{
	(void)d; (void)desc; (void)ci; (void)out;
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
sa_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev)
{
	(void)d; (void)dev;
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
sa_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{
	(void)ci;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus
sa_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{
	(void)ci;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus
sa_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
	(void)action; (void)info;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus
sa_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
	(void)action; (void)info;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Properties */

static Boolean
sa_has(AudioObjectID obj, const AudioObjectPropertyAddress *a)
{
	switch (obj) {
	case kObjPlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyBoxList:
		case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice: case kAudioPlugInPropertyResourceBundle:
			return true;
		}
		return false;
	case kObjDevice:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning: case kAudioObjectPropertyControlList: case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
		case kAudioDevicePropertyClockIsStable:
			return true;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency: case kAudioDevicePropertyStreams: case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyPreferredChannelsForStereo:
			return a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput;
		}
		return false;
	case kObjStream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName: case kAudioObjectPropertyOwnedObjects:
		case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			return true;
		}
		return false;
	}
	return false;
}

static Boolean
sa_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
	(void)pid;
	return d == sa_ref && a != NULL && sa_has(obj, a);
}

static OSStatus
sa_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
	(void)pid;
	if (d != sa_ref || a == NULL || out == NULL)
		return kAudioHardwareIllegalOperationError;
	if (!sa_has(obj, a))
		return kAudioHardwareUnknownPropertyError;
	/* Settable to the one value we support, so clients that "set" 48 kHz / our format succeed. */
	*out = (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
	       (obj == kObjStream && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat));
	return kAudioHardwareNoError;
}

/* One table drives both GetPropertyDataSize and GetPropertyData: fill() writes into out when out != NULL. */
static OSStatus
sa_get(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 cap, UInt32 *size, void *out)
{
#define PUT(type, val) do { if (out) { if (cap < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)out = (val); } \
	*size = sizeof(type); return kAudioHardwareNoError; } while (0)
#define PUT_IDS(n, ...) do { AudioObjectID ids_[] = { __VA_ARGS__ }; UInt32 k_ = (n); if (out) { UInt32 m_ = cap / sizeof(AudioObjectID); \
	if (m_ < k_) k_ = m_; memcpy(out, ids_, k_ * sizeof(AudioObjectID)); } *size = k_ * sizeof(AudioObjectID); return kAudioHardwareNoError; } while (0)
	if (!sa_has(obj, a))
		return kAudioHardwareUnknownPropertyError;
	AudioStreamBasicDescription f = sa_format();
	switch (obj) {
	case kObjPlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
		case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("PixOS"));
		case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: PUT_IDS(1, kObjDevice);
		case kAudioPlugInPropertyBoxList: PUT_IDS(0, 0);
		case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
		case kAudioPlugInPropertyTranslateUIDToDevice: {
			AudioObjectID id = kAudioObjectUnknown;
			if (qsize == sizeof(CFStringRef) && qdata && *(const CFStringRef *)qdata &&
			    CFStringCompare(*(const CFStringRef *)qdata, CFSTR(SA_DEVICE_UID), 0) == kCFCompareEqualTo)
				id = kObjDevice;
			PUT(AudioObjectID, id);
		}
		}
		break;
	case kObjDevice:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjPlugIn);
		case kAudioObjectPropertyName: PUT(CFStringRef, (CFStringRef)CFRetain(sa_name));
		case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("PixOS"));
		case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(SA_DEVICE_UID));
		case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(SA_MODEL_UID));
		case kAudioDevicePropertyTransportType: PUT(UInt32, sa_transport);
		case kAudioDevicePropertyRelatedDevices: PUT_IDS(1, kObjDevice);
		case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
		case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
		case kAudioDevicePropertyDeviceIsRunning: { pthread_mutex_lock(&sa_lock); UInt32 r = sa_running > 0; pthread_mutex_unlock(&sa_lock); PUT(UInt32, r); }
		case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
		case kAudioDevicePropertyLatency: PUT(UInt32, SA_LATENCY);
		case kAudioDevicePropertySafetyOffset: PUT(UInt32, SA_SAFETY);
		case kAudioObjectPropertyOwnedObjects: PUT_IDS(1, kObjStream);
		case kAudioDevicePropertyStreams: PUT_IDS(a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1, kObjStream);
		case kAudioObjectPropertyControlList: PUT_IDS(0, 0);
		case kAudioDevicePropertyNominalSampleRate: PUT(Float64, (Float64)SWAU_RATE);
		case kAudioDevicePropertyAvailableNominalSampleRates: PUT(AudioValueRange, ((AudioValueRange){ SWAU_RATE, SWAU_RATE }));
		case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
		case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, SA_PERIOD);
		case kAudioDevicePropertyClockIsStable: PUT(UInt32, 1);
		case kAudioDevicePropertyPreferredChannelsForStereo: {
			if (out) {
				if (cap < 2 * sizeof(UInt32))
					return kAudioHardwareBadPropertySizeError;
				((UInt32 *)out)[0] = 1;
				((UInt32 *)out)[1] = 2;
			}
			*size = 2 * sizeof(UInt32);
			return kAudioHardwareNoError;
		}
		}
		break;
	case kObjStream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
		case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR("Output"));
		case kAudioObjectPropertyOwnedObjects: PUT_IDS(0, 0);
		case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
		case kAudioStreamPropertyDirection: PUT(UInt32, 0);   /* 0 = output */
		case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeSpeaker);
		case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
		case kAudioStreamPropertyLatency: PUT(UInt32, 0);
		case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, f);
		case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
			PUT(AudioStreamRangedDescription, ((AudioStreamRangedDescription){ f, { SWAU_RATE, SWAU_RATE } }));
		}
		break;
	}
	return kAudioHardwareUnknownPropertyError;
#undef PUT
#undef PUT_IDS
}

static OSStatus
sa_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
    const void *qd, UInt32 *outSize)
{
	(void)pid;
	if (d != sa_ref || a == NULL || outSize == NULL)
		return kAudioHardwareIllegalOperationError;
	return sa_get(obj, a, qs, qd, 0, outSize, NULL);
}

static OSStatus
sa_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
    const void *qd, UInt32 cap, UInt32 *outSize, void *out)
{
	(void)pid;
	if (d != sa_ref || a == NULL || outSize == NULL || out == NULL)
		return kAudioHardwareIllegalOperationError;
	return sa_get(obj, a, qs, qd, cap, outSize, out);
}

static OSStatus
sa_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
    const void *qd, UInt32 size, const void *data)
{
	(void)pid; (void)qs; (void)qd;
	if (d != sa_ref || a == NULL || data == NULL)
		return kAudioHardwareIllegalOperationError;
	if (!sa_has(obj, a))
		return kAudioHardwareUnknownPropertyError;
	if (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
		if (size < sizeof(Float64))
			return kAudioHardwareBadPropertySizeError;
		return *(const Float64 *)data == (Float64)SWAU_RATE ? kAudioHardwareNoError : kAudioDeviceUnsupportedFormatError;
	}
	if (obj == kObjStream && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
		if (size < sizeof(AudioStreamBasicDescription))
			return kAudioHardwareBadPropertySizeError;
		AudioStreamBasicDescription f = sa_format();
		const AudioStreamBasicDescription *g = data;
		return (g->mSampleRate == f.mSampleRate && g->mFormatID == f.mFormatID && g->mChannelsPerFrame == f.mChannelsPerFrame &&
		    g->mBitsPerChannel == f.mBitsPerChannel && (g->mFormatFlags & kAudioFormatFlagIsFloat)) ? kAudioHardwareNoError
		    : kAudioDeviceUnsupportedFormatError;
	}
	return kAudioHardwareUnsupportedOperationError;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* IO */

static OSStatus
sa_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	(void)client;
	if (d != sa_ref || dev != kObjDevice)
		return kAudioHardwareBadObjectError;
	pthread_mutex_lock(&sa_lock);
	if (sa_running++ == 0) {
		sa_clk.anchor = mach_absolute_time();
		sa_clk.count = 0;
		sa_dropped = sa_written = sa_ioerr = 0;
		sa_fd = open(SWAU_DEV_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (sa_fd < 0) {
			sa_log("start: open %s failed, errno %d: running as a silent sink", SWAU_DEV_PATH, errno);
		} else {
			struct swau_info in;
			memset(&in, 0, sizeof in);
			if (ioctl(sa_fd, SWAU_IOC_INFO, &in) != 0 || in.version != SWAU_VERSION || in.rate != SWAU_RATE || in.channels != SWAU_CHANNELS ||
			    in.bits != 16) {
				sa_log("start: %s info mismatch (errno %d, v %u rate %u ch %u bits %u): closed, silent sink", SWAU_DEV_PATH, errno,
				    in.version, in.rate, in.channels, in.bits);
				close(sa_fd);
				sa_fd = -1;
			} else {
				sa_log("start: %s open, fifo %u frames, flags 0x%x, max gain %d mdB", SWAU_DEV_PATH, in.fifo_frames, in.flags, in.max_gain_mdb);
			}
		}
	}
	pthread_mutex_unlock(&sa_lock);
	return kAudioHardwareNoError;
}

static OSStatus
sa_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	(void)client;
	if (d != sa_ref || dev != kObjDevice)
		return kAudioHardwareBadObjectError;
	pthread_mutex_lock(&sa_lock);
	if (sa_running == 0) {
		pthread_mutex_unlock(&sa_lock);
		return kAudioHardwareNotRunningError;
	}
	if (--sa_running == 0) {
		sa_log("stop: %llu frames written, %llu dropped, %llu write errors", (unsigned long long)sa_written,
		    (unsigned long long)sa_dropped, (unsigned long long)sa_ioerr);
		if (sa_fd >= 0) {
			close(sa_fd);
			sa_fd = -1;
		}
	}
	pthread_mutex_unlock(&sa_lock);
	return kAudioHardwareNoError;
}

static OSStatus
sa_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *sample, UInt64 *host, UInt64 *seed)
{
	(void)client;
	if (d != sa_ref || dev != kObjDevice)
		return kAudioHardwareBadObjectError;
	double s;
	uint64_t h;
	pthread_mutex_lock(&sa_lock);
	sa_zero_ts(&sa_clk, mach_absolute_time(), &s, &h);
	pthread_mutex_unlock(&sa_lock);
	*sample = s;
	*host = h;
	*seed = 1;
	return kAudioHardwareNoError;
}

static OSStatus
sa_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace)
{
	(void)client;
	if (d != sa_ref || dev != kObjDevice)
		return kAudioHardwareBadObjectError;
	*will = op == kAudioServerPlugInIOOperationWriteMix;
	*inPlace = true;
	return kAudioHardwareNoError;
}

static OSStatus
sa_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci)
{
	(void)client; (void)op; (void)n; (void)ci;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus
sa_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
    const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary)
{
	(void)client; (void)ci; (void)secondary;
	if (d != sa_ref || dev != kObjDevice || stream != kObjStream)
		return kAudioHardwareBadObjectError;
	if (op != kAudioServerPlugInIOOperationWriteMix || main == NULL)
		return kAudioHardwareNoError;
	/* IO thread: no lock (the fd only changes in StartIO/StopIO, which the HAL does not run concurrently with IO), no logging */
	const float *in = main;
	while (frames > 0) {
		uint32_t n = frames > SA_MAX_FRAMES ? SA_MAX_FRAMES : frames;
		if (sa_fd < 0) {
			sa_dropped += n;
		} else {
			sa_convert(in, sa_buf, n * SWAU_CHANNELS);
			ssize_t rc = write(sa_fd, sa_buf, (size_t)n * SWAU_BYTES_PER_FRAME);
			uint32_t took = sa_frames_taken(rc, n);
			if (rc < 0)
				sa_ioerr++;
			sa_written += took;
			sa_dropped += n - took;
		}
		in += n * SWAU_CHANNELS;
		frames -= n;
	}
	return kAudioHardwareNoError;
}

static OSStatus
sa_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci)
{
	(void)client; (void)op; (void)n; (void)ci;
	return (d == sa_ref && dev == kObjDevice) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static AudioServerPlugInDriverInterface sa_iface = {
	NULL, sa_QueryInterface, sa_AddRef, sa_Release, sa_Initialize, sa_CreateDevice, sa_DestroyDevice, sa_AddDeviceClient,
	sa_RemoveDeviceClient, sa_PerformConfigChange, sa_AbortConfigChange, sa_HasProperty, sa_IsPropertySettable,
	sa_GetPropertyDataSize, sa_GetPropertyData, sa_SetPropertyData, sa_StartIO, sa_StopIO, sa_GetZeroTimeStamp,
	sa_WillDoIOOperation, sa_BeginIOOperation, sa_DoIOOperation, sa_EndIOOperation,
};

/* CFPlugInFactories in Info.plist names this symbol. */
__attribute__((visibility("default"))) void *SweetAudio_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *
SweetAudio_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
	(void)alloc;
	if (!CFEqual(type, kAudioServerPlugInTypeUUID))
		return NULL;
	sa_log("factory: created (pid %d)", getpid());
	return sa_ref;
}
