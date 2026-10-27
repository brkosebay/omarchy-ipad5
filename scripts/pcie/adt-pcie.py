#!/usr/bin/env python3
"""Print the PCIe/NVMe properties of a raw Apple DeviceTree (ADT) blob.

Meant for the *live* ADT dumped on the iPad from m1n1's phram MTD (label
"adt", see docs/pcie/test-plan.md), which carries what iBoot fills in at boot
and the IPSW ADT lacks: nvme-mmu0 sart-region, s3e nvme-scratch-virt-region,
apcie-phy-tunables / apcie-common-tunables, per-bridge tunables.

Usage: python3 -I scripts/pcie/adt-pcie.py adt.bin
Also works on the IPSW blobs in build/adt-dl/*.dtre (placeholders only).
"""
import struct
import sys

WANT = ("/arm-io/apcie", "/arm-io/nvme-mmu0", "/arm-io/dart-apcie0",
        "/arm-io/dart-apcie1")


def parse(buf, off=0):
    nprops, nchild = struct.unpack_from("<II", buf, off)
    off += 8
    props = {}
    for _ in range(nprops):
        name = buf[off:off + 32].split(b"\0", 1)[0].decode("ascii", "replace")
        (size,) = struct.unpack_from("<I", buf, off + 32)
        size &= 0x7fffffff  # bit 31: placeholder/template flag
        off += 36
        props[name] = buf[off:off + size]
        off += (size + 3) & ~3
    children = []
    for _ in range(nchild):
        child, off = parse(buf, off)
        children.append(child)
    return {"props": props, "children": children}, off


def name_of(node):
    return node["props"].get("name", b"").split(b"\0", 1)[0].decode()


def walk(node, path=""):
    p = path + "/" + name_of(node) if path or name_of(node) != "device-tree" else ""
    yield p or "/", node
    for c in node["children"]:
        yield from walk(c, p)


def fmt(name, val):
    if "tunables" in name and len(val) % 24 == 0 and val:
        rows = []
        for i in range(0, len(val), 24):
            o, sz, mask, v = struct.unpack_from("<IIQQ", val, i)
            rows.append(f"      off {o:#07x} size {sz} mask {mask:#018x} value {v:#018x}")
        return f"{len(val) // 24} records\n" + "\n".join(rows)
    if len(val) in (4, 8, 16, 12) and name not in ("compatible", "name", "device_type"):
        n = len(val) // 4
        words = struct.unpack_from(f"<{n}I", val)
        out = " ".join(f"{w:#010x}" for w in words)
        if len(val) == 16:
            a, b = struct.unpack_from("<QQ", val)
            out += f"   (u64 {a:#x}, {b:#x})"
        return out
    if val and all(32 <= c < 127 or c == 0 for c in val):
        return repr(val.rstrip(b"\0").decode())
    return val.hex() if len(val) <= 64 else f"<{len(val)} bytes> {val[:64].hex()}..."


def main():
    buf = open(sys.argv[1], "rb").read()
    root, _ = parse(buf)
    for path, node in walk(root):
        if not any(path == w or path.startswith(w + "/") for w in WANT):
            continue
        print(path)
        for k, v in sorted(node["props"].items()):
            if k in ("name",):
                continue
            if (k.endswith("tunables") or k in (
                    "lane-cfg", "sart-region", "sart-virtual-base",
                    "nvme-scratch-virt-region", "nvme-scratch-region",
                    "maximum-link-speed", "compatible", "pci-l1pm-control",
                    "pci-max-latency", "tags", "virtual-base",
                    "cache-invalidate-workaround", "invalidate-fifo-workaround",
                    "clkreq-wait-time", "t-refclk-to-perst", "nvme-mode",
                    "no-refclk-gating", "no-nand-sysclk-gating",
                    "muxed-auxclk-auto-dis", "manual-enable", "vm-offset",
                    "vm-size", "msi-vector-offset", "msi-address")):
                print(f"    {k}: {fmt(k, v)}")


if __name__ == "__main__":
    main()
