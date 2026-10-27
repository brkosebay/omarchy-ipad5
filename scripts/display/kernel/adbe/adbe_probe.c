// SPDX-License-Identifier: GPL-2.0-only
/*
 * A9 display pipe probe: read-only observation of the pipe iBoot left
 * running, plus opt-in, bounded measurements chosen by module parameters.
 *
 *   (default)       phase A: registers, mode and a 1 s frame-counter poll
 *   irq_index=N     phase B: one DT interrupt for 1 s; irq_mask=0x1/0x8/0x9
 *                   additionally enables VBI/packet-synced in ADP+0x1028
 *                   for that second and restores the exact word after
 *   dart=1          read-only snapshot of the display DART
 *   phase=c         one parameter FIFO kick re-sending the live plane state
 *   phase=d         flip to a second buffer for hold_ms, revert, report
 *
 * Parameters are writable and read once per bind, so a step runs again
 * without unloading the module: unbind, write the parameters, bind.
 * Unbind restores the pipe exactly as unload does.
 *
 * It is not a DRM driver: simpledrm keeps the framebuffer throughout.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "adbe_hw.h"

#define ADBE_WINDOWS		6
#define ADBE_IRQ_LIMIT		1024
#define ADBE_FIFO_WAIT_MS	100	/* iOS waits 1000 ms, then panics */
#define ADBE_KICK_POLL_US	100000	/* three frames take ~51 ms */
#define ADBE_KICK_FRAMES	3
#define ADBE_TRACE		100
#define ADBE_KICKS		4
#define ADBE_HOLD_MAX_MS	10000
#define ADBE_BAND_ROWS		256
#define ADBE_BAND_COLOUR	0x00ff00ff	/* magenta in x8r8g8b8 */

/* Writable; read once per bind under the parameter lock (adbe_params()). */
static int irq_index = -1;
module_param(irq_index, int, 0600);
MODULE_PARM_DESC(irq_index, "Phase B: one DT IRQ index 0..4 for 1 s; -1 disables");

static uint irq_mask;
module_param(irq_mask, uint, 0600);
MODULE_PARM_DESC(irq_mask, "Phase B: ADP+0x1028 bits to add for the 1 s test (subset of 0x9)");

static bool dart;
module_param(dart, bool, 0600);
MODULE_PARM_DESC(dart, "Read-only snapshot of the display DART (implied by phase=d)");

static char *phase = "a";
module_param(phase, charp, 0600);
MODULE_PARM_DESC(phase, "a: read-only (default); c: FIFO re-send of the live plane; d: flip and revert");

static char *packet = "base";
module_param(packet, charp, 0600);
MODULE_PARM_DESC(packet, "Phases C/D: base (3-word packet), full (whole layer set), sync (direct store + empty packet)");

static char *token = "next";
module_param(token, charp, 0600);
MODULE_PARM_DESC(token, "Phases C/D preamble token: next (frame counter + 1), same, serial (round 2)");

static bool arm = true;
module_param(arm, bool, 0600);
MODULE_PARM_DESC(arm, "Phases C/D: set the preamble's armed bit (iOS default)");

static char *fill = "band";
module_param(fill, charp, 0600);
MODULE_PARM_DESC(fill, "Phase D buffer content: band (live copy + magenta band), white, black");

enum adbe_fill {
	ADBE_FILL_BAND,
	ADBE_FILL_WHITE,
	ADBE_FILL_BLACK,
};

static const char * const adbe_fill_names[] = {
	[ADBE_FILL_BAND] = "band",
	[ADBE_FILL_WHITE] = "white",
	[ADBE_FILL_BLACK] = "black",
};

static char *scratch = "vram";
module_param(scratch, charp, 0600);
MODULE_PARM_DESC(scratch, "Phase D buffer: vram (reserved boot VRAM) or dma (new DMA allocation)");

static ulong scratch_offset = 1536 * 2048 * 4;
module_param(scratch_offset, ulong, 0600);
MODULE_PARM_DESC(scratch_offset, "Phase D vram: offset after the boot framebuffer, 64-byte aligned");

static int tag = -1;
module_param(tag, int, 0600);
MODULE_PARM_DESC(tag, "Phase D: low 6 bits of the new base word; -1 keeps the live bits");

static uint hold_ms = 2000;
module_param(hold_ms, uint, 0600);
MODULE_PARM_DESC(hold_ms, "Phase D: how long the second buffer stays on screen (max 10000)");

/* The parameters of one bind. */
struct adbe_opts {
	int irq_index;
	u32 irq_mask;
	bool dart;
	char phase;
	enum adbe_commit commit;
	enum adbe_token token;
	bool arm;
	enum adbe_fill fill;
	bool scratch_dma;
	unsigned long scratch_offset;
	int tag;
	u32 hold_ms;
};

struct adbe_sample {
	u32 status_or;
	u32 frames;
	u32 changes;
	u32 vbi_samples;
	u32 packet_samples;
	u32 base;
	u32 jumps;
	u16 first;
	u16 last;
};

/* A reloaded counter counts as one frame, and as a jump. */
static void adbe_count_frame(struct adbe_sample *s, u16 previous, u16 frame)
{
	if (adbe_frame_jump(previous, frame)) {
		s->jumps++;
		s->frames++;
	} else {
		s->frames += frame != previous;
	}
	s->changes += frame != previous;
}

struct adbe_trace {
	u32 us;
	u32 status;
	u16 frame;
	u16 ptr;
	u8 parmst;
};

