#!/usr/bin/env python3
"""Install round-6 guard and generated OPP aliases in the power worktree only."""
from pathlib import Path
import re
import shutil
import subprocess

from ladder import STEPS

if (Path.cwd() != Path('/src/wt/power') or
        subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() != 'a9/power'):
    raise SystemExit('Run only with scripts/build-kernel.sh --run power ...')
shutil.copyfile('/work/scripts/power/kernel/apple-a9-tuning.h', 'drivers/cpufreq/apple-a9-tuning.h')
path = Path('drivers/cpufreq/apple-soc-cpufreq.c')
source = path.read_text()
marker = 'static unsigned int apple_soc_cpufreq_get_rate(unsigned int cpu)'
prototypes = ('static unsigned int a9_psinfo1_offset(unsigned int state);\n'
              'static unsigned int a9_psinfo_khz(u64 word);\n\n')
if prototypes not in source:
    source = source.replace(marker, prototypes + marker, 1)
lookup = '\tcpufreq_for_each_valid_entry(p, policy->freq_table)'
decode = ('\t/* Extended OPPs share slot 14: decode the live word, not its first alias. */\n'
          '\tif (priv->a9 && pstate > 8 && pstate <= 15)\n'
          '\t\treturn a9_psinfo_khz(readq(priv->reg_base + a9_psinfo1_offset(pstate)));\n\n')
if decode not in source:
    if source.count(lookup) != 1:
        raise SystemExit('unexpected get_rate lookup; refusing to guess')
    source = source.replace(lookup, decode + lookup, 1)
path.write_text(source)
path = Path('arch/arm64/boot/dts/apple/s8003.dtsi')
source = path.read_text()
# Replace only our generated block on repeat runs.
source = re.sub(r'\n\t\t/\* Round-6 shared-slot OPPs: begin \*/.*?'
                r'\t\t/\* Round-6 shared-slot OPPs: end \*/\n', '', source, flags=re.S)
marker = '\n\t};\n};\n'
if marker not in source or 'opp-level = <15>;' not in source:
    raise SystemExit('unexpected round-5 OPP layout')
block = '\n\t\t/* Round-6 shared-slot OPPs: begin */\n'
for khz in STEPS:
    if khz <= 2160000:
        continue
    block += (f'\t\topp-r6-{khz} {{\n'
              f'\t\t\topp-hz = /bits/ 64 <{khz * 1000}>;\n'
              '\t\t\topp-level = <14>;\n'
              '\t\t\tclock-latency-ns = <400000>;\n'
              '\t\t\tturbo-mode;\n\t\t};\n')
block += '\t\t/* Round-6 shared-slot OPPs: end */\n'
source = source.replace(marker, block + marker, 1)
path.write_text(source)
