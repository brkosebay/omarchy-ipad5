#!/usr/bin/env python3
"""Plan the NVMe layout: APFS (iOS) shrunk, Linux partition after it.

  plan-layout.py --backup DIR --apfs FILE [--apfs-gib N] [--headroom-gib 3]
                 [--min-apfs-gib 12] [--min-linux-gib 12] [--out DIR]

Inputs (from scripts/nvme-install/backup.sh, all read from the device
read-only before anything is written):
  DIR/geometry.env           DISK_BYTES, LBA_SIZE (sysfs)
  DIR/nvme0n1-first-1MiB.bin LBAs 0.. (PMBR, primary GPT)
  DIR/nvme0n1-last-1MiB.bin  the last 1 MiB (backup GPT)
  FILE                       apfs-nx.py output for nvme0n1p1 (container now)

Outputs in OUT (default build/nvme-install):
  layout.env    every number the other steps use (shell syntax)
  gpt-plan/     old-primary.bin old-backup.bin new-primary.bin new-backup.bin
                plan.cfg (for gptcas) plan.txt (human-readable)

The new GPT is the dumped one with exactly two changes: partition 1's last
LBA (the container's new end) and a new partition 2 (Linux filesystem) in
the space behind it. Disk GUID, p1's type, unique GUID, attributes, name and
start, header fields and padding all stay byte for byte; both CRCs of both
copies are recomputed. It refuses anything that doesn't look like iOS's
one-partition GPT. Re-running with an existing layout.env reuses its GUIDs
and must reproduce the same plan bytes.
"""
import argparse
import hashlib
import os
import struct
import sys
import uuid
import zlib

GIB = 1 << 30
MIB = 1 << 20
APFS_TYPE = uuid.UUID("7C3457EF-0000-11AA-AA11-00306543ECAC")
LINUX_FS_TYPE = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
ENTRY = struct.Struct("<16s16sQQQ72s")
HDR = struct.Struct("<8sIIIIQQQQ16sQIII")


def die(msg):
    sys.exit(f"plan-layout: {msg}")


def read_env(path):
    env = {}
    for line in open(path):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k] = v.strip("'\"")
    return env


class Header:
    def __init__(self, raw):
        (self.sig, self.rev, self.size, self.crc, self.res, self.my_lba,
         self.alt_lba, self.first_usable, self.last_usable, self.disk_guid,
         self.entries_lba, self.num_entries, self.entry_size,
         self.entries_crc) = HDR.unpack_from(raw, 0)
        self.raw = bytearray(raw)

    def calc_crc(self):
        h = bytearray(self.raw[:self.size])
        h[16:20] = b"\0\0\0\0"
        return zlib.crc32(h) & 0xFFFFFFFF

    def with_entries_crc(self, crc):
        raw = bytearray(self.raw)
        struct.pack_into("<I", raw, 88, crc)
        struct.pack_into("<I", raw, 16, 0)
        struct.pack_into("<I", raw, 16, zlib.crc32(raw[:self.size]) & 0xFFFFFFFF)
        return raw


def entries(raw, n, size):
    return [ENTRY.unpack_from(raw, i * size) for i in range(n)]


