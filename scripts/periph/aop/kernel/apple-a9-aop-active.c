// SPDX-License-Identifier: GPL-2.0-only
/*
 * J71t AOP active driver: loads and starts the AOP firmware, runs the old
 * RTBuddy management protocol and the SPU/AFK v8 endpoint transport over the
 * A7IOPV2 64-bit mailbox, binds the "accel" service and exposes it as an IIO
 * accelerometer. Every message word follows docs/periph/ds-aop-accel.md.
 *
 * Default off twice: CONFIG_APPLE_A9_AOP_ACTIVE and the immutable boot
 * parameter apple_a9_aop.active=1, or the independent experimental selector
 * attach_stage=2/3. Otherwise probe returns before mapping anything.
 * The passive observer is a separate driver.
 *
 * Linux RTKit (drivers/soc/apple/rtkit.c) is not reused: it carries the
 * endpoint in a separate 32-bit mailbox word and its management type field
 * (bits 59:52) overlaps this transport's endpoint byte (bits 63:56); the
 * EPMAP base and AFK words also differ. This file keeps its own small codec.
 *
 * Never clears mailbox faults and never forces a power
 * domain on. The old opt-in running attach requires a fingerprint/reservation
 * proof. Stages 2/3 deliberately bypass it under owner-accepted crash risk,
 * use polling and reserved AFK RAM. The separate aop_force_cold=1 selector
 * stops the core before cold loading; failure to halt is terminal.
 */
#include <crypto/sha2.h>
#include <linux/bitfield.h>
#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmgr-observe.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define A9_RING_LOAD32(p) le32_to_cpu(READ_ONCE(*(const __le32 *)(p)))
#define A9_RING_STORE32(v, p) WRITE_ONCE(*(__le32 *)(p), cpu_to_le32(v))
#include "a9-aop-afk.h"
#include "a9-aop-iio.h"
#include "a9-aop.h"
#include "a9-aop-takeover.h"
#include "a9-aop-diagnostics.h"
#include "a9-aop-memory.h"
#include "a9-aop-experiment.h"
#include "a9-aop-system.h"
#include "a9-aop-handoff.h"
#include "a9-aop-reload.h"

/* Shares the observer's parameter namespace: apple_a9_aop.active=1. */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "apple_a9_aop."

static bool active;
module_param(active, bool, 0400);
MODULE_PARM_DESC(active, "Boot the AOP and expose the accelerometer (default off)");

static bool cycle;
module_param(cycle, bool, 0400);
MODULE_PARM_DESC(cycle, "Append one management 0x201/0x220 sleep/wake pair to stages 2/3 (default off)");

static unsigned int wake;
module_param(wake, uint, 0400);
MODULE_PARM_DESC(wake, "Stages 2/3 one-shot wake: 1=IOP masks, 2=inbox mask, 3=CPU run, 4=CPU run+auto-boot, 5=combined (default 0)");

/* Deliberately short, immutable boot opt-in; independent of running attach. */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX ""
static bool force_cold;
module_param_named(aop_force_cold, force_cold, bool, 0400);
MODULE_PARM_DESC(aop_force_cold, "One forced AOP stop/cold load with attach_stage=2/3 (default off)");
static unsigned int cold_test;
module_param_named(aop_cold_test, cold_test, uint, 0400);
MODULE_PARM_DESC(aop_cold_test, "Forced cold bits: 1=D-cache off, 2=IOP IRQs, 4=real ECID, 8=fresh NONC (requires 4)");
static unsigned int cold_defer;
module_param_named(aop_defer, cold_defer, uint, 0400);
MODULE_PARM_DESC(aop_defer, "Forced cold: 1=sysfs trigger with AP IRQs masked, 2=also park after PRNG (default 0)");
static unsigned int cold_run;
module_param_named(aop_run, cold_run, uint, 0400);
MODULE_PARM_DESC(aop_run, "Deferred cold stage3: 1=system endpoints and power, 2=also accel/IIO (default 0)");
static bool quiesce;
module_param_named(aop_quiesce, quiesce, bool, 0400);
MODULE_PARM_DESC(aop_quiesce, "Stage3 shutdown: ACK rates/AFK, AP quiesce, IOP resumable sleep, halt and drain (default off)");
static char *handoff_mode;
module_param_named(aop_handoff, handoff_mode, charp, 0400);
MODULE_PARM_DESC(aop_handoff, "Stage3 warm handoff: sleep = quiesce to IOP sleep without halt, record, then wake/adopt in the next kernel (default off)");
static bool spi0_reset;
module_param_named(aop_spi0_reset, spi0_reset, bool, 0400);
MODULE_PARM_DESC(aop_spi0_reset, "Stage3 reload of a live AOP: after halt/drain, replay firmware SPI0 reset 0102f2f4 if firmware ran since SoC reset (auto-boot, or previous shutdown transcript bound by +0x4020), with AOP IRQ 0x10014 disabled first (mask readback checked), then channel-3 teardown 0102e4f8 (idle wait <= 20 ms, twice the firmware's own bound); no load unless +0x8d0 bit 6 clears and every teardown readback holds, no RUN unless bit 6 stays clear >= 150 ms after the copy, then 0x10014 is acknowledged and INTC event-3 reset is replayed before RUN (default off)");
static bool wake_wrapper;
module_param_named(aop_wake_wrapper, wake_wrapper, bool, 0400);
MODULE_PARM_DESC(aop_wake_wrapper, "aop_handoff=sleep: replay the iOS resume wrapper masks and outbox enable before 0x220; no RUN, no NMI (default off)");
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "apple_a9_aop."

#define AOP_MAIN_FW		"apple/aop/ipad6faop-20H350.bin"
#define AOP_EXT_FW		"apple/aop/ipad6faop_ext-20H350.bin"
#define AOP_ECID_FW		"apple/aop/boot-ecid.bin"
#define AOP_CAL_FW		"apple/aop/accel-calibration.bin"

/* Wrapper offsets (AppleA7IOPV2, docs/periph/aop-sensors.md §1). */
#define AOP_REMAP_PA_LO		0x08
#define AOP_REMAP_PA_HI		0x10
#define AOP_REMAP_IOP_LO	0x18
#define AOP_REMAP_IOP_HI	0x20
#define AOP_REMAP_SIZE		0x28
#define AOP_REMAP_RESERVED	0x30
#define AOP_REMAP_ENABLE	0x38
#define AOP_CPU_CONTROL		0x44
#define AOP_CPU_START		BIT(4)
/* Vendor running test, but mirrors inbox on J71t: no CPU execution proof. */
#define AOP_WRAPPER_B88		0xb88
#define AOP_B88_BIT0		BIT(0)
#define AOP_HALT_DELAY_MS	10
#define AOP_IRQ_MASK_SET	0x4000
#define AOP_IRQ_MASK_CLEAR	0x4004
#define AOP_IRQ_MIRROR_SET	0xc00
#define AOP_IRQ_MIRROR_CLEAR	0xc04
#define AOP_IRQ_ALL		0x1111
#define AOP_IRQ_BIT0		0x0001
#define AOP_IRQ_BIT12		0x1000
#define AOP_INBOX_CONTROL	0x4008
#define AOP_INBOX_DATA		0x4010
/* Receive half of the same FIFO, inferred from pinned firmware 0102ce1c.
 * AP access is experimental: use only after the stop-status gate succeeds.
 */
#define AOP_INBOX_POP		0x4018
#define AOP_OUTBOX_CONTROL	0x4020
#define AOP_OUTBOX_DATA		0x4038
#define AOP_IDLE_STATUS		0x4040
#define AOP_IRQ_IOP_INBOX		BIT(4)
#define AOP_IRQ_IOP_BOTH		(BIT(4) | BIT(8))
#define AOP_WRAPPER_SIZE	0x1c000
#define AOP_MBOX_FULL		BIT(16)
#define AOP_MBOX_EMPTY		BIT(17)
#define AOP_MBOX_FAULTS		GENMASK(19, 18)
#define AOP_OUTBOX_ENABLE	BIT(0)

/* Firmware: linked at IOP 0x01000000, main image at region + 0. */
#define AOP_IOP_LINK_BASE	0x01000000
#define AOP_TAG(a, b, c, d)	A9_FW_TAG(a, b, c, d)
#define AOP_TAG_RTSZ		A9_FW_TAG('R', 'T', 'S', 'Z')

#define AOP_DOMAINS		5
#define AOP_MAX_EPS		32
#define AOP_MAX_SERVICES	64
#define AOP_TRACE		512
#define AOP_RXQ			256
#define AOP_PACKET_MAX		4096
#define AOP_DRAIN_BUDGET	64
#define AOP_GETBUF_MAX		SZ_1M
#define AOP_FW_WAIT_S		180
#define AOP_HELLO_MS		5000
/* RTBuddy_waitForBootState uses 20 s. Retained sleep gets one wake, no retry. */
#define AOP_COLD_HELLO_MS	20000
#define AOP_COLD_WAKE_MS	1000
#define AOP_COLD_FRAMES		8
#define AOP_EPMAP_MS		2000
#define AOP_IOP_READY_MS	2000
#define AOP_SERVICE_MS		5000
#define AOP_REPLY_MS		2000
#define AOP_REPORT_MS		3000
#define AOP_STOP_MS		200
/* Quiesce: one AFK stop ACK, then the whole sequential stop, are bounded. */
#define AOP_QUIESCE_EP_MS	2000
#define AOP_QUIESCE_AFK_MS	10000
#define AOP_PING_MS		1000
#define AOP_PING_TIMEOUT_MS	5000
#define AOP_ACCEL_SERVICE_ID	0x10000007
#define AOP_ACCEL_EP		0x21 /* saved AOPEndpoint2: wire = name index + 0x1f */
#define AOP_RATE_US		100000
#define AOP_KEY_REPORT		0x00
#define AOP_KEY_BATCH		0x16
#define AOP_KEY_RAW		0x5c
#define AOP_KEY_AROT		0x18
#define AOP_KEY_CAL_STATE	0x3d
#define AOP_IRQ_STORM		1000

static const char * const domain_names[AOP_DOMAINS] = {
	"aop_gpio", "aop", "aop_busif", "aop_cpu", "aop_filter",
};

static atomic_t experiment_attempted = ATOMIC_INIT(0);
/* aop_handoff=sleep, validated once at probe; excludes aop_quiesce. */
static bool handoff;

/* Only the reserved 0x9b000 SRAM aperture; never follow firmware pointers. */
static const struct {
	const char *name;
	u32 begin, end;
} a9_cold_spans[] = {
	{ "boot", 0, 0x1000 },
	{ "text-tags", 0x1000, 0x3a000 },
	{ "data", 0x3a000, 0x53940 },
	{ "bss", 0x53940, 0x6e000 },
	{ "scratch", 0x6e000, A9_ATTACH_REGION_SIZE },
};

/* Status/control only, including both old FIFO aliases. No data-register reads. */
static const u32 a9_cold_reg_offsets[] = {
	0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x44,
	0xb80, 0xb88, 0xba0, 0xc00, 0xc04,
	0x4000, 0x4004, 0x4008, 0x4020, 0x4040,
};

struct a9_aop_cold_frame {
	u64 ns;
	const char *event;
	int error;
	u32 regs[ARRAY_SIZE(a9_cold_reg_offsets)];
	u32 auto_boot, guard, tag_guard;
	u8 sha256[SHA256_DIGEST_SIZE];
	struct a9_aop_memory_delta delta[ARRAY_SIZE(a9_cold_spans)];
};

enum a9_aop_phase {
	PHASE_IDLE,
	PHASE_PROBE,
	PHASE_ATTACH_PREFLIGHT,
	PHASE_ATTACH_PING,
	PHASE_ATTACH_HELLO,
	PHASE_WAIT_FIRMWARE,
	PHASE_WAIT_HELLO,
	PHASE_WAIT_EPMAP,
	PHASE_WAIT_SERVICE,
	PHASE_CONFIGURE,
	PHASE_WAIT_REPORT,
	PHASE_STREAMING,
	PHASE_SYSTEM_READY,
	PHASE_STOPPED,
	PHASE_REFUSED,
	PHASE_FAILED,
};

static const char * const phase_names[] = {
	"idle", "probe", "attach-preflight", "attach-ping", "attach-hello",
	"wait-firmware", "wait-hello", "wait-epmap",
	"wait-service", "configure", "wait-report", "streaming", "system-ready", "stopped",
	"refused", "failed",
};

enum a9_afk_state {
	AFK_OFF,	/* in EPMAP, not started */
	AFK_WAIT_INIT,	/* STARTEP sent, waiting 0x80 */
	AFK_INIT,	/* 0xa0 sent, collecting buffers/rings */
	AFK_STARTING,	/* 0xa3 sent */
	AFK_RUNNING,	/* 0x86 received */
	AFK_STOPPING,	/* 0xa4 sent */
	AFK_STOPPED,	/* 0x87/0xc3 received */
	AFK_FAILED,
};

static const char * const afk_names[] = {
	"off", "wait-init", "init", "starting", "running", "stopping",
	"stopped", "failed",
};

enum { RING_TX, RING_RX, RING_HIST, RING_COUNT };
static const char * const ring_names[RING_COUNT] = { "tx", "rx", "hist" };

struct a9_aop_buffer {
	u16 tag;
	size_t size;
	void *va;
	dma_addr_t dma;
};

struct a9_aop_system {
	struct a9_aop_buffer buffer;
	bool started;
	u32 entries, message_size;
	u64 words, acks, errors, unknown;
};

struct a9_aop_ep {
	u8 num;
	enum a9_afk_state state;
	struct a9_aop_buffer bufs[A9_AFK_MAX_BUFFERS];
	unsigned int nbufs;
	struct a9_ring rings[RING_COUNT];
	bool ring_valid[RING_COUNT];
	u16 ring_tag[RING_COUNT];
	u32 ring_offset[RING_COUNT];
	u32 ring_size[RING_COUNT];
	u8 *ring_seen[RING_COUNT];	/* kept for debugfs even if invalid */
	u64 magic_unexpected;
	u32 last_word;
	u16 outer_seq;
	u64 dropped_before_init, unknown, rx_packets, rx_errors, tx_packets;
	int last_rx_error;
};

struct a9_aop_service {
	u8 ep;
	u32 channel;
	struct a9_spu_service info;
};

struct a9_aop_trace {
	u64 ns;
	u64 word;
	char dir;
	int status;
	enum a9_aop_phase phase;
};

/* Status only. FIFO data registers are read/written solely by drain/send. */
static const u32 a9_exp_reg_offsets[] = {
	AOP_CPU_CONTROL, AOP_WRAPPER_B88, AOP_IRQ_MASK_SET, AOP_IRQ_MASK_CLEAR,
	AOP_IRQ_MIRROR_SET, AOP_IRQ_MIRROR_CLEAR, AOP_INBOX_CONTROL,
	AOP_OUTBOX_CONTROL, AOP_IDLE_STATUS,
};

struct a9_exp_regs {
	u64 ns;
	const char *step, *event;
	u32 value[ARRAY_SIZE(a9_exp_reg_offsets)];
	u32 auto_boot;
	bool auto_boot_valid;
};

struct a9_aop {
	struct device *dev;
	void __iomem *wrapper;
	void __iomem *region;
	void __iomem *autoboot;
	resource_size_t region_pa;
	resource_size_t region_size;
	resource_size_t wrapper_pa;
	u32 chip_id, chip_revision, runtime_size;
	void *ext;
	dma_addr_t ext_dma;
	size_t ext_size;
	struct device_node *domains[AOP_DOMAINS];
	u32 aop_target;
	int irq;

	spinlock_t tx_lock;	/* inbox FIFO */
	spinlock_t rx_lock;	/* outbox FIFO, rxq, trace */
	DECLARE_KFIFO(rxq, u64, AOP_RXQ);
	struct a9_aop_trace trace[AOP_TRACE];
	struct a9_aop_trace startup_trace[AOP_TRACE];
	u64 trace_total;
	unsigned int startup_count;
	unsigned int experiment_stage, epmap_seen;
	unsigned int cold_run;
	struct a9_aop_system system[A9_SYS_COUNT];
	bool system_ready, iop_crashed;
	u64 system_grants;
	const char *stage3_checkpoint;
	bool speculative_hello, trace_exhausted, handshake_complete;
	const char *experiment_step, *experiment_winner;
	unsigned int device_hellos, speculative_replies;
	unsigned int reg_count, reg_dropped;
	bool mailbox_unmasked;
	struct a9_exp_regs reg_trace[A9_EXP_REG_TRACE];
	void *pool;
	phys_addr_t pool_pa;
	size_t pool_size, pool_used;
	struct resource pool_resource;
	struct {
		u64 ns;
		struct a9_aop_motion motion;
		u32 size;
		u8 raw[64];
	} samples[A9_EXP_SAMPLES];
	u64 irq_count, irq_empty, irq_empty_run, rx_words, tx_words;
	u64 rxq_full, fault_word;
	bool irq_enabled, irq_storm;
	bool rx_halt;		/* refused: stop consuming the outbox */
	bool rx_backlog;	/* rxq was full with words left in the FIFO */
	bool irq_paused;	/* IRQ disabled until rx_work drained rxq */
	u32 fault_control;

	struct workqueue_struct *wq;
	struct work_struct rx_work;
	struct work_struct boot_work;
	struct delayed_work poll_work;
	struct delayed_work ping_work;
	wait_queue_head_t wait;
	bool stopping;
	bool inline_rx;		/* shutdown: drain and dispatch synchronously */
	bool shutdown_quiesced, shutdown_power_requested;
	unsigned int poll_ms;
	struct a9_aop_handoff ho;
	struct a9_aop_spi0 spi0;	/* aop_spi0_reset=1 evidence */
	struct device_node *spi0_pmgr;	/* ps_spi0, observed read-only */
	int wake_wrapper_errno;
	bool wake_wrapper_done;
	u8 *crash_copy;		/* frozen full RTKit crashlog buffer */
	size_t crash_size;

	struct mutex lock;	/* protocol state below */
	enum a9_aop_phase phase;
	const char *result;
	int error;
	bool cold;
	bool cold_triggered;
	bool force_halted, force_drained;
	u32 force_halt_control;
	unsigned int stale_inbox, stale_outbox;
	u8 *cold_before, *cold_after;
	u64 cold_release_ns;
	unsigned int cold_frames;
	bool cold_baseline, cold_wake_sent;
	int cold_wake_errno;
	struct a9_aop_cold_frame cold_frame[AOP_COLD_FRAMES];
	bool running_attach;
	struct a9_attach takeover;
	struct a9_attach_fingerprint fingerprint;
	/* Frozen Phase A evidence; debugfs never re-reads refused hardware. */
	struct {
		u8 prefix[A9_DIAG_PREFIX_SIZE];
		u8 expected_prefix[A9_DIAG_PREFIX_SIZE];
		struct a9_attach_fingerprint expected;
		u32 remap[A9_ATTACH_REMAP_WORDS];
		u32 remap_after[A9_ATTACH_REMAP_WORDS];
		bool mapping_matches;
		bool captured, reference_pinned;
		int reference_errno;
		int read_errno;
	} diagnostics;
	bool fingerprint_read, image_verified, tags_verified, ext_reserved, ext_verified;
	u32 snapshot[6];
	bool hello, epmap_done;
	u16 hello_min, hello_max;
	unsigned int epmap_chunks;
	DECLARE_BITMAP(present, 256);
	struct a9_aop_ep eps[AOP_MAX_EPS];
	unsigned int neps;
	u32 ping_seq;
	u64 ping_sent_ns, ping_ack_ns;
	u64 ping_acks;
	u32 ap_power_ack;
	bool ap_power_acked;
	u32 iop_power;
	bool iop_ready;
	u64 system_words, unknown_mgmt, unknown_ep;
	struct a9_aop_service services[AOP_MAX_SERVICES];
	unsigned int nservices;
	int accel;		/* index into services, or -1 */
	u16 cmd_tag;
	bool cmd_pending;
	int cmd_status;
	unsigned int cal_sent, cal_unsupported;
	u32 cal_state;
	bool arot_sent, rate_set;
	u64 reports, report_errors, channel_mismatch, trailing, malformed;
	u64 truncated, doorbells, replies_seen;
	u8 reply_category;
	u16 reply_subtype, reply_tag;
	u32 reply_size, reply_word;
	struct a9_aop_motion motion;
	u8 first_report[64];
	u32 first_report_size;
	bool have_first_report;
	struct iio_dev *iio;
	u8 rx_packet[AOP_PACKET_MAX];
	u8 tx_packet[AOP_PACKET_MAX];
	u8 *table;		/* patched firmware tag table, for debugfs */
	size_t table_size;
	struct dentry *debugfs;
};

static inline u32 aop_rd(struct a9_aop *aop, u32 offset)
{
	return readl(aop->wrapper + offset);
}

static int a9_aop_control_write(struct a9_aop *aop, void __iomem *target,
			       u32 offset, u32 value);

static inline int aop_wr(struct a9_aop *aop, u32 offset, u32 value)
{
	if (force_cold)
		return a9_aop_control_write(aop, aop->wrapper + offset, offset, value);
	if (WARN_ON_ONCE(aop->experiment_stage))
		return -EPERM;
	writel(value, aop->wrapper + offset);
	return 0;
}

struct a9_aop_io32 {
	void __iomem *base;
};

static u32 a9_aop_io_read32(void *context, size_t offset)
{
	struct a9_aop_io32 *io = context;

	return readl(io->base + offset);
}

/* Used only by the existing stopped-CPU loader, never by running attach. */
static void a9_aop_io_write32(void *context, size_t offset, u32 value)
{
	struct a9_aop_io32 *io = context;

	writel(value, io->base + offset);
}

static int a9_aop_fromio32(void __iomem *base, size_t aperture_size,
			   u8 *destination, size_t offset, size_t size)
{
	struct a9_aop_io32 io = { .base = base };

	return a9_aop_copy_from32(&io, a9_aop_io_read32, destination,
				  offset, size, aperture_size);
}

static void a9_aop_trace(struct a9_aop *aop, char dir, u64 word, int status)
{
	struct a9_aop_trace *t;

	lockdep_assert_held(&aop->rx_lock);
	t = &aop->trace[aop->trace_total++ % AOP_TRACE];
	t->ns = ktime_get_boottime_ns();
	t->word = word;
	t->dir = dir;
	t->status = status;
	t->phase = READ_ONCE(aop->phase);
	if (aop->experiment_stage) {
		if (aop->startup_count < AOP_TRACE &&
		    aop->phase != PHASE_STREAMING)
			aop->startup_trace[aop->startup_count++] = *t;
		if (!aop->cold_run || aop->startup_count < AOP_TRACE)
			dev_info(aop->dev, "stage%u %c %016llx status=%d phase=%s\n",
				 aop->experiment_stage, dir, word, status,
				 phase_names[t->phase]);
	}
	if (apple_a9_aop_debug())
		dev_dbg(aop->dev, "%c %016llx %d\n", dir, word, status);
}

/* Must be called with aop->lock held; first result wins. */
static void a9_aop_finish(struct a9_aop *aop, enum a9_aop_phase phase,
			  const char *result, int error)
{
	if (aop->phase == PHASE_REFUSED || aop->phase == PHASE_FAILED)
		return;
	aop->phase = phase;
	aop->result = result;
	aop->error = error;
	if (phase == PHASE_REFUSED ||
	    (aop->experiment_stage &&
	     (!aop->cold_run || aop->fault_control || aop->iop_crashed)))
		WRITE_ONCE(aop->rx_halt, true);
	if (aop->iio)
		a9_aop_accel_invalidate(aop->iio);
	dev_warn(aop->dev, "%s: %s (%d)\n", phase_names[phase], result, error);
	wake_up_all(&aop->wait);
}

#define a9_aop_fail(aop, result, error) \
	a9_aop_finish(aop, PHASE_FAILED, result, error)
#define a9_aop_refuse(aop, result) \
	a9_aop_finish(aop, PHASE_REFUSED, result, -EBUSY)

static bool a9_aop_dead(struct a9_aop *aop)
{
	return READ_ONCE(aop->phase) == PHASE_FAILED ||
	       READ_ONCE(aop->phase) == PHASE_REFUSED ||
	       READ_ONCE(aop->stopping);
}

static void a9_aop_experiment_regs(struct a9_aop *aop, const char *event)
{
	struct a9_exp_regs regs;
	unsigned long flags;
	unsigned int i;
	bool retained = false;

	if (!aop->experiment_stage || READ_ONCE(aop->phase) == PHASE_STREAMING)
		return;
	regs.ns = ktime_get_boottime_ns();
	regs.step = READ_ONCE(aop->experiment_step) ?: "stage3";
	regs.event = event;
	for (i = 0; i < ARRAY_SIZE(a9_exp_reg_offsets); i++)
		regs.value[i] = aop_rd(aop, a9_exp_reg_offsets[i]);
	regs.auto_boot_valid = !!aop->autoboot;
	regs.auto_boot = aop->autoboot ? readl(aop->autoboot) : 0;
	spin_lock_irqsave(&aop->rx_lock, flags);
	if (aop->reg_count < A9_EXP_REG_TRACE) {
		aop->reg_trace[aop->reg_count++] = regs;
		retained = true;
	} else {
		aop->reg_dropped++;
	}
	if ((regs.value[6] | regs.value[7]) & AOP_MBOX_FAULTS)
		aop->fault_control = regs.value[6] | regs.value[7];
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	if (retained)
		dev_info(aop->dev, "REG step=%s event=%s ns=%llu cpu44=%08x rawb88=%08x set4000=%08x clr4004=%08x setc00=%08x clrc04=%08x in4008=%08x out4020=%08x idle4040=%08x auto_boot=%08x valid=%d\n",
			 regs.step, event, regs.ns, regs.value[0], regs.value[1],
			 regs.value[2], regs.value[3], regs.value[4], regs.value[5],
			 regs.value[6], regs.value[7], regs.value[8],
			 regs.auto_boot, regs.auto_boot_valid);
}

/* All forced-stop/loader MMIO stores have ordered before/after evidence. */
static int a9_aop_control_write(struct a9_aop *aop, void __iomem *target,
			       u32 offset, u32 value)
{
	a9_aop_experiment_regs(aop, "control-before");
	if (aop->fault_control || aop->rx_halt ||
	    (a9_aop_dead(aop) && !(quiesce && aop->inline_rx &&
				  aop->shutdown_quiesced &&
				  aop->phase == PHASE_STOPPED)))
		return -EIO;
	dev_info(aop->dev, "COLD-W step=%s bank=%s offset=%04x before=%08x value=%08x\n",
		 aop->experiment_step, target == aop->autoboot ? "aon" : "wrapper",
		 offset, readl(target), value);
	writel(value, target);
	dev_info(aop->dev, "COLD-R offset=%04x after=%08x\n", offset, readl(target));
	a9_aop_experiment_regs(aop, "control-after");
	return aop->fault_control ? -EIO : 0;
}

/* RUN is a command readback, not an independent PC/DMA halt indication.
 * b4e4b8 reads +b88 bit0, but J71t mirrors inbox there (including occupancy).
 * No reliable CPU status was found; owner accepts RUN=0 after a fixed delay.
 */
static bool a9_aop_force_stopped(struct a9_aop *aop)
{
	return !(aop_rd(aop, AOP_CPU_CONTROL) & AOP_CPU_START) &&
	       !readl(aop->autoboot) &&
	       !(aop_rd(aop, AOP_OUTBOX_CONTROL) &
		 (AOP_OUTBOX_ENABLE | AOP_MBOX_FAULTS)) &&
	       !(aop_rd(aop, AOP_INBOX_CONTROL) & AOP_MBOX_FAULTS);
}

/* AppleA7IOP::stop b4b8fc + setCPURun b4e7c8. V2 has no reset hook.
 * No management sleep TX: the inherited inbox may already be full.
 */
static int a9_aop_force_stop(struct a9_aop *aop)
{
	int ret;

	aop->experiment_step = "cold-stop";
	ret = aop_wr(aop, AOP_OUTBOX_CONTROL,
		     aop_rd(aop, AOP_OUTBOX_CONTROL) & ~AOP_OUTBOX_ENABLE);
	if (!ret)
		ret = aop_wr(aop, AOP_IRQ_MASK_SET, AOP_IRQ_ALL);
	if (!ret)
		ret = aop_wr(aop, AOP_IRQ_MIRROR_SET, AOP_IRQ_ALL);
	if (!ret)
		ret = aop_wr(aop, AOP_CPU_CONTROL,
			     aop_rd(aop, AOP_CPU_CONTROL) & ~AOP_CPU_START);
	if (!ret)
		ret = a9_aop_control_write(aop, aop->autoboot, 0x600, 0);
	if (ret)
		return ret;
	msleep(AOP_HALT_DELAY_MS);
	aop->force_halt_control = aop_rd(aop, AOP_CPU_CONTROL);
	a9_aop_experiment_regs(aop, "halt-end");
	ret = aop->fault_control || !a9_aop_force_stopped(aop) ? -EIO : 0;
	dev_info(aop->dev, "COLD halt errno=%d predicate=run-clear-delay cpu44=%08x rawb88=%08x delay_ms=%u execution=unknown reset_hook=none\n",
		 ret, aop->force_halt_control, aop_rd(aop, AOP_WRAPPER_B88),
		 AOP_HALT_DELAY_MS);
	if (ret)
		return ret;
	aop->force_halted = true;
	return 0;
}

/* No invented FIFO reset bit: pop at most 64 stale words from each queue.
 * Never dispatch these words into the new protocol epoch or clear faults.
 */
static int a9_aop_force_drain(struct a9_aop *aop)
{
	unsigned int fifo, i;
	u32 control, offset;
	u64 word;

	if (!aop->force_halted)
		return -EPERM;
	aop->experiment_step = "cold-drain";
	for (fifo = 0; fifo < 2; fifo++) {
		offset = fifo ? AOP_OUTBOX_CONTROL : AOP_INBOX_CONTROL;
		for (i = 0; i < AOP_DRAIN_BUDGET; i++) {
			a9_aop_experiment_regs(aop, "stale-before");
			if (aop->fault_control || !a9_aop_force_stopped(aop))
				return -EIO;
			control = aop_rd(aop, offset);
			if (control & AOP_MBOX_FAULTS)
				return -EIO;
			if (control & AOP_MBOX_EMPTY)
				break;
			word = readq(aop->wrapper +
				     (fifo ? AOP_OUTBOX_DATA : AOP_INBOX_POP));
			if (fifo)
				aop->stale_outbox++;
			else
				aop->stale_inbox++;
			dev_info(aop->dev, "COLD stale fifo=%s index=%u word=%016llx before=%08x after=%08x\n",
				 fifo ? "outbox" : "inbox", i, word, control,
				 aop_rd(aop, offset));
			a9_aop_experiment_regs(aop, "stale-after");
		}
		control = aop_rd(aop, offset);
		if (aop->fault_control || !a9_aop_force_stopped(aop) ||
		    (control & AOP_MBOX_FAULTS))
			return -EIO;
		if (!(control & AOP_MBOX_EMPTY))
			return -ETIMEDOUT;
	}
	aop->force_drained = true;
	return 0;
}

/* ---- mailbox ---------------------------------------------------------- */

static int a9_aop_send(struct a9_aop *aop, u8 ep, u64 payload)
{
	unsigned long flags;
	u64 wire;
	u32 control;
	int ret;

	ret = a9_aop_pack(ep, payload, &wire);
	if (ret)
		return ret;
	if (READ_ONCE(aop->phase) == PHASE_REFUSED ||
	    (aop->cold_run && READ_ONCE(aop->rx_halt)))
		return -ESHUTDOWN;
	if (aop->experiment_stage &&
	    (a9_aop_dead(aop) && !aop->inline_rx &&
	     !(aop->cold_run && !aop->rx_halt &&
	       ((ep && a9_aop_system_endpoint(ep)) ||
		(!ep && a9_mgmt_type(payload) == A9_MGMT_PING)))))
		return -ESHUTDOWN;
	if (aop->experiment_stage &&
	    !a9_exp_tx_allowed(aop->experiment_stage, ep, payload) &&
	    !(aop->cold_run && a9_sys_tx_allowed(ep, payload)) &&
	    !((quiesce || handoff) && aop->inline_rx && aop->stopping &&
	      a9_sys_shutdown_tx_allowed(ep, payload)))
		return -EPERM;
	a9_aop_experiment_regs(aop, "tx-before");
	spin_lock_irqsave(&aop->tx_lock, flags);
	ret = readl_poll_timeout_atomic(aop->wrapper + AOP_INBOX_CONTROL, control,
					!(control & AOP_MBOX_FULL) ||
					(control & AOP_MBOX_FAULTS), 10, 20000);
	if (!ret && (control & AOP_MBOX_FAULTS))
		ret = -EIO;
	if (!ret) {
		/* writeq() orders prior ring writes before the doorbell. */
		writeq(wire, aop->wrapper + AOP_INBOX_DATA);
		aop->tx_words++;
	}
	spin_unlock_irqrestore(&aop->tx_lock, flags);
	/* Ordered status readback, never a destructive data-register readback. */
	a9_aop_experiment_regs(aop, "tx-after0");
	if (aop->experiment_stage && aop->phase != PHASE_STREAMING) {
		usleep_range(50, 100);
		a9_aop_experiment_regs(aop, "tx-after50us");
	}
	spin_lock_irqsave(&aop->rx_lock, flags);
	a9_aop_trace(aop, 'T', wire, ret);
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	return ret;
}

/* Reading the outbox data register consumes the word; bounded per call. */
static unsigned int a9_aop_drain(struct a9_aop *aop)
{
	unsigned long flags;
	unsigned int count = 0;
	unsigned int budget = aop->running_attach && !READ_ONCE(aop->hello) ?
			      1 : AOP_DRAIN_BUDGET;
	u32 control;
	u64 wire;

	if (READ_ONCE(aop->rx_halt))
		return 0;
	spin_lock_irqsave(&aop->rx_lock, flags);
	while (count < budget) {
		if (aop->experiment_stage && !aop->cold_run &&
		    READ_ONCE(aop->phase) != PHASE_STREAMING &&
		    aop->startup_count >= AOP_TRACE - 32) {
			aop->trace_exhausted = true;
			WRITE_ONCE(aop->rx_halt, true);
			break;
		}
		control = aop_rd(aop, AOP_OUTBOX_CONTROL);
		if (control & AOP_MBOX_FAULTS) {
			aop->fault_control = control;
			break;
		}
		if (control & AOP_MBOX_EMPTY)
			break;
		if (kfifo_is_full(&aop->rxq)) {
			aop->rxq_full++;
			aop->rx_backlog = true;
			break;
		}
		wire = readq(aop->wrapper + AOP_OUTBOX_DATA);
		kfifo_put(&aop->rxq, wire);
		aop->rx_words++;
		a9_aop_trace(aop, 'R', wire, 0);
		count++;
	}
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	return count;
}

