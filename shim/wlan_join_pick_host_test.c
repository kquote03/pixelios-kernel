/*
 * Host test of xnu patch 0406 (sweet_wlan_join_pick.h, extracted from the patch by shim/build.sh): the SSID out of the join command, the directed
 * escan bytes, the strongest-AP rule (a scan sequence of one SSID on several BSSIDs, as on a dense campus) and the join-retry room check.
 * Mutants: every check below fails for a wrong offset, a >= instead of > in swjp_stronger, or a missing room term in swjp_retry_ok.
 */
#include <stdio.h>
#include <string.h>
#include "sweet_wlan_join_pick.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the join command the glue builds ("join\0" + wl_extjoin_params_v1_t, sweet_wlan_join_fw.h swj_join_ext_v1): SSID_len at ext +4, SSID at ext +8, 76 B */
static unsigned
mkjoin(uint8_t *b, const char *ssid)
{
	unsigned n = (unsigned)strlen(ssid);

	memset(b, 0, 81);
	memcpy(b, "join", 5);
	b[5 + 4] = (uint8_t)n;
	memcpy(b + 5 + 8, ssid, n);
	memset(b + 5 + 40, 0xff, 20);
	return 81u;
}

/* the scan-record loop of swl25r_note_src (patch 0406 part), reduced to the decision: returns the index of the record that ends as the target */
struct rec { int bss; int rssi; int ours; };
static int
pick(const struct rec *r, unsigned n)
{
	int tgt = -1, best = 0;
	unsigned i;

	for (i = 0; i < n; i++) {
		if (!r[i].ours) {
			continue;
		}
		if (tgt < 0) {
			tgt = r[i].bss;
			best = r[i].rssi;
		} else if (r[i].bss != tgt && swjp_stronger(best, r[i].rssi)) {
			tgt = r[i].bss;
			best = r[i].rssi;
		} else if (r[i].bss == tgt && r[i].rssi > best) {
			best = r[i].rssi;
		}
	}
	return tgt;
}

int
main(void)
{
	uint8_t j[96], ss[32], arg[96], ref[96];
	uint32_t sl = 99;
	unsigned n;

	/* 1. SSID from the join command */
	n = mkjoin(j, "OpenGuest");
	CHECK(swjp_ssid_from_join(j, n, ss, &sl) == 0 && sl == 9 && memcmp(ss, "OpenGuest", 9) == 0);
	n = mkjoin(j, "0123456789abcdef0123456789abcdef");
	CHECK(swjp_ssid_from_join(j, n, ss, &sl) == 0 && sl == 32 && ss[31] == 'f');
	j[9] = 33;
	CHECK(swjp_ssid_from_join(j, n, ss, &sl) == -1 && sl == 0);         /* SSID_len above 32 */
	n = mkjoin(j, "x");
	j[9] = 0;
	CHECK(swjp_ssid_from_join(j, n, ss, &sl) == -1);                     /* SSID_len 0 */
	n = mkjoin(j, "x");
	memcpy(j, "wsec", 5);
	CHECK(swjp_ssid_from_join(j, n, ss, &sl) == -1);                     /* not the join command */
	n = mkjoin(j, "abc");
	CHECK(swjp_ssid_from_join(j, 15, ss, &sl) == -1);                    /* shorter than name + 8 + SSID */
	CHECK(swjp_ssid_from_join(j, 16, ss, &sl) == 0 && sl == 3);
	CHECK(swjp_ssid_from_join(NULL, 81, ss, &sl) == -1);

	/* 2. directed escan bytes: only +12..+47 and +56 change */
	memset(arg, 0, sizeof arg);
	arg[10] = 72;
	memset(arg + 48, 0xff, 6);
	arg[54] = 2;
	arg[56] = 1;
	memset(arg + 60, 0xff, 16);
	memcpy(ref, arg, sizeof ref);
	swjp_escan_ssid(arg, (const uint8_t *)"OpenGuest", 9);
	CHECK(arg[12] == 9 && arg[13] == 0 && arg[14] == 0 && arg[15] == 0);
	CHECK(memcmp(arg + 16, "OpenGuest", 9) == 0 && arg[25] == 0 && arg[47] == 0);
	CHECK(arg[56] == 0);                                                   /* scan_type active */
	CHECK(memcmp(arg, ref, 12) == 0 && memcmp(arg + 48, ref + 48, 8) == 0 && memcmp(arg + 57, ref + 57, 96 - 57) == 0);
	memcpy(arg, ref, sizeof arg);
	swjp_escan_ssid(arg, (const uint8_t *)"x", 0);
	CHECK(memcmp(arg, ref, sizeof arg) == 0);                             /* no SSID: broadcast passive scan unchanged */
	swjp_escan_ssid(arg, (const uint8_t *)"x", 33);
	CHECK(memcmp(arg, ref, sizeof arg) == 0);

	/* 3. strongest AP of the SSID (campus: one SSID on several BSSIDs, repeats, other SSIDs in between) */
	{
		const struct rec a[] = { { 1, -81, 1 }, { 9, -40, 0 }, { 2, -66, 1 }, { 3, -78, 1 }, { 2, -64, 1 }, { 4, -70, 1 } };
		const struct rec b[] = { { 5, -60, 1 }, { 6, -60, 1 }, { 7, -61, 1 } };    /* equal RSSI: the first stays */
		const struct rec c[] = { { 1, -80, 1 }, { 1, -50, 1 }, { 2, -55, 1 } };    /* the target heard again stronger: its best value counts */
		const struct rec d[] = { { 9, -30, 0 } };
		CHECK(pick(a, 6) == 2);
		CHECK(pick(b, 3) == 5);
		CHECK(pick(c, 3) == 1);
		CHECK(pick(d, 1) == -1);
		CHECK(swjp_stronger(-70, -69) && !swjp_stronger(-70, -70) && !swjp_stronger(-60, -70));
	}

	/* 4. join retry room */
	CHECK(swjp_retry_ok(1, 100, 30, 48));
	CHECK(swjp_retry_ok(2, 100, 30, 48));
	CHECK(!swjp_retry_ok(3, 100, 30, 48));                                /* three attempts made */
	CHECK(swjp_retry_ok(1, SWJP_R0_LIMIT - SWJP_R0_ROOM, 0, 48));
	CHECK(!swjp_retry_ok(1, SWJP_R0_LIMIT - SWJP_R0_ROOM + 1, 0, 48));    /* ring 0 full */
	CHECK(swjp_retry_ok(1, 0, 48 - SWJP_EV_ROOM, 48));
	CHECK(!swjp_retry_ok(1, 0, 48 - SWJP_EV_ROOM + 1, 48));               /* event cap */
	CHECK(SWJP_JOIN_TRIES == 3u);

	if (fails) {
		printf("wlan_join_pick_host_test: %d FAIL\n", fails);
		return 1;
	}
	printf("wlan_join_pick_host_test: PASS (ssid parse, directed escan bytes, strongest AP, retry room)\n");
	return 0;
}
