/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple A9 (S8000/S8003) display pipe: register map, ADP parameter FIFO
 * packet encoder and a read-only view of the display DART left by iBoot.
 *
 * Offsets are relative to the ADP window, DT reg[0] (0x206200000). The ADP
 * control registers (0x1000..) and both pipe banks (0x4000, 0x6000) share it.
 * Include this header from exactly one source file per module.
 */
#ifndef ADBE_HW_H
#define ADBE_HW_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "adbe_addr.h"

#define ADP_SPDS		0x1014	/* low 16 bits: SPDS version, 0x13 */
#define ADP_PARMST		0x101c	/* [7:4] ParmSt: 2 blocks a kick */
#define ADP_PARMST_STATE	GENMASK(7, 4)
#define ADP_INTEN		0x1028
#define ADP_STATUS		0x102c	/* W1C, reads are raw (unmasked) */
#define ADP_FRAME		0x1034	/* [15:0] frame counter */
#define ADP_VALID		0x1038	/* bit 0: pipe state usable */
#define ADP_FIFO_PTR		0x1040	/* [12:0] != 0: packet in flight */
#define ADP_FIFO_PTR_BUSY	GENMASK(12, 0)
#define ADP_PKT			0x10c0	/* packet push port, write-only */

#define ADP_FORMAT		0x402c
#define ADP_BASE		0x4030
#define ADP_STRIDE		0x4038
#define ADP_LAYERS		0x4068
#define ADP_SIZE		0x407c
#define ADP_ADDR_TAG		GENMASK(5, 0)	/* low bits of base/stride words */

#define ADP_ST_VBI		BIT(0)
#define ADP_ST_OUTUNDER		BIT(1)
/*
 * Packet completion. On this pipe every parameter packet raises 0x400 at
 * the frame boundary where it is applied: the FIFO holds it (ParmSt 3,
 * 0x1040 = 1) until that boundary, then ParmSt returns to 1, 0x1040 to 0
 * and 0x400 sets together with VBI (round 2b phase C trace; 0x10 also
 * sets). 0x400 is the primary completion. 0x8 is iOS's "packet synced"
 * (iOS waits on 0x408); it never appeared here, with any token, and is
 * only counted.
 */
#define ADP_ST_SYNCED		BIT(3)
#define ADP_ST_APPLIED		BIT(10)
#define ADP_ST_DONE		(ADP_ST_SYNCED | ADP_ST_APPLIED)
#define ADP_ST_ERRORS		0xa42	/* OutUnder, PFAlign, PFOver, RdErr */

#define ADP_SPDS_VERSION	0x13
#define ADP_FMT_FLAGS		(BIT(0) | BIT(21) | BIT(22) | GENMASK(27, 24))

/* Words after the preamble; a full layer set needs 32. */
#define ADP_PKT_MAX_WORDS	40

/*
 * Pipe 0, layer 0 register set in the order iOS queues it (study §12.4,
 * §13.3): format, stride, base, rects, scales, size, position, layer
 * enable (last of the layer group), destination window, background.
 */
static const u16 adbe_layer_regs[] = {
	0x402c, 0x4038, 0x4030, 0x4040, 0x4048, 0x4094, 0x40a4, 0x407c,
	0x406c, 0x4068, 0x4050, 0x4054, 0x4058, 0x405c, 0x4060, 0x4064,
};

/* How a plane update reaches the pipe; selected by module parameters. */
enum adbe_commit {
	ADBE_COMMIT_BASE,	/* packet: [0x4038] + 0x4030 */
	ADBE_COMMIT_FULL,	/* packet: the whole adbe_layer_regs set */
	ADBE_COMMIT_SYNC,	/* direct stores + empty armed packet */
};

static const char * const adbe_commit_names[] = {
	[ADBE_COMMIT_BASE] = "base",
	[ADBE_COMMIT_FULL] = "full",
	[ADBE_COMMIT_SYNC] = "sync",
};

/*
 * The preamble token becomes the frame counter. "next" keeps the counter
 * continuous (the packet lands on the next frame); "serial" is round 2's
 * 1, 2, 3... which reset it.
 */
enum adbe_token {
	ADBE_TOKEN_NEXT,
	ADBE_TOKEN_SAME,
	ADBE_TOKEN_SERIAL,
};

static const char * const adbe_token_names[] = {
	[ADBE_TOKEN_NEXT] = "next",
	[ADBE_TOKEN_SAME] = "same",
	[ADBE_TOKEN_SERIAL] = "serial",
};