static irqreturn_t a9_aop_irq(int irq, void *data)
{
	struct a9_aop *aop = data;
	unsigned int count;

	aop->irq_count++;
	if (READ_ONCE(aop->rx_halt)) {
		disable_irq_nosync(irq);
		aop->irq_enabled = false;
		return IRQ_HANDLED;
	}
	count = a9_aop_drain(aop);
	if (aop->fault_control) {
		/* A faulted FIFO is never drained: stop the level line. */
		disable_irq_nosync(irq);
		aop->irq_enabled = false;
		queue_work(aop->wq, &aop->rx_work);
		return IRQ_HANDLED;
	}
	if (aop->rx_backlog) {
		/* Words left in the FIFO: resume once rx_work emptied the queue. */
		aop->rx_backlog = false;
		aop->irq_paused = true;
		disable_irq_nosync(irq);
		queue_work(aop->wq, &aop->rx_work);
		return IRQ_HANDLED;
	}
	if (count) {
		aop->irq_empty_run = 0;
		queue_work(aop->wq, &aop->rx_work);
		return IRQ_HANDLED;
	}
	aop->irq_empty++;
	/* Unknown level/ack semantics: never storm, fall back to polling. */
	if (++aop->irq_empty_run > AOP_IRQ_STORM) {
		disable_irq_nosync(irq);
		aop->irq_storm = true;
		aop->irq_enabled = false;
	}
	return IRQ_HANDLED;
}

static void a9_aop_poll_work(struct work_struct *work)
{
	struct a9_aop *aop = container_of(to_delayed_work(work), struct a9_aop,
					  poll_work);

	if (a9_aop_drain(aop) || aop->fault_control || aop->rx_backlog)
		queue_work(aop->wq, &aop->rx_work);
	aop->rx_backlog = false;
	if (!READ_ONCE(aop->stopping) && !READ_ONCE(aop->rx_halt))
		queue_delayed_work(aop->wq, &aop->poll_work,
				   msecs_to_jiffies(aop->irq_storm ? 5 : aop->poll_ms));
}

/* ---- management ------------------------------------------------------- */

static struct a9_aop_ep *a9_aop_find_ep(struct a9_aop *aop, u8 num)
{
	unsigned int i;

	for (i = 0; i < aop->neps; i++)
		if (aop->eps[i].num == num)
			return &aop->eps[i];
	return NULL;
}

static void a9_aop_mgmt(struct a9_aop *aop, u64 payload)
{
	struct a9_epmap map;
	unsigned int i, num;
	u64 reply;
	u8 type = a9_mgmt_type(payload);
	bool early_ack = aop->experiment_stage &&
		(type == A9_MGMT_PING_ACK || type == A9_MGMT_IOP_POWER_ACK ||
		 type == A9_MGMT_AP_POWER_ACK);
	int ret;

	if (aop->experiment_stage && type == A9_MGMT_HELLO &&
	    (aop->epmap_chunks || aop->handshake_complete || aop->device_hellos >= 3)) {
		if (aop->cold_run)
			WRITE_ONCE(aop->rx_halt, true);
		a9_aop_fail(aop, "failed-late-or-repeated-hello", -EPROTO);
		return;
	}
	if ((!aop->hello && !early_ack) ||
	    (aop->experiment_stage && type == A9_MGMT_HELLO)) {
		/* The first word on a fresh boot must be the device HELLO. */
		if (a9_mgmt_type(payload) != A9_MGMT_HELLO) {
			if (aop->cold)
				a9_aop_fail(aop, "failed-first-word-not-hello", -EPROTO);
			else
				a9_aop_refuse(aop, "refused-running-transcript-not-hello");
			return;
		}
		ret = a9_aop_hello_reply(payload, &reply);
		aop->hello_min = payload & 0xffff;
		aop->hello_max = (payload >> 16) & 0xffff;
		if (ret) {
			a9_aop_fail(aop, "failed-hello-version", ret);
			return;
		}
		ret = a9_aop_send(aop, 0, reply);
		if (ret) {
			a9_aop_fail(aop, "failed-hello-reply", ret);
			return;
		}
		aop->hello = true;
		aop->device_hellos++;
		aop->phase = PHASE_WAIT_EPMAP;
		wake_up_all(&aop->wait);
		return;
	}

	switch (a9_mgmt_type(payload)) {
	case A9_MGMT_EPMAP:
		if (aop->epmap_done) {
			dev_warn(aop->dev, "unknown duplicate EPMAP %014llx\n", payload);
			if (aop->experiment_stage)
				a9_aop_fail(aop, "failed-duplicate-epmap", -EPROTO);
			if (aop->cold_run)
				WRITE_ONCE(aop->rx_halt, true);
			aop->unknown_mgmt++;
			return;
		}
		a9_mgmt_epmap(payload, &map);
		if (aop->experiment_stage &&
		    a9_exp_epmap(map.base, &aop->epmap_seen)) {
			a9_aop_fail(aop, "failed-epmap-order-or-range", -EPROTO);
			return;
		}
		aop->epmap_chunks++;
		for (i = 0; i < 32; i++) {
			if (!(map.bitmap & BIT(i)))
				continue;
			num = map.base * 32 + i;
			if (num > 255) {
				a9_aop_fail(aop, "failed-epmap-endpoint-range", -ERANGE);
				return;
			}
			set_bit(num, aop->present);
			if (!a9_aop_system_endpoint(num) && aop->neps < AOP_MAX_EPS) {
				aop->eps[aop->neps].num = num;
				aop->eps[aop->neps].state = AFK_OFF;
				aop->neps++;
			}
		}
		ret = a9_aop_send(aop, 0, a9_mgmt_epmap_reply(&map));
		if (ret) {
			a9_aop_fail(aop, "failed-epmap-reply", ret);
			return;
		}
		if (!map.last)
			return;
		aop->epmap_done = true;
		/* The continuation starts system endpoints before AP readiness. */
		if (aop->cold_run) {
			aop->ping_ack_ns = ktime_get_boottime_ns();
			wake_up_all(&aop->wait);
			return;
		}
		ret = a9_aop_send(aop, 0, a9_mgmt_ap_power(true));
		if (ret) {
			a9_aop_fail(aop, "failed-ap-power", ret);
			return;
		}
		aop->ping_ack_ns = ktime_get_boottime_ns();
		if (!aop->experiment_stage)
			queue_delayed_work(aop->wq, &aop->ping_work, 0);
		wake_up_all(&aop->wait);
		return;
	case A9_MGMT_IOP_POWER_ACK:
		/* Boot completion: the firmware reports power state 0x20. */
		aop->iop_power = (u32)payload;
		aop->iop_ready = (payload & 0xff) == A9_MGMT_IOP_POWER_ON;
		if (aop->cold_run)
			dev_info(aop->dev, "SYSTEM IOP power ack state=0x%x\n", aop->iop_power);
		wake_up_all(&aop->wait);
		return;
	case A9_MGMT_PING_ACK:
		if (aop->ping_sent_ns && a9_mgmt_ping_ack(payload, aop->ping_seq)) {
			aop->ping_ack_ns = ktime_get_boottime_ns();
			aop->ping_acks++;
		}
		return;
	case A9_MGMT_AP_POWER_ACK:
		aop->ap_power_ack = (u32)payload;
		aop->ap_power_acked = true;
		if (aop->cold_run)
			dev_info(aop->dev, "SYSTEM AP power ack state=0x%x\n", aop->ap_power_ack);
		wake_up_all(&aop->wait);
		return;
	default:
		aop->unknown_mgmt++;
		dev_warn(aop->dev, "unknown management message %014llx\n", payload);
		if (aop->experiment_stage && !aop->cold_run)
			a9_aop_fail(aop, "failed-unexpected-management", -EPROTO);
		return;
	}
}

static void a9_aop_ping_tick(struct a9_aop *aop)
{
	u64 now;

	mutex_lock(&aop->lock);
	if ((READ_ONCE(aop->stopping) &&
	     !((quiesce || handoff) && aop->inline_rx &&
	       !aop->shutdown_power_requested)) ||
	    READ_ONCE(aop->rx_halt) ||
	    READ_ONCE(aop->phase) == PHASE_REFUSED ||
	    (aop->experiment_stage && a9_aop_dead(aop) && !aop->cold_run))
		goto out;
	/* Inline boot RX and ping_work share this lock. Sample after acquiring
	 * it: a waiting tick must not subtract a newer send/ACK from an old now.
	 * Both unsigned deltas would wrap, expiring a just-acknowledged ping.
	 */
	now = ktime_get_boottime_ns();
	if (aop->ping_sent_ns && now - aop->ping_sent_ns <
	    (u64)AOP_PING_MS * NSEC_PER_MSEC)
		goto out;
	if (aop->phase != PHASE_FAILED && aop->ping_sent_ns &&
	    now - aop->ping_ack_ns > (u64)AOP_PING_TIMEOUT_MS * NSEC_PER_MSEC)
		a9_aop_fail(aop, "failed-ping-timeout", -ETIMEDOUT);
	/* Keep pinging even after a failure: the firmware has a watchdog. */
	aop->ping_seq++;
	aop->ping_sent_ns = now;
	a9_aop_send(aop, 0, a9_mgmt_ping(aop->ping_seq,
					 (u16)div_u64(now, NSEC_PER_SEC)));
out:
	mutex_unlock(&aop->lock);
}

static void a9_aop_ping_work(struct work_struct *work)
{
	struct a9_aop *aop = container_of(to_delayed_work(work), struct a9_aop,
					  ping_work);

	a9_aop_ping_tick(aop);
	if (READ_ONCE(aop->stopping) || READ_ONCE(aop->rx_halt))
		return;
	queue_delayed_work(aop->wq, &aop->ping_work,
			   msecs_to_jiffies(AOP_PING_MS));
}

/* ---- RTKit system endpoints ------------------------------------------- */

/* Also reaches the continuous ramoops console, without a synthetic panic. */
static void a9_aop_stage3_mark(struct a9_aop *aop, const char *step)
{
	if (!aop->cold_run)
		return;
	aop->stage3_checkpoint = step;
	dev_info(aop->dev, "STAGE3 checkpoint=%s run=%u grants=%llu pool_used=0x%zx ping_acks=%llu\n",
		 step, aop->cold_run, aop->system_grants, aop->pool_used,
		 aop->ping_acks);
	if (!pr_flush(1000, false))
		dev_warn(aop->dev, "STAGE3 checkpoint flush timed out: %s\n", step);
	mb(); /* Complete ramoops WC stores before the next stage. */
}

/* ---- reserved shutdown transcript (aop_quiesce=1, aop_handoff=sleep) ----- */

/* Appends one formatted line to the pool tail page; the console may already
 * be filtered by userspace's kernel.printk, this copy is not.
 */
static __printf(2, 3) void a9_aop_slog(struct a9_aop *aop, const char *fmt, ...)
{
	struct a9_sl_header *h = aop->ho.log;
	char line[512];
	va_list args;
	size_t n;

	if (!h || !aop->ho.log_on)
		return;
	va_start(args, fmt);
	n = vscnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	/* Keep one line per record even if a long line was truncated. */
	if (n && line[n - 1] != '\n') {
		if (n == sizeof(line) - 1)
			n--;
		line[n++] = '\n';
	}
	if (n > h->capacity - h->length) {
		h->truncated++;
		wmb(); /* The truncation count is the only store for this line. */
		return;
	}
	memcpy(aop->ho.log_text + h->length, line, n);
	aop->ho.log_hash = a9_sl_hash(aop->ho.log_hash, line, n);
	wmb(); /* Text before the checksum and length that publish it. */
	h->checksum = aop->ho.log_hash;
	h->length += n;
	h->lines++;
	wmb(); /* Line published before the next shutdown step's traffic. */
}

#define aop_tr_info(aop, fmt, ...) do {					\
	dev_info((aop)->dev, fmt, ##__VA_ARGS__);			\
	a9_aop_slog(aop, fmt, ##__VA_ARGS__);				\
} while (0)

#define aop_tr_err(aop, fmt, ...) do {					\
	dev_err((aop)->dev, fmt, ##__VA_ARGS__);			\
	a9_aop_slog(aop, fmt, ##__VA_ARGS__);				\
} while (0)

static void a9_aop_entry_name(char *entry, size_t size)
{
	const char *at = saved_command_line ?
			 strstr(saved_command_line, "linux_a9.entry=") : NULL;

	memset(entry, 0, size);
	if (!at)
		return;
	at += strlen("linux_a9.entry=");
	memcpy(entry, at, min(strcspn(at, " "), size - 1));
}

/* aop_spi0_reset=1: has firmware run since the SoC reset, even if this
 * kernel never released it? The next kernel still binds a carried 1 to
 * the wrapper's reset state before it touches SPI0.
 */
static bool a9_aop_spi0_carry(struct a9_aop *aop, const char **source)
{
	const struct a9_aop_spi0 *s = &aop->spi0;

	if (aop->hello) {
		*source = "hello";
		return true;
	}
	if (s->decided) {
		*source = a9_spi0_reason_name(s->reason);
		return a9_spi0_reason_runs(s->reason);
	}
	*source = s->prev.state == A9_SPI0_PREV_VALID ? "prev-unbound" :
		  a9_spi0_prev_name(s->prev.state);
	return s->prev.state == A9_SPI0_PREV_VALID &&
	       (s->prev.hello || s->prev.carried);
}

/* Start this kernel's transcript; the previous one was replayed at probe. */
static void a9_aop_slog_begin(struct a9_aop *aop)
{
	struct a9_sl_header *h = aop->ho.log;
	const char *source;
	bool retained;

	if (!h)
		return;
	h->magic = 0;
	wmb(); /* Invalidate before rewriting the header. */
	h->version = A9_SL_VERSION;
	h->capacity = A9_SL_CAPACITY;
	h->length = 0;
	h->lines = 0;
	h->truncated = 0;
	h->checksum = A9_SL_HASH_SEED;
	h->mode = handoff ? 2 : 1;
	h->begin_ns = ktime_get_boottime_ns();
	memcpy(h->writer, aop->ho.writer, sizeof(h->writer));
	a9_aop_entry_name(h->entry, sizeof(h->entry));
	aop->ho.log_hash = A9_SL_HASH_SEED;
	aop->ho.log_on = true;
	wmb(); /* Header fields before the magic that validates them. */
	h->magic = A9_SL_MAGIC;
	wmb(); /* Valid empty transcript before the first appended line. */
	aop_tr_info(aop, "SHUTDOWN begin mode=%s entry=%s writer=%*phN hello=%d phase=%s result=%s rx_halt=%d crashed=%d fault=%08x\n",
		    handoff ? "handoff" : "quiesce", h->entry,
		    (int)sizeof(aop->ho.writer), aop->ho.writer, aop->hello,
		    phase_names[aop->phase], aop->result ?: "none",
		    READ_ONCE(aop->rx_halt), aop->iop_crashed, aop->fault_control);
	if (spi0_reset) {
		retained = a9_aop_spi0_carry(aop, &source);
		aop_tr_info(aop, "SPI0-CARRY retained=%d source=%s\n",
			    retained, source);
	}
}

/* Probe: replay the previous kernel's transcript once, then invalidate it. */
static void a9_aop_slog_replay(struct a9_aop *aop)
{
	struct a9_sl_header *h = aop->ho.log;
	struct a9_sl_header copy;
	const char *line, *end;

	memcpy(&copy, h, sizeof(copy));
	if (copy.magic != A9_SL_MAGIC) {
		dev_info(aop->dev, "AOP-PREV shutdown transcript missing: previous kernel wrote none\n");
		return;
	}
	aop->spi0.prev.state = A9_SPI0_PREV_INVALID;
	aop->ho.prev_log = devm_kzalloc(aop->dev, A9_SL_CAPACITY + 1, GFP_KERNEL);
	if (!aop->ho.prev_log)
		return;
	memcpy(aop->ho.prev_log, aop->ho.log_text,
	       min_t(size_t, copy.length, A9_SL_CAPACITY));
	if (!a9_sl_valid(&copy, aop->ho.prev_log)) {
		dev_warn(aop->dev, "AOP-PREV shutdown transcript invalid: version=%u length=%u lines=%u\n",
			 copy.version, copy.length, copy.lines);
		aop->ho.prev_log[0] = 0;
	} else {
		aop->ho.prev_len = copy.length;
		aop->spi0.prev.state = A9_SPI0_PREV_VALID;
		a9_spi0_prev_parse(&aop->spi0.prev, aop->ho.prev_log, copy.length);
		dev_info(aop->dev, "AOP-PREV shutdown transcript mode=%u entry=%s writer=%*phN lines=%u bytes=%u truncated=%u begin_ns=%llu\n",
			 copy.mode, copy.entry, (int)sizeof(copy.writer), copy.writer,
			 copy.lines, copy.length, copy.truncated, copy.begin_ns);
		for (line = aop->ho.prev_log, end = line + copy.length; line < end;) {
			const char *next = memchr(line, '\n', end - line) ?: end;

			dev_info(aop->dev, "AOP-PREV %.*s\n", (int)(next - line), line);
			line = next + 1;
		}
	}
	h->magic = 0;
	wmb(); /* Each transcript is replayed by exactly one later probe. */
}

/* RTKit crashlog: 'CLHE' header (size at +8), then sections with the
 * fourcc at +0 and their total size at +12. Freeze the whole buffer, list
 * every section and print all used bytes, not a 256-byte prefix.
 */
#define A9_CRASH_CLHE	0x434c4845U

static void a9_aop_crashlog(struct a9_aop *aop, const struct a9_aop_buffer *buf,
			    u64 payload)
{
	const u8 *p = buf->va;
	size_t used = buf->size, offset = 32;
	unsigned int n;

	dev_err(aop->dev, "AOP crashlog word=%014llx pa=%pad size=0x%zx\n",
		payload, &buf->dma, buf->size);
	if (!p)
		return;
	dma_rmb();
	aop->crash_copy = devm_kmemdup(aop->dev, p, buf->size, GFP_KERNEL);
	aop->crash_size = aop->crash_copy ? buf->size : 0;
	if (buf->size >= 32 && get_unaligned_le32(p) == A9_CRASH_CLHE &&
	    get_unaligned_le32(p + 8) >= 32 && get_unaligned_le32(p + 8) <= buf->size)
		used = get_unaligned_le32(p + 8);
	dev_err(aop->dev, "AOP crashlog header=%08x version=%08x used=0x%zx flags=%08x snapshot=%zu\n",
		buf->size >= 16 ? get_unaligned_le32(p) : 0,
		buf->size >= 16 ? get_unaligned_le32(p + 4) : 0, used,
		buf->size >= 16 ? get_unaligned_le32(p + 12) : 0, aop->crash_size);
	for (n = 0; n < 64 && offset + 16 <= used; n++) {
		u32 fourcc = get_unaligned_le32(p + offset);
		u32 size = get_unaligned_le32(p + offset + 12);
		char tag[5];
		unsigned int i;

		for (i = 0; i < 4; i++) {
			tag[i] = fourcc >> (24 - 8 * i);
			if (tag[i] < 0x20 || tag[i] > 0x7e)
				tag[i] = '.';
		}
		tag[4] = 0;
		dev_err(aop->dev, "AOP crashlog section=%u off=0x%zx tag=%s size=0x%x\n",
			n, offset, tag, size);
		if (size < 16 || size > used - offset)
			break;
		offset += size;
	}
	print_hex_dump(KERN_ERR, "AOP crash: ", DUMP_PREFIX_OFFSET, 16, 1, p,
		       used, false);
}

static bool a9_aop_system_buffers_ready(struct a9_aop *aop)
{
	unsigned int ep;

	for (ep = 1; ep < A9_SYS_COUNT; ep++)
		if (ep != A9_SYS_DEBUG && aop->system[ep].started &&
		    (!aop->system[ep].buffer.va || aop->system[ep].errors))
			return false;
	return true;
}

static void a9_aop_system_buffer(struct a9_aop *aop, u8 ep, u64 payload)
{
	struct a9_aop_system *sys = &aop->system[ep];
	struct a9_aop_buffer *buf = &sys->buffer;
	size_t size, offset, used = aop->pool_used;
	enum a9_ho_sys_action action = A9_HO_SYS_FRESH;
	const struct a9_ho_sys *grant = NULL;
	u64 pa, reply;
	int ret;

	ret = a9_sys_getbuf(payload, &size, &pa);
	if (!ret && aop->ho.adopt) {
		grant = a9_ho_sys_grant(&aop->ho.rec, ep);
		action = a9_ho_sys_request(grant, aop->pool_pa, pa, size);
	}
	if (ret || buf->va || action == A9_HO_SYS_MISMATCH ||
	    (pa && action != A9_HO_SYS_ADOPT)) {
		/* Only the certified handoff grant may carry a firmware pointer. */
		dev_warn(aop->dev, "system buffer request unhandled ep=%u word=%014llx existing=%d handoff=%d error=%d\n",
			 ep, payload, !!buf->va, action, ret);
		sys->errors++;
		a9_aop_fail(aop, action == A9_HO_SYS_MISMATCH ?
			    "failed-handoff-system-buffer" :
			    "failed-system-buffer-request", ret ?: -EPROTO);
		return;
	}
	if (action == A9_HO_SYS_ADOPT) {
		/* b767f8 maps the existing PA and sends no reply. */
		buf->va = (u8 *)aop->pool + grant->offset;
		buf->dma = aop->pool_pa + grant->offset;
		buf->size = size;
		aop->ho.sys_adopted++;
		dev_info(aop->dev, "HANDOFF system adopt ep=%u pa=%pad size=0x%zx reply=none\n",
			 ep, &buf->dma, size);
		a9_aop_stage3_mark(aop, "handoff-system-adopt");
		wake_up_all(&aop->wait);
		return;
	}
	/* b767f8 also reuses its existing buffer for a PA-0 request that fits. */
	if (action == A9_HO_SYS_REGRANT)
		offset = grant->offset;
	else
		ret = a9_sys_alloc(size, aop->pool_size, &used, &offset);
	if (!ret)
		ret = a9_sys_getbuf_reply(aop->pool_pa + offset, size, &reply);
	if (ret) {
		sys->errors++;
		dev_warn(aop->dev, "system buffer refused ep=%u size=%zx used=%zx error=%d\n",
			 ep, size, aop->pool_used, ret);
		a9_aop_fail(aop, "failed-system-buffer-allocation", ret);
		return;
	}
	buf->va = (u8 *)aop->pool + offset;
	buf->dma = aop->pool_pa + offset;
	buf->size = size;
	aop->pool_used = used;
	memset(buf->va, 0, size);
	dma_wmb();
	/* Pin the grant even if the mailbox TX fails. */
	ret = a9_aop_send(aop, ep, reply);
	if (ret) {
		sys->errors++;
		a9_aop_fail(aop, "failed-system-buffer-tx", ret);
	} else {
		aop->system_grants++;
		if (action == A9_HO_SYS_REGRANT)
			aop->ho.sys_regranted++;
	}
	dev_info(aop->dev, "SYSTEM grant ep=%u pa=%pad size=0x%zx pool_used=0x%zx tx=%d handoff=%s\n",
		 ep, &buf->dma, size, aop->pool_used, ret,
		 action == A9_HO_SYS_REGRANT ? "regrant" : "none");
	a9_aop_stage3_mark(aop, "system-buffer-grant");
	wake_up_all(&aop->wait);
}

static void a9_aop_system_rx(struct a9_aop *aop, u8 ep, u64 payload)
{
	struct a9_aop_system *sys;
	struct a9_aop_buffer *buf;
	u8 type = a9_mgmt_type(payload);
	u32 index = (u32)payload;
	size_t offset, stride, shown;
	char context[25], message[256];
	int ret;

	aop->system_words++;
	if (!a9_sys_supported(ep)) {
		dev_warn(aop->dev, "unknown system endpoint ep=%u word=%014llx\n", ep, payload);
		return;
	}
	sys = &aop->system[ep];
	buf = &sys->buffer;
	sys->words++;
	/* b6d640 tests the existing crash mapping BEFORE decoding the opcode. */
	if (ep == A9_SYS_CRASHLOG && (buf->va || type >= 4)) {
		aop->iop_crashed = true;
		WRITE_ONCE(aop->rx_halt, true);
		a9_aop_crashlog(aop, buf, payload);
		a9_aop_fail(aop, "failed-rtkit-crashlog", -EIO);
		return;
	}
	if (type <= 3 && ep != A9_SYS_DEBUG) {
		a9_aop_system_buffer(aop, ep, payload);
		return;
	}
	if (ep == A9_SYS_SYSLOG && type == 8) {
		sys->entries = payload & 0xffffff;
		sys->message_size = (payload >> 24) & 0xffffff;
		dev_info(aop->dev, "SYSLOG init entries=%u message_size=%u\n",
			 sys->entries, sys->message_size);
		return;
	}
	if (ep == A9_SYS_SYSLOG && (type >> 2) == 1) {
		stride = 32 + sys->message_size;
		offset = index * stride;
		if (buf->va && sys->message_size && index < sys->entries &&
		    offset <= buf->size && stride <= buf->size - offset) {
			dma_rmb();
			memcpy(context, (u8 *)buf->va + offset + 8, 24);
			context[24] = 0;
			shown = min_t(size_t, sys->message_size, sizeof(message) - 1);
			memcpy(message, (u8 *)buf->va + offset + 32, shown);
			message[shown] = 0;
			dev_info(aop->dev, "AOP syslog %s: %s\n", context, message);
		} else {
			sys->errors++;
			dev_warn(aop->dev, "SYSLOG invalid slot word=%014llx; ACK anyway\n",
				 payload);
		}
		/* RTKit blocks if a log slot is not acknowledged. */
		payload = a9_mgmt_word(5, index);
	} else if (ep == A9_SYS_IOREPORT && (type == 4 || type == 12)) {
		/* Completion notifications, not requests: never echo an ACK. */
		return;
	} else if (ep == A9_SYS_IOREPORT && type == 8) {
		payload = a9_mgmt_word(8, 0);
	} else {
		sys->unknown++;
		dev_warn(aop->dev, "unknown system message ep=%u word=%014llx\n", ep, payload);
		return;
	}
	ret = a9_aop_send(aop, ep, payload);
	if (ret)
		sys->errors++;
	else
		sys->acks++;
}

/* ---- AFK endpoint ------------------------------------------------------ */

static void a9_aop_ep_fail(struct a9_aop *aop, struct a9_aop_ep *e,
			   const char *why, int error)
{
	e->state = AFK_FAILED;
	dev_warn(aop->dev, "endpoint %u: %s (%d)\n", e->num, why, error);
	if (aop->accel >= 0 && aop->services[aop->accel].ep == e->num)
		a9_aop_fail(aop, why, error);
	else if (aop->experiment_stage)
		a9_aop_fail(aop, why, error);
}

static void a9_aop_getbuf(struct a9_aop *aop, struct a9_aop_ep *e, u64 payload)
{
	struct a9_aop_buffer *buf;
	u64 reply;
	size_t size;
	size_t offset;
	u16 tag;
	unsigned int i;
	int ret;

	ret = a9_afk_getbuf(payload, &tag, &size);
	if (ret || (aop->experiment_stage && !tag) ||
	    size > AOP_GETBUF_MAX || e->nbufs >= A9_AFK_MAX_BUFFERS) {
		a9_aop_ep_fail(aop, e, "failed-getbuf-request", ret ?: -E2BIG);
		return;
	}
	buf = &e->bufs[e->nbufs];
	for (i = 0; i < e->nbufs; i++) {
		if (e->bufs[i].tag == tag) {
			a9_aop_ep_fail(aop, e, "failed-getbuf-duplicate-tag", -EPROTO);
			return;
		}
	}
	if (aop->experiment_stage) {
		u32 recorded;

		/* The firmware re-INITs after its own a4/87 stop: fresh, zeroed
		 * contents at the recorded address, never replayed ring state.
		 */
		if (aop->ho.adopt &&
		    !a9_ho_take_afk(&aop->ho.rec, aop->ho.afk_used, e->num, tag,
				    size, &recorded)) {
			offset = recorded;
			aop->ho.afk_reused++;
		} else {
			ret = a9_exp_alloc(size, aop->pool_size, &aop->pool_used,
					   &offset);
			if (ret) {
				a9_aop_ep_fail(aop, e, "failed-reserved-pool-exhausted", ret);
				return;
			}
			if (aop->ho.adopt)
				aop->ho.afk_fresh++;
		}
		buf->va = (u8 *)aop->pool + offset;
		buf->dma = aop->pool_pa + offset;
		memset(buf->va, 0, size);
		dma_wmb();
	} else {
		buf->va = dma_alloc_coherent(aop->dev, size, &buf->dma, GFP_KERNEL);
	}
	if (!buf->va) {
		a9_aop_ep_fail(aop, e, "failed-getbuf-alloc", -ENOMEM);
		return;
	}
	buf->tag = tag;
	buf->size = size;
	/* No IOMMU on this device: the DMA address is the AP physical one. */
	ret = a9_afk_getbuf_ack(buf->dma, &reply);
	if (ret || buf->dma + size - 1 > A9_AOP_PA_MASK) {
		if (!aop->experiment_stage)
			dma_free_coherent(aop->dev, size, buf->va, buf->dma);
		buf->va = NULL;
		a9_aop_ep_fail(aop, e, "failed-getbuf-address", -ERANGE);
		return;
	}
	e->nbufs++;
	dev_info(aop->dev, "AFK grant ep=0x%02x tag=0x%04x pa=%pad size=0x%zx reserved=%d\n",
		 e->num, tag, &buf->dma, size, !!aop->experiment_stage);
	ret = a9_aop_send(aop, e->num, reply);
	if (ret)
		a9_aop_ep_fail(aop, e, "failed-getbuf-reply", ret);
}

static void a9_aop_ring_desc(struct a9_aop *aop, struct a9_aop_ep *e,
			     unsigned int which, u64 payload)
{
	struct a9_aop_buffer *buf = NULL;
	size_t offset, size;
	unsigned int i;
	u16 tag = payload & 0xffff;
	int ret;

	for (i = 0; i < e->nbufs; i++)
		if (e->bufs[i].tag == tag)
			buf = &e->bufs[i];
	if (!buf || e->ring_valid[which]) {
		a9_aop_ep_fail(aop, e, "failed-ring-descriptor-tag", -EPROTO);
		return;
	}
	ret = a9_spu_queue_descriptor(payload, buf->size, &tag, &offset, &size);
	if (!ret && (offset % A9_RING_BLOCK || size % A9_RING_BLOCK ||
		     size < A9_RING_HEADER + A9_RING_BLOCK))
		ret = -EINVAL;
	if (ret) {
		a9_aop_ep_fail(aop, e, "failed-ring-descriptor", ret);
		return;
	}
	for (i = 0; i < RING_COUNT; i++) {
		if (aop->experiment_stage && e->ring_seen[i] &&
		    e->ring_tag[i] == tag && offset < e->ring_offset[i] + e->ring_size[i] &&
		    e->ring_offset[i] < offset + size) {
			a9_aop_ep_fail(aop, e, "failed-ring-overlap", -EPROTO);
			return;
		}
	}
	e->ring_tag[which] = tag;
	e->ring_offset[which] = offset;
	e->ring_size[which] = size;
	e->ring_seen[which] = (u8 *)buf->va + offset;
	/* The AOP wrote the header before announcing the ring. */
	dma_rmb();
	ret = a9_ring_attach(&e->rings[which], e->ring_seen[which], size);
	if (ret) {
		a9_aop_ep_fail(aop, e, "failed-ring-header", ret);
		return;
	}
	e->ring_valid[which] = true;
	if (e->state == AFK_INIT && e->ring_valid[RING_TX] &&
	    e->ring_valid[RING_RX]) {
		a9_aop_stage3_mark(aop, "afk-rings-ready");
		ret = a9_aop_send(aop, e->num, a9_afk_word(A9_AFK_START));
		if (ret)
			a9_aop_ep_fail(aop, e, "failed-afk-start", ret);
		else
			e->state = AFK_STARTING;
	}
}

static void a9_aop_record_service(struct a9_aop *aop, struct a9_aop_ep *e,
				  u32 channel,
				  const struct a9_spu_subpacket *sub)
{
	struct a9_aop_service *svc;
	unsigned int i;

	if (aop->nservices >= AOP_MAX_SERVICES)
		return;
	svc = &aop->services[aop->nservices];
	if (a9_spu_service(sub, &svc->info)) {
		aop->malformed++;
		return;
	}
	/* The channel offset is [I]: deduplicate by endpoint and name. */
	for (i = 0; i < aop->nservices; i++)
		if (aop->services[i].ep == e->num &&
		    !strcmp(aop->services[i].info.name, svc->info.name))
			return;
	svc->ep = e->num;
	svc->channel = channel;
	if (strcmp(svc->info.name, "accel")) {
		dev_info(aop->dev, "service ep=%u channel=%u name=%s id=%08x\n",
			 e->num, channel, svc->info.name, svc->info.service_id);
		aop->nservices++;
		return;
	}
	aop->nservices++;
	if (aop->accel >= 0)
		return;
	/* Name is the key; the ADT service id is the consistency check. */
	if (svc->info.service_id != AOP_ACCEL_SERVICE_ID) {
		a9_aop_fail(aop, "failed-accel-service-id", -EPROTO);
		return;
	}
	aop->accel = aop->nservices - 1;
	dev_info(aop->dev, "ACCEL service ep=0x%02x channel=%u id=%08x\n",
		 e->num, channel, svc->info.service_id);
	wake_up_all(&aop->wait);
}

static void a9_aop_report(struct a9_aop *aop, struct a9_aop_ep *e, u32 channel,
			  const struct a9_spu_subpacket *sub)
{
	struct a9_aop_service *svc;
	int ret;

	if (aop->accel < 0)
		return;
	svc = &aop->services[aop->accel];
	if (svc->ep != e->num)
		return;
	/* 0xc2 is accel-only; count but tolerate an unexpected channel. */
	if (channel != svc->channel)
		aop->channel_mismatch++;
	ret = a9_spu_motion(sub, &aop->motion);
	if (ret) {
		aop->report_errors++;
		if (aop->iio)
			a9_aop_accel_invalidate(aop->iio);
		return;
	}
	if (aop->experiment_stage) {
		unsigned int at = aop->reports % A9_EXP_SAMPLES;

		aop->samples[at].ns = ktime_get_boottime_ns();
		aop->samples[at].motion = aop->motion;
		aop->samples[at].size = min_t(u32, sub->size, 64);
		memcpy(aop->samples[at].raw, sub->payload, aop->samples[at].size);
	}
	aop->reports++;
	if (!aop->have_first_report && aop->rate_set) {
		aop->first_report_size = min_t(u32, sub->size,
					       sizeof(aop->first_report));
		memcpy(aop->first_report, sub->payload, aop->first_report_size);
		aop->have_first_report = true;
		wake_up_all(&aop->wait);
	}
	if (aop->iio && aop->phase == PHASE_STREAMING)
		a9_aop_accel_report(aop->iio, sub);
}

static void a9_aop_epic(struct a9_aop *aop, struct a9_aop_ep *e, u32 channel,
			const u8 *packet, size_t length)
{
	struct a9_spu_subpacket subs[8];
	size_t cursor = A9_SPU_OUTER_SIZE;
	unsigned int n = 0, i;
	int ret;

	/* Validate the whole walk before acting on any subpacket. */
	while (n < ARRAY_SIZE(subs)) {
		ret = a9_spu_next(packet, length, &cursor, &subs[n]);
		if (ret <= 0)
			break;
		n++;
	}
	if (!n) {
		aop->malformed++;
		e->rx_errors++;
		dev_warn(aop->dev, "malformed SPU ep=%u channel=%u bytes=%zu error=%d\n",
			 e->num, channel, length, ret);
		return;
	}
	if (ret < 0)
		aop->trailing++;	/* bytes after the last valid subpacket */
	e->rx_packets++;
	for (i = 0; i < n; i++) {
		struct a9_spu_subpacket *sub = &subs[i];
		bool handled = false;

		if (sub->category < A9_SPU_NOTIFY && sub->subtype == 0xc0) {
			a9_aop_record_service(aop, e, channel, sub);
			handled = true;
		} else if (sub->category < A9_SPU_NOTIFY &&
			   sub->subtype == A9_SPU_ACCEL_REPORT) {
			a9_aop_report(aop, e, channel, sub);
			handled = true;
		}
		if (aop->cmd_tag && sub->tag == aop->cmd_tag && aop->accel >= 0 &&
		    aop->services[aop->accel].ep == e->num &&
		    sub->subtype != A9_SPU_ACCEL_REPORT && sub->subtype != 0xc0) {
			/* Whatever answers our tag, for failed-configure diagnosis. */
			aop->reply_category = sub->category;
			aop->reply_subtype = sub->subtype;
			aop->reply_tag = sub->tag;
			aop->reply_size = sub->size;
			aop->reply_word = sub->size >= 4 ?
				get_unaligned_le32(sub->payload) : 0;
			aop->replies_seen++;
		}
		if (sub->category == A9_SPU_REPLY &&
		    sub->subtype == A9_SPU_PROPERTY_REPLY && aop->cmd_pending &&
		    aop->accel >= 0 && aop->services[aop->accel].ep == e->num &&
		    aop->services[aop->accel].channel == channel &&
		    sub->tag == aop->cmd_tag) {
			aop->cmd_status = a9_spu_property_reply(sub, aop->cmd_tag);
			aop->cmd_pending = false;
			dev_info(aop->dev, "ACCEL property reply ep=0x%x channel=0x%x tag=0x%04x subtype=0x%x bytes=%u status=%d ioreturn=0x%08x\n",
				 e->num, channel, sub->tag, sub->subtype,
				 sub->size, aop->cmd_status, aop->reply_word);
			handled = true;
			wake_up_all(&aop->wait);
		}
		if (!handled) {
			dev_info(aop->dev, "unhandled SPU ep=%u channel=%u category=%02x subtype=%04x tag=%04x bytes=%u\n",
				 e->num, channel, sub->category, sub->subtype, sub->tag, sub->size);
		}
	}
}

static void a9_aop_rx_ring(struct a9_aop *aop, struct a9_aop_ep *e)
{
	struct a9_ring *ring = &e->rings[RING_RX];
	struct a9_ring_entry entry;
	unsigned int budget;
	u32 wptr, next;
	int ret;

	if (!e->ring_valid[RING_RX])
		return;
	for (budget = 0; budget < 64; budget++) {
		wptr = a9_ring_wptr(ring);
		/* Entry contents only after the wptr that published them. */
		dma_rmb();
		ret = a9_ring_read(ring, wptr, aop->rx_packet,
				   sizeof(aop->rx_packet), &entry, &next);
		if (ret == -EAGAIN)
			break;
		if (ret) {
			/* Drop to wptr; never reread a malformed entry. */
			e->rx_errors++;
			e->last_rx_error = ret;
			if (!a9_ring_index_ok(ring, wptr))
				break;
			next = wptr;
		} else {
			/* iOS does not check the magic; only count surprises. */
			if (entry.magic != A9_RING_MAGIC_AOP &&
			    entry.magic != A9_RING_MAGIC_IOP)
				e->magic_unexpected++;
			e->last_word = entry.word;
			if (entry.length > sizeof(aop->rx_packet))
				aop->truncated++;
			a9_aop_epic(aop, e, entry.channel, aop->rx_packet,
				    min_t(size_t, entry.length,
					  sizeof(aop->rx_packet)));
		}
		ring->cursor = next;
		/* Finish reading the entry before releasing it to the AOP. */
		dma_mb();
		a9_ring_set_rptr(ring, next);
		if (ret)
			break;
	}
}

static void a9_aop_afk(struct a9_aop *aop, struct a9_aop_ep *e, u64 payload)
{
	u8 type = a9_afk_type(payload);
	int ret;

	/* Shutdown must not negotiate a late INIT or grant fresh rings. Routing
	 * for an enabled, still-uninitialized source is disabled below instead.
	 */
	if ((quiesce || handoff) && aop->stopping &&
	    (e->state == AFK_WAIT_INIT ||
	     (e->state == AFK_STOPPING && type != A9_AFK_SHUTDOWN_ACK &&
	      type != A9_AFK_HIBERNATE_ACK && type != A9_AFK_RX_NOTIFY))) {
		dev_info(aop->dev, "QUIESCE step=afk-late ep=0x%x type=0x%x state=%s ignored\n",
			 e->num, type, afk_names[e->state]);
		return;
	}
	if (e->state == AFK_FAILED || e->state == AFK_OFF) {
		e->unknown++;
		dev_warn(aop->dev, "unknown inactive AFK ep=%u word=%014llx\n", e->num, payload);
		return;
	}
	if (e->state == AFK_WAIT_INIT) {
		/* iOS drops everything until the device's 0x80 arrives. */
		if (type != A9_AFK_INIT) {
			e->dropped_before_init++;
			dev_warn(aop->dev, "unknown pre-INIT AFK ep=%u word=%014llx\n",
				 e->num, payload);
			return;
		}
		if ((payload & A9_AOP_PA_MASK) != A9_AFK_VERSION) {
			a9_aop_ep_fail(aop, e, "failed-afk-version", -EPROTONOSUPPORT);
			return;
		}
		ret = a9_aop_send(aop, e->num, a9_afk_word(A9_AFK_INIT_ACK));
		if (ret)
			a9_aop_ep_fail(aop, e, "failed-afk-init-ack", ret);
		else
			e->state = AFK_INIT;
		a9_aop_stage3_mark(aop, "afk-init-v8");
		return;
	}
	switch (type) {
	case A9_AFK_GETBUF:
		a9_aop_getbuf(aop, e, payload);
		return;
	case A9_AFK_TX_DESC:
		a9_aop_ring_desc(aop, e, RING_TX, payload);
		return;
	case A9_AFK_RX_DESC:
		a9_aop_ring_desc(aop, e, RING_RX, payload);
		return;
	case A9_AFK_HIST_DESC:
		/* Created when offered, never enabled for rotation. */
		a9_aop_ring_desc(aop, e, RING_HIST, payload);
		return;
	case A9_AFK_START_ACK:
		e->state = AFK_RUNNING;
		a9_aop_stage3_mark(aop, "afk-start-ack");
		wake_up_all(&aop->wait);
		a9_aop_rx_ring(aop, e);
		return;
	case A9_AFK_RX_NOTIFY:
		a9_aop_rx_ring(aop, e);
		return;
	case A9_AFK_SHUTDOWN_ACK:
	case A9_AFK_HIBERNATE_ACK:
		e->state = AFK_STOPPED;
		wake_up_all(&aop->wait);
		return;
	case A9_AFK_INIT:
		if (aop->cold_run)
			WRITE_ONCE(aop->rx_halt, true);
		a9_aop_ep_fail(aop, e, "failed-afk-reinit", -EPROTO);
		return;
	default:
		e->unknown++;
		dev_warn(aop->dev, "unknown AFK ep=%u word=%014llx\n", e->num, payload);
		return;
	}
}

/* Lock held. The firmware resumed its certified RTKit session in place:
 * restore endpoints and system grants from the record; apps stay off.
 */
static void a9_aop_handoff_adopt_session(struct a9_aop *aop)
{
	const struct a9_ho_record *r = &aop->ho.rec;
	unsigned int num, ep;

	for (num = 0; num < 256; num++) {
		if (!a9_ho_present(r, num))
			continue;
		set_bit(num, aop->present);
		if (!a9_aop_system_endpoint(num) && aop->neps < AOP_MAX_EPS) {
			aop->eps[aop->neps].num = num;
			aop->eps[aop->neps].state = AFK_OFF;
			aop->neps++;
		}
	}
	for (ep = 1; ep < A9_SYS_COUNT; ep++) {
		const struct a9_ho_sys *grant = a9_ho_sys_grant(r, ep);
		struct a9_aop_system *sys = &aop->system[ep];

		if (!grant)
			continue;
		sys->started = true;
		sys->buffer.va = (u8 *)aop->pool + grant->offset;
		sys->buffer.dma = aop->pool_pa + grant->offset;
		sys->buffer.size = grant->size;
		sys->entries = grant->entries;
		sys->message_size = grant->message_size;
		aop->ho.sys_adopted++;
	}
	aop->hello_min = r->hello_min;
	aop->hello_max = r->hello_max;
	/* As the cold EPMAP path does: the watchdog clock starts at adoption,
	 * not at boot, or the first late ping ACK would read as a 5 s timeout.
	 */
	aop->ping_ack_ns = ktime_get_boottime_ns();
	aop->hello = true;
	aop->epmap_done = true;
	aop->handshake_complete = true;
	aop->phase = PHASE_WAIT_EPMAP;
}

/* Lock held. iOS RTBuddy_waitForBootState accepts a fresh HELLO handshake
 * or a bare IOP ACK. The pinned firmware ACKs after its power-up callbacks,
 * so a callback-sent HELLO would precede that ACK. The first word decides.
 */
static void a9_aop_handoff_classify(struct a9_aop *aop, u8 ep, u64 payload)
{
	aop->ho.waking = false;
	if (!ep && a9_mgmt_type(payload) == A9_MGMT_HELLO) {
		aop->ho.path = A9_HO_PATH_HELLO;
	} else {
		aop->ho.path = A9_HO_PATH_RESUME;
		a9_aop_handoff_adopt_session(aop);
	}
	dev_info(aop->dev, "HANDOFF wake first ep=%u word=%014llx path=%s\n",
		 ep, payload, a9_ho_path_names[aop->ho.path]);
	wake_up_all(&aop->wait);
}

static void a9_aop_dispatch(struct a9_aop *aop, u64 wire)
{
	u8 ep = a9_aop_endpoint(wire);
	u64 payload = a9_aop_payload(wire);
	struct a9_aop_ep *e;

	lockdep_assert_held(&aop->lock);
	if (aop->phase == PHASE_REFUSED || (aop->cold_run && aop->rx_halt))
		return;
	if (aop->experiment_stage && aop->phase == PHASE_FAILED && !aop->cold_run)
		return;
	if (aop->ho.waking)
		a9_aop_handoff_classify(aop, ep, payload);
	if (aop->experiment_stage && ep &&
	    (aop->experiment_stage == 2 || (!aop->handshake_complete && !aop->cold_run))) {
		a9_aop_fail(aop, "failed-application-before-stage3", -EPROTO);
		return;
	}
	if (aop->running_attach && !aop->hello) {
		a9_attach_rx(&aop->takeover, wire);
		if (aop->takeover.stage == A9_ATTACH_REFUSED) {
			if (aop->takeover.result == A9_ATTACH_HANDSHAKE_OWNED)
				dev_warn(aop->dev, "ZOMBIE AOP: two ping acks, no acceptable EPMAP; inherited rings may reference reused DRAM. Only a real power cycle clears it; do not kexec\n");
			a9_aop_refuse(aop, a9_attach_result_name(aop->takeover.result));
			return;
		}
		if (aop->takeover.stage != A9_ATTACH_ADOPTED) {
			aop->ping_seq = aop->takeover.seq;
			aop->ping_acks++;
			aop->ping_ack_ns = ktime_get_boottime_ns();
			return;
		}
		/* EPMAP, not our speculative HELLO_REPLY, proves adoption [I]. */
		aop->hello = true;
		aop->phase = PHASE_WAIT_EPMAP;
	}
	if (ep == 0) {
		a9_aop_mgmt(aop, payload);
		return;
	}
	if (!aop->hello) {
		if (aop->cold)
			a9_aop_fail(aop, "failed-first-word-not-hello", -EPROTO);
		else
			a9_aop_refuse(aop, "refused-running-transcript-not-hello");
		return;
	}
	if (a9_aop_system_endpoint(ep)) {
		if (aop->cold_run)
			a9_aop_system_rx(aop, ep, payload);
		else
			aop->system_words++;
		return;
	}
	if (aop->cold_run && aop->phase == PHASE_FAILED)
		return; /* Keep servicing system endpoints after an accel failure. */
	e = a9_aop_find_ep(aop, ep);
	if (!e) {
		aop->unknown_ep++;
		dev_warn(aop->dev, "unknown endpoint ep=%u word=%014llx\n", ep, payload);
		return;
	}
	a9_aop_afk(aop, e, payload);
}

static void a9_aop_process(struct a9_aop *aop)
{
	u64 wire;

	lockdep_assert_held(&aop->lock);
	if (aop->trace_exhausted)
		a9_aop_fail(aop, "failed-startup-transcript-budget", -ENOSPC);
	if (aop->fault_control && aop->phase != PHASE_FAILED &&
	    aop->phase != PHASE_REFUSED) {
		/* Faults are reported, never acknowledged or cleared. */
		if (aop->running_attach && !aop->hello)
			a9_aop_refuse(aop, "refused-mailbox-fault");
		else
			a9_aop_fail(aop, "failed-mailbox-fault", -EIO);
	}
	if (aop->fault_control)
		WRITE_ONCE(aop->rx_halt, true);
	while (kfifo_out_spinlocked(&aop->rxq, &wire, 1, &aop->rx_lock))
		a9_aop_dispatch(aop, wire);
}

static void a9_aop_rx_work(struct work_struct *work)
{
	struct a9_aop *aop = container_of(work, struct a9_aop, rx_work);

	mutex_lock(&aop->lock);
	a9_aop_process(aop);
	mutex_unlock(&aop->lock);
	wake_up_all(&aop->wait);
	if (READ_ONCE(aop->irq_paused)) {
		WRITE_ONCE(aop->irq_paused, false);
		enable_irq(aop->irq);
	}
}

/* Shutdown path: no workqueue, drain and dispatch in the caller. */
static void a9_aop_inline_rx(struct a9_aop *aop)
{
	a9_aop_drain(aop);
	mutex_lock(&aop->lock);
	a9_aop_process(aop);
	mutex_unlock(&aop->lock);
}

/* Waits without holding aop->lock; @cond is re-evaluated after wakeups. */
#define a9_aop_wait(aop, cond, ms) ({					\
	unsigned long __end = jiffies + msecs_to_jiffies(ms);		\
	/* Cold boot also drains replies inline beside the RX workqueue. */ \
	if ((aop)->inline_rx || (aop)->cold_run) {			\
		while (!(cond) &&					\
		       !((aop)->experiment_stage && (aop)->rx_halt) &&	\
		       !((aop)->cold_run && !(aop)->stopping &&		\
			 a9_aop_dead(aop)) &&				\
		       time_before(jiffies, __end)) {			\
			a9_aop_inline_rx(aop);				\
			if ((aop)->cold_run && (aop)->handshake_complete)	\
				a9_aop_ping_tick(aop);			\
			usleep_range(500, 1000);			\
		}							\
	} else {							\
		wait_event_timeout((aop)->wait, (cond) ||		\
				   a9_aop_dead(aop),			\
				   msecs_to_jiffies(ms));		\
	}								\
	(cond);								\
})

