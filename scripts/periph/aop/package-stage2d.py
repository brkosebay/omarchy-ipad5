#!/usr/bin/env python3
"""Package IOP mask/CPU run tests and stage 3 chained after IOP mask clear."""
from pathlib import Path
import runpy

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('package-stage23.py')))['main']('s2d')
