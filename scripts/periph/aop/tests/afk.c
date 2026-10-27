/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host harness for a9-aop-afk.h: management/AFK words, ring init/read/write
 * including wrap and the three advance rules, EPIC property packets, ready
 * reports, the firmware tag table (real image) and the calibration file.
 * Usage: afk-test <ipad6faop-20H350.bin> <accel-calibration.bin or ->
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-afk.h"

static u8 *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	u8 *data;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(n ? n : 1);
	assert(data && fread(data, 1, n, f) == (size_t)n);
	fclose(f);
	*size = n;
	return data;
}

static void test_words(void)
{
	struct a9_epmap map;
	u64 word;
	u16 tag;
	size_t size;

	assert(!a9_mgmt_epmap(0x0088000100000003ULL, &map));
	assert(map.base == 1 && map.last && map.bitmap == 3);
	assert(a9_mgmt_epmap_reply(&map) == 0x0088000100000000ULL);
	assert(!a9_mgmt_epmap(0x0080000000000f01ULL, &map));
	assert(map.base == 0 && !map.last && map.bitmap == 0xf01);
	assert(a9_mgmt_epmap_reply(&map) == 0x0080000000000001ULL);
	assert(!a9_mgmt_epmap(0x0080003f00000000ULL, &map) && map.base == 0x3f);
	assert(a9_mgmt_epmap(0x0010000000000000ULL, &map) == -EPROTO);
	assert(a9_mgmt_startep(0x20) == 0x0050002000000002ULL);
	assert(a9_mgmt_ping(7, 0x1234) == 0x0030123400000007ULL);
	assert(a9_mgmt_ap_power(true) == 0x00a0000000000001ULL);
	assert(a9_aop_system_endpoint(0) && a9_aop_system_endpoint(1) &&
	       a9_aop_system_endpoint(0x1f) && !a9_aop_system_endpoint(0x20) &&
	       !a9_aop_system_endpoint(0x21));
	assert(a9_mgmt_type(0x0070000000000020ULL) == A9_MGMT_IOP_POWER_ACK);
	assert(a9_afk_type(0x0080000000000008ULL) == A9_AFK_INIT);
	assert(!a9_afk_getbuf(0x0089000000100042ULL, &tag, &size));
	assert(tag == 0x42 && size == 0x10 * 64);
	assert(a9_afk_getbuf(0x0089000000000042ULL, &tag, &size) == -EMSGSIZE);
	assert(a9_afk_getbuf(0x008a000000100042ULL, &tag, &size) == -EPROTO);
	assert(!a9_afk_getbuf_ack(0x8123456780ULL, &word));
	assert(word == 0x00a1008123456780ULL);
	assert(a9_afk_getbuf_ack(1ULL << 48, &word) == -ERANGE);
	assert(a9_afk_word(A9_AFK_DOORBELL) == 0x00a2000000000000ULL);
	assert(a9_afk_word(A9_AFK_SHUTDOWN) == 0x00a4000000000000ULL);
}

/* What the AOP writes before announcing a ring: bufsize, v7, pointers. */
static void aop_header(u8 *base, u32 size, u32 version, u32 rptr, u32 wptr)
{
	memset(base, 0, A9_RING_HEADER);
	put_unaligned_le32(size - A9_RING_HEADER, base);
	put_unaligned_le32(version, base + 4);
	put_unaligned_le32(rptr, base + 0x40);
	put_unaligned_le32(wptr, base + 0x80);
}

