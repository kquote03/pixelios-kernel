/*
 * pixui_probe: read-only-first probe of the iOS state that the Settings Wi-Fi / Bluetooth switches, the volume keys and system
 * sounds depend on (doc 56 "Step 8"). One shot, every wait bounded, counts and states only (no SSID, address or name printed).
 *
 *   pixui_probe [all|mg|wifi|bt|vol|snd|seq] (default all; launchd job, stdout mirrored by xnu sweetnull=pixui_probe; seq: PIXUI_SEQ, Step 9)
 *
 * Every private symbol is looked up with dlsym/objc_getClass: a missing one is a "PIXUI GAP:" line, never a crash.
 * Private prototypes are [guess] (written from memory of class-dump / public reverse-engineering notes, not checked against
 * this root); a wrong guess shows up as a GAP or an implausible value, which the run must read as "unknown", not as a result.
 * The only state it changes: Wi-Fi power on / BT power on when they read off (what the Settings switch does), the system
 * volume (set, then restored to the value read at start), one dispatched volume-up key (then volume restored).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioServices.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static unsigned gaps;
static time_t t_start;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("PIXUI: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	fflush(stdout);
}

static void gap(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("PIXUI GAP: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	fflush(stdout);
	gaps++;
}

static void *lib(const char *p)
{
	void *h = dlopen(p, RTLD_NOW);
	if (!h)
		gap("dlopen %s failed", p);
	return h;
}

static void *sym(void *h, const char *n)
{
	void *s = h ? dlsym(h, n) : NULL;
	if (!s)
		gap("symbol %s missing", n);
	return s;
}

static void run_loop(double s)
{
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, s, false);
}

/* ---- objc helpers ---- */
typedef id (*msg0_t)(id, SEL);
typedef id (*msg1_t)(id, SEL, id);
#define SEL_(s) sel_registerName(s)
static id M0(id o, const char *s) { return o ? ((msg0_t)objc_msgSend)(o, SEL_(s)) : nil; }
static bool MB(id o, const char *s) { return o ? ((bool (*)(id, SEL))objc_msgSend)(o, SEL_(s)) : false; }
static bool responds(id o, const char *s) { return o && ((bool (*)(id, SEL, SEL))objc_msgSend)(o, SEL_("respondsToSelector:"), SEL_(s)); }
static id nsstr(const char *c) { return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), SEL_("stringWithUTF8String:"), c); }
static const char *cstr(id s) { return s ? ((const char *(*)(id, SEL))objc_msgSend)(s, SEL_("UTF8String")) : "(nil)"; }

/* ---- 1. MobileGestalt ---- */
#define MGCACHE "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist"
static int mg_wifi = -1, mg_bt = -1, mg_vol = -1;

static void probe_mg(void)
{
	struct stat st;
	if (stat(MGCACHE, &st) == 0)
		say("mg: cache plist size %lld mtime %ld (now %ld)", (long long)st.st_size, (long)st.st_mtime, (long)time(NULL));
	else
		say("mg: cache plist absent");
	void *h = lib("/usr/lib/libMobileGestalt.dylib");
	bool (*getb)(CFStringRef) = sym(h, "MGGetBoolAnswer");
	CFTypeRef (*copy)(CFStringRef) = sym(h, "MGCopyAnswer");
	static const char *keys[] = {"wifi", "bluetooth", "bluetooth-le", "WiFiCapability", "volume-buttons", "ringer-switch",
				     "telephony", "wapi", "HasBaseband", "DeviceSupportsWiFi"};
	if (getb) {
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
			int v = getb(k) ? 1 : 0;
			CFRelease(k);
			say("mg: bool %s = %d", keys[i], v);
			if (!strcmp(keys[i], "wifi"))
				mg_wifi = v;
			if (!strcmp(keys[i], "bluetooth"))
				mg_bt = v;
			if (!strcmp(keys[i], "volume-buttons"))
				mg_vol = v;
		}
	}
	static const char *skeys[] = {"ProductType", "DeviceClass", "WifiChipset", "BluetoothAddress", "WifiAddress"};
	if (copy) {
		for (unsigned i = 0; i < sizeof(skeys) / sizeof(skeys[0]); i++) {
			CFStringRef k = CFStringCreateWithCString(NULL, skeys[i], kCFStringEncodingUTF8);
			CFTypeRef v = copy(k);
			CFRelease(k);
			char buf[64] = "(null)";
			if (v && CFGetTypeID(v) == CFStringGetTypeID())
				CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
			if (strstr(skeys[i], "Address"))	/* never print an address: presence only */
				say("mg: %s %s", skeys[i], v ? "present" : "absent");
			else
				say("mg: %s = %s", skeys[i], buf);
			if (v)
				CFRelease(v);
		}
	}
}

