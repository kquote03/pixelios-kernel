/*
 * sweetaudio_vol (doc 56 Steps 8-10): the volume of the libsalog tap. VirtualAudio's mix reaches the tap at unity at any iOS volume (run
 * ui-diag1) and its Null_Device route has no volume control (run vol1), so iOS itself never makes the volume keys audible. This agent:
 *  - watches the HID event stream (monitor client) for consumer-page Volume Increment / Decrement key-downs: the physical keys and the
 *    patch 0398 test presses both come from the same kernel service (SweetHIDButtons::key -> dispatchEvent, patch 0087), run vol1 saw the
 *    physical ones reach backboardd;
 *  - steps a LEVEL 0..16 by one per press, 3 dB per level (sa_core.h sa_vol_level_v; Step 10: the Step 9 1/16 linear step was ~1 dB, not
 *    audible), level 0 = mute, and publishes it as the darwin notify state SA_VOL_NOTIFY that libsalog reads per flush (every sound through
 *    mediaserverd follows it);
 *  - keeps the level across boots in LEVEL_FILE (one line "level N"), logs every press with the new level and dB;
 *  - follows AVSystemController's active-category volume when IT moves (e.g. a Settings slider) and sets it best effort after a key press.
 * The HID client starts first, before AVSystemController is looked up (Step 10: Step 9 waited up to 120 s for it before listening).
 * Launchd job org.pixelios.sweetaudio-vol (KeepAlive), runs as root; nothing in it depends on USB.
 *
 * AVSystemController is private (MediaExperience / Celestial): looked up at run time; the methods used [guess from class-dump notes]:
 * +sharedAVSystemController, -getActiveCategoryVolume:andName:, -setActiveCategoryVolumeTo:, -setAttribute:forKey:error: with
 * AVSystemController_SubscribeToNotificationsAttribute. Missing pieces = a log line; the keys work without it.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sa_core.h"

#define SEL_(s) sel_registerName(s)
#define LEVEL_FILE "/private/var/root/org.pixelios.sweetaudio.level"
static id avsc;
static int tok = -1;
static uint64_t last = UINT64_MAX;
static int level = SA_VOL_LEVELS;   /* Step 10: the key level, 16 = 0 dB */
static int avsc_tries;

static id nsstr(const char *c)
{
	return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), SEL_("stringWithUTF8String:"), c);
}

static bool responds(id o, const char *s)
{
	return o && ((bool (*)(id, SEL, SEL))objc_msgSend)(o, SEL_("respondsToSelector:"), SEL_(s));
}

static void publish_level(const char *why)
{
	float v = sa_vol_level_v[level];
	uint64_t st = sa_vol_to_state(v);
	if (st == last)
		return;
	last = st;
	notify_set_state(tok, st);
	notify_post(SA_VOL_NOTIFY);
	if (level == 0)
		printf("sweetaudio_vol: %s: level 0/%d (mute) -> state %llu\n", why, SA_VOL_LEVELS, (unsigned long long)st);
	else
		printf("sweetaudio_vol: %s: level %d/%d (%d dB) -> state %llu (tap gain %.4f)\n", why, level, SA_VOL_LEVELS,
		    -SA_VOL_STEP_DB * (SA_VOL_LEVELS - level), (unsigned long long)st, sa_vol_from_state(st));
	fflush(stdout);
}

static void level_save(void)
{
	char b[32];
	int n = snprintf(b, sizeof b, "level %d\n", level);
	int fd = open(LEVEL_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, b, (size_t)n) != n || fsync(fd) != 0 || close(fd) != 0 || rename(LEVEL_FILE ".tmp", LEVEL_FILE) != 0) {
		if (fd >= 0)
			close(fd);
		printf("sweetaudio_vol: GAP level not saved (%s)\n", strerror(errno));
		fflush(stdout);
	}
}

static void level_load(void)
{
	char b[32] = { 0 };
	int n = -1, fd = open(LEVEL_FILE, O_RDONLY);
	if (fd >= 0) {
		ssize_t r = read(fd, b, sizeof b - 1);
		close(fd);
		if (r > 0 && sscanf(b, "level %d", &n) == 1 && n >= 0 && n <= SA_VOL_LEVELS) {
			level = n;
			printf("sweetaudio_vol: level %d restored from %s\n", level, LEVEL_FILE);
			return;
		}
	}
	printf("sweetaudio_vol: no saved level (%s), level %d\n", fd < 0 ? "no file" : "unreadable", level);
}

static void avsc_poll(const char *why)
{
	float v = -1;
	id name = nil;
	static uint64_t last_avsc = UINT64_MAX;      /* follow AVSystemController only when ITS value moves; the first read is the baseline */
	if (!avsc || !responds(avsc, "getActiveCategoryVolume:andName:"))
		return;
	if (!((bool (*)(id, SEL, float *, id *))objc_msgSend)(avsc, SEL_("getActiveCategoryVolume:andName:"), &v, &name) || v < 0)
		return;
	uint64_t st = sa_vol_to_state(v);
	if (st == last_avsc)
		return;
	bool first = last_avsc == UINT64_MAX;
	last_avsc = st;
	if (first)
		return;
	int n = sa_vol_level_of(v);
	if (n == level)
		return;
	level = n;
	level_save();
	publish_level(why);
}