struct adbe_kick {
	const char *what;
	int result;
	bool pushed;
	u32 words;
	u32 base;
	u32 parmst_before;
	u32 ptr_before;
	u32 status_before;
	u32 parmst_after;
	u32 ptr_after;
	u32 burst_busy;
	u32 burst_ptr_max;
	u16 parmst_seen;
	u32 samples;
	u32 ptr_busy;
	u32 frames;
	u32 jumps;
	u16 token;
	u16 frame_before;
	u16 frame_next;		/* first counter value after the push */
	u32 synced;
	u32 applied;
	u32 first_sync_us;
	u32 first_applied_us;
	u32 status_or;
	u32 errors;
	u32 base_after;
	unsigned int ntrace;
	struct adbe_trace trace[ADBE_TRACE];
};

struct adbe_probe {
	struct device *dev;
	void __iomem *window[ADBE_WINDOWS];
	resource_size_t size[ADBE_WINDOWS];
	void __iomem *adp;
	int adp_index;
	struct resource fb;
	struct resource vram;
	u32 width;
	u32 height;
	u32 stride;
	size_t bytes;
	u64 bus_offset;
	struct mutex lock; /* Serializes phases, restore, debugfs and teardown. */
	struct notifier_block reboot_nb;
	struct dentry *debugfs;
	struct adbe_opts opt;
	bool flipped;
	u32 saved_base;
	u32 test_base;
	u16 serial;
	/* Phase D buffer; kept while it might still be scanned out. */
	void __iomem *scratch_io;
	void *scratch_va;
	dma_addr_t scratch_dma;
	int phase_result;
	struct adbe_dart dart;
	struct adbe_sample d1;
	struct adbe_sample hold;
	unsigned int nkicks;
	struct adbe_kick kick[ADBE_KICKS];
	atomic_t irq_hits;
	atomic_t irq_vbi;
	atomic_t irq_synced;
	atomic_t irq_status;
	atomic_t irq_storm;
	int irq;
};

static u32 adbe_read(struct adbe_probe *p, u32 reg)
{
	return readl(p->adp + reg);
}

/* No ack here: status is a normal, non-destructive read. */
static void adbe_sample(struct adbe_probe *p, unsigned int milliseconds,
			struct adbe_sample *s)
{
	unsigned long end = jiffies + msecs_to_jiffies(milliseconds);
	u16 previous;
	u32 status;

	memset(s, 0, sizeof(*s));
	previous = adbe_read(p, ADP_FRAME);
	s->first = previous;
	do {
		u16 frame;

		usleep_range(1000, 1500);
		frame = adbe_read(p, ADP_FRAME);
		status = adbe_read(p, ADP_STATUS);
		adbe_count_frame(s, previous, frame);
		s->vbi_samples += !!(status & ADP_ST_VBI);
		s->packet_samples += !!(status & ADP_ST_SYNCED);
		s->status_or |= status;
		previous = frame;
	} while (time_before(jiffies, end));
	s->last = previous;
	s->base = adbe_read(p, ADP_BASE);
}

static int adbe_discriminate(struct adbe_probe *p)
{
	u16 frames[ADBE_WINDOWS] = {};
	u32 formats[ADBE_WINDOWS] = {};
	unsigned int found = 0;
	int i;

	for (i = 0; i < ADBE_WINDOWS; i++) {
		if (p->size[i] < 0x60b0)
			continue;
		formats[i] = readl(p->window[i] + ADP_FORMAT);
		frames[i] = readl(p->window[i] + ADP_FRAME);
		dev_info(p->dev, "window %d fmt=%#x base=%#x stride=%#x layers=%#x frame=%u\n",
			 i, formats[i], readl(p->window[i] + ADP_BASE),
			 readl(p->window[i] + ADP_STRIDE),
			 readl(p->window[i] + ADP_LAYERS), frames[i]);
	}
	msleep(100);
	for (i = 0; i < ADBE_WINDOWS; i++) {
		if (p->size[i] < 0x60b0 || !adbe_format_known(formats[i]) ||
		    (u16)readl(p->window[i] + ADP_FRAME) == frames[i])
			continue;
		p->adp = p->window[i];
		p->adp_index = i;
		found++;
	}
	if (found != 1)
		return dev_err_probe(p->dev, -ENODEV,
				     "need one format + advancing-counter match, got %u\n", found);
	return 0;
}

static int adbe_framebuffer(struct adbe_probe *p)
{
	struct device_node *np;
	const char *format;
	int ret = -ENODEV;

	for_each_compatible_node(np, NULL, "simple-framebuffer") {
		if (!of_device_is_available(np))
			continue;
		/* Match simpledrm's preference for memory-region over reg. */
		ret = of_reserved_mem_region_to_resource(np, 0, &p->fb);
		if (ret)
			ret = of_address_to_resource(np, 0, &p->fb);
		if (ret)
			break;
		ret = of_property_read_u32(np, "width", &p->width);
		ret |= of_property_read_u32(np, "height", &p->height);
		ret |= of_property_read_u32(np, "stride", &p->stride);
		ret |= of_property_read_string(np, "format", &format);
		if (ret || p->width != 1536 || p->height != 2048 ||
		    p->stride != 6144 || strcmp(format, "x8r8g8b8")) {
			ret = -EINVAL;
			break;
		}
		p->bytes = p->stride * p->height;
		if (resource_size(&p->fb) < p->bytes) {
			ret = -EINVAL;
			break;
		}
		ret = 0;
		break;
	}
	of_node_put(np);
	if (ret)
		return dev_err_probe(p->dev, ret, "need the live 1536x2048 simple-framebuffer\n");
	/* Optional scratch reservation; never allocate or attach an IOMMU. */
	ret = of_reserved_mem_region_to_resource(p->dev->of_node, 0, &p->vram);
	if (ret)
		p->vram = p->fb;
	return 0;
}

