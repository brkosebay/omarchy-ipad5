// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Run the kernel's own CT819 lux computation (apple_als_calc_lux in
 * drivers/iio/light/tsl2583.h) on the host, against an LSCI blob, and print a
 * table that scripts/sensors/test-ct819-lux.py compares with an independent
 * implementation of the formula in docs/periph/ds-als-hall.md.
 *
 *   ct819-lux-host-test LSCI.bin     one line per case:
 *                                    cycles gain ch0 ch1 lux_max trim -> lux
 *
 * The blob is parsed the way apple_als_load_calibration() does (header,
 * checksum, record checks, gain-ratio folding).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux/kernel.h"
#include "tsl2583.h"

#define HEADER_SIZE 8
#define RECORD_SIZE 0x40
#define RATIO_OFF 0x30
#define RATIO_SCALE CT819_CAL_RATIO_SCALE

static u16 le16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int load(const char *path, struct apple_als_calibration *cal, u32 *cycle_us)
{
	u8 data[512];
	FILE *f = fopen(path, "rb");
	size_t len;
	u32 checksum = 0;
	const u8 *record;
	int i;

	if (!f)
		return -1;
	len = fread(data, 1, sizeof(data), f);
	fclose(f);
	if (len < HEADER_SIZE + RECORD_SIZE || (len & 1) || data[1] != 2 ||
	    data[2] != 1 || data[3] != HEADER_SIZE || le16(data + 4) != len) {
		fprintf(stderr, "bad LSCI header\n");
		return -1;
	}
	for (i = 0; i < (int)len; i += 2)
		checksum += le16(data + i);
	if ((u16)checksum != 0xffff) {
		fprintf(stderr, "bad LSCI checksum\n");
		return -1;
	}
	record = data + data[3];
	if (record[0] || record[3] != RECORD_SIZE || record[4] != 1) {
		fprintf(stderr, "unsupported record\n");
		return -1;
	}
	for (i = 0x2a; i < 0x30; i++)
		if (record[i]) {
			fprintf(stderr, "backlight leakage not supported\n");
			return -1;
		}

	memset(cal, 0, sizeof(*cal));
	cal->lux_fudge = le16(record + 0x08);
	cal->integration_time = le16(record + 0x0a);
	cal->gain = 0;
	for (i = 0; i < 4; i++) {
		static const u8 nominal[4] = { 1, 8, 16, 128 };
		if (nominal[i] == record[0x10])
			cal->gain = i;
	}
	cal->a_coeff1 = le32(record + 0x14);
	cal->b_coeff1 = le32(record + 0x18);
	cal->upper_range = le32(record + 0x1c);
	cal->a_coeff2 = le32(record + 0x20);
	cal->b_coeff2 = le32(record + 0x24);
	cal->ch0_dark_counts = record[0x28];
	cal->ch1_dark_counts = record[0x29];
	cal->gain_factor[0][0] = cal->gain_factor[0][1] = APPLE_ALS_CAL_SCALE;
	for (i = 1; i < 4; i++) {
		const u8 *r = record + RATIO_OFF + (i - 1) * 4;

		if (apple_als_calculate_gain_factor(cal->gain_factor[i - 1][0], le16(r),
						    RATIO_SCALE, &cal->gain_factor[i][0]) ||
		    apple_als_calculate_gain_factor(cal->gain_factor[i - 1][1], le16(r + 2),
						    RATIO_SCALE, &cal->gain_factor[i][1])) {
			fprintf(stderr, "bad gain ratios\n");
			return -1;
		}
	}
	*cycle_us = 2700;
	return 0;
}

/*
 * The expectations of the KUnit cases in drivers/iio/light/tsl2583-test.c,
 * checked against the same function, since KUnit only runs when booted.
 */
