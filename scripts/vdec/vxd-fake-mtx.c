// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace stand-in for the DEVA firmware, for the QEMU smoke only
 * (scripts/vdec/qemu-smoke.sh, boot "fakemtx").  It maps the RAM-backed
 * apple,s8000-vxd register window and the GIC distributor through
 * /dev/mem and plays the MTX side of the protocol as documented in
 * docs/periph/ds-vxd-decode-protocol.md, re-implemented here independently
 * of the driver so the two cross-check each other:
 *
 *   - clears the busy bits the boot ladder polls (0x400 bit 0, 0x1008
 *     bit 28) and emulates write-1-to-clear on 0x414
 *   - on the MTX release (0x6a04 = 0x2201xxxx) publishes the ring geometry
 *     (0x2024 TO_MTX, 0x2018 TO_HOST) and the 0xa5a5a5a5 ready magic
 *   - on every doorbell (0x88 = 1) takes all TO_MTX messages exactly like
 *     the DEVA reader: publish 0x2028, 0x0 = nwords, 0x200c = PC
 *   - 0x80: checks every init-message field against the registers
 *     0x83: dropped silently, as the DEVA dispatcher does
 *     0x84: test hook (the real firmware drops it too): replies a 0xc0
 *           completion for the message's fence, a 0xc6 and an ignored
 *           0xd1, then raises the interrupt
 *     0x85: test hook: replies 0xc7 FW_DEVA_PANIC and raises the interrupt
 *     0x81: an AVC slice packet (ds-vxd-cmdstream.md section 16.3-16.6):
 *           walks the command chunk at w2 through /dev/mem (the GART is an
 *           identity map and QEMU RAM sits below 4 GiB, so VA == PA),
 *           checks its framing, the CAVLC table it DMAs and the bitstream
 *           address.  Like the real firmware (ds-vxd-deva-firmware.md:
 *           0x2000 = NO_RESPONSE, 0x4000 = HOST_INT) only the 0x4004
 *           packet is answered: a 0xc0 (fault bits set if any chunk of the
 *           picture was malformed) and the interrupt, after writing
 *           "VXDFAKE!" into the first bytes of the output luma plane named
 *           by CMDS 0x700c.  No pixel is decoded.
 *   - raises the interrupt the way the firmware does (0x410 |= bit 20)
 *     and makes the GIC see it by setting the SPI pending (GICD_ISPENDR)
 *
 * The rings are deliberately small (24 words each, the driver wants more
 * than 0x14) so the smoke wraps both several times.  Usage: vxd-fake-mtx VXD_BASE GICD_BASE SPI
 * Runs until SIGTERM, then prints a summary.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TO_MTX_BASE	0x100
#define TO_MTX_CAP	24
#define TO_HOST_BASE	0x200
#define TO_HOST_CAP	24
#define FAKE_PC		0x01000958
#define GICD_ISPENDR	0x200

static volatile uint32_t *regs;
static volatile uint32_t *gicd;
static unsigned int spi;
static volatile sig_atomic_t stop;
static unsigned int errors, boots, inits, nops, completions, panics;
static unsigned int slices, pictures, bad_chunks;
static int memfd;
static uint32_t fw_words, init_seq;

static uint32_t rd32(uint32_t off)
{
	return regs[off / 4];
}

static void wr32(uint32_t off, uint32_t val)
{
	regs[off / 4] = val;
}

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static void raise_irq(void)
{
	unsigned int intid = 32 + spi;

	wr32(0x410, rd32(0x410) | 0x100000);
	gicd[(GICD_ISPENDR + 4 * (intid / 32)) / 4] = 1u << (intid % 32);
}

static void post(const uint32_t *msg)
{
	uint32_t n = msg[0] & 0x7f, rd = rd32(0x201c), wr = rd32(0x2020), i;
	uint32_t free_words = rd > wr ? rd - wr - 1 : rd - wr - 1 + TO_HOST_CAP;

	if (rd >= TO_HOST_CAP || wr >= TO_HOST_CAP || free_words < n) {
		printf("fake-mtx: TO_HOST full or corrupt (rd %u wr %u)\n", rd, wr);
		errors++;
		return;
	}
	for (i = 0; i < n; i++) {
		wr32(0x2000 + TO_HOST_BASE + 4 * wr, msg[i]);
		wr = wr + 1 < TO_HOST_CAP ? wr + 1 : 0;
	}
	wr32(0x2020, wr);
	wr32(0x2010, rd32(0x2010) + 1);
}

static int phys_read(uint32_t pa, void *buf, size_t n)
{
	return pread(memfd, buf, n, pa) == (ssize_t)n ? 0 : -1;
}

