/* Execute production allocation/map/unmap/retirement with local vs HW faults. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint64_t dma_addr_t;
typedef int blk_status_t;
#define BIT(n) (1UL << (n))
#define APPLE_ANS_NVMMU_MAX_REQS 36
#define APPLE_ANS_NVMMU_MAX_PAGES 256
#define APPLE_ANS_NVMMU_PAGE_SIZE 4096
#define BLK_STS_OK 0
#define BLK_STS_RESOURCE 1
#define BLK_STS_IOERR 2
#define BLK_STS_NOTSUPP 3
#define GFP_ATOMIC 0
#define RQF_SPECIAL_PAYLOAD 1
#define DMA_FROM_DEVICE 0
#define NVME_CMD_APPLE_ANS_FLATDMA BIT(5)
#define dev_dbg_ratelimited(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
static char message[1024];
#define dev_warn_ratelimited(d, ...) snprintf(message, sizeof(message), __VA_ARGS__)
#define spin_lock_irqsave(l,f) ((f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(f))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le64(x) (x)
struct apple_ans_nvme_req {
    u64 pages[256]; u32 nsid; u16 qid, cid; u8 opcode; bool mapped;
};
struct apple_ans_nvme {
    unsigned last_req; int req_lock;
    unsigned long used_req[1]; struct apple_ans_nvme_req req[36];
};
struct nvme_dev { void *dev, *dmavec_mempool; int ctrl; };
struct nvme_queue { struct nvme_dev *dev; unsigned qid; };
struct nvme_iod {
    struct apple_ans_nvme_req *dma_private;
    struct { u64 addr; unsigned len; } *dma_vecs;
    unsigned nr_dma_vecs, total_len;
    struct { struct {
        u8 flags, opcode; u16 command_id; u32 nsid;
        struct { u64 prp1, prp2; } dptr;
    } common; } cmd;
};
struct blk_mq_hw_ctx { void *driver_data; };
struct request {
    struct blk_mq_hw_ctx *mq_hctx; struct nvme_iod iod;
    unsigned rq_flags, bytes;
};
struct bio_vec { unsigned bv_len; };
struct req_iterator { int i; };
#define rq_for_each_bvec(bv,rq,it) for ((it).i=0; (it).i<1 && ((bv).bv_len=(rq)->bytes); (it).i++)
static struct apple_ans_nvme ans;
static int schedules, maps, retires, map_error, retire_error, bvec_error, frees;
static bool alloc_fail;
static unsigned char vec_storage[1024];
static struct apple_ans_nvme *nvme_pci_apple_ans(struct nvme_dev *d) { return &ans; }
static struct nvme_iod *blk_mq_rq_to_pdu(struct request *r) { return &r->iod; }
static unsigned blk_rq_payload_bytes(struct request *r) { return r->bytes; }
static int rq_dma_dir(struct request *r) { return DMA_FROM_DEVICE; }
static struct bio_vec req_bvec(struct request *r) { return (struct bio_vec){r->bytes}; }
static void *mempool_alloc(void *p, int gfp) { return alloc_fail ? NULL : vec_storage; }
static void mempool_free(void *p, void *pool) { assert(p==vec_storage); frees++; }
static void dma_unmap_page(void *d, u64 a, unsigned l, int dir) {}
static int errno_to_blk_status(int e) { return BLK_STS_IOERR; }
static unsigned find_next_zero_bit(unsigned long *p, unsigned n, unsigned start) {
    while (start<n && (*p & BIT(start))) start++;
    return start;
}
static unsigned find_first_zero_bit(unsigned long *p, unsigned n) { return find_next_zero_bit(p,n,0); }
static void __set_bit(unsigned t, unsigned long *p) { *p |= BIT(t); }
static void __clear_bit(unsigned t, unsigned long *p) { *p &= ~BIT(t); }
static int nvme_reset_ctrl(int *c) { schedules++; return 0; }
static int apple_ans_nvmmu_map(void *d, unsigned t, const u64 *p, unsigned n, bool mem, dma_addr_t *a) {
    if (!n) { retires++; return retire_error; }
    maps++; if (map_error) return map_error;
    assert(p && mem); *a=0x40000000ULL+t*0x800000ULL; return 0;
}
static blk_status_t nvme_pci_apple_ans_map_bvec(struct nvme_dev *d, struct request *r,
        struct nvme_iod *iod, struct apple_ans_nvme_req *a, const struct bio_vec *b,
        unsigned total, unsigned *n, unsigned *c, u64 *offs) {
    if (bvec_error) return BLK_STS_RESOURCE;
    *n=1; *c=total; a->pages[0]=0x812340000ULL; iod->nr_dma_vecs=1;
    return BLK_STS_OK;
}
#include "production.h"
static void fresh(struct request *r) {
    memset(&ans,0,sizeof(ans)); memset(&r->iod,0,sizeof(r->iod));
    ans.last_req=13; r->bytes=4096; r->rq_flags=0;
    r->iod.cmd.common.command_id=0x2007;
    r->iod.cmd.common.opcode=6; r->iod.cmd.common.nsid=2;
    schedules=maps=retires=frees=map_error=retire_error=bvec_error=0;
    alloc_fail=false; message[0]=0;
}
int main(void) {
    struct nvme_dev dev={0}; struct nvme_queue q={.dev=&dev,.qid=0};
    struct blk_mq_hw_ctx ctx={.driver_data=&q}; struct request r={.mq_hctx=&ctx};
    /* Failed maps must never retire an unpublished slot, even if retirement
       would return ENODEV. Also checks allocation zeroes stale ownership. */
    for (int stage=0; stage<3; stage++) {
        fresh(&r); ans.req[14].mapped=true; retire_error=-ENODEV;
        alloc_fail=stage==0; bvec_error=stage==1; map_error=stage==2?-ENODEV:0;
        assert(nvme_pci_apple_ans_map_data(&r)!=BLK_STS_OK);
        assert(!ans.used_req[0] && !retires && !schedules && !r.iod.dma_private);
    }
    int local[]={-ENODEV,-EINVAL,-EOPNOTSUPP,-ENOMEM};
    for (unsigned i=0;i<sizeof(local)/sizeof(*local);i++) {
        fresh(&r); assert(!nvme_pci_apple_ans_map_data(&r));
        assert(ans.req[14].mapped && ans.used_req[0]==BIT(14));
        retire_error=local[i]; assert(nvme_pci_apple_ans_unmap_data(&r));
        assert(ans.used_req[0]==BIT(14) && !schedules && retires==1);
        assert(strstr(message,"qid 0 cid 0x2007 opcode 0x6 nsid 2 mapped 1"));
        assert(strstr(message,"local retirement error; no reset"));
        assert(!r.iod.dma_private && !r.iod.dma_vecs && frees==1);
    }
    int hw[]={-ETIMEDOUT,-EIO};
    for (unsigned i=0;i<2;i++) {
        fresh(&r); q.qid=1; r.iod.cmd.common.opcode=2; r.iod.cmd.common.nsid=1;
        assert(!nvme_pci_apple_ans_map_data(&r)); retire_error=hw[i];
        assert(nvme_pci_apple_ans_unmap_data(&r));
        assert(ans.used_req[0]==BIT(14) && schedules==1);
        assert(strstr(message,"qid 1") && strstr(message,"confirmed NVMMU failure"));
    }
    fresh(&r); r.rq_flags=RQF_SPECIAL_PAYLOAD;
    assert(!nvme_pci_apple_ans_map_data(&r));
    assert(nvme_pci_apple_ans_unmap_data(&r));
    assert(!ans.used_req[0] && !ans.req[14].mapped && !schedules && retires==1);
    assert(!nvme_pci_apple_ans_unmap_data(&r));
    puts("PASS slot 14: real map/unmap lifecycle; exact owner diagnostics; local errors never reset; unpublished slots released; hardware failures quarantined");
}