static int kunit_cases(void)
{
	static const struct apple_als_calibration cal = {
		.lux_fudge = 204, .integration_time = 400, .gain = 1,
		.a_coeff1 = 65536, .b_coeff1 = 131072, .upper_range = 26214,
		.a_coeff2 = 52429, .b_coeff2 = 78643,
		.gain_factor = { { 65536, 65536 }, { 514560, 514560 },
				 { 1047210, 1047210 }, { 7616816, 7616816 } },
	};
	static const struct { u32 cycles; unsigned int gain; u32 max; u16 ch0, ch1; int want; } t[] = {
		{ 148, 1, 5000, 1000, 100, 640 },
		{ 148, 1, 5000, 1000, 500, 160 },
		{ 148, 1, 5000, 100, 1000, 0 },
		{ 148, 1, 5000, 0, 0, 0 },
		{ 148, 1, 5000, 1480, 148, 947 },
		{ 18, 1, 5000, 180, 18, 947 },
		{ 148, 2, 5000, 2035, 204, 640 },
		{ 148, 1, 5000, 65535, 100, 5000 },
		{ 148, 1, 5000, 1000, 65535, 5000 },
		{ 18, 1, 1000000, 18431, 0, 1000000 },
		{ 148, 1, 65535, 60000, 0, 48000 },
		{ 148, 1, 5000, 60000, 0, 5000 },
		{ 0, 1, 5000, 100, 10, -EINVAL },
		{ 148, 8, 5000, 100, 10, -EINVAL },
	};
	struct apple_als_calibration dark = cal;
	int bad = 0, got;
	size_t i;

	for (i = 0; i < sizeof(t) / sizeof(*t); i++) {
		got = apple_als_calc_lux(&cal, t[i].gain, t[i].cycles, 2700, CT819_COUNTS_PER_CYCLE,
					 1000, t[i].max, t[i].ch0, t[i].ch1);
		if (got != t[i].want) {
			printf("KUnit case %zu: got %d, want %d\n", i, got, t[i].want);
			bad++;
		}
	}
	got = apple_als_calc_lux(&cal, 1, 18, 2700, CT819_COUNTS_PER_CYCLE, 1000, 1000000, 18430, 0);
	if (got == 1000000) {
		printf("KUnit: 18430 counts at 18 cycles reads as saturated\n");
		bad++;
	}
	dark.ch0_dark_counts = 4;
	dark.ch1_dark_counts = 2;
	if (apple_als_calc_lux(&dark, 1, 148, 2700, CT819_COUNTS_PER_CYCLE, 1000, 5000, 1004, 102) != 640 ||
	    apple_als_calc_lux(&dark, 1, 148, 2700, CT819_COUNTS_PER_CYCLE, 1000, 5000, 3, 2) != 0) {
		printf("KUnit: dark counts\n");
		bad++;
	}
	got = apple_als_calc_lux(&cal, 1, 18, 2700, 0, 1000, 65535, 18431, 0);
	if (got != 65535) {
		printf("KUnit: 16 bit limit only: got %d\n", got);
		bad++;
	}
	printf("%d KUnit expectations differ\n", bad);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct apple_als_calibration cal;
	/* integration cycles the driver can program: 18 (50 ms) .. 240 (650 ms) */
	static const u32 cycles[] = { 18, 37, 74, 148, 240 };
	static const u32 chans[] = { 0, 1, 2, 5, 10, 50, 100, 300, 1000, 3000, 10000,
				     20000, 40000, 65000, 65534, 65535 };
	u32 cycle_us;
	size_t c, g, a, b;

	if (argc == 2 && !strcmp(argv[1], "--kunit-cases"))
		return kunit_cases();
	if (argc != 2 || load(argv[1], &cal, &cycle_us))
		return 2;

	printf("# fudge %u time %u gain %u a1 %d b1 %d upper %d a2 %d b2 %d dark %u %u\n",
	       cal.lux_fudge, cal.integration_time, cal.gain, cal.a_coeff1,
	       cal.b_coeff1, cal.upper_range, cal.a_coeff2, cal.b_coeff2,
	       cal.ch0_dark_counts, cal.ch1_dark_counts);
	for (g = 0; g < 4; g++)
		printf("# gain_factor[%zu] %u %u\n", g, cal.gain_factor[g][0],
		       cal.gain_factor[g][1]);
	for (c = 0; c < sizeof(cycles) / sizeof(*cycles); c++)
		for (g = 0; g < 4; g++)
			for (a = 0; a < sizeof(chans) / sizeof(*chans); a++)
				for (b = 0; b < sizeof(chans) / sizeof(*chans); b++) {
					int lux = apple_als_calc_lux(&cal, g, cycles[c], cycle_us,
								     CT819_COUNTS_PER_CYCLE, 1000,
								     5000, chans[a], chans[b]);
					printf("%u %zu %u %u 5000 1000 -> %d\n", cycles[c], g,
					       chans[a], chans[b], lux);
				}
	return 0;
}