/*
 * One AVC slice chunk: returns 0 if well formed, and the output luma VA
 * (CMDS 0x700c) in *luma.  Independent of the driver's emitters.
 */
static int walk_chunk(uint32_t va, uint32_t words, uint32_t *luma)
{
	static const uint8_t vlc_head[8] = { 0x00, 0x40, 0x05, 0x42, 0x0a, 0x44, 0x04, 0x22 };
	uint32_t w[4096], i = 0, c, cnt, reg, k;
	uint8_t b[8];
	int dma = 0, sr = 0, load = 0, out = 0;

	if (!words || words > 4096 || phys_read(va, w, words * 4)) {
		printf("fake-mtx: chunk at %#x: %u words unreadable\n", va, words);
		return -1;
	}
	if (w[0] != 0x90000000 || w[words - 1] != 0x60000000) {
		printf("fake-mtx: chunk at %#x: starts %#010x, ends %#010x\n", va, w[0], w[words - 1]);
		return -1;
	}
	while (i < words) {
		c = w[i] >> 28;
		switch (c) {
		case 0x9:
			i += 10;
			break;
		case 0xe:
			if ((w[i] & 0xffff) != 0x440 || phys_read(w[i + 1], b, 8) ||
			    memcmp(b, vlc_head, 8)) {
				printf("fake-mtx: chunk at %#x: VLC DMA %#010x from %#x is not the CAVLC table\n",
				       va, w[i], w[i + 1]);
				return -1;
			}
			dma = 1;
			i += 2;
			break;
		case 0x1:
			i += 1 + ((w[i] >> 16) & 0xfff);
			break;
		case 0xb:
			sr = 1;
			i += 3;
			break;
		case 0xa:
			if (phys_read(w[i + 1], b, 1)) {
				printf("fake-mtx: chunk at %#x: bitstream at %#x unreadable\n", va, w[i + 1]);
				return -1;
			}
			load = (w[i] & 0x0fffffff) != 0;
			i += 2;
			break;
		case 0x5:
			cnt = (w[i] >> 16) & 0x7ff;
			reg = w[i] & 0xffff;
			for (k = 0; k < cnt; k++)
				if (reg + 4 * k == 0x700c) {
					*luma = w[i + 1 + k];
					out = 1;
				}
			i += 1 + cnt;
			break;
		case 0x6:
			i++;
			if (i != words) {
				printf("fake-mtx: chunk at %#x: completion word at %u of %u\n", va, i - 1, words);
				return -1;
			}
			break;
		default:
			printf("fake-mtx: chunk at %#x: unknown command %#010x at word %u\n", va, w[i], i);
			return -1;
		}
	}
	if (!dma || !sr || !load || !out) {
		printf("fake-mtx: chunk at %#x: missing vlc %d sr %d load %d output %d\n",
		       va, dma, sr, load, out);
		return -1;
	}
	return 0;
}

static void handle(const uint32_t *msg, uint32_t n)
{
	uint32_t type = (msg[0] >> 8) & 0xff, fence = msg[0] >> 16;
	uint32_t reply[4];

	switch (type) {
	case 0x80:
		inits++;
		if (n != 5 || msg[1] != rd32(0x6a00) ||
		    (msg[2] & 0xffff) != (fw_words * 4) >> 12 ||
		    (msg[2] >> 16) != (init_seq & 0xff) ||
		    msg[3] != 0xffff0000 || msg[4]) {
			printf("fake-mtx: init message BAD: %08x %08x %08x %08x %08x (fw va %#x, %u words, seq %u)\n",
			       msg[0], msg[1], msg[2], msg[3], msg[4],
			       rd32(0x6a00), fw_words, init_seq);
			errors++;
		} else {
			printf("fake-mtx: init message ok: fw va %#x, %u pages, seq %u\n",
			       msg[1], msg[2] & 0xffff, init_seq);
		}
		init_seq++;
		break;
	case 0x83:
		nops++;
		break;
	case 0x84:
		nops++;
		completions++;
		reply[0] = fence << 16 | 0xc0 << 8 | 4;
		reply[1] = 0;
		reply[2] = 0;
		reply[3] = 0;
		post(reply);
		reply[0] = 0xc6 << 8 | 3;
		reply[1] = 1;
		reply[2] = 2;
		post(reply);
		reply[0] = 0xd1 << 8 | 1;
		post(reply);
		raise_irq();
		break;
	case 0x81: {
		static int picture_bad;
		uint32_t luma = 0, words = msg[1] >> 16, flags = msg[1] & 0xffff;
		int bad = n != 6 || !(flags & 0x6000) ||
			  walk_chunk(msg[2], words, &luma);

		slices++;
		if (bad) {
			bad_chunks++;
			errors++;
			picture_bad = 1;
		}
		if ((flags & 0x4004) != 0x4004)
			break;		/* 0x2000: no response message */

		pictures++;
		completions++;
		if (luma && pwrite(memfd, "VXDFAKE!", 8, luma) != 8)
			errors++;
		reply[0] = fence << 16 | 0xc0 << 8 | 4;
		reply[1] = 0;
		reply[2] = picture_bad ? 0x18000000 : 0;
		reply[3] = 0;
		picture_bad = 0;
		post(reply);
		raise_irq();
		break;
	}
	case 0x85:
		nops++;
		panics++;
		reply[0] = fence << 16 | 0xc7 << 8 | 2;
		reply[1] = 0xdead;
		post(reply);
		raise_irq();
		break;
	default:
		printf("fake-mtx: unexpected message type %#x (%u words)\n", type, n);
		errors++;
		break;
	}
}

