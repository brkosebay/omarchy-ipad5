#!/usr/bin/env python3
"""On-device NVMe install steps, run as root on the iPad under Omarchy.

  nvme-install.py SUBCOMMAND --layout layout.env [options]

  info            facts: NSID -> device, sizes, cache/trim flags, ro flags, window, partitions
  check           every guard below, PASS/FAIL, exit 1 on any FAIL (writes nothing)
  hash-outside    sha256 of everything outside p2: GPT copies, p1, both gaps, NSIDs 2-7
                  (--out FILE to save, --compare FILE to check against an earlier run)
  fence-test      writes that the kernel must refuse, only into the unowned gaps,
                  plus discard/zeroout probes; checks the gaps didn't change
  soak            pattern write/read-verify on p2 (O_DIRECT, parallel, random 4K + fsync,
                  page cache + drop_caches), throughput and fsync latency
  verify          re-check the soak's sequential region only (after a reboot or iOS boot)
  write-image     stdin -> p2 (O_DIRECT), --bytes N --sha256 HEX of the stream
  verify-image    sha256 of p2's first --bytes N against --sha256 HEX

Guards, before anything is written (docs/nvme-install/DESIGN.md):
- root; layout.env from plan-layout.py;
- the namespace with NSID 1 (from sysfs: nvmeXnY numbering is not the NSID)
  has LBA size LBA_SIZE and DISK_LBAS LBAs;
- /sys/module/nvme/parameters/write_window is WRITE_WINDOW;
- NSID 1 has exactly two partitions, p1 = P1_FIRST..P1_LAST and p2 =
  P2_FIRST..P2_LAST (the window) with PARTUUID P2_PARTUUID;
- p2 isn't mounted or used as swap;
- the only device ever opened for writing is that p2, except fence-test,
  which writes only LBAs inside gap A, gap B or straddling p2's edges into
  them (sectors nobody owns after the resize).
"""
import argparse
import hashlib
import mmap
import os
import random
import re
import struct
import subprocess
import sys
import time

MIB = 1 << 20
GIB = 1 << 30
MAGIC = b"LA9SOAK1"
fails = 0


def say(kind, msg):
    global fails
    if kind == "FAIL":
        fails += 1
    print(f"{kind} {msg}", flush=True)


def die(msg):
    print(f"REFUSED {msg}", flush=True)
    sys.exit(2)


def rd(path, default=None):
    try:
        return open(path).read().strip()
    except OSError:
        return default


def load_layout(path):
    env = {}
    for line in open(path):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    for k in ("LBA_SIZE", "DISK_LBAS", "P1_FIRST", "P1_LAST", "P2_FIRST", "P2_LAST", "P2_LBAS",
              "GAPA_FIRST", "GAPA_LAST", "GAPB_FIRST", "GAPB_LAST", "BACKUP_GPT_FIRST", "BACKUP_GPT_LAST"):
        env[k] = int(env[k])
    return env


def namespaces():
    """{nsid: disk name} for every NVMe namespace, from sysfs."""
    out = {}
    for d in sorted(os.listdir("/sys/block")):
        if re.fullmatch(r"nvme\d+n\d+", d):
            nsid = rd(f"/sys/block/{d}/nsid")
            if nsid:
                out[int(nsid)] = d
    return out


def partitions(disk):
    """{partition number: (name, start LBA512, size LBA512)}"""
    out = {}
    for p in os.listdir(f"/sys/block/{disk}"):
        if os.path.exists(f"/sys/block/{disk}/{p}/partition"):
            out[int(rd(f"/sys/block/{disk}/{p}/partition"))] = (
                p, int(rd(f"/sys/block/{disk}/{p}/start")), int(rd(f"/sys/block/{disk}/{p}/size")))
    return out