static void test_rings(void)
{
	u8 memory[0x400], packet[256], epic[100], copy[16];
	struct a9_ring_entry entry;
	struct a9_ring tx, rx;
	u32 next, i, bufsize = sizeof(memory) - A9_RING_HEADER;

	for (i = 0; i < sizeof(epic); i++)
		epic[i] = i;
	epic[0] = 2;

	/* Header is the AOP's: checked, never written. */
	memset(memory, 0xee, sizeof(memory));
	assert(a9_ring_attach(&tx, memory, 0x100 - 1) == -EINVAL);
	assert(a9_ring_attach(&tx, memory, 0xc0) == -EINVAL);
	aop_header(memory, sizeof(memory) - 0x40, 0x00070006, 0, 0);
	assert(a9_ring_attach(&tx, memory, sizeof(memory)) == -EPROTO);
	aop_header(memory, sizeof(memory), 0x00060006, 0, 0);
	assert(a9_ring_attach(&tx, memory, sizeof(memory)) == -EPROTONOSUPPORT);
	aop_header(memory, sizeof(memory), 0x00070006, 0, 0x44);
	assert(a9_ring_attach(&tx, memory, sizeof(memory)) == -ERANGE);
	aop_header(memory, sizeof(memory), 0x00070006, 0x80, 0x80);
	assert(!a9_ring_attach(&rx, memory, sizeof(memory)) && rx.cursor == 0x80);
	assert(rx.bufsize == bufsize && rx.version == 0x00070006);
	aop_header(memory, sizeof(memory), 0x00070006, 0, 0);
	assert(!a9_ring_attach(&tx, memory, sizeof(memory)));
	assert(get_unaligned_le32(memory + 4) == 0x00070006 && memory[0xc0] == 0xee);

	/* Loopback: host writer then host reader over the same memory. */
	rx = tx;
	assert(a9_ring_read(&rx, 0, packet, sizeof(packet), &entry, &next) == -EAGAIN);
	assert(!a9_ring_write(&tx, 0, 0, 0x55, 0, epic, 56, &next));
	assert(next == 0x80);	/* align(16 + 56) */
	assert(get_unaligned_le32(memory + 0xc0) == A9_RING_MAGIC_AOP);
	assert(get_unaligned_le32(memory + 0xc4) == 56);
	assert(get_unaligned_le32(memory + 0xc8) == 0x55);
	assert(get_unaligned_le32(memory + 0xcc) == 0);
	assert(!memcmp(memory + 0xd0, epic, 56));
	for (i = 0xd0 + 56; i < 0xc0 + 0x80; i++)
		assert(memory[i] == 0);	/* zero padding to the stride */
	a9_ring_set_wptr(&tx, next);
	assert(!a9_ring_read(&rx, a9_ring_wptr(&tx), packet, sizeof(packet), &entry, &next));
	assert(next == 0x80 && entry.length == 56 && entry.channel == 0x55 &&
	       entry.magic == A9_RING_MAGIC_AOP && entry.offset == 0 &&
	       !memcmp(packet, epic, 56));

	/* Wrap: entry goes to 0, header copy stays at the old wptr. */
	assert(!a9_ring_write(&tx, bufsize - 0x40, bufsize - 0x40, 7, 9, epic, 100, &next));
	assert(next == 0x80);
	assert(get_unaligned_le32(memory + 0xc4) == 100);
	memcpy(copy, memory + 0xc0, 16);
	assert(!memcmp(memory + 0xc0 + bufsize - 0x40, copy, 16));
	rx.cursor = bufsize - 0x40;
	assert(!a9_ring_read(&rx, next, packet, sizeof(packet), &entry, &next));
	assert(entry.offset == 0 && entry.length == 100 && entry.channel == 7 &&
	       entry.word == 9 && next == 0x80 && !memcmp(packet, epic, 100));
	/* A tail header that claims to wrap past the writer is rejected. */
	rx.cursor = bufsize - 0x40;
	assert(a9_ring_read(&rx, bufsize - 0x40 + 0x40 - 0x40, packet, sizeof(packet),
			    &entry, &next) == -EAGAIN);
	put_unaligned_le32(0x40, memory + 0x80);
	rx.cursor = 0;
	put_unaligned_le32(200, memory + 0xc4);	/* stride 0x100 > wptr 0x40 */
	assert(a9_ring_read(&rx, 0x40, packet, sizeof(packet), &entry, &next) == -EPROTO);

	/* Ending exactly at bufsize returns cursor 0. */
	tx.cursor = 0;
	assert(!a9_ring_write(&tx, 0x40, bufsize - 0x40, 1, 0, epic, 48, &next) && next == 0);

	/* Space rules from fffffff005bab540. */
	assert(a9_ring_write(&tx, 0x40, 0, 0, 0, epic, 48, &next) == -ENOSPC);
	assert(a9_ring_write(&tx, 0, bufsize - 0x40, 0, 0, epic, 48, &next) == -ENOSPC);
	assert(a9_ring_write(&tx, 0, bufsize - 0x40, 0, 0, epic, 56, &next) == -ENOSPC);
	assert(a9_ring_write(&tx, 0x41, 0, 0, 0, epic, 48, &next) == -ERANGE);
	assert(a9_ring_write(&tx, 0, 0, 0, 0, epic, 0, &next) == -EMSGSIZE);
	assert(!a9_ring_write(&tx, 0x100, 0, 0, 0, epic, 48, &next) && next == 0x40);

	/* Malformed entries. */
	aop_header(memory, sizeof(memory), 0x00070006, 0, 0);
	assert(!a9_ring_attach(&rx, memory, sizeof(memory)));
	memset(memory + 0xc0, 0, 16);
	assert(a9_ring_read(&rx, 0x40, packet, sizeof(packet), &entry, &next) == -EMSGSIZE);
	put_unaligned_le32(bufsize, memory + 0xc4);
	assert(a9_ring_read(&rx, 0x40, packet, sizeof(packet), &entry, &next) == -EMSGSIZE);
	assert(a9_ring_read(&rx, 0x44, packet, sizeof(packet), &entry, &next) == -ERANGE);
	assert(a9_ring_read(&rx, bufsize, packet, sizeof(packet), &entry, &next) == -ERANGE);
	put_unaligned_le32(0x30, memory + 0xc4);
	put_unaligned_le32(0x12345678, memory + 0xc0);	/* iOS does not check magic */
	assert(!a9_ring_read(&rx, 0x40, packet, sizeof(packet), &entry, &next));
	assert(entry.magic == 0x12345678 && next == 0x40);
	/* Small copy window clamps; the subpacket walk bounds the rest. */
	assert(!a9_ring_read(&rx, 0x40, packet, 20, &entry, &next) && entry.length == 0x30);
}

