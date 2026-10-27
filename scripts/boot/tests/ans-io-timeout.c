/* Execute the full production timeout policy; never touch real block devices. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
enum blk_eh_timer_return { BLK_EH_DONE, BLK_EH_RESET_TIMER };
enum { NVME_CTRL_LIVE, NVME_CTRL_CONNECTING, NVME_CTRL_RESETTING,
       NVME_CTRL_DELETING, NVME_CTRL_DEAD };
#define NVME_REG_CSTS 28
#define NVMEQ_POLLED 3
#define MQ_RQ_IN_FLIGHT 1
#define IOD_ABORTED 1
#define NVME_REQ_CANCELLED 1
#define ANS_HEALTH_ACTIVE 1
#define ANS_RECOVERY_PENDING 2
#define ANS_HEALTH_FAILED 3
#define APPLE_ANS_ROUND7 0
static int apple_ans_boot_mode(void *d) { return APPLE_ANS_ROUND7; }
static void pr_flush(int ms, bool reset) {}
static void pci_clear_master(void *p) {}
static void set_bit(unsigned b, unsigned long *p) { *p |= 1UL << b; }
#define BLK_MQ_REQ_NOWAIT 1
#define cpu_to_le16(x) (x)
#define nvme_admin_abort_cmd 8
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define mb() ((void)0)
#define fallthrough __attribute__((fallthrough))
#define IS_ERR(x) ((x) == NULL)
#define PTR_ERR(x) (-ENOMEM)
struct device { int unused; };
struct pci_dev { int unused; };
struct nvme_command {
    struct { u8 opcode; } common;
    struct { u8 opcode; u16 cid, sqid; } abort;
};
struct nvme_iod { unsigned flags; };
struct nvme_request { unsigned flags; struct nvme_command *cmd; };
struct nvme_ctrl {
    struct device *device;
    struct { int (*reg_read32)(struct nvme_ctrl *, u32, u32 *); } *ops;
    int state, abort_limit; void *admin_q;
};
struct nvme_dev {
    struct nvme_ctrl ctrl; struct device *dev; char *bar;
    void *dma_ops; unsigned long apple_ans_flags;
};
struct nvme_queue { struct nvme_dev *dev; unsigned long flags; unsigned qid; };
struct blk_mq_hw_ctx { struct nvme_queue *driver_data; };
struct request {
    struct blk_mq_hw_ctx *mq_hctx; int tag, state;
    struct nvme_iod iod; struct nvme_request nr;
    void *end_io, *end_io_data;
};
static int nvme_pci_apple_ans_ops;
static int dumps, reads, disables, resets, aborts, polls, shutdown;
static bool polled_completion, host_fault, board = true, offline;
static struct request *current_req;
static struct pci_dev pci;
static void *abort_endio;
static struct nvme_iod *blk_mq_rq_to_pdu(struct request *r) { return &r->iod; }
static struct nvme_request *nvme_req(struct request *r) { return &r->nr; }
static int nvme_cid(struct request *r) { return 42; }
static struct pci_dev *to_pci_dev(struct device *d) { return &pci; }
static void nvme_apple_ans_dump(struct nvme_dev *d, const char *why) { dumps++; }
static int read_csts(struct nvme_ctrl *c, u32 reg, u32 *v) {
    assert(dumps > reads); reads++; *v = 1; return host_fault ? -EIO : 0;
}
static u32 readl(void *p) { assert(!"ANS timeout must use guarded CSTS"); return 0; }
static bool pci_dev_is_disconnected(struct pci_dev *p) { return false; }
static bool pci_channel_offline(struct pci_dev *p) { return offline; }
static bool nvme_state_terminal(struct nvme_ctrl *c) { return c->state >= NVME_CTRL_DELETING; }
static int nvme_ctrl_state(struct nvme_ctrl *c) { return c->state; }
static bool nvme_change_ctrl_state(struct nvme_ctrl *c, int s) {
    if (nvme_state_terminal(c) && s == NVME_CTRL_RESETTING) return false;
    c->state = s; return true;
}
static bool nvme_should_reset(struct nvme_dev *d, u32 csts) { return false; }
static void nvme_warn_reset(struct nvme_dev *d, u32 csts) {}
static bool test_bit(unsigned b, unsigned long *p) { return !!(*p & (1UL << b)); }
static bool test_and_set_bit(unsigned b, unsigned long *p) {
    bool old = test_bit(b, p); *p |= 1UL << b; return old;
}
static void nvme_poll_irqdisable(struct nvme_queue *q) {
    polls++; if (polled_completion) current_req->state = 0;
}
static void nvme_poll(struct blk_mq_hw_ctx *h, void *unused) { nvme_poll_irqdisable(h->driver_data); }
/* Trees with the shared-vector fix drain every CQ on the ANS vector instead. */
static int drains;
static bool nvme_apple_ans_shared_vector(struct nvme_dev *d) { return d->dma_ops == &nvme_pci_apple_ans_ops; }
static void nvme_apple_ans_drain_vector(struct nvme_queue *q, const char *why) {
    drains++; if (polled_completion) current_req->state = 0;
}
static int blk_mq_rq_state(struct request *r) { return r->state; }
static void nvme_dev_disable(struct nvme_dev *d, bool s) { disables++; shutdown = s; }
static bool nvme_try_sched_reset(struct nvme_ctrl *c) { resets++; return true; }
static void nvme_unquiesce_io_queues(struct nvme_ctrl *c) {}
static bool of_machine_is_compatible(const char *s) { return board; }
static int atomic_dec_return(int *p) { return --*p; }
static void atomic_inc(int *p) { ++*p; }
static const char *nvme_opcode_str(unsigned qid, u8 op) { return "read"; }
static const char *nvme_get_opcode_str(u8 op) { return "read"; }
static const char *blk_op_str(unsigned op) { return "read"; }
static unsigned req_op(struct request *r) { return 0; }
static unsigned blk_rq_bytes(struct request *r) { return 4096; }
static int nvme_req_op(struct nvme_command *c) { return 0; }
static struct request *blk_mq_alloc_request(void *q, int op, int flags) { aborts++; return NULL; }
static void nvme_init_request(struct request *r, struct nvme_command *c) {}
static void blk_execute_rq_nowait(struct request *r, bool head) { assert(0); }
#include "production.h"
int main(void) {
    typeof(*((struct nvme_ctrl *)0)->ops) ops = {.reg_read32 = read_csts};
    struct nvme_dev d = {.ctrl = {.ops = &ops, .state = NVME_CTRL_LIVE, .abort_limit = 1},
                         .dma_ops = &nvme_pci_apple_ans_ops};
    struct nvme_queue q = {.dev = &d, .qid = 1};
    struct blk_mq_hw_ctx h = {.driver_data = &q};
    struct nvme_command cmd = {0};
    struct request req = {.mq_hctx = &h, .state = MQ_RQ_IN_FLIGHT, .nr.cmd = &cmd};
    current_req = &req;
    polled_completion = true;
    assert(nvme_timeout(&req) == BLK_EH_DONE && !resets && !d.apple_ans_flags);
    polled_completion = false; req.state = MQ_RQ_IN_FLIGHT;
    assert(nvme_timeout(&req) == BLK_EH_DONE && resets == 1 && disables == 1 && !aborts && !shutdown);
    d.ctrl.state = NVME_CTRL_LIVE;
    assert(nvme_timeout(&req) == BLK_EH_DONE && resets == 1 && disables == 2 && shutdown);
    assert(d.ctrl.state == NVME_CTRL_DELETING);
    d.ctrl.state = NVME_CTRL_CONNECTING; d.apple_ans_flags = 0;
    assert(nvme_timeout(&req) == BLK_EH_DONE && !d.apple_ans_flags && d.ctrl.state == NVME_CTRL_DELETING);
    d.ctrl.state = NVME_CTRL_RESETTING;
    assert(nvme_timeout(&req) == BLK_EH_RESET_TIMER);
    host_fault = true; d.ctrl.state = NVME_CTRL_LIVE;
    assert(nvme_timeout(&req) == BLK_EH_DONE && resets == 2);
    host_fault = false; d.ctrl.state = NVME_CTRL_LIVE; board = false;
    assert(nvme_timeout(&req) == BLK_EH_RESET_TIMER && aborts == 1);
    assert(reads == dumps);
    assert(polls + drains == 6);
    puts("PASS timeout: snapshot before guarded CSTS; lost IRQ polled; one reset; repeated stall fails; startup/reset states; host fault; other-board generic abort");
}
