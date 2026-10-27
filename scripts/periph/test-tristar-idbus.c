// SPDX-License-Identifier: GPL-2.0-only
/*
 * Offline harness for drivers/power/supply/apple_tristar_idbus.h (branch
 * a9/usb-host3). Runs the exact kernel ID-bus code against a fake CBTL1610
 * register file and checks the CRC8, the 16-field decode, the register
 * sequence and its invariants. No device access.
 *
 * The fake follows the 2026-10-26 hardware trial: the response lands
 * forward in the RX window 0x20..0x27 and stays there; any write into that
 * window overwrites it (the usb-host2 clobber); the TX window ends at 0x60
 * and GO is 0x61; the reorient bit of 0x05 self-clears and can make the IC
 * run its own ID exchange; 0x0d is read-to-clear.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -I <kernel>/drivers/power/supply \
 *      -o test-tristar-idbus scripts/periph/test-tristar-idbus.c
 *   ./test-tristar-idbus            # all tests
 *   ./test-tristar-idbus --crc HEX  # CRC8 of one vector (Python cross-check)
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#include "apple_tristar_idbus.h"

#define MAX_TRACE 1024
#define MAX_GO 4
#define MAX_TRIG 8
#define RX TRISTAR_IDBUS_REG_RX

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		failures++;						\
		printf("FAIL %s:%d: ", __func__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

struct fake {
	u8 reg[256];
	/* 0x0d reads: list 0 before any trigger, then one list per GO/reorient */
	int events[MAX_TRIG][16];
	unsigned int trig;
	unsigned int ev_pos;
	/* Lands in the RX window on GO n (NULL: engine idle) and on a reorient */
	const u8 *go_resp[MAX_GO];
	unsigned int go_resp_len[MAX_GO];
	const u8 *reorient_resp;
	u8 go_window[MAX_GO][4];	/* 0x5d..0x60 when GO was written */
	unsigned int gos;
	unsigned int reorients;
	bool sticky_reset;
	u8 readback_xor;		/* corrupts the first TX readback byte */
	bool clobbered;			/* something wrote into 0x20..0x27 */
	unsigned int fail_op;		/* 1-based I/O op to fail; 0 = none */
	bool fail_after_effect;		/* the failing update still lands */
	unsigned int io_ops;
	unsigned int slept;
	char trace[MAX_TRACE][48];
	unsigned int n;
	char log[32768];
	size_t log_len;
};

static void trace(struct fake *f, const char *fmt, ...) __printf(2, 3);
static void trace(struct fake *f, const char *fmt, ...)
{
	va_list args;

	if (f->n >= MAX_TRACE) {
		fprintf(stderr, "trace overflow\n");
		exit(2);
	}
	va_start(args, fmt);
	vsnprintf(f->trace[f->n++], sizeof(f->trace[0]), fmt, args);
	va_end(args);
}

static bool inject(struct fake *f)
{
	if (++f->io_ops != f->fail_op)
		return false;
	trace(f, "X");
	return true;
}

static void note_write(struct fake *f, unsigned int reg, unsigned int len)
{
	if (reg < RX + 8 && reg + len > RX)
		f->clobbered = true;
}

static void fake_trigger(struct fake *f)
{
	if (++f->trig >= MAX_TRIG) {
		fprintf(stderr, "too many triggers\n");
		exit(2);
	}
	f->ev_pos = 0;
}

static int next_event(struct fake *f)
{
	int *list = f->events[f->trig];

	if (f->ev_pos >= ARRAY_SIZE(f->events[0]) || list[f->ev_pos] < 0)
		return 0;
	return list[f->ev_pos++];
}

static int fake_read(void *ctx, u8 reg)
{
	struct fake *f = ctx;
	int val;

	if (inject(f))
		return -EIO;
	val = reg == TRISTAR_IDBUS_REG_EVENT ? next_event(f) : f->reg[reg];
	trace(f, "R %02x=%02x", reg, val);
	return val;
}

static void fake_go(struct fake *f)
{
	if (f->gos >= MAX_GO) {
		fprintf(stderr, "too many GOs\n");
		exit(2);
	}
	memcpy(f->go_window[f->gos], &f->reg[0x5d], 4);
	if (f->go_resp[f->gos])
		memcpy(&f->reg[RX], f->go_resp[f->gos], f->go_resp_len[f->gos]);
	f->gos++;
	fake_trigger(f);
}

static int fake_write(void *ctx, u8 reg, u8 val)
{
	struct fake *f = ctx;

	if (inject(f))
		return -EIO;
	trace(f, "W %02x %02x", reg, val);
	note_write(f, reg, 1);
	f->reg[reg] = val;
	if (reg == TRISTAR_IDBUS_REG_GO)
		fake_go(f);
	return 0;
}

static int fake_update(void *ctx, u8 reg, u8 clear, u8 set)
{
	struct fake *f = ctx;
	u8 old, val;

	if (inject(f)) {
		/* A NAK after the data byte: the register changed anyway. */
		if (f->fail_after_effect)
			f->reg[reg] = (f->reg[reg] & ~clear) | set;
		return -EIO;
	}
	old = f->reg[reg];
	val = (old & ~clear) | set;
	trace(f, "U %02x %02x>%02x", reg, old, val);
	note_write(f, reg, 1);
	if (f->sticky_reset && reg == TRISTAR_IDBUS_REG_CTRL)
		val |= old & TRISTAR_IDBUS_CTRL_RESET;
	if (reg == TRISTAR_IDBUS_REG_CTRL && (val & TRISTAR_IDBUS_CTRL_REORIENT)) {
		/* Self-clearing; may run the IC's own ID exchange. */
		val &= ~TRISTAR_IDBUS_CTRL_REORIENT;
		f->reorients++;
		if (f->reorient_resp)
			memcpy(&f->reg[RX], f->reorient_resp, 8);
		fake_trigger(f);
	}
	f->reg[reg] = val;
	return old;
}