/* ---- 2. Wi-Fi (MobileWiFi -> wifid) ---- */
typedef const void *WiFiRef;
static int scan_n = -1, scan_err = 0;
static void scan_cb(WiFiRef dev, CFArrayRef res, int err, void *ctx)
{
	(void)dev;
	(void)ctx;
	scan_n = res ? (int)CFArrayGetCount(res) : 0;
	scan_err = err;
}

static void probe_wifi(void)
{
	void *h = lib("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi");
	WiFiRef (*create)(CFAllocatorRef, int) = sym(h, "WiFiManagerClientCreate");
	CFArrayRef (*devs)(WiFiRef) = sym(h, "WiFiManagerClientCopyDevices");
	void (*sched)(WiFiRef, CFRunLoopRef, CFStringRef) = sym(h, "WiFiManagerClientScheduleWithRunLoop");
	bool (*dpow)(WiFiRef) = sym(h, "WiFiDeviceClientGetPower");
	CFTypeRef (*cur)(WiFiRef) = sym(h, "WiFiDeviceClientCopyCurrentNetwork");
	int (*scan)(WiFiRef, CFDictionaryRef, void (*)(WiFiRef, CFArrayRef, int, void *), void *) = sym(h, "WiFiDeviceClientScanAsync");
	CFTypeRef (*mprop)(WiFiRef, CFStringRef) = sym(h, "WiFiManagerClientCopyProperty");
	void (*msetpow)(WiFiRef, bool) = sym(h, "WiFiManagerClientSetPower");
	CFTypeRef (*dprop)(WiFiRef, CFStringRef) = dlsym(h, "WiFiDeviceClientCopyProperty");
	if (!create || !devs)
		return;
	WiFiRef m = create(kCFAllocatorDefault, 0);
	if (!m) {
		gap("wifi: WiFiManagerClientCreate returned NULL");
		return;
	}
	if (sched)
		sched(m, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	run_loop(1.0);
	static const char *props[] = {"AllowEnable", "Power", "WiFiPower", "Enabled", "IsWiFiRestricted"};
	for (unsigned i = 0; mprop && i < sizeof(props) / sizeof(props[0]); i++) {
		CFStringRef k = CFStringCreateWithCString(NULL, props[i], kCFStringEncodingUTF8);
		CFTypeRef v = mprop(m, k);
		CFRelease(k);
		if (!v)
			say("wifi: manager property %s = (null)", props[i]);
		else if (CFGetTypeID(v) == CFBooleanGetTypeID())
			say("wifi: manager property %s = %d", props[i], CFBooleanGetValue(v) ? 1 : 0);
		else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
			long long n = 0;
			CFNumberGetValue(v, kCFNumberLongLongType, &n);
			say("wifi: manager property %s = %lld", props[i], n);
		} else
			say("wifi: manager property %s = (type %lu)", props[i], (unsigned long)CFGetTypeID(v));
		if (v)
			CFRelease(v);
	}
	CFArrayRef a = NULL;
	for (int i = 0; i < 20 && (!a || CFArrayGetCount(a) == 0); i++) {
		if (a)
			CFRelease(a);
		a = devs(m);
		if (!a || CFArrayGetCount(a) == 0)
			run_loop(1.0);
	}
	int n = a ? (int)CFArrayGetCount(a) : 0;
	say("wifi: devices %d", n);
	if (n == 0) {
		gap("wifi: wifid reports no device to a MobileWiFi client (Settings would show no Wi-Fi)");
		return;
	}
	WiFiRef d = CFArrayGetValueAtIndex(a, 0);
	if (dprop) {
		CFStringRef k = CFSTR("Interface");
		CFTypeRef v = dprop(d, k);
		char buf[16] = "?";
		if (v && CFGetTypeID(v) == CFStringGetTypeID())
			CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
		say("wifi: device interface %s", buf);
		if (v)
			CFRelease(v);
	}
	int p = dpow ? dpow(d) : -1;
	say("wifi: device power %d", p);
	if (p == 0 && msetpow) {
		say("wifi: power reads off: WiFiManagerClientSetPower(1) (the Settings switch)");
		msetpow(m, true);
		for (int i = 0; i < 15 && dpow && !dpow(d); i++)
			run_loop(1.0);
		say("wifi: device power after set %d", dpow ? dpow(d) : -1);
	}
	CFTypeRef c = cur ? cur(d) : NULL;
	say("wifi: current network %s", c ? "present (joined)" : "none");
	if (c)
		CFRelease(c);
	if (scan) {
		CFDictionaryRef opt = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		int r = scan(d, opt, scan_cb, NULL);
		say("wifi: scan request rc %d", r);
		for (int i = 0; i < 30 && scan_n < 0; i++)
			run_loop(1.0);
		say("wifi: scan results %d err %d%s", scan_n, scan_err, scan_n < 0 ? " (no callback in 30 s)" : "");
		CFRelease(opt);
	}
	CFRelease(a);
}

