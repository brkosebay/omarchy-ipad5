/* SPDX-License-Identifier: GPL-2.0-only */
/* Production stop/drain/loader against bounded, fault-injectable MMIO. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "a9-aop-experiment.h"
#include "a9-aop-memory.h"
#include "a9-aop-handoff.h"
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define EIO 5
#define EPERM 1
#define EBUSY 16
#define ENOMEM 12
#define ETIMEDOUT 110
#define ENODEV 19
#define EHOSTDOWN 112
#define GFP_KERNEL 0
#define PAGE_ALIGN(n) ALIGN(n, 16384)
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define SZ_16K 16384
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v)>>32))
#define AOP_MAIN_FW "main"
#define AOP_EXT_FW "ext"
#define AOP_DRAIN_BUDGET 64
#define AOP_DOMAINS 5
#define __iomem
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define wmb() ((void)0)
#define WARN_ON_ONCE(x) (x)
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define div_u64(a,b) ((u64)(a)/(b))
#include "cold-constants.h"
#include "a9-aop-reload.h"
enum { PHASE_PROBE, PHASE_WAIT_FIRMWARE, PHASE_WAIT_HELLO, PHASE_STOPPED, PHASE_FAILED };
struct firmware { const u8 *data; size_t size; };
struct apple_pmgr_observation { u32 raw; int genpd_status; };
struct a9_aop {
 int dev,lock,phase,irq;
 u8 *wrapper,*autoboot,*region,*pool,*ext;
 u64 region_pa,ext_dma,pool_pa;
 size_t region_size,ext_size,pool_size,pool_used;
 u32 runtime_size,fault_control,force_halt_control;
 unsigned int experiment_stage,stale_inbox,stale_outbox;
 bool force_halted,force_drained,irq_enabled,shutdown_quiesced,rx_halt,stopping;
 u8 *cold_before,*cold_after;
 u64 cold_release_ns;
 bool cold_wake_sent,inline_rx,hello;
 int cold_wake_errno;
 const char *experiment_step;
 int domains[AOP_DOMAINS];
 struct a9_aop_handoff ho;
 struct a9_aop_spi0 spi0;
 void *spi0_pmgr;
 int wake_wrapper_errno;
 bool wake_wrapper_done;
};
struct a9_aop_regs { struct a9_aop *aop; u32 values[6]; };
static u8 wrapper[0x1c000],region[0x9b000],pool[0x400000];
static u32 regs[0x1c000/4],autoboot;
static unsigned int writes,reads64,delays,copies,irq_enables,allocs;
static u32 offsets[64],values[64];
static unsigned int inbox,outbox,fault_after,change_after_pop,pop_change;
static unsigned int inbox_reads,fault_on_inbox_read;
static bool sticky_run,sticky_fifo,force_cold = true,valid_firmware = true,quiesce,handoff;
static unsigned int releases,fw_requests,cold_test,cold_defer,checkpoints,checkpoint_fail;
static u8 before[0x9b000],after[0x9b000];
static u64 fake_ns,hello_at;
static unsigned int snapshots,sends,vmallocs,capture_fault;
static int send_errno;
static bool hello_during_sample;
static u8 main_data[342336],ext_data[37024];
static const struct firmware main_fw = {main_data,sizeof(main_data)};
static const struct firmware ext_fw = {ext_data,sizeof(ext_data)};
static void log_info(int dev,const char *fmt,...) {(void)dev;(void)fmt;}
#define dev_info log_info
static bool a9_aop_dead(struct a9_aop *a) {return a->phase==PHASE_FAILED || a->stopping;}
/* AOP-private SPI0 model: reset bit 8 acknowledges after spi_ack_wait reads,
 * enable bit 0 drops spi_idle_wait reads later; releasing an acknowledged
 * reset clears the retained channel-3 bit unless spi_sticky. Every access
 * must be inside the AOP-domain PMGR guard with the CPU halted.
 *
 * Channel 3 (+0x280 C3, +0x28c C3C, +0x298 descriptor) starts at the 16:27
 * live words; the reset leaves the observed 0x00010447. A level source
 * re-latches +0x8d0 bit 6 from spi_level_ns on while C3 bit 6 is on, or
 * regardless with spi_level_masked; spi_latch_read latches it on that
 * +0x8d0 read. A C3C store with bit 5 acknowledges a latch unless
 * spi_latch_sticky; bit 5 itself self-clears (19:50: 0x00200029 written,
 * 0x00200009 read) unless spi_ack_latches. Injected faults: C3 bits that a
 * store cannot clear, idle never set, descriptor or acknowledge stores
 * ignored. spi_c3_script replays C3 reads word for word after the reset.
 */
#define SPI_C3_LIVE 0x18011445U	/* 16:27, before the reset */
#define SPI_C3_RESET 0x00010447U	/* 16:27, after the reset */
#define SPI_C3C 0x00200009U
#define SPI_DESC 0x00012340U	/* not captured at 16:27: any nonzero word */
static u8 spi0_mmio[A9_SPI0_SIZE];
static int pmgr_node;
static u32 spi_ctl,spi_fault,spi_log[8],pmgr_spi0_raw;
static unsigned int spi_reads,spi_nwrites,spi_ack_wait,spi_idle_wait,udelays,spi_fault_read_at;
static bool in_guard,spi_acked,spi_never_ack,spi_never_idle,spi_sticky,aop_domains_off;
static u32 spi_c3,spi_c3_post,spi_c3_stuck,spi_c3c,spi_desc,ch3_log[8][2];
static unsigned int ch3_nwrites,spi_fault_reads,spi_latch_read,domain_observes,domain_fail_at;
static bool spi_never_ch3_idle,spi_desc_stuck,spi_ack_dead,spi_latch_sticky,spi_level_masked;
static bool spi_ack_latches,spi_c3_script_armed;	/* armed at the teardown checkpoint */
static const u32 *spi_c3_script;
static unsigned int spi_c3_script_len,spi_c3_script_at;
static u64 spi_level_ns,window_step_ns;
static unsigned int window_sleeps;
/* The idle-poll clock: each failed atomic poll read costs delay_us. C3 idle
 * reads clear for spi_ch3_busy_ns after the stop store. */
static u64 poll_ns,spi_stop_ns,spi_ch3_busy_ns;
static u64 ktime_get_ns(void) {return poll_ns;}
/* INTC model (wake7/8): +0xa00 and +0xa80 read the group-1 bank-0 mask, which
 * the post-quiesce device showed all set; +0x81c reports 0x10014 only while
 * it is latched and unmasked; +0x980 clears the latch unless ack-sticky.
 * The event-3 pulse clears the modeled latch but does not alter SCM. This
 * tests the AP command order, not its unverified effect on silicon. intc_dead:
 * the disable store has no effect; intc_all_ones: every INTC word reads
 * ffffffff. INTC words are read only outside the guard, CPU halted. */
#define INTC_CURRENT_LATCHED 0x00010014U
static unsigned int intc_disables,intc_acks,intc_ack_fault_reads,scm_observes,scm_fail_at,intc_reads;
static u32 intc_mask;
static unsigned int window_observes,window_fail_at;
static unsigned int intc_resets,reset_assert_reads,trace_observes,trace_fail_at;
static bool intc_reset_stuck,intc_ack_sticky,scm_three,reset_assert_zero;
static u32 intc_reset_value;
static bool intc_latched,intc_dead,intc_all_ones;
static bool is_intc(size_t off) {
 return off==A9_INTC_RESET || off==A9_INTC_CURRENT || off==A9_INTC_RAISE || off==A9_INTC_ACK ||
        (off>=A9_INTC_DISABLE && off<=A9_INTC_DISABLE+8) || off==A9_INTC_ENABLE ||
        off==A9_INTC_G7_DISABLE || off==A9_INTC_G7_DISABLE+4;
}
static u32 intc_read(size_t off) {
 assert(!in_guard && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot);
 intc_reads++;
 if(intc_all_ones) return ~0U;
 if(off==A9_INTC_RESET) {
  if(intc_reset_value) reset_assert_reads++;
  return intc_reset_stuck && intc_resets ? 1 : reset_assert_zero ? 0 : intc_reset_value;
 }
 if(off==A9_INTC_CURRENT) return intc_latched && !(intc_mask&A9_INTC_SPI0_SCM)?INTC_CURRENT_LATCHED:0;
 if(off==A9_INTC_DISABLE || off==A9_INTC_ENABLE) return intc_mask;
 if(off==A9_INTC_DISABLE+4 || off==A9_INTC_DISABLE+8) return ~0U;
 if(off==A9_INTC_G7_DISABLE || off==A9_INTC_G7_DISABLE+4) return 2;
 return 0; /* +0x808 after the event-3 pulse; +0x900/+0x980 never read by firmware */
}
static bool is_spi(const void *p) {return (uintptr_t)p-(uintptr_t)spi0_mmio<sizeof(spi0_mmio);}
static u32 spi_read(size_t off) {
 assert(in_guard && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot && !(off&3));
 spi_reads++;
 if(scm_three && off==A9_SPI0_SCM_LO) return 3;
 if(scm_three && off==A9_SPI0_SCM_HI) return 0;
 if(off==A9_SPI0_FAULT) {
  spi_fault_read_at=writes;
  if(++spi_fault_reads==spi_latch_read) spi_fault|=A9_SPI0_CH3_PENDING;
  if(fake_ns>=spi_level_ns && (spi_level_masked || (spi_c3&A9_SPI0_CH3_IRQ0))) spi_fault|=A9_SPI0_CH3_PENDING;
  return spi_fault;
 }
 if(off==A9_SPI0_CH3 && spi_c3_script_armed && spi_c3_script_at<spi_c3_script_len)
  return spi_c3=spi_c3_script[spi_c3_script_at++];
 if(off==A9_SPI0_CH3) return spi_never_ch3_idle || (ch3_nwrites && poll_ns-spi_stop_ns<spi_ch3_busy_ns)?
                              spi_c3&~A9_SPI0_CH3_IDLE:spi_c3;
 if(off==A9_SPI0_CH3_0C) return spi_c3c;
 if(off==A9_SPI0_CH3_DESC) return spi_desc;
 if(off!=A9_SPI0_CONTROL) return 0x1000U+(u32)off;
 if(!(spi_ctl&A9_SPI0_RESET)) return spi_ctl;
 if(spi_never_ack || spi_ack_wait) {if(spi_ack_wait) spi_ack_wait--;return (spi_ctl&~A9_SPI0_RESET)|A9_SPI0_ENABLE;}
 if(spi_never_idle || spi_idle_wait) {if(spi_idle_wait) spi_idle_wait--;return spi_ctl|A9_SPI0_ENABLE;}
 spi_acked=true;return spi_ctl&~A9_SPI0_ENABLE;
}
/* Channel 3: only C3, C3C and the descriptor, and only after a clean reset. */
static void ch3_write(size_t off,u32 v) {
 assert(spi_nwrites==3 && !(spi_ctl&A9_SPI0_RESET) && ch3_nwrites<ARRAY_SIZE(ch3_log));
 ch3_log[ch3_nwrites][0]=(u32)off;ch3_log[ch3_nwrites++][1]=v;
 if(off==A9_SPI0_CH3) {if(ch3_nwrites==1) spi_stop_ns=poll_ns;spi_c3=v|(spi_c3&spi_c3_stuck);return;}
 if(off==A9_SPI0_CH3_DESC) {if(!spi_desc_stuck) spi_desc=v;return;}
 assert(off==A9_SPI0_CH3_0C);
 if(spi_ack_dead) v&=~A9_SPI0_CH3_ACK;
 if((v&A9_SPI0_CH3_ACK) && !spi_latch_sticky) spi_fault&=~A9_SPI0_CH3_PENDING;
 spi_c3c=spi_ack_latches?v:v&~A9_SPI0_CH3_ACK;
}
static void spi_write(size_t off,u32 v) {
 assert(in_guard && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot);
 if(off!=A9_SPI0_CONTROL) {ch3_write(off,v);return;}
 assert(spi_nwrites<8 && !ch3_nwrites); /* the reset precedes any channel store */
 spi_log[spi_nwrites++]=v;
 if(spi_acked && !(v&A9_SPI0_RESET)) spi_c3=spi_c3_post;
 if(spi_acked && !(v&A9_SPI0_RESET) && !spi_sticky) spi_fault&=~A9_SPI0_CH3_PENDING;
 if(!(v&A9_SPI0_RESET)) spi_acked=false;
 spi_ctl=v;
}
static void udelay(unsigned int us) {assert(in_guard && us==A9_SPI0_DELAY_US);udelays++;}
#define readl_poll_timeout_atomic(addr,val,cond,delay_us,timeout_us) ({ \
 int poll_ret_=-ETIMEDOUT; \
 for(unsigned int poll_i_=0;poll_i_<=(timeout_us)/(delay_us);poll_i_++) { \
  (val)=readl(addr); if(cond) {poll_ret_=0;break;} poll_ns+=(delay_us)*NSEC_PER_USEC; } \
 poll_ret_; })