static int fake_block_read(void *ctx, u8 reg, u8 *buf, unsigned int len)
{
	struct fake *f = ctx;

	if (inject(f))
		return -EIO;
	trace(f, "BR %02x %u", reg, len);
	memcpy(buf, &f->reg[reg], len);
	if (reg >= 0x5d && reg <= 0x60)
		buf[0] ^= f->readback_xor;
	return 0;
}

static int fake_block_write(void *ctx, u8 reg, const u8 *buf, unsigned int len)
{
	struct fake *f = ctx;
	char hex[64];

	if (inject(f))
		return -EIO;
	tristar_idbus_format_hex(buf, len, hex, sizeof(hex));
	trace(f, "BW %02x %s", reg, hex);
	note_write(f, reg, len);
	memcpy(&f->reg[reg], buf, len);
	return 0;
}

static void fake_sleep(void *ctx, unsigned int ms)
{
	struct fake *f = ctx;

	trace(f, "S %u", ms);
	f->slept += ms;
}

static void fake_log(void *ctx, const char *fmt, ...) __printf(2, 3);
static void fake_log(void *ctx, const char *fmt, ...)
{
	struct fake *f = ctx;
	va_list args;

	if (f->log_len + 2 >= sizeof(f->log))
		return;
	va_start(args, fmt);
	f->log_len += vsnprintf(f->log + f->log_len, sizeof(f->log) - f->log_len, fmt, args);
	va_end(args);
	if (f->log_len + 1 < sizeof(f->log))
		f->log[f->log_len++] = '\n';
	f->log[f->log_len] = '\0';
}

static const struct tristar_idbus_ops fake_ops = {
	.read = fake_read,
	.write = fake_write,
	.update = fake_update,
	.block_read = fake_block_read,
	.block_write = fake_block_write,
	.sleep_ms = fake_sleep,
	.log = fake_log,
};

/* Our Mac cable, live under iOS: IOAccessoryDigitalID <100c00000000> */
static const u8 mac_frame[8] = { 0x75, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x66 };
static const u8 mac_id[6] = { 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00 };
/* Camera adapter: retained intact before the usb-host2 TX clobbered it */
static const u8 adapter_frame[8] = { 0x75, 0x11, 0xf0, 0x00, 0x00, 0x00, 0x00, 0xd6 };
static const u8 adapter_id[6] = { 0x11, 0xf0, 0x00, 0x00, 0x00, 0x00 };
/* What usb-host2 left after writing 74 00 02 into the RX window */
static const u8 clobbered_frame[8] = { 0x74, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0xd6 };

static void fake_init(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	memset(f->events, 0xff, sizeof(f->events));
	f->reg[TRISTAR_IDBUS_REG_REVISION] = TRISTAR_IDBUS_REVISION_J71T;
	f->reg[TRISTAR_IDBUS_REG_STATUS] = 0x83;	/* VBUS, bit 7 */
	f->reg[TRISTAR_IDBUS_REG_DXCTRL] = 0x11;
	for (unsigned int i = 0; i < MAX_GO; i++) {
		f->go_resp[i] = mac_frame;
		f->go_resp_len[i] = sizeof(mac_frame);
	}
}

static void ib_init(struct tristar_idbus *ib, struct fake *f)
{
	memset(ib, 0, sizeof(*ib));
	ib->ops = &fake_ops;
	ib->ctx = f;
}

static int run_identify(struct fake *f, struct tristar_idbus *ib, unsigned int flags,
			struct tristar_idbus_id_result *res)
{
	ib_init(ib, f);
	return tristar_idbus_identify(ib, flags, res);
}

static int run_recover(struct fake *f, struct tristar_idbus *ib, unsigned int action,
		       struct tristar_idbus_recover_result *res)
{
	ib_init(ib, f);
	return tristar_idbus_recover(ib, action, res);
}

static bool written(const char *t)
{
	unsigned int r;

	return sscanf(t, "W %x", &r) == 1 || sscanf(t, "U %x", &r) == 1 ||
	       sscanf(t, "BW %x", &r) == 1;
}

static unsigned int count_prefix(const struct fake *f, const char *prefix)
{
	unsigned int i, c = 0;

	for (i = 0; i < f->n; i++)
		c += !strncmp(f->trace[i], prefix, strlen(prefix));
	return c;
}

static int find(const struct fake *f, unsigned int from, const char *entry)
{
	for (unsigned int i = from; i < f->n; i++)
		if (!strcmp(f->trace[i], entry))
			return i;
	return -1;
}

static unsigned int writes_after(const struct fake *f, unsigned int from)
{
	unsigned int i, c = 0;

	for (i = from; i < f->n; i++)
		c += written(f->trace[i]);
	return c;
}