/* ---- 3. Bluetooth (BluetoothManager, the Settings path) ---- */
static void probe_bt(void)
{
	lib("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager");
	id cls = (id)objc_getClass("BluetoothManager");
	if (!cls) {
		gap("bt: class BluetoothManager missing");
		return;
	}
	id m = M0(cls, "sharedInstance");
	if (!m) {
		gap("bt: sharedInstance nil (lastInitError %ld)", responds(cls, "lastInitError") ? (long)((long (*)(id, SEL))objc_msgSend)(cls, SEL_("lastInitError")) : -1L);
		return;
	}
	int i;
	for (i = 0; i < 20 && !MB(m, "available"); i++)
		run_loop(1.0);
	say("bt: available %d after %d s, powered %d, enabled %d", MB(m, "available"), i, MB(m, "powered"), responds(m, "enabled") ? MB(m, "enabled") : -1);
	if (!MB(m, "powered")) {
		say("bt: setPowered:YES + setEnabled:YES (the Settings switch)");
		((void (*)(id, SEL, bool))objc_msgSend)(m, SEL_("setPowered:"), true);
		if (responds(m, "setEnabled:"))
			((void (*)(id, SEL, bool))objc_msgSend)(m, SEL_("setEnabled:"), true);
		for (i = 0; i < 20 && !MB(m, "powered"); i++)
			run_loop(1.0);
		say("bt: after set: powered %d after %d s, enabled %d, available %d", MB(m, "powered"), i, responds(m, "enabled") ? MB(m, "enabled") : -1, MB(m, "available"));
	}
	if (MB(m, "powered") && responds(m, "setDeviceScanningEnabled:")) {
		((void (*)(id, SEL, bool))objc_msgSend)(m, SEL_("setDeviceScanningEnabled:"), true);
		run_loop(8.0);
		say("bt: deviceScanningEnabled %d", MB(m, "deviceScanningEnabled"));
		((void (*)(id, SEL, bool))objc_msgSend)(m, SEL_("setDeviceScanningEnabled:"), false);
	}
}

/* ---- 4. volume (AVSystemController) + HID volume key ---- */
static id avsc(void)
{
	static id s;
	if (s)
		return s;
	dlopen("/System/Library/PrivateFrameworks/MediaExperience.framework/MediaExperience", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_NOW);
	id cls = (id)objc_getClass("AVSystemController");
	if (!cls) {
		gap("vol: class AVSystemController missing");
		return nil;
	}
	s = M0(cls, "sharedAVSystemController");
	if (!s)
		gap("vol: sharedAVSystemController nil");
	return s;
}