/* ---- EPIC commands ------------------------------------------------------ */

/* Called with aop->lock held; queues one notify on the accel channel. */
static int a9_aop_queue_property(struct a9_aop *aop, u32 key, const u8 *value,
				 u32 size)
{
	struct a9_aop_service *svc = &aop->services[aop->accel];
	struct a9_aop_ep *e = a9_aop_find_ep(aop, svc->ep);
	struct a9_ring *ring;
	u32 rptr, wptr, next;
	int length, ret;

	if (!e || e->state != AFK_RUNNING || !e->ring_valid[RING_TX])
		return -ENOTCONN;
	ring = &e->rings[RING_TX];
	if (!++aop->cmd_tag)
		aop->cmd_tag = 1;
	length = a9_epic_property(aop->tx_packet, sizeof(aop->tx_packet),
				  e->outer_seq++, aop->cmd_tag, key, value, size);
	if (length < 0)
		return length;
	/* Writer reads both pointers from the shared header (bab540). */
	rptr = a9_ring_rptr(ring);
	wptr = a9_ring_wptr(ring);
	/* Observe the reader's progress before reusing its space. */
	dma_mb();
	ret = a9_ring_write(ring, rptr, wptr, svc->channel, 0, aop->tx_packet,
			    length, &next);
	if (ret)
		return ret;
	aop->cmd_pending = true;
	aop->cmd_status = -ETIMEDOUT;
	/* Entry before wptr, wptr before the rptr re-read. */
	dma_wmb();
	a9_ring_set_wptr(ring, next);
	dma_mb();
	e->tx_packets++;
	dev_info(aop->dev, "ACCEL property queued ep=0x%x channel=0x%x key=0x%x bytes=%u tag=0x%04x\n",
		 e->num, svc->channel, key, size, aop->cmd_tag);
	/* iOS rings 0xa2 only when the AOP had drained the ring. */
	if (a9_ring_rptr(ring) != wptr)
		return 0;
	aop->doorbells++;
	ret = a9_aop_send(aop, e->num, a9_afk_word(A9_AFK_DOORBELL));
	if (ret)
		aop->cmd_pending = false;
	return ret;
}

static int a9_aop_property(struct a9_aop *aop, u32 key, const u8 *value,
			   u32 size)
{
	int ret;

	mutex_lock(&aop->lock);
	ret = aop->accel >= 0 ? a9_aop_queue_property(aop, key, value, size) :
				-ENODEV;
	mutex_unlock(&aop->lock);
	if (ret)
		return ret;
	if (!a9_aop_wait(aop, !READ_ONCE(aop->cmd_pending), AOP_REPLY_MS)) {
		mutex_lock(&aop->lock);
		aop->cmd_pending = false;
		mutex_unlock(&aop->lock);
		return -ETIMEDOUT;
	}
	return READ_ONCE(aop->cmd_status);
}

static int a9_aop_property32(struct a9_aop *aop, u32 key, u32 value)
{
	u8 bytes[4];

	put_unaligned_le32(value, bytes);
	return a9_aop_property(aop, key, bytes, sizeof(bytes));
}

/* Batch 0x16, raw 0x5c, report 0 (fffffff005b99ef0); stop on any error. */
static int a9_aop_set_rate(struct a9_aop *aop, u32 batch, u32 raw, u32 report)
{
	int ret;

	ret = a9_aop_property32(aop, AOP_KEY_BATCH, batch);
	if (!ret)
		ret = a9_aop_property32(aop, AOP_KEY_RAW, raw);
	if (!ret)
		ret = a9_aop_property32(aop, AOP_KEY_REPORT, report);
	return ret;
}

/* Optional per-device file; absent blobs are skipped, never zero-filled. */
static int a9_aop_send_calibration(struct a9_aop *aop)
{
	struct a9_cal_entry entries[ARRAY_SIZE(a9_cal_order)];
	const struct firmware *fw;
	bool low_temp = false, sensitivity = false, interrupt = false;
	u32 state = 1;
	int count, i, ret;

	ret = request_firmware_direct(&fw, AOP_CAL_FW, aop->dev);
	if (ret) {
		if (aop->cold_run)
			return ret;
		if (aop->experiment_stage && ret == -ENOENT) {
			dev_warn(aop->dev, "no calibration file: experiment uses uncalibrated reports\n");
			goto send_state;
		}
		if (aop->running_attach)
			return ret; /* an adopted image still needs factory calibration */
		dev_warn(aop->dev, "no %s (%d): streaming without factory calibration\n",
			 AOP_CAL_FW, ret);
		goto send_state;
	}
	count = a9_cal_parse(fw->data, fw->size, entries, ARRAY_SIZE(entries));
	if (count < 0 || (!count && (aop->running_attach || aop->cold_run))) {
		release_firmware(fw);
		return count ?: -ENODATA;
	}
	for (i = 0; i < count; i++) {
		ret = a9_aop_property(aop, entries[i].key, entries[i].data,
				      entries[i].length);
		/* iOS b96d70 continues on kIOReturnUnsupported. In this image
		 * accel's 010091ec rejects ARot (0x18) before reading its value,
		 * independent of length, contents or sampling state. An unsupported
		 * blob is not an ACK, and must not claim firmware applied ARot.
		 */
		if (ret == -EOPNOTSUPP) {
			mutex_lock(&aop->lock);
			aop->cal_unsupported++;
			mutex_unlock(&aop->lock);
			dev_warn(aop->dev, "ACCEL calibration unsupported key=0x%x bytes=%u ioreturn=0x%08x; continuing as iOS%s\n",
				 entries[i].key, entries[i].length, A9_SPU_UNSUPPORTED,
				 entries[i].key == AOP_KEY_AROT ? " (ARot not applied)" : "");
			ret = 0;
			continue;
		}
		if (ret) {
			dev_err(aop->dev, "calibration key 0x%x: %d\n",
				entries[i].key, ret);
			break;
		}
		mutex_lock(&aop->lock);
		low_temp |= entries[i].key == 0x1a;
		sensitivity |= entries[i].key == 0x0b || entries[i].key == 0x27;
		interrupt |= entries[i].key == 0x19;
		aop->cal_sent++; /* Count acknowledgements, not attempted sends. */
		if (entries[i].key == AOP_KEY_AROT)
			aop->arot_sent = true;
		dev_info(aop->dev, "ACCEL calibration ack key=0x%x bytes=%u sent=%u\n",
			 entries[i].key, entries[i].length, aop->cal_sent);
		mutex_unlock(&aop->lock);
	}
	release_firmware(fw);
	if (ret)
		return ret;
	/* iOS b9d9e4 sends 1 (incomplete) or 2 (complete), even when ARot is
	 * unsupported. Firmware 0100ca1e stores this at source+0x50; the
	 * report gate 0100d31c returns false while it is zero. Rate ACKs alone
	 * do not open that gate. Only claim completeness for applied blobs.
	 */
	if (low_temp && sensitivity && interrupt)
		state = 2;
send_state:
	ret = a9_aop_property32(aop, AOP_KEY_CAL_STATE, state);
	if (!ret) {
		mutex_lock(&aop->lock);
		aop->cal_state = state;
		dev_info(aop->dev, "ACCEL calibration state ack key=0x3d value=%u\n", state);
		a9_aop_stage3_mark(aop, "calibration-state-acked");
		mutex_unlock(&aop->lock);
	}
	return ret;
}

/* ---- firmware load and CPU start --------------------------------------- */

struct a9_aop_regs {
	struct a9_aop *aop;
	u32 values[6];
	bool fingerprint;
	int read_errno;
};

static void a9_aop_read_regs(void *data)
{
	struct a9_aop_regs *regs = data;
	static const u32 offsets[5] = {
		AOP_CPU_CONTROL, AOP_WRAPPER_B88, AOP_INBOX_CONTROL,
		AOP_OUTBOX_CONTROL, AOP_IDLE_STATUS,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		regs->values[i] = aop_rd(regs->aop, offsets[i]);
	if (regs->aop->autoboot)
		regs->values[5] = readl(regs->aop->autoboot);
	if (regs->aop->experiment_stage)
		return; /* No firmware/remap access; only wake=4/5 maps auto-boot. */
	if (regs->fingerprint && (regs->values[1] & AOP_B88_BIT0) &&
	    (regs->values[3] & AOP_MBOX_EMPTY) &&
	    !((regs->values[2] | regs->values[3]) & AOP_MBOX_FAULTS)) {
		struct a9_attach_fingerprint *fp = &regs->aop->fingerprint;
		static const u32 remap_offsets[] = {
			AOP_REMAP_PA_LO, AOP_REMAP_PA_HI,
			AOP_REMAP_IOP_LO, AOP_REMAP_IOP_HI, AOP_REMAP_SIZE,
			AOP_REMAP_RESERVED, AOP_REMAP_ENABLE,
		};

		for (i = 0; i < ARRAY_SIZE(remap_offsets); i++)
			regs->aop->diagnostics.remap[i] = aop_rd(regs->aop, remap_offsets[i]);
		if (apple_a9_aop_debug()) {
			ret = a9_aop_fromio32(regs->aop->region, regs->aop->region_size,
					      regs->aop->diagnostics.prefix, 0,
					      A9_DIAG_PREFIX_SIZE);
			if (ret)
				goto read_error;
			memcpy(fp->head, regs->aop->diagnostics.prefix, sizeof(fp->head));
		} else {
			ret = a9_aop_fromio32(regs->aop->region, regs->aop->region_size,
					      fp->head, 0, sizeof(fp->head));
			if (ret)
				goto read_error;
		}
		ret = a9_aop_fromio32(regs->aop->region, regs->aop->region_size,
				      fp->build, A9_ATTACH_BUILD_OFFSET, sizeof(fp->build));
		if (ret)
			goto read_error;
		ret = a9_aop_fromio32(regs->aop->region, regs->aop->region_size,
				      fp->table, A9_ATTACH_TABLE_OFFSET, sizeof(fp->table));
		if (ret)
			goto read_error;
		for (i = 0; i < ARRAY_SIZE(remap_offsets); i++)
			regs->aop->diagnostics.remap_after[i] =
				aop_rd(regs->aop, remap_offsets[i]);
		regs->aop->diagnostics.mapping_matches =
			a9_attach_mapping_matches(regs->aop->diagnostics.remap,
						  regs->aop->diagnostics.remap_after,
						  regs->aop->region_pa, regs->aop->region_size);
		regs->aop->fingerprint_read = true;
		regs->aop->diagnostics.captured = apple_a9_aop_debug();
	}
	return;
read_error:
	regs->read_errno = ret;
	regs->aop->diagnostics.read_errno = ret;
}

static int a9_aop_wait_firmware(struct a9_aop *aop, const char *name,
				const struct firmware **fw)
{
	unsigned int i;
	int ret;

	/* Built in: /lib/firmware appears only after the root switch. */
	for (i = 0; i < AOP_FW_WAIT_S; i++) {
		ret = request_firmware_direct(fw, name, aop->dev);
		if (ret != -ENOENT)
			return ret;
		if (wait_event_timeout(aop->wait, READ_ONCE(aop->stopping), HZ))
			return -ESHUTDOWN;
	}
	return -ENOENT;
}

struct a9_aop_patch {
	u32 tag;
	const void *data;
	u32 size;
	bool required;
};

static int a9_aop_prepare_identity(struct a9_aop *aop, u8 *image, size_t size)
{
	const struct firmware *identity;
	u8 nonce[8];
	int ret;

	if (!force_cold || !(cold_test & A9_COLD_ECID))
		return 0;
	ret = request_firmware_direct(&identity, AOP_ECID_FW, aop->dev);
	if (ret)
		return ret;
	get_random_bytes(nonce, sizeof(nonce));
	ret = a9_fw_identity(image, size, identity->data, identity->size,
			     cold_test & A9_COLD_NONCE ? nonce : NULL);
	memzero_explicit(nonce, sizeof(nonce));
	release_firmware(identity);
	return ret;
}

/*
 * Validate the image and its tag table and build the patched copy with the
 * tags iOS writes on a cold j71t load (AppleSPU table, RTBuddy b70290 and
 * the segment loop b88238): AOPt, nCal, EC0p, WrAd, McRA, SOC_, SOCR, DEVf,
 * STKG, PRNG and RTTO. Cold identity tests additionally load the saved real
 * /chosen unique-chip-id and optionally fresh NONC, as iOS does. The pinned
 * PRNG requires nonzero ECID. Other modes retain their original defaults.
 * The ext segment stays in DRAM (EC0p = its PA).
 */
static int a9_aop_prepare_image(struct a9_aop *aop, const struct firmware *fw,
				u64 ext_pa, u8 **image)
{
	const struct a9_aop_patch *patch;
	u8 stack_guard[4], prng[32], zero8 = 0, ncal = 0;
	__le64 ext = cpu_to_le64(ext_pa), wrapper = cpu_to_le64(aop->wrapper_pa);
	__le64 rtto = 0;
	__le32 target = cpu_to_le32(aop->aop_target), mcra = cpu_to_le32(SZ_16K);
	__le32 soc = cpu_to_le32(aop->chip_id), socr = cpu_to_le32(aop->chip_revision);
	size_t table, length, value;
	u32 value_size, runtime_size;
	unsigned int i;
	u8 *copy;
	int ret;

	ret = a9_fw_table(fw->data, fw->size, &table, &length);
	if (ret)
		return ret;
	ret = a9_fw_tag(fw->data, table, length, AOP_TAG_RTSZ, &value, &value_size);
	if (ret || value_size != 4)
		return ret ?: -EPROTO;
	/* b6f340: max(image, RTSZ) must fit the target; only the image is copied. */
	runtime_size = get_unaligned_le32(fw->data + value);
	if (max_t(size_t, fw->size, runtime_size) > aop->region_size)
		return -EFBIG;
	aop->runtime_size = runtime_size;

	get_random_bytes(stack_guard, sizeof(stack_guard));
	stack_guard[get_random_u32_below(sizeof(stack_guard))] = 0;
	get_random_bytes(prng, sizeof(prng));
	{
		const struct a9_aop_patch patches[] = {
			{ AOP_TAG('A', 'O', 'P', 't'), &target, 4, true },
			{ AOP_TAG('n', 'C', 'a', 'l'), &ncal, 1, false },
			{ AOP_TAG('E', 'C', '0', 'p'), &ext, 8, true },
			{ AOP_TAG('W', 'r', 'A', 'd'), &wrapper, 8, false },
			{ AOP_TAG('M', 'c', 'R', 'A'), &mcra, 4, false },
			{ AOP_TAG('S', 'O', 'C', '_'), &soc, 4, false },
			{ AOP_TAG('S', 'O', 'C', 'R'), &socr, 4, false },
			{ AOP_TAG('D', 'E', 'V', 'f'), &zero8, 1, false },
			{ AOP_TAG('S', 'T', 'K', 'G'), stack_guard, 4, false },
			{ AOP_TAG('P', 'R', 'N', 'G'), prng, 32, false },
			{ AOP_TAG('R', 'T', 'T', 'O'), &rtto, 8, false },
		};

		copy = vmalloc(fw->size);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, fw->data, fw->size);
		for (i = 0; i < ARRAY_SIZE(patches); i++) {
			patch = &patches[i];
			ret = a9_fw_patch(copy, table, length, patch->tag,
					  patch->data, patch->size);
			/* iOS: a missing tag is a no-op, a wrong length fatal. */
			if (ret == -ENOENT && !patch->required)
				ret = 0;
			if (ret)
				break;
		}
	}
	memzero_explicit(prng, sizeof(prng));
	if (!ret)
		ret = a9_aop_prepare_identity(aop, copy, fw->size);
	if (ret) {
		vfree(copy);
		return ret;
	}
	aop->table = devm_kmemdup(aop->dev, copy + table, length, GFP_KERNEL);
	aop->table_size = aop->table ? length : 0;
	*image = copy;
	return 0;
}

static int a9_aop_copy_verify(void __iomem *dst, const u8 *src, size_t size)
{
	struct a9_aop_io32 io = { .base = dst };
	u8 chunk[256];
	size_t done, n;
	int ret;

	ret = a9_aop_copy_to32(&io, a9_aop_io_write32, src, 0, size, size);
	if (ret)
		return ret;
	for (done = 0; done < size; done += n) {
		n = min(size - done, sizeof(chunk));
		ret = a9_aop_fromio32(dst, size, chunk, done, n);
		if (ret)
			return ret;
		if (memcmp(chunk, src + done, n))
			return -EIO;
	}
	return 0;
}

static bool a9_aop_pinned_firmware(const struct firmware *fw, bool external);

static void a9_aop_cold_free(void *data)
{
	struct a9_aop *aop = data;

	vfree(aop->cold_before);
	vfree(aop->cold_after);
}