static int adbe_mode(struct adbe_probe *p)
{
	u32 base = adbe_read(p, ADP_BASE);
	u32 stride = adbe_read(p, ADP_STRIDE) & ~(ADBE_ALIGN - 1);
	u32 size = adbe_read(p, ADP_SIZE);
	u32 width = size & 0x7fff;
	u32 height = (size >> 16) & 0x7fff;

	dev_info(p->dev, "ADP window %d: raw-base=%#x boot-CPU=%pa stride=%u size-fields=%ux%u layers=%#x fmt=%#x\n",
		 p->adp_index, base, &p->fb.start, stride, width, height,
		 adbe_read(p, ADP_LAYERS) & 0xf, adbe_read(p, ADP_FORMAT));
	/* The RE notes disagree about the order of the two size fields. */
	if (!((width == p->width && height == p->height) ||
	      (height == p->width && width == p->height)) ||
	    stride != p->stride ||
	    (base & ~(ADBE_ALIGN - 1)) != lower_32_bits(p->fb.start) ||
	    (adbe_read(p, ADP_LAYERS) & 0xf) != 1)
		return dev_err_probe(p->dev, -ENODEV,
				     "live plane geometry/base differs; refusing writes\n");
	/* CPU = bus + offset: the 32-bit plane word carries the low bits. */
	p->bus_offset = p->fb.start - (base & ~(ADBE_ALIGN - 1));
	dev_info(p->dev, "mode %ux%u stride=%u bus-offset=%#llx\n",
		 p->width, p->height, stride, p->bus_offset);
	return 0;
}

static void adbe_dump_log(struct adbe_probe *p, u32 start, u32 end)
{
	u32 reg;

	for (reg = start; reg < end; reg += 4) {
		if (reg == 0x100c || reg == 0x1010)
			continue;
		dev_info(p->dev, "%04x: %08x\n", reg, adbe_read(p, reg));
	}
}

