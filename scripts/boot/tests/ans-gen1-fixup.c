#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define PCI_DEVFN(s,f) ((s)<<3|(f))
#define PCI_EXP_LNKCAP2_SLS_2_5GB 2
#define pci_info(...) ((void)0)
struct device {void *of_node;struct device *parent;};
struct pci_dev {void *bus;int devfn;unsigned supported_speeds;};
struct pci_host_bridge {struct device dev;};
struct apple_h9p_pcie {bool ans_gen1;};
static struct device parent;
static struct pci_host_bridge bridge;
static struct apple_h9p_pcie host;
static bool root,compatible;
static bool pci_is_root_bus(void *b) {return root;}
static struct pci_host_bridge *pci_find_host_bridge(void *b) {return &bridge;}
static bool of_device_is_compatible(void *p,const char *s) {return compatible;}
static struct apple_h9p_pcie *apple_h9p_pcie_lookup(struct device *d) {return &host;}
#include "production.h"
int main(void) {
 struct pci_dev d={.supported_speeds=6};
 bridge.dev.parent=&parent;root=compatible=host.ans_gen1=true;
 apple_h9p_ans_gen1_fixup(&d);assert(d.supported_speeds==2);
 for(int guard=0;guard<5;guard++) {
  d=(struct pci_dev){.supported_speeds=6};bridge.dev.parent=&parent;root=compatible=host.ans_gen1=true;
  if(guard==0)root=false;if(guard==1)d.devfn=8;if(guard==2)bridge.dev.parent=NULL;
  if(guard==3)compatible=false;if(guard==4)host.ans_gen1=false;
  apple_h9p_ans_gen1_fixup(&d);assert(d.supported_speeds==6);
 }
 puts("PASS recovery ceiling blocks generic Gen2 lift only on the recovered Apple root port");
}