def partuuid(name):
    """A partition's GPT unique GUID: the kernel's uevent, else blkid"""
    for line in (rd(f"/sys/class/block/{name}/uevent", "") or "").splitlines():
        if line.startswith("PARTUUID="):
            return line.split("=", 1)[1]
    for b in ("/usr/bin/blkid", "/usr/sbin/blkid", "/sbin/blkid", "/bin/blkid"):
        if os.path.exists(b):
            return subprocess.run([b, "-p", "-s", "PART_ENTRY_UUID", "-o", "value", f"/dev/{name}"],
                                  capture_output=True, text=True).stdout.strip()
    return "(unknown: no PARTUUID in uevent, no blkid)"


class Target:
    """The checked state of the disk; check() decides whether writing is allowed."""

    def __init__(self, L):
        self.L = L
        self.ns = namespaces()
        self.disk = self.ns.get(1)
        self.parts = partitions(self.disk) if self.disk else {}
        self.p2 = self.parts.get(2, (None,))[0]

    def check(self, need_window=True, report=True):
        L, ok = self.L, True

        def c(cond, msg):
            nonlocal ok
            if report:
                say("PASS" if cond else "FAIL", msg)
            ok = ok and bool(cond)
            return cond

        c(os.geteuid() == 0, "running as root")
        if not c(self.disk, f"NSID 1 present ({self.disk})"):
            return False
        d = self.disk
        lbs = int(rd(f"/sys/block/{d}/queue/logical_block_size"))
        size512 = int(rd(f"/sys/block/{d}/size"))
        c(lbs == L["LBA_SIZE"], f"{d}: logical block size {lbs} (layout {L['LBA_SIZE']})")
        c(size512 * 512 == L["DISK_LBAS"] * L["LBA_SIZE"], f"{d}: {size512 * 512} bytes (layout {L['DISK_LBAS']} LBAs)")
        ww = rd("/sys/module/nvme/parameters/write_window", "")
        if need_window:
            c(ww == L["WRITE_WINDOW"], f"nvme.write_window is '{ww}' (layout {L['WRITE_WINDOW']})")
            c(rd(f"/sys/block/{d}/ro") == "0", f"{d} is writable (ro=0)")
        for nsid, disk in sorted(self.ns.items()):
            if nsid != 1:
                c(rd(f"/sys/block/{disk}/ro") == "1", f"NSID {nsid} ({disk}) is read-only")
        k = L["LBA_SIZE"] // 512
        c(sorted(self.parts) == [1, 2], f"{d} has exactly partitions 1 and 2 ({sorted(self.parts)})")
        if 1 in self.parts:
            _, s, n = self.parts[1]
            c((s, n) == (L["P1_FIRST"] * k, (L["P1_LAST"] - L["P1_FIRST"] + 1) * k),
              f"p1 is LBAs {s // k}-{(s + n) // k - 1} (layout {L['P1_FIRST']}-{L['P1_LAST']})")
        if 2 in self.parts:
            name, s, n = self.parts[2]
            c((s, n) == (L["P2_FIRST"] * k, L["P2_LBAS"] * k),
              f"p2 ({name}) is LBAs {s // k}-{(s + n) // k - 1} (layout {L['P2_FIRST']}-{L['P2_LAST']})")
            pu = partuuid(name)
            c(pu.lower() == L["P2_PARTUUID"].lower(), f"p2 PARTUUID {pu} (layout {L['P2_PARTUUID']})")
            busy = [l for l in open("/proc/mounts") if l.split()[0] == f"/dev/{name}"]
            busy += [l for l in open("/proc/swaps") if l.split()[0] == f"/dev/{name}"]
            c(not busy, f"p2 is not mounted or swap ({len(busy)} uses)")
        return ok

    def require(self, need_window=True):
        if not self.check(need_window, report=True):
            die("guards failed; nothing was written")


def odirect(path, write):
    return os.open(path, (os.O_RDWR if write else os.O_RDONLY) | os.O_DIRECT)


def sha_range(fd, first, last, lba, buf):
    h = hashlib.sha256()
    off, end = first * lba, (last + 1) * lba
    while off < end:
        n = min(len(buf), end - off)
        got = os.preadv(fd, [memoryview(buf)[:n]], off)
        if got != n:
            die(f"short read at byte {off}")
        h.update(memoryview(buf)[:n])
        off += n
    return h.hexdigest()


