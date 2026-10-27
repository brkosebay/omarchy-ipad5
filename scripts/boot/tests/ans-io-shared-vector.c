/*
 * Production nvme_irq()/vector drain/rescue against a model ANS controller
 * whose one MSI is sent only when the vector's aggregated interrupt state
 * rises 0 -> 1 (it stays 1 while any CQ on the vector holds an entry the host
 * has not acknowledged with a doorbell). That is the mechanism proposed for
 * the 2026-10-26 warm-boot NVMe crawl (docs/boot/ds-kexec-nvme-crawl.md).
 * The legacy per-queue dispatch loses the vector in this model; the fix must
 * not. The model is an assumption, not device evidence.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define NVMEQ_ENABLED 0
#define NVMEQ_POLLED 3
#define ANS_VECTOR_DRAINED 4
#define NVME_QUIRK_SINGLE_VECTOR (1UL << 1)
#define NVME_CTRL_LIVE 1
#define NVME_CTRL_RESETTING 2
#define BITS_PER_LONG 64
#define BIT(n) (1UL << (n))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define le16_to_cpu(x) (x)
#define unlikely(x) (x)
#define dma_rmb() ((void)0)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#include <stddef.h>
static int infos, warns;
static void note(const char *fmt, ...) { (void)fmt; }
#define dev_info(d, ...) (infos++, note(__VA_ARGS__))
#define dev_warn_ratelimited(d, ...) (warns++, note(__VA_ARGS__))
#define DEFINE_IO_COMP_BATCH(n) struct io_comp_batch n = {0}

struct device { int unused; };
struct pci_dev { int unused; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
typedef struct { int held; } spinlock_t;
struct io_comp_batch { int req_list; };
struct nvme_completion { u16 command_id; u16 status; };
#define DEPTH 8
struct nvme_queue {
    struct nvme_dev *dev; unsigned long flags;
    struct nvme_completion *cqes; u32 q_depth;
    u16 cq_head, cq_vector, qid; u8 cq_phase;
    spinlock_t cq_poll_lock;
};
struct nvme_ctrl { unsigned queue_count; unsigned long quirks; int state; };
struct nvme_dev {
    struct nvme_queue *queues; struct nvme_ctrl ctrl; struct device *dev;
    const void *dma_ops; unsigned long apple_ans_flags; unsigned online_queues;
    struct delayed_work ans_irq_rescue; u32 ans_irq_rescue_mark[2];
};
static const int nvme_pci_apple_ans_ops;
static int use_threaded_interrupts;
static unsigned int apple_ans_irq_rescue_ms;
static struct pci_dev pci;
static int irq_disabled, scheduled, completed, batched, msi_count;

/* Model controller: producer tail/phase and acknowledged head per CQ. */
static struct nvme_completion cq[2][DEPTH];
static struct { u16 tail, db; u8 phase; } hw[2];
static bool is_asserted, msi_pending;
static int inject_on_handle = -1, inject_on_doorbell = -1, inject_target;

static bool all_acked(void) { return hw[0].tail == hw[0].db && hw[1].tail == hw[1].db; }
static void post(int q, u16 cid)
{
    cq[q][hw[q].tail] = (struct nvme_completion){.command_id = cid, .status = hw[q].phase};
    if (++hw[q].tail == DEPTH) { hw[q].tail = 0; hw[q].phase ^= 1; }
    if (!is_asserted) { is_asserted = true; msi_pending = true; msi_count++; }
}
static void nvme_handle_cqe(struct nvme_queue *q, struct io_comp_batch *iob, u16 idx)
{
    completed++;
    if (iob) { iob->req_list++; batched++; }
    if (inject_on_handle == q->qid) { inject_on_handle = -1; post(inject_target, 0x77); }
}
static void nvme_ring_cq_doorbell(struct nvme_queue *q)
{
    if (inject_on_doorbell == q->qid) { inject_on_doorbell = -1; post(inject_target, 0x88); }
    hw[q->qid].db = q->cq_head;
    if (all_acked()) is_asserted = false;
}
static bool rq_list_empty(int *l) { return !*l; }
static void nvme_pci_complete_batch(struct io_comp_batch *iob) { iob->req_list = 0; }
static bool test_bit(unsigned b, unsigned long *p) { return *p & (1UL << b); }
static void set_bit(unsigned b, unsigned long *p) { *p |= 1UL << b; }
static bool test_and_clear_bit(unsigned b, unsigned long *p) { bool o = test_bit(b, p); *p &= ~(1UL << b); return o; }
static struct pci_dev *to_pci_dev(struct device *d) { return &pci; }
static int pci_irq_vector(struct pci_dev *p, unsigned v) { return 69; }
static unsigned kstat_irqs_usr(unsigned irq) { return msi_count; }
static void disable_irq(int irq) { irq_disabled++; }
static void enable_irq(int irq) { assert(irq_disabled > 0); irq_disabled--; }
static void spin_lock(spinlock_t *l) { assert(!l->held); l->held = 1; }
static void spin_unlock(spinlock_t *l) { assert(l->held); l->held = 0; }
static int nvme_ctrl_state(struct nvme_ctrl *c) { return c->state; }
static unsigned long msecs_to_jiffies(unsigned ms) { return ms; }
static bool schedule_delayed_work(struct delayed_work *w, unsigned long d) { scheduled++; return true; }
#include "production.h"

static struct nvme_queue qs[2];
static struct nvme_dev dev;
static struct device device;

