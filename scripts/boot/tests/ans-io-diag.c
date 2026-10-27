/* Production rescue store and snapshot; assert host mode never crosses PCIe. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t dma_addr_t;
#define READ_ONCE(x) (x)
#define le16_to_cpu(x) (x)
#define NVMEQ_ENABLED 0
#define NVMEQ_POLLED 3
#define NVME_REG_CSTS 28
#define NVME_REG_CC 20
#define NVME_REG_AQA 36
#define NVME_REG_ASQ 40
#define NVME_REG_ACQ 48
#define NVME_REG_INTMS 12
struct device { void *data; };
struct device_attribute { int unused; };
struct nvme_completion { u16 status, command_id, sq_head; };
struct nvme_queue {
    struct nvme_completion *cqes;
    unsigned q_depth; u16 cq_head, sq_tail, last_sq_tail, cq_vector;
    unsigned cq_phase; unsigned long flags;
    dma_addr_t sq_dma_addr, cq_dma_addr;
};
struct nvme_ctrl {
    unsigned queue_count;
    struct { int (*reg_read32)(struct nvme_ctrl *, u32, u32 *); } *ops;
};
struct nvme_dev {
    struct nvme_ctrl ctrl;
    struct device *dev; void *dma_ops;
    struct nvme_queue *queues;
    unsigned online_queues, num_vecs;
    unsigned long apple_ans_flags;
    int shutdown_lock;
    char *bar;
};
static int nvme_pci_apple_ans_ops, logs, host_dumps, bar_reads, msi_reads, guarded_reads;
static bool busy, enabled = true, fault;
static struct nvme_dev *to_nvme_dev(void *c) { return c; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void log_values(const char *fmt, ...) { logs++; }
#define dev_info(dev, fmt, ...) log_values(fmt, ##__VA_ARGS__)
static bool test_bit(unsigned b, unsigned long *p) { return !!(*p & (1UL << b)); }
static unsigned nvme_ctrl_state(struct nvme_ctrl *c) { return 0; }
static void apple_ans_host_dump(struct device *d, const char *why) { host_dumps++; }
static bool mutex_trylock(int *l) { if (busy) return false; *l = 1; return true; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static bool sysfs_streq(const char *a, const char *b) { return strcspn(a, "\n") == strlen(b) && !strncmp(a, b, strlen(b)); }
static void *to_pci_dev(struct device *d) { return d; }
static bool pci_is_enabled(void *p) { return enabled; }
static int pci_irq_vector(void *p, unsigned v) { assert(enabled); return 19; }
static unsigned kstat_irqs_usr(unsigned irq) { assert(irq == 19); return 5; }
static u32 readl(void *p) { bar_reads++; return 0; }
static uint64_t lo_hi_readq(void *p) { bar_reads++; return 0; }
static void nvme_pci_apple_ans_log_msi(struct nvme_dev *d) { msi_reads++; }
static int read_csts(struct nvme_ctrl *c, u32 reg, u32 *v) {
    assert(host_dumps); guarded_reads++; *v = 1; return fault ? -EIO : 0;
}
#include "production.h"
int main(void) {
    char bar[0x2000];
    struct nvme_completion cq[2] = {{.status = 1, .command_id = 7}};
    struct nvme_queue q = {.cqes = cq, .q_depth = 2, .flags = 1, .cq_phase = 1};
    typeof(*((struct nvme_ctrl *)0)->ops) ops = {.reg_read32 = read_csts};
    struct device parent = {0};
    struct nvme_dev d = {.ctrl = {.queue_count = 1, .ops = &ops}, .dev = &parent,
        .dma_ops = &nvme_pci_apple_ans_ops, .queues = &q, .bar = bar};
    struct device attrdev = {.data = &d.ctrl};
    assert(apple_ans_diag_store(&attrdev, NULL, "host\n", 5) == 5);
    assert(host_dumps == 1 && !guarded_reads && !bar_reads && !msi_reads);
    assert(apple_ans_diag_store(&attrdev, NULL, "endpoint\n", 9) == 9);
    assert(guarded_reads == 1 && bar_reads && msi_reads == 1);
    int prev = bar_reads;
    busy = true;
    assert(apple_ans_diag_store(&attrdev, NULL, "host", 4) == -EBUSY);
    assert(host_dumps == 3 && bar_reads == prev);
    busy = false; fault = true;
    assert(apple_ans_diag_store(&attrdev, NULL, "endpoint", 8) == -EIO && bar_reads == prev);
    /* Disabled queues do not ask PCI core for a freed IRQ vector. */
    enabled = false; q.flags = 0;
    assert(apple_ans_diag_store(&attrdev, NULL, "endpoint", 8) == -ENODEV && bar_reads == prev);
    assert(apple_ans_diag_store(&attrdev, NULL, "bad", 3) == -EINVAL);
    d.dma_ops = NULL;
    assert(apple_ans_diag_store(&attrdev, NULL, "host", 4) == -EOPNOTSUPP);
    assert(!d.shutdown_lock);
    puts("PASS sysfs: host-only queue/IRQ evidence, explicit endpoint, busy lock, guarded fault, disabled endpoint, invalid input and non-ANS exclusion");
}