# ---- patterns: per 4 KiB block, MAGIC + LBA + tag stamped on a per-tag base
_bases = {}


def base(tag):
    if tag not in _bases:
        _bases[tag] = hashlib.shake_256(b"linux-a9 soak %d" % tag).digest(MIB)
    return _bases[tag]


def fill(buf, lba0, nblk, tag):
    b = base(tag)
    mv = memoryview(buf)
    for i in range(nblk):
        lba = lba0 + i
        o = i * 4096
        s = (lba & 255) * 4096
        mv[o:o + 4096] = b[s:s + 4096]
        struct.pack_into("<8sQI", buf, o, MAGIC, lba, tag)


def expected(lba0, nblk, tag):
    e = bytearray(nblk * 4096)
    fill(e, lba0, nblk, tag)
    return e


def write_range(fd, first, nblk, tag, req, buf):
    """Pattern-write blocks first..first+nblk-1 (partition-relative), req blocks per call"""
    done = 0
    while done < nblk:
        n = min(req, nblk - done)
        fill(buf, first + done, n, tag)
        if os.pwritev(fd, [memoryview(buf)[:n * 4096]], (first + done) * 4096) != n * 4096:
            die("short write")
        done += n


def verify_range(fd, first, nblk, tag, req, buf, what):
    done, bad = 0, []
    while done < nblk:
        n = min(req, nblk - done)
        if os.preadv(fd, [memoryview(buf)[:n * 4096]], (first + done) * 4096) != n * 4096:
            die("short read")
        if memoryview(buf)[:n * 4096] != expected(first + done, n, tag):
            for i in range(n):
                blk = bytes(memoryview(buf)[i * 4096:(i + 1) * 4096])
                if blk != bytes(expected(first + done + i, 1, tag)):
                    m, l, t = struct.unpack_from("<8sQI", blk)
                    bad.append((first + done + i, m == MAGIC and l, m == MAGIC and t))
                    if len(bad) >= 8:
                        break
        if len(bad) >= 8:
            break
        done += n
    if bad:
        say("FAIL", f"{what}: {len(bad)}+ bad blocks, first (lba, stamped lba, stamped tag): {bad[:4]}")
    else:
        say("PASS", f"{what}: {nblk} blocks verified")
    return not bad


def mbps(nbytes, secs):
    return f"{nbytes / MIB / secs:.1f} MiB/s" if secs > 0 else "-"


# ---- subcommands -----------------------------------------------------------

def cmd_info(L, a):
    t = Target(L)
    print(f"namespaces (NSID: device): {t.ns}")
    for nsid, d in sorted(t.ns.items()):
        q = f"/sys/block/{d}/queue"
        print(f"NSID {nsid} {d}: {int(rd(f'/sys/block/{d}/size')) * 512} bytes ro={rd(f'/sys/block/{d}/ro')} "
              f"lbs={rd(q + '/logical_block_size')} write_cache={rd(q + '/write_cache')!r} fua={rd(q + '/fua')} "
              f"discard_max={rd(q + '/discard_max_bytes')} write_zeroes_max={rd(q + '/write_zeroes_max_bytes')} "
              f"max_hw_kb={rd(q + '/max_hw_sectors_kb')}")
    print(f"write_window={rd('/sys/module/nvme/parameters/write_window', '(no such parameter)')!r}")
    print(f"partitions of {t.disk}: {t.parts}")
    log = subprocess.run(["dmesg"], capture_output=True, text=True).stdout
    for l in log.splitlines():
        if re.search(r"write window|read-only|refused|Apple ANS: namespaces|NVMMU fault|translation fault", l):
            print("dmesg: " + l)


def cmd_check(L, a):
    Target(L).check(need_window=not a.no_window)