/* Invariants that every scenario must keep. */
static void check_invariants(const struct fake *f, const char *name)
{
	unsigned int i, r, a, b, base, len, base2;

	CHECK(!f->clobbered, "%s: wrote into the RX window 0x20..0x27", name);
	for (i = 0; i < f->n; i++) {
		const char *t = f->trace[i];

		if (!written(t))
			continue;
		/* No write before the revision check */
		CHECK(i > 0 && !strcmp(f->trace[0], "R 0f=8a"), "%s: write before revision check", name);
		if (sscanf(t, "BW %x", &r) == 1) {
			len = (strlen(t) - 6 + 1) / 3;
			CHECK(r >= 0x5d && r + len - 1 == TRISTAR_IDBUS_REG_TX_END,
			      "%s: block write outside the TX window: %s", name, t);
		} else if (sscanf(t, "U %x %x>%x", &r, &a, &b) == 3) {
			CHECK(r == TRISTAR_IDBUS_REG_CTRL || r == TRISTAR_IDBUS_REG_FAULT,
			      "%s: update outside 0x05/0x0a: %s", name, t);
			CHECK(r != TRISTAR_IDBUS_REG_FAULT || a == b, "%s: 0x0a update not an echo: %s",
			      name, t);
		} else if (sscanf(t, "W %x %x", &r, &a) == 2) {
			CHECK(r == TRISTAR_IDBUS_REG_FAULT || r == TRISTAR_IDBUS_REG_TX_END ||
			      r == TRISTAR_IDBUS_REG_GO, "%s: write outside the allowed set: %s", name, t);
			if (r == TRISTAR_IDBUS_REG_TX_END)
				CHECK(a == TRISTAR_IDBUS_TX_PREPARE, "%s: 0x60 written with 0x%02x", name, a);
			if (r == TRISTAR_IDBUS_REG_FAULT) {
				char want[16];

				snprintf(want, sizeof(want), "R 0a=%02x", a);
				CHECK(!strcmp(f->trace[i - 1], want), "%s: 0x0a write is not an echo: %s",
				      name, t);
			}
			/* GO: directly after prepare, TX window and its readback */
			if (r == TRISTAR_IDBUS_REG_GO) {
				CHECK(i >= 3 && !strcmp(f->trace[i - 3], "W 60 01") &&
				      sscanf(f->trace[i - 2], "BW %x", &base2) == 1 &&
				      sscanf(f->trace[i - 1], "BR %x %u", &base, &len) == 2 &&
				      base == base2 && base + len - 1 == TRISTAR_IDBUS_REG_TX_END &&
				      a == (((len - 2) << 1) | 1),
				      "%s: GO at %u is not prepare, TX, readback", name, i);
			}
		}
	}
	CHECK(count_prefix(f, "U 05") <= 2, "%s: more than two 0x05 updates", name);
}

static void dump_trace(const struct fake *f)
{
	for (unsigned int i = 0; i < f->n; i++)
		printf("  %3u %s\n", i, f->trace[i]);
}

static void expect_trace(const struct fake *f, const char *const *want, unsigned int n,
			 const char *name)
{
	int before = failures;
	unsigned int i;

	CHECK(f->n == n, "%s: %u trace entries, expected %u", name, f->n, n);
	for (i = 0; i < n && i < f->n; i++)
		CHECK(!strcmp(f->trace[i], want[i]), "%s: step %u is '%s', expected '%s'",
		      name, i, f->trace[i], want[i]);
	if (failures != before)
		dump_trace(f);
}

/* ------------------------------------------------------------------ */

static void test_crc(void)
{
	static const u8 getid[] = { 0x74, 0x00, 0x02 };
	static const u8 swapped[] = { 0x74, 0x02, 0x00 };
	static const u8 check[] = "123456789";

	/* lightning-idbus.md 5.2: the article's captured request */
	CHECK(tristar_idbus_crc8(getid, 3) == 0x1f, "74 00 02 -> %02x", tristar_idbus_crc8(getid, 3));
	/* Same loop, seed 0: CRC-8/MAXIM-DOW catalogue check value */
	CHECK(tristar_idbus_crc8_seed(0, check, 9) == 0xa1, "MAXIM check %02x",
	      tristar_idbus_crc8_seed(0, check, 9));
	CHECK(tristar_idbus_crc8(swapped, 3) == 0x32, "74 02 00");
	/* Hardware: reg 0x27 read 0x66 with the Mac cable, 0xd6 with the adapter */
	CHECK(tristar_idbus_crc8(mac_frame, 7) == 0x66, "Mac frame CRC");
	CHECK(!tristar_idbus_check_frame(mac_frame, 7, TRISTAR_IDBUS_ID), "Mac frame check");
	CHECK(tristar_idbus_crc8(adapter_frame, 7) == 0xd6, "adapter frame CRC");
	CHECK(!tristar_idbus_check_frame(adapter_frame, 7, TRISTAR_IDBUS_ID), "adapter frame check");
	CHECK(tristar_idbus_check_frame(clobbered_frame, 7, TRISTAR_IDBUS_ID) == -EPROTO,
	      "usb-host2 clobbered frame has a bad header");
	{
		u8 bad[8];

		memcpy(bad, mac_frame, 8);
		bad[7] ^= 1;
		CHECK(tristar_idbus_check_frame(bad, 7, TRISTAR_IDBUS_ID) == -EBADMSG, "bad CRC");
		memcpy(bad, mac_frame, 8);
		bad[0] = 0x74;
		CHECK(tristar_idbus_check_frame(bad, 7, TRISTAR_IDBUS_ID) == -EPROTO, "bad header");
	}
}

static void test_decode(void)
{
	static const struct {
		u8 id[6];
		const char *want;
	} cases[] = {
		{ { 0x10, 0x0c }, "USB=2 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=3 AI=0 AU=0 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0x11, 0xf0 }, "USB=2 ACCx=0 DB=0 MB=0 UART=1 PWR=f HV=0 AI=0 AU=0 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0x20, 0x00 }, "USB=0 ACCx=0 DB=1 MB=0 UART=0 PWR=0 HV=0 AI=0 AU=0 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0xa0, 0x00 }, "USB=0 ACCx=2 DB=1 MB=0 UART=0 PWR=0 HV=0 AI=0 AU=0 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0x04, 0xf1 }, "USB=0 ACCx=0 DB=0 MB=1 UART=0 PWR=f HV=0 AI=0 AU=1 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0x0b, 0xf0 }, "USB=1 ACCx=0 DB=0 MB=0 UART=3 PWR=f HV=0 AI=0 AU=0 PU=0 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, "USB=3 ACCx=3 DB=1 MB=1 UART=3 PWR=f HV=3 AI=1 AU=1 PU=1 BP=1 PO=1 CC=1 BD=1 HS=7 HPM=7" },
		/* single bits do not leak into neighbours */
		{ { 0x00, 0x02, 0x80, 0x00 }, "USB=0 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=0 AI=1 AU=0 PU=1 BP=0 PO=0 CC=0 BD=0 HS=0 HPM=0" },
		{ { 0x00, 0x00, 0x48, 0x50 }, "USB=0 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=0 AI=0 AU=0 PU=0 BP=1 PO=0 CC=1 BD=0 HS=5 HPM=0" },
		{ { 0x00, 0x00, 0x01, 0x83 }, "USB=0 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=0 AI=0 AU=0 PU=0 BP=0 PO=1 CC=0 BD=1 HS=0 HPM=3" },
	};
	char buf[160];

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		tristar_idbus_format_fields(cases[i].id, buf, sizeof(buf));
		CHECK(!strcmp(buf, cases[i].want), "case %u: '%s'", i, buf);
	}
}

