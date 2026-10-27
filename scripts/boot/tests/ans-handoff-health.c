/* Real health commands plus the tree's full queue-limits validator. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef unsigned blk_features_t,blk_flags_t;
enum blk_integrity_checksum { BLK_INTEGRITY_CSUM_NONE };
static unsigned page_shift=14,warnings;
#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512
#define PAGE_SIZE (1UL<<page_shift)
#define PAGE_SECTORS (PAGE_SIZE>>SECTOR_SHIFT)
#define BLK_MAX_BLOCK_SIZE PAGE_SIZE
#define BLK_SAFE_MAX_SECTORS 255
#define BLK_MAX_SEGMENTS 128
#define BLK_MAX_SEGMENT_SIZE 65536
#define BLK_SEG_BOUNDARY_MASK 0xffffffffUL
#define BLK_MIN_SEGMENT_SIZE 4096
#define BLK_DEF_MAX_SECTORS_CAP 8192
#define BLK_FLAG_MISALIGNED 1
#define BLK_FEAT_WRITE_CACHE 1
#define BLK_FEAT_FUA 2
#define WARN_ON_ONCE(x) ({bool v=!!(x);warnings+=v;v;})
#define pr_warn(...) ((void)0)
#define is_power_of_2(x) ((x) && !((x)&((x)-1)))
#define min(x,y) ((x)<(y)?(x):(y))
#define max(x,y) ((x)>(y)?(x):(y))
#define min_t(t,x,y) min((t)(x),(t)(y))
#define min_not_zero(x,y) (!(x)?(y):!(y)?(x):min(x,y))
#define round_down(x,y) ((x)/(y)*(y))
#include "block-limits.h"
/* These optional features are absent on the diagnostic/admin queue. */
static void blk_validate_atomic_write_limits(struct queue_limits *l) {
    assert(!l->features && !l->atomic_write_hw_max);
}
static int blk_validate_integrity_limits(struct queue_limits *l) {
    assert(!l->integrity.metadata_size && !l->integrity.csum_type);return 0;
}
static int blk_validate_zoned_limits(struct queue_limits *l) {
    assert(!l->max_open_zones && !l->max_active_zones);return 0;
}
#include "block-validation.h"
#define HZ 100
#define GFP_KERNEL 0
#define BLK_MQ_REQ_NOWAIT 1
#define ANS_RECOVERY_USED 0
#define ANS_HEALTH_ACTIVE 1
#define ANS_RECOVERY_PENDING 2
#define ANS_HEALTH_FAILED 3
#define APPLE_ANS_ROUND7 0
#define nvme_admin_identify 6
#define NVME_ID_CNS_NS 0
#define nvme_cmd_read 2
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define le64_to_cpu(x) (x)
#define IS_ERR(x) ((intptr_t)(x)<0)
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define dev_info(...) ((void)0)
struct nvme_command {
    struct {u8 opcode;u32 nsid,cns;} identify;
    struct {u8 opcode;u32 nsid;u64 slba;unsigned length;} rw;
};
struct nvme_id_ns {struct {unsigned ms,ds;} lbaf[16];unsigned flbas;u64 nsze;};
struct request_queue {int admin;struct queue_limits limits;};
struct request {unsigned timeout;struct nvme_command *cmd;void *buffer;unsigned size;int admin;};
struct nvme_dev {void *dev,*dma_ops;struct {void *tagset;struct request_queue *admin_q;} ctrl;
    unsigned online_queues;int tagset;unsigned long apple_ans_flags;};
