#!/usr/bin/env python3
"""Exercise production shutdown C against mock MMIO and early USB shell in fixtures."""
import os
import re
import gzip
import importlib.machinery
import zlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path('/work')
KERNEL=Path('/src/wt/boot')

def function(path,name):
 s=(KERNEL/path).read_text()
 # The legacy fixtures explicitly test the selectable Round 7 implementation.
 legacy={'apple_s8003_setup_port_once':'apple_s8003_setup_port_round7',
         'apple_h9p_pcie_park':'apple_h9p_pcie_park_round7',
         'nvme_pci_apple_ans_shutdown':'nvme_pci_apple_ans_shutdown_round7'}
 actual=legacy.get(name,name)
 if actual+'(' not in s:actual=name
 start=re.search(r'static (?:void|int) '+actual+r'\(',s).start(); end=s.index('\n}',start)+2
 return s[start:end].replace(actual+'(',name+'(',1)

class ShutdownTests(unittest.TestCase):
 def test_port_restart_retained_state_and_bounded_retry(self):
  path=Path('drivers/pci/controller/pcie-apple-h9p.c')
  names=['apple_s8003_port_off','apple_s8003_port_power_on',
         'apple_s8003_port_stop','apple_s8003_setup_port_once','apple_s8003_setup_port']
  code='\n'.join(function(path,name) for name in names)
  # Production offsets and PCI bit definitions, with only the surrounding
  # kernel APIs mocked. The fixture checks clock/reset ordering, unsafe MMIO,
  # retained PME/LTSSM/config state, bounded recovery and port isolation.
  regs=[]
  for source,pattern in [(KERNEL/path,r'(?:H9P|S8003)_'),
                          (KERNEL/'include/uapi/linux/pci_regs.h',r'PCI_')]:
   for line in source.read_text().splitlines():
    if re.match(r'#define\s+'+pattern,line) and not line.endswith('\\'):
     regs.append(line)
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'registers.h').write_text('\n'.join(regs))
   (p/'production.h').write_text(code)
   subprocess.run(['clang','-Werror=implicit-function-declaration','-I',d,'-o',str(p/'test'),
                   str(ROOT/'scripts/boot/tests/h9p-port.c')],check=True)
   subprocess.run([str(p/'test')],check=True)

 def test_mmio_and_translation_teardown_order(self):
  nvme=function(Path('drivers/nvme/host/pci-apple-ans.c'),'nvme_pci_apple_ans_shutdown')
  stop=function(Path('drivers/pci/controller/pcie-apple-h9p.c'),'apple_s8003_port_stop')
  host=function(Path('drivers/pci/controller/pcie-apple-h9p.c'),'apple_h9p_pcie_park')+'\n'+function(Path('drivers/pci/controller/pcie-apple-h9p.c'),'apple_h9p_pcie_shutdown')
  pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
