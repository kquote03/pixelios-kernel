/* Host test of xnu patch 0400 (open-network join): sweet_wlan_join.h + sweet_wlan_join_fw.h + sweet_wlan_cred.h as patch 0206 ships them (shim/ copies) with the
 * sweet_wlan_cred.h and sweet_wlan_join_fw.h hunks of patch 0400 on top (shim/build.sh assembles the directory). No hardware.
 * Oracle: the open blob below was produced by tools/mkwlancred.py build_blob (net 2, PIXOS_WIFI2_SECURITY=open, made-up SSID, DHCP), not by the C code.
 *   O1 open blob: loads, open = 1, PMK all zero, SSID exact, addresses zero, `security=open` line, region wiped
 *   O2 security 1 with psk_len 8 (valid crc) is refused (creds invalid), as are security 2 and security 0 with psk_len 0
 *   O3 swj_join_cmds_open: wpa_auth 0 and wsec 0 (OpenBSD bwfm.c:2036-2040); the other four commands are byte-identical to swj_join_cmds
 *   O4 swj_join_cmds (WPA2-PSK path) unchanged: wpa_auth 0x80, wsec 4 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char logbuf[1 << 14];
static unsigned loglen;
static void lg(const char *s) { loglen += (unsigned)snprintf(logbuf + loglen, sizeof logbuf - loglen, "%s\n", s); }
#define SWJ_LOG(s) lg(s)
#define SWJ_GAP(s) lg(s)
#define SWJ_RANDOM(buf, len) memset((buf), 0x42, (len))
#define SWC_LOG(s) lg(s)
#define SWC_GAP(s) lg(s)
#include "sweet_wlan_join.h"
#include "sweet_wlan_cred.h"

static unsigned fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static const uint8_t OPEN_BLOB[SWC_SIZE] = {
	0x57,0x4c,0x41,0x4e,0x43,0x52,0x42,0x31,0x01,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x63,0x61,0x6e,0x61,0x72,0x79,0x2d,0x6f,0x70,0x65,0x6e,0x00,
	[124] = 0x87, 0x40, 0xa4, 0x8b };

static uint32_t var_u32(const struct swj_cmd *c, const char *name)
{
	uint32_t nl = (uint32_t)strlen(name) + 1u;

	if (c->len != nl + 4u || memcmp(c->buf, name, nl) != 0)
		return 0xdeadbeefu;
	return swc_le32(c->buf + nl);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* a copy of the open blob with security / psk_len changed and the crc redone */
static void variant(uint8_t *b, uint32_t sec, uint32_t pl)
{
	memcpy(b, OPEN_BLOB, SWC_SIZE);
	put32(b + 0x08, sec);
	put32(b + 0x10, pl);
	if (pl)
		memset(b + 0x34, 'a', pl);
	put32(b + 0x7c, swc_crc32(b, SWC_SIZE - 4u));
}

int main(void)
{
	uint8_t region[SWC_SIZE], work[SWC_SIZE], z[32];
	struct swc_creds cr;
	struct swj_cmd a[6], o[6];
	static const uint8_t any[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	unsigned i;

	/* O1 */
	memset(z, 0, sizeof z);
	memcpy(region, OPEN_BLOB, SWC_SIZE);
	CHECK(swc_check(region) == 0);
	CHECK(swc_load(region, SWC_SIZE, work, &cr) == 0);
	CHECK(cr.open == 1u && cr.ssid_len == 11u && memcmp(cr.ssid, "canary-open", 11) == 0);
	CHECK(memcmp(cr.pmk, z, 32) == 0 && memcmp(cr.ip, z, 4) == 0 && memcmp(cr.gw, z, 4) == 0);
	CHECK(memcmp(region, z, 32) == 0 && memcmp(work, z, 32) == 0);
	CHECK(strstr(logbuf, "credentials loaded security=open") != NULL);
	/* O2 */
	{
		static const uint32_t bad[3][2] = { { 1u, 8u }, { 2u, 0u }, { 0u, 0u } };

		for (i = 0; i < 3u; i++) {
			variant(region, bad[i][0], bad[i][1]);
			loglen = 0; logbuf[0] = 0;
			CHECK(swc_load(region, SWC_SIZE, work, &cr) == -1);
			CHECK(strstr(logbuf, SWC_GAP_INVALID) != NULL && cr.open == 0u && cr.ssid_len == 0u);
		}
	}
	/* O3 / O4 */
	CHECK(swj_join_cmds(a, (const uint8_t *)"canary-open", 11u, any, 0) == 6u);
	CHECK(swj_join_cmds_open(o, (const uint8_t *)"canary-open", 11u, any) == 6u);
	CHECK(var_u32(&a[1], "wpa_auth") == SWJ_WPA2_AUTH_PSK && var_u32(&a[2], "wsec") == SWJ_WSEC_AES);
	CHECK(var_u32(&o[1], "wpa_auth") == 0u && var_u32(&o[2], "wsec") == 0u);
	CHECK(var_u32(&o[0], "sup_wpa") == 0u && var_u32(&o[3], "auth") == 0u && var_u32(&o[4], "mfp") == 0u);
	for (i = 0; i < 6u; i++) {
		CHECK(o[i].cmd == a[i].cmd && strcmp(o[i].row, a[i].row) == 0);
		if (i != 1u && i != 2u)
			CHECK(o[i].len == a[i].len && memcmp(o[i].buf, a[i].buf, a[i].len) == 0);
	}
	CHECK(swj_join_cmds_open(o, (const uint8_t *)"x", 0u, any) == 0u);
	printf("RESULT: %s wlan_open_host_test failures=%u\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