static struct {u32 nsid;u64 first,last;} write_window={1,100,1000};
static int nvme_pci_apple_ans_ops;
static int fail_command,fail_result,alloc_fail,map_fail,queue_fail,memory_fail;
static int commands,reads,destroys,dumps,flushed,mode=1;
static unsigned lba_shift=12;
static u64 nsze=10000;
static bool terminal;
static u64 lbas[2];
static struct request req;
static struct request_queue io;
static struct nvme_dev *current_dev;
static void *health_alloc(unsigned n) {return memory_fail?NULL:calloc(1,n);}
#define kzalloc(n,g) health_alloc(n)
#define kzalloc_obj(x) health_alloc(sizeof(x))
#define kfree free
static void set_bit(unsigned b,unsigned long *v) {*v|=1UL<<b;}
static void clear_bit(unsigned b,unsigned long *v) {*v&=~(1UL<<b);}
static bool test_bit(unsigned b,unsigned long *v) {return !!(*v&(1UL<<b));}
static bool test_and_set_bit(unsigned b,unsigned long *v) {bool old=test_bit(b,v);set_bit(b,v);return old;}
static int apple_ans_boot_mode(void *d) {return mode;}
static bool nvme_state_terminal(void *c) {return terminal;}
static void nvme_apple_ans_dump(struct nvme_dev *d,const char *s) {dumps++;}
static void pr_flush(int ms,bool reset) {assert(dumps && ms==1000 && !reset);flushed++;}
static int nvme_req_op(struct nvme_command *c) {return 0;}
static struct request *blk_mq_alloc_request(struct request_queue *q,int op,int flags) {
    assert(flags==BLK_MQ_REQ_NOWAIT);memset(&req,0,sizeof(req));req.admin=q->admin;
    return alloc_fail?(void*)(intptr_t)-EAGAIN:&req;
}
static void nvme_init_request(struct request *r,struct nvme_command *c) {r->cmd=c;}
static int blk_rq_map_kern(struct request *r,void *p,unsigned size,int g) {
    assert(r->timeout==5*HZ);r->buffer=p;r->size=size;return map_fail?-ENOMEM:0;
}
static int nvme_execute_rq(struct request *r,bool at_head) {
    assert(!at_head && r->timeout==5*HZ);commands++;
    if(commands==fail_command) {
        if(fail_result==-ETIMEDOUT)set_bit(ANS_HEALTH_FAILED,&current_dev->apple_ans_flags);
        return fail_result;
    }
    if(r->admin) {
        assert(r->cmd->identify.opcode==6 && r->cmd->identify.nsid==1);
        struct nvme_id_ns *id=r->buffer;id->lbaf[0].ds=lba_shift;id->nsze=nsze;
    } else {
        assert(r->cmd->rw.opcode==2 && r->cmd->rw.nsid==1 && r->size==(reads?4096:8192));
        assert(r->cmd->rw.length==r->size/(1U<<lba_shift)-1 && reads<2);
        lbas[reads++]=r->cmd->rw.slba;
    }
    return 0;
}
static void blk_mq_free_request(struct request *r) {}
static unsigned nvme_lbaf_index(unsigned x) {return x&15;}
static bool nvme_pci_write_window_active(void) {return true;}
static struct request_queue *blk_mq_alloc_queue(void *set,struct queue_limits *l,void *p) {
    assert(l->logical_block_size==(1U<<lba_shift));
    assert(l->max_hw_sectors==128 && l->max_segments==32);
    assert(l->virt_boundary_mask==4095 && l->dma_alignment==4095);
    int ret=blk_set_default_limits(l);
    if(ret || queue_fail)return (void*)(intptr_t)(ret?ret:queue_fail);
    io.limits=*l;return &io;
}
static void blk_mq_destroy_queue(struct request_queue *q) {destroys++;}
static void blk_put_queue(struct request_queue *q) {}
#include "production.h"
static void reset(struct nvme_dev *d) {
    commands=reads=destroys=fail_command=fail_result=0;d->apple_ans_flags=0;
}
int main(void) {
    /* Reproduce round 8 exactly: 16 sectors is legal at 4K, illegal at 16K/64K. */
    for(page_shift=12;page_shift<=16;page_shift+=2) {
        struct queue_limits bad={.max_hw_sectors=16,.max_segments=1,
            .logical_block_size=4096,.physical_block_size=4096};
        warnings=0;
        assert(blk_set_default_limits(&bad)==(page_shift==12?0:-EINVAL));
        assert(warnings==(page_shift==12?0:1));
    }
    page_shift=14;
    struct request_queue admin={.admin=1,.limits={.max_hw_sectors=128,.max_segments=32,
        .max_segment_size=UINT_MAX,.virt_boundary_mask=4095,.dma_alignment=4095}};
    assert(!blk_set_default_limits(&admin.limits));
    struct nvme_dev dev={.dma_ops=&nvme_pci_apple_ans_ops,.online_queues=2,
        .ctrl={.tagset=(void*)1,.admin_q=&admin}};
    current_dev=&dev;
    bool unhealthy;
    for(page_shift=12;page_shift<=16;page_shift+=2)for(lba_shift=9;lba_shift<=12;lba_shift++) {
        reset(&dev);warnings=0;
        assert(!nvme_apple_ans_health(&dev,&unhealthy) && !unhealthy);
        assert(commands==3 && reads==2 && destroys==1 && !warnings);
        assert(lbas[0]==0 && lbas[1]==100 && !test_bit(ANS_HEALTH_ACTIVE,&dev.apple_ans_flags));
    }
    page_shift=14;lba_shift=12;
    for(int fail=1;fail<=3;fail++) {
        reset(&dev);fail_command=fail;fail_result=-ETIMEDOUT;
        assert(nvme_apple_ans_health(&dev,&unhealthy)==-ETIMEDOUT && unhealthy);
        assert(commands==fail && !test_bit(ANS_HEALTH_ACTIVE,&dev.apple_ans_flags));
        for(int error=0;error<3;error++) {
            reset(&dev);fail_command=fail;fail_result=error==0?-EINVAL:error==1?-EIO:2;
            assert(nvme_apple_ans_health(&dev,&unhealthy)==fail_result && !unhealthy);
        }
    }
    reset(&dev);alloc_fail=1;
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-EAGAIN && !unhealthy);alloc_fail=0;
    map_fail=1;
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-ENOMEM && !unhealthy);map_fail=0;
    memory_fail=1;
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-ENOMEM && !unhealthy);memory_fail=0;
    for(int error=0;error<2;error++) {
        reset(&dev);queue_fail=error?-ENOMEM:-EINVAL;
        assert(nvme_apple_ans_health(&dev,&unhealthy)==queue_fail && !unhealthy);
        assert(commands==1 && !reads && !destroys);
    }
    queue_fail=0;reset(&dev);lba_shift=13;
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-EINVAL && !unhealthy);
    lba_shift=12;nsze=1;reset(&dev);
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-ERANGE && !unhealthy && !reads && destroys==1);
    dev.online_queues=1;
    assert(nvme_apple_ans_health(&dev,&unhealthy)==-ENODEV && !unhealthy);dev.online_queues=2;
    dev.apple_ans_flags=0;assert(nvme_apple_ans_retry(&dev));
    assert(dumps==1 && flushed==1 && test_bit(ANS_RECOVERY_PENDING,&dev.apple_ans_flags));
    assert(!nvme_apple_ans_retry(&dev) && dumps==1);
    dev.apple_ans_flags=0;mode=0;assert(!nvme_apple_ans_retry(&dev));
    mode=1;terminal=true;assert(!nvme_apple_ans_retry(&dev));
    puts("PASS real blk_validate_limits: round8 -EINVAL at 16K; fixed 4K/16K/64K; NSID1 reads; local/status errors inconclusive, timeout unhealthy");
}