/* Called only under the existing PMGR guard and aop->lock. */
static void a9_aop_cold_read(void *data)
{
	struct a9_aop *aop = data;
	struct a9_aop_cold_frame *frame = &aop->cold_frame[aop->cold_frames];
	unsigned int i;
	u8 *dst;

	frame->ns = ktime_get_boottime_ns();
	for (i = 0; i < ARRAY_SIZE(a9_cold_reg_offsets); i++)
		frame->regs[i] = aop_rd(aop, a9_cold_reg_offsets[i]);
	frame->auto_boot = readl(aop->autoboot);
	if ((aop_rd(aop, AOP_INBOX_CONTROL) | aop_rd(aop, AOP_OUTBOX_CONTROL)) &
	    AOP_MBOX_FAULTS) {
		frame->error = -EIO;
		return;
	}
	dst = aop->cold_baseline ? aop->cold_after : aop->cold_before;
	frame->error = a9_aop_fromio32(aop->region, aop->region_size, dst,
				     0, aop->region_size);
	if (frame->error)
		return;
	if (!aop->cold_baseline) {
		memcpy(aop->cold_after, dst, aop->region_size);
		aop->cold_baseline = true;
	}
	sha256(aop->cold_after, aop->region_size, frame->sha256);
	frame->guard = get_unaligned_le32(aop->cold_after + A9_ATTACH_BUILD_OFFSET +
					A9_ATTACH_GUARD_OFFSET);
	/* STKG is the first tag in this SHA-pinned image's fixed table. */
	frame->tag_guard = get_unaligned_le32(aop->cold_after + A9_ATTACH_TABLE_OFFSET + 8);
	for (i = 0; i < ARRAY_SIZE(a9_cold_spans); i++) {
		u32 begin = a9_cold_spans[i].begin;

		frame->delta[i] = a9_aop_memory_compare(aop->cold_before + begin,
						      aop->cold_after + begin,
						      a9_cold_spans[i].end - begin);
	}
}

static int a9_aop_cold_capture(struct a9_aop *aop, const char *event)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_cold_frame *frame;
	unsigned int i;
	int ret;

	mutex_lock(&aop->lock);
	if (aop->cold_frames == AOP_COLD_FRAMES) {
		mutex_unlock(&aop->lock);
		return -ENOSPC;
	}
	frame = &aop->cold_frame[aop->cold_frames];
	frame->event = event;
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_cold_read, aop);
	frame->error = ret ?: frame->error;
	ret = frame->error;
	aop->cold_frames++;
	mutex_unlock(&aop->lock);
	dev_info(aop->dev, "COLD-M event=%s ns=%llu errno=%d bytes=0x%zx sha256=%*phN guard=%08x STKG=%08x sample=non-atomic\n",
		 event, frame->ns, ret, (size_t)aop->region_size, SHA256_DIGEST_SIZE,
		 frame->sha256, frame->guard, frame->tag_guard);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(a9_cold_spans); i++) {
		struct a9_aop_memory_delta *delta = &frame->delta[i];

		dev_info(aop->dev, "COLD-M span=%s changed_words=%u first=%08x before=%08x after=%08x\n",
			 a9_cold_spans[i].name, delta->words,
			 delta->words ? a9_cold_spans[i].begin + delta->first : (u32)-1,
			 delta->before, delta->after);
	}
	dev_info(aop->dev, "COLD-F event=%s b88=%08x ba0=%08x in4008=%08x out4020=%08x mask4000=%08x maskc00=%08x idle=%08x\n",
		 event, frame->regs[10], frame->regs[11], frame->regs[16],
		 frame->regs[17], frame->regs[14], frame->regs[12], frame->regs[18]);
	return 0;
}

/* Poll the vendor outbox synchronously before starting the background poller.
 * RUN readback does not prove a reset: a retained sleeping instance can need
 * the b65cdc/b71fc4 wake used by RTBuddy's old-status-3 restart. This is one
 * bounded hypothesis test, not an assertion that our unknown state is 3.
 */
static int a9_aop_cold_hello(struct a9_aop *aop)
{
	static const unsigned int times[] = { 10, 100, AOP_COLD_WAKE_MS, 5000 };
	static const char * const events[] = { "10ms", "100ms", "pre-wake", "5s" };
	u64 deadline = aop->cold_release_ns + (u64)AOP_COLD_HELLO_MS * NSEC_PER_MSEC;
	unsigned int next = 0;
	int ret = 0;

	aop->inline_rx = true;
	while (!a9_aop_dead(aop)) {
		u64 now;

		a9_aop_inline_rx(aop);
		if (aop->hello || a9_aop_dead(aop))
			break;
		now = ktime_get_boottime_ns();
		if (now >= deadline)
			break;
		if (next < ARRAY_SIZE(times) &&
		    now - aop->cold_release_ns >= (u64)times[next] * NSEC_PER_MSEC) {
			ret = a9_aop_cold_capture(aop, events[next]);
			if (ret)
				break;
			if (times[next] == AOP_COLD_WAKE_MS) {
				/* SRAM sampling can span the first HELLO arrival. */
				a9_aop_inline_rx(aop);
				if (aop->hello || a9_aop_dead(aop) ||
				    ktime_get_boottime_ns() >= deadline)
					break;
				aop->experiment_step = "cold-wake220";
				aop->cold_wake_sent = true;
				ret = a9_aop_send(aop, 0, 0x0060000000000220ULL);
				aop->cold_wake_errno = ret;
				dev_info(aop->dev, "COLD wake220 attempt=1 errno=%d state3=unverified\n",
					 ret);
				if (ret)
					break;
				ret = a9_aop_cold_capture(aop, "post-wake");
				if (ret)
					break;
			}
			next++;
		}
		usleep_range(500, 1000);
	}
	/* Final synchronous drain catches a word posted at the deadline edge. */
	if (!ret && !a9_aop_dead(aop)) {
		a9_aop_inline_rx(aop);
		ret = a9_aop_cold_capture(aop, aop->hello ? "hello" : "deadline");
	}
	aop->inline_rx = false;
	return ret ?: (a9_aop_dead(aop) ? -EIO : aop->hello ? 0 : -ETIMEDOUT);
}

/* Require the RAM backend itself, never an EFI/NVRAM pstore substitute. */
static bool a9_aop_ramoops_ready(void)
{
	struct console *con;
	bool ready = false;
	int cookie;

	cookie = console_srcu_read_lock();
	for_each_console_srcu(con) {
		if (!strcmp(con->name, "ramoops") &&
		    (console_srcu_read_flags(con) & (CON_ENABLED | CON_SUSPENDED)) ==
		    CON_ENABLED) {
			ready = true;
			break;
		}
	}
	console_srcu_read_unlock(cookie);
	return ready;
}

static int a9_aop_cold_checkpoint(struct a9_aop *aop, const char *step)
{
	if (!a9_aop_ramoops_ready())
		return -ENODEV;
	dev_info(aop->dev, "COLD checkpoint=%s defer=%u test=%u stage=%u ap_irqs=masked firmware=%pa ext=%pad frames=%u\n",
		 step, cold_defer, cold_test, aop->experiment_stage,
		 &aop->region_pa, &aop->ext_dma, aop->cold_frames);
	/* ramoops' console is continuously written through its WC mapping.
	 * Wait for console output and drain stores before permitting RUN.
	 * No synthetic oops/panic, persistent variable or storage write.
	 */
	if (!pr_flush(1000, false))
		return -ETIMEDOUT;
	mb(); /* Complete ramoops WC stores before the next control write. */
	return 0;
}

/* ---- SPI0 pre-reset (aop_spi0_reset=1) ----------------------------------- */

static void a9_aop_spi0_snapshot(void __iomem *base, u32 *values)
{
	unsigned int i;

	for (i = 0; i < A9_SPI0_REGS; i++)
		values[i] = readl(base + a9_spi0_offsets[i]);
}

/*
 * AOPSPIMaster_reset (0102f2f4) under the AOP-domain PMGR guard with IRQs
 * off and the AOP CPU halted. Its provider power transition 3 is an
 * AOP-local call with no AP register equivalent and is not replayed. A poll
 * timeout still restores the saved control word.
 */
static void a9_aop_spi0_reset_locked(void *data)
{
	struct a9_aop_spi0 *s = data;
	void __iomem *control = s->base + A9_SPI0_CONTROL;
	u32 value = 0;
	int ret;

	a9_aop_spi0_snapshot(s->base, s->before);
	s->saved = s->before[A9_SPI0_I_CONTROL];
	s->asserted = a9_spi0_base_word(s->saved) | A9_SPI0_RESET;
	writel(s->asserted, control);
	ret = readl_poll_timeout_atomic(control, value, value & A9_SPI0_RESET,
					1, A9_SPI0_POLL_US);
	if (!ret)
		ret = readl_poll_timeout_atomic(control, value,
						!(value & A9_SPI0_ENABLE), 1,
						A9_SPI0_POLL_US);
	s->polled = value;
	if (!ret) {
		udelay(A9_SPI0_DELAY_US);
		writel(a9_spi0_base_word(s->saved), control);
		udelay(A9_SPI0_DELAY_US);
	}
	writel(s->saved, control);
	a9_aop_spi0_snapshot(s->base, s->after);
	if (!ret && (s->after[A9_SPI0_I_FAULT] & A9_SPI0_CH3_PENDING))
		ret = -EIO;
	s->error = ret;
}

static void a9_aop_spi0_ch3_snapshot(void __iomem *base, u32 *values)
{
	unsigned int i;

	for (i = 0; i < A9_SPI0_CH3_REGS; i++)
		values[i] = readl(base + a9_spi0_ch3_offsets[i]);
}

static void a9_aop_spi0_trace_read(void *data)
{
	struct a9_aop_spi0 *s = data;
	unsigned int i;

	s->trace.control = readl(s->base + A9_SPI0_CONTROL);
	s->trace.clock = readl(s->base + A9_SPI0_CLOCK);
	for (i = 0; i < A9_SPI0_SCM_REGS; i++)
		s->trace.scm[i] = readl(s->base + a9_spi0_scm_offsets[i]);
	a9_aop_spi0_ch3_snapshot(s->base, s->trace.ch3);
}

/* Warm path only, CPU halted. Log outside the PMGR guard; no SCM writes. */
static int a9_aop_spi0_trace(struct a9_aop *aop, const char *stage)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_spi0 *s = &aop->spi0;
	const u32 *c = s->trace.ch3, *m = s->trace.scm;
	int ret;

	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_spi0_trace_read, s);
	if (ret) {
		s->intc.failed = "failed-spi0-trace-guard";
		dev_info(aop->dev, "SPI0 SCM stage=%s errno=%d: guard refused\n", stage, ret);
		return ret;
	}
	dev_info(aop->dev, "SPI0 SCM stage=%s errno=0 ctl=%08x clk=%08x scm_lo=%08x scm_hi=%08x c3_interval=%08x c3_cursor=%08x c3=%08x route=%08x rate=%08x c3c=%08x desc=%08x 8d0=%08x 8d4=%08x cpu=halted\n",
		 stage, s->trace.control, s->trace.clock, m[0], m[1], m[2], m[3],
		 c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
	return 0;
}

/*
 * AOPSPIMaster_teardownChannel (0102e4f8) for channel 3, then the fault
 * handler's acknowledge (0102f20a), under the same guard as the reset with
 * the CPU halted. Every store is a read-modify-write except the descriptor,
 * which the firmware zeroes. The idle wait runs in 1 ms slices, up to twice
 * the firmware's own 10 ms SCMto bound; each slice ends with read-only
 * samples of C3, the SCM state and +0x8d0. An idle timeout stops before the
 * masks. The acknowledge bit self-clears; +0x8d0 bit 6 shows whether it
 * took effect.
 */
static void a9_aop_spi0_ch3_locked(void *data)
{
	struct a9_aop_spi0 *s = data;
	struct a9_aop_spi0_ch3 *c = &s->ch3;
	void __iomem *c3 = s->base + A9_SPI0_CH3;
	void __iomem *c3c = s->base + A9_SPI0_CH3_0C;
	struct a9_aop_ch3_slice *p;
	u32 value = 0;
	u64 start;

	a9_aop_spi0_ch3_snapshot(s->base, c->before);
	c->run_before = c->before[A9_CH3_I_C3] & A9_SPI0_CH3_RUN;
	c->stop = readl(c3) & ~A9_SPI0_CH3_STOP;
	writel(c->stop, c3);
	start = ktime_get_ns();
	c->poll = -ETIMEDOUT;
	for (c->slices = 0; c->poll && c->slices < A9_SPI0_CH3_SLICES; c->slices++) {
		p = &c->slice[c->slices];
		c->poll = readl_poll_timeout_atomic(c3, value, value & A9_SPI0_CH3_IDLE,
						    1, A9_SPI0_CH3_SLICE_US);
		p->ns = ktime_get_ns() - start;
		p->c3 = value;
		p->scm_lo = readl(s->base + A9_SPI0_SCM_LO);
		p->scm_hi = readl(s->base + A9_SPI0_SCM_HI);
		p->fault = readl(s->base + A9_SPI0_FAULT);
	}
	c->idle = value;
	if (!c->poll) {
		c->idle_us = div_u64(c->slice[c->slices - 1].ns, NSEC_PER_USEC);
		c->class0 = readl(c3) & ~A9_SPI0_CH3_CLASS0;
		writel(c->class0, c3);
		c->class1 = readl(c3) & ~A9_SPI0_CH3_CLASS1;
		writel(c->class1, c3);
		writel(0, s->base + A9_SPI0_CH3_DESC);
		c->ack = readl(c3c) | A9_SPI0_CH3_ACK;
		writel(c->ack, c3c);
	}
	a9_aop_spi0_ch3_snapshot(s->base, c->after);
	c->failed = c->poll ? "failed-ch3-teardown-idle" : a9_spi0_ch3_verify(c->after);
	c->error = c->poll ?: c->failed ? -EIO : 0;
}

/* Called only if ps_spi0 is fully on; reads nothing. */
static void a9_aop_spi0_pmgr_on(void *data)
{
	*(bool *)data = true;
}

/* AP PMGR ps_spi0, read-only: never requested, resumed or written.
 * 0 = on, -EHOSTDOWN = off (raw still filled), -ENODEV = no node.
 */
static int a9_aop_spi0_pmgr(struct a9_aop *aop, u32 *raw)
{
	struct apple_pmgr_observation state = {};
	bool on = false;
	int ret;

	if (!aop->spi0_pmgr)
		return -ENODEV;
	ret = apple_pmgr_observe(&aop->spi0_pmgr, 1, &state,
				 a9_aop_spi0_pmgr_on, &on);
	*raw = state.raw;
	return ret ?: on ? 0 : -EIO;
}

/*
 * After a reset that cleared +0x8d0 bit 6, before the image copy: CPU halted,
 * AP notifications masked. A refusal sets ch3.failed and nothing is loaded.
 */
static int a9_aop_spi0_ch3_teardown(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_spi0 *s = &aop->spi0;
	struct a9_aop_spi0_ch3 *c = &s->ch3;
	const u32 *b = c->before, *a = c->after;
	const struct a9_aop_ch3_slice *p;
	unsigned int i;
	int ret;

	aop->experiment_step = "spi0-ch3-teardown";
	dev_info(aop->dev, "SPI0 CH3TD begin pa=%llx cpu=halted: first channel-3 store follows\n",
		 A9_SPI0_PA + A9_SPI0_CH3);
	if (cold_defer) {
		ret = a9_aop_cold_checkpoint(aop, "spi0-ch3-teardown");
		if (ret) {
			c->failed = "failed-ch3-teardown-checkpoint";
			return ret;
		}
	}
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_spi0_ch3_locked, s);
	if (ret) {
		c->failed = "failed-ch3-teardown-guard";
		dev_info(aop->dev, "SPI0 CH3TD errno=%d ran=0: guard refused, channel 3 not touched\n",
			 ret);
		return ret;
	}
	c->ran = true;
	dev_info(aop->dev, "SPI0 CH3TD before c3=%08x route=%08x rate=%08x c3c=%08x desc=%08x 8d0=%08x 8d4=%08x\n",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
	for (i = 0; i < c->slices; i++) {
		p = &c->slice[i];
		dev_info(aop->dev, "SPI0 CH3TD idle-sample slice=%u at_us=%llu c3=%08x scm_lo=%08x scm_hi=%08x 8d0=%08x idle=%u\n",
			 i, div_u64(p->ns, NSEC_PER_USEC), p->c3, p->scm_lo,
			 p->scm_hi, p->fault, !!(p->c3 & A9_SPI0_CH3_IDLE));
	}
	/* beyond_fw_bound=1: idle, but later than the firmware would wait. */
	dev_info(aop->dev, "SPI0 CH3TD idle-poll run_before=%u slices=%u/%u idle_us=%llu bound_us=%u fw_bound_us=%u beyond_fw_bound=%u result=%s\n",
		 c->run_before, c->slices, A9_SPI0_CH3_SLICES, c->idle_us,
		 A9_SPI0_CH3_SLICES * A9_SPI0_CH3_SLICE_US, A9_SPI0_CH3_IDLE_FW_US,
		 !c->poll && c->idle_us > A9_SPI0_CH3_IDLE_FW_US,
		 c->poll ? "timeout" : "idle");
	if (c->poll)
		dev_info(aop->dev, "SPI0 CH3TD writes stop=%08x idle_poll=%08x poll_errno=%d masks=skipped: idle bit 1 never set, no further store\n",
			 c->stop, c->idle, c->poll);
	else
		dev_info(aop->dev, "SPI0 CH3TD writes stop=%08x idle_poll=%08x poll_errno=0 class0=%08x class1=%08x desc=00000000 c3c=%08x\n",
			 c->stop, c->idle, c->class0, c->class1, c->ack);
	dev_info(aop->dev, "SPI0 CH3TD after c3=%08x route=%08x rate=%08x c3c=%08x desc=%08x 8d0=%08x 8d4=%08x\n",
		 a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
	/* c3c_bit5 is the pulse's readback, logged only (0 at 19:50). */
	dev_info(aop->dev, "SPI0 CH3TD check errno=%d run=%u class0=%u class1=%u idle=%u desc=%08x c3c_bit5=%u ch3_pending=%u result=%s\n",
		 c->error, !!(a[A9_CH3_I_C3] & A9_SPI0_CH3_RUN),
		 !!(a[A9_CH3_I_C3] & A9_SPI0_CH3_IRQ0),
		 !!(a[A9_CH3_I_C3] & A9_SPI0_CH3_IRQ1), !!(a[A9_CH3_I_C3] & A9_SPI0_CH3_IDLE),
		 a[A9_CH3_I_DESC], !!(a[A9_CH3_I_C3C] & A9_SPI0_CH3_ACK),
		 !!(a[A9_CH3_I_8D0] & A9_SPI0_CH3_PENDING), c->failed ?: "pass");
	return c->error;
}

/*
 * The ten INTC words of a9_intc_offsets into state[@at], read only. The
 * INTC is the AOP wrapper block (0xc1800000 in the firmware, space 2 as
 * SPI0's verified 0xc1100000 = 0x210100000), which this driver reads
 * unguarded for the mailbox and CPU control; no separate PMGR domain.
 */
static void a9_aop_intc_snapshot(struct a9_aop *aop, unsigned int at)
{
	struct a9_aop_intc *n = &aop->spi0.intc;
	u32 *v = n->state[at];
	unsigned int i;

	for (i = 0; i < A9_INTC_REGS; i++)
		v[i] = aop_rd(aop, a9_intc_offsets[i]);
	n->states |= BIT(at);
	dev_info(aop->dev, "AOP INTC state at=%s reset808=%08x current81c=%08x raise900=%08x ack980=%08x mask_a00=%08x mask_a04=%08x mask_a08=%08x mask_a80=%08x g7_c020=%08x g7_c024=%08x src_masked=%u view=%s\n",
		 a9_intc_at_name(at), v[0], v[1], v[2], v[3], v[4], v[5], v[6],
		 v[7], v[8], v[9], !!(v[A9_INTC_I_MASK0] & A9_INTC_SPI0_SCM),
		 a9_intc_view(v));
}

/*
 * Round 4 (ds-aop-spi0-ch3-round4.md): the old session's SPI0 global IRQ
 * 0x10014 stays latched in the AOP interrupt controller, and fresh firmware
 * takes it as soon as SPI lifecycle event 1 enables it, before its own SPI0
 * reset (SCMErr:0x3,3). With the CPU halted and before the first SPI0
 * access, disable that source (group 1 disable bank, 0102c9ce). The bank
 * reads back the mask, as the firmware's own reads of it show: bit 20 must
 * read 1 after the store.
 */
static int a9_aop_intc_disable(struct a9_aop *aop)
{
	struct a9_aop_intc *n = &aop->spi0.intc;
	const u32 *b = n->state[A9_INTC_AT_PRE_DISABLE];
	const u32 *a = n->state[A9_INTC_AT_POST_DISABLE];
	int ret;

	aop->experiment_step = "intc-disable";
	dev_info(aop->dev, "AOP INTC disable begin irq=0x10014 pa=%llx value=%08x cpu=halted: first INTC store follows\n",
		 A9_SPI0_WRAPPER_PA + A9_INTC_DISABLE, A9_INTC_SPI0_SCM);
	if (cold_defer) {
		ret = a9_aop_cold_checkpoint(aop, "intc-disable");
		if (ret) {
			n->failed = "failed-intc-disable-checkpoint";
			return ret;
		}
	}
	a9_aop_intc_snapshot(aop, A9_INTC_AT_PRE_DISABLE);
	ret = aop_wr(aop, A9_INTC_DISABLE, A9_INTC_SPI0_SCM);
	a9_aop_intc_snapshot(aop, A9_INTC_AT_POST_DISABLE);
	if (ret) {
		n->failed = "failed-intc-disable";
	} else if (!(a[A9_INTC_I_MASK0] & A9_INTC_SPI0_SCM)) {
		n->failed = "failed-intc-disable-readback";
		ret = -EIO;
	}
	n->disabled = !ret;
	dev_info(aop->dev, "AOP INTC disable errno=%d before=%08x value=%08x after=%08x mask_a80=%08x src_masked=%u view=%s result=%s\n",
		 ret, b[A9_INTC_I_MASK0], A9_INTC_SPI0_SCM, a[A9_INTC_I_MASK0],
		 a[A9_INTC_I_ENABLE0], !!(a[A9_INTC_I_MASK0] & A9_INTC_SPI0_SCM),
		 a9_intc_view(a), n->failed ?: "pass");
	return ret;
}

/*
 * After the proven halt/drain, before any SRAM write. Only firmware that ran
 * since the last SoC reset leaves SPI0 state behind: auto-boot 1, or the
 * previous kernel's transcript bound by +0x4020 (a9_spi0_decide). Without
 * that evidence the block is never touched. @auto_boot and @outbox are the
 * boot snapshot from before the stop. Nonzero means: do not load or
 * release the AOP.
 */
static int a9_aop_spi0_prereset(struct a9_aop *aop, u32 auto_boot, u32 outbox)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_spi0 *s = &aop->spi0;
	const u32 *b = s->before, *a = s->after;
	bool run;
	int ret;

	s->auto_boot = auto_boot;
	s->outbox = outbox;
	s->reason = a9_spi0_decide(auto_boot, outbox & AOP_OUTBOX_ENABLE, &s->prev);
	s->decided = true;
	run = a9_spi0_reason_runs(s->reason);
	dev_info(aop->dev, "SPI0 reset decision=%s reason=%s auto_boot=%u out4020=%08x prev=%s prev_hello=%d prev_halted=%d prev_steps=0x%x prev_carry=%d%s\n",
		 run ? "run" : "skip", a9_spi0_reason_name(s->reason), auto_boot,
		 outbox, a9_spi0_prev_name(s->prev.state), s->prev.hello,
		 s->prev.halted, s->prev.steps, s->prev.carried,
		 run ? "" : ": block not touched");
	if (!run) {
		s->skipped = a9_spi0_reason_name(s->reason);
		return 0;
	}
	if (!s->base || !aop->force_halted || !aop->force_drained ||
	    !a9_aop_force_stopped(aop))
		return -EPERM;
	ret = a9_aop_intc_disable(aop);
	if (ret)
		return ret;
	aop->experiment_step = "spi0-reset";
	s->pmgr_before_errno = a9_aop_spi0_pmgr(aop, &s->pmgr_before);
	/* Reaches ramoops before the first AP access to the block. */
	dev_info(aop->dev, "SPI0 reset begin pa=%llx cpu=halted ps_spi0=%08x errno=%d: first AP access follows\n",
		 A9_SPI0_PA, s->pmgr_before, s->pmgr_before_errno);
	if (cold_defer) {
		ret = a9_aop_cold_checkpoint(aop, "spi0-reset");
		if (ret)
			return ret;
	}
	ret = a9_aop_spi0_trace(aop, "before-controller-reset");
	if (ret)
		return ret;
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_spi0_reset_locked, s);
	s->ran = !ret;
	s->error = ret ?: s->error;
	s->pmgr_after_errno = a9_aop_spi0_pmgr(aop, &s->pmgr_after);
	dev_info(aop->dev, "SPI0 reset errno=%d ran=%d pa=%llx before ctl=%08x clk=%08x 8d0=%08x 8d4=%08x ch3=%08x ch3c=%08x\n",
		 s->error, s->ran, A9_SPI0_PA, b[0], b[1], b[2], b[3], b[4], b[5]);
	dev_info(aop->dev, "SPI0 reset after ctl=%08x clk=%08x 8d0=%08x 8d4=%08x ch3=%08x ch3c=%08x saved=%08x asserted=%08x polled=%08x ch3_pending=%u->%u provider3=not-replayed\n",
		 a[0], a[1], a[2], a[3], a[4], a[5], s->saved, s->asserted,
		 s->polled, !!(b[A9_SPI0_I_FAULT] & A9_SPI0_CH3_PENDING),
		 !!(a[A9_SPI0_I_FAULT] & A9_SPI0_CH3_PENDING));
	dev_info(aop->dev, "SPI0 pmgr ps_spi0 before=%08x errno=%d after=%08x errno=%d (read-only, never powered)\n",
		 s->pmgr_before, s->pmgr_before_errno, s->pmgr_after,
		 s->pmgr_after_errno);
	if (s->error)
		return s->error;
	ret = a9_aop_spi0_trace(aop, "after-controller-reset");
	if (!ret)
		ret = a9_aop_spi0_ch3_teardown(aop);
	if (!ret)
		ret = a9_aop_spi0_trace(aop, "after-ch3-teardown");
	return ret;
}

/* A set bit 6 also captures the channel-3 registers in the same guard. */
static void a9_aop_spi0_fault_read(void *data)
{
	struct a9_aop_spi0 *s = data;

	s->prerun = readl(s->base + A9_SPI0_FAULT);
	if (s->prerun & A9_SPI0_CH3_PENDING)
		a9_aop_spi0_ch3_snapshot(s->base, s->ch3.reassert);
}

/*
 * Last AP look before RUN, CPU still halted, after the image copy: bit 6
 * must stay clear in every one of 31 guarded samples spaced >= 5 ms apart,
 * spanning >= 150 ms. A reasserted channel-3 bit would crash the fresh
 * image as before, so the first set sample refuses the release; nothing is
 * retried. Any refusal sets ch3.failed.
 */
static int a9_aop_spi0_recheck(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_spi0 *s = &aop->spi0;
	struct a9_aop_spi0_ch3 *c = &s->ch3;
	const u32 *r = c->reassert;
	u64 start = ktime_get_boottime_ns();
	unsigned int i;
	int ret = 0;

	for (i = 0; i < A9_SPI0_WINDOW_SAMPLES; i++) {
		if (i)
			usleep_range(A9_SPI0_WINDOW_US, A9_SPI0_WINDOW_US + 1000);
		ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
					 a9_aop_spi0_fault_read, s);
		c->samples = i + 1;
		c->elapsed_ms = div_u64(ktime_get_boottime_ns() - start, NSEC_PER_MSEC);
		if (ret) {
			c->failed = "failed-ch3-window-guard";
			break;
		}
		if (s->prerun & A9_SPI0_CH3_PENDING) {
			s->reasserted = c->reasserted = true;
			c->reassert_sample = i;
			c->reassert_ms = c->elapsed_ms;
			c->failed = "failed-ch3-reasserted";
			ret = -EIO;
			dev_info(aop->dev, "SPI0 CH3TD reasserted sample=%u at_ms=%llu c3=%08x route=%08x rate=%08x c3c=%08x desc=%08x 8d0=%08x 8d4=%08x cpu=halted\n",
				 i, c->reassert_ms, r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
			break;
		}
	}
	/* usleep_range() cannot return early; this only trips on a bad clock. */
	if (!ret && c->elapsed_ms < A9_SPI0_WINDOW_MS) {
		c->failed = "failed-ch3-window-short";
		ret = -EIO;
	}
	c->window_done = !ret;
	dev_info(aop->dev, "SPI0 CH3TD window errno=%d samples=%u/%u elapsed_ms=%llu min_ms=%u result=%s\n",
		 ret, c->samples, A9_SPI0_WINDOW_SAMPLES, c->elapsed_ms,
		 A9_SPI0_WINDOW_MS, ret ? c->failed : "pass");
	dev_info(aop->dev, "SPI0 pre-run errno=%d 8d0=%08x ch3_pending=%u cpu=halted\n",
		 ret, s->prerun, !!(s->prerun & A9_SPI0_CH3_PENDING));
	return ret;
}

/* SCM state and the channel-3 interval pair, read-only, into scm[scm_at]. */
static void a9_aop_spi0_scm_read(void *data)
{
	struct a9_aop_spi0 *s = data;
	unsigned int i;

	for (i = 0; i < A9_SPI0_SCM_REGS; i++)
		s->intc.scm[s->intc.scm_at][i] = readl(s->base + a9_spi0_scm_offsets[i]);
}

/*
 * After the clean pre-RUN window, CPU still halted: the firmware's group-1
 * pre-handler acknowledge for 0x10014 (0102c6de..0102c6f2) discards the
 * old-session latch now that its SPI0 cause is quiet. Fresh firmware
 * re-enables the source in event 1. The INTC words, the SCM state and the
 * C3 interval pair are logged around it and never gated: with 0x10014
 * masked, no readable word shows whether it is still pending.
 */
static int a9_aop_intc_ack(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_spi0 *s = &aop->spi0;
	struct a9_aop_intc *n = &s->intc;
	const u32 *p = n->scm[0], *q = n->scm[1];
	const u32 *b = n->state[A9_INTC_AT_PRE_ACK];
	const u32 *a = n->state[A9_INTC_AT_POST_ACK];
	int ret;

	aop->experiment_step = "intc-ack";
	n->scm_at = 0;
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_spi0_scm_read, s);
	if (ret) {
		n->failed = "failed-intc-ack-guard";
		dev_info(aop->dev, "AOP INTC pre-ack errno=%d: guard refused, no acknowledge\n",
			 ret);
		return ret;
	}
	a9_aop_intc_snapshot(aop, A9_INTC_AT_PRE_ACK);
	dev_info(aop->dev, "AOP INTC pre-ack scm_lo=%08x scm_hi=%08x c3_interval=%08x c3_cursor=%08x current=%08x cpu=halted\n",
		 p[0], p[1], p[2], p[3], b[A9_INTC_I_CURRENT]);
	ret = aop_wr(aop, A9_INTC_ACK, A9_INTC_SPI0_SCM);
	/* Also the posted-write barrier. */
	a9_aop_intc_snapshot(aop, A9_INTC_AT_POST_ACK);
	n->acked = !ret;
	dev_info(aop->dev, "AOP INTC ack errno=%d before=%08x value=%08x after=%08x\n",
		 ret, b[A9_INTC_I_ACK], A9_INTC_SPI0_SCM, a[A9_INTC_I_ACK]);
	if (ret) {
		n->failed = "failed-intc-ack";
		return ret;
	}
	n->scm_at = 1;
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_spi0_scm_read, s);
	if (ret)
		n->failed = "failed-intc-post-guard";
	dev_info(aop->dev, "AOP INTC post-ack errno=%d scm_lo=%08x scm_hi=%08x c3_interval=%08x c3_cursor=%08x current=%08x result=%s\n",
		 ret, q[0], q[1], q[2], q[3], a[A9_INTC_I_CURRENT], n->failed ?: "pass");
	return ret;
}

/*
 * Wake7 still took 0x10014 before SPI event 5 despite the targeted ack.
 * Replay INTC lifecycle event 3 (0102c592/598/59c/5a2): 1/read/0/read at
 * +0x808. aop_wr includes a posted-write read after each store. This resets
 * all AOP-private interrupt state; it does not write the SCM status or MPU.
 * No assumption that SCM must become zero. Only the final reset readback
 * and the established channel-3 quiet conditions gate RUN.
 */
static int a9_aop_intc_reset(struct a9_aop *aop)
{
	struct a9_aop_spi0 *s = &aop->spi0;
	struct a9_aop_intc *n = &s->intc;
	int ret;

	if (!aop->force_halted || !aop->force_drained || !s->ch3.window_done ||
	    !n->acked || n->reset_done ||
	    (aop_rd(aop, AOP_CPU_CONTROL) & AOP_CPU_START) || readl(aop->autoboot)) {
		n->failed = "failed-intc-reset-state";
		return -EPERM;
	}
	ret = a9_aop_spi0_trace(aop, "pre-intc-reset");
	if (ret)
		return ret;
	aop->experiment_step = "intc-reset";
	dev_info(aop->dev, "AOP INTC reset begin pa=%llx sequence=1/read/0/read cpu=halted\n",
		 A9_SPI0_WRAPPER_PA + A9_INTC_RESET);
	ret = aop_wr(aop, A9_INTC_RESET, 1);
	n->reset_asserted = aop_rd(aop, A9_INTC_RESET);
	if (ret) {
		n->failed = "failed-intc-reset-assert";
		return ret;
	}
	ret = aop_wr(aop, A9_INTC_RESET, 0);
	n->reset_after = aop_rd(aop, A9_INTC_RESET);
	if (ret) {
		n->failed = "failed-intc-reset-deassert";
		return ret;
	}
	if (n->reset_after) {
		n->failed = "failed-intc-reset-readback";
		ret = -EIO;
	} else {
		ret = a9_aop_spi0_trace(aop, "post-intc-reset");
		if (!ret && a9_spi0_ch3_verify(s->trace.ch3)) {
			n->failed = "failed-intc-reset-ch3";
			ret = -EIO;
		}
	}
	n->reset_done = !ret;
	dev_info(aop->dev, "AOP INTC reset errno=%d asserted=%08x after=%08x result=%s\n",
		 ret, n->reset_asserted, n->reset_after, n->failed ?: "pass");
	return ret;
}