static void test_ring_fuzz(void)
{
	u8 memory[0x300], packet[128], epic[64] = { 2 };
	unsigned int seed = 99, trial, i;
	struct a9_ring_entry entry;
	struct a9_ring ring;
	u32 next, wptr, rptr;
	int ret;

	for (trial = 0; trial < 50000; trial++) {
		aop_header(memory, sizeof(memory), 0x00070006, 0, 0);
		assert(!a9_ring_attach(&ring, memory, sizeof(memory)));
		for (i = 0xc0; i < sizeof(memory); i++) {
			seed = seed * 1103515245U + 12345U;
			memory[i] = seed >> 16;
		}
		ring.cursor = ((seed >> 4) % 9) * 0x40;
		wptr = ((seed >> 12) % 10) * 0x40;
		ret = a9_ring_read(&ring, wptr, packet, sizeof(packet), &entry, &next);
		if (!ret)
			assert(next < ring.bufsize && !(next % 0x40) &&
			       entry.offset + a9_ring_align(16 + entry.length) <= ring.bufsize);
		rptr = ((seed >> 20) % 9) * 0x40;
		ret = a9_ring_write(&ring, rptr, wptr % ring.bufsize, 0, 0, epic,
				    1 + (seed >> 8) % 63, &next);
		if (!ret)
			assert(next < ring.bufsize && !(next % 0x40));
	}
}

