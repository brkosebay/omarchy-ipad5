#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define H9P_NUM_PORTS 4
#define PCI_SLOT(v) (((v)>>3)&31)
struct pci_bus;
struct device { bool pci; struct pci_dev *pdev; };
struct pci_dev { struct pci_bus *bus; unsigned devfn; };
struct pci_bus { struct pci_bus *parent; struct pci_dev *self; unsigned number; };
struct apple_h9p_pcie { int unused; };
static bool pci_is_root_bus(struct pci_bus *b) { return !b->parent; }
static bool dev_is_pci(struct device *d) { return d->pci; }
static struct pci_dev *to_pci_dev(struct device *d) { return d->pdev; }
/* No readl() implementation: production ownership must not read ECAM. */
#include "production.h"
int main(void) {
    struct apple_h9p_pcie host={0}; struct pci_bus root={0};
    struct pci_dev rp0={.bus=&root,.devfn=0}, rp1={.bus=&root,.devfn=8};
    struct pci_bus b0={.parent=&root,.self=&rp0,.number=1};
    struct pci_bus b1={.parent=&root,.self=&rp1,.number=2};
    struct pci_dev ep={.bus=&b0}; struct device dev={.pci=true,.pdev=&ep};
    assert(apple_h9p_pcie_device_port(&host,&dev)==0);
    ep.bus=&b1; assert(apple_h9p_pcie_device_port(&host,&dev)==1);
    struct pci_dev bridge={.bus=&b0};
    struct pci_bus nested={.parent=&b0,.self=&bridge,.number=13};
    ep.bus=&nested; assert(apple_h9p_pcie_device_port(&host,&dev)==0);
    /* Routing bus numbers can change without moving the endpoint's parent. */
    for (unsigned n=0;n<256;n++) {
        b0.number=n; nested.number=255-n;
        assert(apple_h9p_pcie_device_port(&host,&dev)==0);
    }
    ep.bus=NULL; assert(apple_h9p_pcie_device_port(&host,&dev)==-ENODEV);
    dev.pci=false; assert(apple_h9p_pcie_device_port(&host,&dev)==-ENODEV);
    ep.bus=&root; dev.pci=true; assert(apple_h9p_pcie_device_port(&host,&dev)==H9P_NUM_PORTS);
    puts("PASS topology ownership: port0/port1/nested bridge; no ECAM; bus renumbering; invalid endpoints rejected");
}
