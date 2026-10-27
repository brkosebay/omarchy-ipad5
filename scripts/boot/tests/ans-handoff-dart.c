#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL<<(n))
#define DART_S5L8960X 1
#define DART_S5L8960X_STREAM_COMMAND 0
#define DART_S5L8960X_STREAM_COMMAND_SID_SHIFT 8
#define DART_S5L8960X_STREAM_COMMAND_BUSY BIT(3)
#define DART_STREAM_COMMAND_BUSY_TIMEOUT 100
#define spin_lock_irqsave(l,f) ((f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(f))
#define DART_S5L8960X_ERROR 0x10
#define DART_S5L8960X_ERROR_ADDR 0x1c
#define DART_S5L8960X_ERROR_STREAM (3U<<24)
#define DART_S5L8960X_TCR 0xc
#define DART_S5L8960X_ERROR_REFLECTOR 0x24
#define DART_S5L8960X_FETCH_CONFIG 0x30
#define DART_S5L8960X_DIAG_CONFIG 0x20
#define DART_TTBR(d,s,i) (0x40+16*(s)+4*(i))
#define IOMMU_DOMAIN_DMA 1
#define IOMMU_DOMAIN_DMA_FQ 2
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctz(m))
#define BITS_TO_LONGS(n) 1
#define for_each_set_bit(i,p,n) for((i)=0;(i)<(n);(i)++)if(*(p)&BIT(i))
#define for_each_stream_map(i,c,m) for((i)=0;(i)<1 && ((m)=&(c)->stream_maps[i],1);(i)++)
#define EXPORT_SYMBOL_GPL(x)
struct device {int unused;};
struct apple_dart;
struct apple_dart_stream_map {struct apple_dart *dart;unsigned long sidmap[1];};
struct apple_dart_atomic_stream_map {struct apple_dart *dart;unsigned long sidmap[1];};
struct apple_dart_hw {int type,ttbr_count;u32 (*read_tcr)(struct apple_dart*,u32);int (*invalidate_tlb)(struct apple_dart_stream_map*);};
struct apple_dart {unsigned char *regs;struct apple_dart_hw *hw;struct device *dev;
    int lock;
    unsigned num_streams;u32 error_reflector,fetch_config,diag_config;bool has_fetch_config,has_diag_config;};
struct io_pgtable_cfg {struct {int n_ttbrs,n_levels;u64 ttbr[4];} apple_dart_cfg;};
struct pgtable {struct io_pgtable_cfg cfg;};
struct iommu_domain {int type;};
struct apple_dart_domain {struct pgtable *pgtbl_ops;int flush_error;struct apple_dart_atomic_stream_map stream_maps[1];struct iommu_domain domain;};
struct apple_dart_master_cfg {struct apple_dart_stream_map stream_maps[1];};
static struct apple_dart_domain domain;
static struct apple_dart_master_cfg master;
static int event,logged,acked,disabled,cleared,reflector,tables,enabled,fetch,diag,flush;
static bool bad_tcr,bad_ttbr,stuck;
static unsigned char regs[0x100];
#define io_pgtable_ops_to_pgtable(x) (x)
static void dev_info(void *d,const char *fmt,...) {if(strstr(fmt,"before ack"))logged=++event;}
#define dev_err(...) ((void)0)
static u32 readl(void *p) {
    if(p==regs)return *(u32*)p | (stuck?BIT(3):0);
    if(p==regs+0x40 && bad_ttbr && flush)return 0;
    return *(u32*)p;
}
static void writel(u32 v,void *p) {
    if(p==regs)assert(v==0x102 && *(u32*)(regs+0xc)==0x80);
    if(p==regs+0x10) {assert(logged);acked=++event;}
    if(p==regs+0x24) {assert(cleared>disabled);reflector=++event;}
    if(p==regs+0x30) {assert(enabled>tables);fetch=++event;}
    if(p==regs+0x20) {assert(fetch>enabled);diag=++event;}
    *(u32*)p=v;
}
static u32 read_tcr(struct apple_dart *d,u32 sid) {
    if(bad_tcr && enabled)return 0;
    return (*(u32*)(regs+0xc)>>(sid*8))&255;
}
static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *m) {
    disabled=++event;*(u32*)(regs+0xc)=0;
}
static void apple_dart_hw_clear_all_ttbrs(struct apple_dart_stream_map *m) {
    assert(disabled);cleared=++event;memset(regs+0x40,0,16);
}
static void apple_dart_hw_clear_ttbr(struct apple_dart_stream_map *m,int i) {*(u32*)(regs+0x40+4*i)=0;}
static void apple_dart_hw_set_ttbr(struct apple_dart_stream_map *m,int i,u64 pa) {
    assert(reflector>cleared);tables=++event;*(u32*)(regs+0x40+4*i)=BIT(31)|(pa>>12);
}
static void dma_wmb(void) {assert(tables);}
static void apple_dart_hw_enable_translation(struct apple_dart_stream_map *m,int levels) {
    assert(tables>reflector && levels==3);enabled=++event;*(u32*)(regs+0xc)=0x80;
}
static int apple_dart_s5l8960x_hw_stream_command(struct apple_dart_stream_map*,u32);
#define readl_poll_timeout_atomic(p,v,cond,delay,timeout) \
    ({ assert((timeout)==100 && (delay)==1);(v)=readl(p);(cond)?0:-ETIMEDOUT; })