/* The full expected order for the corrected 9.2 sequence with a latched reset. */
static void test_identify_exact_order(void)
{
	static const char *const want[] = {
		"R 0f=8a",
		"R 0c=83", "R 0d=00", "R 05=08", "R 01=11", "R 02=00", "R 17=00", "R 0a=00",
		"BR 20 8",
		"U 05 08>00", "R 05=00", "S 20",
		"W 60 01", "BW 5d 02 00 74 0c", "BR 5d 4", "W 61 05",
		"R 0d=00", "S 10", "R 0d=00", "S 10", "R 0d=18",
		"BR 20 8", "R 0c=83",
	};
	static const u8 window[4] = { 0x02, 0x00, 0x74, 0x0c };
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	int ret;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = TRISTAR_IDBUS_CTRL_RESET;
	f.events[1][0] = 0;
	f.events[1][1] = 0;
	f.events[1][2] = TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE;	/* hardware: 0x18 */
	ret = run_identify(&f, &ib, 0, &res);
	CHECK(ret == 0, "ret %d", ret);
	CHECK(res.id_valid && !memcmp(res.id, mac_id, 6), "ID");
	CHECK(res.released_reset && !res.reoriented && !res.autonomous && res.attempts == 1, "flags");
	CHECK(!memcmp(res.frame, mac_frame, 8) && res.frame_len == 8, "raw frame");
	CHECK(res.orientation == 0x83, "0x0c read with the frame");
	CHECK(!memcmp(f.go_window[0], window, 4), "TX window at GO is 02 00 74 0c");
	CHECK(strstr(f.log, "USB=2 ACCx=0 DB=0 MB=0 UART=0 PWR=0 HV=3") != NULL, "decode logged");
	CHECK(strstr(f.log, "write 0x05: before 0x08, wrote 0x00, after 0x00") != NULL,
	      "0x05 before/after logged");
	CHECK(strstr(f.log, "write TX 0x5d: [02 00 74 0c] (request [74 00 02] reversed, RX length 0x0c; Tristar appends CRC 0x1f)"),
	      "TX logged");
	CHECK(strstr(f.log, "write 0x61 = 0x05 (GO)") != NULL, "GO logged");
	expect_trace(&f, want, ARRAY_SIZE(want), __func__);
	check_invariants(&f, __func__);
}

static void test_completion_codes(void)
{
	static const int codes[] = { TRISTAR_IDBUS_EV_DIGITAL_ID, TRISTAR_IDBUS_EV_DONE,
				     TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE };
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	for (unsigned int i = 0; i < ARRAY_SIZE(codes); i++) {
		fake_init(&f);
		f.events[1][0] = codes[i];
		CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.id_valid && f.gos == 1,
		      "0x0d = 0x%02x completes GetID", codes[i]);
		CHECK(count_prefix(&f, "U 05") == 0, "no 0x05 write when bit 3 clear");
		check_invariants(&f, __func__);
	}
}

static void test_readback_mismatch_no_go(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.readback_xor = 0x40;
	CHECK(run_identify(&f, &ib, 0, &res) == -EREMOTEIO, "readback mismatch aborts");
	CHECK(f.gos == 0 && count_prefix(&f, "W 61") == 0, "no GO after a mismatch");
	CHECK(!res.reoriented && res.attempts == 1 && !res.id_valid, "no retry, no reorient");
	CHECK(strstr(f.log, "readback mismatch at 0x5d: [42 00 74 0c]; no GO") != NULL, "logged");
	check_invariants(&f, __func__);
}

/*
 * The 2026-10-26 trial: our GetID gets no answer, the reorient makes the IC
 * run its own exchange (0x0d = 0x18, frame in 0x20). Read it before writing
 * anything and accept it; no second GetID.
 */
static void test_rx_after_reorient_accepted(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	int i, rx;

	fake_init(&f);
	memcpy(&f.reg[RX], clobbered_frame, 8);	/* what usb-host2 left behind */
	f.reg[TRISTAR_IDBUS_REG_CTRL] = 0x01;		/* VBUS sink on: must survive */
	f.go_resp[0] = NULL;				/* engine gives no answer */
	f.reorient_resp = adapter_frame;
	f.events[2][0] = TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0, "ok from the reorient-armed exchange");
	CHECK(res.id_valid && !memcmp(res.id, adapter_id, 6), "adapter ID 11 f0 00 00 00 00");
	CHECK(res.autonomous && res.reoriented && res.attempts == 1 && f.gos == 1,
	      "accepted without a second GetID");
	CHECK(res.reorient_read && res.reorient_status == 0 && res.reorient_events == 0x18,
	      "reorient evidence kept");
	i = find(&f, 0, "U 05 01>03");
	CHECK(i > 0 && !strcmp(f.trace[i - 1], "R 0c=83"), "VBUS read right before reorient");
	CHECK(i > 0 && !strcmp(f.trace[i + 1], "R 05=01"), "reorient bit self-clears, bit 0 kept");
	CHECK(i > 0 && !strcmp(f.trace[i + 2], "S 20") && !strcmp(f.trace[i + 3], "R 0d=18"),
	      "settle, then 0x0d");
	rx = find(&f, i, "BR 20 8");
	CHECK(rx == i + 4 && writes_after(&f, i + 1) == 0, "RX read before any write; nothing after");
	CHECK(!memcmp(&f.reg[RX], adapter_frame, 8), "RX window untouched");
	CHECK(f.slept == 100 + 20, "slept %u", f.slept);
	CHECK(strstr(f.log, "digital ID 11 f0 00 00 00 00 (reorient-armed exchange)") != NULL,
	      "source logged");
	check_invariants(&f, __func__);
}

