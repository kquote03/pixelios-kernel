/*
 * sweetaudio_vol (doc 56 Step 8): publishes the iOS system volume for the libsalog tap. VirtualAudio's mix reaches the tap at unity at any
 * volume (run ui-diag1), so the volume HUD / volume keys changed nothing audible. This agent reads AVSystemController's active-category
 * volume (the value the volume HUD shows) and sets the darwin notify state SA_VOL_NOTIFY (sa_core.h) whenever it changes; libsalog reads
 * it per flush. Read-only towards iOS (it never sets a volume). Launchd job org.pixelios.sweetaudio-vol (KeepAlive), runs as root.
 *
 * AVSystemController is private (MediaExperience / Celestial): looked up at run time; the methods used [guess from class-dump notes]:
 * +sharedAVSystemController, -getActiveCategoryVolume:andName:, -setAttribute:forKey:error: with
 * AVSystemController_SubscribeToNotificationsAttribute (so the volume-change notification is posted). Missing pieces = a log line and the
 * 1 s poll alone.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "sa_core.h"

#define SEL_(s) sel_registerName(s)
static id avsc;
static int tok = -1;
static uint64_t last = UINT64_MAX;
static float cur_v = -1;           /* Step 9: the volume the keys step from */

static id nsstr(const char *c)
{
	return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), SEL_("stringWithUTF8String:"), c);
}

static bool responds(id o, const char *s)
{
	return o && ((bool (*)(id, SEL, SEL))objc_msgSend)(o, SEL_("respondsToSelector:"), SEL_(s));
}

static void publish(const char *why)
{
	float v = -1;
	id name = nil;
	if (!avsc || !responds(avsc, "getActiveCategoryVolume:andName:"))
		return;
	if (!((bool (*)(id, SEL, float *, id *))objc_msgSend)(avsc, SEL_("getActiveCategoryVolume:andName:"), &v, &name) || v < 0)
		return;
	uint64_t st = sa_vol_to_state(v);
	static uint64_t last_avsc = UINT64_MAX;      /* Step 9: follow AVSystemController only when ITS value moves (a key step is not undone) */
	if (st == last_avsc)
		return;
	last_avsc = st;
	cur_v = v;
	if (st == last)
		return;
	last = st;
	notify_set_state(tok, st);
	notify_post(SA_VOL_NOTIFY);
	const char *n = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, SEL_("UTF8String")) : "?";
	printf("sweetaudio_vol: %s: category %s volume %.3f -> state %llu (tap gain %.4f)\n", why, n, v, (unsigned long long)st, sa_vol_from_state(st));
	fflush(stdout);
}

/* Step 9 (doc 56, run vol1): the physical keys reach backboardd (SWEET KEYS key=1/2, SWEET HID 0xe9/0xea) but no volume change reaches
 * mediaserverd: VirtualAudio's Null_Device route has no volume control ("VolumeProperties for Port: pnul is [ ]"). So this agent also
 * watches the HID event stream (monitor client) for consumer-page Volume Increment / Decrement key-downs and steps its own volume by 1/16
 * (the iOS step), sets it on AVSystemController (best effort, so iOS reads the same value) and publishes it for the tap at once.
 * IOHIDEvent constants from IOHIDFamily (open source) IOHIDEventTypes.h: kIOHIDEventTypeKeyboard 3, field base type << 16, keyboard fields
 * UsagePage +0, Usage +1, Down +2; kIOHIDEventSystemClientTypeMonitor 1. [read from source; checked on the phone by run vol2] */
#define HID_EV_KEYBOARD 3u
#define HID_F_KB(n) ((HID_EV_KEYBOARD << 16) | (n))


static void publish_v(float v, const char *why)
{
	uint64_t st = sa_vol_to_state(v);
	if (st == last)
		return;
	last = st;
	notify_set_state(tok, st);
	notify_post(SA_VOL_NOTIFY);
	printf("sweetaudio_vol: %s: volume %.3f -> state %llu (tap gain %.4f)\n", why, v, (unsigned long long)st, sa_vol_from_state(st));
	fflush(stdout);
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
	if (!ev || gtype(ev) != HID_EV_KEYBOARD || gint(ev, HID_F_KB(0)) != 0x0C || !gint(ev, HID_F_KB(2)))
		return;
	long u = gint(ev, HID_F_KB(1));
	if (u != 0xE9 && u != 0xEA)
		return;
	if (cur_v < 0)
		cur_v = 1.0f;
	float v = cur_v + (u == 0xE9 ? 1.0f : -1.0f) / 16.0f;
	v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	cur_v = v;
	if (avsc && responds(avsc, "setActiveCategoryVolumeTo:"))
		((bool (*)(id, SEL, float))objc_msgSend)(avsc, SEL_("setActiveCategoryVolumeTo:"), v);
	publish_v(v, u == 0xE9 ? "key volume-up" : "key volume-down");
}

