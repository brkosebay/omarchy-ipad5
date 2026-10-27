/* Compile the production capture callback/store; never access a device. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <setjmp.h>
typedef uint32_t u32;
#define __iomem
#define PCI_VENDOR_ID_APPLE 0x106b
#define PCI_CLASS_STORAGE_EXPRESS 0x0108
#define PCI_CLASS_STORAGE_OTHER 0x0180
#define SZ_8K 8192
#define U32_MAX UINT32_MAX
#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1c
#define NVME_CSTS_RDY 1
#define BIT(x) (1UL<<(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_warn(...) do {} while(0)
#define dev_info(...) do {} while(0)
struct device {int unused;};
struct device_attribute {int unused;};
struct pci_dev {struct device dev;unsigned vendor,class;void *driver;};
struct bridge {void *bus;};
struct apple_h9p_pcie {struct device *dev;struct bridge *bridge;int late_ports;struct {int work;} late_work;};
struct apple_h9p_crash_state {struct apple_h9p_pcie *pcie;int result;bool busy,found;};
static struct device dev;
static struct bridge bridge;
static struct apple_h9p_pcie pcie={.dev=&dev,.bridge=&bridge};
static struct pci_dev endpoint={.vendor=PCI_VENDOR_ID_APPLE,.class=0x018002};
static bool capture=true,board=true,panic_resume_once;
static int armed=1, panics;
static jmp_buf panic_jump;
static int kexec_crash_loaded(void) {return armed;}
static void panic(const char *s) {panics++;longjmp(panic_jump,1);}
static unsigned char bar[8192];
static int scenario,links,reads,writes,master,disabled,parked,delays;
static struct apple_h9p_pcie *apple_h9p_pcie_lookup(struct device *d) {return &pcie;}
static bool of_machine_is_compatible(const char *s) {return board;}
static bool is_kdump_kernel(void) {return capture;}
static bool sysfs_streq(const char *s,const char *t) {return !strcmp(s,t);}
static int work_busy(void *w) {return 0;}
static int apple_ans_link_up(struct device *d) {links++;return scenario==1 || (scenario==3 && links>2)?0:1;}
static int pci_enable_device_mem(struct pci_dev *p) {return scenario==5?-EIO:0;}
static void *pci_iomap_range(struct pci_dev *p,int n,int off,int len) {return scenario==4?NULL:bar;}
static void pci_iounmap(struct pci_dev *p,void *b) {}
static void pci_clear_master(struct pci_dev *p) {master++;}
static void pci_disable_device(struct pci_dev *p) {disabled++;}
static u32 readl(void *p) {reads++;return (scenario==2 || scenario==3)?1:scenario==6?U32_MAX:0;}
static void writel(u32 val,void *p) {assert(p==bar+NVME_REG_CC && val==0);writes++;}
static void usleep_range(int a,int b) {delays++;assert(a==1000);}
static void apple_h9p_pcie_park_round7(struct apple_h9p_pcie *p,unsigned long ports) {assert(ports==BIT(0));parked++;}
static void pci_walk_bus(void *b,int (*cb)(struct pci_dev *,void *),void *data) {cb(&endpoint,data);}
#include "production.h"
int main(void) {
 for(scenario=0;scenario<=6;scenario++) {
  struct apple_h9p_crash_state state={.pcie=&pcie};
  links=reads=writes=master=disabled=parked=delays=0;
  assert(!apple_h9p_crash_stop_ans(&endpoint,&state));assert(state.found && !state.busy);
  if(scenario==0)assert(state.result==0 && writes==1 && master==1 && !delays);
  if(scenario==1)assert(state.result==-ENOLINK && !reads && !writes);
  if(scenario==2)assert(state.result==-ETIMEDOUT && delays==2000 && master==1);
  if(scenario==3)assert(state.result==-ENOLINK && writes==1 && master==1 && reads==1);
  if(scenario==4)assert(state.result==-ENOMEM && !reads && master==1);
  if(scenario==5)assert(state.result==-EIO && !reads && !writes);
  if(scenario==6)assert(state.result==-EIO && master==1 && !delays);
 }
 scenario=0;parked=0;
 assert(crash_quiesce_store(&dev,NULL,"1",1)==1 && parked==1);
 capture=false;assert(crash_quiesce_store(&dev,NULL,"1",1)==-EPERM);capture=true;
 board=false;assert(crash_quiesce_store(&dev,NULL,"1",1)==-EPERM);board=true;
 assert(crash_quiesce_store(&dev,NULL,"0",1)==-EPERM);
 pcie.late_ports=2;assert(crash_quiesce_store(&dev,NULL,"1",1)==-EBUSY);pcie.late_ports=0;
 parked=0;endpoint.driver=&dev;
 assert(crash_quiesce_store(&dev,NULL,"1",1)==-EBUSY && !parked);
 panic_resume_once=true;armed=0;apple_h9p_crash_resume_test(&dev);assert(!panic_resume_once && !panics);
 armed=1;board=false;panic_resume_once=true;apple_h9p_crash_resume_test(&dev);assert(panic_resume_once && !panics);
 board=true;if(!setjmp(panic_jump)) {apple_h9p_crash_resume_test(&dev);assert(!"must panic");}
 assert(panics==1 && !panic_resume_once);apple_h9p_crash_resume_test(&dev);assert(panics==1);
 puts("PASS production phoenix ANS: CC0, finite timeout, link/fault/map guards, gate checks and port-0-only park");
}