/* A changed valid frame is fresh even when the event was missed. */
static void test_rx_after_reorient_changed_without_event(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.go_resp[0] = NULL;
	f.reorient_resp = adapter_frame;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.autonomous && f.gos == 1, "accepted");
	CHECK(count_prefix(&f, "R 0d") == 1 + 11 + 4, "baseline, 11 polls, 4 after reorient");
	CHECK(f.slept == 100 + 20 + 30, "bounded wait after reorient: %u", f.slept);
	check_invariants(&f, __func__);
}

/*
 * A valid but stale frame (the Mac cable's, retained from an earlier
 * exchange) unchanged after the reorient and without an event is not the
 * adapter's: send GetID again, which answers with the adapter's ID.
 */
static void test_stale_retained_frame_not_accepted(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	int i;

	fake_init(&f);
	memcpy(&f.reg[RX], mac_frame, 8);
	f.go_resp[0] = NULL;
	f.go_resp[1] = adapter_frame;
	f.events[3][0] = TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0, "ok");
	CHECK(res.retained_ok, "retained Mac frame is valid");
	CHECK(res.reorient_status == -ESTALE && !res.autonomous, "stale frame refused");
	CHECK(res.id_valid && !memcmp(res.id, adapter_id, 6), "adapter ID from the second GetID");
	CHECK(f.gos == 2 && res.attempts == 2, "second GetID sent");
	/* 100 ms of polls + 50 ms after the reorient: no extra CmdID gap */
	i = find(&f, 0, "U 05 00>02");
	CHECK(i > 0 && find(&f, i, "W 60 01") == i + 12, "retry follows the RX read directly");
	CHECK(f.slept == 100 + 50, "slept %u", f.slept);
	check_invariants(&f, __func__);
}

/* An event proves freshness even if the frame is byte-identical. */
static void test_same_frame_with_event_accepted(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	memcpy(&f.reg[RX], mac_frame, 8);
	f.go_resp[0] = NULL;
	f.events[2][0] = TRISTAR_IDBUS_EV_DIGITAL_ID;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.autonomous && f.gos == 1, "accepted");
	CHECK(!memcmp(res.id, mac_id, 6), "Mac ID");
	check_invariants(&f, __func__);
}

static void test_bad_frame_after_reorient_retries(void)
{
	static const u8 bad_crc[8] = { 0x75, 0x11, 0xf0, 0, 0, 0, 0, 0xd7 };
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.go_resp[0] = NULL;
	f.reorient_resp = bad_crc;
	f.events[2][0] = TRISTAR_IDBUS_EV_DIGITAL_ID;
	f.events[3][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && !res.autonomous && f.gos == 2, "retried");
	CHECK(res.reorient_status == -EBADMSG && !memcmp(res.reorient_frame, bad_crc, 8),
	      "bad frame kept as evidence");
	check_invariants(&f, __func__);
}

/*
 * Regression for the usb-host2 clobber: a full frame sits in 0x20 when our
 * code runs; it must be read intact and stay intact. The old TX path turned
 * 75 11 f0 .. d6 into 74 00 02 .. d6 here.
 */
static void test_clobber_regression(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	memcpy(&f.reg[RX], adapter_frame, 8);
	f.go_resp[0] = NULL;
	f.reorient_resp = adapter_frame;
	f.events[2][0] = TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0, "ok");
	CHECK(!memcmp(res.retained, adapter_frame, 8) && res.retained_ok, "baseline intact");
	CHECK(!memcmp(res.frame, adapter_frame, 8), "frame read intact");
	CHECK(!memcmp(&f.reg[RX], adapter_frame, 8) && !f.clobbered, "RX window never written");
	CHECK(f.reg[0x40] == 0 && f.reg[0x41] == 0, "other generation's registers untouched");
	check_invariants(&f, __func__);

	/* The detector itself: an RX write is caught. */
	fake_init(&f);
	note_write(&f, RX, 3);
	CHECK(f.clobbered, "clobber detector");
}

static void test_timeout_vbus_reorients_once(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = 0x01;
	f.go_resp[0] = NULL;
	f.events[3][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.id_valid, "second GetID ok");
	CHECK(res.reoriented && res.attempts == 2 && f.gos == 2 && f.reorients == 1,
	      "one reorient, two GOs");
	CHECK(res.reorient_status == -EPROTO, "empty RX after reorient refused");
	CHECK(count_prefix(&f, "U 05 01>03") == 1, "reorient keeps bit 0 (cached | 0x02)");
	CHECK(f.slept == 100 + 50, "slept %u", f.slept);
	check_invariants(&f, __func__);
}

static void test_tristar_timeout_event_keeps_cmdid_gap(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	int i;

	fake_init(&f);
	f.go_resp[0] = NULL;
	f.events[1][0] = 0;
	f.events[1][1] = TRISTAR_IDBUS_EV_TIMEOUT;	/* after 10 ms */
	f.events[3][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.reoriented, "ok after reorient");
	i = find(&f, 0, "W 60 01");
	i = find(&f, i + 1, "W 60 01");
	CHECK(i > 0 && !strcmp(f.trace[i - 1], "S 40"), "10 + 50 + 40 >= 100 ms between CmdIDs");
	check_invariants(&f, __func__);
}

static void test_timeout_without_vbus_no_reorient(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_STATUS] = 0x80;
	CHECK(run_identify(&f, &ib, 0, &res) == -ETIMEDOUT, "timeout");
	CHECK(!res.reoriented && res.attempts == 1 && f.gos == 1, "no retry");
	CHECK(count_prefix(&f, "U ") == 0, "no 0x05 write at all");
	check_invariants(&f, __func__);
}

