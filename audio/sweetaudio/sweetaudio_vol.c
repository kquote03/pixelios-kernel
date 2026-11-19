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
	if (st == last)
		return;
	last = st;
	notify_set_state(tok, st);
	notify_post(SA_VOL_NOTIFY);
	const char *n = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, SEL_("UTF8String")) : "?";
	printf("sweetaudio_vol: %s: category %s volume %.3f -> state %llu (tap gain %.4f)\n", why, n, v, (unsigned long long)st, sa_vol_from_state(st));
	fflush(stdout);
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
	publish("start");
	CFRunLoopRun();
	return 0;
}
