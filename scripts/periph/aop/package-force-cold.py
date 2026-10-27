#!/usr/bin/env python3
"""Package two committed tri49a forced-cold entries; host only."""
from pathlib import Path
import runpy
if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('package-stage23.py')))['main']('cold')