static float vol_get(const char *tag)
{
	id s = avsc();
	float v = -1;
	id name = nil;
	if (s && responds(s, "getActiveCategoryVolume:andName:")) {
		bool ok = ((bool (*)(id, SEL, float *, id *))objc_msgSend)(s, SEL_("getActiveCategoryVolume:andName:"), &v, &name);
		say("vol: %s active category %s volume %.3f ok %d", tag, cstr(name), v, ok);
	}
	if (s && responds(s, "getVolume:forCategory:")) {
		float r = -1, a = -1;
		((bool (*)(id, SEL, float *, id))objc_msgSend)(s, SEL_("getVolume:forCategory:"), &r, nsstr("Ringtone"));
		((bool (*)(id, SEL, float *, id))objc_msgSend)(s, SEL_("getVolume:forCategory:"), &a, nsstr("Audio/Video"));
		say("vol: %s Ringtone %.3f Audio/Video %.3f", tag, r, a);
	}
	return v;
}

static void vol_set(float v)
{
	id s = avsc();
	if (s && responds(s, "setActiveCategoryVolumeTo:")) {
		bool ok = ((bool (*)(id, SEL, float))objc_msgSend)(s, SEL_("setActiveCategoryVolumeTo:"), v);
		say("vol: setActiveCategoryVolumeTo %.3f ok %d", v, ok);
	} else
		gap("vol: setActiveCategoryVolumeTo: not available");
}

static int spawn_wait(const char *path, const char *arg, int max_s)
{
	pid_t pid;
	char *av[] = {(char *)path, (char *)arg, NULL};
	if (access(path, X_OK) != 0) {
		gap("spawn: %s not executable", path);
		return -1;
	}
	if (posix_spawn(&pid, path, NULL, NULL, av, environ) != 0) {
		gap("spawn: %s failed", path);
		return -1;
	}
	int st = 0;
	for (int i = 0; i < max_s * 10; i++) {
		if (waitpid(pid, &st, WNOHANG) == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
		usleep(100000);
	}
	kill(pid, 9);
	waitpid(pid, &st, 0);
	gap("spawn: %s %s killed after %d s", path, arg, max_s);
	return -2;
}

static void probe_hid(float v0)
{
	void *h = lib("/System/Library/Frameworks/IOKit.framework/IOKit");
	CFTypeRef (*cli)(CFAllocatorRef) = sym(h, "IOHIDEventSystemClientCreate");
	CFArrayRef (*svcs)(CFTypeRef) = sym(h, "IOHIDEventSystemClientCopyServices");
	CFTypeRef (*sprop)(CFTypeRef, CFStringRef) = sym(h, "IOHIDServiceClientCopyProperty");
	CFTypeRef (*kev)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, bool, uint32_t) = sym(h, "IOHIDEventCreateKeyboardEvent");
	void (*disp)(CFTypeRef, CFTypeRef) = sym(h, "IOHIDEventSystemClientDispatchEvent");
	if (!cli)
		return;
	CFTypeRef c = cli(kCFAllocatorDefault);
	if (!c) {
		gap("hid: IOHIDEventSystemClientCreate NULL");
		return;
	}
	CFArrayRef a = svcs ? svcs(c) : NULL;
	int n = a ? (int)CFArrayGetCount(a) : -1, sweet = 0;
	for (int i = 0; i < n && sprop; i++) {
		CFTypeRef s = CFArrayGetValueAtIndex(a, i);
		CFTypeRef man = sprop(s, CFSTR("Manufacturer")), pu = sprop(s, CFSTR("PrimaryUsagePage")), pr = sprop(s, CFSTR("Product"));
		char mb[32] = "", pb[48] = "";
		long long page = -1;
		if (man && CFGetTypeID(man) == CFStringGetTypeID())
			CFStringGetCString(man, mb, sizeof(mb), kCFStringEncodingUTF8);
		if (pr && CFGetTypeID(pr) == CFStringGetTypeID())
			CFStringGetCString(pr, pb, sizeof(pb), kCFStringEncodingUTF8);
		if (pu && CFGetTypeID(pu) == CFNumberGetTypeID())
			CFNumberGetValue(pu, kCFNumberLongLongType, &page);
		if (!strcmp(mb, "Sweet")) {
			sweet++;
			say("hid: event-system service '%s' page 0x%llx", pb, page);
		}
		if (man)
			CFRelease(man);
		if (pu)
			CFRelease(pu);
		if (pr)
			CFRelease(pr);
	}
	say("hid: event-system services %d (Sweet %d)", n, sweet);
	if (a)
		CFRelease(a);
	if (kev && disp) {
		static const struct { uint32_t usage; const char *name; } keys[] = {{0xEA, "volume-down"}, {0xEA, "volume-down"}, {0xE9, "volume-up"}};
		float prev = v0;
		for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			for (int down = 1; down >= 0; down--) {
				CFTypeRef e = kev(kCFAllocatorDefault, mach_absolute_time(), 0x0C, keys[k].usage, down, 0);
				if (e) {
					disp(c, e);
					CFRelease(e);
				}
				usleep(80000);
			}
			sleep(2);
			float v1 = vol_get(keys[k].name);
			say("hid: dispatched %s key: %s (%.3f -> %.3f)", keys[k].name, (v1 > prev + 0.001f || v1 < prev - 0.001f) ? "CHANGED the volume" : "did NOT change the volume", prev, v1);
			prev = v1;
		}
		if (getenv("PIXUI_AQ_AFTER_KEYS"))
			say("snd: aq tone after the keys (volume %.3f) rc %d", prev, spawn_wait("/usr/local/bin/sweetaudio_play", "aq", 20));
		if (v0 >= 0)
			vol_set(v0);
	}
	CFRelease(c);
}