static int a9_aop_cold_start(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_regs regs = { .aop = aop };
	const struct firmware *fw = NULL, *ext = NULL;
	u64 pa = aop->region_pa;
	u8 *image = NULL;
	u32 control;
	size_t offset;
	unsigned int i;
	const struct { u32 offset, value; } writes[] = {
		{ AOP_REMAP_IOP_LO, 0 }, { AOP_REMAP_IOP_HI, 0 },
		{ AOP_REMAP_SIZE, aop->region_size & ~0xfffU },
		{ AOP_REMAP_RESERVED, 0 },
		{ AOP_REMAP_PA_LO, lower_32_bits(pa) },
		{ AOP_REMAP_PA_HI, upper_32_bits(pa) },
		{ AOP_REMAP_ENABLE, 1 },
		{ AOP_IRQ_MASK_SET, AOP_IRQ_ALL },
		{ AOP_IRQ_MIRROR_SET, AOP_IRQ_ALL },
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT0 },
		{ AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT0 },
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT12 },
		{ AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT12 },
	};
	int ret;

	if (force_cold && (!aop->force_halted || !aop->force_drained))
		return -EPERM;
	mutex_lock(&aop->lock);
	aop->phase = PHASE_WAIT_FIRMWARE;
	mutex_unlock(&aop->lock);
	ret = a9_aop_wait_firmware(aop, AOP_MAIN_FW, &fw);
	if (!ret)
		ret = request_firmware_direct(&ext, AOP_EXT_FW, aop->dev);
	if (ret)
		goto out;

	if (!a9_aop_pinned_firmware(fw, false) || !a9_aop_pinned_firmware(ext, true)) {
		ret = -EILSEQ;
		goto out;
	}
	/* Forced boots retain ext in the same no-map pool as the AFK grants.
	 * Reserve 16K alignment (McRA), including on a kernel with 4K pages.
	 */
	aop->ext_size = ext->size;
	if (force_cold) {
		ret = a9_exp_alloc(ALIGN(ext->size, SZ_16K), aop->pool_size,
				   &aop->pool_used, &offset);
		if (ret)
			goto out;
		aop->ext = (u8 *)aop->pool + offset;
		aop->ext_dma = aop->pool_pa + offset;
	} else {
		aop->ext = dmam_alloc_coherent(aop->dev, PAGE_ALIGN(ext->size),
					       &aop->ext_dma, GFP_KERNEL);
	}
	if (!aop->ext) {
		ret = -ENOMEM;
		goto out;
	}
	if (aop->ext_dma + PAGE_ALIGN(ext->size) - 1 > A9_AOP_PA_MASK) {
		ret = -ERANGE;
		goto out;
	}
	ret = a9_aop_prepare_image(aop, fw, aop->ext_dma, &image);
	if (ret)
		goto out;
	if (force_cold) {
		ret = a9_aop_cold_patch(image, fw->size, cold_test);
		if (ret)
			goto out;
		dev_info(aop->dev, "COLD test=%u dcache_off=%u iop_irqs=%u ecid=%u nonce=%u reset_word=%08x\n",
			 cold_test, !!(cold_test & A9_COLD_UNCACHED),
			 !!(cold_test & A9_COLD_IOP_IRQS),
			 !!(cold_test & A9_COLD_ECID), !!(cold_test & A9_COLD_NONCE),
			 get_unaligned_le32(image + A9_COLD_SCTLR_OFFSET));
		aop->cold_before = vmalloc(aop->region_size);
		aop->cold_after = vmalloc(aop->region_size);
		if (!aop->cold_before || !aop->cold_after) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (handoff) {
		/* Fingerprint of the final patched image; a new instance also
		 * invalidates any handoff record left in the pool tail.
		 */
		sha256(image, A9_HO_SRAM_SPAN, aop->ho.sram_sha256);
		aop->ho.sram_valid = true;
		aop->ho.generation = 0;
		memset(aop->ho.slot, 0, sizeof(*aop->ho.slot));
		wmb(); /* Clear the record before the new instance can run. */
		dev_info(aop->dev, "HANDOFF cold-load sram_span=0x%x sram_sha=%*phN record=cleared\n",
			 A9_HO_SRAM_SPAN, A9_HO_DIGEST, aop->ho.sram_sha256);
	}
	/* The firmware wait can be long: re-check before any write. */
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_regs, &regs);
	if (!ret && ((!force_cold && (regs.values[1] & AOP_B88_BIT0)) ||
		     !(regs.values[3] & AOP_MBOX_EMPTY) ||
		     (force_cold && (!(regs.values[2] & AOP_MBOX_EMPTY) ||
				     (regs.values[0] & AOP_CPU_START) || regs.values[5] ||
				     (regs.values[3] & AOP_OUTBOX_ENABLE))) ||
		     ((regs.values[2] | regs.values[3]) & AOP_MBOX_FAULTS)))
		ret = -EBUSY;
	if (ret)
		goto out;

	/* b6f340 + b6f9b4: image (with patched tags) at region + 0, no BSS clear. */
	memcpy(aop->ext, ext->data, ext->size);
	wmb();	/* ext buffer contents before the AOP can be released */
	if (memcmp(aop->ext, ext->data, ext->size)) {
		ret = -EIO;
		goto out;
	}
	if (force_cold && cold_defer == 2) {
		ret = a9_aop_cold_ext_fence(aop->ext, ext->size);
		if (ret)
			goto out;
		wmb(); /* Publish the fence before readback and CPU release. */
		if (get_unaligned_le16((u8 *)aop->ext + A9_COLD_EXT_FENCE_OFFSET) !=
		    A9_COLD_EXT_FENCE_BRANCH) {
			ret = -EIO;
			goto out;
		}
		dev_info(aop->dev, "COLD fence linked=01072204 root=01052ab8+0c expected=21 branch=e7fe\n");
	}
	ret = a9_aop_copy_verify(aop->region, image, fw->size);
	if (ret)
		goto out;
	if (aop->spi0.ran) {
		ret = a9_aop_spi0_trace(aop, "after-copy");
		if (ret)
			goto out;
	}
	if (force_cold) {
		ret = a9_aop_cold_capture(aop, "loaded");
		if (ret)
			goto out;
	}

	/* Exact iOS b4e72c remap, then b4b738 mailbox/start order. */
	aop->experiment_step = "cold-load";
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		/* Polling needs no AP mailbox notifications. Keep both AP bits
		 * masked on both views throughout every deferred attempt.
		 */
		if (cold_defer && (writes[i].offset == AOP_IRQ_MASK_CLEAR ||
				   writes[i].offset == AOP_IRQ_MIRROR_CLEAR))
			continue;
		ret = aop_wr(aop, writes[i].offset, writes[i].value);
		if (ret)
			goto out;
	}
	if (force_cold && (cold_test & A9_COLD_IOP_IRQS)) {
		/* The cold2 v3 AP view retained 0x110. Test both documented
		 * IOP notification bits before RUN, without changing FIFO offsets.
		 */
		ret = aop_wr(aop, AOP_IRQ_MASK_CLEAR, AOP_IRQ_IOP_BOTH);
		if (!ret)
			ret = aop_wr(aop, AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_IOP_BOTH);
		if (ret)
			goto out;
	}
	if (!aop->experiment_stage) {
		aop->irq_enabled = true;
		enable_irq(aop->irq);
	}
	control = aop_rd(aop, AOP_OUTBOX_CONTROL);
	ret = aop_wr(aop, AOP_OUTBOX_CONTROL, control | AOP_OUTBOX_ENABLE);
	if (ret)
		goto out;
	if (aop->spi0.ran) {
		ret = a9_aop_spi0_recheck(aop);
		if (!ret)
			ret = a9_aop_spi0_trace(aop, "after-window");
		if (ret)
			goto out;
	}
	if (aop->spi0.intc.disabled) {
		ret = a9_aop_intc_ack(aop);
		if (!ret)
			ret = a9_aop_intc_reset(aop);
		if (ret)
			goto out;
	}

	mutex_lock(&aop->lock);
	aop->phase = PHASE_WAIT_HELLO;
	mutex_unlock(&aop->lock);
	if (cold_defer) {
		ret = a9_aop_cold_checkpoint(aop, "before-run");
		if (ret)
			goto out;
	}
	control = aop_rd(aop, AOP_CPU_CONTROL);
	ret = aop_wr(aop, AOP_CPU_CONTROL, control | AOP_CPU_START);
	if (ret)
		goto out;
	if (cold_defer) {
		ret = a9_aop_cold_checkpoint(aop, "run-written-before-autoboot");
		if (ret)
			goto out;
	}
	if (force_cold)
		ret = a9_aop_control_write(aop, aop->autoboot, 0x600, 1);
	else
		writel(1, aop->autoboot);
	if (ret)
		goto out;
	if (force_cold) {
		aop->cold_release_ns = ktime_get_boottime_ns();
		ret = a9_aop_cold_capture(aop, "released");
		if (ret)
			goto out;
	}
	if (cold_defer)
		a9_aop_cold_checkpoint(aop, "autoboot-written");
	dev_info(aop->dev, "AOP released: image %zu B (RTSZ 0x%x) at %pa, ext %zu B at %pad\n",
		 fw->size, aop->runtime_size, &aop->region_pa, ext->size,
		 &aop->ext_dma);
out:
	vfree(image);
	release_firmware(ext);
	release_firmware(fw);
	return ret;
}

/* ---- running attach: no loader, mask, reset or power writes --------------- */

static bool a9_aop_pinned_firmware(const struct firmware *fw, bool external)
{
	static const u8 main_sha[SHA256_DIGEST_SIZE] = {
		0xaf, 0x5d, 0x51, 0xa3, 0x68, 0xce, 0xb8, 0x9d,
		0xc5, 0xc3, 0x59, 0xe7, 0xc8, 0x4e, 0x54, 0xcb,
		0x60, 0x82, 0xe3, 0x08, 0x95, 0x6b, 0xe7, 0x42,
		0xc1, 0x76, 0x22, 0xeb, 0x4a, 0x60, 0x47, 0xc8,
	};
	static const u8 ext_sha[SHA256_DIGEST_SIZE] = {
		0x25, 0x28, 0xd2, 0x68, 0x6e, 0xd1, 0x26, 0x2d,
		0xe9, 0x91, 0x0c, 0x2d, 0x60, 0xe4, 0x5b, 0x68,
		0x13, 0x7e, 0x50, 0xaa, 0x60, 0x1a, 0x0f, 0x8d,
		0x54, 0xea, 0x7d, 0x13, 0x47, 0xb9, 0xde, 0x22,
	};
	u8 digest[SHA256_DIGEST_SIZE];

	if (fw->size != (external ? A9_ATTACH_EXT_SIZE : A9_ATTACH_MAIN_SIZE))
		return false;
	sha256(fw->data, fw->size, digest);
	return !memcmp(digest, external ? ext_sha : main_sha, sizeof(digest));
}

/*
 * Accept only a boot-time carveout Linux cannot allocate: the AOP region or
 * a plain, non-reusable /reserved-memory entry. Reject pools and entries
 * with a driver/ops (which could hand the pages to another device later).
 * Mapped RAM additionally needs PageReserved on EVERY covering page.
 * Never attempt a late memblock reservation: the allocator is already live.
 */
static bool a9_aop_ext_reserved(struct a9_aop *aop, u64 pa, bool *io)
{
	struct device_node *root, *node;
	struct reserved_mem *rmem;
	u64 size = ALIGN(A9_ATTACH_EXT_SIZE, A9_ATTACH_PAGE_SIZE);
	unsigned long pfn, end;
	bool found = false;

	*io = true;
	if (a9_attach_contains(aop->region_pa, aop->region_size, pa, size))
		return true;
	root = of_find_node_by_path("/reserved-memory");
	if (!root)
		return false;
	for_each_available_child_of_node(root, node) {
		rmem = of_reserved_mem_lookup(node);
		if (!rmem || rmem->ops || of_property_present(node, "compatible") ||
		    of_property_present(node, "reusable") ||
		    !a9_attach_contains(rmem->base, rmem->size, pa, size))
			continue;
		*io = of_property_present(node, "no-map");
		found = true;
		if (!*io) {
			end = PHYS_PFN(PAGE_ALIGN(pa + size));
			for (pfn = PHYS_PFN(pa); pfn < end; pfn++) {
				if (!pfn_valid(pfn) || !PageReserved(pfn_to_page(pfn))) {
					found = false;
					break;
				}
			}
		}
		of_node_put(node);
		break;
	}
	of_node_put(root);
	return found;
}

/* Caller holds lock; Phase A does not consume FIFO words or send anything. */
static enum a9_attach_result a9_aop_check_ext(struct a9_aop *aop,
					      const struct firmware *ext, u64 pa)
{
	void __iomem *mapping;
	void *ram;
	u8 chunk[256];
	size_t done, n;
	bool io;
	int mismatch = 0;

	aop->ext_dma = pa;
	aop->ext_size = ext->size;
	aop->ext_reserved = a9_aop_ext_reserved(aop, pa, &io);
	if (!aop->ext_reserved)
		return A9_ATTACH_UNRESERVED_EXT; /* never map an arbitrary tag PA */
	if (!io) {
		/* Preserve the linear map's attributes: no Device alias of RAM. */
		ram = memremap(pa, ext->size, MEMREMAP_WB);
		if (!ram)
			return A9_ATTACH_STALE_EXT;
		mismatch = !a9_attach_ext_matches(ram, ext->size, ext->data, ext->size);
		memunmap(ram);
	} else {
		if (a9_attach_contains(aop->region_pa, aop->region_size, pa, ext->size))
			mapping = aop->region + (pa - aop->region_pa);
		else
			mapping = ioremap(pa, ext->size);
		if (!mapping)
			return A9_ATTACH_STALE_EXT;
		for (done = 0; done < ext->size; done += n) {
			n = min(ext->size - done, sizeof(chunk));
			if (a9_aop_fromio32(mapping, ext->size, chunk, done, n) ||
			    memcmp(chunk, ext->data + done, n)) {
				mismatch = 1;
				break;
			}
		}
		if (!a9_attach_contains(aop->region_pa, aop->region_size, pa, ext->size))
			iounmap(mapping);
	}
	aop->ext_verified = !mismatch;
	return mismatch ? A9_ATTACH_STALE_EXT : A9_ATTACH_OK;
}

static bool a9_aop_attach_preflight(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_regs regs = { .aop = aop, .fingerprint = true };
	const struct firmware *fw = NULL, *ext = NULL;
	enum a9_attach_result result = A9_ATTACH_FOREIGN_IMAGE;
	u64 pa;
	int ret;

	/* Firmware files may appear only after switch_root; all waits bounded. */
	ret = a9_aop_wait_firmware(aop, AOP_MAIN_FW, &fw);
	if (!ret && !a9_aop_pinned_firmware(fw, false))
		ret = -EILSEQ;
	mutex_lock(&aop->lock);
	aop->diagnostics.reference_errno = ret;
	if (!ret && apple_a9_aop_debug()) {
		struct a9_attach_fingerprint *expected = &aop->diagnostics.expected;

		memcpy(aop->diagnostics.expected_prefix, fw->data, A9_DIAG_PREFIX_SIZE);
		memcpy(expected->head, fw->data, sizeof(expected->head));
		memcpy(expected->build, fw->data + A9_ATTACH_BUILD_OFFSET,
		       sizeof(expected->build));
		memcpy(expected->table, fw->data + A9_ATTACH_TABLE_OFFSET,
		       sizeof(expected->table));
		aop->diagnostics.reference_pinned = true;
	}
	if (ret || a9_aop_dead(aop))
		goto out;
	/* Recheck domains and wrapper after the filesystem wait, before region IO. */
	if (aop->region_size < A9_ATTACH_BUILD_OFFSET + A9_ATTACH_BUILD_SIZE) {
		ret = -EINVAL;
		goto out;
	}
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_regs, &regs);
	if (ret) {
		a9_aop_refuse(aop, "refused-domain-state");
		goto out;
	}
	memcpy(aop->snapshot, regs.values, sizeof(aop->snapshot));
	if (regs.read_errno)
		goto out;
	if ((regs.values[2] | regs.values[3]) & AOP_MBOX_FAULTS) {
		a9_aop_refuse(aop, "refused-mailbox-fault");
		goto out;
	}
	if (!(regs.values[1] & AOP_B88_BIT0) ||
	    !(regs.values[3] & AOP_MBOX_EMPTY)) {
		a9_aop_refuse(aop, "refused-running-unexpected-mgmt");
		goto out;
	}
	aop->table = devm_kmemdup(aop->dev, aop->fingerprint.table,
				  A9_ATTACH_TABLE_SIZE, GFP_KERNEL);
	aop->table_size = aop->table ? A9_ATTACH_TABLE_SIZE : 0;
	/* A matching string in an unsupported address translation is insufficient. */
	if (!aop->diagnostics.mapping_matches) {
		dev_warn(aop->dev, "unsupported runtime remap; no verified alternate layout, retaining foreign-image refusal\n");
		goto out;
	}
	result = a9_attach_fingerprint(&aop->fingerprint, fw->data, fw->size,
				       aop->wrapper_pa, aop->aop_target,
				       aop->chip_id, aop->chip_revision, &pa);
	aop->image_verified = result != A9_ATTACH_FOREIGN_IMAGE;
	if (result != A9_ATTACH_OK)
		goto out;
	aop->tags_verified = true;
	result = A9_ATTACH_STALE_EXT;
	ret = request_firmware_direct(&ext, AOP_EXT_FW, aop->dev);
	if (ret || !a9_aop_pinned_firmware(ext, true))
		goto out;
	result = a9_aop_check_ext(aop, ext, pa);
out:
	if (result != A9_ATTACH_OK) {
		aop->takeover.result = result;
		a9_aop_refuse(aop, a9_attach_result_name(result));
	}
	mutex_unlock(&aop->lock);
	release_firmware(ext);
	release_firmware(fw);
	return result == A9_ATTACH_OK && !a9_aop_dead(aop);
}

/* IRQ and background poll stay off throughout B/C: exactly one reader. */
static void a9_aop_attach_poll(struct a9_aop *aop, unsigned int ms,
			       enum a9_attach_stage stage)
{
	u64 deadline = ktime_get_boottime_ns() + (u64)ms * NSEC_PER_MSEC;

	while (!a9_aop_dead(aop) && ktime_get_boottime_ns() < deadline) {
		a9_aop_inline_rx(aop);
		if (aop->takeover.stage != stage ||
		    (stage == A9_ATTACH_PING && !aop->takeover.pending))
			return;
		usleep_range(500, 1000);
	}
}

static bool a9_aop_running_attach(struct a9_aop *aop)
{
	int ret;

	if (!a9_aop_attach_preflight(aop))
		return false;
	mutex_lock(&aop->lock);
	aop->phase = PHASE_ATTACH_PING;
	aop->takeover.stage = A9_ATTACH_PING;
	while (a9_attach_next_ping(&aop->takeover)) {
		u64 now = ktime_get_boottime_ns();

		aop->ping_seq = aop->takeover.seq;
		aop->ping_sent_ns = now;
		ret = a9_aop_send(aop, 0, a9_mgmt_ping(aop->ping_seq,
						       (u16)div_u64(now, NSEC_PER_SEC)));
		if (ret) {
			a9_attach_refuse(&aop->takeover, A9_ATTACH_NO_PING_ACK);
			break;
		}
		mutex_unlock(&aop->lock);
		a9_aop_attach_poll(aop, A9_ATTACH_PING_MS, A9_ATTACH_PING);
		mutex_lock(&aop->lock);
		if (a9_aop_dead(aop))
			goto out;
		a9_attach_ping_timeout(&aop->takeover);
	}
	if (aop->takeover.stage == A9_ATTACH_PING)
		a9_attach_refuse(&aop->takeover, A9_ATTACH_NO_PING_ACK);
	if (!a9_attach_hello(&aop->takeover, apple_a9_aop_attach()))
		goto refused;
	aop->phase = PHASE_ATTACH_HELLO;
	ret = a9_aop_send(aop, 0, A9_ATTACH_HELLO_REPLY);
	if (ret) {
		a9_attach_refuse(&aop->takeover, A9_ATTACH_HANDSHAKE_OWNED);
		goto refused;
	}
	mutex_unlock(&aop->lock);
	a9_aop_attach_poll(aop, A9_ATTACH_HELLO_MS, A9_ATTACH_WAIT_EPMAP);
	mutex_lock(&aop->lock);
	if (a9_aop_dead(aop))
		goto out;
	a9_attach_hello_timeout(&aop->takeover);
	if (aop->takeover.stage == A9_ATTACH_ADOPTED)
		goto out;
refused:
	if (aop->takeover.result == A9_ATTACH_HANDSHAKE_OWNED)
		dev_warn(aop->dev, "ZOMBIE AOP: two ping acks, no EPMAP; only a real power cycle clears inherited state, never kexec\n");
	a9_aop_refuse(aop, a9_attach_result_name(aop->takeover.result));
out:
	mutex_unlock(&aop->lock);
	return aop->takeover.stage == A9_ATTACH_ADOPTED && !a9_aop_dead(aop);
}

/* ---- bring-up ------------------------------------------------------------ */

static void a9_aop_experiment_unmask(struct a9_aop *aop)
{
	static const struct { u32 offset, value; } writes[] = {
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT0 },
		{ AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT0 },
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT12 },
		{ AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT12 },
	};
	unsigned int i;

	/* iOS enable helpers e548/e5a8; only mailbox masks, never CPU/PMGR. */
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		a9_aop_experiment_regs(aop, "unmask-before");
		if (aop->fault_control)
			return;
		dev_info(aop->dev, "REG-W step=mailbox-unmask offset=%04x value=%08x\n",
			 writes[i].offset, writes[i].value);
		writel(writes[i].value, aop->wrapper + writes[i].offset);
		a9_aop_experiment_regs(aop, "unmask-after");
		if (aop->fault_control)
			return;
	}
	aop->mailbox_unmasked = true;
}

/* IOP mask hypothesis; CPU release matches iOS b4e7c8, never a reset. */
static void a9_aop_experiment_wake(struct a9_aop *aop)
{
	unsigned int i, masks = (wake <= A9_EXP_WAKE_INBOX ||
				 wake == A9_EXP_WAKE_COMBINED) ? 2 : 0;
	unsigned int count = masks + (wake < A9_EXP_WAKE_RUN ? 0 :
				      wake == A9_EXP_WAKE_RUN ? 1 : 2);
	u32 offset, value;
	void __iomem *target;

	for (i = 0; i < count; i++) {
		a9_aop_experiment_regs(aop, "wake-before");
		if (aop->fault_control || a9_aop_dead(aop))
			return;
		if (i < masks) {
			offset = i ? AOP_IRQ_MIRROR_CLEAR : AOP_IRQ_MASK_CLEAR;
			value = wake == A9_EXP_WAKE_INBOX ?
				AOP_IRQ_IOP_INBOX : AOP_IRQ_IOP_BOTH;
			target = aop->wrapper + offset;
		} else if (i == masks) {
			offset = AOP_CPU_CONTROL;
			value = aop_rd(aop, offset) | AOP_CPU_START;
			target = aop->wrapper + offset;
		} else {
			/* Mapped only by the explicit wake=4/5 selectors. */
			offset = 0x600;
			value = 1;
			target = aop->autoboot;
		}
		dev_info(aop->dev, "REG-W wake=%u step=%s bank=%s offset=%04x before=%08x value=%08x\n",
			 wake, aop->experiment_step, target == aop->autoboot ?
			 "aon-auto-boot" : "wrapper", offset, readl(target), value);
		writel(value, target);
		/* Ordered readback before the next store; no polling or retry. */
		dev_info(aop->dev, "REG-R wake=%u offset=%04x after=%08x\n",
			 wake, offset, readl(target));
		a9_aop_experiment_regs(aop, "wake-after");
		if (aop->fault_control)
			return;
	}
}

static void a9_aop_experiment_window(struct a9_aop *aop, unsigned int ms)
{
	static const unsigned int times[] = { 1, 10, 100, 1000, 2000, 5000 };
	static const char * const labels[] = {
		"wait1ms", "wait10ms", "wait100ms", "wait1000ms",
		"wait2000ms", "wait5000ms",
	};
	unsigned int i, previous = 0, target;

	for (i = 0; i < ARRAY_SIZE(times) && previous < ms; i++) {
		target = min(times[i], ms);
		a9_aop_wait(aop, READ_ONCE(aop->epmap_done), target - previous);
		a9_aop_experiment_regs(aop, target == times[i] ? labels[i] : "wait-end");
		if (a9_aop_dead(aop) || aop->epmap_done)
			break;
		previous = target;
	}
}

/* Deliberately unverified attachment, authorized by the owner's crash-risk opt-in. */
static bool a9_aop_experiment_handshake(struct a9_aop *aop)
{
	unsigned int i;
	const struct a9_exp_step *sequence = wake ? a9_exp_wake_steps : a9_exp_steps;
	unsigned int steps = wake ? ARRAY_SIZE(a9_exp_wake_steps) :
		ARRAY_SIZE(a9_exp_steps) - (cycle ? 0 : A9_EXP_CYCLE_STEPS);
	int ret = 0;

	if (atomic_cmpxchg(&experiment_attempted, 0, 1)) {
		mutex_lock(&aop->lock);
		a9_aop_refuse(aop, "refused-second-experiment-attempt");
		mutex_unlock(&aop->lock);
		return false;
	}
	dev_warn(aop->dev, "EXPERIMENT stage=%u wake=%u attempt=1 identity=unverified old_dma=unknown; bounded polling, no reset or firmware load\n",
		 aop->experiment_stage, wake);
	aop->inline_rx = true;
	aop->phase = PHASE_WAIT_HELLO;
	for (i = 0; i < steps; i++) {
		const struct a9_exp_step *step = &sequence[i];

		aop->experiment_step = step->name;
		a9_aop_experiment_regs(aop, "step-before");
		a9_aop_inline_rx(aop);
		if (a9_aop_dead(aop))
			goto out;
		if (aop->epmap_done) {
			aop->experiment_winner = step->name;
			break;
		}
		dev_info(aop->dev, "STEP %u/%u %s action=%u word=%016llx window_ms=%u\n",
			 i + 1, steps, step->name,
			 step->action, step->word, step->ms);
		if (step->action == A9_EXP_UNMASK)
			a9_aop_experiment_unmask(aop);
		if (step->action == A9_EXP_WAKE)
			a9_aop_experiment_wake(aop);
		if (step->action == A9_EXP_SEND) {
			mutex_lock(&aop->lock);
			if (a9_mgmt_type(step->word) == A9_MGMT_HELLO_REPLY) {
				aop->speculative_hello = true;
				aop->speculative_replies++;
			}
			if (a9_mgmt_type(step->word) == A9_MGMT_PING) {
				aop->ping_seq = (u32)step->word;
				aop->ping_sent_ns = ktime_get_boottime_ns();
			}
			ret = a9_aop_send(aop, 0, step->word);
			if (ret) {
				a9_aop_fail(aop, "failed-ladder-send", ret);
			} else if (a9_mgmt_type(step->word) == A9_MGMT_HELLO_REPLY) {
				aop->hello = true;
				aop->phase = PHASE_WAIT_EPMAP;
			}
			mutex_unlock(&aop->lock);
		}
		if (!a9_aop_dead(aop))
			a9_aop_experiment_window(aop, step->ms);
		a9_aop_experiment_regs(aop, "step-after");
		if (a9_aop_dead(aop))
			goto out;
		if (aop->epmap_done) {
			aop->experiment_winner = step->name;
			break;
		}
		if (aop->epmap_chunks) {
			mutex_lock(&aop->lock);
			a9_aop_fail(aop, "failed-partial-epmap", -ETIMEDOUT);
			mutex_unlock(&aop->lock);
			goto out;
		}
	}
	if (!aop->epmap_done) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-ladder-no-epmap", -ETIMEDOUT);
		mutex_unlock(&aop->lock);
		goto out;
	}
	/* Record the complete bounded reply window, including power notifications. */
	a9_aop_wait(aop, false, AOP_IOP_READY_MS);
	mutex_lock(&aop->lock);
	if (!a9_aop_dead(aop)) {
		aop->handshake_complete = true;
		if (aop->experiment_stage == 2)
			a9_aop_finish(aop, PHASE_STOPPED, "stage2-handshake-complete", 0);
		else
			aop->phase = PHASE_WAIT_SERVICE;
	}
	mutex_unlock(&aop->lock);
out:
	a9_aop_experiment_regs(aop, "handshake-end");
	if (aop->handshake_complete && aop->experiment_stage == 3)
		aop->experiment_step = "stage3";
	aop->inline_rx = false;
	return aop->handshake_complete && aop->experiment_stage == 3 &&
	       !a9_aop_dead(aop);
}

static int a9_aop_system_start(struct a9_aop *aop)
{
	unsigned int ep;
	int ret = 0;

	mutex_lock(&aop->lock);
	aop->experiment_step = "cold-system-start";
	for (ep = 1; ep < 32 && !ret; ep++) {
		if (!test_bit(ep, aop->present))
			continue;
		if (!a9_sys_supported(ep)) {
			dev_warn(aop->dev, "unknown advertised system endpoint %u\n", ep);
			continue;
		}
		ret = a9_aop_send(aop, 0, a9_mgmt_startep(ep));
		if (!ret)
			aop->system[ep].started = true;
	}
	/* iOS readiness, IOP on request and AP on notification are distinct. */
	if (!ret)
		ret = a9_aop_send(aop, 0, a9_mgmt_ap_power(true));
	if (!ret)
		ret = a9_aop_send(aop, 0, 0x0060000000000220ULL);
	if (!ret)
		ret = a9_aop_send(aop, 0, 0x00b0000000000020ULL);
	a9_aop_stage3_mark(aop, "system-endpoints-started");
	aop->experiment_step = "cold-system-power-acks";
	mutex_unlock(&aop->lock);
	if (ret)
		return ret;
	if (!a9_aop_wait(aop, READ_ONCE(aop->iop_ready) &&
			 READ_ONCE(aop->ap_power_acked) &&
			 (READ_ONCE(aop->ap_power_ack) & 0xff) == 0x20 &&
			 a9_aop_system_buffers_ready(aop), 20000))
		return -ETIMEDOUT;
	mutex_lock(&aop->lock);
	if (a9_aop_dead(aop)) {
		mutex_unlock(&aop->lock);
		return -EIO;
	}
	aop->system_ready = true;
	if (aop->cold_run == 1) {
		aop->phase = PHASE_SYSTEM_READY;
		aop->result = "cold-system-ready";
	}
	dev_info(aop->dev, "SYSTEM ready IOP=%x AP=%x run=%u pool_used=%zx; polling and ping remain active\n",
		 aop->iop_power, aop->ap_power_ack, aop->cold_run, aop->pool_used);
	a9_aop_stage3_mark(aop, "system-ready");
	mutex_unlock(&aop->lock);
	return 0;
}

/* ---- sleep/wake handoff (aop_handoff=sleep) ----------------------------- */

struct a9_aop_handoff_live {
	struct a9_aop *aop;
	u8 *sram;
	u32 cpu, auto_boot, inbox, outbox;
	int error;
};

/* PMGR-guarded: status registers and the resident image span, no data pops. */
static void a9_aop_handoff_read(void *data)
{
	struct a9_aop_handoff_live *live = data;
	struct a9_aop *aop = live->aop;

	live->cpu = aop_rd(aop, AOP_CPU_CONTROL);
	live->auto_boot = readl(aop->autoboot);
	live->inbox = aop_rd(aop, AOP_INBOX_CONTROL);
	live->outbox = aop_rd(aop, AOP_OUTBOX_CONTROL);
	if ((live->inbox | live->outbox) & AOP_MBOX_FAULTS) {
		live->error = -EIO;
		return;
	}
	live->error = a9_aop_fromio32(aop->region, aop->region_size, live->sram,
				      0, A9_HO_SRAM_SPAN);
}

static void a9_aop_handoff_digest(const struct a9_ho_record *r, u8 *digest)
{
	sha256((const u8 *)r, A9_HO_DIGESTED, digest);
}

static struct a9_ho_expect a9_aop_handoff_expect(struct a9_aop *aop)
{
	return (struct a9_ho_expect) {
		.region_pa = aop->region_pa, .region_size = aop->region_size,
		.pool_pa = aop->pool_pa, .pool_size = aop->pool_size,
	};
}

/* Read-only: no AOP traffic and no RAM writes. */
static enum a9_ho_reason a9_aop_handoff_validate(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_handoff_live live = { .aop = aop };
	const struct a9_ho_expect expect = a9_aop_handoff_expect(aop);
	struct a9_ho_record *r = &aop->ho.rec;
	u8 digest[A9_HO_DIGEST], pool[A9_HO_DIGEST], sram[A9_HO_DIGEST] = {};
	enum a9_ho_reason reason;
	int ret;

	memcpy(r, aop->ho.slot, sizeof(*r));
	a9_aop_handoff_digest(r, digest);
	reason = a9_ho_check(r, digest, &expect);
	dev_info(aop->dev, "HANDOFF record state=%u version=%u generation=%u errno=%u entry=%.63s writer=%*phN pool_used=0x%x nafk=%u check=%s\n",
		 r->state, r->version, r->generation, r->quiesce_errno, r->entry,
		 (int)sizeof(r->writer), r->writer, r->pool_used, r->nafk,
		 a9_ho_reason_names[reason]);
	if (reason)
		return reason;
	sha256(aop->pool, r->pool_used, pool);
	live.sram = vmalloc(A9_HO_SRAM_SPAN);
	if (!live.sram)
		return A9_HO_UNAVAILABLE;
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_handoff_read, &live);
	ret = ret ?: live.error;
	if (!ret)
		sha256(live.sram, A9_HO_SRAM_SPAN, sram);
	vfree(live.sram);
	dev_info(aop->dev, "HANDOFF live errno=%d cpu44=%08x auto_boot=%08x in4008=%08x out4020=%08x pool_sha=%*phN sram_sha=%*phN\n",
		 ret, live.cpu, live.auto_boot, live.inbox, live.outbox,
		 A9_HO_DIGEST, pool, A9_HO_DIGEST, sram);
	if (ret)
		return ret == -EIO ? A9_HO_WRAPPER : A9_HO_UNAVAILABLE;
	return a9_ho_check_live(r, pool, sram, live.auto_boot, live.inbox,
				live.outbox);
}