def regions(L, t):
    lba = L["LBA_SIZE"]
    out = [("gpt-primary", 0, L["P1_FIRST"] - 1), ("p1", L["P1_FIRST"], L["P1_LAST"]),
           ("gap-a", L["GAPA_FIRST"], L["GAPA_LAST"]), ("gap-b", L["GAPB_FIRST"], L["GAPB_LAST"]),
           ("gpt-backup", L["BACKUP_GPT_FIRST"], L["BACKUP_GPT_LAST"])]
    return out


def cmd_hash_outside(L, a):
    t = Target(L)
    if not t.disk:
        die("no NSID 1")
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(f"/dev/{t.disk}", False)
    lines, t0 = [], time.time()
    for name, f, l in regions(L, t):
        lines.append(f"REGION {name} {f} {l} {sha_range(fd, f, l, L['LBA_SIZE'], buf)}")
        print(lines[-1], flush=True)
    os.close(fd)
    for nsid, d in sorted(t.ns.items()):
        if nsid == 1:
            continue
        fd = odirect(f"/dev/{d}", False)
        nlba = int(rd(f"/sys/block/{d}/size")) * 512 // L["LBA_SIZE"]
        lines.append(f"NSID {nsid} 0 {nlba - 1} {sha_range(fd, 0, nlba - 1, L['LBA_SIZE'], buf)}")
        print(lines[-1], flush=True)
        os.close(fd)
    print(f"INFO hashed in {time.time() - t0:.0f} s")
    if a.ns_sums:
        sums = {}
        for line in open(a.ns_sums):
            h, n = line.split()
            sums[h] = n
        for line in lines:
            if line.startswith("NSID"):
                _, nsid, _, _, h = line.split()
                n = sums.get(h)
                # Informational: iOS itself rewrites NVRAM (and may touch the
                # effaceable area) when it runs. "Linux didn't write it" is
                # the --compare between two runs in one Linux session.
                say("PASS" if n == f"nvme0n{nsid}" else "WARN",
                    f"NSID {nsid} matches the Mac's dump {n}" if n else
                    f"NSID {nsid} differs from every dump in {a.ns_sums} (expected for NVRAM after iOS ran)")
    if a.out:
        open(a.out, "w").write("\n".join(lines) + "\n")
    if a.compare:
        old = {tuple(l.split()[:4]): l.split()[4] for l in open(a.compare) if l.split()}
        for line in lines:
            k, h = tuple(line.split()[:4]), line.split()[4]
            if k not in old:
                say("FAIL", f"{' '.join(k)}: not in {a.compare}")
            else:
                say("PASS" if old[k] == h else "FAIL", f"{' '.join(k)} unchanged" if old[k] == h else f"{' '.join(k)} CHANGED")