static irqreturn_t adbe_irq(int irq, void *data)
{
	struct adbe_probe *p = data;
	u32 status = adbe_read(p, ADP_STATUS);
	int hits = atomic_inc_return(&p->irq_hits);

	if (status) {
		/* W1C exactly the observed word: a level line must be acked. */
		writel(status, p->adp + ADP_STATUS);
		atomic_or(status, &p->irq_status);
		if (status & ADP_ST_VBI)
			atomic_inc(&p->irq_vbi);
		if (status & ADP_ST_SYNCED)
			atomic_inc(&p->irq_synced);
	}
	/* Count even empty interrupts: a wrong candidate must also be bounded. */
	if (hits >= ADBE_IRQ_LIMIT && !atomic_xchg(&p->irq_storm, 1))
		disable_irq_nosync(irq);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static int adbe_phase_b(struct platform_device *pdev, struct adbe_probe *p)
{
	u32 saved, during = 0, after;
	u16 first, last;
	int ret;

	if (p->opt.irq_index == -1)
		return 0;
	p->irq = platform_get_irq(pdev, p->opt.irq_index);
	if (p->irq < 0)
		return p->irq;
	/* Exclusive request: never disable another driver's shared IRQ. */
	ret = request_irq(p->irq, adbe_irq, IRQF_NO_AUTOEN, "apple-adbe-probe", p);
	if (ret)
		return ret;
	saved = adbe_read(p, ADP_INTEN);
	first = adbe_read(p, ADP_FRAME);
	enable_irq(p->irq);
	if (p->opt.irq_mask) {
		writel(saved | p->opt.irq_mask, p->adp + ADP_INTEN);
		during = adbe_read(p, ADP_INTEN);
	}
	msleep(1000);
	/* Exact word, not a bit clear: restore what iBoot left. */
	if (p->opt.irq_mask)
		writel(saved, p->adp + ADP_INTEN);
	after = adbe_read(p, ADP_INTEN);
	last = adbe_read(p, ADP_FRAME);
	/* Balance exactly once, whether the storm guard or this thread won. */
	if (!atomic_xchg(&p->irq_storm, 1))
		disable_irq(p->irq);
	synchronize_irq(p->irq);
	dev_info(p->dev, "phase B index=%d Linux-IRQ=%d hits=%d vbi=%d synced=%d frames=%u status-or=%#x guard=%s mask %#x->%#x->%#x\n",
		 p->opt.irq_index, p->irq, atomic_read(&p->irq_hits),
		 atomic_read(&p->irq_vbi), atomic_read(&p->irq_synced),
		 (u16)(last - first), atomic_read(&p->irq_status),
		 atomic_read(&p->irq_hits) >= ADBE_IRQ_LIMIT ? "tripped" : "no",
		 saved, during, after);
	free_irq(p->irq, p);
	if (after != saved)
		return dev_err_probe(p->dev, -EIO, "interrupt mask not restored\n");
	return 0;
}

static void adbe_dart_snapshot(struct adbe_probe *p)
{
	u32 live = adbe_read(p, ADP_BASE) & ~(ADBE_ALIGN - 1);
	u64 reserved = p->vram.end - p->fb.start + 1;
	s64 covered;

	if (p->dart.read)
		return;
	if (adbe_dart_read(p->dev, &p->dart)) {
		dev_warn(p->dev, "DART snapshot failed\n");
		return;
	}
	covered = adbe_dart_reachable(p->dev, &p->dart, live, p->fb.start,
				      live, p->fb.start, reserved);
	dev_info(p->dev, "DART: boot VRAM from bus %#x: %lld of %llu bytes verified linear\n",
		 live, covered, reserved);
}

/*
 * One parameter FIFO kick with the measurements of study §13.9: ParmSt and
 * the FIFO pointer before and after, a short microsecond burst, then a
 * ~1 ms poll for three frames counting completion events (0x8 synced,
 * 0x400 applied) and frame-counter jumps. Every wait is bounded; a busy FIFO
 * is reported and nothing is written. *@pushed tells whether anything
 * reached the pipe. Success: a completion, no error bits, base read back.
 */
static int adbe_kick(struct adbe_probe *p, const char *what, u32 base,
		     u32 stride, bool *pushed)
{
	struct adbe_kick *k;
	struct adbe_pkt pkt;
	struct adbe_sample frames = {};
	u32 parmst, ptr, status;
	u16 previous;
	ktime_t t0;
	s64 us = 0;
	int ret, i;

	*pushed = false;
	if (p->nkicks >= ADBE_KICKS)
		return -ENOSPC;
	/* A direct store that moves the base needs the restore path armed. */
	if (p->opt.commit == ADBE_COMMIT_SYNC && !p->flipped &&
	    base != adbe_read(p, ADP_BASE))
		return -EINVAL;
	k = &p->kick[p->nkicks++];
	k->what = what;
	k->base = base;
	ret = adbe_pkt_build(&pkt, p->adp, p->opt.commit, base, stride);
	if (ret)
		goto out;
	k->words = pkt.count + 1;
	ret = adbe_fifo_wait_ready(p->adp, ADBE_FIFO_WAIT_MS, &parmst, &ptr);
	k->parmst_before = parmst;
	k->ptr_before = ptr;
	if (ret) {
		dev_warn(p->dev, "%s: FIFO not ready after %u ms (0x101c=%#x 0x1040=%#x), not kicked\n",
			 what, ADBE_FIFO_WAIT_MS, parmst, ptr);
		goto out;
	}
	/* Count only this kick's events: clear stale completion bits. */
	status = adbe_read(p, ADP_STATUS);
	k->status_before = status;
	if (status & ADP_ST_DONE)
		writel(status & ADP_ST_DONE, p->adp + ADP_STATUS);
	if (p->opt.commit == ADBE_COMMIT_SYNC) {
		if (adbe_read(p, ADP_STRIDE) != stride)
			writel(stride, p->adp + ADP_STRIDE);
		writel(base, p->adp + ADP_BASE);
	}
	k->frame_before = adbe_read(p, ADP_FRAME);
	previous = k->frame_before;
	t0 = ktime_get();
	k->token = adbe_pkt_push(p->adp, &pkt, p->opt.token, ++p->serial, p->opt.arm);
	k->pushed = true;
	*pushed = true;
	k->parmst_after = adbe_read(p, ADP_PARMST);
	k->ptr_after = adbe_read(p, ADP_FIFO_PTR);
	for (i = 0; i < 32; i++) {
		ptr = adbe_read(p, ADP_FIFO_PTR) & ADP_FIFO_PTR_BUSY;
		parmst = FIELD_GET(ADP_PARMST_STATE, adbe_read(p, ADP_PARMST));
		k->burst_busy += !!ptr;
		k->burst_ptr_max = max(k->burst_ptr_max, ptr);
		k->parmst_seen |= BIT(parmst);
		udelay(5);
	}
	do {
		u16 frame;

		usleep_range(900, 1100);
		us = ktime_us_delta(ktime_get(), t0);
		frame = adbe_read(p, ADP_FRAME);
		status = adbe_read(p, ADP_STATUS);
		ptr = adbe_read(p, ADP_FIFO_PTR);
		parmst = adbe_read(p, ADP_PARMST);
		if (status & ADP_ST_SYNCED) {
			k->synced++;
			if (!k->first_sync_us)
				k->first_sync_us = max_t(s64, us, 1);
			writel(ADP_ST_SYNCED, p->adp + ADP_STATUS);
		}
		if (status & ADP_ST_APPLIED) {
			k->applied++;
			if (!k->first_applied_us)
				k->first_applied_us = max_t(s64, us, 1);
			writel(ADP_ST_APPLIED, p->adp + ADP_STATUS);
		}
		if (frame != previous && frames.changes == 0)
			k->frame_next = frame;
		adbe_count_frame(&frames, previous, frame);
		previous = frame;
		k->status_or |= status;
		k->errors |= status & ADP_ST_ERRORS;
		k->ptr_busy += !!(ptr & ADP_FIFO_PTR_BUSY);
		k->parmst_seen |= BIT(FIELD_GET(ADP_PARMST_STATE, parmst));
		if (k->ntrace < ADBE_TRACE)
			k->trace[k->ntrace++] = (struct adbe_trace) {
				.us = us, .status = status, .frame = frame,
				.ptr = ptr & ADP_FIFO_PTR_BUSY,
				.parmst = FIELD_GET(ADP_PARMST_STATE, parmst),
			};
		k->samples++;
	} while (frames.frames < ADBE_KICK_FRAMES && us < ADBE_KICK_POLL_US);
	k->frames = frames.frames;
	k->jumps = frames.jumps;
	k->base_after = adbe_read(p, ADP_BASE);
	if (k->errors)
		ret = -EIO;
	else if (!k->synced && !k->applied)
		ret = -ETIMEDOUT;
	else if (k->base_after != base)
		ret = -EREMOTEIO;
out:
	k->result = ret;
	dev_info(p->dev, "%s: packet=%s token=%s/%#x armed=%u words=%u base %#x; ParmSt %lu->%lu 0x1040 %#x->%#x; burst busy=%u max=%#x; poll %u samples %u frames ptr-busy=%u ParmSt-seen=%#x synced=%u@%uus applied=%u@%uus errors=%#x status-or=%#x; frame %u->%u jumps=%u; readback %#x; result %d\n",
		 what, adbe_commit_names[p->opt.commit],
		 adbe_token_names[p->opt.token], k->token, p->opt.arm, k->words,
		 base, FIELD_GET(ADP_PARMST_STATE, k->parmst_before),
		 FIELD_GET(ADP_PARMST_STATE, k->parmst_after),
		 k->ptr_before, k->ptr_after, k->burst_busy, k->burst_ptr_max,
		 k->samples, k->frames, k->ptr_busy, k->parmst_seen, k->synced,
		 k->first_sync_us, k->applied, k->first_applied_us, k->errors,
		 k->status_or, k->frame_before, k->frame_next, k->jumps,
		 k->base_after, ret);
	return ret;
}

/* Preconditions for any write to the live pipe. */
static int adbe_pipe_ok(struct adbe_probe *p)
{
	if ((adbe_read(p, ADP_SPDS) & 0xffff) != ADP_SPDS_VERSION ||
	    !adbe_format_known(adbe_read(p, ADP_FORMAT)) ||
	    !(adbe_read(p, ADP_VALID) & 1) ||
	    (adbe_read(p, ADP_STATUS) & ADP_ST_ERRORS)) {
		dev_warn(p->dev, "pipe not in the expected state; no writes\n");
		return -EBUSY;
	}
	return adbe_mode(p);
}

static void adbe_phase_c(struct adbe_probe *p)
{
	u32 base = adbe_read(p, ADP_BASE);
	bool pushed;

	p->phase_result = adbe_pipe_ok(p);
	if (p->phase_result)
		return;
	/* The exact live words: nothing on screen may change. */
	p->phase_result = adbe_kick(p, "C re-send", base,
				    adbe_read(p, ADP_STRIDE), &pushed);
	dev_info(p->dev, "phase C %s: one kick, expect one completion (0x400 applied), no jump, an unchanged picture\n",
		 p->phase_result ? "FAILED" : "done");
}

static void adbe_release_scratch(struct adbe_probe *p)
{
	if (p->scratch_io)
		iounmap(p->scratch_io);
	if (p->scratch_va)
		dma_free_wc(p->dev, p->bytes, p->scratch_va, p->scratch_dma);
	p->scratch_io = NULL;
	p->scratch_va = NULL;
}

/*
 * Last resort, also used by teardown: store the boot word directly, then
 * commit with an empty armed packet if the FIFO is ready. Caller holds lock.
 */
static void adbe_restore(struct adbe_probe *p)
{
	struct adbe_pkt empty = { .count = 0 };
	u32 parmst, ptr;

	if (!p->flipped)
		return;
	writel(p->saved_base, p->adp + ADP_BASE);
	if (!adbe_fifo_wait_ready(p->adp, 20, &parmst, &ptr))
		adbe_pkt_push(p->adp, &empty, ADBE_TOKEN_NEXT, ++p->serial, true);
	/* Let a scanout of the old buffer drain before it is released. */
	msleep(100);
	p->flipped = adbe_read(p, ADP_BASE) != p->saved_base;
	dev_info(p->dev, "restore by direct store: base %#x readback %s\n",
		 p->saved_base, p->flipped ? "DIFFERS (buffer kept)" : "matches");
	if (!p->flipped)
		adbe_release_scratch(p);
}

/*
 * band: copy the live frame and paint a band across the middle.
 * white/black: one solid colour, for a camera-free check with the
 * ambient light sensor (see the test plan).
 */
static int adbe_fill(struct adbe_probe *p, void __iomem *live)
{
	u32 band = p->height / 2 - ADBE_BAND_ROWS / 2;
	u32 *row;
	u32 x, y;

	row = kmalloc(p->stride, GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	if (p->opt.fill != ADBE_FILL_BAND)
		memset32(row, p->opt.fill == ADBE_FILL_WHITE ? 0x00ffffff : 0,
			 p->stride / 4);
	for (y = 0; y < p->height; y++) {
		if (p->opt.fill == ADBE_FILL_BAND)
			memcpy_fromio(row, live + y * p->stride, p->stride);
		if (p->opt.fill == ADBE_FILL_BAND &&
		    y >= band && y < band + ADBE_BAND_ROWS)
			for (x = 0; x < p->width; x++)
				row[x] = ADBE_BAND_COLOUR;
		if (p->scratch_io)
			memcpy_toio(p->scratch_io + y * p->stride, row, p->stride);
		else
			memcpy(p->scratch_va + y * p->stride, row, p->stride);
	}
	kfree(row);
	/* Publish every write-combined store before the base changes. */
	wmb();
	return 0;
}

static int adbe_scratch_get(struct adbe_probe *p, phys_addr_t *cpu)
{
	u64 reserved = p->vram.end - p->fb.start + 1;
	int ret;

	if (p->opt.scratch_dma) {
		ret = dma_set_mask_and_coherent(p->dev, DMA_BIT_MASK(36));
		if (ret)
			return ret;
		p->scratch_va = dma_alloc_wc(p->dev, p->bytes, &p->scratch_dma,
					     GFP_KERNEL);
		if (!p->scratch_va)
			return -ENOMEM;
		/* No IOMMU is attached, so the DMA address is the CPU address. */
		*cpu = p->scratch_dma;
		return 0;
	}
	if (p->fb.start < p->vram.start || p->fb.end > p->vram.end ||
	    !adbe_scratch_ok(p->bytes, p->opt.scratch_offset, reserved))
		return -ERANGE;
	*cpu = p->fb.start + p->opt.scratch_offset;
	/* Reserved no-map VRAM; WC like simpledrm, no region request. */
	p->scratch_io = ioremap_wc(*cpu, p->bytes);
	return p->scratch_io ? 0 : -ENOMEM;
}

/* Hold the flip, watching for errors or a stalled pipe; true if healthy. */
static bool adbe_hold(struct adbe_probe *p)
{
	unsigned long end = jiffies + msecs_to_jiffies(p->opt.hold_ms);
	u16 last = adbe_read(p, ADP_FRAME);
	unsigned long moved = jiffies;
	u32 status;

	memset(&p->hold, 0, sizeof(p->hold));
	p->hold.first = last;
	while (time_before(jiffies, end)) {
		u16 frame;

		usleep_range(10000, 12000);
		frame = adbe_read(p, ADP_FRAME);
		status = adbe_read(p, ADP_STATUS);
		p->hold.status_or |= status;
		adbe_count_frame(&p->hold, last, frame);
		if (frame != last)
			moved = jiffies;
		last = frame;
		if (status & ADP_ST_ERRORS) {
			dev_warn(p->dev, "phase D: error bits %#x during hold, reverting early\n",
				 status & ADP_ST_ERRORS);
			return false;
		}
		if (time_after(jiffies, moved + msecs_to_jiffies(200))) {
			dev_warn(p->dev, "phase D: frame counter stalled, reverting early\n");
			return false;
		}
	}
	p->hold.last = last;
	p->hold.base = adbe_read(p, ADP_BASE);
	return true;
}

static void adbe_phase_d(struct adbe_probe *p)
{
	void __iomem *live;
	phys_addr_t cpu;
	u32 base, stride, bus, bits;
	bool pushed;
	s64 covered;
	int ret;

	ret = adbe_pipe_ok(p);
	if (ret)
		goto out;
	base = adbe_read(p, ADP_BASE);
	stride = adbe_read(p, ADP_STRIDE);
	adbe_dart_snapshot(p);
	ret = adbe_scratch_get(p, &cpu);
	if (ret) {
		dev_warn(p->dev, "phase D: no %s buffer (%d)\n",
			 p->opt.scratch_dma ? "dma" : "vram", ret);
		goto out;
	}
	if (!adbe_cpu_to_bus(cpu, p->bus_offset, p->bytes, &bus)) {
		dev_warn(p->dev, "phase D: buffer %pa is outside the 32-bit pipe bus\n", &cpu);
		ret = -ERANGE;
		goto release;
	}
	covered = adbe_dart_reachable(p->dev, &p->dart, base & ~(ADBE_ALIGN - 1),
				      p->fb.start, bus, cpu, p->bytes);
	if (covered != p->bytes) {
		dev_warn(p->dev, "phase D: DART does not verify bus %#x -> %pa (%lld of %zu bytes)\n",
			 bus, &cpu, covered, p->bytes);
		ret = -EFAULT;
		goto release;
	}
	live = ioremap_wc(p->fb.start, p->bytes);
	if (!live) {
		ret = -ENOMEM;
		goto release;
	}
	ret = adbe_fill(p, live);
	iounmap(live);
	if (ret)
		goto release;
	if (adbe_read(p, ADP_BASE) != base ||
	    (adbe_read(p, ADP_STATUS) & ADP_ST_ERRORS)) {
		ret = -EIO;
		goto release;
	}
	bits = p->opt.tag < 0 ? base & (ADBE_ALIGN - 1) : (u32)p->opt.tag;
	p->test_base = adbe_tag_word(bus, bits);
	p->saved_base = base;
	p->flipped = true;
	dev_info(p->dev, "phase D: %s buffer (%s) CPU %pa bus %#x, flip %#x -> %#x, hold %u ms\n",
		 p->opt.scratch_dma ? "dma" : "vram", adbe_fill_names[p->opt.fill],
		 &cpu, bus, base, p->test_base, p->opt.hold_ms);
	ret = adbe_kick(p, "D flip", p->test_base, stride, &pushed);
	if (!pushed) {
		/* Nothing reached the pipe. */
		p->flipped = false;
		goto release;
	}
	/* Hold unless the pipe already reported a fatal error. */
	if (ret != -EIO && adbe_hold(p))
		dev_info(p->dev, "phase D hold: frames=%u jumps=%u status-or=%#x base=%#x\n",
			 p->hold.frames, p->hold.jumps, p->hold.status_or, p->hold.base);
	/* Revert through the same path, then the direct-store fallback. */
	if (!adbe_kick(p, "D revert", base, stride, &pushed)) {
		msleep(100);
		p->flipped = false;
		adbe_release_scratch(p);
	} else {
		adbe_restore(p);
	}
	dev_info(p->dev, "phase D %s: flip result %d, revert %s\n",
		 ret || p->flipped ? "FAILED" : "done", ret,
		 p->flipped ? "NOT confirmed (buffer kept, retried at unload)" : "confirmed");
	if (p->flipped)
		ret = -EIO;
	goto out;
release:
	adbe_release_scratch(p);
out:
	p->phase_result = ret;
}

static int adbe_regs_show(struct seq_file *s, void *unused)
{
	struct adbe_probe *p = s->private;
	static const u32 ranges[][2] = {
		{ 0x1000, 0x100c }, { 0x1014, 0x1080 },
		{ 0x4020, 0x40b0 }, { 0x6020, 0x60b0 },
	};
	u32 reg;
	unsigned int i;

	/* Skip the back-end FIFO ports and unknown CLCD apertures. */
	seq_printf(s, "ADP window %d (all reads; no W1C ack)\n", p->adp_index);
	for (i = 0; i < ARRAY_SIZE(ranges); i++)
		for (reg = ranges[i][0]; reg < ranges[i][1]; reg += 4)
			seq_printf(s, "%04x %08x\n", reg, adbe_read(p, reg));
	for (reg = 0; reg < p->size[5]; reg += 4)
		seq_printf(s, "side+%02x %08x\n", reg, readl(p->window[5] + reg));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(adbe_regs);

static void adbe_sample_show(struct seq_file *s, const char *name,
			     struct adbe_sample *sample)
{
	seq_printf(s, "%s frames=%u changes=%u first=%u last=%u vbi-samples=%u packet-samples=%u status-or=%#x base=%#x\n",
		   name, sample->frames, sample->changes, sample->first, sample->last,
		   sample->vbi_samples, sample->packet_samples, sample->status_or, sample->base);
}

static int adbe_state_show(struct seq_file *s, void *unused)
{
	struct adbe_probe *p = s->private;
	unsigned int i, j;

	mutex_lock(&p->lock);
	seq_printf(s, "window=%d mode=%ux%u stride=%u CPU-fb=%pa fb-bytes=%llu reserved=%pa..%pa bus-offset=%#llx\n",
		   p->adp_index, p->width, p->height, p->stride, &p->fb.start,
		   (unsigned long long)resource_size(&p->fb), &p->vram.start,
		   &p->vram.end, p->bus_offset);
	seq_printf(s, "phase=%c packet=%s token=%s armed=%u scratch=%s fill=%s hold=%ums result=%d flipped=%u saved-base=%#x test-base=%#x\n",
		   p->opt.phase, adbe_commit_names[p->opt.commit],
		   adbe_token_names[p->opt.token], p->opt.arm,
		   p->opt.scratch_dma ? "dma" : "vram", adbe_fill_names[p->opt.fill],
		   p->opt.hold_ms, p->phase_result, p->flipped, p->saved_base,
		   p->test_base);
	adbe_sample_show(s, "D1-1s", &p->d1);
	adbe_sample_show(s, "D-hold", &p->hold);
	seq_printf(s, "current frame=%u status=%#x mask=%#x base=%#x 0x101c=%#x 0x1040=%#x IRQ-index=%d hits=%d vbi=%d synced=%d status-or=%#x\n",
		   adbe_read(p, ADP_FRAME) & 0xffff, adbe_read(p, ADP_STATUS),
		   adbe_read(p, ADP_INTEN), adbe_read(p, ADP_BASE),
		   adbe_read(p, ADP_PARMST), adbe_read(p, ADP_FIFO_PTR), p->opt.irq_index,
		   atomic_read(&p->irq_hits), atomic_read(&p->irq_vbi),
		   atomic_read(&p->irq_synced), atomic_read(&p->irq_status));
	if (p->dart.read)
		for (i = 0; i < ADBE_DART_WINDOWS; i++)
			seq_printf(s, "DART win%u tcr=%#010x error=%#x\n", i,
				   p->dart.tcr[i], p->dart.error[i]);
	for (i = 0; i < p->nkicks; i++) {
		struct adbe_kick *k = &p->kick[i];

		seq_printf(s, "kick %u %s: result=%d words=%u token=%#x base=%#x readback=%#x synced=%u@%uus applied=%u@%uus errors=%#x frames=%u frame %u->%u jumps=%u\n",
			   i, k->what, k->result, k->words, k->token, k->base,
			   k->base_after, k->synced, k->first_sync_us, k->applied,
			   k->first_applied_us, k->errors, k->frames, k->frame_before,
			   k->frame_next, k->jumps);
		seq_printf(s, "  before 0x101c=%#x 0x1040=%#x status=%#x; after 0x101c=%#x 0x1040=%#x; burst busy=%u max=%#x\n",
			   k->parmst_before, k->ptr_before, k->status_before,
			   k->parmst_after, k->ptr_after, k->burst_busy, k->burst_ptr_max);
		for (j = 0; j < k->ntrace; j++)
			seq_printf(s, "  t=%6uus frame=%5u ParmSt=%u ptr=%#06x status=%#06x\n",
				   k->trace[j].us, k->trace[j].frame, k->trace[j].parmst,
				   k->trace[j].ptr, k->trace[j].status);
	}
	mutex_unlock(&p->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(adbe_state);

static int adbe_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	struct adbe_probe *p = container_of(nb, struct adbe_probe, reboot_nb);

	mutex_lock(&p->lock);
	adbe_restore(p);
	mutex_unlock(&p->lock);
	return NOTIFY_DONE;
}

static void adbe_quiesce(struct adbe_probe *p)
{
	mutex_lock(&p->lock);
	adbe_restore(p);
	if (p->flipped)
		dev_crit(p->dev, "base not restored; buffer intentionally leaked\n");
	mutex_unlock(&p->lock);
}

static int adbe_suspend(struct device *dev)
{
	adbe_quiesce(dev_get_drvdata(dev));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(adbe_pm_ops, adbe_suspend, NULL);

static const char * const adbe_phase_names[] = { "a", "c", "d" };

/*
 * Validate and copy the parameters for this bind; writes may race. String
 * matches tolerate the newline that "echo > parameters/x" leaves.
 */
static int adbe_params(struct adbe_probe *p)
{
	struct adbe_opts *o = &p->opt;
	int which, commit, policy, content, ret = -EINVAL;

	kernel_param_lock(THIS_MODULE);
	if (irq_index < -1 || irq_index >= 5 ||
	    (irq_mask & ~(ADP_ST_VBI | ADP_ST_SYNCED)) ||
	    tag > 0x3f || tag < -1 || hold_ms > ADBE_HOLD_MAX_MS ||
	    !(sysfs_streq(scratch, "vram") || sysfs_streq(scratch, "dma")))
		goto out;
	which = sysfs_match_string(adbe_phase_names, phase);
	commit = sysfs_match_string(adbe_commit_names, packet);
	policy = sysfs_match_string(adbe_token_names, token);
	content = sysfs_match_string(adbe_fill_names, fill);
	if (which < 0 || commit < 0 || policy < 0 || content < 0)
		goto out;
	*o = (struct adbe_opts) {
		.irq_index = irq_index,
		.irq_mask = irq_mask,
		.dart = dart,
		.phase = adbe_phase_names[which][0],
		.commit = commit,
		.token = policy,
		.arm = arm,
		.fill = content,
		.scratch_dma = sysfs_streq(scratch, "dma"),
		.scratch_offset = scratch_offset,
		.tag = tag,
		.hold_ms = hold_ms,
	};
	ret = 0;
out:
	kernel_param_unlock(THIS_MODULE);
	return ret;
}

static int adbe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adbe_probe *p;
	struct resource *res;
	int i, ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	ret = adbe_params(p);
	if (ret)
		return dev_err_probe(dev, ret, "bad module parameters\n");
	mutex_init(&p->lock);
	platform_set_drvdata(pdev, p);
	for (i = 0; i < ADBE_WINDOWS; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res || resource_size(res) < (i == 5 ? 0x1c : 0x8000))
			return -EINVAL;
		p->size[i] = resource_size(res);
		/* Honours the bus's nonposted-mmio, needed for the writes. */
		p->window[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(p->window[i]))
			return PTR_ERR(p->window[i]);
	}
	ret = adbe_discriminate(p);
	if (ret)
		return ret;
	ret = adbe_framebuffer(p);
	if (ret)
		return ret;
	ret = adbe_mode(p);
	if (ret)
		return ret;
	adbe_dump_log(p, 0x1000, 0x1080);
	adbe_dump_log(p, 0x4020, 0x40b0);
	adbe_dump_log(p, 0x6020, 0x60b0);
	adbe_sample(p, 1000, &p->d1);
	dev_info(dev, "D1 1s: frames=%u changes=%u VBI-samples=%u packet-samples=%u status-or=%#x\n",
		 p->d1.frames, p->d1.changes, p->d1.vbi_samples,
		 p->d1.packet_samples, p->d1.status_or);
	if (!p->d1.frames)
		return -ENODEV;
	ret = adbe_phase_b(pdev, p);
	if (ret)
		return dev_err_probe(dev, ret, "phase B failed\n");
	p->reboot_nb.notifier_call = adbe_reboot;
	ret = register_reboot_notifier(&p->reboot_nb);
	if (ret)
		return ret;
	mutex_lock(&p->lock);
	if (p->opt.dart)
		adbe_dart_snapshot(p);
	if (p->opt.phase == 'c')
		adbe_phase_c(p);
	else if (p->opt.phase == 'd')
		adbe_phase_d(p);
	mutex_unlock(&p->lock);
	p->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("regs", 0400, p->debugfs, p, &adbe_regs_fops);
	debugfs_create_file("state", 0400, p->debugfs, p, &adbe_state_fops);
	dev_info(dev, "ready: /sys/kernel/debug/%s phase=%c packet=%s token=%s result=%d (rebind to run again)\n",
		 dev_name(dev), p->opt.phase, adbe_commit_names[p->opt.commit],
		 adbe_token_names[p->opt.token], p->phase_result);
	return 0;
}

static void adbe_remove(struct platform_device *pdev)
{
	struct adbe_probe *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->debugfs);
	unregister_reboot_notifier(&p->reboot_nb);
	adbe_quiesce(p);
}

static void adbe_shutdown(struct platform_device *pdev)
{
	adbe_quiesce(platform_get_drvdata(pdev));
}

/*
 * No MODULE_DEVICE_TABLE: the KMS driver owns this node at boot, and the
 * probe is loaded by hand only (after unbinding apple-adbe).
 */
static const struct of_device_id adbe_of_match[] = {
	{ .compatible = "adbe0,s8000" },
	{}
};

static struct platform_driver adbe_driver = {
	.probe = adbe_probe,
	.remove = adbe_remove,
	.shutdown = adbe_shutdown,
	.driver = {
		.name = "apple-adbe-probe",
		.of_match_table = adbe_of_match,
		.pm = pm_sleep_ptr(&adbe_pm_ops),
	},
};
module_platform_driver(adbe_driver);

MODULE_DESCRIPTION("A9 display pipe probe with opt-in FIFO and flip measurements");
MODULE_AUTHOR("linux-a9 contributors");
MODULE_LICENSE("GPL");
