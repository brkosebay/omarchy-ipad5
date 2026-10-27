/* Production stop/reset, CSTS guard/deadline and request retirement. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef long ktime_t;
#define BIT(n) (1UL << (n))
#define APPLE_ANS_NVMMU_MAX_REQS 36
#define APPLE_ANS_REG_CORE_MASK 0x1824
#define APPLE_ANS_REG_LOG_SIZE 0x1828
#define APPLE_ANS_REG_HMB_SIZE_REQ 0x1808
#define APPLE_ANS_REG_HMB_ALIGN_REQ 0x180c
#define APPLE_ANS_REG_BFH 0x1b18
#define APPLE_ANS_BFH_MAGIC 0xbfbfbfbf
#define NVME_REG_CAP 0
#define NVME_REG_VS 8
#define NVME_REG_CC 20
#define NVME_REG_CSTS 28
#define NVME_CSTS_RDY 1
#define NVME_CSTS_CFS 2
#define NVME_CC_ENABLE 1
#define NVME_CC_SHN_MASK (3 << 14)
#define APPLE_ANS_ROUND7 0
static int apple_ans_boot_mode(void *d) { return APPLE_ANS_ROUND7; }
static int apple_ans_dart_set_active(void *d, bool active) { return 0; }
#define MSEC_PER_SEC 1000
#define U32_MAX UINT32_MAX
#define ans_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define dev_err_probe(dev, err, ...) (err)
#define spin_lock_irqsave(l, f) ((f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(f))
#define clamp_val(v, l, h) ((v) < (l) ? (l) : (v) > (h) ? (h) : (v))
struct device { int unused; };
struct apple_ans_host_state { u32 link_up, nvmmu_err, dart_err, dart_addr; };
struct apple_ans_nvme_req { unsigned qid, cid, opcode, nsid; bool mapped; };
struct apple_ans_nvme {
    unsigned hmb_size, last_req; int req_lock;
    unsigned long used_req[1];
    struct apple_ans_nvme_req req[36];
    bool cc_base_valid, cc_written, host_fault, dart_fault_logged;
    struct apple_ans_host_state cc_base;
    u32 last_cc; ktime_t cc_written_at;
};
struct nvme_dev { struct device *dev; char *bar; int ctrl; };
static struct apple_ans_nvme ans;
static struct nvme_dev dev;
static bool stuck, link = true, reset_fail, new_fault, warm_board = true;
static int inv_error;
static int cc_zero, resets, schedules, bar_reads, dumps;
static long now;
static bool apple_ans_dart_hmb;
static unsigned apple_ans_rdy_timeout = 5;
static struct apple_ans_nvme *nvme_pci_apple_ans(struct nvme_dev *d) { return &ans; }
static void bitmap_zero(unsigned long *p, unsigned n) { *p = 0; }
static void __clear_bit(unsigned b, unsigned long *p) { *p &= ~BIT(b); }
static int apple_ans_nvmmu_reset(struct device *d) {
    assert(cc_zero && !stuck); resets++; return reset_fail ? -ETIMEDOUT : 0;
}
static int apple_ans_nvmmu_map(struct device *d, unsigned t, void *p, unsigned n, bool mem, void *a) {
    return inv_error;
}
static int nvme_reset_ctrl(int *ctrl) { schedules++; return 0; }
static void apple_ans_settle(void) {}
static int nvme_pci_apple_ans_check_link(struct nvme_dev *d, const char *s) { return link ? 0 : -ENOLINK; }
static void nvme_pci_apple_ans_config(struct nvme_dev *d) {}
static int nvme_pci_apple_ans_find_hmb(struct nvme_dev *d, u32 sz, u32 al) { ans.hmb_size = 0x1200000; return 0; }
static int nvme_pci_apple_ans_alloc_dart_hmb(struct nvme_dev *d, u32 sz, u32 al) { return -ENODEV; }
static u32 readl(void *p) {
    bar_reads++;
    if (p == dev.bar + NVME_REG_CSTS) return stuck ? NVME_CSTS_RDY : 0;
    return 0;
}
static u64 lo_hi_readq(void *p) { return 0; }
static void writel(u32 v, void *p) {
    assert(p == dev.bar + NVME_REG_CC && !v); cc_zero++;
}
#define readl_poll_timeout(p, v, cond, delay, timeout) \
    ({ assert((timeout) == 2000000); (v) = readl(p); (cond) ? 0 : -ETIMEDOUT; })
static int apple_ans_host_state(struct device *d, struct apple_ans_host_state *s) {
    memset(s, 0, sizeof(*s)); s->link_up = link; return 0;
}
static bool nvme_pci_apple_ans_new_fault(struct nvme_dev *d, struct apple_ans_host_state *s) { return new_fault; }
static bool nvme_pci_apple_ans_new_dart_fault(const struct apple_ans_host_state *s) { return false; }
static void apple_ans_host_dump(struct device *d, const char *s) { dumps++; }
static bool of_machine_is_compatible(const char *s) { return warm_board; }
static ktime_t ktime_get(void) { return now; }
static long ktime_ms_delta(ktime_t a, ktime_t b) { return a - b; }
#include "production.h"
int main(void) {
    char bar[0x2000]; dev.bar = bar;
    ans.used_req[0] = BIT(2);
    assert(!nvme_pci_apple_ans_preinit(&dev));
    assert(cc_zero == 1 && resets == 1 && !ans.used_req[0]);
    stuck = true;
    assert(nvme_pci_apple_ans_preinit(&dev) == -ETIMEDOUT && resets == 1);
    stuck = false; reset_fail = true;
    assert(nvme_pci_apple_ans_preinit(&dev) == -ETIMEDOUT);
    reset_fail = false; link = false;
    assert(nvme_pci_apple_ans_preinit(&dev) == -ENOLINK);
    ans.used_req[0] = BIT(2); inv_error = -ETIMEDOUT; ans.req[2].mapped = true;
    nvme_pci_apple_ans_free_req(&dev, &ans.req[2]);
    assert(ans.used_req[0] == BIT(2) && schedules == 1);
    inv_error = 0;
    nvme_pci_apple_ans_free_req(&dev, &ans.req[2]);
    assert(!ans.used_req[0]);
    /* Replay the observed completed-slot ENODEV: preserve the TCB's slot
       without interpreting a host lookup error as controller failure. */
    int local_errors[]={-ENODEV,-EINVAL,-EOPNOTSUPP};
    for(unsigned i=0;i<sizeof(local_errors)/sizeof(*local_errors);i++) {
        int old_schedules=schedules;
        ans.req[14].mapped=true; ans.used_req[0]=BIT(14); inv_error=local_errors[i];
        nvme_pci_apple_ans_free_req(&dev,&ans.req[14]);
        assert(ans.used_req[0]==BIT(14) && schedules==old_schedules);
    }
    inv_error=0;
    u32 csts; int prev = bar_reads;
    assert(nvme_pci_apple_ans_read_csts(&dev, &csts) == -EIO && bar_reads == prev);
    link = true; ans.host_fault = true;
    assert(nvme_pci_apple_ans_read_csts(&dev, &csts) == -EIO && bar_reads == prev);
    ans.host_fault = false; ans.cc_base_valid = true; new_fault = true;
    assert(nvme_pci_apple_ans_read_csts(&dev, &csts) == -EIO && bar_reads == prev);
    ans.host_fault = false; new_fault = false; ans.cc_written = true;
    ans.last_cc = NVME_CC_ENABLE; now = 4999;
    assert(!nvme_pci_apple_ans_read_csts(&dev, &csts));
    now = 5000; assert(nvme_pci_apple_ans_read_csts(&dev, &csts) == -ETIMEDOUT);
    ans.host_fault = false; ans.last_cc = NVME_CC_SHN_MASK | NVME_CC_ENABLE;
    assert(!nvme_pci_apple_ans_read_csts(&dev, &csts));
    ans.last_cc = NVME_CC_ENABLE; warm_board = false;
    assert(!nvme_pci_apple_ans_read_csts(&dev, &csts));
    puts("PASS controller: RDY=0 before reset; stop/reset/link failures; failed slot quarantined; host fault skips BAR; RDY deadline; SHN/other-board exclusions");
}