struct adbe_pkt {
	u32 word[ADP_PKT_MAX_WORDS];
	unsigned int count;	/* words after the preamble */
};

static inline bool adbe_format_known(u32 format)
{
	format &= ~ADP_FMT_FLAGS;
	return format == 0xc6000 || format == 0xe4000;
}

static inline int adbe_pkt_add(struct adbe_pkt *pkt, u32 reg, u32 value)
{
	if (pkt->count + 2 > ADP_PKT_MAX_WORDS)
		return -ENOSPC;
	pkt->word[pkt->count++] = adbe_pkt_header(reg);
	pkt->word[pkt->count++] = value;
	return 0;
}

/*
 * Build a packet that changes only base and, if different, stride. For the
 * full variant every other layer register is re-sent with its live value.
 */
static inline int adbe_pkt_build(struct adbe_pkt *pkt, void __iomem *adp,
				 enum adbe_commit commit, u32 base, u32 stride)
{
	unsigned int i;
	int ret = 0;

	pkt->count = 0;
	if (commit == ADBE_COMMIT_SYNC)
		return 0;
	if (commit == ADBE_COMMIT_BASE) {
		if (readl(adp + ADP_STRIDE) != stride)
			ret = adbe_pkt_add(pkt, ADP_STRIDE, stride);
		return ret ?: adbe_pkt_add(pkt, ADP_BASE, base);
	}
	for (i = 0; i < ARRAY_SIZE(adbe_layer_regs) && !ret; i++) {
		u32 reg = adbe_layer_regs[i];
		u32 value = readl(adp + reg);

		if (reg == ADP_BASE)
			value = base;
		else if (reg == ADP_STRIDE)
			value = stride;
		ret = adbe_pkt_add(pkt, reg, value);
	}
	return ret;
}

/* readCurrentParamFIFOState(): 1 ready, 0 in flight, -EIO undefined. */
static inline int adbe_fifo_state(void __iomem *adp, u32 *parmst, u32 *ptr)
{
	*ptr = readl(adp + ADP_FIFO_PTR);
	*parmst = readl(adp + ADP_PARMST);
	if (*ptr & ADP_FIFO_PTR_BUSY)
		return 0;
	if (FIELD_GET(ADP_PARMST_STATE, *parmst) > 5)
		return -EIO;
	return FIELD_GET(ADP_PARMST_STATE, *parmst) != 2;
}

/* Bounded, sleeping wait for the pre-kick gate. Never spins forever. */
static inline int adbe_fifo_wait_ready(void __iomem *adp, unsigned int ms,
				       u32 *parmst, u32 *ptr)
{
	int state;

	for (;;) {
		state = adbe_fifo_state(adp, parmst, ptr);
		if (state)
			return state > 0 ? 0 : state;
		if (!ms--)
			return -EBUSY;
		usleep_range(900, 1100);
	}
}

/*
 * Push one packet: preamble (armed, word count, token), then the words.
 * The port has no per-word handshake, so keep the burst together, and
 * read the frame counter for the token right next to it. For
 * ADBE_COMMIT_SYNC the caller has already stored the registers and this
 * pushes only the empty preamble, as the upstream ADP driver does for the
 * related H7 pipe. Returns the token used.
 */
static inline u16 adbe_pkt_push(void __iomem *adp, const struct adbe_pkt *pkt,
				enum adbe_token policy, u16 serial, bool armed)
{
	unsigned long flags;
	unsigned int i;
	u16 token;

	local_irq_save(flags);
	token = serial;
	if (policy != ADBE_TOKEN_SERIAL)
		token = readl(adp + ADP_FRAME) + (policy == ADBE_TOKEN_NEXT);
	writel(adbe_pkt_preamble(pkt->count, token, armed), adp + ADP_PKT);
	for (i = 0; i < pkt->count; i++)
		writel(pkt->word[i], adp + ADP_PKT);
	local_irq_restore(flags);
	return token;
}

/* The counter moved by other than one frame: reloaded or skipped. */
static inline bool adbe_frame_jump(u16 last, u16 now)
{
	return now != last && now != (u16)(last + 1);
}

/*
 * Display DART (dart-disp0, "dart,s5l8960x" family), read-only.
 *
 * Linux has no node for it: attaching apple-dart would reset the streams
 * under the live scanout. The two ADT windows are read here only to learn
 * whether the pipe's bus addresses are translated. TCR at 0xc (one byte per
 * stream, bit 7 = translate), TTBRs at 0x40 (valid bit 31, PA >> 12).
 * Page tables: 4 KiB granule, TTBR index = bus[31:30], L1 = bus[29:21],
 * L2 = bus[20:12], PTE valid bit 0, address bits [35:12].
 */