/* Everything the wake needs must exist before the record is consumed. */
static int a9_aop_handoff_inputs(struct a9_aop *aop)
{
	const struct firmware *fw;
	unsigned int i;
	int ret;

	for (i = 0; i < AOP_FW_WAIT_S && !a9_aop_ramoops_ready(); i++)
		if (wait_event_timeout(aop->wait, READ_ONCE(aop->stopping), HZ))
			return -ESHUTDOWN;
	if (!a9_aop_ramoops_ready())
		return -ENODEV;
	if (aop->cold_run < 2)
		return 0;
	ret = a9_aop_wait_firmware(aop, AOP_CAL_FW, &fw);
	if (!ret)
		release_firmware(fw);
	return ret;
}

/* Consumed once: a failed wake can never be retried from the same record. */
static void a9_aop_handoff_consume(struct a9_aop *aop)
{
	struct a9_ho_record *r = &aop->ho.out;

	memcpy(r, &aop->ho.rec, sizeof(*r));
	r->state = A9_HO_CONSUMED;
	memcpy(r->consumer, aop->ho.writer, sizeof(r->consumer));
	a9_aop_handoff_digest(r, r->digest);
	memcpy(aop->ho.slot, r, sizeof(*r));
	wmb(); /* Consumed before the wake word can reach the firmware. */
}

/* Resumed in place: only the AP state changes, like Asahi's RTKit wake. */
static int a9_aop_handoff_resume_power(struct a9_aop *aop)
{
	int ret;

	mutex_lock(&aop->lock);
	aop->experiment_step = "handoff-ap-on";
	aop->ap_power_acked = false;
	ret = a9_aop_send(aop, 0, A9_HO_AP_ON);
	mutex_unlock(&aop->lock);
	dev_info(aop->dev, "HANDOFF ap-on word=%016llx errno=%d\n", A9_HO_AP_ON, ret);
	if (ret)
		return ret;
	if (!a9_aop_wait(aop, READ_ONCE(aop->iop_ready) &&
			 READ_ONCE(aop->ap_power_acked) &&
			 (READ_ONCE(aop->ap_power_ack) & 0xff) == 0x20, 20000))
		return -ETIMEDOUT;
	mutex_lock(&aop->lock);
	if (a9_aop_dead(aop)) {
		mutex_unlock(&aop->lock);
		return -EIO;
	}
	aop->system_ready = true;
	if (aop->cold_run == 1) {
		aop->phase = PHASE_SYSTEM_READY;
		aop->result = "handoff-system-ready";
	}
	dev_info(aop->dev, "SYSTEM ready IOP=%x AP=%x run=%u pool_used=%zx handoff=resume\n",
		 aop->iop_power, aop->ap_power_ack, aop->cold_run, aop->pool_used);
	a9_aop_stage3_mark(aop, "system-ready");
	mutex_unlock(&aop->lock);
	return 0;
}

static void a9_aop_handoff_fail(struct a9_aop *aop, const char *result, int error)
{
	mutex_lock(&aop->lock);
	aop->ho.waking = false;
	aop->ho.result = "failed";
	a9_aop_fail(aop, result, error);
	mutex_unlock(&aop->lock);
}

/*
 * aop_wake_wrapper=1, before the one 0x220: AppleA7IOP status-3 resume steps
 * 3-7 (b4b854..b4b8b4) in iOS order. No +0x44 RUN, no auto-boot and no NMI
 * (+0xc004/+0xc024): the ordinary resume path uses none of them. Step 2,
 * setActive(true) (b4b6f4), has no register-level definition and is not
 * replayed. Status reads only; FIFO data registers are never touched.
 */
static int a9_aop_wrapper_wake(struct a9_aop *aop)
{
	static const struct { u32 offset, value; } writes[] = {
		{ AOP_IRQ_MASK_SET, AOP_IRQ_ALL }, { AOP_IRQ_MIRROR_SET, AOP_IRQ_ALL },
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT0 }, { AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT0 },
		{ AOP_IRQ_MASK_CLEAR, AOP_IRQ_BIT12 }, { AOP_IRQ_MIRROR_CLEAR, AOP_IRQ_BIT12 },
	};
	u32 in = aop_rd(aop, AOP_INBOX_CONTROL), out = aop_rd(aop, AOP_OUTBOX_CONTROL);
	u32 mask = aop_rd(aop, AOP_IRQ_MASK_SET), mirror = aop_rd(aop, AOP_IRQ_MIRROR_SET);
	unsigned int i, n = 0;
	int ret = 0;

	aop->experiment_step = "handoff-wrapper-wake";
	for (i = 0; !ret && i < ARRAY_SIZE(writes); i++) {
		ret = aop_wr(aop, writes[i].offset, writes[i].value);
		n += !ret;
	}
	if (!ret) {
		ret = aop_wr(aop, AOP_OUTBOX_CONTROL,
			     aop_rd(aop, AOP_OUTBOX_CONTROL) | AOP_OUTBOX_ENABLE);
		n += !ret;
	}
	aop->wake_wrapper_done = !ret;
	aop->wake_wrapper_errno = ret;
	dev_info(aop->dev, "HANDOFF wrapper-wake errno=%d writes=%u in4008=%08x->%08x out4020=%08x->%08x set4000=%08x->%08x setc00=%08x->%08x cpu44=%08x run=untouched auto_boot=untouched nmi=untouched setactive=not-replayed\n",
		 ret, n, in, aop_rd(aop, AOP_INBOX_CONTROL), out,
		 aop_rd(aop, AOP_OUTBOX_CONTROL), mask,
		 aop_rd(aop, AOP_IRQ_MASK_SET), mirror,
		 aop_rd(aop, AOP_IRQ_MIRROR_SET), aop_rd(aop, AOP_CPU_CONTROL));
	return ret;
}

/*
 * Wake the certified sleeping firmware without halt or reload. One wake
 * word, bounded waits, no retry and no fallback load in this boot.
 */
static bool a9_aop_handoff_wake(struct a9_aop *aop)
{
	const struct a9_ho_record *r = &aop->ho.rec;
	u64 deadline;
	int ret;

	aop->ho.reason = a9_aop_handoff_validate(aop);
	if (!aop->ho.reason) {
		ret = a9_aop_handoff_inputs(aop);
		if (ret)
			aop->ho.reason = A9_HO_UNAVAILABLE;
	}
	if (aop->ho.reason) {
		dev_err(aop->dev, "HANDOFF refused reason=%s: no wake, reload or fallback this boot; recover with DFU\n",
			a9_ho_reason_names[aop->ho.reason]);
		mutex_lock(&aop->lock);
		aop->ho.result = "refused";
		a9_aop_refuse(aop, "refused-handoff-record");
		mutex_unlock(&aop->lock);
		return false;
	}
	a9_aop_handoff_consume(aop);
	/* Adopt the certified layout before any reply can request memory. */
	mutex_lock(&aop->lock);
	aop->ho.adopt = true;
	aop->ho.result = "waking";
	aop->ho.generation = r->generation;
	memcpy(aop->ho.sram_sha256, r->sram_sha256, A9_HO_DIGEST);
	aop->ho.sram_valid = true;
	aop->pool_used = r->pool_used;
	aop->ext = (u8 *)aop->pool + r->ext_offset;
	aop->ext_dma = aop->pool_pa + r->ext_offset;
	aop->ext_size = r->ext_size;
	aop->phase = PHASE_WAIT_HELLO;
	mutex_unlock(&aop->lock);
	dev_info(aop->dev, "HANDOFF consumed generation=%u consumer=%*phN pool_used=0x%x; no halt or reload\n",
		 r->generation, (int)sizeof(aop->ho.writer), aop->ho.writer,
		 r->pool_used);
	ret = a9_aop_cold_checkpoint(aop, "handoff-before-wake");
	if (ret) {
		a9_aop_handoff_fail(aop, "failed-handoff-checkpoint", ret);
		return false;
	}
	if (wake_wrapper) {
		ret = a9_aop_wrapper_wake(aop);
		if (ret) {
			a9_aop_handoff_fail(aop, "failed-handoff-wrapper-wake", ret);
			return false;
		}
	}
	aop->inline_rx = true;
	mutex_lock(&aop->lock);
	aop->experiment_step = "handoff-wake220";
	aop->ho.waking = true;
	ret = a9_aop_send(aop, 0, A9_HO_IOP_WAKE);
	mutex_unlock(&aop->lock);
	dev_info(aop->dev, "HANDOFF wake220 word=%016llx errno=%d\n", A9_HO_IOP_WAKE, ret);
	if (ret) {
		aop->inline_rx = false;
		a9_aop_handoff_fail(aop, "failed-handoff-wake-tx", ret);
		return false;
	}
	deadline = ktime_get_boottime_ns() + (u64)AOP_COLD_HELLO_MS * NSEC_PER_MSEC;
	while (!a9_aop_dead(aop) && READ_ONCE(aop->ho.waking) &&
	       ktime_get_boottime_ns() < deadline) {
		a9_aop_inline_rx(aop);
		usleep_range(500, 1000);
	}
	a9_aop_inline_rx(aop);
	aop->inline_rx = false;
	/* Inbox empty again = 0x220 consumed, on success and on a silent wake. */
	dev_info(aop->dev, "HANDOFF wake-window end path=%s in4008=%08x out4020=%08x wrapper=%d\n",
		 a9_ho_path_names[aop->ho.path], aop_rd(aop, AOP_INBOX_CONTROL),
		 aop_rd(aop, AOP_OUTBOX_CONTROL), aop->wake_wrapper_done);
	if (a9_aop_dead(aop))
		return false;
	if (aop->ho.path == A9_HO_PATH_NONE) {
		a9_aop_handoff_fail(aop, "failed-handoff-no-wake-reply", -ETIMEDOUT);
		return false;
	}
	queue_delayed_work(aop->wq, &aop->poll_work, 0);
	if (aop->ho.path == A9_HO_PATH_HELLO) {
		if (!a9_aop_wait(aop, READ_ONCE(aop->epmap_done), AOP_EPMAP_MS)) {
			a9_aop_handoff_fail(aop, "failed-no-epmap", -ETIMEDOUT);
			return false;
		}
		mutex_lock(&aop->lock);
		aop->handshake_complete = true;
		aop->experiment_winner = "handoff-hello";
		a9_aop_stage3_mark(aop, "handshake-complete");
		mutex_unlock(&aop->lock);
	} else {
		aop->experiment_winner = "handoff-resume";
	}
	aop->experiment_step = "handoff-stage3";
	queue_delayed_work(aop->wq, &aop->ping_work, 0);
	ret = aop->ho.path == A9_HO_PATH_HELLO ? a9_aop_system_start(aop) :
	      a9_aop_handoff_resume_power(aop);
	if (ret) {
		a9_aop_handoff_fail(aop, "failed-system-bringup", ret);
		return false;
	}
	mutex_lock(&aop->lock);
	aop->ho.result = "adopted";
	mutex_unlock(&aop->lock);
	return !a9_aop_dead(aop);
}

/* Shutdown only: certify (status 0) or explicitly fail the next adoption.
 * Returns the final status, after the same check the next kernel applies.
 */
static int a9_aop_handoff_write(struct a9_aop *aop, int status)
{
	const struct a9_ho_expect expect = a9_aop_handoff_expect(aop);
	struct a9_ho_record *r = &aop->ho.out;
	unsigned int i, j, n = 0;
	enum a9_ho_reason check = A9_HO_OK;

	memset(r, 0, sizeof(*r));
	r->magic = A9_HO_MAGIC;
	r->version = A9_HO_VERSION;
	r->size = sizeof(*r);
	r->generation = aop->ho.generation + 1;
	r->written_ns = ktime_get_boottime_ns();
	memcpy(r->writer, aop->ho.writer, sizeof(r->writer));
	a9_aop_entry_name(r->entry, sizeof(r->entry));
	r->region_pa = aop->region_pa;
	r->region_size = aop->region_size;
	r->sram_span = A9_HO_SRAM_SPAN;
	memcpy(r->sram_sha256, aop->ho.sram_sha256, A9_HO_DIGEST);
	r->pool_pa = aop->pool_pa;
	r->pool_size = aop->pool_size;
	r->pool_used = aop->pool_used;
	r->ext_offset = aop->ext_dma - aop->pool_pa;
	r->ext_size = aop->ext_size;
	r->cpu_control = aop->ho.idle_cpu;
	r->auto_boot = aop->ho.idle_auto_boot;
	r->inbox_control = aop->ho.idle_inbox;
	r->outbox_control = aop->ho.idle_outbox;
	r->ap_ack = aop->ap_power_ack;
	r->iop_ack = aop->iop_power;
	r->hello_min = aop->hello_min;
	r->hello_max = aop->hello_max;
	r->accel_ep = aop->accel >= 0 ? aop->services[aop->accel].ep : 0;
	r->cal_state = aop->cal_state;
	r->steps = aop->ho.steps;
	r->failed_step = aop->ho.failed_step;
	r->failed_errno = aop->ho.failed_errno;
	for (i = 0; i < 256; i++)
		if (test_bit(i, aop->present))
			r->present[i / 8] |= 1U << (i % 8);
	for (i = 1; i < A9_SYS_COUNT; i++) {
		const struct a9_aop_system *sys = &aop->system[i];

		if (!sys->buffer.va)
			continue;
		r->sys[i].offset = sys->buffer.dma - aop->pool_pa;
		r->sys[i].size = sys->buffer.size;
		r->sys[i].entries = sys->entries;
		r->sys[i].message_size = sys->message_size;
	}
	for (i = 0; i < aop->neps; i++) {
		for (j = 0; j < aop->eps[i].nbufs; j++) {
			const struct a9_aop_buffer *buf = &aop->eps[i].bufs[j];

			if (n == A9_HO_MAX_AFK) {
				status = status ?: -E2BIG;
				break;
			}
			r->afk[n].ep = aop->eps[i].num;
			r->afk[n].tag = buf->tag;
			r->afk[n].offset = buf->dma - aop->pool_pa;
			r->afk[n].size = buf->size;
			n++;
		}
	}
	r->nafk = n;
	memcpy(r->pool_sha256, aop->ho.pool_sha256, A9_HO_DIGEST);
	if (!status && !aop->ho.sram_valid)
		status = -ENODATA;
	r->state = status ? A9_HO_FAILED : A9_HO_VALID;
	r->quiesce_errno = -status;
	a9_aop_handoff_digest(r, r->digest);
	if (!status) {
		check = a9_ho_check(r, r->digest, &expect);
		if (check) {
			status = -EINVAL;
			r->state = A9_HO_FAILED;
			r->quiesce_errno = EINVAL;
			a9_aop_handoff_digest(r, r->digest);
		}
	}
	memcpy(aop->ho.slot, r, sizeof(*r));
	wmb(); /* Publish the complete record before kexec. */
	aop_tr_info(aop, "HANDOFF record state=%s status=%d check=%s generation=%u steps=0x%x entry=%s writer=%*phN pool_used=0x%x pool_sha=%*phN sram_sha=%*phN nafk=%u\n",
		    status ? "failed" : "valid", status, a9_ho_reason_names[check],
		    r->generation, r->steps, r->entry, (int)sizeof(r->writer),
		    r->writer, r->pool_used, A9_HO_DIGEST, r->pool_sha256,
		    A9_HO_DIGEST, r->sram_sha256, r->nafk);
	return status;
}

static void a9_aop_boot_work(struct work_struct *work)
{
	struct a9_aop *aop = container_of(work, struct a9_aop, boot_work);
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_regs regs = { .aop = aop };
	struct a9_aop_accel_config config = {};
	struct a9_spu_subpacket first = {};
	struct iio_dev *iio;
	unsigned int tries, i;
	bool running, empty;
	int ret;

	/* A handoff wake is queued at probe: wait (bounded) for ramoops and the
	 * calibration file first. By then PMGR has long probed, so the 10 s
	 * deferral bound below cannot refuse the single attempt.
	 */
	if (handoff && aop->ho.seen) {
		ret = a9_aop_handoff_inputs(aop);
		if (ret) {
			dev_err(aop->dev, "HANDOFF refused reason=unavailable inputs=%d: no wake, reload or fallback this boot\n",
				ret);
			mutex_lock(&aop->lock);
			aop->ho.reason = A9_HO_UNAVAILABLE;
			aop->ho.result = "refused";
			a9_aop_refuse(aop, "refused-handoff-inputs");
			mutex_unlock(&aop->lock);
			return;
		}
	}
	/* PMGR providers are not fw_devlink suppliers of this node. */
	for (tries = 0; tries < 100; tries++) {
		ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
					 a9_aop_read_regs, &regs);
		if (ret != -EPROBE_DEFER)
			break;
		msleep(100);
	}
	mutex_lock(&aop->lock);
	memcpy(aop->snapshot, regs.values, sizeof(aop->snapshot));
	if (ret) {
		/* Never force a domain on. */
		a9_aop_finish(aop, PHASE_REFUSED, "refused-domain-state", ret);
		mutex_unlock(&aop->lock);
		return;
	}
	if ((regs.values[2] | regs.values[3]) & AOP_MBOX_FAULTS) {
		a9_aop_refuse(aop, "refused-mailbox-fault");
		mutex_unlock(&aop->lock);
		return;
	}
	/* Legacy non-forced admission heuristic only; b88 is not CPU status. */
	running = regs.values[1] & AOP_B88_BIT0;
	empty = regs.values[3] & AOP_MBOX_EMPTY;
	if (force_cold) {
		mutex_unlock(&aop->lock);
		if (atomic_cmpxchg(&experiment_attempted, 0, 1)) {
			mutex_lock(&aop->lock);
			a9_aop_refuse(aop, "refused-second-experiment-attempt");
			mutex_unlock(&aop->lock);
			return;
		}
		if (handoff && aop->ho.seen) {
			/* A record exists: wake or refuse, never halt or reload. */
			if (!a9_aop_handoff_wake(aop) || aop->cold_run == 1)
				return;
			goto start_endpoints;
		}
		if (handoff && regs.values[5]) {
			/* Auto-boot set without a record: the uncertified live
			 * instance that crashed 4/4 force-cold reloads.
			 */
			mutex_lock(&aop->lock);
			a9_aop_refuse(aop, "refused-handoff-live-aop-without-record");
			mutex_unlock(&aop->lock);
			return;
		}
		dev_warn(aop->dev, "FORCE-COLD stage=%u attempt=1; owner accepts crash risk; stop before load, no reset hook\n",
			 aop->experiment_stage);
		if (cold_defer) {
			ret = a9_aop_cold_checkpoint(aop, "before-stop");
			if (ret) {
				mutex_lock(&aop->lock);
				a9_aop_fail(aop, "failed-cold-checkpoint", ret);
				mutex_unlock(&aop->lock);
				return;
			}
		}
		ret = a9_aop_force_stop(aop);
		if (ret) {
			mutex_lock(&aop->lock);
			a9_aop_fail(aop, "failed-force-halt", ret);
			mutex_unlock(&aop->lock);
			return;
		}
		ret = a9_aop_force_drain(aop);
		if (ret) {
			mutex_lock(&aop->lock);
			a9_aop_fail(aop, "failed-force-drain", ret);
			mutex_unlock(&aop->lock);
			return;
		}
		if (spi0_reset) {
			/* Boot snapshot before the stop: [5] auto-boot, [3] +0x4020. */
			ret = a9_aop_spi0_prereset(aop, regs.values[5], regs.values[3]);
			if (ret) {
				/* An INTC or channel-3 teardown refusal names its own check. */
				mutex_lock(&aop->lock);
				a9_aop_fail(aop, aop->spi0.intc.failed ?: aop->spi0.ch3.failed ?:
					    "failed-spi0-reset", ret);
				mutex_unlock(&aop->lock);
				return;
			}
		}
		aop->cold = true;
		goto load_cold;
	}
	if (aop->experiment_stage) {
		if (!running || !(regs.values[3] & AOP_OUTBOX_ENABLE)) {
			a9_aop_refuse(aop, "refused-experiment-needs-running-enabled-aop");
			mutex_unlock(&aop->lock);
			return;
		}
		mutex_unlock(&aop->lock);
		if (!a9_aop_experiment_handshake(aop))
			return;
		queue_delayed_work(aop->wq, &aop->poll_work, 0);
		queue_delayed_work(aop->wq, &aop->ping_work, 0);
		goto start_endpoints;
	}
	if (running && empty) {
		aop->running_attach = true;
		aop->phase = PHASE_ATTACH_PREFLIGHT;
	}
	if (!running && !empty) {
		a9_aop_refuse(aop, "refused-stopped-outbox-not-empty");
		mutex_unlock(&aop->lock);
		return;
	}
	aop->cold = !running;
	mutex_unlock(&aop->lock);

load_cold:
	if (aop->running_attach) {
		if (!a9_aop_running_attach(aop))
			return;
		/* First EPMAP already went through the ordinary management handler. */
		aop->irq_enabled = true;
		enable_irq(aop->irq);
	} else if (aop->cold) {
		ret = a9_aop_cold_start(aop);
		if (ret) {
			mutex_lock(&aop->lock);
			a9_aop_fail(aop, aop->spi0.intc.failed ?: aop->spi0.ch3.failed ?:
				    "failed-cold-load", ret);
			mutex_unlock(&aop->lock);
			return;
		}
	} else {
		/* Warm first attach: the pending transcript must start with HELLO. */
		mutex_lock(&aop->lock);
		aop->phase = PHASE_WAIT_HELLO;
		mutex_unlock(&aop->lock);
		aop->irq_enabled = true;
		enable_irq(aop->irq);
	}
	if (force_cold) {
		ret = a9_aop_cold_hello(aop);
		if (ret) {
			mutex_lock(&aop->lock);
			a9_aop_fail(aop, aop->cold_wake_errno ? "failed-cold-wake-tx" :
				    ret == -ETIMEDOUT ? "failed-no-hello" :
				    "failed-cold-observation-or-wake", ret);
			mutex_unlock(&aop->lock);
			return;
		}
	}
	queue_delayed_work(aop->wq, &aop->poll_work, 0);

	if (a9_aop_dead(aop))
		return;
	if (!a9_aop_wait(aop, READ_ONCE(aop->hello), AOP_HELLO_MS)) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-no-hello", -ETIMEDOUT);
		mutex_unlock(&aop->lock);
		return;
	}
	if (!a9_aop_wait(aop, READ_ONCE(aop->epmap_done), AOP_EPMAP_MS)) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-no-epmap", -ETIMEDOUT);
		mutex_unlock(&aop->lock);
		return;
	}
	if (a9_aop_dead(aop))
		return;
	if (force_cold) {
		mutex_lock(&aop->lock);
		aop->handshake_complete = true;
		aop->experiment_winner = "cold-device-hello";
		a9_aop_stage3_mark(aop, "handshake-complete");
		if (aop->experiment_stage == 2)
			a9_aop_finish(aop, PHASE_STOPPED, "cold-stage2-handshake-complete", 0);
		mutex_unlock(&aop->lock);
		if (aop->experiment_stage == 2)
			return;
		aop->experiment_step = "cold-stage3";
		queue_delayed_work(aop->wq, &aop->ping_work, 0);
		if (aop->cold_run) {
			ret = a9_aop_system_start(aop);
			if (ret) {
				mutex_lock(&aop->lock);
				a9_aop_fail(aop, "failed-system-bringup", ret);
				mutex_unlock(&aop->lock);
				return;
			}
			if (aop->cold_run == 1)
				return;
			goto start_endpoints;
		}
	}
	/* iOS waits for power state 0x20; continue without it, but record it. */
	if (!a9_aop_wait(aop, READ_ONCE(aop->iop_ready), AOP_IOP_READY_MS))
		dev_warn(aop->dev, "no IOP power state 0x20 (last 0x%x); starting endpoints anyway\n",
			 READ_ONCE(aop->iop_power));
start_endpoints:
	ret = 0;
	mutex_lock(&aop->lock);
	a9_aop_stage3_mark(aop, "before-accel-startep");
	if (aop->cold_run && !test_bit(AOP_ACCEL_EP, aop->present))
		ret = -ENODEV;
	/* The pinned firmware initializes EndpointRunLoopEventSources serially.
	 * wakehint (routing 0x2c) can block before accel emits even AFK INIT.
	 * Like iOS, enable every advertised application endpoint before waiting
	 * for services, then service their AFK handshakes on the shared RX path.
	 * Only the discovered accel service receives calibration/rate commands.
	 */
	for (i = 0; !ret && i < aop->neps; i++) {
		ret = a9_aop_send(aop, 0, a9_mgmt_startep(aop->eps[i].num));
		if (!ret)
			aop->eps[i].state = AFK_WAIT_INIT;
	}
	if (ret)
		a9_aop_fail(aop, "failed-startep", ret);
	else if (!aop->neps)
		a9_aop_fail(aop, "failed-no-app-endpoint", -ENODEV);
	else if (aop->phase == PHASE_WAIT_EPMAP)
		aop->phase = PHASE_WAIT_SERVICE;
	if (!ret)
		a9_aop_stage3_mark(aop, "application-endpoints-enabled");
	mutex_unlock(&aop->lock);
	if (a9_aop_dead(aop))
		return;
	if (!a9_aop_wait(aop, READ_ONCE(aop->accel) >= 0, AOP_SERVICE_MS)) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-no-accel-service", -ETIMEDOUT);
		mutex_unlock(&aop->lock);
		return;
	}
	mutex_lock(&aop->lock);
	aop->phase = PHASE_CONFIGURE;
	a9_aop_stage3_mark(aop, "accel-service-ready");
	aop->poll_ms = aop->experiment_stage ? 10 : 100;
	mutex_unlock(&aop->lock);

	ret = a9_aop_send_calibration(aop);
	if (!ret) {
		mutex_lock(&aop->lock);
		a9_aop_stage3_mark(aop, "calibration-acked");
		aop->rate_set = true;	/* first report after this point */
		aop->phase = PHASE_WAIT_REPORT;
		mutex_unlock(&aop->lock);
		ret = a9_aop_set_rate(aop, AOP_RATE_US, 0, AOP_RATE_US);
	}
	if (ret) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-configure", ret);
		mutex_unlock(&aop->lock);
		return;
	}
	a9_aop_stage3_mark(aop, "rate-acked");
	if (!a9_aop_wait(aop, READ_ONCE(aop->have_first_report), AOP_REPORT_MS)) {
		mutex_lock(&aop->lock);
		a9_aop_fail(aop, "failed-no-report", -ETIMEDOUT);
		mutex_unlock(&aop->lock);
		return;
	}

	mutex_lock(&aop->lock);
	a9_aop_stage3_mark(aop, "first-report");
	first.category = 0;
	first.subtype = A9_SPU_ACCEL_REPORT;
	first.payload = aop->first_report;
	first.size = aop->first_report_size;
	config.report_interval_us = AOP_RATE_US;
	config.first_report = &first;
	config.firmware_applied_arot = aop->arot_sent;
	mutex_unlock(&aop->lock);
	iio = a9_aop_accel_register(aop->dev, &config);
	mutex_lock(&aop->lock);
	if (IS_ERR(iio)) {
		a9_aop_fail(aop, "failed-iio-register", PTR_ERR(iio));
	} else if (aop->phase == PHASE_WAIT_REPORT) {
		aop->iio = iio;
		aop->phase = PHASE_STREAMING;
		aop->result = "streaming";
		a9_aop_stage3_mark(aop, "iio-registered");
		dev_info(aop->dev, "accelerometer streaming at %u us, calibration blobs %u%s\n",
			 AOP_RATE_US, aop->cal_sent,
			 aop->arot_sent ? " (ARot applied in firmware)" :
			 " (ARot not acknowledged; validate report frame)");
	} else {
		a9_aop_accel_invalidate(iio);
	}
	mutex_unlock(&aop->lock);
}

/* ---- debugfs ------------------------------------------------------------- */

static int a9_aop_system_buffers_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	unsigned int ep;

	mutex_lock(&aop->lock);
	for (ep = 1; ep < A9_SYS_COUNT; ep++) {
		struct a9_aop_buffer *buf = &aop->system[ep].buffer;

		seq_printf(s, "ep=%u pa=%pad size=0x%zx shown=0x%zx\n", ep,
			   &buf->dma, buf->size, min_t(size_t, buf->size, SZ_64K));
		if (!buf->va)
			continue;
		dma_rmb();
		seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, buf->va,
			     min_t(size_t, buf->size, SZ_64K), true);
	}
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_system_buffers);