/* ---- 5. sounds: system sounds (lock 1100, key click 1104) and an AudioQueue tone at two volumes ---- */
static volatile int snd_done;
static void snd_cb(SystemSoundID id, void *ctx)
{
	(void)id;
	(void)ctx;
	snd_done = 1;
}

static void play_id(SystemSoundID id)
{
	snd_done = 0;
	AudioServicesAddSystemSoundCompletion(id, NULL, NULL, snd_cb, NULL);
	AudioServicesPlaySystemSound(id);
	for (int i = 0; i < 40 && !snd_done; i++)
		run_loop(0.1);
	AudioServicesRemoveSystemSoundCompletion(id);
	say("snd: system sound %u completion %s", (unsigned)id, snd_done ? "yes" : "no (4 s)");
}

static void probe_snd(float v0)
{
	int tok;
	uint64_t st = 0;
	if (notify_register_check("com.apple.springboard.ringerstate", &tok) == NOTIFY_STATUS_OK) {
		notify_get_state(tok, &st);
		notify_cancel(tok);
	}
	say("snd: springboard ringerstate %llu", (unsigned long long)st);
	play_id(1100);
	sleep(1);
	play_id(1104);
	sleep(1);
	vol_set(0.25f);
	vol_get("at-0.25");
	say("snd: aq tone at volume 0.25 rc %d", spawn_wait("/usr/local/bin/sweetaudio_play", "aq", 20));
	sleep(1);
	vol_set(1.0f);
	vol_get("at-1.0");
	say("snd: aq tone at volume 1.0 rc %d", spawn_wait("/usr/local/bin/sweetaudio_play", "aq", 20));
	if (v0 >= 0)
		vol_set(v0);
}

/* ---- 6. listening sequence (doc 56 Step 9): PIXUI_SEQ="T:k,T:k,..." with T = seconds of kernel uptime (CLOCK_UPTIME_RAW) and k =
 * d (/dev/sweetaudio direct: sweetaudio_tone 3 660 3 = 3 beeps), a (AudioQueue via mediaserverd: sweetaudio_play aq = 5 beeps 1320 Hz),
 * s (AudioServices system sound: sweetaudio_play sys = one tri-tone). At most 16 entries, T <= 900, each pattern bounded 20 s; the volume
 * and ringer state are printed before each pattern (read only). ---- */