typedef uint32_t u32;
#define BIT(n) (1UL<<(n))
#define NVME_CC_ENABLE 1
#define NVME_CC_SHN_MASK (3<<14)
#define NVME_CSTS_RDY 1
#define NVME_REG_CC 20
#define H9P_NVMMU_SART_CTRL 32
#define H9P_NUM_PORTS 4
#define H9P_CFG_PORT_STRIDE 256
#define PCI_COMMAND 4
#define PCI_PRIMARY_BUS 24
#define GENMASK(a,b) 0
#define S8003_PORT_PMETO 64
#define S8003_PORT_PMETO_SEND 1
#define S8003_PORT_PMETO_PENDING 2
#define S8003_PORT_PMETO_HW_TIMEOUT 4
#define S8003_PME_TO_ACK_TIMEOUT_US 100
#define S8003_PORT_LINKSTS 72
#define S8003_PORT_LINKSTS_L2 1
#define S8003_L2_TIMEOUT_US 100
#define S8003_PORT_IRQSTAT_LINK_DOWN 1
#define H9P_PORT_IRQSTAT 80
#define S8003_PERST_TO_REFCLK_US 100
#define S8003_RC_PORT_PWR(n) (n)
#define S8003_RC_PORT_REFCLK_NOGATE 256
#define H9P_PORT_IRQMASK_PRE_LINK 1
#define H9P_PORT_IRQSTAT_PRE_LINK 1
#define H9P_PORT_IRQMASK 84
#define H9P_PORT_LTSSMCTL 88
#define H9P_PORT_LTSSM_ENABLE 1
#define __iomem
#define WRITE_ONCE(a,b) ((a)=(b))
#define for_each_set_bit(i,p,n) for((i)=0;(i)<(n);(i)++) if(*(p)&BIT(i))
#define dev_info(...) do {} while(0)
#define dev_warn(...) do {} while(0)
struct device {void *of_node;}; struct pci_dev {int x;};
struct nvme_dev {struct device *dev; struct {u32 ctrl_config;} ctrl; char *bar;};
struct apple_ans_nvme {bool host_fault;};
struct platform_device {struct device dev;};
struct apple_h9p_pcie {struct {bool ready; char *base;} nvmmu; unsigned long enabled_ports,down_ports; int late_work; char *base_rc,*base_config,*base_port[4]; void *perst[4]; struct device *dev;};
static struct apple_ans_nvme ans;
static struct apple_h9p_pcie pcie;
static struct pci_dev pci;
static int scenario,reads,event,write_event,master_event,port_event,sart_event,perst_event;
static void *cc_addr;
static bool kexec_in_progress=true,compatible=true;
static struct apple_ans_nvme *nvme_pci_apple_ans(struct nvme_dev*d){return &ans;}
static struct pci_dev *to_pci_dev(struct device*d){return &pci;}
static int nvme_pci_apple_ans_check_link(struct nvme_dev*d,const char*s){return scenario==1?-ENOLINK:0;}
static int nvme_pci_apple_ans_read_csts(struct nvme_dev*d,u32*s){reads++;event++;*s=scenario==4?1:0;return scenario==3?-EIO:0;}
static void writel(u32 v,void*p){if(p==cc_addr){assert(!(v&(NVME_CC_ENABLE|NVME_CC_SHN_MASK)));write_event=++event;}}
static void pci_clear_master(struct pci_dev*p){master_event=++event;}
#define read_poll_timeout(op,val,cond,a,b,c,...) ({int _r=-ETIMEDOUT;for(int _i=0;_i<4;_i++){val=op(__VA_ARGS__);if(cond){_r=0;break;}}_r;})
static struct apple_h9p_pcie *apple_h9p_pcie_lookup(struct device*d){return &pcie;}
static bool of_device_is_compatible(void*p,const char*s){return compatible;}
static void cancel_delayed_work_sync(int*p){event++;}
static int gpiod_direction_output(void*p,int v){assert(v==1);if(p==(void*)1)perst_event=++event;return 0;}
static void apple_s8003_port_off(struct apple_h9p_pcie*p,unsigned int n){assert(!"kexec must keep ports powered");}
static void h9p_writel_flush(u32 v,void*p){assert(v==0);if(p==pcie.nvmmu.base+H9P_NVMMU_SART_CTRL){assert(perst_event>master_event);sart_event=++event;}}
static void h9p_rmw(void*p,u32 clear,u32 set){}
static u32 readl(void*p){return 0;}
static void writew(unsigned short v,void*p){assert(v==0);}
static void apple_s8003_port_dump(struct apple_h9p_pcie*p,unsigned int n,const char*w){}
static void udelay(int n){}
#define readl_poll_timeout(p,v,cond,a,b) ((v)=~0U,0)
'''
  main=r'''