static void test_epic(void)
{
	u8 packet[128], value[4] = { 0xa0, 0x86, 0x01, 0x00 };
	struct a9_spu_subpacket sub;
	struct a9_spu_service service;
	size_t cursor = 16;
	int length;

	length = a9_epic_property(packet, sizeof(packet), 0x1234, 0x77,
				  0x16, value, 4);
	assert(length == 48 + 4);
	assert(packet[0] == 2 && a9_epic_sequence(packet) == 0x1234 &&
	       packet[3] == 0 && get_unaligned_le32(packet + 4) == 0);
	assert(get_unaligned_le64(packet + 8) == 0);
	assert(a9_spu_next(packet, length, &cursor, &sub) == 1 && cursor == (size_t)length);
	assert(sub.category == A9_SPU_NOTIFY && sub.subtype == 4 && sub.tag == 0x77);
	assert(sub.size == 12 && !sub.timestamp);
	assert(get_unaligned_le32(packet + 16 + 20) == 0xe4);
	assert(get_unaligned_le32(sub.payload) == 0 &&
	       get_unaligned_le32(sub.payload + 4) == 0x16 &&
	       get_unaligned_le32(sub.payload + 8) == 100000);
	assert(a9_epic_property(packet, 51, 0, 0, 0, value, 4) == -EMSGSIZE);

	/* Ready report with interface number (flags bit 3). */
	memset(packet, 0, sizeof(packet));
	sub.category = 0;
	sub.subtype = 0xc0;
	sub.payload = packet;
	sub.size = 44;
	memcpy(packet, "accel", 5);
	put_unaligned_le32(0x10000007, packet + 32);
	put_unaligned_le32(8, packet + 36);
	put_unaligned_le32(3, packet + 40);
	assert(!a9_spu_service(&sub, &service));
	assert(!strcmp(service.name, "accel") && service.service_id == 0x10000007 &&
	       service.has_interface && service.interface == 3);
	sub.size = 40;
	assert(a9_spu_service(&sub, &service) == -EMSGSIZE);
	put_unaligned_le32(0, packet + 36);
	assert(!a9_spu_service(&sub, &service) && !service.has_interface);
	sub.size = 39;
	assert(a9_spu_service(&sub, &service) == -EMSGSIZE);
}

static void test_firmware(const char *path)
{
	size_t size, table, length, value;
	u32 value_size;
	u8 *fw = slurp(path, &size);

	assert(fw && size == 342336);
	assert(!a9_fw_table(fw, size, &table, &length));
	assert(table == 0x389b8 && length == 0x1df);
	assert(!a9_fw_tag(fw, table, length, A9_FW_TAG('R', 'T', 'S', 'Z'), &value, &value_size));
	assert(value_size == 4 && get_unaligned_le32(fw + value) == 0x6e000);
	assert(!a9_fw_tag(fw, table, length, A9_FW_TAG('A', 'O', 'P', 't'), &value, &value_size));
	assert(value_size == 4 && get_unaligned_le32(fw + value) == 0);
	assert(!a9_fw_tag(fw, table, length, A9_FW_TAG('E', 'C', '0', 'p'), &value, &value_size));
	assert(value_size == 8);
	assert(a9_fw_tag(fw, table, length, A9_FW_TAG('N', 'O', 'P', 'E'), &value, &value_size) == -ENOENT);
	/* Every tag the driver patches exists with the size it writes. */
	{
		static const struct { u32 tag, size; } patched[] = {
			{ A9_FW_TAG('A', 'O', 'P', 't'), 4 }, { A9_FW_TAG('n', 'C', 'a', 'l'), 1 },
			{ A9_FW_TAG('E', 'C', '0', 'p'), 8 }, { A9_FW_TAG('W', 'r', 'A', 'd'), 8 },
			{ A9_FW_TAG('M', 'c', 'R', 'A'), 4 }, { A9_FW_TAG('S', 'O', 'C', '_'), 4 },
			{ A9_FW_TAG('S', 'O', 'C', 'R'), 4 }, { A9_FW_TAG('D', 'E', 'V', 'f'), 1 },
			{ A9_FW_TAG('S', 'T', 'K', 'G'), 4 }, { A9_FW_TAG('P', 'R', 'N', 'G'), 32 },
			{ A9_FW_TAG('R', 'T', 'T', 'O'), 8 },
		};
		u8 eight[8] = { 8 }, before[16];
		unsigned int k;

		for (k = 0; k < sizeof(patched) / sizeof(patched[0]); k++) {
			assert(!a9_fw_tag(fw, table, length, patched[k].tag, &value, &value_size));
			assert(value_size == patched[k].size);
		}
		assert(!a9_fw_tag(fw, table, length, A9_FW_TAG('A', 'O', 'P', 't'), &value, &value_size));
		memcpy(before, fw + value - 8, 16);
		assert(!a9_fw_patch(fw, table, length, A9_FW_TAG('A', 'O', 'P', 't'), eight, 4));
		assert(get_unaligned_le32(fw + value) == 8);
		assert(!memcmp(fw + value - 8, before, 8));	/* tag/len untouched */
		assert(a9_fw_patch(fw, table, length, A9_FW_TAG('A', 'O', 'P', 't'), eight, 8) == -EMSGSIZE);
		assert(a9_fw_patch(fw, table, length, A9_FW_TAG('E', 'C', '0', 'v'), eight, 4) == -ENOENT);
		assert(!a9_fw_table(fw, size, &table, &length));
	}
	assert(a9_fw_table(fw, 0x4f, &table, &length) == -EINVAL);
	assert(a9_fw_table(fw, table + length - 1, &table, &length) == -EMSGSIZE);
	put_unaligned_le32(0x1de, fw + 0x4c);	/* walk no longer ends exactly */
	assert(a9_fw_table(fw, size, &table, &length) == -EPROTO ||
	       a9_fw_table(fw, size, &table, &length) == -EMSGSIZE);
	put_unaligned_le32(0x1df, fw + 0x4c);
	put_unaligned_le32(0x1000, fw + 0x389b8 + 4);	/* oversized first value */
	assert(a9_fw_table(fw, size, &table, &length) == -EMSGSIZE);
	fw[0x20] ^= 1;
	assert(a9_fw_table(fw, size, &table, &length) == -EINVAL);
	free(fw);
}