static u32 readl(const void *p) {
 if(is_spi(p)) return spi_read((uintptr_t)p-(uintptr_t)spi0_mmio);
 if(p==&autoboot) return autoboot;
 size_t off=(uintptr_t)p-(uintptr_t)wrapper;
 assert(off<sizeof(wrapper) && !(off&3));
 if(off==AOP_WRAPPER_B88) return readl(wrapper+AOP_INBOX_CONTROL);
 if(is_intc(off)) return intc_read(off);
 if(off==AOP_INBOX_CONTROL && ++inbox_reads==fault_on_inbox_read) regs[off/4]|=BIT(18);
 if(off==AOP_INBOX_CONTROL) return (inbox ? inbox<<8 : AOP_MBOX_EMPTY) | (inbox>=8?AOP_MBOX_FULL:0) | 1 | (regs[off/4]&AOP_MBOX_FAULTS);
 if(off==AOP_OUTBOX_CONTROL) return regs[off/4] | (outbox?outbox<<8:AOP_MBOX_EMPTY);
 return regs[off/4];
}
static u32 aop_rd(struct a9_aop *a,u32 off) {return readl(a->wrapper+off);}
static void writel(u32 v,void *p) {
 if(is_spi(p)) {spi_write((uintptr_t)p-(uintptr_t)spi0_mmio,v);return;}
 assert(writes<64);
 size_t off=p==&autoboot?0x600:(uintptr_t)p-(uintptr_t)wrapper;
 offsets[writes]=off;values[writes++]=v;
 if(p==&autoboot) autoboot=v;
 else { assert(off!=AOP_INBOX_CONTROL); regs[off/4]=v;
  if(off==AOP_CPU_CONTROL && sticky_run) regs[off/4]|=AOP_CPU_START;
 }
 /* INTC: CPU halted, auto-boot 0. The disable precedes every SPI0 access;
  * the acknowledge follows the clean window. */
 if(p!=&autoboot && (off==A9_INTC_DISABLE || off==A9_INTC_ACK)) {
  assert(v==A9_INTC_SPI0_SCM && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot && !in_guard);
  if(off==A9_INTC_DISABLE) {assert(!spi_reads && !spi_nwrites);intc_disables++;if(!intc_dead) intc_mask|=v;}
  else {intc_acks++;intc_ack_fault_reads=spi_fault_reads;if(!intc_ack_sticky) intc_latched=false;}
 }
 if(p!=&autoboot && off==A9_INTC_RESET) {
  assert(!in_guard && !autoboot && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START));
  assert(intc_acks==1 && window_observes==31 && copies==1);
  if(!intc_resets) {assert(v==1);intc_reset_value=1;}
  else {assert(intc_resets==1 && v==0 && reset_assert_reads);intc_reset_value=0;intc_latched=false;}
  intc_resets++;
 }
 if(writes==fault_after) regs[AOP_INBOX_CONTROL/4]|=BIT(18);
}
static u64 readq(void *p) {
 size_t off=(uintptr_t)p-(uintptr_t)wrapper;
 assert(!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot && delays==1);
 assert(off==AOP_INBOX_POP || off==AOP_OUTBOX_DATA);
 unsigned int *count=off==AOP_INBOX_POP?&inbox:&outbox;
 assert(*count); if(!sticky_fifo) --*count;
 ++reads64;
 if(reads64==change_after_pop) {
  if(pop_change==0) regs[AOP_CPU_CONTROL/4]|=AOP_CPU_START;
  if(pop_change==1) autoboot=1;
  if(pop_change==2) regs[AOP_OUTBOX_CONTROL/4]|=AOP_OUTBOX_ENABLE;
  if(pop_change==3) regs[AOP_INBOX_CONTROL/4]|=BIT(18);
 }
 return reads64;
}
static void a9_aop_experiment_regs(struct a9_aop *a,const char *event) {
 (void)event;
 a->fault_control=(readl(wrapper+AOP_INBOX_CONTROL)|readl(wrapper+AOP_OUTBOX_CONTROL))&AOP_MBOX_FAULTS;
}
static void msleep(unsigned int ms) { assert(ms==AOP_HALT_DELAY_MS && ms==10 && writes==5 && !reads64 && !copies); delays++; }
static int a9_aop_control_write(struct a9_aop *,void *,u32,u32);
static void enable_irq(int irq) {(void)irq;irq_enables++;}
static int a9_aop_wait_firmware(struct a9_aop *a,const char *n,const struct firmware **fw) {
 (void)a;(void)n;fw_requests++;*fw=&main_fw;return 0;
}
static int request_firmware_direct(const struct firmware **fw,const char *n,int dev) {
 (void)n;(void)dev;fw_requests++;*fw=&ext_fw;return 0;
}
static bool a9_aop_pinned_firmware(const struct firmware *fw,bool ext) {
 assert(fw==(ext?&ext_fw:&main_fw));return valid_firmware;
}
static void *dmam_alloc_coherent(int dev,size_t n,u64 *pa,int flags) {
 (void)dev;(void)n;(void)pa;(void)flags;allocs++;return NULL;
}
static int a9_aop_prepare_image(struct a9_aop *a,const struct firmware *fw,u64 pa,u8 **img) {
 assert(pa==0x858000000ULL);a->runtime_size=0x6e000;*img=(u8 *)fw->data;return 0;
}
/* One node = the read-only ps_spi0 probe (0x200 = off, as captured); five =
 * the AOP domains, whose callback runs with the guard held.
 */
static void a9_aop_spi0_fault_read(void *data);
static void a9_aop_spi0_scm_read(void *data);
static void a9_aop_spi0_trace_read(void *data);

