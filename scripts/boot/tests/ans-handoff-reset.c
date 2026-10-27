/* Execute the actual reset worker, counting every disable/recovery/publication. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define ANS_RECOVERY_USED 0
#define ANS_HEALTH_ACTIVE 1
#define ANS_RECOVERY_PENDING 2
#define ANS_HEALTH_FAILED 3
#define APPLE_ANS_ROUND7 0
#define NVME_CC_ENABLE 1
#define NVME_CC_SHN_NORMAL 0x4000
#define NVME_CTRL_RESETTING 1
#define NVME_CTRL_CONNECTING 2
#define NVME_CTRL_LIVE 3
#define NVME_CTRL_DELETING 4
#define NVME_CTRL_DEAD 5
#define NVME_MAX_META_SEGS 1
#define dev_warn(...) ((void)0)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct work_struct {int unused;};
struct ctrl {struct work_struct reset_work;unsigned ctrl_config;int state;void *device,*admin_q;unsigned max_integrity_segments;};
struct nvme_dev {struct ctrl ctrl;void *dev,*dma_ops;unsigned long apple_ans_flags;int shutdown_lock,admin_tagset;unsigned online_queues;};
static int nvme_pci_apple_ans_ops,nvme_mq_admin_ops;
static int gate_result,gate_calls,disables,recoveries,starts,dumps,flushes,enables;
static bool gate_unhealthy;
static void set_bit(unsigned n,unsigned long *p) {*p|=1UL<<n;}
static void clear_bit(unsigned n,unsigned long *p) {*p&=~(1UL<<n);}
static bool test_and_set_bit(unsigned n,unsigned long *p) {bool v=*p&(1UL<<n);set_bit(n,p);return v;}
static bool test_and_clear_bit(unsigned n,unsigned long *p) {bool v=*p&(1UL<<n);clear_bit(n,p);return v;}
static int apple_ans_boot_mode(void *d) {return 1;}
static bool nvme_state_terminal(struct ctrl *c) {return c->state>=NVME_CTRL_DELETING;}
static int nvme_ctrl_state(struct ctrl *c) {return c->state;}
static bool nvme_change_ctrl_state(struct ctrl *c,int s) {c->state=s;return true;}
static void nvme_apple_ans_dump(struct nvme_dev *d,const char *s) {dumps++;}
static void pr_flush(int t,bool r) {flushes++;}
static void nvme_dev_disable(struct nvme_dev *d,bool shutdown) {disables++;d->ctrl.ctrl_config=0;}
static void nvme_sync_queues(struct ctrl *c) {}
static void *to_pci_dev(void *d) {return d;}
static void pci_clear_master(void *d) {}
static int nvme_pci_apple_ans_recover(struct nvme_dev *d) {recoveries++;return 0;}
static void mutex_lock(int *p) {}
static void mutex_unlock(int *p) {}
static int nvme_pci_enable(struct nvme_dev *d) {enables++;d->ctrl.ctrl_config=1;return 0;}
struct nvme_iod {int unused;};
static int nvme_alloc_admin_tag_set(struct ctrl *c,int *t,int *ops,size_t n) {assert(0);return 0;}
static void nvme_unquiesce_admin_queue(struct ctrl *c) {}
static int nvme_init_ctrl_finish(struct ctrl *c,bool suspend) {return 0;}
static void nvme_pci_write_window_limits(struct nvme_dev *d) {}
static bool nvme_ctrl_meta_sgl_supported(struct ctrl *c) {return false;}
static void nvme_dbbuf_dma_alloc(struct nvme_dev *d) {}
static int nvme_setup_host_mem(struct nvme_dev *d) {return 0;}
static void nvme_update_attrs(struct nvme_dev *d) {}
static int nvme_setup_io_queues(struct nvme_dev *d) {return 0;}
static void nvme_dbbuf_set(struct nvme_dev *d) {}
static void nvme_unquiesce_io_queues(struct ctrl *c) {}
static int nvme_wait_freeze_timeout(struct ctrl *c) {return 1;}
static void nvme_wait_freeze(struct ctrl *c) {}
static bool nvme_pci_update_nr_queues(struct nvme_dev *d) {return true;}
static void nvme_unfreeze(struct ctrl *c) {}
static void nvme_mark_namespaces_dead(struct ctrl *c) {}
static void nvme_remove_namespaces(struct ctrl *c) {}
static void nvme_free_tagset(struct nvme_dev *d) {}
static void nvme_start_ctrl(struct ctrl *c) {starts++;}
/* Trees with nvme.apple_ans_irq_rescue_ms arm it right after start. */
static void nvme_apple_ans_irq_rescue_start(struct nvme_dev *d) {assert(starts);}
static int nvme_apple_ans_health(struct nvme_dev *d,bool *unhealthy) {
    gate_calls++;*unhealthy=gate_unhealthy;return gate_result;
}
#include "production.h"
int main(void) {
    const int errors[]={0,-EINVAL,-ENOMEM,-EAGAIN,-ENODEV,-ERANGE,-EIO,2};
    for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++) {
        struct nvme_dev d={.ctrl={.state=NVME_CTRL_RESETTING,.admin_q=(void*)1},
            .dma_ops=&nvme_pci_apple_ans_ops,.online_queues=2};
        gate_result=errors[i];gate_unhealthy=false;
        gate_calls=disables=recoveries=starts=dumps=flushes=enables=0;
        nvme_reset_work(&d.ctrl.reset_work);
        assert(d.ctrl.state==NVME_CTRL_LIVE && starts==1 && enables==1 && gate_calls==1);
        assert(!disables && !recoveries && !dumps && !flushes && !d.apple_ans_flags);
    }
    struct nvme_dev d={.ctrl={.state=NVME_CTRL_RESETTING,.admin_q=(void*)1},
        .dma_ops=&nvme_pci_apple_ans_ops,.online_queues=2};
    gate_result=-ETIMEDOUT;gate_unhealthy=true;
    gate_calls=disables=recoveries=starts=dumps=flushes=enables=0;
    nvme_reset_work(&d.ctrl.reset_work);
    assert(d.ctrl.state==NVME_CTRL_DEAD && !starts && recoveries==1 && dumps==1 && flushes==1);
    assert(gate_calls==2 && enables==2);
    puts("PASS reset worker: local/status gate errors publish without disable/recovery; timeout uses exactly one recovery");
}