static void test_calibration(const char *path)
{
	struct a9_cal_entry entries[5];
	u8 blob[64];
	size_t size;
	u8 *real;
	int count;

	memcpy(blob, "A9AC", 4);
	put_unaligned_le32(1, blob + 4);
	put_unaligned_le32(2, blob + 8);
	put_unaligned_le32(0x0b, blob + 12);
	put_unaligned_le32(2, blob + 16);
	blob[20] = 1; blob[21] = 2;
	put_unaligned_le32(0x18, blob + 22);
	put_unaligned_le32(3, blob + 26);
	blob[30] = 3; blob[31] = 4; blob[32] = 5;
	assert(a9_cal_parse(blob, 33, entries, 5) == 2);
	assert(entries[0].key == 0x0b && entries[0].length == 2 && entries[0].data[1] == 2);
	assert(entries[1].key == 0x18 && entries[1].data[2] == 5);
	assert(a9_cal_parse(blob, 34, entries, 5) == -EPROTO);	/* trailing */
	assert(a9_cal_parse(blob, 32, entries, 5) == -EMSGSIZE);
	assert(a9_cal_parse(blob, 33, entries, 1) == -E2BIG);
	put_unaligned_le32(0x1a, blob + 22);	/* LTAO after ASCl: order */
	assert(a9_cal_parse(blob, 33, entries, 5) == -EPROTO);
	put_unaligned_le32(0x4b, blob + 22);	/* flagged entry: not allowed */
	assert(a9_cal_parse(blob, 33, entries, 5) == -EPROTO);
	put_unaligned_le32(0x18, blob + 22);
	put_unaligned_le32(0, blob + 16);
	assert(a9_cal_parse(blob, 31, entries, 5) == -EPROTO);
	blob[0] = 'X';
	assert(a9_cal_parse(blob, 33, entries, 5) == -EINVAL);
	if (strcmp(path, "-")) {
		real = slurp(path, &size);
		assert(real);
		count = a9_cal_parse(real, size, entries, 5);
		assert(count == 4 && entries[0].key == 0x1a && entries[0].length == 20 &&
		       entries[1].key == 0x0b && entries[1].length == 36 &&
		       entries[2].key == 0x18 && entries[2].length == 16 &&
		       entries[3].key == 0x19 && entries[3].length == 16);
		/* ARot v1 candidate layout of this unit. */
		assert(get_unaligned_le32(entries[2].data) == 1);
		free(real);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	test_words();
	test_rings();
	test_ring_fuzz();
	test_epic();
	test_firmware(argv[1]);
	test_calibration(argv[2]);
	puts("PASS: mgmt/AFK words, AOP-owned v7 ring header, loopback/wrap-copy/space rules, 50000 ring fuzz trials, EPIC property and ready report, real firmware tag table, calibration file");
	return 0;
}