/* The DEVA reader (fw 0x10008f0): everything between rd and wr. */
static void consume(void)
{
	uint32_t rd = rd32(0x2028), wr = rd32(0x2030), msg[0x14], n, i;

	while (rd != wr) {
		if (rd >= TO_MTX_CAP || wr >= TO_MTX_CAP) {
			printf("fake-mtx: TO_MTX indices out of range (rd %u wr %u)\n", rd, wr);
			errors++;
			return;
		}
		msg[0] = rd32(0x2000 + TO_MTX_BASE + 4 * rd);
		n = msg[0] & 0x7f;
		if (!n || n > 0x14) {
			/* The real reader wedges here; report and stop reading. */
			printf("fake-mtx: bad header %#010x at %u\n", msg[0], rd);
			errors++;
			return;
		}
		for (i = 0; i < n; i++) {
			msg[i] = rd32(0x2000 + TO_MTX_BASE + 4 * rd);
			rd = rd + 1 < TO_MTX_CAP ? rd + 1 : 0;
		}
		wr32(0x200c, FAKE_PC);
		wr32(0x0, n);
		wr32(0x2028, rd);
		handle(msg, n);
	}
}

static void *map(int fd, off_t base, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);

	if (p == MAP_FAILED) {
		perror("fake-mtx: mmap");
		exit(1);
	}
	return p;
}

int main(int argc, char **argv)
{
	uint32_t v;
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s VXD_BASE GICD_BASE SPI\n", argv[0]);
		return 2;
	}
	spi = strtoul(argv[3], NULL, 0);
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("fake-mtx: /dev/mem");
		return 1;
	}
	memfd = fd;
	regs = map(fd, strtoull(argv[1], NULL, 0), 0x100000);
	gicd = map(fd, strtoull(argv[2], NULL, 0), 0x10000);
	signal(SIGTERM, on_term);
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("fake-mtx: up (TO_MTX %u words at %#x, TO_HOST %u words at %#x, SPI %u)\n",
	       TO_MTX_CAP, 0x2000 + TO_MTX_BASE, TO_HOST_CAP,
	       0x2000 + TO_HOST_BASE, spi);

	while (!stop) {
		v = rd32(0x400);
		if (v & 1)
			wr32(0x400, v & ~1u);
		v = rd32(0x1008);
		if (v & 0x10000000)
			wr32(0x1008, v & ~0x10000000u);
		v = rd32(0x414);
		if (v) {
			wr32(0x410, rd32(0x410) & ~v);
			wr32(0x414, 0);
		}

		v = rd32(0x6a04);
		if ((v & 0xffff0000) == 0x22010000) {
			wr32(0x6a04, 0);
			fw_words = v & 0xffff;
			init_seq = 0;
			boots++;
			wr32(0x2024, TO_MTX_BASE << 16 | TO_MTX_CAP);
			wr32(0x2018, TO_HOST_BASE << 16 | TO_HOST_CAP);
			wr32(0x200c, FAKE_PC);
			wr32(0x2014, 0xa5a5a5a5);
			printf("fake-mtx: MTX released (%u words at VA %#x), rings + ready magic published\n",
			       fw_words, rd32(0x6a00));
		}

		if (boots && rd32(0x88)) {
			wr32(0x88, 0);
			consume();
		}
		usleep(20);
	}

	printf("fake-mtx: summary boots %u inits %u nops %u completions %u panics %u errors %u\n",
	       boots, inits, nops, completions, panics, errors);
	printf("fake-mtx: decode slices %u pictures %u bad chunks %u\n",
	       slices, pictures, bad_chunks);
	return errors ? 1 : 0;
}
