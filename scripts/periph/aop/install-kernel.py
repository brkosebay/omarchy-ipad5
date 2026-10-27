#!/usr/bin/env python3
"""Install canonical AOP templates, only inside Docker worktree aop."""
from pathlib import Path
import re
import shutil
import subprocess

SOURCES = ('apple-a9-aop.c', 'apple-a9-aop-active.c', 'a9-aop.h',
           'a9-aop-protocol.h', 'a9-aop-afk.h', 'a9-aop-takeover.h', 'a9-aop-diagnostics.h',
           'a9-aop-memory.h', 'a9-aop-experiment.h', 'a9-aop-system.h', 'a9-aop-ring.h', 'a9-aop-ring-reader.h',
           'a9-aop-ring-reader.c',
           'a9-aop-ring-target.c', 'a9-aop-ring-target.h',
           'a9-aop-iio.c', 'a9-aop-iio.h')
BINDINGS = ('apple,j71t-aop-observer.yaml', 'apple,j71t-aop.yaml')
DOCS = ('apple-a9-aop.rst', 'apple-a9-aop-target.rst')
KCONFIG = '''config APPLE_A9_AOP
	bool "J71t AOP read-only bring-up observer"
	depends on ARCH_APPLE && OF && DEBUG_FS && APPLE_PMGR_PWRSTATE
	select FW_LOADER
	default n
	help
	  Read only five allowlisted AOP wrapper registers after checking all
	  five power domains. Does not attach to genpd, change power, consume
	  FIFOs, request IRQs or write hardware. Also requires the boot parameter
	  apple_a9_aop.debug=1. An optional attach preflight never starts RTKit.
	  ring_scan=1 instead scans linear-mapped System RAM once through debugfs;
	  ring_iio=1 reads a previously reserved inherited ring when IIO=y.
	  All ring modes exclude the active driver and all wrapper access.
	  With debug=1 ring_scan=1, ring_scan_early=1 captures at postcore initcall
	  before device probing. E1 skips protected/unreadable pages and reports
	  bounded coverage; it never reserves or writes the inherited rings.
	  ring_scan_reserved=1 also reads reserved linear-mapped System RAM;
	  no-map pages and MMIO remain excluded.
	  With debug=1, matching ring_pa/ring_pa_size lists capture up to four
	  exact linear-mapped System RAM ranges twice at postcore, 1 s apart.
	  Each range is at most 64 KiB. No source reservation or write occurs.
	  Independent state_probe=1 captures only CPU status and outbox enable at
	  probe after the five-domain PMGR check; debugfs repeats the saved line.
	  It needs neither debug=1 nor ring mode and excludes the active driver.
	  Independent attach_stage=1 freezes four PMGR-guarded mailbox status
	  registers at probe, without consuming data. Stages 2/3 instead select
	  an explicitly unverified one-shot HELLO / reserved-AFK accel experiment
	  in APPLE_A9_AOP_ACTIVE, with all other observer modes and active=0.

config APPLE_A9_AOP_IIO
	bool "J71t AOP accelerometer IIO consumer"
	depends on APPLE_A9_AOP && IIO
	select IIO_BUFFER
	select IIO_TRIGGERED_BUFFER
	default n
	help
	  IIO accelerometer for the AOP accel service. Registered only by the
	  active driver after the firmware delivered a first report.

config APPLE_A9_AOP_ACTIVE
	bool "J71t AOP active driver (firmware boot, RTBuddy, SPU/AFK)"
	depends on APPLE_A9_AOP_IIO && HAS_DMA
	select CRYPTO_LIB_SHA256
	default n
	help
	  Load and start the AOP firmware from /lib/firmware/apple/aop, run
	  the RTBuddy management and SPU/AFK v8 transport, bind the accel
	  service and register its IIO device. Experimental; inert unless the
	  boot parameter apple_a9_aop.active=1 is given. Never resets the AOP;
	  a running AOP needs fingerprinted, reserved ext memory and two ping
	  acks. Also requires apple_a9_aop.attach=1 for the old attach path.
	  Independent attach_stage=2/3 bypasses that proof under accepted crash
	  risk. No cold loader, AON writes or IRQs; stage 3 grants reserved RAM.
'''