int main(void) {
 struct device device={0}; struct platform_device platform={0}; char bar[64],cfg[1024],pb[256];
 struct nvme_dev nvme={.dev=&device,.bar=bar};
 cc_addr=bar+NVME_REG_CC;pcie.base_rc=cfg;pcie.base_config=cfg;pcie.base_port[0]=pb;pcie.base_port[1]=pb;
 pcie.perst[0]=(void*)1;pcie.perst[1]=(void*)2;pcie.dev=&device;
 for(scenario=0;scenario<5;scenario++) {
  event=write_event=reads=master_event=port_event=sart_event=perst_event=0;
  ans.host_fault=scenario==2; nvme.ctrl.ctrl_config=NVME_CC_ENABLE|NVME_CC_SHN_MASK;
  pcie.enabled_ports=3;pcie.down_ports=0;pcie.nvmmu.ready=true;pcie.nvmmu.base=bar;
  nvme_pci_apple_ans_shutdown(&nvme,0);
  assert(master_event);
  if(scenario==1||scenario==2) assert(!write_event&&!reads);
  else assert(write_event&&master_event>write_event&&reads);
  apple_h9p_pcie_shutdown(&platform);
  assert(!pcie.enabled_ports&&pcie.down_ports==3&&!pcie.nvmmu.ready);
  assert(master_event<perst_event&&perst_event<sart_event&&!port_event);
 }
 event=0;kexec_in_progress=false;apple_h9p_pcie_shutdown(&platform);assert(!event);
 puts("PASS shutdown success/link-loss/host-fault/CSTS-error/timeout; PERST before SART teardown; ports stay powered");
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(pre+nvme+'\n'+stop+'\n'+host+'\n'+main)
   subprocess.run(['clang','-Werror=implicit-function-declaration','-o',str(p/'test'),str(p/'test.c')],check=True)
   subprocess.run([str(p/'test')],check=True)

class EarlyUSBTests(unittest.TestCase):
 def setup_run(self,bound=True,tty=True,host=True,blocked=False):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)
   for f,v in {'sys/module/nvme/parameters/apple_ans_probe':'N','dev/kmsg':'boot-backlog\n',
               'config/usb_gadget/g1/UDC':'dwc2' if bound else '',
               'sys/bus/pci/devices/0000:01:00.0/vendor':'0x106b',
               'sys/bus/pci/devices/0000:01:00.0/class':'0x018002',
               'sys/bus/pci/drivers_probe':''}.items():
    q=p/f;q.parent.mkdir(parents=True,exist_ok=True);q.write_text(v)
   if tty:(p/'dev/ttyGS0').touch()
   if host:
    h=p/'sys/module/pcie_apple_h9p/parameters/probe';h.parent.mkdir(parents=True);h.write_text('N')
    (p/'sys/bus/platform/devices/610000000.pcie').mkdir(parents=True)
    h=p/'sys/bus/platform/drivers_probe'
    if blocked:os.mkfifo(h)
    else:h.touch()
   src=(ROOT/'scripts/boot/device/early-storage.sh').read_text()
   src=src.replace('>/dev/kmsg','>>/dev/kmsg')
   for prefix in ['/sys/','/config/','/dev/']:src=src.replace(prefix,str(p)+prefix)
   enabled=p/'config/netconsole/mac/enabled';enabled.parent.mkdir(parents=True)
   setup=f'start_netconsole() {{ echo 1 > {enabled}; }}\nusb_if=usb0\nnetconsole_wait=1\n'
   end='\nlinux_a9_early_storage; rc=$?; '
   if blocked:end+='echo "PID1 still running"; sleep 0.1; kill -0 "$early_probe_pid" || exit 9; kill "$early_probe_pid"; '
   r=subprocess.run(['bash','-c',setup+src+end+'wait; linux_a9_early_storage_stop; exit $rc'],text=True,capture_output=True,timeout=5)
   if host:
    self.assertEqual((p/'sys/module/pcie_apple_h9p/parameters/probe').read_text().strip(),'Y' if r.returncode==0 else 'N')
    if not blocked and r.returncode==0:
     self.assertEqual((p/'sys/bus/platform/drivers_probe').read_text().strip(),'610000000.pcie')
   return r,(p/'sys/module/nvme/parameters/apple_ans_probe').read_text(),(p/'sys/bus/pci/drivers_probe').read_text(),(p/'dev/ttyGS0').read_text() if tty else ''
 def test_usb_before_probe(self):
  r,gate,probe,serial=self.setup_run();self.assertEqual(r.returncode,0,r.stderr)
  self.assertEqual(gate.strip(),'Y');self.assertEqual(probe.strip(),'0000:01:00.0')
  self.assertIn('boot-backlog',serial);self.assertIn('EARLY USB READY',r.stdout)
 def test_unbound_gadget_holds_storage(self):
  r,gate,probe,_=self.setup_run(bound=False);self.assertNotEqual(r.returncode,0)
  self.assertEqual(gate,'N');self.assertFalse(probe)
 def test_no_serial_holds_storage(self):
  r,gate,probe,_=self.setup_run(tty=False);self.assertNotEqual(r.returncode,0)
  self.assertEqual(gate,'N');self.assertFalse(probe)
 def test_old_endpoint_only_gate_compatible(self):
  r,gate,probe,_=self.setup_run(host=False);self.assertEqual(r.returncode,0,r.stderr)
  self.assertEqual(gate.strip(),'Y');self.assertTrue(probe)
 def test_blocked_host_probe_does_not_block_pid1(self):
  r,gate,probe,_=self.setup_run(blocked=True);self.assertEqual(r.returncode,0,r.stderr)
  self.assertIn('PID1 still running',r.stdout);self.assertEqual(gate.strip(),'Y');self.assertFalse(probe)
  self.assertLess(r.stdout.index('EARLY USB READY'),r.stdout.index('reprobe platform PCIe host'))