#define ADBE_DART_WINDOWS	2
#define ADBE_DART_STREAMS	4
#define ADBE_DART_TTBRS		4
#define ADBE_DART_SIZE		0x4000
#define DART_TCR		0x0c
#define DART_TCR_TRANSLATE	BIT(7)
#define DART_ERROR		0x10
#define DART_TTBR(sid, idx)	(0x40 + 4 * ((sid) * ADBE_DART_TTBRS + (idx)))
#define DART_TTBR_VALID		BIT(31)
#define DART_TTBR_ADDR		GENMASK(23, 0)
#define DART_PTE_VALID		BIT_ULL(0)
#define DART_PTE_ADDR		GENMASK_ULL(35, 12)

/* ADT dart-disp0 reg order: 0x206304000, then 0x206300000. */
static const phys_addr_t adbe_dart_phys[ADBE_DART_WINDOWS] = {
	0x206304000, 0x206300000,
};

struct adbe_dart {
	u32 tcr[ADBE_DART_WINDOWS];
	u32 error[ADBE_DART_WINDOWS];
	u32 ttbr[ADBE_DART_WINDOWS][ADBE_DART_STREAMS][ADBE_DART_TTBRS];
	bool read;
};

static inline bool adbe_dart_translates(const struct adbe_dart *dart,
					unsigned int win, unsigned int sid)
{
	return (dart->tcr[win] >> (8 * sid)) & DART_TCR_TRANSLATE;
}

/* Snapshot TCR, error and TTBR words of both windows; reads only. */
static inline int adbe_dart_read(struct device *dev, struct adbe_dart *dart)
{
	unsigned int win, sid, idx;

	for (win = 0; win < ADBE_DART_WINDOWS; win++) {
		void __iomem *regs = ioremap_np(adbe_dart_phys[win], ADBE_DART_SIZE);

		if (!regs)
			return -ENOMEM;
		dart->tcr[win] = readl(regs + DART_TCR);
		dart->error[win] = readl(regs + DART_ERROR);
		for (sid = 0; sid < ADBE_DART_STREAMS; sid++)
			for (idx = 0; idx < ADBE_DART_TTBRS; idx++)
				dart->ttbr[win][sid][idx] =
					readl(regs + DART_TTBR(sid, idx));
		iounmap(regs);
		dev_info(dev, "DART win%u %pa: tcr=%#010x error=%#x\n", win,
			 &adbe_dart_phys[win], dart->tcr[win], dart->error[win]);
		for (sid = 0; sid < ADBE_DART_STREAMS; sid++)
			dev_info(dev, "DART win%u sid%u translate=%u ttbr=%#x %#x %#x %#x\n",
				 win, sid, adbe_dart_translates(dart, win, sid),
				 dart->ttbr[win][sid][0], dart->ttbr[win][sid][1],
				 dart->ttbr[win][sid][2], dart->ttbr[win][sid][3]);
	}
	dart->read = true;
	return 0;
}

/* Table pages are only read if they are inside a /memory range. */
static inline bool adbe_pa_is_dram(phys_addr_t pa)
{
	struct device_node *np;
	struct resource res;
	bool found = false;
	int i;

	for_each_node_by_type(np, "memory") {
		for (i = 0; !found && !of_address_to_resource(np, i, &res); i++)
			found = pa >= res.start && pa + SZ_4K - 1 <= res.end;
		if (found) {
			of_node_put(np);
			break;
		}
	}
	return found;
}

static inline int adbe_dart_pte(phys_addr_t table, unsigned int index, u64 *pte)
{
	u64 *page;

	if (!adbe_pa_is_dram(table))
		return -EFAULT;
	page = memremap(table, SZ_4K, MEMREMAP_WB);
	if (!page)
		return -ENOMEM;
	*pte = READ_ONCE(page[index]);
	memunmap(page);
	return 0;
}

/* Translate one bus page through one stream; -ENOENT if unmapped. */
static inline int adbe_dart_lookup(const struct adbe_dart *dart,
				   unsigned int win, unsigned int sid,
				   u32 bus, phys_addr_t *pa)
{
	u32 ttbr = dart->ttbr[win][sid][bus >> 30];
	u64 pte;
	int ret;

	if (!(ttbr & DART_TTBR_VALID))
		return -ENOENT;
	ret = adbe_dart_pte((phys_addr_t)FIELD_GET(DART_TTBR_ADDR, ttbr) << 12,
			    (bus >> 21) & 0x1ff, &pte);
	if (ret || !(pte & DART_PTE_VALID))
		return ret ?: -ENOENT;
	ret = adbe_dart_pte(pte & DART_PTE_ADDR, (bus >> 12) & 0x1ff, &pte);
	if (ret || !(pte & DART_PTE_VALID))
		return ret ?: -ENOENT;
	*pa = (pte & DART_PTE_ADDR) | (bus & 0xfff);
	return 0;
}

