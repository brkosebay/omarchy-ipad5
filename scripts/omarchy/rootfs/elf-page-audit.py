#!/usr/bin/env python3
"""16K-page audit of an aarch64 root filesystem.

The iPad kernel uses 16K pages (CONFIG_ARM64_16K_PAGES). Two classes of
userspace break there:

  ALIGN     an ELF whose PT_LOAD segments are aligned to less than 16K
            (p_align < 0x4000). The kernel refuses to map it (execve fails,
            dlopen fails with "ELF load command address/offset not page-aligned").
            Arch Linux ARM's toolchain links at 64K, so these are prebuilt
            third-party binaries.
  JEMALLOC  jemalloc built for 4K pages (LG_PAGE 12) aborts with
            "<jemalloc>: Unsupported system page size". Reported for every ELF
            that links libjemalloc or embeds it, with the page size it was
            built for when the binary says.

Usage: elf-page-audit.py ROOT   (prints one tab-separated line per finding,
then a summary; paths are absolute on the build host)
"""
import os
import re
import struct
import sys

PAGE = 0x4000
SKIP_DIRS = {"proc", "sys", "dev", "run", "tmp"}
JEMALLOC_RE = re.compile(rb"jemalloc|je_malloc_conf|_rjem_")
LG_PAGE_RE = re.compile(rb"--with-lg-page=(\d+)")


def elf_info(path):
    """(min PT_LOAD p_align, needs jemalloc, embeds jemalloc, lg_page) or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
            if len(head) < 64 or head[:4] != b"\x7fELF" or head[4] != 2 or head[5] != 1:
                return None
            e_type, e_machine = struct.unpack_from("<HH", head, 16)
            if e_machine != 183 or e_type not in (2, 3):  # EM_AARCH64; EXEC/DYN
                return None
            e_phoff = struct.unpack_from("<Q", head, 32)[0]
            e_phentsize, e_phnum = struct.unpack_from("<HH", head, 54)
            f.seek(e_phoff)
            ph = f.read(e_phentsize * e_phnum)
            aligns = []
            dynamic = None
            for i in range(e_phnum):
                p_type, _flags, p_offset, _vaddr, _paddr, p_filesz, _memsz, p_align = struct.unpack_from(
                    "<IIQQQQQQ", ph, i * e_phentsize)
                if p_type == 1:  # PT_LOAD
                    aligns.append(p_align)
                elif p_type == 2:  # PT_DYNAMIC
                    dynamic = (p_offset, p_filesz)
            if not aligns:
                return None
            f.seek(0)
            data = f.read()
    except (OSError, struct.error):
        return None

    needs_jemalloc = False
    if dynamic:
        # DT_NEEDED names live in .dynstr; a byte search of the whole file is
        # enough to spot libjemalloc.so.
        needs_jemalloc = b"libjemalloc.so" in data
    embeds = bool(JEMALLOC_RE.search(data)) and not os.path.basename(path).startswith("libjemalloc")
    lg = LG_PAGE_RE.search(data)
    return min(aligns), needs_jemalloc, embeds, (int(lg.group(1)) if lg else None)


def main():
    root = sys.argv[1]
    counts = {"elf": 0, "align": 0, "jemalloc": 0}
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        if rel == ".":
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            info = elf_info(path)
            if info is None:
                continue
            counts["elf"] += 1
            align, needs, embeds, lg = info
            if align < PAGE:
                counts["align"] += 1
                print(f"ALIGN\tp_align=0x{align:x}\t{path}")
            if needs or embeds or os.path.basename(path).startswith("libjemalloc"):
                counts["jemalloc"] += 1
                how = "links" if needs else ("is" if os.path.basename(path).startswith("libjemalloc") else "embeds")
                page = f"lg_page={lg}" if lg is not None else "lg_page=?"
                print(f"JEMALLOC\t{how}\t{page}\t{path}")
    print(f"# {counts['elf']} aarch64 ELF files; {counts['align']} with PT_LOAD alignment < 16K; "
          f"{counts['jemalloc']} jemalloc users")


if __name__ == "__main__":
    main()