static int a9_aop_state_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	unsigned int i, j;

	mutex_lock(&aop->lock);
	seq_printf(s, "format=1 phase=%s result=%s errno=%d path=%s\n",
		   phase_names[aop->phase], aop->result ?: "none", aop->error,
		   aop->phase <= PHASE_PROBE ? "undecided" :
		   aop->running_attach ? "attach" : aop->cold ? "cold" : "warm");
	seq_printf(s, "force_cold=%d halt_accepted=%d halt_control=%08x halt_predicate=run-clear-delay halt_delay_ms=10 execution=unknown drained=%d stale_inbox=%u stale_outbox=%u reset_hook=none\n",
		   force_cold, aop->force_halted, aop->force_halt_control,
		   aop->force_drained, aop->stale_inbox, aop->stale_outbox);
	seq_printf(s, "quiesce_opt_in=%d shutdown_quiesced=%d shutdown_power_requested=%d\n",
		   quiesce, aop->shutdown_quiesced, aop->shutdown_power_requested);
	seq_printf(s, "spi0_reset opt_in=%d ran=%d skipped=%s errno=%d before_ctl=%08x before_8d0=%08x after_ctl=%08x after_8d0=%08x prerun_8d0=%08x reasserted=%d pmgr_before=%08x/%d pmgr_after=%08x/%d decision=%s reason=%s auto_boot=%u out4020=%08x prev=%s prev_hello=%d prev_halted=%d prev_steps=0x%x prev_carry=%d\n",
		   spi0_reset, aop->spi0.ran, aop->spi0.skipped ?: "none",
		   aop->spi0.error, aop->spi0.before[A9_SPI0_I_CONTROL],
		   aop->spi0.before[A9_SPI0_I_FAULT], aop->spi0.after[A9_SPI0_I_CONTROL],
		   aop->spi0.after[A9_SPI0_I_FAULT], aop->spi0.prerun,
		   aop->spi0.reasserted, aop->spi0.pmgr_before,
		   aop->spi0.pmgr_before_errno, aop->spi0.pmgr_after,
		   aop->spi0.pmgr_after_errno,
		   !aop->spi0.decided ? "none" :
		   a9_spi0_reason_runs(aop->spi0.reason) ? "run" : "skip",
		   aop->spi0.decided ? a9_spi0_reason_name(aop->spi0.reason) : "none",
		   aop->spi0.auto_boot, aop->spi0.outbox,
		   a9_spi0_prev_name(aop->spi0.prev.state), aop->spi0.prev.hello,
		   aop->spi0.prev.halted, aop->spi0.prev.steps,
		   aop->spi0.prev.carried);
	seq_printf(s, "spi0_ch3 ran=%d result=%s errno=%d poll_errno=%d run_before=%d idle_slices=%u idle_us=%llu before_c3=%08x before_c3c=%08x before_desc=%08x before_8d0=%08x after_c3=%08x after_c3c=%08x after_desc=%08x after_8d0=%08x window=%d samples=%u elapsed_ms=%llu reasserted=%d reassert_sample=%u reassert_ms=%llu\n",
		   aop->spi0.ch3.ran, aop->spi0.ch3.failed ?:
		   aop->spi0.ch3.ran ? "pass" : "none", aop->spi0.ch3.error,
		   aop->spi0.ch3.poll, aop->spi0.ch3.run_before,
		   aop->spi0.ch3.slices, aop->spi0.ch3.idle_us,
		   aop->spi0.ch3.before[A9_CH3_I_C3],
		   aop->spi0.ch3.before[A9_CH3_I_C3C], aop->spi0.ch3.before[A9_CH3_I_DESC],
		   aop->spi0.ch3.before[A9_CH3_I_8D0], aop->spi0.ch3.after[A9_CH3_I_C3],
		   aop->spi0.ch3.after[A9_CH3_I_C3C], aop->spi0.ch3.after[A9_CH3_I_DESC],
		   aop->spi0.ch3.after[A9_CH3_I_8D0], aop->spi0.ch3.window_done,
		   aop->spi0.ch3.samples, aop->spi0.ch3.elapsed_ms,
		   aop->spi0.ch3.reasserted, aop->spi0.ch3.reassert_sample,
		   aop->spi0.ch3.reassert_ms);
	seq_printf(s, "spi0_intc_reset done=%d asserted=%08x after=%08x result=%s\n",
		   aop->spi0.intc.reset_done, aop->spi0.intc.reset_asserted,
		   aop->spi0.intc.reset_after, aop->spi0.intc.failed ?:
		   aop->spi0.intc.reset_done ? "pass" : "none");
	seq_printf(s, "spi0_intc disabled=%d acked=%d result=%s states=0x%x disable=%08x->%08x ack=%08x->%08x current_pre=%08x current_post=%08x scm_pre=%08x:%08x scm_post=%08x:%08x c3_ival_pre=%08x:%08x c3_ival_post=%08x:%08x\n",
		   aop->spi0.intc.disabled, aop->spi0.intc.acked,
		   aop->spi0.intc.failed ?: aop->spi0.intc.acked ? "pass" : "none",
		   aop->spi0.intc.states,
		   aop->spi0.intc.state[A9_INTC_AT_PRE_DISABLE][A9_INTC_I_MASK0],
		   aop->spi0.intc.state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_MASK0],
		   aop->spi0.intc.state[A9_INTC_AT_PRE_ACK][A9_INTC_I_ACK],
		   aop->spi0.intc.state[A9_INTC_AT_POST_ACK][A9_INTC_I_ACK],
		   aop->spi0.intc.state[A9_INTC_AT_PRE_ACK][A9_INTC_I_CURRENT],
		   aop->spi0.intc.state[A9_INTC_AT_POST_ACK][A9_INTC_I_CURRENT],
		   aop->spi0.intc.scm[0][1], aop->spi0.intc.scm[0][0],
		   aop->spi0.intc.scm[1][1], aop->spi0.intc.scm[1][0],
		   aop->spi0.intc.scm[0][2], aop->spi0.intc.scm[0][3],
		   aop->spi0.intc.scm[1][2], aop->spi0.intc.scm[1][3]);
	seq_printf(s, "wake_wrapper opt_in=%d done=%d errno=%d\n",
		   wake_wrapper, aop->wake_wrapper_done, aop->wake_wrapper_errno);
	seq_printf(s, "handoff opt_in=%d record=%s reason=%s result=%s generation=%u path=%s adopt=%d sram_valid=%d sys_adopted=%u sys_regranted=%u afk_reused=%u afk_fresh=%u pool_capacity=0x%zx\n",
		   handoff, aop->ho.seen ? "present" : "missing",
		   a9_ho_reason_names[aop->ho.reason], aop->ho.result ?: "none",
		   aop->ho.generation, a9_ho_path_names[aop->ho.path],
		   aop->ho.adopt, aop->ho.sram_valid, aop->ho.sys_adopted,
		   aop->ho.sys_regranted, aop->ho.afk_reused, aop->ho.afk_fresh,
		   aop->pool_size);
	seq_printf(s, "shutdown_log reserved=%d prev_transcript_bytes=%zu steps=0x%x crashlog_snapshot=%zu\n",
		   !!aop->ho.log, aop->ho.prev_len, aop->ho.steps, aop->crash_size);
	seq_printf(s, "cold_test=%u dcache_off=%u iop_irqs=%u ecid=%u nonce=%u\n",
		   cold_test, !!(cold_test & A9_COLD_UNCACHED),
		   !!(cold_test & A9_COLD_IOP_IRQS),
		   !!(cold_test & A9_COLD_ECID), !!(cold_test & A9_COLD_NONCE));
	seq_printf(s, "cold_defer=%u triggered=%d ap_irqs_masked=%d post_prng_fence=%d\n",
		   cold_defer, aop->cold_triggered, !!cold_defer, cold_defer == 2);
	seq_printf(s, "cold_run=%u system_ready=%d iop_crashed=%d accel_endpoint=0x%02x\n",
		   aop->cold_run, aop->system_ready, aop->iop_crashed, AOP_ACCEL_EP);
	seq_printf(s, "stage3_checkpoint=%s system_grants=%llu\n",
		   aop->stage3_checkpoint ?: "none", aop->system_grants);
	for (i = 1; i < A9_SYS_COUNT; i++) {
		struct a9_aop_system *sys = &aop->system[i];

		seq_printf(s, "system ep=%u started=%d pa=%pad size=0x%zx words=%llu acks=%llu errors=%llu unknown=%llu entries=%u message_size=%u\n",
			   i, sys->started, &sys->buffer.dma, sys->buffer.size,
			   sys->words, sys->acks, sys->errors, sys->unknown,
			   sys->entries, sys->message_size);
	}
	seq_printf(s, "cold_observation frames=%u baseline=%d wake220_sent=%d wake_errno=%d hello_bound_ms=%u release_ns=%llu\n",
		   aop->cold_frames, aop->cold_baseline, aop->cold_wake_sent,
		   aop->cold_wake_errno, AOP_COLD_HELLO_MS, aop->cold_release_ns);
	seq_printf(s, "experiment stage=%u attempt=%d speculative_hello=%d handshake_complete=%d identity=unverified old_dma=unknown startup_words=%u pool_pa=%pap pool_size=0x%zx pool_used=0x%zx pool_retained=1\n",
		   aop->experiment_stage, atomic_read(&experiment_attempted),
		   aop->speculative_hello, aop->handshake_complete, aop->startup_count,
		   &aop->pool_pa, aop->pool_size, aop->pool_used);
	seq_printf(s, "ladder=%s wake=%u cycle=%d step=%s winner=%s device_hellos=%u speculative_replies=%u mailbox_unmasked=%d polling_only=1 registers=%u registers_dropped=%u auto_boot_valid=%d\n",
		   wake ? "hardware-wake" : "s2b", wake, cycle,
		   aop->experiment_step ?: "none", aop->experiment_winner ?: "none",
		   aop->device_hellos, aop->speculative_replies, aop->mailbox_unmasked,
		   aop->reg_count, aop->reg_dropped, !!aop->autoboot);
	seq_printf(s, "snapshot cpu_control=0x%08x wrapper_b88=0x%08x inbox=0x%08x outbox=0x%08x idle=0x%08x auto_boot=0x%08x\n",
		   aop->snapshot[0], aop->snapshot[1], aop->snapshot[2],
		   aop->snapshot[3], aop->snapshot[4], aop->snapshot[5]);
	seq_printf(s, "attach enabled=%d stage=%u result=%s fingerprint_read=%d image_verified=%d tags_verified=%d ext_reserved=%d ext_verified=%d attempts=%u consecutive_acks=%u hello_reply_sent=%d adopted=%d\n",
		   apple_a9_aop_attach(), aop->takeover.stage,
		   a9_attach_result_name(aop->takeover.result),
		   aop->fingerprint_read, aop->image_verified, aop->tags_verified,
		   aop->ext_reserved, aop->ext_verified, aop->takeover.attempts,
		   aop->takeover.acks, aop->takeover.hello_sent,
		   aop->takeover.stage == A9_ATTACH_ADOPTED);
	seq_printf(s, "layout supported=contiguous-v5 alternate_layouts=0 remap_matches_cold=%d\n",
		   aop->diagnostics.mapping_matches);
	seq_printf(s, "region pa=0x%llx size=0x%llx aop_target=%u\n",
		   (unsigned long long)aop->region_pa,
		   (unsigned long long)aop->region_size, aop->aop_target);
	seq_printf(s, "mailbox rx_words=%llu tx_words=%llu irqs=%llu irq_empty=%llu irq_enabled=%d irq_storm=%d rxq_full=%llu fault_control=0x%08x\n",
		   aop->rx_words, aop->tx_words, aop->irq_count, aop->irq_empty,
		   aop->irq_enabled, aop->irq_storm, aop->rxq_full,
		   aop->fault_control);
	seq_printf(s, "mgmt hello=%d min=%u max=%u epmap_done=%d chunks=%u ap_power_ack=%d:0x%x iop_power=0x%x iop_ready=%d ping_seq=%u ping_acks=%llu system_words=%llu unknown_mgmt=%llu unknown_ep=%llu\n",
		   aop->hello, aop->hello_min, aop->hello_max, aop->epmap_done,
		   aop->epmap_chunks, aop->ap_power_acked, aop->ap_power_ack,
		   aop->iop_power, aop->iop_ready, aop->ping_seq, aop->ping_acks,
		   aop->system_words, aop->unknown_mgmt, aop->unknown_ep);
	seq_printf(s, "firmware runtime_size=0x%x ext_pa=%pad ext_size=0x%zx\n",
		   aop->runtime_size, &aop->ext_dma, aop->ext_size);
	seq_printf(s, "endpoints=%*pbl\n", 256, aop->present);
	for (i = 0; i < aop->neps; i++) {
		struct a9_aop_ep *e = &aop->eps[i];

		seq_printf(s, "ep %u state=%s bufs=%u dropped_before_init=%llu unknown=%llu rx_packets=%llu rx_errors=%llu last_rx_error=%d magic_unexpected=%llu last_word=0x%x tx_packets=%llu\n",
			   e->num, afk_names[e->state], e->nbufs,
			   e->dropped_before_init, e->unknown, e->rx_packets,
			   e->rx_errors, e->last_rx_error, e->magic_unexpected,
			   e->last_word, e->tx_packets);
		for (j = 0; j < e->nbufs; j++)
			seq_printf(s, "  buf tag=0x%04x pa=%pad size=0x%zx\n",
				   e->bufs[j].tag, &e->bufs[j].dma, e->bufs[j].size);
		for (j = 0; j < RING_COUNT; j++) {
			struct a9_ring *r = &e->rings[j];

			if (!e->ring_seen[j])
				continue;
			seq_printf(s, "  ring %s tag=0x%04x offset=0x%x size=0x%x valid=%d hdr_bufsize=0x%x hdr_version=0x%08x cursor=0x%x rptr=0x%x wptr=0x%x\n",
				   ring_names[j], e->ring_tag[j], e->ring_offset[j],
				   e->ring_size[j], e->ring_valid[j],
				   get_unaligned_le32(e->ring_seen[j]),
				   get_unaligned_le32(e->ring_seen[j] + 4),
				   r->cursor,
				   get_unaligned_le32(e->ring_seen[j] + A9_RING_BLOCK),
				   get_unaligned_le32(e->ring_seen[j] + 2 * A9_RING_BLOCK));
		}
	}
	for (i = 0; i < aop->nservices; i++) {
		struct a9_aop_service *svc = &aop->services[i];

		seq_printf(s, "service name=%s ep=%u channel=0x%x service_id=0x%08x flags=0x%x interface=%u%s\n",
			   svc->info.name, svc->ep, svc->channel,
			   svc->info.service_id, svc->info.flags,
			   svc->info.interface,
			   (int)i == aop->accel ? " bound=accel" : "");
	}
	seq_printf(s, "accel cal_sent=%u arot_sent=%d rate_set=%d reports=%llu report_errors=%llu channel_mismatch=%llu malformed=%llu trailing=%llu truncated=%llu doorbells=%llu iio=%d cal_unsupported=%u cal_state=%u\n",
		   aop->cal_sent, aop->arot_sent, aop->rate_set, aop->reports,
		   aop->report_errors, aop->channel_mismatch, aop->malformed,
		   aop->trailing, aop->truncated, aop->doorbells, !!aop->iio,
		   aop->cal_unsupported, aop->cal_state);
	seq_printf(s, "command tag=0x%04x pending=%d status=%d last_reply category=0x%02x subtype=0x%x tag=0x%04x size=%u word0=0x%08x seen=%llu\n",
		   aop->cmd_tag, aop->cmd_pending, aop->cmd_status,
		   aop->reply_category, aop->reply_subtype, aop->reply_tag,
		   aop->reply_size, aop->reply_word, aop->replies_seen);
	if (aop->reports)
		seq_printf(s, "accel last seq=%u status=0x%08x x=%d y=%d z=%d\n",
			   aop->motion.sequence, aop->motion.status,
			   aop->motion.axes[0], aop->motion.axes[1],
			   aop->motion.axes[2]);
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_state);

static int a9_aop_previous_shutdown_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;

	if (aop->ho.prev_log)
		seq_write(s, aop->ho.prev_log, aop->ho.prev_len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_previous_shutdown);

/* Raw frozen bytes; decode on the host with decode-crashlog.py. */
static ssize_t a9_aop_crashlog_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct a9_aop *aop = file->private_data;
	ssize_t ret;

	mutex_lock(&aop->lock);
	ret = simple_read_from_buffer(buf, count, ppos, aop->crash_copy,
				      aop->crash_size);
	mutex_unlock(&aop->lock);
	return ret;
}

static const struct file_operations a9_aop_crashlog_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = a9_aop_crashlog_read,
	.llseek = default_llseek,
};

static int a9_aop_registers_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	struct a9_exp_regs r;
	unsigned long flags;
	unsigned int i, j, count;

	/* Copy one saved entry at a time; reading debugfs never accesses MMIO. */
	spin_lock_irqsave(&aop->rx_lock, flags);
	count = aop->reg_count;
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	seq_puts(s, "format=1 status_only=1 data_words=startup_transcript offsets=wrapper-relative\n");
	for (i = 0; i < count; i++) {
		spin_lock_irqsave(&aop->rx_lock, flags);
		r = aop->reg_trace[i];
		spin_unlock_irqrestore(&aop->rx_lock, flags);
		seq_printf(s, "%u ns=%llu step=%s event=%s", i, r.ns, r.step, r.event);
		for (j = 0; j < ARRAY_SIZE(a9_exp_reg_offsets); j++)
			seq_printf(s, " %04x=%08x", a9_exp_reg_offsets[j], r.value[j]);
		seq_printf(s, " auto_boot=%08x auto_boot_valid=%d",
			   r.auto_boot, r.auto_boot_valid);
		seq_putc(s, '\n');
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_registers);

static int a9_aop_transcript_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	struct a9_aop_trace *copy;
	unsigned long flags;
	u64 total, first, i;

	copy = kmalloc_array(AOP_TRACE, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	spin_lock_irqsave(&aop->rx_lock, flags);
	memcpy(copy, aop->trace, sizeof(aop->trace));
	total = aop->trace_total;
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	first = total > AOP_TRACE ? total - AOP_TRACE : 0;
	seq_printf(s, "# total=%llu shown_from=%llu  dir ep payload status phase\n",
		   total, first);
	for (i = first; i < total; i++) {
		struct a9_aop_trace *t = &copy[i % AOP_TRACE];

		seq_printf(s, "%llu %llu.%09llu %c ep=0x%02x %014llx %d phase=%s\n", i,
			   t->ns / NSEC_PER_SEC, t->ns % NSEC_PER_SEC, t->dir,
			   a9_aop_endpoint(t->word), a9_aop_payload(t->word),
			   t->status, phase_names[t->phase]);
	}
	kfree(copy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_transcript);

static int a9_aop_startup_transcript_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	struct a9_aop_trace *copy;
	unsigned long flags;
	unsigned int count, i;

	copy = kmalloc_array(AOP_TRACE, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	spin_lock_irqsave(&aop->rx_lock, flags);
	count = aop->startup_count;
	memcpy(copy, aop->startup_trace, count * sizeof(*copy));
	spin_unlock_irqrestore(&aop->rx_lock, flags);
	seq_printf(s, "# startup_words=%u retained=1 maximum=%u\n", count, AOP_TRACE);
	for (i = 0; i < count; i++)
		seq_printf(s, "%u %llu %c %016llx status=%d phase=%s\n", i,
			   copy[i].ns, copy[i].dir, copy[i].word, copy[i].status,
			   phase_names[copy[i].phase]);
	kfree(copy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_startup_transcript);

static int a9_aop_samples_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	u64 first, i;
	unsigned int at;

	mutex_lock(&aop->lock);
	first = aop->reports > A9_EXP_SAMPLES ? aop->reports - A9_EXP_SAMPLES : 0;
	seq_printf(s, "# reports=%llu shown_from=%llu units=unverified-aop-counts\n",
		   aop->reports, first);
	for (i = first; i < aop->reports; i++) {
		at = i % A9_EXP_SAMPLES;
		seq_printf(s, "%llu ns=%llu seq=%u status=0x%x x=%d y=%d z=%d raw=%*phN\n",
			   i, aop->samples[at].ns, aop->samples[at].motion.sequence,
			   aop->samples[at].motion.status, aop->samples[at].motion.axes[0],
			   aop->samples[at].motion.axes[1], aop->samples[at].motion.axes[2],
			   aop->samples[at].size, aop->samples[at].raw);
	}
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_samples);

static int a9_aop_rings_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	unsigned int i, j;
	u32 at, n;

	mutex_lock(&aop->lock);
	for (i = 0; i < aop->neps; i++) {
		struct a9_aop_ep *e = &aop->eps[i];

		for (j = 0; j < RING_COUNT; j++) {
			struct a9_ring *r = &e->rings[j];
			u8 *base = e->ring_seen[j];

			if (!base)
				continue;
			seq_printf(s, "ep %u ring %s size=0x%x valid=%d\n", e->num,
				   ring_names[j], e->ring_size[j], e->ring_valid[j]);
			seq_hex_dump(s, "  +00 ", DUMP_PREFIX_NONE, 16, 4, base, 16, false);
			seq_hex_dump(s, "  +40 ", DUMP_PREFIX_NONE, 16, 4,
				     base + A9_RING_BLOCK, 16, false);
			seq_hex_dump(s, "  +80 ", DUMP_PREFIX_NONE, 16, 4,
				     base + 2 * A9_RING_BLOCK, 16, false);
			if (!e->ring_valid[j]) {
				n = min_t(u32, 256, e->ring_size[j] - A9_RING_HEADER);
				seq_hex_dump(s, "  ", DUMP_PREFIX_OFFSET, 16, 4,
					     base + A9_RING_HEADER, n, false);
				continue;
			}
			/* Up to 256 bytes before the RX cursor / TX wptr. */
			at = j == RING_TX ? a9_ring_wptr(r) : r->cursor;
			if (at >= r->bufsize)
				at = 0;
			n = min_t(u32, 256, r->bufsize);
			at = at >= n ? at - n : 0;
			seq_printf(s, "  entries from 0x%x:\n", at);
			seq_hex_dump(s, "  ", DUMP_PREFIX_OFFSET, 16, 4,
				     a9_ring_entry(r, at), min(n, r->bufsize - at), false);
		}
	}
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_rings);

/* All diagnostic readers below use frozen RAM copies, including on refusal. */
static int a9_aop_cold_memory_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	unsigned int i, j;

	mutex_lock(&aop->lock);
	seq_printf(s, "format=1 pa=%pa bytes=0x%zx baseline=%d frames=%u sample=non-atomic execution=unproven\n",
		   &aop->region_pa, (size_t)aop->region_size,
		   aop->cold_baseline, aop->cold_frames);
	for (i = 0; i < aop->cold_frames; i++) {
		struct a9_aop_cold_frame *f = &aop->cold_frame[i];

		seq_printf(s, "%u event=%s ns=%llu errno=%d sha256=%*phN guard=%08x STKG=%08x auto_boot=%08x\n",
			   i, f->event, f->ns, f->error, SHA256_DIGEST_SIZE, f->sha256,
			   f->guard, f->tag_guard, f->auto_boot);
		for (j = 0; j < ARRAY_SIZE(a9_cold_reg_offsets); j++)
			seq_printf(s, " %04x=%08x", a9_cold_reg_offsets[j], f->regs[j]);
		seq_putc(s, '\n');
		for (j = 0; !f->error && j < ARRAY_SIZE(a9_cold_spans); j++) {
			struct a9_aop_memory_delta *d = &f->delta[j];

			seq_printf(s, " span=%s changed_words=%u first=%08x before=%08x after=%08x\n",
				   a9_cold_spans[j].name, d->words,
				   d->words ? a9_cold_spans[j].begin + d->first : (u32)-1,
				   d->before, d->after);
		}
	}
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_cold_memory);

static ssize_t a9_aop_cold_ram_read(struct file *file, char __user *buf,
				  size_t size, loff_t *pos, bool before)
{
	struct a9_aop *aop = file->private_data;
	ssize_t ret = 0;

	mutex_lock(&aop->lock);
	if (aop->cold_baseline)
		ret = simple_read_from_buffer(buf, size, pos,
					      before ? aop->cold_before : aop->cold_after,
					      aop->region_size);
	mutex_unlock(&aop->lock);
	return ret;
}

static ssize_t a9_aop_cold_before_read(struct file *file, char __user *buf,
				     size_t size, loff_t *pos)
{
	return a9_aop_cold_ram_read(file, buf, size, pos, true);
}

static ssize_t a9_aop_cold_after_read(struct file *file, char __user *buf,
				    size_t size, loff_t *pos)
{
	return a9_aop_cold_ram_read(file, buf, size, pos, false);
}

static const struct file_operations a9_aop_cold_before_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = a9_aop_cold_before_read,
	.llseek = default_llseek,
};

static const struct file_operations a9_aop_cold_after_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = a9_aop_cold_after_read,
	.llseek = default_llseek,
};

static void a9_aop_fingerprint_window(struct seq_file *s, const char *name,
				      u32 offset, const u8 *actual,
				      const u8 *expected, size_t size, bool pinned)
{
	struct a9_diag_diff diff;
	size_t i;

	seq_printf(s, "window=%s region_offset=0x%x bytes=0x%zx\n", name, offset, size);
	if (pinned) {
		diff = a9_diag_compare(actual, expected, size);
		seq_printf(s, "comparison=%s mismatched_bytes=%zu",
			   diff.count ? "different" : "equal", diff.count);
		if (diff.count)
			seq_printf(s, " first_mismatch=0x%zx actual=0x%02x expected=0x%02x",
				   offset + diff.first, actual[diff.first], expected[diff.first]);
		seq_putc(s, '\n');
	}
	seq_hex_dump(s, "actual ", DUMP_PREFIX_OFFSET, 16, 1, actual, size, true);
	if (pinned)
		seq_hex_dump(s, "expected ", DUMP_PREFIX_OFFSET, 16, 1, expected, size, true);
	if (strcmp(name, "build-string"))
		return;
	/* Exactly 64 bytes, including embedded NULs; never treat RAM as a C string. */
	seq_puts(s, "actual_printable=|");
	for (i = 0; i < size; i++)
		seq_putc(s, a9_diag_printable(actual[i]));
	seq_puts(s, "|\n");
	if (pinned) {
		seq_puts(s, "expected_printable=|");
		for (i = 0; i < size; i++)
			seq_putc(s, a9_diag_printable(expected[i]));
		seq_puts(s, "|\n");
	}
}

static void a9_aop_fingerprint_tags(struct seq_file *s, struct a9_aop *aop)
{
	const u8 *actual = aop->fingerprint.table;
	const u8 *expected = aop->diagnostics.expected.table;
	struct a9_diag_tag tag;
	size_t at, cursor = 0;
	int ret;

	seq_puts(s, "tag_words: expected_unpatched is the pinned file template; loader values differ legitimately\n");
	for (at = 0; at + 4 <= A9_ATTACH_TABLE_SIZE; at += 4) {
		seq_printf(s, "+%06zx actual_le32=0x%08x", A9_ATTACH_TABLE_OFFSET + at,
			   get_unaligned_le32(actual + at));
		if (aop->diagnostics.reference_pinned)
			seq_printf(s, " expected_unpatched_le32=0x%08x",
				   get_unaligned_le32(expected + at));
		seq_putc(s, '\n');
	}
	seq_printf(s, "+%06zx tail_actual=%*phN", A9_ATTACH_TABLE_OFFSET + at,
		   (int)(A9_ATTACH_TABLE_SIZE - at), actual + at);
	if (aop->diagnostics.reference_pinned)
		seq_printf(s, " tail_expected_unpatched=%*phN",
			   (int)(A9_ATTACH_TABLE_SIZE - at), expected + at);
	seq_putc(s, '\n');
	/* Raw words are always shown, even when the first TLV is malformed. */
	while ((ret = a9_diag_next_tag(actual, A9_ATTACH_TABLE_SIZE, &cursor, &tag)) > 0)
		seq_printf(s, "actual_tag +%06zx name=%c%c%c%c len=%u data=%*phN\n",
			   A9_ATTACH_TABLE_OFFSET + cursor - 8 - tag.size,
			   a9_diag_printable(tag.name >> 24),
			   a9_diag_printable(tag.name >> 16),
			   a9_diag_printable(tag.name >> 8), a9_diag_printable(tag.name),
			   tag.size, (int)min_t(u32, tag.size, 64), tag.data);
	if (ret)
		seq_printf(s, "actual_tag_walk=malformed offset=0x%zx errno=%d (raw words retained)\n",
			   A9_ATTACH_TABLE_OFFSET + cursor, ret);
	seq_printf(s, "expected_loader AOPt=%u WrAd=0x%llx McRA=0x4000 nCal=0 SOC_=0x%x SOCR=%u ED0p=0 RTTO=0 DEVf=0\n",
		   aop->aop_target, (unsigned long long)aop->wrapper_pa,
		   aop->chip_id, aop->chip_revision);
	seq_puts(s, "expected_loader EC0p=nonzero,16K-aligned,48-bit,intact-ext-in-reserved-memory; STKG/PRNG/NONC/ECID=loader-specific; other values=file defaults\n");
}

static int a9_aop_fingerprint_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	const struct a9_attach_fingerprint *expected = &aop->diagnostics.expected;
	const struct a9_attach_fingerprint *actual = &aop->fingerprint;
	struct a9_diag_diff identity;
	bool pinned;

	mutex_lock(&aop->lock);
	pinned = aop->diagnostics.reference_pinned;
	seq_puts(s, "format=1 source=phase-A-frozen-snapshot hardware_reads=none hardware_writes=none\n");
	seq_printf(s, "snapshot_access=aligned-readl32 stride=4 tag_bus_bytes=0x1e0 read_errno=%d build_identity_bytes=%u runtime_guard_offset=0x47474\n",
		   aop->diagnostics.read_errno, A9_ATTACH_BUILD_ID_SIZE);
	seq_puts(s, "build_window_tail=runtime-data (guard/cursor are not build text); guard must match pinned-layout STKG; no alternate AP base is followed\n");
	seq_printf(s, "reference=%s size=%u sha256=af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8 pinned=%d errno=%d\n",
		   AOP_MAIN_FW, A9_ATTACH_MAIN_SIZE, pinned, aop->diagnostics.reference_errno);
	seq_printf(s, "region pa=0x%llx size=0x%llx snapshot_available=%d\n",
		   (unsigned long long)aop->region_pa, (unsigned long long)aop->region_size,
		   aop->diagnostics.captured);
	if (!aop->diagnostics.captured) {
		seq_puts(s, "no running/empty Phase A snapshot; no diagnostic MMIO attempted by this read\n");
		goto out;
	}
	seq_printf(s, "remap actual +08=%08x +10=%08x +18=%08x +20=%08x +28=%08x +30=%08x +38=%08x\n",
		   aop->diagnostics.remap[0], aop->diagnostics.remap[1],
		   aop->diagnostics.remap[2], aop->diagnostics.remap[3],
		   aop->diagnostics.remap[4], aop->diagnostics.remap[5], aop->diagnostics.remap[6]);
	seq_printf(s, "remap after +08=%08x +10=%08x +18=%08x +20=%08x +28=%08x +30=%08x +38=%08x\n",
		   aop->diagnostics.remap_after[0], aop->diagnostics.remap_after[1],
		   aop->diagnostics.remap_after[2], aop->diagnostics.remap_after[3],
		   aop->diagnostics.remap_after[4], aop->diagnostics.remap_after[5],
		   aop->diagnostics.remap_after[6]);
	seq_printf(s, "layout supported=contiguous-v5 alternate_layouts=0 remap_stable=%d remap_matches_cold=%d (software whitelist, not proof of runtime location)\n",
		   !memcmp(aop->diagnostics.remap, aop->diagnostics.remap_after,
			   sizeof(aop->diagnostics.remap)), aop->diagnostics.mapping_matches);
	seq_printf(s, "firmware_translation delta=0x%llx first_page=0x%x last_page=0x%x (inclusive 32-bit page view in pinned firmware; live code physical address unknown)\n",
		   (unsigned long long)((u64)aop->diagnostics.remap[1] << 32 |
					aop->diagnostics.remap[0]),
		   aop->diagnostics.remap[2] >> 12 | aop->diagnostics.remap[3] << 20,
		   aop->diagnostics.remap[4] >> 12 | aop->diagnostics.remap[5] << 20);
	seq_printf(s, "remap expected_cold_loader +08=%08x +10=%08x +18=00000000 +20=00000000 +28=%08x +30=00000000 +38=00000001 (diagnostic only; no following remap PA)\n",
		   lower_32_bits(aop->region_pa), upper_32_bits(aop->region_pa),
		   (u32)aop->region_size & ~0xfffU);
	a9_aop_fingerprint_window(s, "region-prefix", 0, aop->diagnostics.prefix,
				  aop->diagnostics.expected_prefix, A9_DIAG_PREFIX_SIZE, pinned);
	a9_aop_fingerprint_window(s, "uuid-header", A9_DIAG_UUID_OFFSET,
				  actual->head + A9_DIAG_UUID_OFFSET,
				  expected->head + A9_DIAG_UUID_OFFSET, A9_DIAG_UUID_SIZE, pinned);
	seq_printf(s, "uuid actual magic=0x%08x version=%u table_offset=0x%x table_size=0x%x\n",
		   get_unaligned_le32(actual->head + 0x20),
		   get_unaligned_le32(actual->head + 0x24),
		   get_unaligned_le32(actual->head + 0x48),
		   get_unaligned_le32(actual->head + 0x4c));
	seq_puts(s, "uuid expected magic=0x64697575 version=5 table_offset=0x389b8 table_size=0x1df\n");
	seq_printf(s, "runtime_header data_backing_word=0x%08x expected=0 (IOP-side metadata, not an AP address; non-default header is refused)\n",
		   get_unaligned_le32(actual->head + 0x58));
	a9_aop_fingerprint_window(s, "build-string", A9_ATTACH_BUILD_OFFSET,
				  actual->build, expected->build, A9_ATTACH_BUILD_SIZE, pinned);
	if (pinned) {
		identity = a9_diag_compare(actual->build, expected->build, A9_ATTACH_BUILD_ID_SIZE);
		seq_printf(s, "build_identity comparison=%s bytes=%u mismatched_bytes=%zu (raw 64-byte window above includes mutable globals)\n",
			   identity.count ? "different" : "equal", A9_ATTACH_BUILD_ID_SIZE,
			   identity.count);
	}
	seq_printf(s, "runtime_data guard=0x%08x cursor=0x%08x tags_and_guard_verified=%d\n",
		   get_unaligned_le32(actual->build + A9_ATTACH_GUARD_OFFSET),
		   get_unaligned_le32(actual->build + 0x38), aop->tags_verified);
	a9_aop_fingerprint_window(s, "tag-table-unpatched-template", A9_ATTACH_TABLE_OFFSET,
				  actual->table, expected->table, A9_ATTACH_TABLE_SIZE, pinned);
	a9_aop_fingerprint_tags(s, aop);
out:
	seq_puts(s, "snapshot_end=1\n");
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_fingerprint);

static int a9_aop_patchbay_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	size_t cursor = 0;
	u32 tag, size;

	mutex_lock(&aop->lock);
	if (aop->fingerprint_read) {
		seq_puts(s, "running snapshot (read-only, not patched by Linux)\n");
		seq_hex_dump(s, "head ", DUMP_PREFIX_OFFSET, 16, 1,
			     aop->fingerprint.head, sizeof(aop->fingerprint.head), false);
		seq_hex_dump(s, "build@47440 ", DUMP_PREFIX_OFFSET, 16, 1,
			     aop->fingerprint.build, sizeof(aop->fingerprint.build), false);
	}
	if (!aop->table) {
		seq_puts(s, "not loaded by this driver\n");
		mutex_unlock(&aop->lock);
		return 0;
	}
	while (cursor + 8 <= aop->table_size) {
		tag = get_unaligned_le32(aop->table + cursor);
		size = get_unaligned_le32(aop->table + cursor + 4);
		if (size > aop->table_size - cursor - 8)
			break;
		seq_printf(s, "%c%c%c%c %u ", tag >> 24, (tag >> 16) & 0xff,
			   (tag >> 8) & 0xff, tag & 0xff, size);
		seq_printf(s, "%*phN\n", (int)min_t(u32, size, 64),
			   aop->table + cursor + 8);
		cursor += 8 + size;
	}
	mutex_unlock(&aop->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_patchbay);

/* ---- platform ------------------------------------------------------------- */

static ssize_t cold_release_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct a9_aop *aop = dev_get_drvdata(dev);

	return sysfs_emit(buf, "defer=%u triggered=%u\n", cold_defer,
			  READ_ONCE(aop->cold_triggered));
}

static ssize_t cold_release_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct a9_aop *aop = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret || value != 1 || !force_cold || !cold_defer)
		return -EINVAL;
	if (!a9_aop_ramoops_ready())
		return -ENODEV;
	mutex_lock(&aop->lock);
	/* A handoff record of any state forbids a fallback reload this boot. */
	if (aop->stopping || aop->cold_triggered || (handoff && aop->ho.seen)) {
		ret = -EBUSY;
	} else {
		aop->cold_triggered = true;
		queue_work(system_long_wq, &aop->boot_work);
		ret = 0;
	}
	mutex_unlock(&aop->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(cold_release);

static ssize_t handoff_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct a9_aop *aop = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&aop->lock);
	length = sysfs_emit(buf,
			    "record=%s reason=%s result=%s gen=%u path=%s used=%d phase=%s wrapper=%d\n",
			    aop->ho.seen ? "present" : "missing",
			    a9_ho_reason_names[aop->ho.reason],
			    aop->ho.result ?: "none", aop->ho.generation,
			    a9_ho_path_names[aop->ho.path], aop->ho.adopt,
			    phase_names[aop->phase], aop->wake_wrapper_done);
	mutex_unlock(&aop->lock);
	return length;
}
static DEVICE_ATTR_RO(handoff);

static struct attribute *a9_aop_cold_attrs[] = {
	&dev_attr_cold_release.attr,
	NULL,
};
static const struct attribute_group a9_aop_cold_group = {
	.attrs = a9_aop_cold_attrs,
};

static struct attribute *a9_aop_handoff_attrs[] = {
	&dev_attr_handoff.attr,
	NULL,
};
static const struct attribute_group a9_aop_handoff_group = {
	.attrs = a9_aop_handoff_attrs,
};

static void a9_aop_put_node(void *node)
{
	of_node_put(node);
}

/* ps_spi0 (PMGR 0x801b8), for read-only state logging around the reset. */
static struct device_node *a9_aop_find_spi0_pmgr(void)
{
	struct device_node *np;
	const char *label;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (!of_property_read_string(np, "label", &label) &&
		    !strcmp(label, "spi0"))
			return np;
	}
	return NULL;
}

static void a9_aop_remove_debugfs(void *dir)
{
	debugfs_remove_recursive(dir);
}

static void a9_aop_destroy_wq(void *wq)
{
	destroy_workqueue(wq);
}

static int a9_aop_pool_claim(struct a9_aop *aop, resource_size_t start,
			     resource_size_t size)
{
	struct resource *parent = &iomem_resource, *next;
	unsigned int depth;

	/*
	 * arm64 inserts a no-map "reserved" resource, then the persistent FDT
	 * header reservation adds a BUSY "reserved" child of the same extent.
	 * request_mem_region() cannot descend through that BUSY child. Keep
	 * both reservations and claim a child, only under exact generic nodes.
	 * Any driver claim, System RAM, partial overlap or deep tree is fatal.
	 */
	for (depth = 0; depth < 4; depth++) {
		next = lookup_resource(parent, start);
		if (!next)
			break;
		if (next->start != start || resource_size(next) != size ||
		    resource_type(next) != IORESOURCE_MEM ||
		    (next->flags & IORESOURCE_SYSRAM) ||
		    !next->name || strcmp(next->name, "reserved")) {
			dev_err(aop->dev, "stage3 pool conflicts with %s %pR\n",
				next->name ?: "unnamed", next);
			return -EBUSY;
		}
		parent = next;
	}
	if (!depth || depth == 4)
		return -EBUSY;
	aop->pool_resource = (struct resource) {
		.name = "a9-aop-stage3-pool",
		.start = start,
		.end = start + size - 1,
		.flags = IORESOURCE_MEM | IORESOURCE_BUSY,
	};
	return devm_request_resource(aop->dev, parent, &aop->pool_resource);
}