def cmd_fence_test(L, a):
    t = Target(L)
    t.require()
    lba, d = L["LBA_SIZE"], t.disk
    if L["GAPA_LAST"] - L["GAPA_FIRST"] < 8 or L["GAPB_LAST"] - L["GAPB_FIRST"] < 8:
        die("gaps too small for the fence test")
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(f"/dev/{d}", False)
    before = {n: sha_range(fd, f, l, lba, buf) for n, f, l in regions(L, t) if n.startswith("gap")}
    os.close(fd)
    log0 = len(subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines())
    w = odirect(f"/dev/{d}", True)
    probes = [  # (name, first LBA, blocks): every one lies in the gaps or straddles p2's edges into them
        ("gap A first LBA", L["GAPA_FIRST"], 1),
        ("gap A, last LBA before p2", L["P2_FIRST"] - 1, 1),
        ("straddling p2's start", L["P2_FIRST"] - 1, 2),
        ("straddling p2's end", L["P2_LAST"], 2),
        ("gap B, first LBA after p2", L["P2_LAST"] + 1, 1),
        ("gap B last usable LBA", L["GAPB_LAST"], 1),
        ("1 MiB across p2's end", L["P2_LAST"] - 127, 256),
    ]
    for name, first, n in probes:
        assert L["GAPA_FIRST"] <= first and first + n - 1 <= L["GAPB_LAST"]
        assert not (L["P2_FIRST"] <= first and first + n - 1 <= L["P2_LAST"])
        fill(buf, first, n, 0xFE)
        try:
            os.pwritev(w, [memoryview(buf)[:n * lba]], first * lba)
            os.fsync(w)
            say("FAIL", f"write {name} (LBA {first}+{n}) was NOT refused")
        except OSError as e:
            say("PASS" if e.errno == 5 else "FAIL", f"write {name} (LBA {first}+{n}): {os.strerror(e.errno)}")
    os.close(w)
    fd = odirect(f"/dev/{d}", False)
    for n, f, l in regions(L, t):
        if n in before:
            h = sha_range(fd, f, l, lba, buf)
            say("PASS" if h == before[n] else "FAIL", f"{n} unchanged after the refused writes")
    os.close(fd)
    log = subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines()[log0:]
    refused = [l for l in log if "refused write nsid 1" in l]
    say("PASS" if refused else "FAIL", f"kernel logged {len(refused)} refused writes")
    for l in refused[:8]:
        print("    " + l)
    q = f"/sys/block/{d}/queue"
    say("PASS" if rd(q + "/discard_max_bytes") == "0" else "FAIL", f"trim not advertised ({rd(q + '/discard_max_bytes')})")
    say("PASS" if rd(q + "/write_zeroes_max_bytes") == "0" else "FAIL",
        f"write zeroes not advertised ({rd(q + '/write_zeroes_max_bytes')})")
    p2 = os.open(f"/dev/{t.p2}", os.O_RDWR)
    import fcntl
    try:
        fcntl.ioctl(p2, 0x1277, struct.pack("QQ", 0, 16 * lba))  # BLKDISCARD
        say("FAIL", "BLKDISCARD on p2 succeeded")
    except OSError as e:
        say("PASS" if e.errno == 95 else "FAIL", f"BLKDISCARD on p2: {os.strerror(e.errno)}")
    os.close(p2)
    for nsid, disk in sorted(t.ns.items()):
        if nsid == 1:
            continue
        try:
            x = os.open(f"/dev/{disk}", os.O_RDWR | os.O_DIRECT)
            try:
                os.pwritev(x, [memoryview(buf)[:lba]], 0)
                say("FAIL", f"NSID {nsid} accepted a write")
            except OSError as e:
                say("PASS" if e.errno in (1, 13, 30) else "FAIL", f"NSID {nsid} ({disk}) write: {os.strerror(e.errno)}")
            os.close(x)
        except OSError as e:
            say("PASS", f"NSID {nsid} ({disk}) open for write: {os.strerror(e.errno)}")


