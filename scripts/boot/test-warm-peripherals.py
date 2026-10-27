#!/usr/bin/env python3
"""Round 6 production C tests. Run in linux-a9-kbuild; no device access."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path('/src/wt/boot'))
args = parser.parse_args()


def source(path):
    return (args.source/path).read_text()


def function(text, name):
    start = re.search(r'static (?:int|void|irqreturn_t) (?:__init )?'+name+r'\(', text).start()
    return text[start:text.index('\n}', start)+2]


def defines(text, prefix):
    return '\n'.join(line for line in text.splitlines() if line.startswith('#define '+prefix))


def run_c(pre, prod, test):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)
        (p/'test.c').write_text(pre+'\n'+prod+'\n'+test)
        subprocess.run(['clang', '-std=gnu11', '-Werror=implicit-function-declaration',
                        '-fsanitize=address,undefined', '-O1', '-g', str(p/'test.c'),
                        '-o', str(p/'test')], check=True)
        subprocess.run([str(p/'test')], check=True, timeout=10)


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_GET(m,v) (((v)&(m)) >> __builtin_ctz(m))
#define FIELD_PREP(m,v) (((v)<<__builtin_ctz(m)) & (m))
#define min(a,b) ((a)<(b)?(a):(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define __init
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,n) ((v)=(n))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
static jmp_buf panic_jmp;
static unsigned int panics;
#define panic(...) do {panics++;longjmp(panic_jmp,1);} while(0)
'''


class WarmPeripheralTests(unittest.TestCase):
    def test_spi_transfer_deadline_stale_fifo_and_trace_budget(self):
        text = source('drivers/spi/spi-apple-s8000.c')
        pre = COMMON+r'''
#define SZ_64K 65536
#define SPI_CPHA BIT(1)
#define SPI_CPOL BIT(2)
#define NSEC_PER_SEC 1000000000ULL
#define U32_MAX UINT32_MAX
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define DIV_ROUND_UP_ULL(a,b) DIV_ROUND_UP(a,b)
typedef int64_t ktime_t;
struct device {int unused;};
struct s8000_spi {u8 *base;void *clk;unsigned int traced_xfers;};
struct spi_controller {struct s8000_spi *data;};
struct spi_device {struct device dev;u32 max_speed_hz,mode;};
struct spi_transfer {const u8 *tx_buf;u8 *rx_buf;unsigned int len,bits_per_word,speed_hz,effective_speed_hz;int word_delay;};
static u32 regs[64];static int scenario,sent,received,reads,writes,ticks,logs;
#undef dev_info
#define dev_info(...) (logs++)
static unsigned long clk_get_rate(void*p){return 61538461;}
static int spi_delay_to_ns(void*p,struct spi_transfer*t){return 0;}
static struct s8000_spi *spi_controller_get_devdata(struct spi_controller*c){return c->data;}
static ktime_t ktime_get(void){return ++ticks*1000;}
static ktime_t ktime_add_us(ktime_t t,unsigned int us){return t+us;}
static int ktime_compare(ktime_t a,ktime_t b){return (a>b)-(a<b);}
static void cond_resched(void){}
static void cpu_relax(void){}
static void writel(u32 v,void*p){writes++;unsigned int off=(u8*)p-(u8*)regs;regs[off/4]=v;if(off==0x10)sent++;}
static u32 readl(void*p){
 reads++;unsigned int off=(u8*)p-(u8*)regs;
 if(off==8){
  if(scenario==1)return 17<<11; // impossible FIFO level
  if(scenario==2)return 0; // no progress
  if(scenario==3)return 1<<11; // retained RX before any new TX
  return (sent-received)<<11;
 }
 if(off==0x20)return (received++)&0xff;
 return regs[off/4];
}
'''
        prod = defines(text, 'S8000_')+'\n'+function(text, 's8000_spi_transfer_one')
        run_c(pre, prod, r'''
int main(void){
 struct s8000_spi spi={.base=(u8*)regs};struct spi_controller ctlr={.data=&spi};
 struct spi_device dev={.max_speed_hz=12048192};u8 rx[64];
 struct spi_transfer t={.rx_buf=rx,.len=64,.bits_per_word=8};
 for(scenario=0;scenario<4;scenario++){
  memset(regs,0,sizeof(regs));sent=received=reads=writes=ticks=0;
  int ret=s8000_spi_transfer_one(&ctlr,&dev,&t);
  assert(ret==(scenario==0?0:scenario==2?-ETIMEDOUT:-EIO));
  assert(!regs[S8000_CTRL/4]&&regs[S8000_CFG/4]==S8000_CFG_BASE);
  assert(t.effective_speed_hz==61538461/6&&reads<1200);
  if(!scenario){assert(sent==64&&received==64);for(int i=0;i<64;i++)assert(rx[i]==i);}
  if(scenario==2)assert(sent==16&&!received);
 }
 scenario=0;logs=0;
 for(int i=0;i<20;i++){
  sent=received=ticks=0;assert(!s8000_spi_transfer_one(&ctlr,&dev,&t));
 }
 assert(spi.traced_xfers==8&&logs==8); // four begin/end pairs left
 t.bits_per_word=16;writes=reads=0;assert(s8000_spi_transfer_one(&ctlr,&dev,&t)==-EINVAL&&!writes&&!reads);
 t.bits_per_word=8;t.len=65537;assert(s8000_spi_transfer_one(&ctlr,&dev,&t)==-EINVAL&&!writes&&!reads);
 puts("PASS PIO success, impossible/stale RX rejection, no-progress deadline, FIFO backpressure, stop on error, finite trace budget");
}
''')

    def test_spi_idle_and_shutdown(self):
        text = source('drivers/spi/spi-apple-s8000.c')
        prod = defines(text, 'S8000_')+'\n'+ '\n'.join(function(text, n) for n in
                  ('s8000_spi_idle', 's8000_spi_shutdown'))
        pre = COMMON+r'''
struct device {int unused;};
struct s8000_spi {u8 *base;};
struct spi_controller {struct s8000_spi *data;unsigned int num_chipselect;void **cs_gpiods;};
struct platform_device {struct device dev;struct spi_controller *data;};
static u32 regs[64];static int events,drain,pm,writes,reads,fail_drain,fail_pm,stuck;
static bool kexec_in_progress=true;
static struct spi_controller *platform_get_drvdata(struct platform_device*p){return p->data;}
static struct s8000_spi *spi_controller_get_devdata(struct spi_controller*c){return c->data;}
static int spi_controller_suspend(struct spi_controller*c){drain=++events;return fail_drain?-EBUSY:0;}
static int pm_runtime_resume_and_get(struct device*d){assert(drain);pm=++events;return fail_pm?-EIO:0;}
static void gpiod_set_value_cansleep(void*p,int v){assert(pm&&v==0&&p==(void*)1);}
static void writel(u32 v,void*p){assert(pm);writes++;regs[((u8*)p-(u8*)regs)/4]=v;}
static u32 readl(void*p){reads++;return stuck?1:*(u32*)p;}
'''
        run_c(pre, prod, r'''
int main(void){
 struct s8000_spi spi={.base=(u8*)regs};struct spi_controller ctlr={.data=&spi};
 void *cs=(void*)1;ctlr.cs_gpiods=&cs;ctlr.num_chipselect=1;
 struct platform_device p={.data=&ctlr};
 for(int scenario=0;scenario<4;scenario++){
  memset(regs,0xa5,sizeof(regs));events=drain=pm=writes=reads=panics=0;
  fail_drain=scenario==1;fail_pm=scenario==2;stuck=scenario==3;
  if(!setjmp(panic_jmp))s8000_spi_shutdown(&p);
  assert(drain==1);
  if(!scenario){assert(!panics&&pm==2&&writes==6&&reads==4);
   assert(!regs[S8000_CTRL/4]&&regs[S8000_CFG/4]==S8000_CFG_BASE);
   assert(!regs[S8000_RXCNT/4]&&!regs[S8000_TXCNT/4]);
   assert(regs[S8000_PIN/4]==S8000_PIN_CS&&regs[S8000_STATUS/4]==S8000_STATUS_CLEAR);
  }else{assert(panics==1);if(scenario<3)assert(!writes&&!reads);}
 }
 kexec_in_progress=false;fail_drain=1;panics=writes=reads=0;
 s8000_spi_shutdown(&p);assert(!panics&&!writes&&!reads);
 puts("PASS SPI drain before power/MMIO, inherited-state normalization, posted flush, refuse failed kexec handoff");
}
''')

    def test_warm_gpio_baseline_and_orphan_irq(self):
        text = source('drivers/pinctrl/pinctrl-apple-gpio.c')
        prod = defines(text, 'REG_')+'\n'+ '\n'.join(function(text, n) for n in
                                    ('apple_gpio_warm_quiesce', 'apple_gpio_irq_handler'))
        pre = COMMON+r'''
struct irq_chip {int unused;};
struct gpio_chip {unsigned int ngpio;struct {unsigned int num_parents;void *domain;} irq;};
struct apple_gpio_pinctrl {void *dev;u8 *base;bool warm;struct gpio_chip gpio_chip;u8 irqgrps[2];};
struct irq_desc {u8 *data;};
static u32 regs[1024];static unsigned int sets,acks,delivered,unmapped=65,warns;
static u32 apple_gpio_get_reg(struct apple_gpio_pinctrl*p,unsigned int n){return regs[n];}
static void apple_gpio_set_reg(struct apple_gpio_pinctrl*p,unsigned int n,u32 mask,u32 v){
 assert(n<p->gpio_chip.ngpio);sets++;regs[n]=(regs[n]&~mask)|v;
}
static void writel(u32 v,void*p){acks++;*(u32*)p&=~v;}
static u32 readl_relaxed(void*p){return *(u32*)p;}
static struct irq_chip *irq_desc_get_chip(struct irq_desc*d){return NULL;}
static void *irq_desc_get_handler_data(struct irq_desc*d){return d->data;}
static int generic_handle_domain_irq(void*d,unsigned int pin){if(pin==unmapped)return -EINVAL;delivered++;return 0;}
#define for_each_set_bit(b,p,n) for((b)=0;(b)<(n);(b)++)if(*(p)&(1UL<<(b)))
#define chained_irq_enter(...) ((void)0)
#define chained_irq_exit(...) ((void)0)
#define dev_warn_ratelimited(...) (warns++)
'''
        run_c(pre, prod, r'''
int main(void){
 struct apple_gpio_pinctrl p={.base=(u8*)regs,.warm=true,.gpio_chip={.ngpio=70,.irq={.num_parents=2}}};
 p.irqgrps[0]=0;p.irqgrps[1]=1;
 for(unsigned int mode=0;mode<8;mode++)regs[mode]=(mode<<1)|BIT(0)|BIT(9)|BIT(16)|BIT(21);
 regs[16]=FIELD_PREP(REG_GPIOx_PERIPH,2)|FIELD_PREP(REG_GPIOx_MODE,5);
 u32 before[70];memcpy(before,regs,sizeof(before));
 for(int g=0;g<2;g++)for(int pin=0;pin<70;pin+=32)regs[REG_IRQ(g,pin)/4]=~0U;
 apple_gpio_warm_quiesce(&p);assert(sets==5&&acks==6);
 for(int pin=0;pin<70;pin++){
  assert((regs[pin]&~REG_GPIOx_MODE)==(before[pin]&~REG_GPIOx_MODE));
  if(pin>=2&&pin<=6)assert(FIELD_GET(REG_GPIOx_MODE,regs[pin])==REG_GPIOx_IN_IRQ_OFF);
  else assert(regs[pin]==before[pin]);
 }
 assert(regs[REG_IRQ(1,64)/4]==(~0U<<6));
 sets=acks=0;p.warm=false;apple_gpio_warm_quiesce(&p);assert(!sets&&!acks);
 p.warm=true;regs[65]=FIELD_PREP(REG_GPIOx_MODE,5);
 regs[REG_IRQ(0,64)/4]=BIT(1)|BIT(2)|BIT(15); // orphan, mapped, invalid tail
 struct irq_desc d={.data=&p.irqgrps[0]};
 // Clear other banks: mapped IRQ handling normally acknowledges its own latch.
 regs[REG_IRQ(0,0)/4]=regs[REG_IRQ(0,32)/4]=0;
 apple_gpio_irq_handler(&d);assert(sets==1&&acks==2&&warns==2&&delivered==1);
 assert(regs[REG_IRQ(0,64)/4]==BIT(2));
 assert(FIELD_GET(REG_GPIOx_MODE,regs[65])==REG_GPIOx_IN_IRQ_OFF);
 for(int mode=0;mode<2;mode++){
  regs[65]=mode?FIELD_PREP(REG_GPIOx_PERIPH,2)|FIELD_PREP(REG_GPIOx_MODE,5):
               FIELD_PREP(REG_GPIOx_MODE,REG_GPIOx_OUT)|BIT(0);
  u32 saved=regs[65];sets=acks=0;regs[REG_IRQ(0,64)/4]=BIT(1);
  apple_gpio_irq_handler(&d);assert(!sets&&acks==1&&regs[65]==saved);
 }
 puts("PASS warm GPIO masks only inherited input IRQ modes, preserves mux/output/data, clears valid bank bits, contains orphan sources");
}
''')

    def test_boot_deadline_policy_and_expiry(self):
        text = source('drivers/soc/apple/apple-boot-watchdog.c')
        pre = COMMON+r'''
#define HZ 100
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
struct timer_list {void(*fn)(struct timer_list*);};
struct kernel_param {int unused;};
static unsigned int timeout;static bool initialized,supported;static struct timer_list boot_timer;
static bool board=true,capture;static unsigned long jiffies=7,expires;static int arms,deletes;
static int kstrtouint(const char*s,int b,unsigned int*v){char*end;unsigned long n=strtoul(s,&end,b);if(!*s||*end||*s=='-'||n>0xffffffffUL)return -EINVAL;*v=n;return 0;}
static void timer_setup(struct timer_list*t,void(*f)(struct timer_list*),int flags){t->fn=f;}
static void timer_delete_sync(struct timer_list*t){deletes++;expires=0;}
static void mod_timer(struct timer_list*t,unsigned long e){arms++;expires=e;}
static bool of_machine_is_compatible(const char*s){assert(!strcmp(s,"apple,j71t"));return board;}
static bool is_kdump_kernel(void){return capture;}
'''
        prod = '\n'.join(function(text, n) for n in ('apple_boot_expired', 'apple_boot_timeout_set',
                                                    'apple_boot_watchdog_init'))
        run_c(pre, prod, r'''
int main(void){
 for(int bits=0;bits<8;bits++){
  board=bits&1;capture=bits&2;timeout=(bits&4)?120:0;initialized=false;arms=deletes=expires=0;
  assert(!apple_boot_watchdog_init()&&initialized);
  assert(arms==((bits==5)?1:0));if(arms)assert(expires==120*HZ+jiffies);
  assert(apple_boot_timeout_set("120",NULL)==(board&&!capture?0:-EOPNOTSUPP));
  if(board&&!capture)assert(expires==120*HZ+jiffies);
  assert(!apple_boot_timeout_set("0",NULL)&&!timeout&&!expires);
  assert(deletes==(board&&!capture?2:1));
 }
 initialized=false;
 const char*bad[]={"1","29","601","-1","no","4294967296"};
 for(int i=0;i<6;i++)assert(apple_boot_timeout_set(bad[i],NULL)<0);
 assert(!apple_boot_timeout_set("30",NULL)&&timeout==30);
 assert(!apple_boot_timeout_set("600",NULL)&&timeout==600);
 board=true;capture=false;assert(!apple_boot_watchdog_init()&&expires==600*HZ+jiffies);
 if(!setjmp(panic_jmp))boot_timer.fn(&boot_timer);assert(panics==1);
 assert(!apple_boot_timeout_set("0",NULL)&&!expires);
 puts("PASS opt-in software deadline, j71t/capture guards, 30..600 limits, synchronous disarm/rearm, expiry panics");
}
''')

    def test_n1_shutdown_still_synchronizes_before_reset(self):
        text = source('drivers/input/touchscreen/apple_n1.c')
        # Current driver has a mutex, idempotent IRQ mask, and recovery work.
        # The strengthened fixture keeps every original free/contact/reset
        # assertion and also requires cancellation outside the lock.
        subprocess.run(['python3', '-B', '-I',
                        str(HERE.parent/'release/test-current-lifecycle.py'),
                        '--source', str(args.source)], check=True)
        self.assertIn('.shutdown = n1_remove,', text)

    def test_initramfs_disarms_only_at_verified_root_transition(self):
        init = Path('/work/scripts/omarchy/initramfs/init').read_text()
        boot_root = init[init.index('boot_root() {'):init.index('\n}', init.index('boot_root() {'))]
        self.assertLess(boot_root.index('return 1'), boot_root.index('stop_shells'))
        build = (HERE/'build-initramfs.sh').read_text()
        self.assertIn('linux_a9_early_storage_stop\\n', build)
        text = (HERE/'device/early-storage.sh').read_text()
        stop = text[text.index('linux_a9_early_storage_stop() {'):]
        self.assertEqual(text.count('echo 0 >"$deadline"'), 1)
        self.assertIn('/sys/module/apple_boot_watchdog/parameters/timeout', stop)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'timeout';p.write_text('120\n')
            stop = stop.replace('/sys/module/apple_boot_watchdog/parameters/timeout', str(p))
            subprocess.run(['bash', '-c', stop+'\nlinux_a9_early_storage_stop'], check=True)
            self.assertEqual(p.read_text(), '0\n')
        cfg = Path('/work/patches/boot/boot.config').read_text()
        for symbol in ('APPLE_J71T_BOOT_WATCHDOG', 'SOFTLOCKUP_DETECTOR',
                       'HARDLOCKUP_DETECTOR_BUDDY', 'DETECT_HUNG_TASK'):
            self.assertIn('CONFIG_'+symbol+'=y', cfg)


if __name__ == '__main__':
    unittest.main(argv=['test-warm-peripherals'], verbosity=2)