static int apple_pmgr_observe(void *domains,int n,struct apple_pmgr_observation *states,void (*fn)(void *),void *ctx) {
 assert(!in_guard);
 if(n==1) {
  assert(*(void **)domains==&pmgr_node);
  states[0].raw=pmgr_spi0_raw;states[0].genpd_status=0;
  if((pmgr_spi0_raw&0xff)!=0xff) return -EHOSTDOWN;
  in_guard=true;fn(ctx);in_guard=false;return 0;
 }
 assert(n==AOP_DOMAINS);(void)states;
 if(aop_domains_off || ++domain_observes==domain_fail_at) return -EHOSTDOWN;
 if(fn==a9_aop_spi0_fault_read && ++window_observes==window_fail_at) return -EHOSTDOWN;
 if(fn==a9_aop_spi0_trace_read && ++trace_observes==trace_fail_at) return -EHOSTDOWN;
 if(fn==a9_aop_spi0_scm_read && ++scm_observes==scm_fail_at) return -EHOSTDOWN;
 in_guard=true;fn(ctx);in_guard=false;return 0;
}
static void a9_aop_read_regs(void *ctx) {
 struct a9_aop_regs *r=ctx;
 r->values[0]=aop_rd(r->aop,AOP_CPU_CONTROL);r->values[1]=aop_rd(r->aop,AOP_WRAPPER_B88);
 r->values[2]=aop_rd(r->aop,AOP_INBOX_CONTROL);r->values[3]=aop_rd(r->aop,AOP_OUTBOX_CONTROL);
 r->values[5]=readl(&autoboot);
}
static int a9_aop_copy_verify(void *dst,const u8 *src,size_t n) {
 assert(!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot && !inbox && !outbox);copies++;memcpy(dst,src,n);return 0;
}
#define vfree(x) ((void)(x))
static void *vmalloc(size_t n) {assert(n==sizeof(before));return ++vmallocs&1?before:after;}
static u64 ktime_get_boottime_ns(void) {return fake_ns;}
static int a9_aop_cold_capture(struct a9_aop *a,const char *event) {
 (void)a;(void)event;snapshots++;assert(snapshots<=AOP_COLD_FRAMES);
 if(hello_during_sample && !strcmp(event,"pre-wake")) {hello_at=fake_ns+NSEC_PER_MSEC;fake_ns+=2*NSEC_PER_MSEC;}
 return snapshots==capture_fault?-EIO:0;
}
static void a9_aop_inline_rx(struct a9_aop *a) {
 assert(a->inline_rx);
 if(hello_at && fake_ns>=hello_at) a->hello=true;
}
static int a9_aop_send(struct a9_aop *a,u8 ep,u64 word) {
 assert(a->inline_rx && !a->hello && !ep && word==0x0060000000000220ULL);
 assert(fake_ns-a->cold_release_ns>=AOP_COLD_WAKE_MS*NSEC_PER_MSEC);
 assert(++sends==1);return send_errno;
}
static void usleep_range(unsigned int low,unsigned int high) {
 if(low==A9_SPI0_WINDOW_US) {
  /* Pre-RUN window: CPU halted, outside the guard; a real sleep is >= 5 ms. */
  assert(high==A9_SPI0_WINDOW_US+1000 && !in_guard && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot);
  window_sleeps++;fake_ns+=window_step_ns;return;
 }
 assert(low==500 && high==1000);fake_ns+=NSEC_PER_MSEC;
}
static void release_firmware(const struct firmware *fw) {(void)fw;releases++;}
static int a9_aop_cold_checkpoint(struct a9_aop *a,const char *step) {
 (void)a;
 if(!strcmp(step,"before-run")) assert(!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot);
 if(!strcmp(step,"run-written-before-autoboot")) assert((regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !autoboot);
 /* The begin line is flushed before the first AP access to SPI0. */
 if(!strcmp(step,"spi0-reset")) assert(!spi_reads && !spi_nwrites && !in_guard && intc_disables==1);
 /* The INTC begin line is flushed before the first INTC store and any SPI0 access. */
 if(!strcmp(step,"intc-disable")) assert(!spi_reads && !spi_nwrites && !in_guard && !intc_disables);
 /* ...and the teardown's begin line after the reset, before any C3 access. */
 if(!strcmp(step,"spi0-ch3-teardown")) {
  assert(spi_nwrites==3 && !ch3_nwrites && !in_guard && !(spi_fault&A9_SPI0_CH3_PENDING));
  spi_c3_script_armed=spi_c3_script!=NULL;
 }
 return ++checkpoints==checkpoint_fail?-EIO:0;
}
static unsigned int image_hashes;
static void sha256(const u8 *p,size_t n,u8 *out) {assert(p && n==A9_HO_SRAM_SPAN);image_hashes++;memset(out,0xa5,32);}
#include "cold-functions.h"
#include "spi0-transcript.h"
static struct a9_aop fresh(void);
/* +0x4020 at boot: DFU cold (enable set, 12/12 snapshots) and after the
 * 15:52 quiesce halt (enable cleared by the Linux stop).
 */
#define DFU_OUTBOX 0x20001U
#define QUIESCED_OUTBOX 0x21100U
static struct a9_spi0_prev parsed(const char *text) {
 struct a9_spi0_prev p={.state=A9_SPI0_PREV_VALID};
 a9_spi0_prev_parse(&p,text,strlen(text));return p;
}
/* Production decision then reset + channel-3 teardown on a halted+drained
 * AOP; 3 control and 5 channel-3 stores iff run.
 */
static struct a9_aop decide(struct a9_spi0_prev prev,u32 auto_boot,u32 outbox,enum a9_spi0_reason want) {
 struct a9_aop a=fresh();
 a.spi0.prev=prev;
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(!a9_aop_spi0_prereset(&a,auto_boot,outbox) && a.spi0.decided && a.spi0.reason==want);
 if(a9_spi0_reason_runs(want)) {
  assert(a.spi0.ran && !a.spi0.error && spi_nwrites==3 && !a.spi0.skipped);
  assert(a.spi0.before[A9_SPI0_I_FAULT]==0x40 && !a.spi0.after[A9_SPI0_I_FAULT]);
  assert(a.spi0.ch3.ran && !a.spi0.ch3.failed && ch3_nwrites==5);
  assert(a.spi0.intc.disabled && intc_disables==1 && writes==6);
 } else {
  assert(!a.spi0.ran && !spi_reads && !spi_nwrites && !strcmp(a.spi0.skipped,a9_spi0_reason_name(want)));
  assert(!a.spi0.ch3.ran && !ch3_nwrites && !intc_disables && writes==5);
 }
 return a;
}
/* Halted and drained live instance; inject SPI0 faults afterwards. */
static struct a9_aop halted(void) {
 struct a9_aop a=fresh();
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 return a;
}
static struct a9_aop fresh(void) {
 quiesce=false;
 memset(regs,0,sizeof(regs));writes=reads64=delays=copies=irq_enables=allocs=fw_requests=releases=0;
 sticky_run=sticky_fifo=false;valid_firmware=true;force_cold=true;fault_after=change_after_pop=pop_change=inbox_reads=fault_on_inbox_read=0;
 fake_ns=hello_at=0;snapshots=sends=vmallocs=capture_fault=0;send_errno=0;
 hello_during_sample=false;cold_test=cold_defer=checkpoints=checkpoint_fail=0;
 spi_ctl=0x11;spi_fault=A9_SPI0_CH3_PENDING;pmgr_spi0_raw=0x200;spi_fault_read_at=~0U;
 spi_reads=spi_nwrites=udelays=0;spi_ack_wait=2;spi_idle_wait=1;
 in_guard=spi_acked=spi_never_ack=spi_never_idle=spi_sticky=aop_domains_off=false;
 spi_c3=SPI_C3_LIVE;spi_c3_post=SPI_C3_RESET;spi_c3_stuck=0;spi_c3c=SPI_C3C;spi_desc=SPI_DESC;
 ch3_nwrites=spi_fault_reads=spi_latch_read=domain_observes=domain_fail_at=window_sleeps=0;
 window_observes=window_fail_at=0;
 intc_disables=intc_acks=intc_ack_fault_reads=scm_observes=scm_fail_at=intc_reads=0;
 intc_resets=reset_assert_reads=trace_observes=trace_fail_at=0;intc_reset_value=0;intc_reset_stuck=intc_ack_sticky=scm_three=reset_assert_zero=false;
 intc_mask=~0U;intc_latched=true;intc_dead=intc_all_ones=false;poll_ns=spi_stop_ns=spi_ch3_busy_ns=0;
 spi_never_ch3_idle=spi_desc_stuck=spi_ack_dead=spi_latch_sticky=spi_level_masked=false;
 spi_ack_latches=spi_c3_script_armed=false;spi_c3_script=NULL;spi_c3_script_len=spi_c3_script_at=0;
 /* 16:27: re-latched ~105 ms after the reset; first window sample at t=0. */
 spi_level_ns=100*NSEC_PER_MSEC;window_step_ns=A9_SPI0_WINDOW_US*1000ULL;
 memset(ch3_log,0,sizeof(ch3_log));
 put_unaligned_le16(A9_COLD_EXT_FENCE_ORIGINAL,ext_data+A9_COLD_EXT_FENCE_OFFSET);
 put_unaligned_le32(A9_COLD_SCTLR_ORIGINAL,main_data+A9_COLD_SCTLR_OFFSET);
 regs[AOP_CPU_CONTROL/4]=0x210;regs[AOP_OUTBOX_CONTROL/4]=1;
 autoboot=1;inbox=8;outbox=3;
 return (struct a9_aop){.wrapper=wrapper,.autoboot=(u8 *)&autoboot,.region=region,
 .region_pa=0x210e00000ULL,.region_size=sizeof(region),.pool=pool,.pool_pa=0x858000000ULL,
 .pool_size=sizeof(pool),.experiment_stage=2,.spi0={.base=spi0_mmio},.spi0_pmgr=&pmgr_node};
}
int main(void) {
 struct a9_aop warm=fresh();warm.stopping=true;warm.phase=PHASE_STOPPED;
 assert(a9_aop_force_stop(&warm)==-EIO && !writes);
 quiesce=true;warm.inline_rx=true;
 assert(a9_aop_force_stop(&warm)==-EIO && !writes);
 warm.shutdown_quiesced=true;
 assert(!a9_aop_force_stop(&warm) && warm.force_halted && writes==5);
 assert(!a9_aop_force_drain(&warm) && warm.force_drained);
 puts("PASS actual shutdown MMIO: stopping blocks all stores until opt-in + inline owner + power-quiesced proof; exact halt/drain sequence then succeeds");
 struct a9_aop a=fresh();sticky_run=true;
 assert(a9_aop_force_stop(&a)==-EIO && delays==1 && writes==5);
 assert(!a.force_halted && !reads64 && !copies);
 assert(a9_aop_force_drain(&a)==-EPERM && a9_aop_cold_start(&a)==-EPERM && !fw_requests);
 a=fresh();assert(!a9_aop_force_stop(&a) && delays==1 && a.force_halt_control==0x200);
 assert(aop_rd(&a,AOP_WRAPPER_B88)==0x10801); /* Real cold2 mirror, bit0 stays set. */
 assert(offsets[0]==0x4020 && !(values[0]&1));
 assert(offsets[1]==0x4000 && values[1]==0x1111 && offsets[2]==0xc00);
 assert(offsets[3]==0x44 && values[3]==0x200 && offsets[4]==0x600 && !values[4]);
 assert(!a9_aop_force_drain(&a) && a.force_drained && a.stale_inbox==8 && a.stale_outbox==3);
 assert(!a9_aop_cold_start(&a) && copies==1 && !allocs && !irq_enables && writes==21);
 assert(offsets[5]==0x18 && offsets[6]==0x20 && offsets[7]==0x28 && values[7]==0x9b000);
 assert(offsets[9]==8 && values[9]==0x10e00000 && offsets[10]==0x10 && values[10]==2);
 assert(offsets[20]==0x600 && values[20]==1 && a.ext_dma==a.pool_pa && a.pool_used==0xc000);
 assert(!image_hashes && !a.ho.sram_valid);
 /* aop_handoff=sleep: same stores/layout; fingerprint the final image, clear any old record. */
 {struct a9_ho_record stale;memset(&stale,0xff,sizeof(stale));
  a=fresh();handoff=true;a.ho.slot=&stale;a.ho.generation=7;
  assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
  assert(!a9_aop_cold_start(&a) && copies==1 && writes==21 && a.pool_used==0xc000);
  assert(image_hashes==1 && a.ho.sram_valid && !a.ho.generation && a.ho.sram_sha256[0]==0xa5);
  for(size_t i=0;i<sizeof(stale);i++) assert(!((u8 *)&stale)[i]);
  handoff=false;}
 assert(!memcmp(a.ext,ext_data,sizeof(ext_data)) && fw_requests==2 && releases==2);
 for(unsigned int fail=1;fail<=5;fail++) {a=fresh();fault_after=fail;
  assert(a9_aop_force_stop(&a)==-EIO && writes==fail && !reads64 && !copies);}
 a=fresh();assert(!a9_aop_force_stop(&a));sticky_fifo=true;
 assert(a9_aop_force_drain(&a)==-ETIMEDOUT && reads64==64 && !a.force_drained);
 a=fresh();assert(!a9_aop_force_stop(&a));assert(!a9_aop_force_drain(&a));valid_firmware=false;
 assert(a9_aop_cold_start(&a)==-EILSEQ && !copies && writes==5 && !a.pool_used);
 for(unsigned int fail=6;fail<=21;fail++){a=fresh();assert(!a9_aop_force_stop(&a));assert(!a9_aop_force_drain(&a));fault_after=fail;
  assert(a9_aop_cold_start(&a)==-EIO && writes==fail && !irq_enables);}
 a=fresh();assert(!a9_aop_force_stop(&a));assert(!a9_aop_force_drain(&a));regs[AOP_CPU_CONTROL/4]|=AOP_CPU_START;
 assert(a9_aop_cold_start(&a)==-EBUSY && !copies && writes==5);
 for(unsigned int changed=0;changed<3;changed++) {
  a=fresh();assert(!a9_aop_force_stop(&a));
  if(changed==0) regs[AOP_CPU_CONTROL/4]|=AOP_CPU_START;
  if(changed==1) autoboot=1;
  if(changed==2) regs[AOP_OUTBOX_CONTROL/4]|=AOP_OUTBOX_ENABLE;
  assert(a9_aop_force_drain(&a)==-EIO && !reads64);
  a=fresh();assert(!a9_aop_force_stop(&a));assert(!a9_aop_force_drain(&a));
  if(changed==0) regs[AOP_CPU_CONTROL/4]|=AOP_CPU_START;
  if(changed==1) autoboot=1;
  if(changed==2) regs[AOP_OUTBOX_CONTROL/4]|=AOP_OUTBOX_ENABLE;
  assert(a9_aop_cold_start(&a)==-EBUSY && !copies && writes==5);
 }
 a=fresh();assert(!a9_aop_force_stop(&a));
 /* snapshot (1), stop controls (2), final pre-pop status (3). */
 inbox_reads=0;fault_on_inbox_read=3;
 assert(a9_aop_force_drain(&a)==-EIO && !reads64 && !a.force_drained);
 for(unsigned int changed=0;changed<4;changed++) {
  const unsigned int edges[]={1,8,11};
  for(unsigned int i=0;i<ARRAY_SIZE(edges);i++) {
   a=fresh();assert(!a9_aop_force_stop(&a));pop_change=changed;change_after_pop=edges[i];
   assert(a9_aop_force_drain(&a)==-EIO && reads64==edges[i] && !a.force_drained);
   assert(a9_aop_cold_start(&a)==-EPERM && !copies && !fw_requests);
  }
 }
 a=fresh();regs[AOP_CPU_CONTROL/4]=0;autoboot=0; /* Already RUN=0 at entry. */
 assert(!a9_aop_force_stop(&a) && delays==1 && !a9_aop_force_drain(&a));
 assert(aop_rd(&a,AOP_WRAPPER_B88)==0x20001 && !a9_aop_cold_start(&a));
 /* Actual loader with each independent test; baseline remains 21 stores. */
 for (unsigned int test=1;test<=A9_COLD_TEST_MAX;test++) {
  if(!a9_aop_cold_test_valid(test)) continue;
  a=fresh();cold_test=test;
  assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
  assert(!a9_aop_cold_start(&a) && writes==21+((test&2)?2:0));
  assert(get_unaligned_le32(region+A9_COLD_SCTLR_OFFSET)==
         ((test&1)?A9_COLD_SCTLR_UNCACHED:A9_COLD_SCTLR_ORIGINAL));
  if(test&2) {
   assert(offsets[18]==AOP_IRQ_MASK_CLEAR && values[18]==0x110);
   assert(offsets[19]==AOP_IRQ_MIRROR_CLEAR && values[19]==0x110);
  }
 }
 for (unsigned int fail=19;fail<=23;fail++) {
  a=fresh();cold_test=3;
  assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));fault_after=fail;
  assert(a9_aop_cold_start(&a)==-EIO && writes==fail);
 }
 a=fresh();cold_test=1;main_data[A9_COLD_SCTLR_OFFSET]^=1;
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(a9_aop_cold_start(&a)==-EILSEQ && !copies && writes==5);
 puts("PASS cold tests 1/2/3: exact reset opcode, isolated IOP mask stores, post-store faults terminal, opcode mismatch prevents load");
 for(unsigned int mode=1;mode<=2;mode++) {
  a=fresh();cold_test=5;cold_defer=mode;
  assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
  assert(!a9_aop_cold_start(&a) && writes==17 && checkpoints==3 && !irq_enables);
  for(unsigned int i=0;i<writes;i++) assert(offsets[i]!=AOP_IRQ_MASK_CLEAR && offsets[i]!=AOP_IRQ_MIRROR_CLEAR);
  if(mode==2) {
   assert(get_unaligned_le16(a.ext+A9_COLD_EXT_FENCE_OFFSET)==A9_COLD_EXT_FENCE_BRANCH);
   assert(!memcmp(a.ext,ext_data,A9_COLD_EXT_FENCE_OFFSET));
   assert(!memcmp(a.ext+A9_COLD_EXT_FENCE_OFFSET+2,ext_data+A9_COLD_EXT_FENCE_OFFSET+2,
                  sizeof(ext_data)-A9_COLD_EXT_FENCE_OFFSET-2));
  } else assert(!memcmp(a.ext,ext_data,sizeof(ext_data)));
 }
 for(unsigned int fail=1;fail<=2;fail++) {
  a=fresh();cold_defer=1;checkpoint_fail=fail;
  assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
  assert(a9_aop_cold_start(&a)==-EIO && !autoboot);
  assert(!!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START)==(fail==2));
 }
 a=fresh();cold_defer=2;ext_data[A9_COLD_EXT_FENCE_OFFSET]^=1;
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(a9_aop_cold_start(&a)==-EILSEQ && writes==5 && !copies);
 assert(a9_aop_cold_ext_fence(ext_data,A9_COLD_EXT_FENCE_OFFSET+1)==-EILSEQ);
 puts("PASS deferred loader: both AP masks retained, no Linux IRQ enable, flushed checkpoints before RUN/autoboot, checkpoint failures terminal, exact two-byte pinned post-PRNG fence");
 /* aop_spi0_reset=1: exact 0102f2f4 replay, only on a halted+drained live instance. */
 a=fresh();
 assert(!a9_aop_spi0_prereset(&a,0,DFU_OUTBOX) && !strcmp(a.spi0.skipped,"fresh-instance"));
 assert(!a.spi0.ran && !spi_reads && !spi_nwrites && a.spi0.reason==A9_SPI0_FRESH);
 assert(!intc_disables && !a.spi0.intc.disabled && !a.spi0.intc.failed); /* skip: no INTC store */
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EPERM && !spi_reads && !intc_disables); /* live, not halted */
 assert(!a9_aop_force_stop(&a));
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EPERM && !spi_reads && !intc_disables); /* not drained */
 assert(!a9_aop_force_drain(&a) && writes==5);
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && a.spi0.ran && !a.spi0.error);
 /* Round 4: IRQ 0x10014 disabled first, the 6th store, before any SPI0 access. */
 assert(intc_disables==1 && offsets[5]==A9_INTC_DISABLE && values[5]==A9_INTC_SPI0_SCM);
 assert(a.spi0.intc.disabled && !a.spi0.intc.acked && !a.spi0.intc.failed);
 /* wake6 on the device: the quiesce left every bank-0 source masked, so
  * the store changes nothing and bit 20 reads 1 before and after. */
 {const struct a9_aop_intc *n=&a.spi0.intc;
  assert(n->states==3 && n->state[A9_INTC_AT_PRE_DISABLE][A9_INTC_I_MASK0]==~0U);
  assert(n->state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_MASK0]==~0U && n->state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_ENABLE0]==~0U);
  assert(!n->state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_CURRENT] && n->state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_G7_0]==2);
  assert(!strcmp(a9_intc_view(n->state[A9_INTC_AT_POST_DISABLE]),"intc") && intc_reads==2*A9_INTC_REGS+2);}
 assert(a.spi0.reason==A9_SPI0_LIVE_AUTO_BOOT && a.spi0.auto_boot==1 && a.spi0.outbox==DFU_OUTBOX);
 assert(spi_nwrites==3 && spi_log[0]==0x110 && spi_log[1]==0x10 && spi_log[2]==0x11 && udelays==2);
 assert(a.spi0.saved==0x11 && a.spi0.asserted==0x110 && !(a.spi0.polled&A9_SPI0_ENABLE));
 assert(a.spi0.before[A9_SPI0_I_FAULT]==0x40 && !a.spi0.after[A9_SPI0_I_FAULT]);
 assert(a.spi0.before[A9_SPI0_I_CLOCK]==0x1008 && a.spi0.before[4]==SPI_C3_LIVE && a.spi0.before[5]==SPI_C3C);
 assert(a.spi0.after[4]==SPI_C3_RESET && a.spi0.after[5]==SPI_C3C);
 assert(a.spi0.after[A9_SPI0_I_CONTROL]==0x11 && writes==6); /* only the INTC disable added */
 assert(a.spi0.pmgr_before==0x200 && a.spi0.pmgr_before_errno==-EHOSTDOWN && a.spi0.pmgr_after_errno==-EHOSTDOWN);
 /* ds-aop-spi0-ch3-round3.md section 5, from the observed post-reset words:
  * C3 0x00010447 -> 0x00010046 -> 0x00010006, C3C 0x00200009 |= 0x20 (a
  * self-clearing pulse, reads 0x00200009 again), descriptor -> 0; nothing
  * else in the block is written. */
 {static const u32 want[5][2]={{A9_SPI0_CH3,0x00010046},{A9_SPI0_CH3,0x00010006},{A9_SPI0_CH3,0x00010006},
                               {A9_SPI0_CH3_DESC,0},{A9_SPI0_CH3_0C,0x00200029}};
  const struct a9_aop_spi0_ch3 *c=&a.spi0.ch3;
  assert(ch3_nwrites==5 && !memcmp(ch3_log,want,sizeof(want)));
  assert(c->ran && !c->failed && !c->error && !c->poll && (c->idle&A9_SPI0_CH3_IDLE));
  assert(c->run_before && c->slices==1 && !c->idle_us && c->slice[0].c3==0x00010046);
  assert(c->slice[0].scm_lo==0x1000+A9_SPI0_SCM_LO && c->slice[0].scm_hi==0x1000+A9_SPI0_SCM_HI && !c->slice[0].fault);
  assert(c->stop==0x00010046 && c->class0==0x00010006 && c->class1==0x00010006 && c->ack==0x00200029);
  assert(c->before[A9_CH3_I_C3]==SPI_C3_RESET && c->before[A9_CH3_I_C3C]==SPI_C3C && c->before[A9_CH3_I_DESC]==SPI_DESC);
  assert(c->before[A9_CH3_I_ROUTE]==0x1284 && c->before[A9_CH3_I_RATE]==0x1288 && c->before[A9_CH3_I_8D4]==0x18d4);
  assert(!c->before[A9_CH3_I_8D0] && !c->after[A9_CH3_I_8D0]);
  assert(c->after[A9_CH3_I_C3]==0x00010006 && c->after[A9_CH3_I_C3C]==SPI_C3C && !c->after[A9_CH3_I_DESC]);
  assert(c->after[A9_CH3_I_ROUTE]==0x1284 && c->after[A9_CH3_I_RATE]==0x1288 && writes==6);}
 /* The proven deferred loader, plus 31 guarded +0x8d0 reads >= 5 ms apart
  * after the outbox enable (store 15), then INTC ack (16), reset pulse
  * (17/18) and RUN (19). The 16:27 level source (100 ms) stays quiet:
  * teardown cleared C3 bit 6. */
 cold_test=5;cold_defer=1;
 assert(!a9_aop_cold_start(&a) && writes==21 && checkpoints==3 && !a.spi0.reasserted);
 assert(spi_fault_read_at==19 && offsets[16]==A9_INTC_ACK && values[16]==A9_INTC_SPI0_SCM);
 assert(offsets[19]==AOP_CPU_CONTROL && (values[19]&AOP_CPU_START));
 assert(offsets[20]==0x600 && values[20]==1 && spi_nwrites==3 && ch3_nwrites==5);
 assert(a.spi0.ch3.window_done && a.spi0.ch3.samples==31 && window_sleeps==30);
 assert(a.spi0.ch3.elapsed_ms==150 && fake_ns==150*NSEC_PER_MSEC && spi_fault_reads==12+31);
 assert(!a.spi0.ch3.failed && !a.spi0.ch3.reasserted && !a.spi0.prerun);
 for(unsigned int i=0;i<writes;i++) assert(offsets[i]!=AOP_CPU_CONTROL || i==3 || i==19); /* RUN once */
 /* The acknowledge follows the window's last +0x8d0 read; +0x81c, SCM and the C3
  * interval pair are logged around it (values recorded, never compared). */
 assert(intc_acks==1 && intc_ack_fault_reads==10+31 && scm_observes==2 && !intc_latched);
 assert(a.spi0.intc.acked && !a.spi0.intc.failed && a.spi0.intc.states==0xf);
 /* 0x10014 stays masked to RUN: +0x81c cannot show it pending. */
 assert(!a.spi0.intc.state[A9_INTC_AT_PRE_ACK][A9_INTC_I_CURRENT] && !a.spi0.intc.state[A9_INTC_AT_POST_ACK][A9_INTC_I_CURRENT]);
 assert(a.spi0.intc.state[A9_INTC_AT_PRE_ACK][A9_INTC_I_MASK0]&A9_INTC_SPI0_SCM);
 for(unsigned int i=0;i<A9_SPI0_SCM_REGS;i++)
  assert(a.spi0.intc.scm[0][i]==0x1000+a9_spi0_scm_offsets[i] && a.spi0.intc.scm[1][i]==a.spi0.intc.scm[0][i]);
 /* Re-raised between reset and RUN: no acknowledge, RUN, auto-boot or checkpoint. */
 a=fresh();cold_test=5;cold_defer=1;
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a) && !a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 assert(checkpoints==3); /* intc-disable, spi0-reset, spi0-ch3-teardown */
 spi_fault|=A9_SPI0_CH3_PENDING;
 assert(a9_aop_cold_start(&a)==-EIO && a.spi0.reasserted && writes==16 && checkpoints==3);
 assert(!autoboot && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && !intc_acks && !a.spi0.intc.acked);
 assert(a.spi0.ch3.reasserted && !a.spi0.ch3.reassert_sample && !a.spi0.ch3.reassert_ms && a.spi0.ch3.samples==1);
 assert(!strcmp(a.spi0.ch3.failed,"failed-ch3-reasserted") && !a.spi0.ch3.window_done);
 assert(a.spi0.ch3.reassert[A9_CH3_I_8D0]==0x40 && a.spi0.ch3.reassert[A9_CH3_I_C3]==0x00010006);
 /* No reload without a reset: a proven fresh instance never reads SPI0 at all. */
 a=fresh();cold_test=5;cold_defer=1;
 assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a) && !a9_aop_spi0_prereset(&a,0,DFU_OUTBOX));
 assert(!a9_aop_cold_start(&a) && writes==17 && !spi_reads && !window_sleeps && !a.spi0.ch3.samples);
 assert(!intc_disables && !intc_acks && intc_latched && !intc_reads); /* no INTC access */
 /* Bounded polls: bit 8 never set / bit 0 never clear; saved word restored, no delay. */
 for(unsigned int mode=0;mode<2;mode++) {
  a=fresh();assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
  if(mode) spi_never_idle=true; else spi_never_ack=true;
  assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-ETIMEDOUT && a.spi0.ran && a.spi0.error==-ETIMEDOUT);
  assert(spi_nwrites==2 && spi_log[0]==0x110 && spi_log[1]==0x11 && !udelays);
  assert(a.spi0.after[A9_SPI0_I_FAULT]==0x40 && !!(a.spi0.polled&A9_SPI0_RESET)==mode);
  assert(!ch3_nwrites && !a.spi0.ch3.ran && !a.spi0.ch3.failed); /* no teardown after a failed reset */
 }
 /* Channel-3 bit survives the reset: refused, the load must not follow. */
 a=fresh();assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));spi_sticky=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && spi_nwrites==3 && udelays==2);
 assert(a.spi0.after[A9_SPI0_I_FAULT]==0x40 && !ch3_nwrites && !a.spi0.ch3.ran && !a.spi0.ch3.failed);
 /* Deferred mode flushes each begin line first; a failed flush means no access. */
 a=fresh();cold_defer=1;assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && checkpoints==3 && spi_nwrites==3 && ch3_nwrites==5);
 a=fresh();cold_defer=1;checkpoint_fail=1;assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !a.spi0.ran && !spi_reads && !spi_nwrites);
 assert(writes==5 && !intc_disables && !strcmp(a.spi0.intc.failed,"failed-intc-disable-checkpoint"));
 a=fresh();cold_defer=1;checkpoint_fail=2;assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !a.spi0.ran && !spi_reads && !spi_nwrites);
 assert(intc_disables==1 && !a.spi0.intc.failed);
 a=fresh();cold_defer=1;checkpoint_fail=3;assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && a.spi0.ran && spi_nwrites==3 && !ch3_nwrites);
 assert(!a.spi0.ch3.ran && !strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-checkpoint") && spi_fault_reads==4);
 /* AOP domains not verifiably on: SPI0 untouched. */
 a=fresh();assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));aop_domains_off=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EHOSTDOWN && !a.spi0.ran && !spi_reads && !spi_nwrites);
 assert(!ch3_nwrites && !a.spi0.ch3.ran && !a.spi0.ch3.failed);
 /* ...or not on for the teardown only: reset done, channel 3 untouched. */
 a=halted();domain_fail_at=domain_observes+4;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EHOSTDOWN && a.spi0.ran && spi_nwrites==3);
 assert(!ch3_nwrites && !a.spi0.ch3.ran && !strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-guard"));
 /* Nothing pending: the reset still runs; ps_spi0 on or absent is only logged. */
 a=fresh();assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));spi_fault=0;pmgr_spi0_raw=0xff;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && spi_nwrites==3 && !a.spi0.pmgr_before_errno && a.spi0.pmgr_before==0xff);
 assert(ch3_nwrites==5 && !a.spi0.ch3.failed);
 a=fresh();assert(!a9_aop_force_stop(&a) && !a9_aop_force_drain(&a));a.spi0_pmgr=NULL;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && a.spi0.pmgr_before_errno==-ENODEV && a.spi0.pmgr_after_errno==-ENODEV);
 puts("PASS aop_spi0_reset channel-3 teardown: after the clean 110/10/11 reset and its own flushed begin line, inside the AOP PMGR guard, exact RMW C3 00010447->00010046->00010006 (twice), desc->0, C3C 00200029 written (a pulse, reads 00200009), route/rate/8d0/8d4 never written; none after a failed reset, failed flush or refused guard");
 /*
  * Teardown readback refusals: each returns nonzero with a distinct reason,
  * so the boot work never loads, RUNs or retries.
  */
 a=halted();spi_never_ch3_idle=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-ETIMEDOUT && a.spi0.ch3.ran && a.spi0.ch3.poll==-ETIMEDOUT);
 /* 20 slices of 1 ms (1001 reads each here), every one sampled, no idle time. */
 assert(a.spi0.ch3.slices==A9_SPI0_CH3_SLICES && !a.spi0.ch3.idle_us && spi_fault_reads==4+1+20+1);
 assert(a.spi0.ch3.slice[19].ns==20*1001*NSEC_PER_USEC && !(a.spi0.ch3.slice[19].c3&A9_SPI0_CH3_IDLE));
 /* The RMW stop keeps the read-only idle bit as read (clear here). */
 assert(!strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-idle") && ch3_nwrites==1 && ch3_log[0][1]==0x00010044);
 assert(!a.spi0.ch3.class0 && !a.spi0.ch3.ack && a.spi0.ch3.after[A9_CH3_I_C3C]==SPI_C3C);
 /* Idle late: within the firmware's 10 ms bound, past it (still idle within
  * the AP's 20 ms: passes, flagged by idle_us), or past 20 ms (refused). */
 {static const struct {u64 busy_us,idle_us;unsigned int slices;int ret;} late[]={
   {7000,7000,7,0},{15000,15000,15,0},{25000,0,20,-ETIMEDOUT}};
  for(unsigned int i=0;i<ARRAY_SIZE(late);i++) {
   const struct a9_aop_spi0_ch3 *c;
   a=halted();spi_ch3_busy_ns=late[i].busy_us*NSEC_PER_USEC;
   assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==late[i].ret);
   c=&a.spi0.ch3;
   assert(c->ran && c->slices==late[i].slices && c->idle_us==late[i].idle_us && c->poll==late[i].ret);
   assert(spi_fault_reads==4+1+late[i].slices+1+(late[i].ret?0:1) && ch3_nwrites==(late[i].ret?1:5));
   for(unsigned int j=0;j<c->slices;j++)
    assert(!!(c->slice[j].c3&A9_SPI0_CH3_IDLE)==(!late[i].ret && j==c->slices-1));
   assert(late[i].ret ? !strcmp(c->failed,"failed-ch3-teardown-idle") : !c->failed);
   assert((c->idle_us>A9_SPI0_CH3_IDLE_FW_US)==(i==1));
  }}
 /*
  * The wake6 device trace (23:25): C3 01011445 before the stop, 01011044
  * stored, then 02010444 (idle clear) on every read. It took 1 ms then; it
  * now gets 20 sampled slices and still refuses, with no mask store.
  */
 {static const u32 c3_reads[]={0x01011445,0x01011445,0x02010444};
  const struct a9_aop_spi0_ch3 *c;
  a=halted();cold_test=5;cold_defer=1;spi_desc=0;spi_latch_read=5;
  spi_c3_script=c3_reads;spi_c3_script_len=ARRAY_SIZE(c3_reads);
  assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-ETIMEDOUT);
  c=&a.spi0.ch3;
  assert(c->ran && c->run_before && c->stop==0x01011044 && c->idle==0x02010444 && ch3_nwrites==1);
  assert(c->slices==A9_SPI0_CH3_SLICES && !strcmp(c->failed,"failed-ch3-teardown-idle"));
  for(unsigned int j=0;j<c->slices;j++) assert(c->slice[j].c3==0x02010444);
  assert(c->after[A9_CH3_I_C3]==0x02010444 && !copies && checkpoints==3);}
 puts("PASS channel-3 idle wait: 1 ms slices up to 20 ms (firmware SCMto bound 10 ms), C3/SCM/8d0 sampled per slice; idle at 7 ms passes, at 15 ms passes flagged beyond the firmware bound, never (or the wake6 02010444 trace) refuses failed-ch3-teardown-idle after 20 slices with only the stop stored");
 {static const struct {u32 post,stuck;const char *failed;} stuck[]={
   {SPI_C3_RESET,A9_SPI0_CH3_RUN,"failed-ch3-teardown-run"},
   {SPI_C3_RESET,A9_SPI0_CH3_IRQ0,"failed-ch3-teardown-class0"},
   {SPI_C3_RESET|A9_SPI0_CH3_IRQ1,A9_SPI0_CH3_IRQ1,"failed-ch3-teardown-class1"}};
  for(unsigned int i=0;i<ARRAY_SIZE(stuck);i++) {
   a=halted();spi_c3_post=stuck[i].post;spi_c3_stuck=stuck[i].stuck;
   assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && a.spi0.ch3.ran && !a.spi0.ch3.poll);
   assert(!strcmp(a.spi0.ch3.failed,stuck[i].failed) && a.spi0.ch3.error==-EIO && ch3_nwrites==5);
   assert(a.spi0.ch3.after[A9_CH3_I_C3]&stuck[i].stuck);
  }}
 a=halted();spi_desc_stuck=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-desc"));
 assert(a.spi0.ch3.after[A9_CH3_I_DESC]==SPI_DESC && ch3_nwrites==5);
 /* The acknowledge is judged by +0x8d0 bit 6 alone. A C3C bit 5 that reads
  * back set (latching) or clear (pulse) is only logged; one that has no
  * effect passes when nothing is pending and refuses when bit 6 is. */
 a=halted();spi_ack_latches=true;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && !a.spi0.ch3.failed);
 assert(a.spi0.ch3.after[A9_CH3_I_C3C]==0x00200029);
 a=halted();spi_ack_dead=true;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && !a.spi0.ch3.failed && ch3_log[4][1]==0x00200029);
 a=halted();spi_ack_dead=true;spi_latch_read=5;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-pending"));
 /* Re-latched between the reset and the teardown: the acknowledge clears it... */
 a=halted();spi_latch_read=5;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && !a.spi0.ch3.failed);
 assert(a.spi0.ch3.before[A9_CH3_I_8D0]==0x40 && !a.spi0.ch3.after[A9_CH3_I_8D0]);
 /* ...unless it stays latched: refused while still halted. */
 a=halted();spi_latch_read=5;spi_latch_sticky=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !strcmp(a.spi0.ch3.failed,"failed-ch3-teardown-pending"));
 assert(a.spi0.ch3.after[A9_CH3_I_8D0]==0x40 && ch3_log[4][1]==0x00200029);
 puts("PASS aop_spi0_reset channel-3 readbacks: idle timeout (one store, masks skipped), run/class0/class1 bit, descriptor and still-pending 8d0 each refuse with their own failed-ch3-teardown-* reason; C3C bit 5 read back set or clear is only logged; a latch between reset and teardown is acknowledged");
 /*
  * The 19:50 device trace word for word, which a9/aop-wake4 refused as
  * failed-ch3-teardown-ack: bit 6 re-pended before the teardown, C3 status
  * bits moved under the stores, and C3C read 0x00200009 after 0x00200029
  * while +0x8d0 went 0x40 -> 0. It must now pass to the window and RUN;
  * the same trace with bit 6 still set afterwards must refuse.
  */
 {static const u32 c3_reads[]={0x01011445,0x01011445,0x02010446,0x02010446,0x02010006,0x00010406};
  static const u32 want[5][2]={{A9_SPI0_CH3,0x01011044},{A9_SPI0_CH3,0x02010006},{A9_SPI0_CH3,0x02010006},
                               {A9_SPI0_CH3_DESC,0},{A9_SPI0_CH3_0C,0x00200029}};
  for(unsigned int sticky=0;sticky<2;sticky++) {
   const struct a9_aop_spi0_ch3 *c;
   int ret;
   a=halted();cold_test=5;cold_defer=1;spi_desc=0;spi_latch_read=5;spi_latch_sticky=sticky;
   spi_c3_script=c3_reads;spi_c3_script_len=ARRAY_SIZE(c3_reads);
   ret=a9_aop_spi0_prereset(&a,1,DFU_OUTBOX);
   c=&a.spi0.ch3;
   assert(c->ran && ch3_nwrites==5 && !memcmp(ch3_log,want,sizeof(want)));
   assert(spi_c3_script_at==ARRAY_SIZE(c3_reads) && !c->poll);
   assert(a.spi0.before[4]==SPI_C3_LIVE && a.spi0.after[4]==SPI_C3_RESET && !a.spi0.after[A9_SPI0_I_FAULT]);
   assert(c->before[A9_CH3_I_C3]==0x01011445 && c->before[A9_CH3_I_C3C]==0x00200009);
   assert(!c->before[A9_CH3_I_DESC] && c->before[A9_CH3_I_8D0]==0x40);
   assert(c->stop==0x01011044 && c->idle==0x02010446 && c->class0==0x02010006);
   assert(c->class1==0x02010006 && c->ack==0x00200029);
   assert(c->after[A9_CH3_I_C3]==0x00010406 && c->after[A9_CH3_I_C3C]==0x00200009 && !c->after[A9_CH3_I_DESC]);
   if(sticky) {
    assert(ret==-EIO && c->error==-EIO && !strcmp(c->failed,"failed-ch3-teardown-pending"));
    assert(c->after[A9_CH3_I_8D0]==0x40 && !copies);
    continue;
   }
   assert(!ret && !c->failed && !c->error && !c->after[A9_CH3_I_8D0] && checkpoints==3);
   assert(!a9_aop_cold_start(&a) && copies==1 && writes==21 && checkpoints==6);
   assert(c->window_done && c->samples==31 && c->elapsed_ms==150 && !c->reasserted && !c->failed);
   assert(offsets[16]==A9_INTC_ACK && offsets[19]==AOP_CPU_CONTROL && (values[19]&AOP_CPU_START));
   assert(offsets[20]==0x600 && values[20]==1 && a.spi0.intc.acked);
  }}
 puts("PASS 19:50 channel-3 trace: C3 01011445 -> 01011044 / idle 02010446 -> 02010006 x2 -> 00010406, desc 0, C3C 00200029 written / 00200009 read, +0x8d0 40 -> 0 passes the teardown, the 31-sample 150 ms window and RUN once; the same trace with bit 6 still set refuses failed-ch3-teardown-pending before the copy");
 /*
  * Pre-RUN window after the copy: >= 150 ms, every sample guarded.
  * Refusals leave the INTC acknowledge, RUN and auto-boot unwritten (16
  * stores: halt 5, INTC disable 1, loader 10) and never retry.
  */
 {static const struct {u64 level_ms;bool masked,c3_irq0;unsigned int sample;} late[]={
   {100,true,false,20},	/* reasserts despite the mask, 16:27 timing */
   {150,true,false,30},	/* only at the last sample: the window spans 150 ms */
   {100,false,true,20}};	/* class-0 enable back on: the 16:27 failure itself */
  for(unsigned int i=0;i<ARRAY_SIZE(late);i++) {
   a=halted();cold_test=5;cold_defer=1;spi_level_ns=late[i].level_ms*NSEC_PER_MSEC;spi_level_masked=late[i].masked;
   assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && !a.spi0.ch3.failed);
   if(late[i].c3_irq0) spi_c3|=A9_SPI0_CH3_IRQ0;
   assert(a9_aop_cold_start(&a)==-EIO && writes==16 && !autoboot && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START));
   assert(!strcmp(a.spi0.ch3.failed,"failed-ch3-reasserted") && a.spi0.reasserted && a.spi0.ch3.reasserted);
   assert(a.spi0.ch3.reassert_sample==late[i].sample && a.spi0.ch3.reassert_ms==late[i].level_ms);
   assert(a.spi0.ch3.samples==late[i].sample+1 && window_sleeps==late[i].sample && !a.spi0.ch3.window_done);
   assert(a.spi0.ch3.reassert[A9_CH3_I_8D0]==0x40 && a.spi0.ch3.reassert[A9_CH3_I_C3C]==SPI_C3C);
   assert(!a.spi0.ch3.reassert[A9_CH3_I_DESC] && checkpoints==3 && copies==1 && !intc_acks);
  }}
 /* A refused guard mid-window: no RUN. */
 a=halted();cold_test=5;cold_defer=1;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 window_fail_at=8;
 assert(a9_aop_cold_start(&a)==-EHOSTDOWN && writes==16 && !autoboot && a.spi0.ch3.samples==8);
 assert(!strcmp(a.spi0.ch3.failed,"failed-ch3-window-guard") && !a.spi0.ch3.reasserted && !intc_acks);
 /* A clock that says < 150 ms after 31 samples: refused, not trusted. */
 a=halted();cold_test=5;cold_defer=1;window_step_ns=4*NSEC_PER_MSEC;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 assert(a9_aop_cold_start(&a)==-EIO && writes==16 && !autoboot && a.spi0.ch3.samples==31);
 assert(a.spi0.ch3.elapsed_ms==120 && !strcmp(a.spi0.ch3.failed,"failed-ch3-window-short") && !intc_acks);
 puts("PASS aop_spi0_reset pre-RUN window: 31 guarded samples, 30 sleeps >= 5 ms, 150 ms after the copy, RUN once on pass; reassert despite the mask (100 ms, last sample at 150 ms) or with class-0 back on refuses failed-ch3-reasserted with sample/time/register snapshot; refused guard or short clock refuse; no INTC acknowledge, RUN, auto-boot or retry");
 /*
  * Round 4 (ds-aop-spi0-ch3-round4.md section 3): IRQ 0x10014 disabled at
  * +0xa00 before the first SPI0 access and acknowledged at +0x980 after the
  * clean window, just before RUN, both 0x00100000 and outside the guard.
  * Refusals name their step; a refused store or guard never reaches RUN.
  */
 /* A fault on the disable store: no SPI0 access, no load. */
 a=halted();fault_after=6;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && !a.spi0.ran && !spi_reads && !spi_nwrites);
 assert(intc_disables==1 && !a.spi0.intc.disabled && !strcmp(a.spi0.intc.failed,"failed-intc-disable"));
 /* A fault on the acknowledge store (the 17th): no RUN, no auto-boot. */
 a=halted();cold_test=5;cold_defer=1;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 fault_after=17;
 assert(a9_aop_cold_start(&a)==-EIO && writes==17 && offsets[16]==A9_INTC_ACK && !autoboot);
 assert(!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START) && a.spi0.ch3.window_done);
 assert(!a.spi0.intc.acked && !strcmp(a.spi0.intc.failed,"failed-intc-ack"));
 /* AOP domains not verifiably on for the pre-ack reads: no acknowledge, no RUN. */
 a=halted();cold_test=5;cold_defer=1;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 scm_fail_at=1;
 assert(a9_aop_cold_start(&a)==-EHOSTDOWN && writes==16 && !intc_acks && !autoboot);
 assert(!strcmp(a.spi0.intc.failed,"failed-intc-ack-guard") && a.spi0.ch3.window_done);
 /* ...or for the post-ack reads: acknowledged, RUN still refused. */
 a=halted();cold_test=5;cold_defer=1;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
 scm_fail_at=2;
 assert(a9_aop_cold_start(&a)==-EHOSTDOWN && writes==17 && intc_acks==1 && !autoboot);
 assert(a.spi0.intc.acked && !strcmp(a.spi0.intc.failed,"failed-intc-post-guard"));
 assert(!(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START));
 /* The disable lands even if the reset guard then refuses; nothing else does. */
 a=halted();aop_domains_off=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EHOSTDOWN && intc_disables==1 && !spi_reads && writes==6);
 /* Wake7: the mask readback decides. Unmasked before: bit 20 reads 0, then 1. */
 a=halted();intc_mask=0;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && a.spi0.intc.disabled && !a.spi0.intc.failed);
 assert(!a.spi0.intc.state[A9_INTC_AT_PRE_DISABLE][A9_INTC_I_MASK0]);
 assert(a.spi0.intc.state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_MASK0]==A9_INTC_SPI0_SCM);
 assert(a.spi0.intc.state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_ENABLE0]==A9_INTC_SPI0_SCM && spi_nwrites==3);
 /* Latched and unmasked, +0x81c reports it; once masked it cannot. */
 assert(a.spi0.intc.state[A9_INTC_AT_PRE_DISABLE][A9_INTC_I_CURRENT]==INTC_CURRENT_LATCHED);
 assert(!a.spi0.intc.state[A9_INTC_AT_POST_DISABLE][A9_INTC_I_CURRENT]);
 /* A store that does not stick: refused before any SPI0 access, nothing loaded. */
 a=halted();intc_mask=0;intc_dead=true;
 assert(a9_aop_spi0_prereset(&a,1,DFU_OUTBOX)==-EIO && intc_disables==1 && writes==6);
 assert(!a.spi0.intc.disabled && !strcmp(a.spi0.intc.failed,"failed-intc-disable-readback"));
 assert(!a.spi0.ran && !spi_reads && !spi_nwrites && a.spi0.intc.states==3);
 /* All-ones everywhere (+0x81c included) is not an INTC view: bit 20 still
  * reads 1, so it passes, and the state lines say view=all-ones. */
 a=halted();intc_all_ones=true;
 assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX) && a.spi0.intc.disabled && ch3_nwrites==5);
 assert(!strcmp(a9_intc_view(a.spi0.intc.state[A9_INTC_AT_POST_DISABLE]),"all-ones"));
 assert(!strcmp(a9_intc_view(a.spi0.intc.state[A9_INTC_AT_PRE_DISABLE]),"all-ones"));
 for(unsigned int at=0;at<A9_INTC_AT_N;at++) assert(strcmp(a9_intc_at_name(at),"unknown"));
 assert(!strcmp(a9_intc_at_name(A9_INTC_AT_N),"unknown"));
 puts("PASS round-4 INTC: 0x10014 disabled (+0xa00=00100000) after the flushed begin line and before every SPI0 access, acknowledged (+0x980=00100000) after the last window read and before RUN; skip path, window refusals, faulted stores, refused guards and checkpoint failures give their own reasons with no RUN");
 puts("PASS wake7 INTC readback: ten INTC words snapshotted around both stores; +0xa00 bit 20 must read 1 after the disable (post-quiesce ffffffff passes, 0->00100000 passes, a store that does not stick refuses failed-intc-disable-readback before any SPI0 access); +0x81c shows 0x10014 only unmasked; all-ones is labelled view=all-ones; SCM/interval/+0x81c/+0x980 logged not gated");
 /* Wake8: model a latch the targeted ack does not clear. This checks the
  * AP sequence, not the untested silicon effect of the reset. SCM=3 stays
  * observational and must not prevent RUN. Both first-read 0 and 1 pass. */
 for(unsigned int first=0;first<2;first++) {
  a=halted();cold_test=5;cold_defer=1;intc_ack_sticky=true;scm_three=true;reset_assert_zero=first;
  assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
  assert(!a9_aop_cold_start(&a) && writes==21 && intc_resets==2 && reset_assert_reads);
  assert(offsets[16]==A9_INTC_ACK && offsets[17]==A9_INTC_RESET && values[17]==1);
  assert(offsets[18]==A9_INTC_RESET && values[18]==0 && offsets[19]==AOP_CPU_CONTROL);
  assert(a.spi0.intc.reset_done && !a.spi0.intc.reset_after && !intc_latched && trace_observes==7);
  assert(a.spi0.trace.scm[0]==3 && !a.spi0.trace.scm[1]);
 }
 /* Every snapshot guard is terminal: no stale trace permits a later RUN. */
 for(unsigned int fail=1;fail<=7;fail++) {
  a=halted();cold_test=5;cold_defer=1;trace_fail_at=fail;
  int ret=a9_aop_spi0_prereset(&a,1,DFU_OUTBOX);
  if(!ret) ret=a9_aop_cold_start(&a);
  assert(ret==-EHOSTDOWN && !autoboot && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START));
  assert(!strcmp(a.spi0.intc.failed,"failed-spi0-trace-guard") && !a.spi0.intc.reset_done);
  assert(intc_resets==(fail==7?2:0));
 }
 /* Fault after either reset store, stuck reset, and a peripheral source
  * appearing during the pulse all refuse RUN, without a second pulse. */
 for(unsigned int fail=0;fail<4;fail++) {
  a=halted();cold_test=5;cold_defer=1;
  assert(!a9_aop_spi0_prereset(&a,1,DFU_OUTBOX));
  if(fail<2) fault_after=18+fail;
  else if(fail==2) intc_reset_stuck=true;
  else spi_latch_read=43; /* final trace's +0x8d0, after the pulse */
  assert(a9_aop_cold_start(&a)==-EIO && !autoboot && !(regs[AOP_CPU_CONTROL/4]&AOP_CPU_START));
  const char *why[]={"failed-intc-reset-assert","failed-intc-reset-deassert",
                     "failed-intc-reset-readback","failed-intc-reset-ch3"};
  assert(!strcmp(a.spi0.intc.failed,why[fail]) && !a.spi0.intc.reset_done);
  assert(intc_resets==(fail?2:1) && writes==(fail?19:18));
 }
 a=halted();
 assert(a9_aop_intc_reset(&a)==-EPERM && writes==5 && !intc_resets && !trace_observes);
 puts("PASS wake8: seven guarded read-only SPI0/SCM milestones; exact event-3 INTC 1/read/0/read after ack/window/copy and before RUN once; SCM=3 and first reset read 0 or 1 pass; every trace guard, either faulted store, nonzero final readback and post-pulse channel fault refuse RUN; unprepared call refuses without access");
 /*
  * Retained-state decision on the real 15:52 transcript (63 lines, 3479
  * bytes): its quiesce halt cleared auto-boot, which skipped the reset.
  */
 {struct a9_spi0_prev real=parsed(spi0_transcript),none={0},bad={.state=A9_SPI0_PREV_INVALID},p;
  assert(real.hello && real.halted && real.steps==0x1bff && !real.carried);
  /* DFU cold: no record, auto-boot 0, +0x4020 enable set: skip, no SPI0 access. */
  decide(none,0,DFU_OUTBOX,A9_SPI0_FRESH);
  /* The 15:52 reload: post-quiesce-halt, auto-boot 0: now runs. */
  a=decide(real,0,QUIESCED_OUTBOX,A9_SPI0_PREV_HALTED);
  cold_test=5;cold_defer=1;
  assert(!a9_aop_cold_start(&a) && writes==21 && spi_fault_read_at==19 && !a.spi0.reasserted && a.spi0.intc.acked);
  /* Unquiesced reload: auto-boot 1 wins whatever the record says. */
  decide(none,1,DFU_OUTBOX,A9_SPI0_LIVE_AUTO_BOOT);
  decide(bad,1,QUIESCED_OUTBOX,A9_SPI0_LIVE_AUTO_BOOT);
  decide(real,1,DFU_OUTBOX,A9_SPI0_LIVE_AUTO_BOOT);
  /* Missing or corrupt record with auto-boot 0: conservative skip, even with
   * a Linux stop footprint (enable clear). */
  decide(none,0,QUIESCED_OUTBOX,A9_SPI0_FRESH);
  decide(bad,0,QUIESCED_OUTBOX,A9_SPI0_PREV_BAD);
  decide(bad,0,DFU_OUTBOX,A9_SPI0_PREV_BAD);
  /* Valid record kept in DRAM across a DFU: wrapper back at reset: skip, and
   * the proven loader then runs with no SPI0 read at all. */
  a=decide(real,0,DFU_OUTBOX,A9_SPI0_STALE);
  cold_test=5;cold_defer=1;
  assert(!a9_aop_cold_start(&a) && writes==17 && !spi_reads);
  /* Previous kernel never had a HELLO and carried nothing: skip. */
  p=parsed("SHUTDOWN begin mode=quiesce entry=x writer=00 hello=0 phase=probe result=none rx_halt=0 crashed=0 fault=00000000\n"
           "QUIESCE step=skipped reason=no-owned-handshake phase=probe result=none\n");
  assert(!p.hello && !p.carried);
  decide(p,0,QUIESCED_OUTBOX,A9_SPI0_PREV_NO_RUN);
  /* ...unless it carried an earlier run in this chain (still bound). */
  p=parsed("SHUTDOWN begin mode=quiesce entry=x writer=00 hello=0 phase=probe result=none rx_halt=0 crashed=0 fault=00000000\n"
           "SPI0-CARRY retained=1 source=prev-unbound\n"
           "QUIESCE step=skipped reason=no-owned-handshake phase=probe result=none\n");
  assert(p.carried && !p.hello);
  decide(p,0,QUIESCED_OUTBOX,A9_SPI0_PREV_CARRIED);
  decide(p,0,DFU_OUTBOX,A9_SPI0_STALE);
  /* A truncated record still has line 1; halted=1 needs the steps bit too. */
  p=parsed("SHUTDOWN begin mode=quiesce entry=x writer=00 hello=1 phase=streaming result=streaming rx_halt=0 crashed=0 fault=00000000\n");
  assert(p.hello && !p.halted && !p.steps);
  decide(p,0,QUIESCED_OUTBOX,A9_SPI0_PREV_LIVE);
  p=parsed("SHUTDOWN begin mode=quiesce entry=x hello=1 phase=x\nQUIESCE step=complete status=-5 quiesced=1 halted=1 drained=0 steps=0x3ff failed_step=halted errno=5\n");
  assert(p.hello && !p.halted && p.steps==0x3ff);
  /* Only whole-line prefixes and exact tokens count. */
  p=parsed("xSHUTDOWN begin hello=1 \nSHUTDOWN begin mode=q hello=10 phase=x\n"
           " SPI0-CARRY retained=1 source=x\nSPI0-CARRY retained=10 source=x\n"
           "AOP-PREV QUIESCE step=complete quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none\n"
           "SHUTDOWN begin hello=1");  /* no trailing space or newline */
  assert(!p.hello && !p.halted && !p.carried && !p.steps);
  decide(p,0,QUIESCED_OUTBOX,A9_SPI0_PREV_NO_RUN);
  p=parsed("");assert(!p.hello && !p.carried);
  for(unsigned int r=A9_SPI0_LIVE_AUTO_BOOT;r<=A9_SPI0_STALE;r++)
   assert(a9_spi0_reason_runs(r)==(r<=A9_SPI0_PREV_CARRIED) && strcmp(a9_spi0_reason_name(r),"unknown"));
 }
 puts("PASS aop_spi0_reset decision: real 15:52 transcript (63 lines/3479 B) parses hello/halted/steps 0x1bff; DFU cold skips untouched; post-quiesce-halt reload runs (prev-quiesce-halted); unquiesced (auto-boot 1) runs; missing/corrupt skip; DRAM-kept record with +0x4020 at reset skips stale; no-HELLO skip unless carried; truncated runs prev-live; mid-line tokens ignored");
 puts("PASS aop_spi0_reset: fresh instance untouched; needs halt+drain; begin line flushed before the first access; exact 0102f2f4 stores 110/10/11 with two 100us delays inside the AOP PMGR guard; bit6 cleared; pre-RUN recheck refuses release; bounded polls restore the saved word; sticky bit/domains off refuse; ps_spi0 read-only");
 /* aop_wake_wrapper=1: iOS resume steps 3-7 only; no RUN, auto-boot or NMI store. */
 {static const u32 wo[]={0x4000,0xc00,0x4004,0xc04,0x4004,0xc04,0x4020};
  static const u32 wv[]={0x1111,0x1111,1,1,0x1000,0x1000,0x20001};
  a=fresh();outbox=0;
  assert(!a9_aop_wrapper_wake(&a) && a.wake_wrapper_done && !a.wake_wrapper_errno && writes==7);
  for(unsigned int i=0;i<writes;i++) {
   assert(offsets[i]==wo[i] && values[i]==wv[i]);
   assert(offsets[i]!=AOP_CPU_CONTROL && offsets[i]!=0x600 && offsets[i]!=0xc004 && offsets[i]!=0xc024);
  }
  assert(autoboot==1 && regs[AOP_CPU_CONTROL/4]==0x210 && !reads64 && !spi_reads);
  for(unsigned int fail=1;fail<=7;fail++) {
   a=fresh();outbox=0;fault_after=fail;
   assert(a9_aop_wrapper_wake(&a)==-EIO && writes==fail && !a.wake_wrapper_done && a.wake_wrapper_errno==-EIO);
  }
  a=fresh();a.phase=PHASE_FAILED;
  assert(a9_aop_wrapper_wake(&a)==-EIO && !writes && !a.wake_wrapper_done);}
 puts("PASS aop_wake_wrapper: exact 4000/c00=1111, 4004/c04=1 then 1000, 4020|=1 RMW; no +44/auto-boot/c004/c024; a fault after any store stops the sequence; refused once failed");
 /* Actual synchronous wait: immediate HELLO, retained-state wake, and silence. */
 a=fresh();hello_at=100*NSEC_PER_MSEC;
 assert(!a9_aop_cold_hello(&a) && !sends && !a.inline_rx && a.hello);
 a=fresh();hello_at=1200*NSEC_PER_MSEC;
 assert(!a9_aop_cold_hello(&a) && sends==1 && a.cold_wake_sent && !a.inline_rx);
 a=fresh();hello_during_sample=true;
 assert(!a9_aop_cold_hello(&a) && !sends && a.hello);
 a=fresh();
 assert(a9_aop_cold_hello(&a)==-ETIMEDOUT && sends==1 && !a.inline_rx);
 assert(fake_ns==AOP_COLD_HELLO_MS*NSEC_PER_MSEC && snapshots==6);
 a=fresh();hello_at=AOP_COLD_HELLO_MS*NSEC_PER_MSEC;
 assert(!a9_aop_cold_hello(&a) && a.hello); /* Deadline-edge drain. */
 a=fresh();capture_fault=3;
 assert(a9_aop_cold_hello(&a)==-EIO && !sends && !a.inline_rx);
 a=fresh();send_errno=-EBUSY;
 assert(a9_aop_cold_hello(&a)==-EBUSY && sends==1 && a.cold_wake_errno==-EBUSY);
 puts("PASS production cold HELLO: synchronous RX, no wake after HELLO, one wake after 1s, 20s bound, deadline-edge RX, capture/send failures terminal");
 puts("PASS production forced cold: mirrored b88 ignored; fixed 10ms delay; stuck RUN suppresses drain/load; control revalidation; exact stop/start order; bounded stale FIFO; fault after every store aborts; pinned firmware; reserved ext; no Linux IRQ/DMA allocation");
}