static void test_timeout_twice_stops(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.go_resp[0] = NULL;
	f.go_resp[1] = NULL;
	CHECK(run_identify(&f, &ib, 0, &res) == -ETIMEDOUT, "timeout");
	CHECK(res.reoriented && res.attempts == 2 && f.gos == 2, "exactly one reorient retry");
	CHECK(count_prefix(&f, "U 05") == 1, "one reorient write");
	CHECK(f.slept == 100 + 50 + 100, "bounded: %u ms of sleeps", f.slept);
	check_invariants(&f, __func__);
}

static void test_noreorient_flag(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	CHECK(run_identify(&f, &ib, TRISTAR_IDBUS_NO_REORIENT, &res) == -ETIMEDOUT, "timeout");
	CHECK(!res.reoriented && f.gos == 1 && count_prefix(&f, "U ") == 0, "no reorient");
	check_invariants(&f, __func__);
}

static void test_transfer_error_no_retry(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.events[1][0] = TRISTAR_IDBUS_EV_ERROR;
	CHECK(run_identify(&f, &ib, 0, &res) == -EIO, "transfer error");
	CHECK(f.gos == 1 && !res.reoriented && count_prefix(&f, "BR 20") == 1, "no retry, no RX read");
	/* done + error is an error, not a completion */
	fake_init(&f);
	f.events[1][0] = TRISTAR_IDBUS_EV_DONE | TRISTAR_IDBUS_EV_ERROR;
	CHECK(run_identify(&f, &ib, 0, &res) == -EIO, "0x30 is an error");
	check_invariants(&f, __func__);
}

static void test_detach_event_no_retry(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.events[1][0] = TRISTAR_IDBUS_EV_RESET;
	CHECK(run_identify(&f, &ib, 0, &res) == -ECONNRESET, "detach");
	CHECK(f.gos == 1 && !res.reoriented, "no retry");
	check_invariants(&f, __func__);
}

static void test_bad_frames_kept_not_retried(void)
{
	static const u8 bad_crc[8] = { 0x75, 0x10, 0x0c, 0, 0, 0, 0, 0x67 };
	static const u8 bad_hdr[8] = { 0x74, 0x10, 0x0c, 0, 0, 0, 0, 0x66 };
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.go_resp[0] = bad_crc;
	f.events[1][0] = TRISTAR_IDBUS_EV_DIGITAL_ID;
	CHECK(run_identify(&f, &ib, 0, &res) == -EBADMSG, "bad CRC");
	CHECK(!res.id_valid && res.frame_len == 8 && !memcmp(res.frame, bad_crc, 8),
	      "raw frame still published");
	CHECK(f.gos == 1, "no retry");
	check_invariants(&f, __func__);

	fake_init(&f);
	f.go_resp[0] = bad_hdr;
	f.events[1][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == -EPROTO, "bad header");
	CHECK(f.gos == 1 && res.frame_status == -EPROTO, "no retry");
	check_invariants(&f, __func__);
}

static void test_pin_fault_echo(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	int i;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_FAULT] = 0x05;
	f.events[0][0] = TRISTAR_IDBUS_EV_FAULT;
	f.events[1][0] = TRISTAR_IDBUS_EV_FAULT;
	f.events[1][1] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.id_valid, "ok despite faults");
	CHECK(count_prefix(&f, "U 0a 05>05") == 2, "baseline and poll faults echoed");
	CHECK(ib.faults == 2, "two acknowledgements");
	i = find(&f, 0, "U 0a 05>05");
	CHECK(i > 0 && find(&f, 0, "BR 20 8") < i, "baseline ack after the baseline reads");
	CHECK(strstr(f.log, "pin fault 0x05 acknowledged: ACC1 DP2/DN2") != NULL, "fault names");
	check_invariants(&f, __func__);
}

static void test_wrong_revision_touches_nothing(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus_recover_result rc;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_REVISION] = 0x00;
	CHECK(run_identify(&f, &ib, 0, &res) == -ENODEV, "wrong revision");
	CHECK(f.n == 1 && !strcmp(f.trace[0], "R 0f=00"), "only the revision read");
	for (unsigned int a = 0; a <= TRISTAR_IDBUS_RECOVER_FAULT_ACK; a++) {
		fake_init(&f);
		f.reg[TRISTAR_IDBUS_REG_REVISION] = 0x00;
		CHECK(run_recover(&f, &ib, a, &rc) == -ENODEV && f.n == 1, "recover %u", a);
	}
}

static void test_sticky_reset_fails_closed(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = TRISTAR_IDBUS_CTRL_RESET;
	f.sticky_reset = true;
	CHECK(run_identify(&f, &ib, 0, &res) == -EBUSY, "reset hold stuck");
	CHECK(count_prefix(&f, "BW") == 0 && count_prefix(&f, "W 60") == 0 && f.gos == 0,
	      "no GetID while in reset");
	check_invariants(&f, __func__);
}

static void test_swap_hostid(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.events[1][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, TRISTAR_IDBUS_SWAP_HOSTID, &res) == 0, "ok");
	CHECK(find(&f, 0, "BW 5d 00 02 74 0c") > 0 && count_prefix(&f, "BW 5d 02 00 74") == 0,
	      "manual byte-order variant, reversed in the window");
	check_invariants(&f, __func__);
}

static void test_retained_frame_reported(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	memcpy(&f.reg[RX], mac_frame, 8);
	f.events[1][0] = TRISTAR_IDBUS_EV_DONE;
	CHECK(run_identify(&f, &ib, 0, &res) == 0, "ok");
	CHECK(res.retained_ok && !memcmp(res.retained, mac_frame, 8), "retained frame verified");
	CHECK(strstr(f.log, "baseline RX 0x20 = [75 10 0c 00 00 00 00 66] (retained valid ID frame; may be stale)"),
	      "retained frame logged");
	check_invariants(&f, __func__);
}