static int a9_aop_experiment_pool(struct a9_aop *aop)
{
	struct device_node *node;
	struct reserved_mem *rmem;
	struct resource res;
	bool valid;
	int ret;

	node = of_parse_phandle(aop->dev->of_node, "memory-region", 0);
	if (!node)
		return -EINVAL;
	rmem = of_reserved_mem_lookup(node);
	valid = rmem && of_device_is_available(node) &&
		of_property_read_bool(node, "no-map") &&
		!of_find_property(node, "reusable", NULL) &&
		!of_find_property(node, "compatible", NULL) &&
		!of_address_to_resource(node, 0, &res) &&
		rmem->base == A9_EXP_POOL_PA && rmem->size == A9_EXP_POOL_SIZE &&
		res.start == rmem->base && resource_size(&res) == rmem->size;
	of_node_put(node);
	if (!valid)
		return -EINVAL;
	ret = a9_aop_pool_claim(aop, rmem->base, rmem->size);
	if (ret)
		return ret;
	/* Normal non-cacheable mapping, like arm64 coherent DMA; no cached alias. */
	aop->pool = devm_memremap(aop->dev, rmem->base, rmem->size, MEMREMAP_WC);
	if (IS_ERR(aop->pool))
		return PTR_ERR(aop->pool);
	aop->pool_pa = rmem->base;
	aop->pool_size = rmem->size;
	dev_info(aop->dev, "stage3 reserved AFK pool pa=%pap size=0x%zx never freed\n",
		 &aop->pool_pa, aop->pool_size);
	return 0;
}

static bool a9_aop_apps_stopped(struct a9_aop *aop)
{
	unsigned int i;

	for (i = 0; i < aop->neps; i++)
		if (aop->eps[i].state != AFK_OFF &&
		    aop->eps[i].state != AFK_WAIT_INIT &&
		    aop->eps[i].state != AFK_STOPPED)
			return false;
	return true;
}

/* Handoff: the firmware stays resident and asleep. Status registers only;
 * a word left in either FIFO would be the next kernel's first reply.
 */
static int a9_aop_handoff_idle(struct a9_aop *aop)
{
	int ret;

	aop->ho.idle_cpu = aop_rd(aop, AOP_CPU_CONTROL);
	aop->ho.idle_auto_boot = readl(aop->autoboot);
	aop->ho.idle_inbox = aop_rd(aop, AOP_INBOX_CONTROL);
	aop->ho.idle_outbox = aop_rd(aop, AOP_OUTBOX_CONTROL);
	ret = a9_ho_wrapper_idle(aop->ho.idle_auto_boot, aop->ho.idle_inbox,
				 aop->ho.idle_outbox, true) ? 0 : -EBUSY;
	aop_tr_info(aop, "QUIESCE step=mailbox-idle cpu44=%08x auto_boot=%08x in4008=%08x out4020=%08x status=%d halt=skipped\n",
		    aop->ho.idle_cpu, aop->ho.idle_auto_boot, aop->ho.idle_inbox,
		    aop->ho.idle_outbox, ret);
	return a9_ho_step(&aop->ho, A9_HO_STEP_MAILBOX_IDLE, ret);
}

/*
 * iOS AppleSPU_stopEndpoint, one endpoint at a time: empty the outbox, send
 * AFK 0xa4, then service RX inline until that endpoint's 0x87/0xc3. The
 * AOP->AP outbox holds 8 words. At 17:35 seventeen stops went out back to
 * back, slowed by console output, with nothing draining it, and only the
 * first 8 ACKs arrived. With one stop outstanding its ACK always has room.
 * The first missing ACK ends the sequence; no stop is ever sent twice.
 */
static int a9_aop_quiesce_afk(struct a9_aop *aop)
{
	u64 start = ktime_get_boottime_ns(), sent, rx;
	unsigned int i, j, elapsed, limit;
	bool stopped;
	int ret;

	for (i = 0; i < aop->neps; i++) {
		struct a9_aop_ep *e = &aop->eps[i];

		if (e->state < AFK_INIT || e->state > AFK_RUNNING)
			continue;
		elapsed = div_u64(ktime_get_boottime_ns() - start, NSEC_PER_MSEC);
		if (elapsed >= AOP_QUIESCE_AFK_MS) {
			aop_tr_err(aop, "QUIESCE step=afk-budget ep=0x%x elapsed_ms=%u limit_ms=%u\n",
				   e->num, elapsed, AOP_QUIESCE_AFK_MS);
			return a9_ho_step(&aop->ho, A9_HO_STEP_AFK_ACKED, -ETIMEDOUT);
		}
		limit = AOP_QUIESCE_AFK_MS - elapsed;
		if (limit > AOP_QUIESCE_EP_MS)
			limit = AOP_QUIESCE_EP_MS;
		a9_aop_inline_rx(aop);
		mutex_lock(&aop->lock);
		ret = a9_aop_send(aop, e->num, a9_afk_word(A9_AFK_SHUTDOWN));
		if (!ret)
			e->state = AFK_STOPPING;
		mutex_unlock(&aop->lock);
		aop_tr_info(aop, "QUIESCE step=afk-send ep=0x%x status=%d\n", e->num, ret);
		if (ret)
			return a9_ho_step(&aop->ho, A9_HO_STEP_AFK_SENT, ret);
		sent = ktime_get_boottime_ns();
		rx = aop->rx_words;
		/* Inline RX also keeps the watchdog pings going. */
		stopped = a9_aop_wait(aop, READ_ONCE(e->state) == AFK_STOPPED, limit);
		aop_tr_info(aop, "QUIESCE step=afk-ack ep=0x%x state=%s ms=%llu rx=%llu\n",
			    e->num, afk_names[e->state],
			    div_u64(ktime_get_boottime_ns() - sent, NSEC_PER_MSEC),
			    aop->rx_words - rx);
		if (!stopped) {
			aop_tr_err(aop, "QUIESCE step=afk-timeout ep=0x%x limit_ms=%u rx_halt=%d crashed=%d fault=%08x\n",
				   e->num, limit, aop->rx_halt, aop->iop_crashed,
				   aop->fault_control);
			for (j = i + 1; j < aop->neps; j++)
				aop_tr_info(aop, "QUIESCE step=afk-unsent ep=0x%x state=%s\n",
					    aop->eps[j].num, afk_names[aop->eps[j].state]);
			return a9_ho_step(&aop->ho, A9_HO_STEP_AFK_ACKED, -ETIMEDOUT);
		}
	}
	a9_ho_step(&aop->ho, A9_HO_STEP_AFK_SENT, 0);
	if (a9_aop_apps_stopped(aop))
		return a9_ho_step(&aop->ho, A9_HO_STEP_AFK_ACKED, 0);
	/* A failed endpoint was never sent a stop and cannot be quiesced. */
	for (i = 0; i < aop->neps; i++)
		if (aop->eps[i].state != AFK_OFF && aop->eps[i].state != AFK_WAIT_INIT &&
		    aop->eps[i].state != AFK_STOPPED)
			aop_tr_err(aop, "QUIESCE step=afk-unstopped ep=0x%x state=%s\n",
				   aop->eps[i].num, afk_names[aop->eps[i].state]);
	return a9_ho_step(&aop->ho, A9_HO_STEP_AFK_ACKED, -ETIMEDOUT);
}

/* Called only after workers/IRQ are stopped, with inline RX still servicing
 * syslog/IOReport and command replies. No pool/ring allocation is freed.
 * A missing ACK is terminal: never turn a failed quiesce into a forced halt.
 * @halt=false (handoff) stops after the stable-RAM check and mailbox check.
 * Every step lands in the reserved transcript and the step bitmap.
 */
static int a9_aop_quiesce(struct a9_aop *aop, bool halt)
{
	u8 before[SHA256_DIGEST_SIZE], after[SHA256_DIGEST_SIZE];
	unsigned int i;
	u64 word;
	int ret = 0;

	aop_tr_info(aop, "QUIESCE step=begin ready=%d crashed=%d fault=%08x pool_used=0x%zx halt=%d\n",
		    aop->system_ready, aop->iop_crashed, aop->fault_control,
		    aop->pool_used, halt);
	aop->force_halted = false;
	aop->force_drained = false;
	if (!aop->system_ready || aop->rx_halt || aop->iop_crashed ||
	    aop->fault_control)
		return a9_ho_step(&aop->ho, A9_HO_STEP_BEGIN, -EIO);
	a9_ho_step(&aop->ho, A9_HO_STEP_BEGIN, 0);
	if (aop->iio)
		a9_aop_accel_invalidate(aop->iio);
	/* Configure can fail after setting just one rate. Stop any usable accel
	 * channel, even if rate_set never became true.
	 */
	if (aop->accel >= 0)
		ret = a9_aop_set_rate(aop, 0, 0, 0);
	aop_tr_info(aop, "QUIESCE step=rates-zero status=%d accel=%d\n", ret, aop->accel);
	if (a9_ho_step(&aop->ho, A9_HO_STEP_RATES, ret))
		return ret;
	ret = a9_aop_quiesce_afk(aop);
	if (ret)
		return ret;
	for (i = 0; i < aop->neps; i++) {
		if (aop->eps[i].state != AFK_STOPPED &&
		    aop->eps[i].state != AFK_WAIT_INIT)
			continue;
		/* Unsolicited words keep arriving: keep the outbox empty. */
		a9_aop_inline_rx(aop);
		/* iOS b717a4: endpoint bits47:32, low32=1 disables routing.
		 * There is no separate management stop ACK; AFK ACK came first.
		 */
		word = a9_mgmt_word(A9_MGMT_STARTEP,
				    (u64)aop->eps[i].num << 32 | 1);
		mutex_lock(&aop->lock);
		ret = a9_aop_send(aop, 0, word);
		if (!ret && aop->eps[i].state == AFK_WAIT_INIT)
			aop->eps[i].state = AFK_OFF;
		mutex_unlock(&aop->lock);
		aop_tr_info(aop, "QUIESCE step=endpoint-disable ep=0x%x word=%016llx status=%d\n",
			    aop->eps[i].num, word, ret);
		if (ret)
			break;
	}
	if (a9_ho_step(&aop->ho, A9_HO_STEP_DISABLED, ret))
		return ret;
	/* Empty pending software/hardware RX before arming fresh ACK state. */
	a9_aop_inline_rx(aop);
	mutex_lock(&aop->lock);
	aop->shutdown_power_requested = true;
	aop->ap_power_acked = false;
	ret = a9_aop_send(aop, 0, a9_sys_ap_quiesce());
	mutex_unlock(&aop->lock);
	aop_tr_info(aop, "QUIESCE step=ap-send word=%016llx status=%d\n",
		    a9_sys_ap_quiesce(), ret);
	if (a9_ho_step(&aop->ho, A9_HO_STEP_AP_SENT, ret))
		return ret;
	if (!a9_aop_wait(aop, aop->ap_power_acked &&
			aop->ap_power_ack == A9_SYS_AP_QUIESCED, AOP_QUIESCE_EP_MS)) {
		aop_tr_err(aop, "QUIESCE step=ap-timeout seen=%d state=0x%x\n",
			    aop->ap_power_acked, aop->ap_power_ack);
		return a9_ho_step(&aop->ho, A9_HO_STEP_AP_ACKED, -ETIMEDOUT);
	}
	aop_tr_info(aop, "QUIESCE step=ap-ack state=0x%x\n", aop->ap_power_ack);
	a9_ho_step(&aop->ho, A9_HO_STEP_AP_ACKED, 0);
	mutex_lock(&aop->lock);
	aop->iop_power = ~0U;
	ret = a9_aop_send(aop, 0, a9_sys_iop_sleep());
	mutex_unlock(&aop->lock);
	aop_tr_info(aop, "QUIESCE step=iop-send word=%016llx status=%d\n",
		    a9_sys_iop_sleep(), ret);
	if (a9_ho_step(&aop->ho, A9_HO_STEP_IOP_SENT, ret))
		return ret;
	if (!a9_aop_wait(aop, aop->iop_power == A9_SYS_IOP_SLEEP, AOP_QUIESCE_EP_MS)) {
		aop_tr_err(aop, "QUIESCE step=iop-timeout state=0x%x\n", aop->iop_power);
		return a9_ho_step(&aop->ho, A9_HO_STEP_IOP_ACKED, -ETIMEDOUT);
	}
	aop_tr_info(aop, "QUIESCE step=iop-ack state=0x%x\n", aop->iop_power);
	if (aop->rx_halt || aop->iop_crashed || aop->fault_control)
		return a9_ho_step(&aop->ho, A9_HO_STEP_IOP_ACKED, -EIO);
	a9_ho_step(&aop->ho, A9_HO_STEP_IOP_ACKED, 0);
	/* Observe all granted RAM, including system buffers, before RUN clear.
	 * Stable bytes corroborate the ACK; this is not a hardware DMA-idle bit.
	 */
	msleep(20);
	sha256(aop->pool, aop->pool_used, before);
	msleep(20);
	sha256(aop->pool, aop->pool_used, after);
	ret = memcmp(before, after, sizeof(before)) ? -EBUSY : 0;
	aop_tr_info(aop, "QUIESCE step=ram-stable bytes=0x%zx window_ms=20 before=%*phN after=%*phN status=%d\n",
		    aop->pool_used, (int)sizeof(before), before,
		    (int)sizeof(after), after, ret);
	if (a9_ho_step(&aop->ho, A9_HO_STEP_RAM_STABLE, ret))
		return ret;
	aop->shutdown_quiesced = true;
	memcpy(aop->ho.pool_sha256, after, sizeof(after));
	if (!halt)
		return a9_aop_handoff_idle(aop);
	ret = a9_aop_force_stop(aop);
	aop_tr_info(aop, "QUIESCE step=halt status=%d cpu44=%08x\n",
		    ret, aop->force_halt_control);
	if (a9_ho_step(&aop->ho, A9_HO_STEP_HALTED, ret))
		return ret;
	ret = a9_aop_force_drain(aop);
	aop_tr_info(aop, "QUIESCE step=drain status=%d inbox=%u outbox=%u pool=retained\n",
		    ret, aop->stale_inbox, aop->stale_outbox);
	return a9_ho_step(&aop->ho, A9_HO_STEP_DRAINED, ret);
}

static void a9_aop_shutdown(struct platform_device *pdev)
{
	struct a9_aop *aop = platform_get_drvdata(pdev);
	unsigned int i;
	bool streaming;
	int ret = 0;

	if (!aop)
		return;
	mutex_lock(&aop->lock);
	WRITE_ONCE(aop->stopping, true);
	mutex_unlock(&aop->lock);
	wake_up_all(&aop->wait);
	cancel_work_sync(&aop->boot_work);
	if (aop->irq_enabled)
		disable_irq(aop->irq);
	cancel_delayed_work_sync(&aop->poll_work);
	cancel_delayed_work_sync(&aop->ping_work);
	flush_workqueue(aop->wq);
	/* Survives the kexec in reserved RAM, whatever the console loglevel. */
	a9_aop_slog_begin(aop);
	if (!aop->hello) {
		if (quiesce || handoff)
			aop_tr_info(aop, "QUIESCE step=skipped reason=no-owned-handshake phase=%s result=%s\n",
				    phase_names[aop->phase], aop->result ?: "none");
		/* This boot consumed a record or loaded firmware: certify failure. */
		if (handoff && (aop->cold_triggered || aop->ho.adopt))
			a9_aop_handoff_write(aop, -ENOTCONN);
		if (aop->running_attach)
			dev_warn(aop->dev, "inherited AOP was not adopted or stopped; kexec is unsafe, use a real power cycle\n");
		return;
	}
	if (aop->experiment_stage &&
	    (aop->experiment_stage == 2 || READ_ONCE(aop->rx_halt))) {
		if (quiesce || handoff)
			aop_tr_err(aop, "QUIESCE step=skipped reason=transport-failed phase=%s result=%s crashed=%d; recover with DFU\n",
				    phase_names[aop->phase], aop->result ?: "none",
				    aop->iop_crashed);
		if (handoff)
			a9_aop_handoff_write(aop, -EIO);
		return; /* No retry, reset or further traffic after an experiment failure. */
	}

	/*
	 * The rings live in memory the next (kexec) kernel will reuse: stop
	 * the stream and the endpoints so the AOP stops writing to them.
	 */
	aop->inline_rx = true;
	mutex_lock(&aop->lock);
	streaming = aop->accel >= 0 && aop->rate_set;
	aop->phase = PHASE_STOPPED;
	mutex_unlock(&aop->lock);
	if (quiesce || handoff) {
		ret = a9_aop_quiesce(aop, quiesce);
		aop_tr_info(aop, "QUIESCE step=complete status=%d quiesced=%d halted=%d drained=%d%s steps=0x%x failed_step=%s errno=%u\n",
			    ret, aop->shutdown_quiesced, aop->force_halted,
			    aop->force_drained, handoff ? " handoff=1" : "",
			    aop->ho.steps, aop->ho.failed_errno ?
			    a9_ho_step_names[aop->ho.failed_step] : "none",
			    aop->ho.failed_errno);
		if (handoff)
			ret = a9_aop_handoff_write(aop, ret);
		if (ret)
			aop_tr_err(aop, "%s failed; shutdown cannot veto kexec; do not wake or release the next AOP, recover with DFU\n",
				    handoff ? "HANDOFF" : "QUIESCE");
		aop_tr_info(aop, "STAGE3 checkpoint=%s\n", handoff ?
			    (ret ? "shutdown-handoff-failed" : "shutdown-handoff-complete") :
			    (ret ? "shutdown-quiesce-failed" : "shutdown-quiesce-complete"));
		a9_aop_stage3_mark(aop, handoff ?
				   (ret ? "shutdown-handoff-failed" : "shutdown-handoff-complete") :
				   (ret ? "shutdown-quiesce-failed" : "shutdown-quiesce-complete"));
		return;
	}
	if (streaming)
		ret = a9_aop_set_rate(aop, 0, 0, 0);
	/* One stop outstanding at a time, as in a9_aop_quiesce_afk(), so the
	 * 8-word outbox never drops an ACK; best effort, every endpoint asked.
	 */
	for (i = 0; i < aop->neps; i++) {
		struct a9_aop_ep *e = &aop->eps[i];
		bool sent;

		if (e->state < AFK_INIT || e->state > AFK_RUNNING)
			continue;
		a9_aop_inline_rx(aop);
		mutex_lock(&aop->lock);
		sent = !a9_aop_send(aop, e->num, a9_afk_word(A9_AFK_SHUTDOWN));
		if (sent)
			e->state = AFK_STOPPING;
		mutex_unlock(&aop->lock);
		if (sent && !a9_aop_wait(aop, READ_ONCE(e->state) == AFK_STOPPED, AOP_STOP_MS))
			dev_warn(aop->dev, "ep %u: no shutdown ack; AOP may still own its rings\n",
				 e->num);
	}
	dev_info(aop->dev, "stopped (rate stop %d)\n", ret);
}

static int a9_aop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a9_aop *aop;
	struct resource *res;
	const char *label;
	unsigned int i;
	unsigned int stage = apple_a9_aop_attach_stage();
	int ret;

	/* No resource is touched unless explicitly enabled on this board. */
	if ((!active && stage < 2) || !of_machine_is_compatible("apple,j71t"))
		return -ENODEV;
	if (!a9_aop_cold_test_valid(cold_test) || (cold_test && !force_cold))
		return dev_err_probe(dev, -EINVAL,
				     "invalid aop_cold_test or forced cold missing\n");
	if (cold_run > 2 || (cold_run &&
	    (!force_cold || stage != 3 || cold_defer != 1 || cold_test != 5)))
		return dev_err_probe(dev, -EINVAL,
				     "aop_run=1/2 needs forced cold stage3, defer1 and test5\n");
	if (quiesce && !cold_run)
		return dev_err_probe(dev, -EINVAL, "aop_quiesce requires aop_run=1/2\n");
	if (handoff_mode && strcmp(handoff_mode, "sleep"))
		return dev_err_probe(dev, -EINVAL, "aop_handoff accepts only sleep\n");
	handoff = !!handoff_mode;
	if (handoff && (quiesce || !cold_run))
		return dev_err_probe(dev, -EINVAL,
				     "aop_handoff=sleep needs aop_run=1/2 and excludes aop_quiesce=1\n");
	if (spi0_reset && !cold_run)
		return dev_err_probe(dev, -EINVAL, "aop_spi0_reset needs aop_run=1/2\n");
	if (wake_wrapper && !handoff)
		return dev_err_probe(dev, -EINVAL, "aop_wake_wrapper needs aop_handoff=sleep\n");
	if (cold_defer > 2 || (cold_defer && !force_cold) ||
	    (cold_defer == 2 && (stage != 2 ||
				 (cold_test & (A9_COLD_UNCACHED | A9_COLD_ECID)) !=
				 (A9_COLD_UNCACHED | A9_COLD_ECID))))
		return dev_err_probe(dev, -EINVAL,
				     "aop_defer needs forced cold; fence needs stage2, cache off and ECID\n");
	if (force_cold && (stage < 2 || stage > 3 || wake || cycle || active ||
			   apple_a9_aop_attach()))
		return dev_err_probe(dev, -EINVAL,
				     "aop_force_cold requires stage 2/3, other active modes off\n");
	if (wake && (stage < 2 || wake > A9_EXP_WAKE_COMBINED || cycle))
		return dev_err_probe(dev, -EINVAL, "wake=1..5 requires stage 2/3 and cycle=0\n");
	if (apple_a9_aop_ring_mode() || apple_a9_aop_state_probe() ||
	    stage == 1 || stage > 3 ||
	    (stage && (active || apple_a9_aop_debug() || apple_a9_aop_attach())))
		return dev_err_probe(dev, -EBUSY, "read-only diagnostics exclude active AOP\n");
	aop = devm_kzalloc(dev, sizeof(*aop), GFP_KERNEL);
	if (!aop)
		return -ENOMEM;
	aop->dev = dev;
	aop->experiment_stage = stage;
	aop->cold_run = cold_run;
	aop->accel = -1;
	aop->poll_ms = 10;
	aop->phase = PHASE_PROBE;
	spin_lock_init(&aop->tx_lock);
	spin_lock_init(&aop->rx_lock);
	INIT_KFIFO(aop->rxq);
	mutex_init(&aop->lock);
	init_waitqueue_head(&aop->wait);
	INIT_WORK(&aop->rx_work, a9_aop_rx_work);
	INIT_WORK(&aop->boot_work, a9_aop_boot_work);
	INIT_DELAYED_WORK(&aop->poll_work, a9_aop_poll_work);
	INIT_DELAYED_WORK(&aop->ping_work, a9_aop_ping_work);

	/* The observer owns 4-byte sub-ranges of the wrapper: map, don't claim. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wrapper");
	if (!res || resource_size(res) != AOP_WRAPPER_SIZE)
		return -EINVAL;
	aop->wrapper = devm_ioremap(dev, res->start, resource_size(res));
	if (!aop->wrapper)
		return -ENOMEM;
	aop->wrapper_pa = res->start;
	if (spi0_reset) {
		/* Mapped only: nothing touches SPI0 before a live AOP is halted. */
		if (aop->wrapper_pa != A9_SPI0_WRAPPER_PA)
			return dev_err_probe(dev, -EINVAL, "aop_spi0_reset: unexpected wrapper\n");
		aop->spi0.base = devm_ioremap(dev, A9_SPI0_PA, A9_SPI0_SIZE);
		if (!aop->spi0.base)
			return -ENOMEM;
		aop->spi0_pmgr = a9_aop_find_spi0_pmgr();
		if (aop->spi0_pmgr) {
			ret = devm_add_action_or_reset(dev, a9_aop_put_node,
						       aop->spi0_pmgr);
			if (ret)
				return ret;
		}
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "region");
	if (!res)
		return -EINVAL;
	aop->region_pa = res->start;
	aop->region_size = resource_size(res);
	if (force_cold) {
		struct device_node *node;
		struct reserved_mem *rmem;
		bool valid;

		node = of_find_node_by_path("/reserved-memory/aop-firmware@210e00000");
		rmem = node ? of_reserved_mem_lookup(node) : NULL;
		valid = rmem && of_device_is_available(node) &&
			of_property_read_bool(node, "no-map") &&
			!of_find_property(node, "reusable", NULL) &&
			!of_find_property(node, "compatible", NULL) &&
			rmem->base == 0x210e00000ULL && rmem->size == 0x9b000 &&
			res->start == rmem->base && resource_size(res) == rmem->size;
		of_node_put(node);
		if (!valid)
			return dev_err_probe(dev, -EINVAL,
					     "cold firmware needs exact no-map reservation\n");
		aop->region = devm_ioremap(dev, res->start, resource_size(res));
		if (!aop->region)
			return -ENOMEM;
	}
	if (!stage) {
		aop->region = devm_ioremap_resource(dev, res);
		if (IS_ERR(aop->region))
			return PTR_ERR(aop->region);
		aop->autoboot = devm_platform_ioremap_resource_byname(pdev, "auto-boot");
		if (IS_ERR(aop->autoboot))
			return PTR_ERR(aop->autoboot);
	}
	if (force_cold || wake == A9_EXP_WAKE_AUTOBOOT || wake == A9_EXP_WAKE_COMBINED) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "auto-boot");
		if (!res || res->start != 0x210000600ULL || resource_size(res) != 4)
			return -EINVAL;
		aop->autoboot = devm_ioremap_resource(dev, res);
		if (IS_ERR(aop->autoboot))
			return PTR_ERR(aop->autoboot);
	}
	if (stage == 3 || force_cold) {
		ret = a9_aop_experiment_pool(aop);
		if (ret)
			return dev_err_probe(dev, ret,
					     "stage3 requires dedicated no-map AFK pool\n");
	}
	if (quiesce || handoff) {
		/* The tail page (record + shutdown transcript) is excluded from
		 * the allocator before ext or any grant exists.
		 */
		aop->pool_size -= A9_HO_PAGE;
		aop->ho.slot = (struct a9_ho_record *)((u8 *)aop->pool + aop->pool_size);
		aop->ho.log = (struct a9_sl_header *)((u8 *)aop->ho.slot + A9_HO_LOG_OFFSET);
		aop->ho.log_text = (char *)(aop->ho.log + 1);
		get_random_bytes(aop->ho.writer, sizeof(aop->ho.writer));
		dev_info(dev, "SHUTDOWN-LOG writer=%*phN tail_offset=0x%zx; transcript survives kexec in reserved RAM\n",
			 (int)sizeof(aop->ho.writer), aop->ho.writer, aop->pool_size);
		a9_aop_slog_replay(aop);
	}
	if (handoff) {
		bool magic = READ_ONCE(aop->ho.slot->magic) == A9_HO_MAGIC;
		/* AON auto-boot: 1 on every warm kexec capture, 0 after DFU. DRAM
		 * can keep an old record across DFU; with no live instance it is
		 * stale and must not block the first cold start.
		 */
		u32 auto_boot = readl(aop->autoboot);

		aop->ho.seen = magic && auto_boot == 1;
		aop->ho.reason = aop->ho.seen ? A9_HO_OK : magic ? A9_HO_WRAPPER : A9_HO_MISSING;
		aop->ho.result = aop->ho.seen ? "pending-validation" :
				 magic ? "stale-after-cold-boot" : "no-record";
		dev_info(dev, "HANDOFF probe magic=%d auto_boot=%08x result=%s\n",
			 magic, auto_boot, aop->ho.result);
	}
	if (of_property_read_u32(dev->of_node, "apple,aop-target", &aop->aop_target) ||
	    of_property_read_u32(dev->of_node, "apple,chip-id", &aop->chip_id) ||
	    of_property_read_u32(dev->of_node, "apple,chip-revision",
				 &aop->chip_revision))
		return -EINVAL;

	if (of_count_phandle_with_args(dev->of_node, "apple,observe-domains",
				       NULL) != AOP_DOMAINS)
		return -EINVAL;
	for (i = 0; i < AOP_DOMAINS; i++) {
		aop->domains[i] = of_parse_phandle(dev->of_node,
						   "apple,observe-domains", i);
		if (!aop->domains[i])
			return -EINVAL;
		ret = devm_add_action_or_reset(dev, a9_aop_put_node, aop->domains[i]);
		if (ret)
			return ret;
		if (of_property_read_string(aop->domains[i], "label", &label) ||
		    strcmp(label, domain_names[i]))
			return -EINVAL;
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));
	if (ret)
		return ret;
	if (device_iommu_mapped(dev))
		return -EINVAL;	/* 0xa1 carries an AP physical address */

	/* Only the RX (outbox not empty) line; TX space is polled. */
	if (!stage) {
		aop->irq = platform_get_irq_byname(pdev, "rx");
		if (aop->irq < 0)
			return aop->irq;
		ret = devm_request_irq(dev, aop->irq, a9_aop_irq, IRQF_NO_AUTOEN,
				       dev_name(dev), aop);
		if (ret)
			return ret;
	}

	/* Registered before workqueue/debugfs cleanup: readers and work stop first. */
	if (force_cold) {
		ret = devm_add_action_or_reset(dev, a9_aop_cold_free, aop);
		if (ret)
			return ret;
	}
	aop->wq = alloc_ordered_workqueue("apple-a9-aop", WQ_MEM_RECLAIM);
	if (!aop->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, a9_aop_destroy_wq, aop->wq);
	if (ret)
		return ret;

	if (apple_a9_aop_debug() || stage) {
		aop->debugfs = debugfs_create_dir("apple-a9-aop-active", NULL);
		if (!IS_ERR(aop->debugfs)) {
			ret = devm_add_action_or_reset(dev, a9_aop_remove_debugfs,
						       aop->debugfs);
			if (ret)
				return ret;
			debugfs_create_file("state", 0400, aop->debugfs, aop,
					    &a9_aop_state_fops);
			if (cold_run) {
				debugfs_create_file("system_buffers", 0400, aop->debugfs, aop,
						    &a9_aop_system_buffers_fops);
				debugfs_create_file("crashlog", 0400, aop->debugfs, aop,
						    &a9_aop_crashlog_fops);
			}
			if (quiesce || handoff)
				debugfs_create_file("previous_shutdown", 0400, aop->debugfs,
						    aop, &a9_aop_previous_shutdown_fops);
			debugfs_create_file("registers", 0400, aop->debugfs, aop,
					    &a9_aop_registers_fops);
			debugfs_create_file("transcript", 0400, aop->debugfs, aop,
					    &a9_aop_transcript_fops);
			debugfs_create_file("startup_transcript", 0400, aop->debugfs, aop,
					    &a9_aop_startup_transcript_fops);
			if (stage == 3)
				debugfs_create_file("samples", 0400, aop->debugfs, aop,
						    &a9_aop_samples_fops);
			debugfs_create_file("rings", 0400, aop->debugfs, aop,
					    &a9_aop_rings_fops);
			debugfs_create_file("patchbay", 0400, aop->debugfs, aop,
					    &a9_aop_patchbay_fops);
			if (force_cold) {
				debugfs_create_file("cold_memory", 0400, aop->debugfs, aop,
						    &a9_aop_cold_memory_fops);
				debugfs_create_file("cold_before", 0400, aop->debugfs, aop,
						    &a9_aop_cold_before_fops);
				debugfs_create_file("cold_after", 0400, aop->debugfs, aop,
						    &a9_aop_cold_after_fops);
			}
			debugfs_create_file("fingerprint", 0400, aop->debugfs, aop,
					    &a9_aop_fingerprint_fops);
		}
	}
	if (spi0_reset || wake_wrapper)
		dev_info(dev, "EXPERIMENT spi0_reset=%d wake_wrapper=%d setactive=not-implemented automatic_fallback=none spi0_applies=%s spi0_pmgr=%s\n",
			 spi0_reset, wake_wrapper,
			 !spi0_reset ? "off" : handoff ? "never(handoff-refuses-live-reload)" :
			 "live-reload", aop->spi0_pmgr ? "ps_spi0" : "absent");
	if (spi0_reset)
		dev_info(dev, "SPI0 evidence prev=%s prev_hello=%d prev_halted=%d prev_steps=0x%x prev_carry=%d: decided at the trigger with the boot auto-boot and +0x4020\n",
			 a9_spi0_prev_name(aop->spi0.prev.state),
			 aop->spi0.prev.hello, aop->spi0.prev.halted,
			 aop->spi0.prev.steps, aop->spi0.prev.carried);
	platform_set_drvdata(pdev, aop);
	if (cold_defer) {
		ret = devm_device_add_group(dev, &a9_aop_cold_group);
		if (ret)
			return ret;
		if (handoff) {
			ret = devm_device_add_group(dev, &a9_aop_handoff_group);
			if (ret)
				return ret;
			dev_info(dev, "HANDOFF mode=sleep record=%s writer=%*phN pool_capacity=0x%zx record_offset=0x%zx\n",
				 aop->ho.seen ? "present" : "missing",
				 (int)sizeof(aop->ho.writer), aop->ho.writer,
				 aop->pool_size, aop->pool_size);
			if (aop->ho.seen) {
				/* Validate, then wake without halt/reload; never cold load. */
				queue_work(system_long_wq, &aop->boot_work);
				return 0;
			}
		}
		dev_info(dev, "COLD deferred mode=%u: awaiting cold_release=1 in sysfs; no stop/load/release at probe\n",
			 cold_defer);
		return 0;
	}
	/* Firmware lives on the root filesystem: decide and boot later. */
	queue_work(system_long_wq, &aop->boot_work);
	dev_info(dev, "active AOP driver armed (debugfs %s)\n",
		 aop->debugfs ? "on" : "off");
	return 0;
}

static const struct of_device_id a9_aop_active_match[] = {
	{ .compatible = "apple,j71t-aop" },
	{}
};

static struct platform_driver a9_aop_active_driver = {
	.probe = a9_aop_probe,
	.shutdown = a9_aop_shutdown,
	.driver = {
		.name = "apple-a9-aop-active",
		.of_match_table = a9_aop_active_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(a9_aop_active_driver);

MODULE_FIRMWARE(AOP_MAIN_FW);
MODULE_FIRMWARE(AOP_EXT_FW);
MODULE_FIRMWARE(AOP_ECID_FW);
MODULE_FIRMWARE(AOP_CAL_FW);
MODULE_DESCRIPTION("Apple J71t AOP active driver (RTBuddy, SPU/AFK, accelerometer)");
MODULE_LICENSE("GPL");