def soak_layout(L, size_gib):
    nblk = L["P2_LBAS"]
    seq = min(size_gib * GIB // 4096, nblk // 2)
    par = min(GIB // 4096, (nblk - seq) // 4)
    rnd0, rnd = seq + par, min(256 * MIB // 4096, (nblk - seq - par) // 3)
    buf0, bufn = rnd0 + rnd, min(256 * MIB // 4096, (nblk - seq - par - rnd) // 2)
    return seq, (seq, par), (rnd0, rnd), (buf0, bufn)


def cmd_soak(L, a):
    t = Target(L)
    t.require()
    dev = f"/dev/{t.p2}"
    seq, (par0, par), (rnd0, rnd), (buf0, bufn) = soak_layout(L, a.size_gib)
    tag = a.tag
    print(f"INFO soak on {dev}: sequential {seq * 4 // 1024} MiB, parallel {par * 4 // 1024} MiB, "
          f"random 4K in {rnd * 4 // 1024} MiB, page cache {bufn * 4 // 1024} MiB, tag {tag}", flush=True)
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(dev, True)
    # Twice over the same blocks with different tags, so stale data from the
    # first pass can't pass the second verify. The region keeps `tag`.
    for req, tg in ((256, tag + 10), (32, tag)):
        t0 = time.time()
        write_range(fd, 0, seq, tg, req, buf)
        os.fsync(fd)
        tw = time.time() - t0
        t0 = time.time()
        ok = verify_range(fd, 0, seq, tg, 256, buf, f"sequential {req * 4} KiB writes (tag {tg})")
        tr = time.time() - t0
        print(f"INFO {req * 4} KiB O_DIRECT writes: {mbps(seq * 4096, tw)} incl. fsync; verify read {mbps(seq * 4096, tr)}",
              flush=True)
        if not ok:
            die("verify failed: stop, report the FAIL line and dmesg")
    # 4 parallel writers, 128 KiB requests: many FlatDMA slots in flight
    t0 = time.time()
    pids = []
    q = par // 4
    for i in range(4):
        pid = os.fork()
        if pid == 0:
            b = mmap.mmap(-1, 4 * MIB)
            f = odirect(dev, True)
            write_range(f, par0 + i * q, q, tag + 1, 32, b)
            os.fsync(f)
            os._exit(0)
        pids.append(pid)
    rcs = [os.waitpid(p, 0)[1] for p in pids]
    print(f"INFO 4 parallel writers: {mbps(4 * q * 4096, time.time() - t0)} aggregate", flush=True)
    if any(rcs):
        die("a parallel writer failed")
    verify_range(fd, par0, 4 * q, tag + 1, 256, buf, "parallel writers")
    # random 4 KiB writes in a fresh region, fsync every 64
    write_range(fd, rnd0, rnd, tag + 2, 256, buf)
    rng = random.Random(tag)
    picks = sorted(set(rng.randrange(rnd) for _ in range(a.random)))
    lat, t0 = [], time.time()
    for i, k in enumerate(picks):
        fill(buf, rnd0 + k, 1, tag + 3)
        os.pwritev(fd, [memoryview(buf)[:4096]], (rnd0 + k) * 4096)
        if i % 64 == 63:
            s = time.time()
            os.fsync(fd)
            lat.append(time.time() - s)
    os.fsync(fd)
    el = time.time() - t0
    lat.sort()
    print(f"INFO {len(picks)} random 4 KiB writes: {len(picks) / el:.0f} IOPS incl. fsyncs; fsync after 64 writes: "
          f"median {lat[len(lat) // 2] * 1000:.1f} ms, max {lat[-1] * 1000:.1f} ms", flush=True)
    bad = 0
    pick = set(picks)
    chunk = 256
    for s in range(0, rnd, chunk):
        n = min(chunk, rnd - s)
        os.preadv(fd, [memoryview(buf)[:n * 4096]], (rnd0 + s) * 4096)
        for i in range(n):
            want = expected(rnd0 + s + i, 1, tag + 3 if (s + i) in pick else tag + 2)
            if memoryview(buf)[i * 4096:(i + 1) * 4096] != want:
                bad += 1
    say("FAIL" if bad else "PASS", f"random 4 KiB writes: {bad} bad blocks in {rnd} (rewritten {len(picks)})")
    # fsync latency on an idle device
    lat = []
    for i in range(50):
        fill(buf, rnd0 + i, 1, tag + 3)
        os.pwritev(fd, [memoryview(buf)[:4096]], (rnd0 + i) * 4096)
        s = time.time()
        os.fsync(fd)
        lat.append(time.time() - s)
    lat.sort()
    print(f"INFO fsync after one 4 KiB write: median {lat[25] * 1000:.2f} ms, max {lat[-1] * 1000:.2f} ms "
          f"(near 0 on a write-back cache would mean Flush isn't waiting)", flush=True)
    os.close(fd)
    # page-cache path: buffered writes, fsync, drop caches, O_DIRECT read
    t0 = time.time()
    f = os.open(dev, os.O_RDWR)
    write_range(f, buf0, bufn, tag + 4, 256, buf)
    os.fsync(f)
    os.close(f)
    print(f"INFO buffered writes + fsync: {mbps(bufn * 4096, time.time() - t0)}", flush=True)
    open("/proc/sys/vm/drop_caches", "w").write("3\n")
    fd = odirect(dev, False)
    verify_range(fd, buf0, bufn, tag + 4, 256, buf, "page-cache writes after drop_caches")
    verify_range(fd, 0, seq, tag, 256, buf, "sequential region still intact")
    os.close(fd)
    print(f"INFO verify later with: verify --size-gib {a.size_gib} --tag {tag}")


def cmd_verify(L, a):
    t = Target(L)
    t.check(need_window=False)
    seq = soak_layout(L, a.size_gib)[0]
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(f"/dev/{t.p2}", False)
    verify_range(fd, 0, seq, a.tag, 256, buf, f"sequential region (tag {a.tag})")


def cmd_write_image(L, a):
    t = Target(L)
    t.require()
    if a.bytes > L["P2_LBAS"] * 4096 or a.bytes % 4096:
        die(f"--bytes {a.bytes}: must be a multiple of 4096 and at most p2's {L['P2_LBAS'] * 4096}")
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(f"/dev/{t.p2}", True)
    h, off, t0, last = hashlib.sha256(), 0, time.time(), 0
    src = sys.stdin.buffer
    while True:
        n = 0
        while n < len(buf):
            got = src.readinto(memoryview(buf)[n:])
            if not got:
                break
            n += got
        if not n:
            break
        if off + n > a.bytes or n % 4096:
            die(f"stream longer than --bytes or not 4 KiB aligned (at byte {off + n}); {off} bytes written")
        os.pwritev(fd, [memoryview(buf)[:n]], off)
        h.update(memoryview(buf)[:n])
        off += n
        if off - last >= GIB:
            last = off
            print(f"INFO {off // MIB} MiB written, {mbps(off, time.time() - t0)}", file=sys.stderr, flush=True)
    os.fsync(fd)
    os.close(fd)
    print(f"INFO {off} bytes in {time.time() - t0:.0f} s ({mbps(off, time.time() - t0)}), stream sha256 {h.hexdigest()}")
    say("PASS" if off == a.bytes else "FAIL", f"stream length {off} (expected {a.bytes})")
    say("PASS" if h.hexdigest() == a.sha256 else "FAIL", "stream sha256 matches the Mac's image")


def cmd_verify_image(L, a):
    t = Target(L)
    t.check(need_window=False)
    open("/proc/sys/vm/drop_caches", "w").write("3\n")
    buf = mmap.mmap(-1, 4 * MIB)
    fd = odirect(f"/dev/{t.p2}", False)
    t0 = time.time()
    h = sha_range(fd, 0, a.bytes // 4096 - 1, 4096, buf)
    print(f"INFO read {a.bytes} bytes in {time.time() - t0:.0f} s, sha256 {h}")
    say("PASS" if h == a.sha256 else "FAIL", "p2 holds exactly the Mac's image")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "check", "hash-outside", "fence-test", "soak", "verify",
                                    "write-image", "verify-image"])
    ap.add_argument("--layout", required=True)
    ap.add_argument("--no-window", action="store_true", help="check: the read-only kernel (no write window)")
    ap.add_argument("--out")
    ap.add_argument("--compare")
    ap.add_argument("--ns-sums")
    ap.add_argument("--size-gib", type=int, default=4)
    ap.add_argument("--tag", type=int, default=1)
    ap.add_argument("--random", type=int, default=20000)
    ap.add_argument("--bytes", type=int)
    ap.add_argument("--sha256")
    a = ap.parse_args()
    os.environ.setdefault("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
    L = load_layout(a.layout)
    {"info": cmd_info, "check": cmd_check, "hash-outside": cmd_hash_outside, "fence-test": cmd_fence_test,
     "soak": cmd_soak, "verify": cmd_verify, "write-image": cmd_write_image,
     "verify-image": cmd_verify_image}[a.cmd](L, a)
    if fails:
        print(f"RESULT {fails} FAIL")
        sys.exit(1)
    print("RESULT all PASS")


if __name__ == "__main__":
    main()
