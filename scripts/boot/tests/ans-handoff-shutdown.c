#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef uint32_t u32;
enum apple_ans_boot_mode { APPLE_ANS_ROUND7, APPLE_ANS_HANDOFF, APPLE_ANS_FULL_CYCLE };
#define NVME_CSTS_SHST_MASK 12
#define NVME_CSTS_SHST_CMPLT 8
#define NVME_CSTS_RDY 1
#define NVME_CSTS_CFS 2
#define NVME_CC_ENABLE 1
#define NVME_CC_SHN_MASK (3<<14)
#define NVME_REG_CC 20
#define dev_info(...) ((void)0)
struct nvme_dev {void *dev; struct {u32 ctrl_config;} ctrl; unsigned char *bar;};
static int mode, result, status, inactive, master, dump, legacy, writes, reads;
static int write_error, link_error, stuck, readback_stuck, dart_error, stop_read_error;
static u32 reg[16];
static jmp_buf jump;
#define panic(...) longjmp(jump,1)
static enum apple_ans_boot_mode apple_ans_boot_mode(void *d) {return mode;}
static void nvme_pci_apple_ans_shutdown_round7(struct nvme_dev *d,int r) {legacy++;}
static int nvme_pci_apple_ans_read_csts(struct nvme_dev *d,u32 *v) {
    assert(!master && !inactive); reads++;
    if(writes && stop_read_error)return -EIO;
    *v=status;return result;
}
static void apple_ans_host_dump(void *d,const char *s) {assert(master);dump++;}
static void *to_pci_dev(void *d) {return d;}
static void pci_clear_master(void *d) {master++;}
static int apple_ans_dart_set_active(void *d,bool a) {
    assert(!a && master && writes && !(status & NVME_CSTS_RDY));inactive++;return dart_error;
}
static int nvme_pci_apple_ans_write_cc(struct nvme_dev *d,u32 cc) {
    assert(status==9 && !master && !inactive);
    assert(!(cc & (NVME_CC_ENABLE|NVME_CC_SHN_MASK)));writes++;
    if(!stuck)status=0;
    reg[NVME_REG_CC/4]=readback_stuck?1:cc;return write_error;
}
static int nvme_pci_apple_ans_check_link(struct nvme_dev *d,const char *step) {return link_error;}
static u32 readl(void *p) {assert(writes && !master && !inactive);return *(u32*)p;}
#define read_poll_timeout(op,err,condition,interval,timeout,sleep,d,v) ({ \
    assert(interval==1000 && timeout==2000000); int r=-ETIMEDOUT; \
    for(int n=0;n<3;n++) {err=op(d,v);if(condition){r=0;break;}}r;})
#include "production.h"
static struct nvme_dev init(int m) {
    mode=m;result=inactive=master=dump=legacy=writes=reads=0;
    write_error=link_error=stuck=readback_stuck=dart_error=stop_read_error=0;status=9;
    return (struct nvme_dev){.ctrl.ctrl_config=0x464001,.bar=(void*)reg};
}
int main(void) {
    for(int m=APPLE_ANS_HANDOFF;m<=APPLE_ANS_FULL_CYCLE;m++) {
        struct nvme_dev d=init(m);
        nvme_pci_apple_ans_shutdown(&d,0);
        assert(master==1 && writes==1 && reads==2 && !dump);
        assert(!(d.ctrl.ctrl_config&(NVME_CC_ENABLE|NVME_CC_SHN_MASK)));
        assert(inactive==1);
        for(int i=0;i<10;i++) {
            d=init(m);
            if(i==0)status=1; if(i==1)result=-EIO;
            if(i==3)write_error=-EIO; if(i==4)stuck=1;
            if(i==5)link_error=-ENOLINK; if(i==6)readback_stuck=1;
            if(i==7)stop_read_error=1;
            if(i==8)status=11; /* CFS after SHST must not certify */
            if(i==9)dart_error=-EIO;
            if(!setjmp(jump)) {nvme_pci_apple_ans_shutdown(&d,i==2?-ETIMEDOUT:0);assert(0);}
            assert(master==1);
            if(i!=9)assert(!inactive && dump==1);
            if(i<3)assert(!writes);
        }
    }
    struct nvme_dev d=init(APPLE_ANS_ROUND7);
    nvme_pci_apple_ans_shutdown(&d,0);assert(legacy==1 && !writes);
    puts("PASS SHST then CC.EN/SHN clear, bounded RDY=0 and CC readback before BME/DART; failures refuse port reset; Round7 selectable");
}