def main():
    here = Path.cwd()
    if str(here) != '/src/wt/aop':
        raise SystemExit('Run with scripts/build-kernel.sh --run aop')
    branch = subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip()
    if branch != 'a9/aop':
        raise SystemExit(f'Refusing branch {branch}')
    source = Path('/work/scripts/periph/aop/kernel')
    maintainers = here / 'MAINTAINERS'
    text = maintainers.read_text()
    addition = (source / 'MAINTAINERS.a9-aop').read_text().rstrip() + '\n\n'
    title = addition.splitlines()[0] + '\n'
    if title in text:
        start = text.index(title)
        end = text.index('\n\n', start) + 2
        text = text[:start] + addition + text[end:]
    else:
        text = text.replace('APPLE BCM5974 MULTITOUCH DRIVER\n',
                            addition + 'APPLE BCM5974 MULTITOUCH DRIVER\n', 1)
    maintainers.write_text(text)
    soc = here / 'drivers/soc/apple'
    for name in SOURCES:
        shutil.copyfile(source / name, soc / name)
    header = here / 'include/linux/soc/apple/pmgr-observe.h'
    header.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / header.name, header)
    bindings = here / 'Documentation/devicetree/bindings/soc/apple'
    bindings.mkdir(parents=True, exist_ok=True)
    for name in BINDINGS:
        shutil.copyfile(source / name, bindings / name)
    for name in DOCS:
        shutil.copyfile(source / name, here / 'Documentation/driver-api' / name)
    pmgr = here / 'drivers/pmdomain/apple/pmgr-pwrstate.c'
    text = pmgr.read_text()
    include = '#include <linux/soc/apple/pmgr-observe.h>\n'
    if include not in text:
        text = text.replace('#include <linux/module.h>\n', '#include <linux/module.h>\n' + include)
    if '#include <linux/of_platform.h>\n' not in text:
        text = text.replace('#include <linux/of_address.h>\n', '#include <linux/of_address.h>\n#include <linux/of_platform.h>\n')
    marker = '/* A9 AOP observer API: BEGIN */'
    end_marker = '/* A9 AOP observer API: END */'
    addition = marker + '\n#if IS_ENABLED(CONFIG_APPLE_A9_AOP)\n' + (source / 'pmgr-observe.inc').read_text() + '#endif\n' + end_marker + '\n\n'
    if marker in text:
        start, end = text.index(marker), text.index(end_marker) + len(end_marker) + 2
        text = text[:start] + addition + text[end:]
    else:
        text = text.replace('static const struct of_device_id apple_pmgr_ps_of_match[]', addition + 'static const struct of_device_id apple_pmgr_ps_of_match[]')
    # Publish only after successful initialization, independent of baseline's
    # PMGR probe details. No existing power behavior is changed.
    anchor = '\n\treturn 0;\nerr_remove:'
    if 'platform_set_drvdata(pdev, ps);' not in text:
        if anchor not in text:
            raise RuntimeError('PMGR probe anchor changed')
        text = text.replace(anchor, '\n\tplatform_set_drvdata(pdev, ps);\n' + anchor)
    pmgr.write_text(text)
    kconfig = soc / 'Kconfig'
    text = kconfig.read_text()
    # Drop any earlier AOP blocks, then insert the canonical set at a stable
    # interior anchor instead of the shared append location.
    text = re.sub(r'config APPLE_A9_AOP(?:_IIO|_ACTIVE)?\n.*?(?=\nconfig |\nendmenu)', '', text, flags=re.S)
    if 'config APPLE_MAILBOX\n' not in text:
        raise RuntimeError('Kconfig anchor APPLE_MAILBOX changed')
    text = text.replace('config APPLE_MAILBOX\n', KCONFIG + '\nconfig APPLE_MAILBOX\n', 1)
    kconfig.write_text(re.sub(r'\n{3,}', '\n\n', text))
    makefile = soc / 'Makefile'
    text = makefile.read_text()
    lines = ('obj-$(CONFIG_APPLE_A9_AOP) += apple-a9-aop.o\n',
             'obj-$(CONFIG_APPLE_A9_AOP) += a9-aop-ring-reader.o\n',
             'obj-$(CONFIG_APPLE_A9_AOP) += a9-aop-ring-target.o\n',
             'obj-$(CONFIG_APPLE_A9_AOP_IIO) += a9-aop-iio.o\n',
             'obj-$(CONFIG_APPLE_A9_AOP_ACTIVE) += apple-a9-aop-active.o\n')
    text = ''.join(line for line in text.splitlines(keepends=True) if line not in lines)
    text = text.rstrip() + '\n'
    text = text.replace('obj-$(CONFIG_APPLE_MAILBOX)', ''.join(lines) + '\nobj-$(CONFIG_APPLE_MAILBOX)', 1)
    makefile.write_text(re.sub(r'\n{3,}', '\n\n', text))
    dts = here / 'arch/arm64/boot/dts/apple/s8003-j71t.dts'
    text = dts.read_text()
    old = '\n/* Diagnostic only:'
    if old in text:
        text = text[:text.index(old)]
    # The small /soc fragment can precede the shared SoC include. Phandles
    # resolve after parsing; the common include supplies the bus properties.
    include = '#include "s8003-j71t-aop-observer.dtsi"\n'
    if include not in text:
        text = text.replace('#include "s8003.dtsi"', include + '#include "s8003.dtsi"', 1)
    reservation = '/memreserve/ 0x858000000 0x400000;\n\n'
    if reservation not in text:
        # Header reservations are retained by the warm merger even when a
        # recovery kernel predates the static no-map AFK node.
        text = text.replace(include, reservation + include, 1)
    firmware_reservation = '/memreserve/ 0x210e00000 0x9b000;\n'
    if firmware_reservation not in text:
        text = text.replace(reservation, firmware_reservation + reservation, 1)
    active = '#include "s8003-j71t-aop.dtsi"\n'
    if active not in text:
        text = text.replace(include, include + active, 1)
    for name in ('s8003-j71t-aop-observer.dtsi', 's8003-j71t-aop.dtsi'):
        shutil.copyfile(source / name, dts.parent / name)
    dts.write_text(text)
    print('Installed AOP observer, active driver and IIO consumer in a9/aop')


if __name__ == '__main__':
    main()
