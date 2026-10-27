#!/usr/bin/env python3
"""Check one boot of the QEMU write-window test.

  check-fence.py CASE LOG NS1_BEFORE NS1_AFTER NS2_BEFORE NS2_AFTER

CASE is window | none | bad | ns2. Checks the RESULT/SYSFS/DMESG lines
fence-init.sh printed, and compares the namespace images block by block:
exactly the blocks that boot's allowed writes cover may have changed, with
the expected content. Prints PASS/FAIL lines; exits 1 on any FAIL.
"""
import re
import sys

BLK = 4096
case, log, n1b, n1a, n2b, n2a = sys.argv[1:7]
text = open(log, errors="replace").read()
fails = 0


def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        fails += 1


results = dict(re.findall(r"^RESULT (\S+) (ok|err [A-Z]+)", text, re.M))
sysfs = {m[0]: dict(kv.split("=", 1) for kv in m[1].split())
         for m in re.findall(r"^SYSFS (\S+) (.*)$", text, re.M)}
check("FENCE-TEST done" in text, "test ran to the end")

OUT = "err EIO"  # refused by the allowlist: the request fails at the device
# Read-only gendisk: in this kernel open(O_RDWR) succeeds and the write,
# BLKDISCARD and BLKZEROOUT fail with EPERM (block/fops.c, block/ioctl.c),
# not EACCES at open as docs/pcie/read-only.md says.
RO = "err EPERM"
expect = {
    # tag: (window, none, bad, ns2)
    "in_first": ("ok", RO, RO, RO),
    "in_last": ("ok", RO, RO, RO),
    "in_big": ("ok", RO, RO, RO),
    "below": (OUT, RO, RO, RO),
    "above": (OUT, RO, RO, RO),
    "straddle_lo": (OUT, RO, RO, RO),
    "straddle_hi": (OUT, RO, RO, RO),
    "lba0": (OUT, RO, RO, RO),
    "end": (OUT, RO, RO, RO),
    "ns2_openrw": ("ok", "ok", "ok", "ok"),
    "ns2_write": (RO, RO, RO, "ok"),
    # trim isn't advertised with a window (checked before read-only)
    "discard_in": ("err EOPNOTSUPP", RO, RO, "err EOPNOTSUPP"),
    "zeroout_in": ("ok", RO, RO, RO),
    "zeroout_out": (OUT, RO, RO, RO),
}
col = ["window", "none", "bad", "ns2"].index(case)
for t, exp in expect.items():
    check(results.get(t) == exp[col], f"{t}: {results.get(t)} (expected {exp[col]})")

ro1 = "0" if case == "window" else "1"
ro2 = "0" if case == "ns2" else "1"
check(sysfs.get("ns1", {}).get("ro") == ro1, f"NSID 1 ro={sysfs.get('ns1', {}).get('ro')} (expected {ro1})")
check(sysfs.get("ns2", {}).get("ro") == ro2, f"NSID 2 ro={sysfs.get('ns2', {}).get('ro')} (expected {ro2})")
print(f"INFO NSID 1 = {sysfs.get('ns1', {}).get('dev')}, NSID 2 = {sysfs.get('ns2', {}).get('dev')} this boot")
if case in ("window", "ns2"):
    for d in ("ns1", "ns2"):
        s = sysfs.get(d, {})
        check(s.get("discard") == "0" and s.get("wz") == "0",
              f"{d}: no trim/write zeroes advertised (discard={s.get('discard')} wz={s.get('wz')})")
dm = "\n".join(re.findall(r"^DMESG (.*)$", text, re.M))
if case == "window":
    check("read-only except NVMe Write to NSID 1 LBAs 4096-12287" in dm, "probe logs the window")
    check("refused write nsid 1 LBA 4095+1 outside the write window" in dm, "refusal logged with its LBA range")
    check("write window: controller ONCS" in dm, "ONCS/VWC logged")
elif case == "ns2":
    check("read-only except NVMe Write to NSID 2 LBAs 0-1023" in dm, "probe logs the window")
else:
    check("media writes and destructive admin commands are refused" in dm, "fully read-only")
if case == "bad":
    check("invalid for parameter" in text, "bad parameter rejected")


def blocks(path):
    data = open(path, "rb").read()
    return [data[i:i + BLK] for i in range(0, len(data), BLK)]


def compare(before, after, allowed, name):
    b, a = blocks(before), blocks(after)
    check(len(b) == len(a), f"{name}: same size")
    changed = {i for i in range(len(b)) if b[i] != a[i]}
    stray = sorted(changed - set(allowed))
    check(not stray, f"{name}: {len(changed)} blocks changed, none outside the allowed writes"
          + (f" (stray: {stray[:8]})" if stray else ""))
    for i, content in allowed.items():
        if a[i] != content:
            check(False, f"{name}: block {i} lacks its expected content")
            return
    check(True, f"{name}: all {len(allowed)} written blocks hold the expected data")


fill = lambda byte: bytes([byte]) * BLK
ns1_allowed, ns2_allowed = {}, {}
if case == "window":
    ns1_allowed = {4096: fill(0xa1), 12287: fill(0xa2)}
    ns1_allowed.update({i: fill(0xa3) for i in range(6000, 7024)})
    ns1_allowed.update({i: fill(0) for i in range(8192, 8208)})
elif case == "ns2":
    ns2_allowed = {0: fill(0xc1)}
compare(n1b, n1a, ns1_allowed, "ns1")
compare(n2b, n2a, ns2_allowed, "ns2")
sys.exit(1 if fails else 0)