/* The busiest identify: reset release, faults, timeout, reorient, retry. */
static void setup_busy(struct fake *f)
{
	fake_init(f);
	f->reg[TRISTAR_IDBUS_REG_CTRL] = TRISTAR_IDBUS_CTRL_RESET | 0x01;
	f->reg[TRISTAR_IDBUS_REG_FAULT] = 0x02;
	f->events[0][0] = TRISTAR_IDBUS_EV_FAULT;
	f->events[1][0] = TRISTAR_IDBUS_EV_FAULT;
	f->go_resp[0] = NULL;
	f->events[3][0] = TRISTAR_IDBUS_EV_DONE;
}

/* An I2C error at any point stops every later write. */
static void test_fail_closed_everywhere(void)
{
	struct tristar_idbus_id_result res;
	struct tristar_idbus ib;
	struct fake f;
	unsigned int total, op;
	int x;

	setup_busy(&f);
	CHECK(run_identify(&f, &ib, 0, &res) == 0 && res.reoriented, "reference run");
	total = f.io_ops;
	CHECK(total > 40, "reference run has %u I/O ops", total);
	for (op = 1; op <= total; op++) {
		setup_busy(&f);
		f.fail_op = op;
		CHECK(run_identify(&f, &ib, 0, &res) == -EIO, "op %u: error propagates", op);
		CHECK(!res.id_valid && res.status == -EIO, "op %u: no ID", op);
		x = find(&f, 0, "X");
		CHECK(x >= 0 && writes_after(&f, x) == 0, "op %u: writes after the error", op);
		CHECK(f.io_ops == op, "op %u: %u I/O ops issued", op, f.io_ops);
	}
}

static void test_power_handshake(void)
{
	static const u8 state[6] = { 0x73, 0x09, 0x02, 0x03, 0x04, 0x00 };
	static const u8 ack[2] = { 0x71, 0x93 };
	static const char *const want[] = {
		"R 0f=8a", "R 0c=83", "R 05=01",
		"W 60 01", "BW 5f 72 08", "BR 5f 2", "W 61 01",
		"R 0d=10", "BR 20 6",
		"W 60 01", "BW 5d 02 01 70 00", "BR 5d 4", "W 61 05",
		"R 0d=00", "S 10", "R 0d=10", "BR 20 2",
	};
	struct tristar_idbus_ph_result res;
	struct tristar_idbus ib;
	u8 frame[6];
	struct fake f;

	memcpy(frame, state, 6);
	frame[5] = tristar_idbus_crc8(frame, 5);
	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = 0x01;
	f.go_resp[0] = frame;
	f.go_resp_len[0] = 6;
	f.go_resp[1] = ack;
	f.go_resp_len[1] = 2;
	f.events[1][0] = TRISTAR_IDBUS_EV_DONE;
	f.events[2][0] = 0;
	f.events[2][1] = TRISTAR_IDBUS_EV_DONE;
	ib_init(&ib, &f);
	CHECK(tristar_idbus_power_handshake(&ib, &res) == 0, "handshake ok");
	CHECK(res.state_ok && res.ack_ok && res.request[1] == 0x01 && res.request[2] == 0x02,
	      "0x70 frame {70, state[1] & ~8, state[2]}");
	expect_trace(&f, want, ARRAY_SIZE(want), __func__);
	check_invariants(&f, __func__);

	/* Generic transfers complete only on (0x0d & 0x38) == 0x10. */
	fake_init(&f);
	f.go_resp[0] = frame;
	f.go_resp_len[0] = 6;
	f.events[1][0] = TRISTAR_IDBUS_EV_DIGITAL_ID;
	ib_init(&ib, &f);
	CHECK(tristar_idbus_power_handshake(&ib, &res) == -ETIMEDOUT, "0x08 is not a 0x72 completion");

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_STATUS] = 0x00;
	ib_init(&ib, &f);
	CHECK(tristar_idbus_power_handshake(&ib, &res) == -ENODEV, "VBUS-gated");
	CHECK(writes_after(&f, 0) == 0, "nothing sent");

	fake_init(&f);
	f.go_resp[0] = frame;
	f.go_resp_len[0] = 6;
	ib_init(&ib, &f);
	CHECK(tristar_idbus_power_handshake(&ib, &res) == -ETIMEDOUT, "no reply");
	CHECK(f.gos == 1 && count_prefix(&f, "U") == 0, "no retry, no reorient");
	check_invariants(&f, __func__);
}

/* ---- link recovery (9.6) ----------------------------------------- */

static void test_recover_reorient(void)
{
	static const char *const want[] = {
		"R 0f=8a", "R 0c=83", "R 05=00", "BR 20 8",
		"U 05 00>02", "R 05=00", "S 20", "R 0d=18",
		"BR 20 8", "R 0c=83", "R 0c=83",
	};
	struct tristar_idbus_recover_result rc;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	memcpy(&f.reg[RX], mac_frame, 8);
	f.reorient_resp = mac_frame;
	f.events[1][0] = TRISTAR_IDBUS_EV_DIGITAL_ID | TRISTAR_IDBUS_EV_DONE;
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_REORIENT, &rc) == 0, "reorient done");
	CHECK(rc.frame_read && rc.frame_status == 0 && rc.events == 0x18, "frame consumed");
	CHECK(rc.status_before == 0x83 && rc.status_after == 0x83, "0x0c before and after");
	CHECK(strstr(f.log, "0x0c before: 0x83") && strstr(f.log, "0x0c after: 0x83"), "0x0c logged");
	expect_trace(&f, want, ARRAY_SIZE(want), __func__);
	check_invariants(&f, __func__);

	/* Without an event: 50 ms at most, still read only. */
	fake_init(&f);
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_REORIENT, &rc) == 0, "no event is fine");
	CHECK(f.slept == 50 && count_prefix(&f, "R 0d") == 4, "bounded: %u ms", f.slept);
	CHECK(writes_after(&f, 0) == 1, "the reorient is the only write");
	check_invariants(&f, __func__);

	/* Refused while the reset hold is latched. */
	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = TRISTAR_IDBUS_CTRL_RESET;
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_REORIENT, &rc) == -EBUSY, "refused");
	CHECK(writes_after(&f, 0) == 0, "nothing written");
}