/*
 * Bytes from @bus (page aligned) that one stream maps linearly onto @pa.
 * Stops at the first gap; bounded by @len.
 */
static inline u64 adbe_dart_linear(const struct adbe_dart *dart,
				   unsigned int win, unsigned int sid,
				   u32 bus, phys_addr_t pa, u64 len)
{
	u64 done;

	for (done = 0; done < len; done += SZ_4K) {
		phys_addr_t got;

		if ((u64)bus + done > U32_MAX ||
		    adbe_dart_lookup(dart, win, sid, bus + done, &got) ||
		    got != pa + done)
			break;
	}
	return min(done, len);
}

/*
 * 1 if this stream translates and maps the live scanout page, 0 if it does
 * not translate or leaves that page unmapped/elsewhere (so it cannot be the
 * stream the pipe reads through), <0 if its page tables could not be read.
 */
static inline int adbe_dart_is_scanout(const struct adbe_dart *dart,
				       unsigned int win, unsigned int sid,
				       u32 live_bus, phys_addr_t live_pa)
{
	phys_addr_t got;
	int ret;

	if (!adbe_dart_translates(dart, win, sid))
		return 0;
	ret = adbe_dart_lookup(dart, win, sid, live_bus & ~0xfffU, &got);
	if (ret)
		return ret == -ENOENT ? 0 : ret;
	return got == (live_pa & ~(phys_addr_t)0xfff);
}

/* Translating streams that carry the live scanout (0: bypass), or <0. */
static inline int adbe_dart_scanout_streams(const struct adbe_dart *dart,
					    u32 live_bus, phys_addr_t live_pa)
{
	unsigned int win, sid;
	int ret, streams = 0;

	if (!dart->read)
		return -EIO;
	for (win = 0; win < ADBE_DART_WINDOWS; win++) {
		for (sid = 0; sid < ADBE_DART_STREAMS; sid++) {
			ret = adbe_dart_is_scanout(dart, win, sid, live_bus, live_pa);
			if (ret < 0)
				return ret;
			streams += ret;
		}
	}
	return streams;
}

/*
 * Whether the pipe can read [@bus, @bus + @len) as [@pa, ...). The live
 * scanout (@live_bus -> @live_pa) is known good, so a translating stream
 * that does not map it is not the scanout stream and is ignored. If no
 * translating stream maps it, the pipe reads through bypass, which is
 * linear; the caller derived the offset from the same live scanout.
 * Returns the verified length (0..@len); <0 if the DART was not read or a
 * page table was unreadable (unknown is never treated as bypass).
 */
static inline s64 adbe_dart_reachable(struct device *dev,
				      const struct adbe_dart *dart,
				      u32 live_bus, phys_addr_t live_pa,
				      u32 bus, phys_addr_t pa, u64 len)
{
	unsigned int win, sid, streams = 0;
	u64 covered = len;

	if (!dart->read)
		return -EIO;
	for (win = 0; win < ADBE_DART_WINDOWS; win++) {
		for (sid = 0; sid < ADBE_DART_STREAMS; sid++) {
			int scanout = adbe_dart_is_scanout(dart, win, sid,
							   live_bus, live_pa);
			u64 linear;

			if (scanout < 0) {
				dev_warn(dev, "DART win%u sid%u: page table unreadable (%d)\n",
					 win, sid, scanout);
				return scanout;
			}
			if (!scanout) {
				if (adbe_dart_translates(dart, win, sid))
					dev_info(dev, "DART win%u sid%u translates but not the live scanout; ignored\n",
						 win, sid);
				continue;
			}
			streams++;
			linear = adbe_dart_linear(dart, win, sid, bus, pa, len);
			dev_info(dev, "DART win%u sid%u maps the live scanout; %#llx of %#llx bytes at bus %#x linear\n",
				 win, sid, linear, len, bus);
			covered = min(covered, linear);
		}
	}
	if (!streams)
		dev_info(dev, "DART: no translating stream maps the scanout; bypass, bus %#x linear\n",
			 bus);
	return covered;
}

#endif /* ADBE_HW_H */
