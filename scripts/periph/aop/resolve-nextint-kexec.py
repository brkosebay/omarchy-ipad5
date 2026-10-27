#!/usr/bin/env python3
"""Resolve only the pinned nextint sleep-helper vs kexecfix extraction conflict.

Keep nextint's shared helper and its nonfatal full-off suspend behavior.
Place kexecfix's powered certificate in the fatal (kexec) path only.
"""
from pathlib import Path
import subprocess

p = Path('drivers/pci/controller/pcie-apple-h9p.c')
assert subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() == 'a9/aop-nextint-prep'
assert subprocess.check_output(['git', 'diff', '--name-only', '--diff-filter=U'], text=True).splitlines() == [str(p)]
s = p.read_text()
assert s.count('<<<<<<< HEAD\n') == s.count('\n=======\n') == 1
start = s.index('<<<<<<< HEAD\n')
end = s.index('\n>>>>>>> ', start)
end = s.index('\n', end + 1) + 1
conflict = s[start:end]
ours, theirs = conflict.split('\n=======\n')
assert ours == '<<<<<<< HEAD\n\t\tapple_s8003_ans_port_down(pcie, true);'
begin = theirs.index('\t\tif (ans_keep_power && pcie->ans_mode == APPLE_ANS_HANDOFF) {')
finish = theirs.index('\t\t} else {', begin)
powered = theirs[begin:finish] + '\t\t}\n'
powered = '\n'.join(line[1:] if line.startswith('\t') else line for line in powered.split('\n'))
powered = powered.replace('if (ans_keep_power &&', 'if (fatal && ans_keep_power &&', 1)
powered = powered.rsplit('\t}', 1)[0] + '\t\tpcie->enabled_ports &= ~BIT(0);\n\t\treturn 0;\n\t}\n'
s = s[:start] + '\t\tapple_s8003_ans_port_down(pcie, true);\n' + s[end:]
anchor = '\tpcie->nvmmu.active = false;\n\tapple_s8003_port_off(pcie, 0);'
assert s.count(anchor) == 1
s = s.replace(anchor, '\tpcie->nvmmu.active = false;\n' +
              '\t/* Powered retention is kexec-only; sleep keeps its full-off certificate. */\n' +
              powered + '\tapple_s8003_port_off(pcie, 0);')
assert '<<<<<<<' not in s and '>>>>>>>' not in s
p.write_text(s)
