#!/usr/bin/env python3
"""Install round-2 kernel sources into the a9/power Docker worktree only."""
from pathlib import Path
import shutil
import subprocess

tree = Path.cwd()
branch = subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip()
if branch != 'a9/power' or str(tree) != '/src/wt/power':
    raise SystemExit('Run with scripts/build-kernel.sh --run power ...; refusing other trees')
source = Path('/work/scripts/power/kernel')
for name, target in (
        ('apple-s8003-thermal.c', 'drivers/thermal/apple-s8003-thermal.c'),
        ('apple,s8003-thermal.yaml', 'Documentation/devicetree/bindings/thermal/apple,s8003-thermal.yaml'),
        ('s8003-thermal.dtsi', 'arch/arm64/boot/dts/apple/s8003-thermal.dtsi'),
        ('apple-a9-tuning.h', 'drivers/cpufreq/apple-a9-tuning.h'),
        ('s8003-power.h', 'include/linux/soc/apple/s8003-power.h'),
        ('apple-soc-cpufreq.c', 'drivers/cpufreq/apple-soc-cpufreq.c')):
    shutil.copyfile(source / name, tree / target)
for target, line in (
    ('drivers/thermal/Makefile', 'obj-$(CONFIG_APPLE_S8003_THERMAL) += apple-s8003-thermal.o\n'),
    ('arch/arm64/boot/dts/apple/s8003.dtsi', '#include "s8003-thermal.dtsi"\n')):
    path = tree / target
    text = path.read_text()
    if line.strip() not in text:
        path.write_text(text + '\n' + line)
path = tree / 'drivers/thermal/Kconfig'
text = path.read_text()
if 'config APPLE_S8003_THERMAL' not in text:
    pos = text.rfind('\nendif')
    text = text[:pos] + '''
config APPLE_S8003_THERMAL
    tristate "Apple S8003 on-die temperature sensors"
    depends on ARCH_APPLE || COMPILE_TEST
    depends on OF && THERMAL_OF
    depends on HWMON
    help
      Six calibrated SOC/TCC thermal sensors and hwmon inputs on Apple A9
      S8003. Preserves bootloader trim and hardware SoCHOT protections.
''' + text[pos:]
    path.write_text(text)
path = tree / 'drivers/cpufreq/Kconfig.arm'
text = path.read_text()
if 'config APPLE_A9_TUNING' not in text:
    text += '''
config APPLE_A9_TUNING
    bool "Guarded experimental S8003 CPU clocks"
    depends on ARM_APPLE_SOC_CPUFREQ=y
    depends on APPLE_S8003_THERMAL=y && CPU_THERMAL && DEBUG_FS
    depends on !THERMAL_EMULATION
    help
      Root-only, default-off, heartbeat-protected S8003 stock boost and
      experimental clocks. No voltage increase is supported until the DAC
      transfer function is independently verified. Compile-time ceiling is
      stock maximum +50000 uV. Enable with apple_soc_cpufreq.a9_tuning=1.
'''
    path.write_text(text)
elif '    depends on !THERMAL_EMULATION' not in text:
    path.write_text(text.replace('    depends on APPLE_S8003_THERMAL=y && CPU_THERMAL && DEBUG_FS',
                                '    depends on APPLE_S8003_THERMAL=y && CPU_THERMAL && DEBUG_FS\n    depends on !THERMAL_EMULATION'))
text = path.read_text()
text = text.replace('    depends on ARM_APPLE_SOC_CPUFREQ\n',
                    '    depends on ARM_APPLE_SOC_CPUFREQ=y\n')
path.write_text(text)
text = path.read_text().replace(
    'experimental clocks. No voltage increase is supported until the DAC\n'
    '      transfer function is independently verified. Compile-time ceiling is\n'
    '      stock maximum +50000 uV.',
    'experimental clocks with decoded CPU core/buck7 DAC offsets.\n'
    '      Exact 3125 uV increments, bounded per rail to the cloned stock state\n'
    '      plus 50000 uV. No PMIC or factory-calibration writes are exposed.')
path.write_text(text)
path = tree / 'drivers/thermal/Kconfig'
text = path.read_text().replace(
    'Six calibrated SOC/TCC thermal sensors and hwmon inputs on Apple A9\n'
    '      S8003. Preserves bootloader trim and hardware SoCHOT protections.',
    'Validated SOC0/SOC1/TCC2 zones and six-channel raw diagnostics on A9\n'
    '      S8003. Preserves bootloader trim and hardware SoCHOT protections.')
path.write_text(text)
path = tree / 'arch/arm64/boot/dts/apple/s8003.dtsi'
text = path.read_text()
text = text.replace('#if 0\n\t\t/* Not available until CPU deep sleep is implemented */',
                    '/* Opt-in stock boost; hardware load restriction is preserved. */')
text = text.replace('1844000000', '1848000000')
text = text.replace('\n#endif\n\t};', '\n\t};', 1)
if '\t\topp09 {' not in text:
    marker = '\n\t};\n};'
    pos = text.index(marker)
    opps = ''
    for state, khz in enumerate((1896000, 1944000, 1992000, 2040000, 2088000, 2160000), 9):
        opps += f'''
\t\t/* Guarded experimental slot; inaccessible until explicitly configured. */
\t\topp{state:02d} {{
\t\t\topp-hz = /bits/ 64 <{khz * 1000}>;
\t\t\topp-level = <{state}>;
\t\t\tclock-latency-ns = <400000>;
\t\t\tturbo-mode;
\t\t}};
'''
    text = text[:pos] + opps + text[pos:]
path.write_text(text)