static int invalidate(struct apple_dart_stream_map *m) {
    assert(diag>fetch && *(u32*)(regs+0xc)==0x80);flush=++event;
    return apple_dart_s5l8960x_hw_stream_command(m,2);
}
static bool of_machine_is_compatible(const char *s) {return true;}
static struct iommu_domain *iommu_get_domain_for_dev(struct device *d) {return &domain.domain;}
static void *dev_iommu_priv_get(struct device *d) {return &master;}
static struct apple_dart_domain *to_dart_domain(struct iommu_domain *d) {return &domain;}
static unsigned long atomic_long_read(unsigned long *p) {return *p;}
#include "production.h"
static void init(void) {
    memset(regs,0,sizeof(regs));*(u32*)(regs+0x10)=0x500;*(u32*)(regs+0x1c)=0x3bfff0d0;
    *(u32*)(regs+0x40)=0x80000001;
    event=logged=acked=disabled=cleared=reflector=tables=enabled=fetch=diag=flush=0;
    domain.flush_error=0;bad_tcr=bad_ttbr=stuck=false;
}
int main(void) {
    struct apple_dart_hw hw={.type=DART_S5L8960X,.ttbr_count=4,.read_tcr=read_tcr,.invalidate_tlb=invalidate};
    struct apple_dart dart={.regs=regs,.hw=&hw,.num_streams=1,.error_reflector=0x20fffc,
        .fetch_config=0xe0303,.diag_config=0x100,.has_fetch_config=true,.has_diag_config=true};
    struct pgtable pt={.cfg={.apple_dart_cfg={.n_ttbrs=2,.n_levels=3,.ttbr={0x8080cebc000ULL,0x800000000ULL}}}};
    domain.pgtbl_ops=&pt;domain.domain.type=IOMMU_DOMAIN_DMA;
    master.stream_maps[0]=(struct apple_dart_stream_map){.dart=&dart,.sidmap={1}};
    domain.stream_maps[0]=(struct apple_dart_atomic_stream_map){.dart=&dart,.sidmap={1}};
    init();assert(!apple_ans_dart_set_active(NULL,true));
    assert(logged<acked && acked<disabled && cleared<reflector && tables<enabled && diag<flush);
    assert(*(u32*)(regs+0x40)==(BIT(31)|(pt.cfg.apple_dart_cfg.ttbr[0]>>12)));
    assert(!apple_ans_dart_set_active(NULL,false));assert(!*(u32*)(regs+0xc));
    for(int i=0;i<3;i++) {
        init();bad_tcr=i==0;bad_ttbr=i==1;stuck=i==2;
        assert(apple_ans_dart_set_active(NULL,true)==(stuck?-ETIMEDOUT:-EIO));
        assert(domain.flush_error && !*(u32*)(regs+0xc));
    }
    init();assert(!apple_ans_dart_set_active(NULL,true));stuck=true;
    assert(apple_dart_domain_flush_tlb(&domain)==-ETIMEDOUT);
    assert(domain.flush_error==-ETIMEDOUT && !*(u32*)(regs+0xc));
    puts("PASS DART pre-ack snapshot; TCR off/TTBR clear/reflector/tables/TCR/fetch/diag/flush; readback and busy failures block DMA");
}