static void reset(void)
{
    memset(cq, 0, sizeof(cq)); memset(hw, 0, sizeof(hw)); memset(qs, 0, sizeof(qs));
    for (int i = 0; i < 2; i++) {
        hw[i].phase = 1;
        qs[i] = (struct nvme_queue){.dev = &dev, .flags = BIT(NVMEQ_ENABLED), .cqes = cq[i],
                                    .q_depth = DEPTH, .qid = i, .cq_phase = 1};
    }
    dev = (struct nvme_dev){.queues = qs, .ctrl = {.queue_count = 2,
            .quirks = NVME_QUIRK_SINGLE_VECTOR, .state = NVME_CTRL_LIVE},
            .dev = &device, .dma_ops = &nvme_pci_apple_ans_ops, .online_queues = 2};
    is_asserted = msi_pending = false;
    inject_on_handle = inject_on_doorbell = -1;
    completed = batched = 0;
}
static bool pending(int q) { return nvme_cqe_pending(&qs[q]); }

/* One shared IRQ: q0's action then q1's, as registered by nvme-pci. */
static void deliver(bool legacy)
{
    while (msi_pending) {
        msi_pending = false;
        for (int i = 0; i < 2; i++) {
            if (legacy) {
                struct io_comp_batch iob = {0};
                nvme_poll_cq(&qs[i], &iob);
            } else {
                nvme_irq(69, &qs[i]);
            }
        }
    }
}

int main(void)
{
    /* Race A: an admin CQE lands while the I/O action handles its CQ. */
    reset(); inject_on_handle = 1; inject_target = 0;
    post(1, 0x20); deliver(true);
    assert(pending(0) && is_asserted && !msi_pending);
    post(1, 0x21);
    assert(!msi_pending && pending(0) && pending(1));     /* legacy: vector silent */

    /* Timeout drain of every CQ restarts the vector; nvme_irq reports it. */
    nvme_apple_ans_drain_vector(&qs[1], "timeout");
    assert(!pending(0) && !pending(1) && !is_asserted && !irq_disabled && warns == 1);
    assert(!qs[0].cq_poll_lock.held && !qs[1].cq_poll_lock.held);
    assert(test_bit(ANS_VECTOR_DRAINED, &dev.apple_ans_flags));
    post(1, 0x22); assert(msi_pending); deliver(false);
    assert(!pending(1) && !is_asserted && !test_bit(ANS_VECTOR_DRAINED, &dev.apple_ans_flags));
    assert(infos == 1);

    /* Same race with the production handler: nothing left behind. */
    reset(); inject_on_handle = 1; inject_target = 0;
    post(1, 0x20); deliver(false);
    assert(!pending(0) && !pending(1) && !is_asserted && completed == 2);
    assert(batched == 1);                    /* admin CQE completes inline */
    post(1, 0x21); assert(msi_pending); deliver(false); assert(!pending(1));

    /* Race B: a CQE lands just before the doorbell write takes effect. */
    reset(); inject_on_doorbell = 1; inject_target = 1;
    post(1, 0x30); deliver(true);
    assert(pending(1) && is_asserted && !msi_pending);    /* legacy: lost */
    reset(); inject_on_doorbell = 1; inject_target = 1;
    post(1, 0x30); deliver(false);
    assert(!pending(1) && !is_asserted && completed == 2); /* rescan caught it */

    /* Gates: other controllers, threaded IRQs and disabled queues. */
    reset(); dev.dma_ops = NULL; post(0, 1); post(1, 2);
    assert(nvme_irq(69, &qs[1]) == IRQ_HANDLED && pending(0) && !pending(1));
    reset(); use_threaded_interrupts = 1; post(0, 1); post(1, 2);
    nvme_irq(69, &qs[1]); assert(pending(0) && !pending(1));
    use_threaded_interrupts = 0;
    reset(); post(0, 1); post(1, 2); qs[0].flags = 0;
    assert(nvme_irq(69, &qs[1]) == IRQ_HANDLED && pending(0) && !pending(1));
    assert(nvme_irq(69, &qs[1]) == IRQ_NONE);

#ifdef HAVE_RESCUE
    /* Rescue: off by default; drains on the second tick at the same head. */
    reset(); scheduled = 0; nvme_apple_ans_irq_rescue_start(&dev); assert(!scheduled);
    apple_ans_irq_rescue_ms = 200; nvme_apple_ans_irq_rescue_start(&dev); assert(scheduled == 1);
    inject_on_handle = 1; inject_target = 0; post(1, 0x20); deliver(true);
    assert(pending(0));
    nvme_apple_ans_irq_rescue_work(&dev.ans_irq_rescue.work);
    assert(pending(0) && scheduled == 2);                 /* first sighting */
    nvme_apple_ans_irq_rescue_work(&dev.ans_irq_rescue.work);
    assert(!pending(0) && !is_asserted && scheduled == 3 && !irq_disabled);
    post(0, 0x40);                                       /* fresh CQE: no drain */
    nvme_apple_ans_irq_rescue_work(&dev.ans_irq_rescue.work);
    assert(pending(0) && dev.ans_irq_rescue_mark[0]);
    deliver(false);
    nvme_apple_ans_irq_rescue_work(&dev.ans_irq_rescue.work);
    assert(!dev.ans_irq_rescue_mark[0] && scheduled == 5);
    dev.ctrl.state = NVME_CTRL_RESETTING;
    nvme_apple_ans_irq_rescue_work(&dev.ans_irq_rescue.work);
    assert(scheduled == 5);                              /* stops; reset re-arms */
    puts("PASS rescue: off by default, drains a stuck CQE on the second tick, stops outside LIVE");
#endif
    puts("PASS shared vector: legacy dispatch loses the model vector (races A and B); "
         "nvme_irq services every CQ and rescans; timeout drain restarts it; gates hold");
    return 0;
}
