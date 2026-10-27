#!/usr/bin/env python3
"""Compile production ANS powered-reset/fallback and shutdown C with mock MMIO."""
import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('handoff', HERE / 'test-ans-handoff-offline.py')
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)
PCIE = handoff.PCIE


class KeepPowerTests(unittest.TestCase):
    run_c = handoff.HandoffTests.run_c

    def test_powered_reset_and_real_full_cycle_fallback(self):
        names = ['apple_s8003_port_off', 'apple_s8003_port_power_on',
                 'apple_s8003_port_stop', 'apple_s8003_link_sample',
                 'apple_s8003_port_wait', 'apple_s8003_link_finish',
                 'apple_s8003_setup_port_full', 'apple_s8003_setup_port_once',
                 'apple_s8003_setup_port']

        def fixture(s):
            s = s.replace('struct apple_h9p_pcie {', '''
enum apple_ans_boot_mode { APPLE_ANS_ROUND7, APPLE_ANS_HANDOFF, APPLE_ANS_FULL_CYCLE };
static const char *ans_boot_modes[] = {"round7", "handoff", "full-cycle"};
static bool ans_keep_power, warm_boot = true;
static void *of_chosen;
static bool of_property_read_bool(void *n,const char *p) {return warm_boot;}
struct apple_h9p_pcie { enum apple_ans_boot_mode ans_mode;''')
            s = s.replace('static unsigned mode_off', 'static unsigned clock_off;\nstatic unsigned mode_off')
            s = s.replace('static void writel(u32 v,void *p) {', '''static void writel(u32 v,void *p) {
 if(p==rc+S8003_RC_PORT_REFCLK(0) && !(v&1))clock_off++;''')
            s = s.replace('assert(!perst[n] && cycles[n] && phy_inits[n]);',
                          'assert(!perst[n] && phy_inits[n]);')
            s = s.replace('#include "production.h"', '''
static int apple_s8003_setup_port_round7(struct apple_h9p_pcie *h,unsigned p) {
 assert(!"legacy path not under test");return 0;
}
#include "production.h"''')
            s = s[:s.index('int main(void)')] + r'''
static void prepare(void) {
 init_state(false,true);pcie.ans_mode=APPLE_ANS_HANDOFF;
 ans_keep_power=true;warm_boot=true;perst[0]=true;clock_off=0;
 raw_write(0x101,rc+S8003_RC_PORT_PWR(0));
 raw_write(1,rc+S8003_RC_PORT_REFCLK(0));
}
int main(void) {
 scenario=0;prepare();
 unsigned char old_pb[H9P_PORT_SIZE];memcpy(old_pb,pb[1],sizeof(old_pb));
 u32 wifi_power=raw32(rc+S8003_RC_PORT_PWR(1));
 assert(!apple_s8003_setup_port(&pcie,0));
 assert(attempts[0]==1 && !cycles[0] && !clock_off && !mode_on && !mode_off);
 assert(now_us>=100000 && tunables[0]==1 && phy_inits[0]==1);
 assert(raw32(rc+S8003_RC_PORT_REFCLK(0))==1 && powered[0] && !perst[0]);
 assert(!memcmp(old_pb,pb[1],sizeof(old_pb)) && !attempts[1] && !cycles[1]);
 assert(raw32(rc+S8003_RC_PORT_PWR(1))==wifi_power);
 /* The failed powered attempt must fall back to the REAL full sequence. */
 scenario=2;prepare();assert(!apple_s8003_setup_port(&pcie,0));
 assert(attempts[0]==2 && cycles[0]==1 && clock_off && mode_on==1 && !perst[0]);
 /* The original two-attempt outer quota remains finite on persistent loss. */
 scenario=1;prepare();assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);
 assert(attempts[0]==4 && cycles[0]==2 && perst[0]);
 /* A stop timeout never authorizes endpoint release/training. */
 scenario=0;prepare();fail_addr=pb[0]+H9P_PORT_LTSSMCTL;
 assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);
 assert(!attempts[0] && perst[0]);
 /* A local capability error returns directly, without cycling/retrying. */
 scenario=5;prepare();assert(apple_s8003_setup_port(&pcie,0)==-ENODEV);
 assert(!attempts[0] && !cycles[0] && perst[0]);
 /* Default-off, full-cycle, cold boot and partial certificates use full reset. */
 for(int fail=0;fail<9;fail++) {
  scenario=0;prepare();
  if(fail==0)ans_keep_power=false;
  if(fail==1)pcie.ans_mode=APPLE_ANS_FULL_CYCLE;
  if(fail==2)warm_boot=false;
  if(fail==3)perst[0]=false;
  if(fail==4)raw_write(0,rc+S8003_RC_PORT_REFCLK(0));
  if(fail==5)raw_write(1,rc+S8003_RC_PORT_PWR(0));
  if(fail==6)raw_write(0x101,rc+S8003_RC_PORT_CTL(0));
  if(fail==7){raw_write(0,rc+S8003_RC_PORT_CTL(0));powered[0]=false;}
  if(fail==8){raw_write(0,rc+S8003_RC_PORT_PWR(0));powered[0]=false;}
  assert(!apple_s8003_setup_port(&pcie,0));
  assert(attempts[0]==1 && cycles[0]==1 && mode_on==1);
 }
 scenario=0;prepare();raw_write(0,rc+S8003_RC_PORT_READY(0));
 assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);
 assert(cycles[0]==2 && !attempts[0] && perst[0]);
 puts("PASS real powered reset, 100ms PERST, tunables/PHY/MSI, full fallback, finite failures, cold/control gates, Wi-Fi isolation");
}
'''
            return s

        self.run_c('h9p-port.c', [(PCIE, n) for n in names], fixture, registers=True)

    def test_outgoing_certificates_and_control_modes(self):
        def fixture(s):
            return s[:s.index('int main(void)')] + r'''
int main(void) {
 init();ans_keep_power=true;inactive=1;
 apple_h9p_pcie_park(&host,3);
 assert(pin && !disabled_clock && !disabled_power && legacy_ports==2);
 assert(sart==1 && !host.nvmmu.ready && !host.nvmmu.active);
 assert(host.enabled_ports==2 && (host.down_ports&1));
 assert((readl(rc+S8003_RC_PORT_PWR(0))&0x101)==0x101);
 assert(readl(rc+S8003_RC_PORT_CTL(0))&1);
 assert(readl(rc+S8003_RC_PORT_REFCLK(0))&1);
 for(int fail=0;fail<5;fail++) {
  init();ans_keep_power=true;inactive=1;
  if(fail==0)gpio_error=-EIO;
  if(fail==1)stop_error=-ETIMEDOUT;
  if(fail==2)*(u32*)(rc+S8003_RC_PORT_REFCLK(0))=0;
  if(fail==3)*(u32*)(rc+S8003_RC_PORT_READY(0))=0;
  if(fail==4)*(u32*)(rc+S8003_RC_PORT_CTL(0))|=0x100;
  if(!setjmp(jump)){apple_h9p_pcie_park(&host,3);assert(0);}
  assert(host.enabled_ports==3 && !disabled_clock && !disabled_power);
 }
 for(int mode=APPLE_ANS_HANDOFF;mode<=APPLE_ANS_FULL_CYCLE;mode++) {
  init();host.ans_mode=mode;ans_keep_power=mode==APPLE_ANS_FULL_CYCLE;inactive=1;
  apple_h9p_pcie_park(&host,3);
  assert(pin && disabled_clock<delay_event && delay_event<disabled_power);
  assert(!(readl(rc+S8003_RC_PORT_REFCLK(0))&1));
  assert(!(readl(rc+S8003_RC_PORT_PWR(0))&1));
  assert(!(readl(rc+S8003_RC_PORT_CTL(0))&1));
  assert(legacy_ports==2 && host.enabled_ports==2);
 }
 init();host.ans_mode=APPLE_ANS_ROUND7;ans_keep_power=true;
 apple_h9p_pcie_park(&host,3);assert(legacy_ports==3 && !gpio);
 puts("PASS PERST/LTSSM/SART before powered certificate; failures refuse kexec; default, full-cycle, Round7 and Wi-Fi controls");
}
'''

        self.run_c('ans-handoff-port.c', [(PCIE, 'apple_h9p_pcie_park')],
                   fixture, registers=True)


if __name__ == '__main__':
    unittest.main(argv=['test-ans-keep-power-offline', *handoff.remaining], verbosity=2)