def check_header(h, lba_size, my_lba, alt_lba, what):
    if h.sig != b"EFI PART" or h.rev != 0x00010000 or h.size != 92:
        die(f"{what}: not a GPT header (sig {h.sig!r}, rev {h.rev:#x}, size {h.size})")
    if h.crc != h.calc_crc():
        die(f"{what}: header CRC mismatch")
    if (h.my_lba, h.alt_lba) != (my_lba, alt_lba):
        die(f"{what}: my/alternate LBA {h.my_lba}/{h.alt_lba}, expected {my_lba}/{alt_lba}")
    if h.entry_size != 128 or h.num_entries * h.entry_size % lba_size:
        die(f"{what}: unexpected entry array ({h.num_entries} x {h.entry_size})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backup", required=True)
    ap.add_argument("--apfs", required=True, help="apfs-nx.py output (may lack the used size)")
    ap.add_argument("--apfs-used-bytes", type=int,
                    help="iOS's used space, e.g. from df on iOS, when apfs-nx.py couldn't read it")
    ap.add_argument("--apfs-gib", type=int)
    ap.add_argument("--headroom-gib", type=int, default=3)
    ap.add_argument("--min-apfs-gib", type=int, default=12)
    ap.add_argument("--min-linux-gib", type=int, default=12)
    ap.add_argument("--p2-name", default="omarchy-root")
    ap.add_argument("--fs-label", default="omarchy-nvme")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "../../build/nvme-install"))
    a = ap.parse_args()

    geo = read_env(os.path.join(a.backup, "geometry.env"))
    lba = int(geo["LBA_SIZE"])
    disk_bytes = int(geo["DISK_BYTES"])
    if lba != 4096 or disk_bytes % lba:
        die(f"expected 4096-byte LBAs, got LBA_SIZE={lba} DISK_BYTES={disk_bytes}")
    n = disk_bytes // lba
    first = open(os.path.join(a.backup, "nvme0n1-first-1MiB.bin"), "rb").read()
    last = open(os.path.join(a.backup, "nvme0n1-last-1MiB.bin"), "rb").read()
    if len(first) != MIB or len(last) != MIB:
        die("the dumps must be exactly 1 MiB each")
    tail0 = n - MIB // lba  # LBA of the first byte of the last-MiB dump
    blk = lambda buf, base, l, count=1: buf[(l - base) * lba:(l - base + count) * lba]

    # Protective MBR: one 0xEE partition
    if first[510:512] != b"\x55\xaa" or first[446 + 4] != 0xEE:
        die("LBA 0 is not a protective MBR")
    ph = Header(blk(first, 0, 1))
    check_header(ph, lba, 1, n - 1, "primary")
    elbas = ph.num_entries * ph.entry_size // lba
    if ph.entries_lba != 2 or ph.first_usable < 2 + elbas:
        die(f"primary entries at LBA {ph.entries_lba}, first usable {ph.first_usable}")
    pent_raw = blk(first, 0, 2, elbas)
    if zlib.crc32(pent_raw) & 0xFFFFFFFF != ph.entries_crc:
        die("primary entry array CRC mismatch")
    bh = Header(blk(last, tail0, n - 1))
    check_header(bh, lba, n - 1, 1, "backup")
    if bh.entries_lba != n - 1 - elbas:
        die(f"backup entries at LBA {bh.entries_lba}, expected {n - 1 - elbas}")
    bent_raw = blk(last, tail0, bh.entries_lba, elbas)
    if bent_raw != pent_raw:
        die("primary and backup entry arrays differ")
    for f in ("disk_guid", "first_usable", "last_usable", "num_entries", "entry_size", "entries_crc"):
        if getattr(ph, f) != getattr(bh, f):
            die(f"primary and backup headers differ in {f}")

    ents = entries(pent_raw, ph.num_entries, ph.entry_size)
    used = [i for i, e in enumerate(ents) if e[0] != bytes(16)]
    layout_path = os.path.join(a.out, "layout.env")
    old = read_env(layout_path) if os.path.exists(layout_path) else {}
    if used == [0, 1] and old:
        e1 = ents[1]
        if (uuid.UUID(bytes_le=e1[1]) == uuid.UUID(old["P2_PARTUUID"]) and
                (e1[2], e1[3]) == (int(old["P2_FIRST"]), int(old["P2_LAST"]))):
            print(f"plan-layout: the GPT already has the planned p2 ({old['P2_FIRST']}-{old['P2_LAST']}); "
                  f"layout.env unchanged")
            return
    if used != [0]:
        die(f"expected iOS's single partition, found entries {used}")
    p1 = ents[0]
    if uuid.UUID(bytes_le=p1[0]) != APFS_TYPE:
        die(f"partition 1 type {uuid.UUID(bytes_le=p1[0])} is not APFS")
    p1_first, p1_last_old = p1[2], p1[3]
    if p1_first != ph.first_usable or p1_last_old > ph.last_usable:
        die(f"partition 1 spans {p1_first}-{p1_last_old}, usable {ph.first_usable}-{ph.last_usable}")

    apfs = read_env(a.apfs) if os.path.exists(a.apfs) else {}
    if apfs.get("APFS_BLOCK_SIZE") and int(apfs["APFS_BLOCK_SIZE"]) != lba:
        die("APFS block size differs from the LBA size")
    if apfs.get("APFS_BLOCK_COUNT"):
        cont_blocks = int(apfs["APFS_BLOCK_COUNT"])
    else:
        # Before the resize the container fills partition 1 (gptcas re-checks on iOS)
        cont_blocks = p1_last_old - p1_first + 1
        print(f"plan-layout: no container size in {a.apfs}; assuming it fills partition 1 ({cont_blocks} blocks)")
    if cont_blocks > p1_last_old - p1_first + 1:
        die(f"container ({cont_blocks} blocks) is bigger than partition 1")
    if a.apfs_used_bytes:
        used_bytes = a.apfs_used_bytes
    elif apfs.get("APFS_USED_BYTES"):
        used_bytes = int(apfs["APFS_USED_BYTES"])
    else:
        die("APFS used space unknown: pass --apfs-used-bytes (iOS: df -k / /private/var ..., sum the used column x 1024)")
    if a.apfs_gib:
        target = a.apfs_gib * GIB
    else:
        target = max(a.min_apfs_gib * GIB, -(-(used_bytes + a.headroom_gib * GIB) // GIB) * GIB)
    if target < used_bytes + GIB:
        die(f"{target / GIB:.1f} GiB leaves APFS less than 1 GiB free ({used_bytes / GIB:.2f} GiB used)")
    if target > cont_blocks * lba:
        die(f"{target / GIB:.1f} GiB is not smaller than the container ({cont_blocks * lba / GIB:.2f} GiB)")

    align = MIB // lba
    p1_last = p1_first + target // lba - 1
    p2_first = -(-(p1_last + 1 + align) // align) * align
    p2_last = (ph.last_usable + 1 - align) // align * align - 1
    if p2_last - p2_first + 1 < a.min_linux_gib * GIB // lba:
        die(f"Linux partition would be {(p2_last - p2_first + 1) * lba / GIB:.1f} GiB (< {a.min_linux_gib})")

    p2_guid = uuid.UUID(old["P2_PARTUUID"]) if old.get("P2_PARTUUID") else uuid.uuid4()
    fs_uuid = uuid.UUID(old["FS_UUID"]) if old.get("FS_UUID") else uuid.uuid4()
    name = a.p2_name.encode("utf-16-le").ljust(72, b"\0")
    new_raw = bytearray(pent_raw)
    ENTRY.pack_into(new_raw, 0, p1[0], p1[1], p1_first, p1_last, p1[4], p1[5])
    ENTRY.pack_into(new_raw, ph.entry_size, LINUX_FS_TYPE.bytes_le, p2_guid.bytes_le, p2_first, p2_last, 0, name)
    new_crc = zlib.crc32(new_raw) & 0xFFFFFFFF
    old_primary = bytes(blk(first, 0, 1, 1 + elbas))
    old_backup = bytes(blk(last, tail0, bh.entries_lba, elbas + 1))
    new_primary = bytes(ph.with_entries_crc(new_crc)) + bytes(new_raw)
    new_backup = bytes(new_raw) + bytes(bh.with_entries_crc(new_crc))

    # Independent re-parse of what will be written
    for region, hdr_off, ent_off, my, alt in ((new_primary, 0, lba, 1, n - 1),
                                              (new_backup, elbas * lba, 0, n - 1, 1)):
        h = Header(region[hdr_off:hdr_off + lba])
        check_header(h, lba, my, alt, "planned")
        e = region[ent_off:ent_off + elbas * lba]
        assert zlib.crc32(e) & 0xFFFFFFFF == h.entries_crc
        es = entries(e, h.num_entries, h.entry_size)
        assert es[0][:2] == p1[:2] and es[0][4:] == p1[4:] and es[0][2:4] == (p1_first, p1_last)
        assert es[1][2:4] == (p2_first, p2_last) and all(x[0] == bytes(16) for x in es[2:])
        assert h.first_usable <= p1_first <= p1_last < p2_first <= p2_last <= h.last_usable
    diff = [i for i in range(len(old_primary)) if old_primary[i] != new_primary[i]]
    assert all(i in range(16, 20) or i in range(88, 92) or lba <= i < lba + 2 * ph.entry_size for i in diff)

    plan_dir = os.path.join(a.out, "gpt-plan")
    os.makedirs(plan_dir, exist_ok=True)
    files = {"old-primary.bin": old_primary, "old-backup.bin": old_backup,
             "new-primary.bin": new_primary, "new-backup.bin": new_backup}
    for fn, data in files.items():
        path = os.path.join(plan_dir, fn)
        if old and os.path.exists(path) and open(path, "rb").read() != data:
            die(f"{path} exists with different content: delete gpt-plan/ and layout.env to replan")
        open(path, "wb").write(data)
    sha = lambda b: hashlib.sha256(b).hexdigest()
    cfg = {
        "lba_size": lba, "disk_lbas": n,
        "primary_lba": 1, "primary_count": 1 + elbas,
        "backup_lba": bh.entries_lba, "backup_count": elbas + 1,
        "p1_first": p1_first, "p1_last_old": p1_last_old, "p1_last_new": p1_last,
    }
    with open(os.path.join(plan_dir, "plan.cfg"), "w") as f:
        for k, v in cfg.items():
            f.write(f"{k} {v}\n")
    lay = {
        "LBA_SIZE": lba, "DISK_LBAS": n, "DISK_BYTES": disk_bytes,
        "P1_FIRST": p1_first, "P1_LAST_OLD": p1_last_old, "P1_LAST": p1_last,
        "APFS_BYTES": target, "APFS_USED_BYTES_AT_PLAN": used_bytes,
        "GAPA_FIRST": p1_last + 1, "GAPA_LAST": p2_first - 1,
        "P2_FIRST": p2_first, "P2_LAST": p2_last, "P2_LBAS": p2_last - p2_first + 1,
        "P2_BYTES": (p2_last - p2_first + 1) * lba,
        "P2_NAME": a.p2_name, "P2_PARTUUID": str(p2_guid),
        "GAPB_FIRST": p2_last + 1, "GAPB_LAST": ph.last_usable,
        "BACKUP_GPT_FIRST": bh.entries_lba, "BACKUP_GPT_LAST": n - 1,
        "WRITE_WINDOW": f"1:{p2_first}-{p2_last}",
        "FS_LABEL": a.fs_label, "FS_UUID": str(fs_uuid),
        "OLD_GPT_SHA256": sha(old_primary + old_backup), "NEW_GPT_SHA256": sha(new_primary + new_backup),
        "DISK_GUID": str(uuid.UUID(bytes_le=ph.disk_guid)), "P1_PARTUUID": str(uuid.UUID(bytes_le=p1[1])),
    }
    with open(layout_path + ".tmp", "w") as f:
        f.write("# generated by scripts/nvme-install/plan-layout.py; LBAs are 4096 bytes, ranges inclusive\n")
        for k, v in lay.items():
            f.write(f"{k}={v}\n")
    os.replace(layout_path + ".tmp", layout_path)
    rows = [("PMBR + primary GPT", 0, 1 + elbas), ("p1 APFS (iOS)", p1_first, p1_last),
            ("gap A (unowned)", p1_last + 1, p2_first - 1), (f"p2 {a.p2_name}", p2_first, p2_last),
            ("gap B (unowned)", p2_last + 1, ph.last_usable), ("backup GPT", bh.entries_lba, n - 1)]
    text = [f"disk {n} LBAs x {lba} = {disk_bytes} bytes; APFS {used_bytes / GIB:.2f} GiB used now, "
            f"container {cont_blocks * lba / GIB:.2f} -> {target / GIB:.2f} GiB",
            f"{'region':22} {'first':>9} {'last':>9}  size"]
    for r, f0, l0 in rows:
        text.append(f"{r:22} {f0:9} {l0:9}  {(l0 - f0 + 1) * lba / MIB:10.1f} MiB")
    text += [f"resize_apfs disk0s1 {target}", f"nvme.write_window=1:{p2_first}-{p2_last}",
             f"p1 last LBA {p1_last_old} -> {p1_last}; p2 PARTUUID {p2_guid}"]
    open(os.path.join(plan_dir, "plan.txt"), "w").write("\n".join(text) + "\n")
    print("\n".join(text))
    print(f"wrote {layout_path} and {plan_dir}/")


if __name__ == "__main__":
    main()
