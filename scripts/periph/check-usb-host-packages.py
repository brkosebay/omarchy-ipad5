#!/usr/bin/env python3
"""Check the offline package hashes and AArch64 ELF page alignment."""
import hashlib
import importlib.machinery
from pathlib import Path
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
audit = importlib.machinery.SourceFileLoader('page_audit', str(ROOT / 'scripts/omarchy/rootfs/elf-page-audit.py')).load_module()
packages = ROOT / 'build/usb-host/pkgs'
count = 0
with tempfile.TemporaryDirectory() as directory:
    for line in (packages / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split()
        file = packages / name
        assert hashlib.sha256(file.read_bytes()).hexdigest() == digest, name
        with tarfile.open(file) as archive:
            for member in archive:
                if not member.isfile():
                    continue
                data = archive.extractfile(member).read()
                if not data.startswith(b'\x7fELF'):
                    continue
                target = Path(directory) / 'elf'
                target.write_bytes(data)
                info = audit.elf_info(target)
                if info is None:
                    continue
                alignment, needs, embeds, page = info
                assert alignment >= 0x4000, (name, member.name, alignment)
                assert not (needs or embeds), (name, member.name, 'jemalloc')
                count += 1
print(f'PASS package SHA256 and 16K ELF audit: {count} AArch64 ELF files')
