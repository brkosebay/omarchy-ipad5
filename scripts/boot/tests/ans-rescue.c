#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define H9P_NVMMU_SART_CTRL 0
#define H9P_PORT_LTSSMCTL 0
#define H9P_PORT_LTSSM_ENABLE 1
#define BIT(n) (1U<<(n))
#define PCI_DEVFN(s,f) ((s)<<3|(f))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define APPLE_ANS_ROUND7 0
struct device {void *of_node;};
struct device_attribute {};
struct pci_dev {};
struct bridge {void *bus;};
struct apple_h9p_pcie {
 struct {void *tcb,*base;bool ready,active;} nvmmu;
 struct bridge *bridge;
 bool ans_rescue_allowed;
 unsigned enabled_ports, down_ports, ans_rescue_attempts;
 unsigned late_ports;
 int late_work;
 int ans_mode, ans_rescue_result;
 void *perst[4],*base_port[4];
};
static struct bridge bridge;
static struct apple_h9p_pcie host;
static struct pci_dev rp;
static int lock, recoveries, setups, scans, off, pins, deep_result, nv_result, scheduled;
static bool crash, present;
static struct apple_h9p_pcie *apple_h9p_pcie_lookup(struct device *d) {return &host;}
static bool sysfs_streq(const char *a,const char *b) {return !strcmp(a,b);}
static bool is_kdump_kernel(void) {return crash;}
static bool of_machine_is_compatible(const char *s) {return true;}
static bool of_device_is_compatible(void *p,const char *s) {return true;}
static void pci_lock_rescan_remove(void) {assert(!lock);lock=1;}
static void pci_unlock_rescan_remove(void) {assert(lock);lock=0;}
static struct pci_dev *pci_get_slot(void *b,int fn) {assert(lock);return present?&rp:NULL;}
static void pci_dev_put(struct pci_dev *p) {}
static int apple_s8003_deep_recover(struct apple_h9p_pcie *p) {assert(lock);recoveries++;return deep_result;}
static int apple_h9p_setup_nvmmu(struct apple_h9p_pcie *p) {assert(lock && !p->ans_rescue_allowed);setups++;p->nvmmu.tcb=(void*)1;return nv_result;}
static void msleep(int n) {assert(n==100);}
static void pci_rescan_bus(void *b) {assert(lock && !host.ans_rescue_allowed && !(host.down_ports&1) && (host.enabled_ports&1));scans++;}
static int gpiod_direction_output(void *p,int v) {assert(v==1);pins++;return 0;}
static void apple_s8003_port_off(struct apple_h9p_pcie *p,int n) {assert(n==0);off++;}
static void h9p_writel_flush(int v,void *p) {assert(!v);}
static void apple_s8003_port_stop(struct apple_h9p_pcie *p,int n) {assert(!n);}
static int apple_s8003_port_wait(struct apple_h9p_pcie *p,int n,void *a,unsigned m,unsigned v,unsigned t,const char *s) {return 0;}
static void apple_s8003_rescue_bus(struct apple_h9p_pcie *p,unsigned port) {assert(port==0 && p->ans_rescue_allowed);}
static void schedule_delayed_work(int *w, int delay) {assert(!lock && !delay && (host.enabled_ports&1));scheduled++;}
#include "production.h"
static void init(void) {
 host=(struct apple_h9p_pcie){.ans_mode=1,.ans_rescue_allowed=true,.bridge=&bridge,.down_ports=1};
 recoveries=setups=scans=off=pins=deep_result=nv_result=lock=scheduled=0;present=crash=false;
}
int main(void) {
 struct device dev={0};
 init();deep_result=-ETIMEDOUT;
 assert(ans_recover_store(&dev,NULL,"1",1)==-ETIMEDOUT && host.ans_rescue_allowed && recoveries==1 && !scans);
 deep_result=0;
 assert(ans_recover_store(&dev,NULL,"1",1)==1 && !host.ans_rescue_allowed && setups==1 && scans==1);
 assert(ans_recover_store(&dev,NULL,"1",1)==-EBUSY && recoveries==2);
 for(int guard=0;guard<4;guard++) {
  init();
  if(guard==0)host.ans_rescue_allowed=false;
  if(guard==1)host.enabled_ports=1;
  if(guard==2)host.nvmmu.tcb=(void*)1;
  if(guard==3)present=true;
  assert(ans_recover_store(&dev,NULL,"1",1)==-EBUSY && !recoveries && !scans && !lock);
 }
 init();nv_result=-ENOMEM;
 assert(ans_recover_store(&dev,NULL,"1",1)==-ENOMEM && !host.ans_rescue_allowed && off==1 && pins==1 && !scans);
 assert(ans_recover_store(&dev,NULL,"1",1)==-EBUSY && recoveries==1);
 init();crash=true;
 assert(ans_recover_store(&dev,NULL,"1",1)==-EPERM && !recoveries);
 crash=false;host.ans_mode=0;
 assert(ans_recover_store(&dev,NULL,"1",1)==-EPERM && !recoveries);
 puts("PASS rescue retry/rescan; live, previously published, partial NVMMU, existing PCI function and crash guards");
}
