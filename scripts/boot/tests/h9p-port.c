/* Mock MMIO for the production S8003 restart functions (no device access).
 * production.h is extracted from a9/boot by test-ans-offline.py. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#define WRITE_ONCE(x,v) ((x)=(v))
#define __iomem
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctz(mask))
#define dev_info(d,...) record_log(false, __VA_ARGS__)
#define dev_warn(...) do {} while (0)
#define dev_err(d,...) record_log(true, __VA_ARGS__)
#define dev_err_probe(dev,err,...) (record_log(true, __VA_ARGS__), (err))
#define ilog2(x) __builtin_ctz(x)
#include "registers.h"
static char last_step[256], first_error[256];
static void *fail_addr;
static unsigned polls;
static void record_log(bool error, const char *fmt, ...) {
 va_list ap;va_start(ap,fmt);
 vsnprintf(last_step,sizeof(last_step),fmt,ap);va_end(ap);
 if(error && !*first_error)snprintf(first_error,sizeof(first_error),"%s",last_step);
}
struct device { int unused; };
static bool of_machine_is_compatible(const char *s) {return true;}
struct apple_h9p_pcie {
 bool ans_deep_recovery, ans_gen1, ans_pre_enumeration; u16 link_cap[4]; unsigned long down_ports;
 struct {void *base;} nvmmu;
 void *base_rc, *base_port[4], *base_config, *base_phy, *port_nodes[4];
 struct device *dev;
 void *perst[4];
 u32 lane_cfg, max_speed[4];
 bool nvme_mode[4];
};
static unsigned char nvmmu[0x100];
static unsigned char rc[H9P_RC_SIZE], pb[2][H9P_PORT_SIZE];
static unsigned char cfg[2 * H9P_CFG_PORT_STRIDE], phy[H9P_PHY_SIZE];
static struct apple_h9p_pcie pcie;
static bool perst[2], powered[2];
static unsigned cycles[2], attempts[2], phy_inits[2], tunables[2];
static unsigned mode_off, mode_on, scenario;
static unsigned long now_us, assert_us[2], clock_us[2];
static u64 ktime_get_ns(void) {return now_us*1000;}
static int gpiod_get_value_cansleep(void *p) {return perst[(uintptr_t)p-1];}
static int apple_s8003_init_common(struct apple_h9p_pcie *p) {return 0;}
static int port_addr(const void *p) {
 for (int i=0;i<2;i++)
  if ((uintptr_t)p >= (uintptr_t)pb[i] && (uintptr_t)p < (uintptr_t)pb[i]+H9P_PORT_SIZE) return i;
 return -1;
}
static u32 raw32(const void *p) {u32 v;memcpy(&v,p,4);return v;}
static void raw_write(u32 v,void *p) {memcpy(p,&v,4);}
static u32 readl(const void *p) {
 int n=port_addr(p);
 if (n>=0) {
  assert(powered[n]);
  if (p==pb[n]+S8003_PORT_LINKSTS) {
   if (scenario==1 || (scenario==2 && attempts[n]==1)) return 0;
   return attempts[n] ? S8003_PORT_LINKSTS_UP : 0;
  }
  if (p==pb[n]+S8003_PORT_LINKCDMSTS)
   return (scenario==3 ? 3U : S8003_LTSSM_L0) << 9;
 }
 return raw32(p);
}
static void writel(u32 v,void *p) {
 int n=port_addr(p);
 if (n>=0) {
  assert(powered[n]);
  if (p==pb[n]+H9P_PORT_LTSSMCTL && (v&H9P_PORT_LTSSM_ENABLE) &&
      !(raw32(p)&H9P_PORT_LTSSM_ENABLE)) {
   assert(!perst[n] && cycles[n] && phy_inits[n]);
   assert(raw32(pb[n]+S8003_PORT_PMETO)==0);
   attempts[n]++;
  }
 }
 for (int i=0;i<2;i++) {
  if (p==rc+S8003_RC_PORT_PWR(i) && !(v&S8003_RC_PORT_PWR_ON)) {
   assert(perst[i] && !(raw32(pb[i]+H9P_PORT_LTSSMCTL)&H9P_PORT_LTSSM_ENABLE));
   cycles[i]++;powered[i]=false;
  }
  if (p==rc+S8003_RC_PORT_CTL(i) && (v&S8003_RC_PORT_CTL_ENABLE) &&
      (raw32(rc+S8003_RC_PORT_PWR(i))&S8003_RC_PORT_PWR_ON)) powered[i]=true;
  if (p==rc+S8003_RC_PORT_REFCLK(i) && (v&S8003_RC_PORT_REFCLK_ON)) clock_us[i]=now_us;
 }
 if (p==rc+S8003_RC_NVME_MODE1) {if(v)mode_on++;else mode_off++;}
 raw_write(v,p);
}
static u16 readw(const void *p) {u16 v;memcpy(&v,p,2);return v;}
static void writew(u16 v,void *p) {memcpy(p,&v,2);}
static void h9p_rmw(void *p,u32 clear,u32 set) {writel((readl(p)&~clear)|set,p);}
static void h9p_rmww(void *p,u16 clear,u16 set) {writew((readw(p)&~clear)|set,p);}
static void h9p_writel_flush(u32 v,void *p) {
 if(p==nvmmu+H9P_NVMMU_SART_CTRL) assert(perst[0] && powered[0] && v==0);
 writel(v,p);readl(p);
}
static void udelay(unsigned n) {now_us+=n;}
static void usleep_range(unsigned a,unsigned b) {now_us+=a;}
static void msleep(unsigned n) {now_us+=1000UL*n;}
static int gpiod_direction_output(void *p,int value) {
 int n=(uintptr_t)p-1;
 if (value) {perst[n]=true;assert_us[n]=now_us;return 0;}
 assert(powered[n]);
 assert(now_us-assert_us[n]>=1000UL*S8003_PORT_RESET_MS);
 assert(now_us-clock_us[n]>=S8003_T_REFCLK_TO_PERST_US);
 assert(raw32(rc+S8003_RC_PORT_PWR(n))&S8003_RC_PORT_REFCLK_NOGATE);
 assert(!(raw32(pb[n]+H9P_PORT_LTSSMCTL)&H9P_PORT_LTSSM_ENABLE));
 assert(!raw32(pb[n]+S8003_PORT_PMETO));
 assert(!(readw(cfg+n*H9P_CFG_PORT_STRIDE+0x70+PCI_EXP_LNKCTL)&
          (PCI_EXP_LNKCTL_ASPMC|PCI_EXP_LNKCTL_LD|PCI_EXP_LNKCTL_RL)));
 assert(!(readw(cfg+n*H9P_CFG_PORT_STRIDE+0x70+PCI_EXP_LNKCTL2)&PCI_EXP_LNKCTL2_ENTER_COMP));
 perst[n]=false;return 0;
}
static int apple_h9p_wait(void *p,u32 mask,u32 min,u32 max,unsigned long t) {
 if (scenario==4 || fail_addr==p) return -ETIMEDOUT;
 return ((readl(p)&mask)>=min && (readl(p)&mask)<=max) ? 0 : -ETIMEDOUT;
}
#define readl_poll_timeout(p,v,condition,interval,timeout) ({ \
 assert(timeout>0 && timeout<=500000 && interval>0); polls++; \
 int result=-ETIMEDOUT; for(int k=0;k<3;k++) { v=readl(p); \
 if(fail_addr!=p && (condition)) {result=0;break;} now_us+=interval; } result; })
static u32 apple_s8003_lane_mask(struct apple_h9p_pcie *p,unsigned n) {return n?4:3;}
static void apple_s8003_port_dump(struct apple_h9p_pcie *p,unsigned n,const char *when) {assert(powered[n]);}
static u64 apple_h9p_read_pci_cap(struct apple_h9p_pcie *p,unsigned devfn,u32 type) {return scenario==5?0:0x70;}
static int apple_h9p_apply_tunables(struct apple_h9p_pcie *p,void *node,const char *prop,void *base,unsigned size,const char *what) {
 for(int n=0;n<2;n++) if(base==pb[n])tunables[n]++;
 return 0;
}
static void apple_s8003_phy_ip_init(struct apple_h9p_pcie *p,unsigned n,u32 lanes,u32 speed) {
 assert(powered[n] && !perst[n]);phy_inits[n]++;
}
#include "production.h"
static void init_state(bool retained, bool initially_powered) {
 pcie.nvmmu.base=nvmmu;
 memset(rc,0,sizeof(rc));memset(pb,0,sizeof(pb));memset(cfg,0,sizeof(cfg));memset(phy,0,sizeof(phy));
 memset(&pcie,0,sizeof(pcie));memset(cycles,0,sizeof(cycles));memset(attempts,0,sizeof(attempts));
 memset(phy_inits,0,sizeof(phy_inits));memset(tunables,0,sizeof(tunables));
 now_us=mode_off=mode_on=0;fail_addr=NULL;polls=0;last_step[0]=first_error[0]=0;
 pcie.base_rc=rc;pcie.base_config=cfg;pcie.base_phy=phy;pcie.lane_cfg=1;
 raw_write(S8003_RC_STAT_COMMON_READY|S8003_RC_STAT_PORT_READY,rc+S8003_RC_STAT);
 raw_write(7,phy+H9P_PHY_PORTMASK);
 for (int n=0;n<2;n++) {
  pcie.base_port[n]=pb[n];pcie.perst[n]=(void*)(uintptr_t)(n+1);
  pcie.max_speed[n]=n?1:2;pcie.nvme_mode[n]=!n;
  perst[n]=false;powered[n]=initially_powered;
  raw_write(initially_powered?1:0,rc+S8003_RC_PORT_CTL(n));
  raw_write(initially_powered?1:0,rc+S8003_RC_PORT_PWR(n));
  raw_write(1,rc+S8003_RC_PORT_READY(n));
  raw_write(retained?1:0,pb[n]+H9P_PORT_LTSSMCTL);
  raw_write(retained?S8003_PORT_PMETO_SEND:0,pb[n]+S8003_PORT_PMETO);
  writew(PCI_EXP_LNKCTL_ASPMC|PCI_EXP_LNKCTL_LD|PCI_EXP_LNKCTL_RL,cfg+n*H9P_CFG_PORT_STRIDE+0x70+PCI_EXP_LNKCTL);
  writew(PCI_EXP_LNKCTL2_ENTER_COMP|2,cfg+n*H9P_CFG_PORT_STRIDE+0x70+PCI_EXP_LNKCTL2);
  raw_write(6,cfg+n*H9P_CFG_PORT_STRIDE+0x70+PCI_EXP_LNKCAP2);
 }
}
int main(void) {
 for(int retained=0;retained<2;retained++)for(int on=0;on<2;on++) {
  scenario=0;init_state(retained,on);
  assert(!apple_s8003_setup_port(&pcie,0));
  u32 p0=raw32(rc+S8003_RC_PORT_PWR(0));
  assert(!apple_s8003_setup_port(&pcie,1));
  assert(raw32(rc+S8003_RC_PORT_PWR(0))==p0); /* Wi-Fi must not reset NVMe. */
  assert(attempts[0]==1 && attempts[1]==1 && cycles[0]==1 && cycles[1]==1);
  assert(mode_off==1 && mode_on==2 && tunables[0] && tunables[1]);
 }
 scenario=2;init_state(true,false);assert(!apple_s8003_setup_port(&pcie,0));assert(attempts[0]==2&&cycles[0]==2);
 scenario=1;init_state(true,true);assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);assert(attempts[0]==2&&perst[0]);
 scenario=3;init_state(true,true);assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);assert(attempts[0]==2&&perst[0]);
 scenario=4;init_state(true,false);assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);assert(!attempts[0]&&perst[0]);
 scenario=5;init_state(false,true);assert(apple_s8003_setup_port(&pcie,0)==-ENODEV);assert(cycles[0]==1&&perst[0]);
 puts("PASS cold/retained powered/off states; two ports; reset timing; retry success/exhaustion; L0 and power timeout; no capability");
}
