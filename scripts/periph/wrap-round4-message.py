#!/usr/bin/env python3
"""Wrap only the current Round 4 commit message, without altering its tree."""
from pathlib import Path
import subprocess
import textwrap

if Path.cwd() != Path('/src/wt/periph'):
    raise SystemExit('wrong tree')
text = subprocess.check_output(['git', 'log', '-1', '--format=%B'], text=True).strip()
subject, body = text.split('\n\n', 1)
message = subject + '\n\n' + '\n\n'.join(textwrap.fill(p, width=72) for p in body.split('\n\n')) + '\n'
subprocess.run(['git', 'commit', '--amend', '--file=-'], input=message, text=True, check=True)
