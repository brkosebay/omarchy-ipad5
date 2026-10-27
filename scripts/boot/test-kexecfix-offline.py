#!/usr/bin/env python3
"""Exercise the production missing-ANS late-port guard, without a device."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path('/src/wt/kexecfix'))
args = p.parse_args()
HERE = Path(__file__).resolve().parent
source = (args.source / 'drivers/pci/controller/pcie-apple-h9p.c').read_text()


def function(name):
    m = re.search(r'^static [\w *]+\b' + name + r'\(', source, re.M)
    return source[m.start():source.index('\n}', m.start()) + 2]


def run_c(text):
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 'test.c').write_text(text)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror=implicit-function-declaration',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-I', tmp, str(d / 'test.c'), '-o', str(d / 'test')], check=True)
        subprocess.run([str(d / 'test')], check=True)


class KexecFixTests(unittest.TestCase):
    def test_missing_ans_never_trains_or_publishes_wifi_against_dart0(self):
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define APPLE_ANS_ROUND7 0
#define H9P_LATE_PORT_TRIES 3
#define H9P_LATE_PORT_RETRY_MS 100
#define BIT(n) (1UL<<(n))
#define READ_ONCE(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define for_each_set_bit(n,p,l) for(n=0;n<l;n++)if(*(p)&BIT(n))
#define H9P_NUM_PORTS 4
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define msecs_to_jiffies(x) (x)
struct work_struct {int x;};
struct delayed_work {struct work_struct work;};
struct bridge {void *bus;};
struct apple_h9p_pcie {
 struct delayed_work late_work;
 unsigned long late_ports, enabled_ports;
 unsigned late_tries;
 int ans_mode;
 struct bridge *bridge;
 void *dev;
};
static bool j71t=true;
static int trained,scanned,scheduled,link_result;
static struct delayed_work *to_delayed_work(struct work_struct *w) {return container_of(w,struct delayed_work,work);}
static bool of_machine_is_compatible(const char *s) {return j71t;}
static int apple_h9p_late_port(struct apple_h9p_pcie *p,unsigned n) {trained++;return link_result;}
static void pci_lock_rescan_remove(void) {}
static void pci_unlock_rescan_remove(void) {}
static void pci_rescan_bus(void *b) {scanned++;}
static void schedule_delayed_work(struct delayed_work *w,int delay) {scheduled++;}
'''
        fixture += function('apple_h9p_late_ports_work')
        fixture += r'''
int main(void) {
 struct bridge b={0};
 struct apple_h9p_pcie h={.ans_mode=1,.late_ports=BIT(1),.bridge=&b};
 apple_h9p_late_ports_work(&h.late_work.work);
 assert(!trained && !scanned && !scheduled && h.late_ports==BIT(1));
 /* Restored ANS has already reserved bus 1; Wi-Fi may now take bus 2. */
 h.enabled_ports=BIT(0);
 apple_h9p_late_ports_work(&h.late_work.work);
 assert(trained==1 && scanned==1 && !h.late_ports);
 /* Round 7 and other boards retain their existing behavior. */
 h.enabled_ports=0;h.late_ports=BIT(1);h.ans_mode=APPLE_ANS_ROUND7;
 apple_h9p_late_ports_work(&h.late_work.work);assert(trained==2 && scanned==2);
 h.late_ports=BIT(1);h.ans_mode=1;j71t=false;
 apple_h9p_late_ports_work(&h.late_work.work);assert(trained==3 && scanned==3);
 /* GPIO deferral and failed links still cannot publish an endpoint. */
 j71t=true;h.enabled_ports=BIT(0);h.late_ports=BIT(1);link_result=-EPROBE_DEFER;
 apple_h9p_late_ports_work(&h.late_work.work);assert(scheduled==1 && h.late_ports==BIT(1) && scanned==3);
 link_result=-ETIMEDOUT;
 apple_h9p_late_ports_work(&h.late_work.work);assert(!h.late_ports && scanned==3);
 puts("PASS absent ANS blocks late Wi-Fi before power/MMIO/scan; ANS-present, legacy, GPIO and link-failure paths");
}
'''
        run_c(fixture.replace('#include <errno.h>', '#include <errno.h>\n#define EPROBE_DEFER 517'))

    def test_only_successful_ans_rescan_reschedules_wifi(self):
        fixture = (HERE / 'tests/ans-rescue.c').read_text()
        fixture = fixture.replace('#include "production.h"', function('ans_recover_store'))
        fixture = fixture.replace('.ans_rescue_allowed=true,.bridge=', '.ans_rescue_allowed=true,.late_ports=2,.bridge=')
        fixture = fixture.replace('&& scans==1);', '&& scans==1 && scheduled==1);')
        fixture = fixture.replace('&& !scans);', '&& !scans && !scheduled);')
        run_c(fixture)


if __name__ == '__main__':
    unittest.main(argv=['test-kexecfix-offline'], verbosity=2)