static void hid_start(void)
{
	void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	CFTypeRef (*mk)(CFAllocatorRef, int, CFDictionaryRef) = h ? dlsym(h, "IOHIDEventSystemClientCreateWithType") : NULL;
	void (*sched)(CFTypeRef, CFRunLoopRef, CFStringRef) = h ? dlsym(h, "IOHIDEventSystemClientScheduleWithRunLoop") : NULL;
	void (*reg)(CFTypeRef, void (*)(void *, void *, void *, CFTypeRef), void *, void *) =
	    h ? dlsym(h, "IOHIDEventSystemClientRegisterEventCallback") : NULL;
	CFTypeRef c = mk ? mk(kCFAllocatorDefault, 1, NULL) : NULL;
	if (!c || !sched || !reg) {
		printf("sweetaudio_vol: GAP hid monitor client not available (create %p sched %p reg %p): volume keys not followed\n", (void *)c, (void *)sched, (void *)reg);
		return;
	}
	reg(c, on_hid, NULL, NULL);
	sched(c, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	printf("sweetaudio_vol: hid monitor client on (volume keys step 1/16)\n");
}

static void on_note(CFNotificationCenterRef c, void *obs, CFNotificationName name, const void *obj, CFDictionaryRef info)
{
	(void)c; (void)obs; (void)name; (void)obj; (void)info;
	publish("notification");
}

static void on_timer(CFRunLoopTimerRef t, void *info)
{
	(void)t; (void)info;
	publish("poll");
}

int main(void)
{
	if (notify_register_check(SA_VOL_NOTIFY, &tok) != NOTIFY_STATUS_OK) {
		printf("sweetaudio_vol: FAIL notify_register_check\n");
		return 1;
	}
	dlopen("/System/Library/PrivateFrameworks/MediaExperience.framework/MediaExperience", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_NOW);
	for (int i = 0; i < 120 && !avsc; i++) {     /* mediaserverd may still be starting: <= 120 s */
		id cls = (id)objc_getClass("AVSystemController");
		avsc = cls ? ((id (*)(id, SEL))objc_msgSend)(cls, SEL_("sharedAVSystemController")) : nil;
		if (!avsc)
			sleep(1);
	}
	if (!avsc) {
		printf("sweetaudio_vol: FAIL no AVSystemController\n");
		return 1;
	}
	/* the framework's own constant strings (their values need not equal the symbol names; ui-diag2: the literal names gave ok 0 and no update) */
	id *k_sub = dlsym(RTLD_DEFAULT, "AVSystemController_SubscribeToNotificationsAttribute");
	id *k_note = dlsym(RTLD_DEFAULT, "AVSystemController_SystemVolumeDidChangeNotification");
	id sub = k_sub ? *k_sub : nsstr("AVSystemController_SubscribeToNotificationsAttribute");
	id note = k_note ? *k_note : nsstr("AVSystemController_SystemVolumeDidChangeNotification");
	if (responds(avsc, "setAttribute:forKey:error:")) {
		id arr = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSArray"), SEL_("arrayWithObject:"), note);
		bool ok = ((bool (*)(id, SEL, id, id, id *))objc_msgSend)(avsc, SEL_("setAttribute:forKey:error:"), arr, sub, NULL);
		printf("sweetaudio_vol: subscribe volume notifications ok %d (constants %s/%s)\n", ok, k_sub ? "dlsym" : "literal", k_note ? "dlsym" : "literal");
	}
	CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, on_note, (CFStringRef)note, NULL,
	    CFNotificationSuspensionBehaviorDeliverImmediately);
	CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1.0, 0, 0, on_timer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
	hid_start();
	publish("start");
	CFRunLoopRun();
	return 0;
}