static void test_recover_idbus_reset(void)
{
	static const char *const want[] = {
		"R 0f=8a", "R 0c=83",
		"U 05 01>09", "S 500", "U 05 09>01", "R 05=01", "S 20",
		"R 0c=83",
	};
	struct tristar_idbus_recover_result rc;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_CTRL] = 0x01;	/* VBUS sink kept across the pulse */
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_RESET, &rc) == 0, "pulse done");
	CHECK(f.reg[TRISTAR_IDBUS_REG_CTRL] == 0x01, "released");
	expect_trace(&f, want, ARRAY_SIZE(want), __func__);
	check_invariants(&f, __func__);

	fake_init(&f);
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_RESET, &rc) == 0, "pulse from 0x00");
	CHECK(find(&f, 0, "U 05 00>08") > 0 && find(&f, 0, "U 05 08>00") > 0, "0x08, then 0x00");

	fake_init(&f);
	f.sticky_reset = true;
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_RESET, &rc) == -EBUSY, "stuck hold reported");
}

/* Once the hold may be set, the release goes out even after an I/O error. */
static void test_recover_idbus_reset_always_released(void)
{
	struct tristar_idbus_recover_result rc;
	struct tristar_idbus ib;
	struct fake f;
	unsigned int total, op, landed;
	int set, rel, x;

	fake_init(&f);
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_RESET, &rc) == 0, "reference");
	total = f.io_ops;
	for (landed = 0; landed < 2; landed++) {
		for (op = 1; op <= total; op++) {
			fake_init(&f);
			f.fail_op = op;
			f.fail_after_effect = landed;
			CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_RESET, &rc) == -EIO,
			      "op %u/%u: error reported", op, landed);
			x = find(&f, 0, "X");
			set = find(&f, 0, "U 05 00>08");
			/* A release: any 0x05 update after the set that clears bit 3 */
			rel = -1;
			for (int i = set + 1; set >= 0 && i < (int)f.n; i++) {
				unsigned int r, a, b;

				if (sscanf(f.trace[i], "U %x %x>%x", &r, &a, &b) == 3 && r == 5 &&
				    !(b & TRISTAR_IDBUS_CTRL_RESET))
					rel = i;
			}
			if (op <= 2) {
				/* Revision/0x0c read failed: nothing written at all. */
				CHECK(writes_after(&f, 0) == 0, "op %u/%u: no write", op, landed);
				continue;
			}
			CHECK(f.reg[TRISTAR_IDBUS_REG_CTRL] == 0,
			      "op %u/%u: hold released (0x05 = 0x%02x)", op, landed,
			      f.reg[TRISTAR_IDBUS_REG_CTRL]);
			if (set >= 0)
				CHECK(rel > set, "op %u/%u: release after the set", op, landed);
			CHECK(x >= 0 && writes_after(&f, x) <= 1,
			      "op %u/%u: only the release after the error", op, landed);
		}
	}
}

static void test_recover_fault_ack(void)
{
	static const char *const want[] = {
		"R 0f=8a", "R 0c=83", "R 0a=05", "W 0a 05", "R 0a=05", "R 0c=83",
	};
	struct tristar_idbus_recover_result rc;
	struct tristar_idbus ib;
	struct fake f;

	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_FAULT] = 0x05;
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_FAULT_ACK, &rc) == 0 && rc.fault == 0x05,
	      "fault acknowledged");
	CHECK(strstr(f.log, "pin fault 0x05 written back: ACC1 DP2/DN2") != NULL, "names");
	expect_trace(&f, want, ARRAY_SIZE(want), __func__);
	check_invariants(&f, __func__);

	/* Bits 4..7 alone are not pin faults: nothing written. */
	fake_init(&f);
	f.reg[TRISTAR_IDBUS_REG_FAULT] = 0x30;
	CHECK(run_recover(&f, &ib, TRISTAR_IDBUS_RECOVER_FAULT_ACK, &rc) == 0, "ok");
	CHECK(writes_after(&f, 0) == 0, "no write without a latched pin fault");

	fake_init(&f);
	CHECK(run_recover(&f, &ib, 7, &rc) == -EINVAL && f.n == 0, "unknown action: no I/O");
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--crc")) {
		u8 buf[64];
		unsigned int len = 0, byte;
		const char *p = argv[2];

		while (len < sizeof(buf) && sscanf(p, "%2x", &byte) == 1) {
			buf[len++] = byte;
			p += 2;
		}
		printf("%02x\n", tristar_idbus_crc8(buf, len));
		return 0;
	}
	test_crc();
	test_decode();
	test_identify_exact_order();
	test_completion_codes();
	test_readback_mismatch_no_go();
	test_rx_after_reorient_accepted();
	test_rx_after_reorient_changed_without_event();
	test_stale_retained_frame_not_accepted();
	test_same_frame_with_event_accepted();
	test_bad_frame_after_reorient_retries();
	test_clobber_regression();
	test_timeout_vbus_reorients_once();
	test_tristar_timeout_event_keeps_cmdid_gap();
	test_timeout_without_vbus_no_reorient();
	test_timeout_twice_stops();
	test_noreorient_flag();
	test_transfer_error_no_retry();
	test_detach_event_no_retry();
	test_bad_frames_kept_not_retried();
	test_pin_fault_echo();
	test_wrong_revision_touches_nothing();
	test_sticky_reset_fails_closed();
	test_swap_hostid();
	test_retained_frame_reported();
	test_fail_closed_everywhere();
	test_power_handshake();
	test_recover_reorient();
	test_recover_idbus_reset();
	test_recover_idbus_reset_always_released();
	test_recover_fault_ack();
	if (failures) {
		printf("FAIL %d check(s)\n", failures);
		return 1;
	}
	printf("PASS Tristar ID-bus core: CRC8, decode, TX window 0x5d..0x61, read-only RX, reorient RX, recovery, fail-closed\n");
	return 0;
}