static double uptime_s(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static void probe_seq(const char *spec)
{
	char buf[256];
	unsigned n = 0;
	snprintf(buf, sizeof buf, "%s", spec ? spec : "");
	for (char *save = NULL, *e = strtok_r(buf, ",", &save); e && n < 16; e = strtok_r(NULL, ",", &save), n++) {
		int t = atoi(e);
		char *c = strchr(e, ':');
		char k = c ? c[1] : '?';
		if (t <= 0 || t > 900 || (k != 'd' && k != 'a' && k != 's')) {
			gap("seq: entry '%s' ignored", e);
			continue;
		}
		double now = uptime_s();
		if (now < t)
			usleep((useconds_t)((t - now) * 1e6));
		int tok;
		uint64_t st = 0;
		if (notify_register_check("com.apple.springboard.ringerstate", &tok) == NOTIFY_STATUS_OK) {
			notify_get_state(tok, &st);
			notify_cancel(tok);
		}
		vol_get("seq");
		say("seq: pattern %u kind %c start at uptime %.1f s (planned %d), ringerstate %llu", n, k, uptime_s(), t, (unsigned long long)st);
		int rc;
		if (k == 'd') {
			pid_t pid;
			char *av[] = {"/usr/local/bin/sweetaudio_tone", "3", "660", "3", NULL};
			int s = 0;
			rc = posix_spawn(&pid, av[0], NULL, NULL, av, environ) == 0 ? 0 : -1;
			for (int i = 0; rc == 0 && i < 200; i++) {
				if (waitpid(pid, &s, WNOHANG) == pid) {
					rc = WIFEXITED(s) ? WEXITSTATUS(s) : 128;
					break;
				}
				usleep(100000);
				if (i == 199) {
					kill(pid, 9);
					waitpid(pid, &s, 0);
					rc = -2;
				}
			}
		} else {
			rc = spawn_wait("/usr/local/bin/sweetaudio_play", k == 'a' ? "aq" : "sys", 20);
		}
		say("seq: pattern %u kind %c end at uptime %.1f s rc %d", n, k, uptime_s(), rc);
	}
}

int main(int argc, char **argv)
{
	const char *m = argc > 1 ? argv[1] : "all";
	if (!strcmp(m, "seq")) {
		say("start mode seq at uptime %.1f s, spec %s", uptime_s(), getenv("PIXUI_SEQ") ? getenv("PIXUI_SEQ") : "(none)");
		vol_get("seq-start");
		probe_seq(getenv("PIXUI_SEQ"));
		say("RESULT done seq gaps=%u", gaps);
		return 0;
	}
	int all = !strcmp(m, "all");
	int wait_s = getenv("PIXUI_WAIT") ? atoi(getenv("PIXUI_WAIT")) : 0;
	t_start = time(NULL);
	say("start mode %s wait %d", m, wait_s);
	if (all || !strcmp(m, "mg"))
		probe_mg();
	if (wait_s > 0 && wait_s <= 600)
		sleep((unsigned)wait_s);
	if (all || !strcmp(m, "bt"))
		probe_bt();
	if (all || !strcmp(m, "wifi"))
		probe_wifi();
	float v0 = -1;
	if (all || !strcmp(m, "vol") || !strcmp(m, "snd"))
		v0 = vol_get("start");
	if (all || !strcmp(m, "snd"))
		probe_snd(v0);
	if (all || !strcmp(m, "vol"))
		probe_hid(v0);
	if (all || !strcmp(m, "mg"))
		probe_mg();	/* again: MG answers after the daemons are up */
	if (getenv("PIXUI_LATE_AQ")) {	/* a tone at a fixed time after the start (e.g. after a kernel sweethidtestkey press) */
		int at = atoi(getenv("PIXUI_LATE_AQ"));
		time_t now = time(NULL);
		if (at > 0 && at <= 900 && now - t_start < at)
			sleep((unsigned)(at - (now - t_start)));
		vol_get("late");
		say("snd: late aq tone rc %d", spawn_wait("/usr/local/bin/sweetaudio_play", "aq", 20));
	}
	if (getenv("PIXUI_FINAL_VOL")) {	/* Step 9: put the volume back after kernel test presses (sweethidtestkey) changed it */
		float fv = (float)atof(getenv("PIXUI_FINAL_VOL"));
		if (fv >= 0.0f && fv <= 1.0f) {
			vol_set(fv);
			vol_get("final");
		}
	}
	say("RESULT done mg_wifi=%d mg_bt=%d mg_volbuttons=%d wifi_scan=%d gaps=%u", mg_wifi, mg_bt, mg_vol, scan_n, gaps);
	return 0;
}
