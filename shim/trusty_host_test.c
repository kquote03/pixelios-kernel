/* Host test of the Trusty version probe (xnu patch 0156, bsd/dev/sweet_trusty.h) against a fake monitor. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sweet_trusty.h"

struct fake {
	const char *ver;
	uint32_t len_ret, api_ret, bad_at, bad_val;     /* len_ret 0 = strlen(ver); bad_at = index+1 that answers bad_val */
	unsigned calls, other, order_err;
	uint64_t log_fid[256], log_a1[256];
};

static uint64_t
fsmc(void *c, uint64_t fid, uint64_t a1)
{
	struct fake *f = c;
	uint32_t n = (uint32_t)strlen(f->ver);

	if (f->calls < 256) {
		f->log_fid[f->calls] = fid;
		f->log_a1[f->calls] = a1;
	}
	f->calls++;
	if (fid == SWT_FC_GET_VERSION_STR) {
		if (a1 == 0xFFFFFFFFull) {
			return 0xDEAD000000000000ull | (f->len_ret ? f->len_ret : n);  /* junk in the high half must be ignored */
		}
		if (f->bad_at && a1 == f->bad_at - 1u) {
			return f->bad_val;
		}
		if (a1 >= n) {
			f->order_err++;
			return 0xFFFFFFFEu;
		}
		return (uint8_t)f->ver[a1];
	}
	if (fid == SWT_FC_API_VERSION) {
		if (a1 != 5) {
			f->order_err++;
		}
		return f->api_ret;
	}
	f->other++;
	return 0xFFFFFFFFu;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int
run(struct fake *f, struct swt_res *r)
{
	struct swt_bus b = { fsmc, f };

	f->calls = f->other = f->order_err = 0;
	memset(r, 0xA5, sizeof *r);
	return swt_probe(&b, r);
}

int
main(void)
{
	static const char stock[] = "Project: slider, Build: 15644732, Built: 13:17:42 Jun 15 2026";
	struct fake f;
	struct swt_res r;
	char big[400];
	unsigned i;

	/* happy path: the stock answer */
	memset(&f, 0, sizeof f);
	f.ver = stock;
	f.api_ret = 5;
	CHECK(run(&f, &r) == SWT_OK && r.rc == SWT_OK);
	CHECK(r.len == strlen(stock) && strcmp(r.ver, stock) == 0 && r.api == 5);
	CHECK(f.calls == strlen(stock) + 2 && r.nsmc == f.calls && f.other == 0 && f.order_err == 0);
	CHECK(f.log_fid[0] == SWT_FC_GET_VERSION_STR && f.log_a1[0] == 0xFFFFFFFFull);     /* length first */
	CHECK(f.log_fid[f.calls - 1] == SWT_FC_API_VERSION && f.log_a1[f.calls - 1] == 5); /* api version last */
	for (i = 1; i + 1 < f.calls; i++) {
		CHECK(f.log_fid[i] == SWT_FC_GET_VERSION_STR && f.log_a1[i] == i - 1);
	}

	/* an older Trusty selects a lower api: accepted and reported */
	f.api_ret = 3;
	CHECK(run(&f, &r) == SWT_OK && r.api == 3);

	/* undefined SMC on the first call: one call, nothing else tried */
	memset(&f, 0, sizeof f);
	f.ver = stock;
	f.len_ret = 0xFFFFFFFFu;
	CHECK(run(&f, &r) == SWT_E_UNDEF && f.calls == 1 && r.raw == 0xFFFFFFFFu && r.ver[0] == 0);

	/* negative and zero lengths, and a length above the bound: refused after one call */
	f.len_ret = 0xFFFFFFFEu;
	CHECK(run(&f, &r) == SWT_E_LEN && f.calls == 1);
	f.len_ret = 0x80000000u;
	CHECK(run(&f, &r) == SWT_E_LEN && f.calls == 1);
	memset(big, 'x', sizeof big);
	big[SWT_VER_MAX + 1] = 0;
	f.ver = big;
	f.len_ret = 0;
	CHECK(run(&f, &r) == SWT_E_LEN && f.calls == 1 && r.raw == SWT_VER_MAX + 1);
	big[SWT_VER_MAX] = 0;                           /* exactly the bound: accepted, the buffer holds it and the NUL */
	f.api_ret = 5;
	CHECK(run(&f, &r) == SWT_OK && r.len == SWT_VER_MAX && strlen(r.ver) == SWT_VER_MAX && f.calls == SWT_VER_MAX + 2);
	f.ver = "";
	CHECK(run(&f, &r) == SWT_E_LEN && f.calls == 1);

	/* an error in the middle of the string: stop there, no api call */
	memset(&f, 0, sizeof f);
	f.ver = stock;
	f.api_ret = 5;
	f.bad_at = 4;
	f.bad_val = 0xFFFFFFF9u;                        /* SM_ERR_INTERNAL_FAILURE */
	CHECK(run(&f, &r) == SWT_E_CHAR && f.calls == 5 && r.raw == 0xFFFFFFF9u && strcmp(r.ver, "Pro") == 0);
	CHECK(f.log_fid[f.calls - 1] == SWT_FC_GET_VERSION_STR);

	/* unprintable or wide characters never reach the log */
	f.bad_val = 0x1Bu;
	CHECK(run(&f, &r) == SWT_OK && r.ver[3] == '?');
	f.bad_val = 0x141u;
	CHECK(run(&f, &r) == SWT_OK && r.ver[3] == '?');
	f.bad_val = 0x7Fu;
	CHECK(run(&f, &r) == SWT_OK && r.ver[3] == '?');

	/* api answers: undefined, and a version above the one we asked for */
	memset(&f, 0, sizeof f);
	f.ver = stock;
	f.api_ret = 0xFFFFFFFFu;
	CHECK(run(&f, &r) == SWT_E_UNDEF && r.api == 0 && strcmp(r.ver, stock) == 0);
	f.api_ret = 6;
	CHECK(run(&f, &r) == SWT_E_API && r.raw == 6 && r.api == 0);

	/* the allowlist itself */
	CHECK(swt_allowed(SWT_FC_GET_VERSION_STR, 0xFFFFFFFFull, 0));
	CHECK(!swt_allowed(SWT_FC_GET_VERSION_STR, 0, 0));
	CHECK(swt_allowed(SWT_FC_GET_VERSION_STR, 60, 61) && !swt_allowed(SWT_FC_GET_VERSION_STR, 61, 61));
	CHECK(!swt_allowed(SWT_FC_GET_VERSION_STR, 0xFFFFFFFFFFFFFFFFull, 61));
	CHECK(!swt_allowed(SWT_FC_GET_VERSION_STR, 0, SWT_VER_MAX + 1));
	CHECK(swt_allowed(SWT_FC_API_VERSION, 5, 0) && !swt_allowed(SWT_FC_API_VERSION, 4, 0) && !swt_allowed(SWT_FC_API_VERSION, 6, 0));
	CHECK(!swt_allowed(0x3C000003u, 0, 0) && !swt_allowed(0x32000014u, 0, 0) && !swt_allowed(0xBC000003u, 0, 0) && !swt_allowed(0x84000073u, 0, 0));
	CHECK(!swt_allowed(0x82000504u, 0, 0) && !swt_allowed(0, 0, 0));

	/* bad arguments */
	{
		struct swt_bus nb = { 0, 0 };
		CHECK(swt_probe(&nb, &r) == SWT_E_ARG && swt_probe(0, &r) == SWT_E_ARG);
	}
	CHECK(strcmp(swt_ename(SWT_E_UNDEF), "undefined-smc") == 0 && strcmp(swt_ename(99), "?") == 0);

	printf(fails ? "FAIL trusty_host_test (%d)\n" : "PASS trusty_host_test\n", fails);
	return fails != 0;
}
