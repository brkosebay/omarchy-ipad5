#!/usr/bin/env python3
"""Round 8: compile real kernel functions with faulted MMIO/queues, no device."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path('/work')
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path('/src/wt/boot'))
args, remaining = p.parse_known_args()
KERNEL = args.source
PCIE = 'drivers/pci/controller/pcie-apple-h9p.c'
NVME = 'drivers/nvme/host/pci.c'
DART = 'drivers/iommu/apple-dart.c'


def function(path, name):
    s = (KERNEL / path).read_text()
    m = re.search(r'^(?:static )?(?:[\w *]+\n?)\b' + name + r'\(', s, re.M)
    assert m, name
    return s[m.start():s.index('\n}', m.start()) + 2]


class HandoffTests(unittest.TestCase):
    def test_mode_parser(self):
        self.run_c('ans-handoff-mode.c', [(PCIE, 'ans_boot_mode_set'), (PCIE, 'ans_boot_mode_get')])

    def run_c(self, fixture, pairs, transform=lambda x: x, registers=False, limits=False):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # Round-8f stop is shared with system sleep; keep exercising the
            # production helper as well as its unchanged kexec entry point.
            expanded = []
            for path, name in pairs:
                helper = {'apple_h9p_pcie_park': 'apple_s8003_ans_port_down',
                          'nvme_pci_apple_ans_shutdown': 'nvme_pci_apple_ans_stop',
                          'apple_s8003_setup_port_full': 'apple_s8003_setup_port_reset'}.get(name)
                if helper and helper + '(' in (KERNEL / path).read_text():
                    expanded.append((path, helper))
                expanded.append((path, name))
            (d / 'production.h').write_text('\n\n'.join(function(p, n) for p, n in expanded))
            if limits:
                # Use the real structure and full limits/default validation.
                # Optional atomic/integrity/zoned subvalidators are asserted
                # inactive in the fixture; none are used by this queue.
                header = (KERNEL / 'include/linux/blkdev.h').read_text()
                structs = []
                for name in ('blk_integrity', 'queue_limits'):
                    start = header.index('struct ' + name + ' {')
                    structs.append(header[start:header.index('\n};', start) + 3])
                (d / 'block-limits.h').write_text('\n\n'.join(structs))
                (d / 'block-validation.h').write_text('\n\n'.join([
                    function('include/linux/blkdev.h', 'blk_validate_block_size'),
                    function('block/blk-settings.c', 'blk_validate_limits'),
                    function('block/blk-settings.c', 'blk_set_default_limits')]))
            if registers:
                lines = []
                for path, pattern in [(KERNEL / PCIE, r'(H9P|S8003)_'),
                                      (KERNEL / 'include/uapi/linux/pci_regs.h', r'PCI_')]:
                    lines += [s for s in path.read_text().splitlines()
                              if re.match(r'#define\s+' + pattern, s) and not s.endswith('\\')]
                (d / 'registers.h').write_text('\n'.join(lines))
            source = (ROOT / 'scripts/boot/tests' / fixture).read_text()
            if '#define dev_err(' not in source:
                source = '#define dev_err(...) ((void)0)\n' + source
            (d / 'test.c').write_text(transform(source))
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall',
                            '-Wno-unused-function', '-Wno-unused-but-set-variable',
                            '-Werror=implicit-function-declaration',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(d), str(d / 'test.c'), '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True, timeout=15)

    def test_full_cycle_electrical_sequence_and_bounded_failures(self):
        names = ['apple_s8003_port_off', 'apple_s8003_port_power_on',
                 'apple_s8003_port_stop', 'apple_s8003_link_sample', 'apple_s8003_port_wait', 'apple_s8003_link_finish', 'apple_s8003_setup_port_full', 'apple_s8003_setup_port']
        def fixture(s):
            # Reuse the electrical assertions, exercise the new full sequence.
            s = s.replace('puts("PASS', '''for(int ctl=0;ctl<2;ctl++)for(int clock=0;clock<2;clock++) {
  scenario=0;init_state(false,false);
  raw_write(ctl,rc+S8003_RC_PORT_CTL(0));raw_write(clock,rc+S8003_RC_PORT_PWR(0));
  powered[0]=ctl&&clock;
  assert(!apple_s8003_setup_port(&pcie,0));
 }
 puts("PASS''')
            return (s.replace('#include "production.h"',
                              '#define apple_s8003_setup_port_full apple_s8003_setup_port_once\n#include "production.h"')
                    .replace('retained?1:0,pb[n]+H9P_PORT_LTSSMCTL',
                             'retained&&initially_powered?1:0,pb[n]+H9P_PORT_LTSSMCTL')
                    .replace('mode_off==1 && mode_on==2', 'mode_off==0 && mode_on==1'))
        self.run_c('h9p-port.c', [(PCIE, n) for n in names], fixture, registers=True)

    def test_each_full_bringup_poll_returns_failure_without_extra_mmio(self):
        names = ['apple_s8003_port_off', 'apple_s8003_port_power_on',
                 'apple_s8003_port_stop', 'apple_s8003_link_sample', 'apple_s8003_port_wait', 'apple_s8003_link_finish',
                 'apple_s8003_setup_port_full', 'apple_s8003_setup_port']
        def fixture(s):
            s = s.replace('puts("PASS', r'''void *addresses[]={rc+S8003_RC_PORT_CTL(0),rc+S8003_RC_STAT,
  rc+S8003_RC_PORT_READY(0),pb[0]+H9P_PORT_LTSSMCTL,phy+H9P_PHY_PORTMASK,
  pb[0]+S8003_PORT_LINKSTS,pb[0]+S8003_PORT_LINKCDMSTS};
 const char *steps[]={"port-off","power-ready","port-ready","LTSSM-stop",
  "phy-lanes-ready","link-up","link-L0"};
 for(int i=0;i<7;i++) {
  scenario=0;init_state(false,false);fail_addr=addresses[i];
  assert(apple_s8003_setup_port(&pcie,0)==-ETIMEDOUT);
  assert(perst[0] && strstr(first_error,steps[i]) && polls<=14);
  if(i<5)assert(!attempts[0]);
 }
 puts("PASS''')
            return (s.replace('#include "production.h"',
                              '#define apple_s8003_setup_port_full apple_s8003_setup_port_once\n#include "production.h"')
                    .replace('retained?1:0,pb[n]+H9P_PORT_LTSSMCTL',
                             'retained&&initially_powered?1:0,pb[n]+H9P_PORT_LTSSMCTL')
                    .replace('mode_off==1 && mode_on==2', 'mode_off==0 && mode_on==1'))
        self.run_c('h9p-port.c', [(PCIE, n) for n in names], fixture, registers=True)

    def test_same_entry_normalizes_port_and_alternates(self):
        self.run_c('ans-handoff-port.c', [(PCIE, n) for n in (
            'apple_s8003_setup_port_once',
            'apple_ans_restart_port', 'apple_h9p_pcie_park')], registers=True)

    def test_missing_endpoint_one_full_boot_retry_and_exhaustion(self):
        names = ['apple_s8003_port_off', 'apple_s8003_port_power_on',
                 'apple_s8003_port_stop', 'apple_s8003_link_sample', 'apple_s8003_port_wait', 'apple_s8003_link_finish',
                 'apple_s8003_setup_port_full', 'apple_s8003_deep_cycle', 'apple_s8003_retrain', 'apple_s8003_replay_phy', 'apple_s8003_deep_recover', 'apple_s8003_boot_retry']
        def fixture(s):
            s = s.replace('struct apple_h9p_pcie {', '''#define APPLE_ANS_ROUND7 0
struct apple_h9p_pcie { int ans_mode;bool ans_boot_retry_used;''')
            s = s.replace('if (scenario==1 || (scenario==2 && attempts[n]==1))',
                          'if (scenario==1 || (scenario==2 && attempts[n]<=2))')
            a = s.index('int main(void)')
            s = s[:a] + r'''int main(void) {
 scenario=2;init_state(false,false);pcie.ans_mode=1;
 for(int i=0;i<2;i++)assert(apple_s8003_setup_port_full(&pcie,0)==-ETIMEDOUT);
 assert(!apple_s8003_boot_retry(&pcie,-ETIMEDOUT));
 assert(pcie.ans_boot_retry_used && attempts[0]==3 && !attempts[1]);
 assert(now_us>=430000 && !perst[0]);
 unsigned old=cycles[0];
 assert(apple_s8003_boot_retry(&pcie,-ETIMEDOUT)==-ETIMEDOUT && cycles[0]==old);
 scenario=1;init_state(false,false);pcie.ans_mode=1;
 assert(apple_s8003_boot_retry(&pcie,-ETIMEDOUT)==-ETIMEDOUT);
 assert(attempts[0]==6 && perst[0] && pcie.ans_boot_retry_used);
 scenario=0;init_state(false,false);pcie.ans_mode=1;
 assert(apple_s8003_boot_retry(&pcie,-ENODEV)==-ENODEV && !cycles[0]);
 pcie.ans_mode=0;
 assert(apple_s8003_boot_retry(&pcie,-ETIMEDOUT)==-ETIMEDOUT && !cycles[0]);
 pcie.ans_mode=1;fail_addr=rc+S8003_RC_STAT;
 assert(apple_s8003_boot_retry(&pcie,-ENOLINK)==-ETIMEDOUT && perst[0] && !attempts[0]);
 scenario=0;init_state(false,true);pcie.ans_mode=1;fail_addr=pb[0]+H9P_PORT_LTSSMCTL;
 assert(apple_s8003_boot_retry(&pcie,-ETIMEDOUT)==-ETIMEDOUT && !cycles[0]);
 puts("PASS third-attempt root recovery, one-shot exhaustion, port isolation, Round7/local failure exclusions");
}'''
            return s
        self.run_c('h9p-port.c', [(PCIE, n) for n in names], fixture, registers=True)
        setup = function(PCIE, 'apple_h9p_setup_ports')
        self.assertLess(setup.index('apple_s8003_boot_retry'), setup.index('port %u disabled'))

    def test_deep_gen1_long_hold_stability_loss_and_isolation(self):
        names = ['apple_s8003_port_off', 'apple_s8003_port_power_on',
                 'apple_s8003_port_stop', 'apple_s8003_link_sample', 'apple_s8003_port_wait', 'apple_s8003_link_finish',
                 'apple_s8003_setup_port_full', 'apple_s8003_deep_cycle', 'apple_s8003_retrain', 'apple_s8003_replay_phy', 'apple_s8003_deep_recover']
        def fixture(s):
            s = s.replace('static unsigned mode_off', 'static int stable_reads;\nstatic unsigned mode_off')
            s = s.replace('attempts[n]++;', 'attempts[n]++;stable_reads=0;')
            s = s.replace('if (scenario==1 || (scenario==2 && attempts[n]==1))',
                          'if (scenario==6 && ++stable_reads>50) return 4;\n   if (scenario==1 || (scenario==2 && attempts[n]==1))')
            # An ADT root tunable overwrites TLS: recovery must override it last.
            s = s.replace('for(int n=0;n<2;n++) if(base==pb[n])tunables[n]++;',
                          'if(base==cfg)writew(2,cfg+0x70+PCI_EXP_LNKCTL2);\n for(int n=0;n<2;n++) if(base==pb[n])tunables[n]++;')
            a = s.index('int main(void)')
            return s[:a] + r'''int main(void) {
 scenario=0;init_state(false,true);
 raw_write(0x55aa0001,rc+S8003_RC_PORT_PWR(1));
 assert(!apple_s8003_deep_recover(&pcie));
 assert(now_us>=1210000 && attempts[0]==1 && !attempts[1]);
 assert(pcie.ans_gen1 && pcie.max_speed[0]==1 && !pcie.ans_deep_recovery);
 assert((readw(cfg+0x70+PCI_EXP_LNKCTL2)&PCI_EXP_LNKCTL2_TLS)==1);
 assert(raw32(rc+S8003_RC_PORT_PWR(1))==0x55aa0001 && !cycles[1]);
 scenario=6;stable_reads=0;init_state(false,true);
 assert(apple_s8003_deep_recover(&pcie)==-ENOLINK);
 assert(perst[0] && !powered[0] && !pcie.ans_gen1 && !pcie.ans_deep_recovery);
 scenario=0;init_state(false,true);fail_addr=pb[0]+H9P_PORT_LTSSMCTL;
 assert(apple_s8003_deep_recover(&pcie)==-ETIMEDOUT && !attempts[0]);
 puts("PASS deep off hold, Gen1 after tunables, stable window rejects link loss, isolation and finite stop failure");
}'''
        self.run_c('h9p-port.c', [(PCIE, n) for n in names], fixture, registers=True)

    def test_retrain_and_shared_replay_obey_reset_and_enumeration(self):
        def fixture(s):
            s = s.replace('static int apple_s8003_init_common(struct apple_h9p_pcie *p) {return 0;}',
                          'static unsigned common_calls;\nstatic int apple_s8003_init_common(struct apple_h9p_pcie *p) {common_calls++;return 0;}')
            s = s.replace('(scenario==3 ? 3U : S8003_LTSSM_L0)', '(scenario==9 ? 0x17U : scenario==3 ? 3U : S8003_LTSSM_L0)')
            a = s.index('int main(void)')
            return s[:a] + r'''int main(void) {
 for(int gen1=0;gen1<2;gen1++) {
  scenario=0;init_state(false,true);pcie.link_cap[0]=0x70;cycles[0]=phy_inits[0]=1;
  writew(0,cfg+0x70+PCI_EXP_LNKCTL);
  assert(!apple_s8003_retrain(&pcie,gen1));
  assert(attempts[0]==1 && cycles[0]==1 && pcie.ans_gen1==gen1);
  assert(readw(cfg+0x70+PCI_EXP_LNKCTL)&PCI_EXP_LNKCTL_RL);
  if(gen1)assert((readw(cfg+0x70+PCI_EXP_LNKCTL2)&PCI_EXP_LNKCTL2_TLS)==1);
 }
 scenario=9;init_state(false,true);pcie.link_cap[0]=0x70;
 assert(apple_s8003_retrain(&pcie,false)==-ENOLINK && !attempts[0]);
 scenario=0;init_state(false,false);common_calls=0;
 assert(apple_s8003_replay_phy(&pcie)==-EBUSY && !common_calls);
 pcie.ans_pre_enumeration=true;perst[0]=true;perst[1]=false;
 assert(apple_s8003_replay_phy(&pcie)==-EBUSY && !common_calls);
 perst[1]=true;
 assert(!apple_s8003_replay_phy(&pcie) && common_calls==1);
 raw_write(1,rc+S8003_RC_PORT_CTL(0));
 assert(apple_s8003_replay_phy(&pcie)==-EBUSY && common_calls==1);
 puts("PASS real RL restart, Gen1 PHY path, disabled-state exclusion and shared replay only pre-scan/all-reset/off");
}'''
        self.run_c('h9p-port.c', [(PCIE, n) for n in (
            'apple_s8003_port_stop', 'apple_s8003_link_sample', 'apple_s8003_port_wait',
            'apple_s8003_link_finish', 'apple_s8003_retrain', 'apple_s8003_replay_phy')], fixture, registers=True)

    def test_ladder_order_and_shared_phy_exclusion(self):
        self.run_c('ans-ladder.c', [(PCIE, 'apple_s8003_deep_recover')])

    def test_real_failure_capture_separates_status_and_ltssm_and_samples_20ms(self):
        def fixture(s):
            s = s.replace('#define WRITE_ONCE', '#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))\n#define WRITE_ONCE')
            s = s.replace('static unsigned polls;', 'static unsigned polls, captures;static bool raw4,poll4;')
            s = s.replace('if(error && !*first_error)', 'if(strstr(last_step,"capture training-sample"))captures++;\n if(strstr(last_step,"LINKSTS=0x4"))raw4=true;\n if(strstr(last_step,"LTSSM=0x4/POLL_CONFIG"))poll4=true;\n if(error && !*first_error)')
            s = s.replace('static void apple_s8003_port_dump(struct apple_h9p_pcie *p,unsigned n,const char *when) {assert(powered[n]);}', '')
            s = s.replace('if (scenario==1 ||', 'if (scenario==8)return 4;\n   if (scenario==1 ||')
            s = s.replace('(scenario==3 ? 3U : S8003_LTSSM_L0)', '(scenario==8 ? 4U : scenario==3 ? 3U : S8003_LTSSM_L0)')
            a = s.index('int main(void)')
            return s[:a] + r'''int main(void) {
 scenario=8;init_state(false,true);pcie.link_cap[0]=0x70;u64 next=0;
 assert(apple_s8003_link_sample(&pcie,0,&next));
 assert(raw4 && poll4 && captures==1);
 now_us=19999;apple_s8003_link_sample(&pcie,0,&next);assert(captures==1);
 now_us=20000;apple_s8003_link_sample(&pcie,0,&next);assert(captures==2);
 assert(!strcmp(apple_s8003_ltssm_name(5),"PRE_DETECT_QUIET"));
 assert(!strcmp(apple_s8003_ltssm_name(0x11),"L0"));
 puts("PASS actual capture distinguishes LINKSTS=4 from POLL_CONFIG, samples at 20ms, no unpowered reads");
}'''
        self.run_c('h9p-port.c', [(PCIE, n) for n in (
            'apple_s8003_ltssm_name', 'apple_s8003_port_dump', 'apple_s8003_link_sample')], fixture, registers=True)

    def test_rescue_bus_numbers_preserve_dt_iommu_map(self):
        self.run_c('ans-rescue-bus.c', [(PCIE, 'apple_s8003_rescue_bus')])
        setup = function(PCIE, 'apple_h9p_setup_ports')
        late = function(PCIE, 'apple_h9p_late_port')
        for body in (setup, late):
            self.assertIn('if (pcie->ans_rescue_allowed)\n', body)
            self.assertIn('apple_s8003_rescue_bus(pcie, port)', body)
        rescue = function(PCIE, 'ans_recover_store')
        self.assertLess(rescue.index('apple_s8003_rescue_bus(pcie, 0)'), rescue.index('pci_rescan_bus'))

    def test_gen1_ceiling_survives_generic_failed_link_quirk(self):
        self.run_c('ans-gen1-fixup.c', [(PCIE, 'apple_h9p_ans_gen1_fixup')])

    def test_rescue_gate_never_resets_published_or_failed_controller(self):
        self.run_c('ans-rescue.c', [(PCIE, 'ans_recover_store')])

    def test_enable_and_admin_failures_retry_once_before_publication(self):
        def fixture(s):
            s = s.replace('static int gate_result,', 'static int failure_stage;\nstatic int gate_result,')
            s = s.replace('d->ctrl.ctrl_config=1;return 0;',
                          'd->ctrl.ctrl_config=1;return failure_stage==0 && enables==1?-ETIMEDOUT:0;')
            s = s.replace('static int nvme_init_ctrl_finish(struct ctrl *c,bool suspend) {return 0;}',
                          'static int nvme_init_ctrl_finish(struct ctrl *c,bool suspend) {return failure_stage==1 && enables==1?-ETIMEDOUT:0;}')
            a = s.index('int main(void)')
            return s[:a] + r'''int main(void) {
 for(failure_stage=0;failure_stage<2;failure_stage++) {
  struct nvme_dev d={.ctrl={.state=NVME_CTRL_RESETTING,.admin_q=(void*)1},
   .dma_ops=&nvme_pci_apple_ans_ops,.online_queues=2};
  gate_calls=disables=recoveries=starts=dumps=flushes=enables=0;
  nvme_reset_work(&d.ctrl.reset_work);
  assert(d.ctrl.state==NVME_CTRL_LIVE && starts==1 && enables==2 && recoveries==1 && dumps==1);
 }
 puts("PASS enable/admin failure gets one full recovery before namespace publication");
}'''
        self.run_c('ans-handoff-reset.c', [(NVME, n) for n in (
            'nvme_apple_ans_retry', 'nvme_reset_work')], fixture)

    def test_chain_checks_zero_or_hex_port_state_and_rejects_recovery(self):
        script = (ROOT / 'scripts/boot/warm-chain.sh').read_text()
        a = script.index('\t\t\t\t\tfor reg in RC124')
        loop = script[a:script.index('\n\t\t\t\tfi', a)]
        for registers, success in (
                ('RC124=0 RC114=0x100 RC100=0x1000010;', True),
                ('RC124=0x0 RC114=0 RC100=0;', True),
                ('RC124=0x1 RC114=0 RC100=0;', False),
                ('RC124=0 RC114=0x101 RC100=0;', False),
                ('RC124=0 RC114=0 RC100=0x1000011;', False),
                ('RC124=0 RC114=0;', False),
                ('RC124=garbage RC114=0 RC100=0;', False)):
            test = ('fail() { exit 1; }\nincoming="Apple ANS: incoming port 0 '
                    + registers + ' policy=full-down/full-bringup"\n' + loop)
            run = subprocess.run(['bash', '-c', test], capture_output=True, text=True)
            self.assertEqual(run.returncode == 0, success, registers + run.stderr)
        pattern = re.search(r"^BAD='(.*)'$", script, re.M)[1]
        for line in ('Apple ANS: no root endpoint: port 0 bring-up failed -110; one full controller+port boot retry',
                     'Apple ANS: controller/admin/health bring-up failed -110; one full controller+port retry before namespace publication',
                     'Apple ANS: boot retry exhausted -110; no root endpoint, leaving rescue armed'):
            self.assertEqual(subprocess.run(['grep', '-Eq', pattern], input=line,
                                           text=True).returncode, 0, line)

    def test_shutdown_shst_then_disabled_controller_before_port_reset(self):
        self.run_c('ans-handoff-shutdown.c', [
            ('drivers/nvme/host/pci-apple-ans.c', 'nvme_pci_apple_ans_shutdown')])

    def test_outgoing_fifo_and_used_tags(self):
        def fixture(s):
            s = s.replace('#define READ_ONCE', '#define bitmap_empty(p,n) (!*(p))\n#define READ_ONCE')
            s = s.replace('puts("PASS', '''host.nvmmu.ready=true; host.nvmmu.used[0]=0; host.nvmmu.poisoned=false;
    fifo_stuck=false; assert(!apple_ans_nvmmu_drain(&ep));
    host.nvmmu.used[0]=BIT(14); assert(apple_ans_nvmmu_drain(&ep)==-EBUSY);
    host.nvmmu.used[0]=0; fifo_stuck=true; assert(apple_ans_nvmmu_drain(&ep)==-ETIMEDOUT);
    fifo_stuck=false; host.nvmmu.poisoned=true; assert(apple_ans_nvmmu_drain(&ep)==-EBUSY);
    puts("PASS''')
            return s
        self.run_c('ans-io-nvmmu.c', [(PCIE, n) for n in (
            'apple_h9p_nvmmu_program', 'apple_h9p_nvmmu_invalidate',
            'apple_ans_nvmmu_map', 'apple_ans_nvmmu_reset', 'apple_ans_nvmmu_drain')], fixture)

    def test_dart_order_checked_flush_readback_and_latched_errors(self):
        self.run_c('ans-handoff-dart.c', [(DART, n) for n in (
            'apple_dart_s5l8960x_hw_stream_command',
            'apple_dart_s5l8960x_log_state', 'apple_dart_s5l8960x_attach_config',
            'apple_dart_setup_translation', 'apple_ans_dart_set_active',
            'apple_dart_domain_flush_tlb')])

    def test_health_reads_timeout_and_one_recovery_quota(self):
        self.run_c('ans-handoff-health.c', [(NVME, n) for n in (
            'nvme_apple_ans_health_cmd', 'nvme_apple_ans_health', 'nvme_apple_ans_retry')],
            limits=True)

    def test_gate_errors_publish_normally_without_recovery_or_link_cycle(self):
        self.run_c('ans-handoff-reset.c', [(NVME, n) for n in (
            'nvme_apple_ans_retry', 'nvme_reset_work')])

    def test_dart_valid_bit_required_even_when_transaction_address_changes(self):
        self.run_c('ans-handoff-fault.c', [
            ('drivers/nvme/host/pci-apple-ans.c', 'nvme_pci_apple_ans_new_dart_fault')])

    def test_health_timeout_blocks_dma_without_endpoint_read_or_second_reset(self):
        def fixture(s):
            a = s.index('int main(void)')
            s = s[:a] + '''int main(void) {
    struct device device={0}; struct nvme_command cmd={0};
    struct nvme_dev dev={.dev=&device,.dma_ops=&nvme_pci_apple_ans_ops};
    struct nvme_queue q={.dev=&dev,.qid=1};
    struct blk_mq_hw_ctx h={.driver_data=&q};
    struct request req={.mq_hctx=&h,.state=MQ_RQ_IN_FLIGHT,.nr={.cmd=&cmd}};
    current_req=&req; dev.ctrl.state=NVME_CTRL_CONNECTING;
    dev.apple_ans_flags=1UL<<ANS_HEALTH_ACTIVE;
    assert(nvme_timeout(&req)==BLK_EH_DONE);
    assert(dumps==1 && !reads && disables==1 && !shutdown && !resets && !aborts);
    assert(test_bit(ANS_HEALTH_FAILED,&dev.apple_ans_flags));
    puts("PASS health timeout: snapshot, DMA block, cancellation, no endpoint read/reset loop");
}'''
            return s
        self.run_c('ans-io-timeout.c', [(NVME, 'nvme_timeout')], fixture)

    def test_gate_before_publication_and_fail_closed_dart_order(self):
        reset = function(NVME, 'nvme_reset_work')
        self.assertLess(reset.index('nvme_apple_ans_health(dev, &unhealthy)'), reset.index('nvme_start_ctrl'))
        self.assertEqual(reset.count('goto retry;'), 1)
        self.assertIn('nvme_apple_ans_retry(dev)', reset)
        pre = function('drivers/nvme/host/pci-apple-ans.c', 'nvme_pci_apple_ans_preinit')
        self.assertLess(pre.index('controller stopped'), pre.index('apple_ans_dart_set_active'))
        self.assertLess(pre.index('apple_ans_dart_set_active'), pre.index('nvme_pci_apple_ans_reset_dma'))
        self.assertIn('if (ret)\n\t\t\treturn ret;', pre)
        hw = function(DART, 'apple_dart_hw_reset')
        self.assertLess(hw.index('log_state'), hw.index('apple_dart_hw_disable_dma'))
        self.assertIn('return apple_dart_domain_flush_tlb', function(DART, 'apple_dart_iotlb_sync_map'))
        self.assertIn('flush_error', function(DART, 'apple_dart_map_pages'))
        gpio = function(PCIE, 'apple_h9p_pcie_get_gpios')
        self.assertIn('GPIOD_ASIS : GPIOD_OUT_HIGH', gpio)

    def test_rescue_prints_incoming_log_without_usb_and_keeps_pstore(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'console').touch()
            for name, content in [('linux-a9-ans-state', 'echo incoming-timeout-tag-14'),
                                  ('linux-a9-pstore', 'echo retained-pstore-readable')]:
                f = d / name
                f.write_text('#!/bin/sh\n' + content + '\n')
                f.chmod(0o755)
            script = (ROOT / 'scripts/boot/device/early-storage.sh').read_text()
            for name in ('linux-a9-ans-state', 'linux-a9-pstore'):
                script = script.replace('/' + name, str(d / name))
            script = script.replace('/dev/console', str(d / 'console'))
            subprocess.run(['sh', '-c', script + '\nlinux_a9_rescue_evidence'], check=True)
            log = (d / 'console').read_text()
            self.assertIn('incoming-timeout-tag-14', log)
            self.assertIn('retained-pstore-readable', log)
        state = (ROOT / 'scripts/boot/device/linux-a9-ans-state').read_text()
        self.assertIn("dmesg | grep -Ei", state)
        self.assertNotIn('rm ', (ROOT / 'scripts/boot/device/linux-a9-pstore').read_text())


if __name__ == '__main__':
    unittest.main(argv=[__file__] + remaining, verbosity=2)