static void on_hid(void *target, void *refcon, void *sender, CFTypeRef ev)
{
	static uint32_t (*gtype)(CFTypeRef);
	static long (*gint)(CFTypeRef, uint32_t);
	(void)target; (void)refcon; (void)sender;
	if (!gtype) {
		gtype = (uint32_t (*)(CFTypeRef))dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
		gint = (long (*)(CFTypeRef, uint32_t))dlsym(RTLD_DEFAULT, "IOHIDEventGetIntegerValue");
		if (!gtype || !gint)
			return;
	}
	/* IOHIDEvent constants from IOHIDFamily (open source) IOHIDEventTypes.h: kIOHIDEventTypeKeyboard 3, field = type << 16 | n, keyboard
	 * fields UsagePage 0, Usage 1, Down 2; consumer page 0x0C, Volume Increment 0xE9, Decrement 0xEA [read from source; run vol2] */
	if (!ev || gtype(ev) != 3u || gint(ev, (3u << 16) | 0u) != 0x0C || !gint(ev, (3u << 16) | 2u))
		return;
	long u = gint(ev, (3u << 16) | 1u);
	if (u != 0xE9 && u != 0xEA)
		return;
	level = sa_vol_level_clamp(level + (u == 0xE9 ? 1 : -1));
	last = UINT64_MAX;      /* log every press, also at the ends */
	publish_level(u == 0xE9 ? "key volume-up" : "key volume-down");
	level_save();
	if (avsc && responds(avsc, "setActiveCategoryVolumeTo:"))
		((bool (*)(id, SEL, float))objc_msgSend)(avsc, SEL_("setActiveCategoryVolumeTo:"), sa_vol_level_v[level]);
}

static void hid_start(void)
{
	void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	CFTypeRef (*mk)(CFAllocatorRef, int, CFDictionaryRef) = h ? dlsym(h, "IOHIDEventSystemClientCreateWithType") : NULL;
	void (*sched)(CFTypeRef, CFRunLoopRef, CFStringRef) = h ? dlsym(h, "IOHIDEventSystemClientScheduleWithRunLoop") : NULL;
	void (*reg)(CFTypeRef, void (*)(void *, void *, void *, CFTypeRef), void *, void *) =
	    h ? dlsym(h, "IOHIDEventSystemClientRegisterEventCallback") : NULL;
	CFTypeRef c = mk ? mk(kCFAllocatorDefault, 1, NULL) : NULL;      /* kIOHIDEventSystemClientTypeMonitor 1 */
	if (!c || !sched || !reg) {
		printf("sweetaudio_vol: GAP hid monitor client not available (create %p sched %p reg %p): volume keys not followed\n", (void *)c, (void *)sched, (void *)reg);
		return;
	}
	reg(c, on_hid, NULL, NULL);
	sched(c, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	printf("sweetaudio_vol: hid monitor client on (volume keys: %d levels of %d dB, level 0 = mute)\n", SA_VOL_LEVELS, SA_VOL_STEP_DB);
}

static void on_note(CFNotificationCenterRef c, void *obs, CFNotificationName name, const void *obj, CFDictionaryRef info)
{
	(void)c; (void)obs; (void)name; (void)obj; (void)info;
	avsc_poll("notification");
}

static void avsc_attach(void)
{
	id cls = (id)objc_getClass("AVSystemController");
	avsc = cls ? ((id (*)(id, SEL))objc_msgSend)(cls, SEL_("sharedAVSystemController")) : nil;
	if (!avsc) {
		if (++avsc_tries == 120)
			printf("sweetaudio_vol: GAP no AVSystemController after 120 s (keys still work, Settings slider not followed)\n");
		return;
	}
	/* the framework's own constant strings (their values need not equal the symbol names; ui-diag2: the literal names gave ok 0 and no update) */
	id *k_sub = dlsym(RTLD_DEFAULT, "AVSystemController_SubscribeToNotificationsAttribute");
	id *k_note = dlsym(RTLD_DEFAULT, "AVSystemController_SystemVolumeDidChangeNotification");
	id sub = k_sub ? *k_sub : nsstr("AVSystemController_SubscribeToNotificationsAttribute");
	id note = k_note ? *k_note : nsstr("AVSystemController_SystemVolumeDidChangeNotification");
	if (responds(avsc, "setAttribute:forKey:error:")) {
		id arr = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSArray"), SEL_("arrayWithObject:"), note);
		bool ok = ((bool (*)(id, SEL, id, id, id *))objc_msgSend)(avsc, SEL_("setAttribute:forKey:error:"), arr, sub, NULL);
		printf("sweetaudio_vol: AVSystemController after %d s, subscribe volume notifications ok %d (constants %s/%s)\n", avsc_tries, ok,
		    k_sub ? "dlsym" : "literal", k_note ? "dlsym" : "literal");
	}
	CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, on_note, (CFStringRef)note, NULL,
	    CFNotificationSuspensionBehaviorDeliverImmediately);
	avsc_poll("start");
	fflush(stdout);
}

static void on_timer(CFRunLoopTimerRef t, void *info)
{
	(void)t; (void)info;
	if (!avsc)
		avsc_attach();
	else
		avsc_poll("poll");
}

int main(void)
{
	if (notify_register_check(SA_VOL_NOTIFY, &tok) != NOTIFY_STATUS_OK) {
		printf("sweetaudio_vol: FAIL notify_register_check\n");
		return 1;
	}
	level_load();
	publish_level("start");
	hid_start();
	dlopen("/System/Library/PrivateFrameworks/MediaExperience.framework/MediaExperience", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_NOW);
	CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1.0, 0, 0, on_timer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
	fflush(stdout);
	CFRunLoopRun();
	return 0;
}