class InitramfsTests(unittest.TestCase):
 def test_real_overlay_preserves_base_and_orders_usb_before_storage(self):
  merge=importlib.machinery.SourceFileLoader('merge',str(ROOT/'scripts/merge-initramfs.py')).load_module()
  def files(path):
   return {merge.norm(n):b for n,h,b in merge.entries(gzip.decompress(path.read_bytes())) if n!='TRAILER!!!'}
  base=files(ROOT/'build/nvme-install/initramfs-omarchy.gz')
  path=ROOT/'build/boot/initramfs-omarchy.gz'; new=files(path)
  for name,body in base.items():
   if name!='init':self.assertEqual(body,new[name],name)
  self.assertEqual(set(new)-set(base),{'linux-a9-early-storage.sh','linux-a9-pstore','linux-a9-ans-state'})
  self.assertEqual(new['linux-a9-ans-state'],(ROOT/'scripts/boot/device/linux-a9-ans-state').read_bytes())
  self.assertEqual(new['linux-a9-pstore'],(ROOT/'scripts/boot/device/linux-a9-pstore').read_bytes())
  self.assertEqual(new['linux-a9-early-storage.sh'],(ROOT/'scripts/boot/device/early-storage.sh').read_bytes())
  init=new['init'].decode(); main=init[init.rindex('mount_proc_sys_dev\nsetup_log'):]
  old_init=base['init'].decode()
  for name in ['setup_gadget','setup_network','start_netconsole']:
   def body(s):
    start=s.index(name+'() {');return s[start:s.index('\n}',start)+2]
   self.assertEqual(body(old_init),body(init),name+' changed from packed base')
  ordered=['setup_gadget ||','setup_network ||','start_shells\n','. /linux-a9-early-storage.sh','linux_a9_early_storage ||','while :; do','if ! connect_nvme;']
  positions=[main.index(x) for x in ordered]
  self.assertEqual(positions,sorted(positions))
  self.assertIn('linux_a9_early_storage_stop\n  stop_shells',init)
  self.assertIn('fallback=rescue\n',init)
  self.assertIn('nvme_wait=120\n',init)
  self.assertIn('nvme_fallback() {\n  linux_a9_storage_status',init)
  self.assertIn('rescue() {\n  command -v linux_a9_rescue_evidence >/dev/null || . /linux-a9-early-storage.sh\n  linux_a9_rescue_evidence\n',init)
  d=zlib.decompressobj(31);d.decompress(path.read_bytes());self.assertTrue(d.eof);self.assertFalse(d.unused_data)

if __name__=='__main__':unittest.main(verbosity=2)
