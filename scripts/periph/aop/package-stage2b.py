#!/usr/bin/env python3
"""Package the s2b/s3b bounded ladder entries. Host only; no default changes."""
from pathlib import Path
import runpy

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('package-stage23.py')))['main']('s2b')
