#!/usr/bin/env python3
"""Read-only: an APFS container's size and free space, from its own metadata.

  apfs-nx.py DEVICE_OR_IMAGE [--offset BYTES]

Reads the container superblock at block 0, scans the checkpoint descriptor
area for the newest valid container superblock (highest xid, good Fletcher-64
checksum), looks its space manager up in that checkpoint's map and prints
shell variables:

  APFS_BLOCK_SIZE APFS_BLOCK_COUNT APFS_BYTES APFS_XID APFS_FREE_BLOCKS
  APFS_USED_BYTES APFS_BLOCK0_XID

Container-level objects are never encrypted, so this works on iOS's
container from Linux (docs/nvme-install/DESIGN.md). It only ever opens the
target read-only. Used to size the resize and, after iOS's
APFSContainerResize, to confirm from Linux that the container really shrank
before the GPT edit relies on it.

Layout from Apple's "Apple File System Reference" (nx_superblock_t,
checkpoint_map_phys_t, spaceman_phys_t).
"""
import os
import struct
import sys

NX_MAGIC = 0x4253584E  # 'NXSB'
OBJ_NX_SUPERBLOCK = 0x1
OBJ_SPACEMAN = 0x5
OBJ_CHECKPOINT_MAP = 0xC


def fletcher64(block):
    n = (len(block) - 8) // 4
    s1 = s2 = 0
    for w in struct.unpack_from("<%dI" % n, block, 8):
        s1 = (s1 + w) % 0xFFFFFFFF
        s2 = (s2 + s1) % 0xFFFFFFFF
    c1 = 0xFFFFFFFF - ((s1 + s2) % 0xFFFFFFFF)
    c2 = 0xFFFFFFFF - ((s1 + c1) % 0xFFFFFFFF)
    return (c2 << 32) | c1


def valid(block):
    return struct.unpack_from("<Q", block, 0)[0] == fletcher64(block)


def obj_type(block):
    return struct.unpack_from("<I", block, 24)[0] & 0xFFFF


def xid(block):
    return struct.unpack_from("<Q", block, 16)[0]


def main():
    args = sys.argv[1:]
    offset = 0
    if "--offset" in args:
        i = args.index("--offset")
        offset = int(args[i + 1], 0)
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    fd = os.open(args[0], os.O_RDONLY)

    def read(block_no, size, count=1):
        data = os.pread(fd, size * count, offset + block_no * size)
        if len(data) != size * count:
            sys.exit(f"short read at block {block_no}")
        return data

    b0 = read(0, 4096)
    if struct.unpack_from("<I", b0, 32)[0] != NX_MAGIC:
        sys.exit("no APFS container superblock (NXSB) at block 0")
    bs = struct.unpack_from("<I", b0, 36)[0]
    if bs != 4096:
        b0 = read(0, bs)
    desc_blocks, = struct.unpack_from("<I", b0, 0x68)
    desc_base, = struct.unpack_from("<Q", b0, 0x70)
    if desc_blocks & 0x80000000:
        sys.exit("checkpoint descriptor area is a B-tree; not supported")
    desc = read(desc_base, bs, desc_blocks)

    best, maps = None, {}
    for i in range(desc_blocks):
        blk = desc[i * bs:(i + 1) * bs]
        if not valid(blk):
            continue
        t = obj_type(blk)
        if t == OBJ_NX_SUPERBLOCK and struct.unpack_from("<I", blk, 32)[0] == NX_MAGIC:
            if best is None or xid(blk) > xid(best):
                best = blk
        elif t == OBJ_CHECKPOINT_MAP:
            maps.setdefault(xid(blk), []).append(blk)
    if best is None:
        sys.exit("no valid container superblock in the checkpoint area")

    block_count, = struct.unpack_from("<Q", best, 0x28)
    spaceman_oid, = struct.unpack_from("<Q", best, 0x98)
    sm_paddr = sm_size = None
    for cpm in maps.get(xid(best), []):
        count, = struct.unpack_from("<I", cpm, 0x24)
        for j in range(count):
            # checkpoint_mapping_t: type, subtype, size, pad, fs_oid, oid, paddr
            typ, _, size, _, _, oid, paddr = struct.unpack_from("<IIIIQQQ", cpm, 0x28 + 40 * j)
            if (typ & 0xFFFF) == OBJ_SPACEMAN and oid == spaceman_oid:
                sm_paddr, sm_size = paddr, size
    free = None
    if sm_paddr is not None:
        sm = os.pread(fd, sm_size, offset + sm_paddr * bs)
        if valid(sm) and obj_type(sm) == OBJ_SPACEMAN:
            # sm_dev[SD_MAIN] at 0x30: block_count, chunk_count, cib, cab, free_count
            dev_blocks, = struct.unpack_from("<Q", sm, 0x30)
            free, = struct.unpack_from("<Q", sm, 0x48)
            if dev_blocks != block_count:
                print(f"# warning: space manager has {dev_blocks} blocks, superblock {block_count}",
                      file=sys.stderr)
    print(f"APFS_BLOCK_SIZE={bs}")
    print(f"APFS_BLOCK_COUNT={block_count}")
    print(f"APFS_BYTES={block_count * bs}")
    print(f"APFS_XID={xid(best)}")
    print(f"APFS_BLOCK0_XID={xid(b0)}")
    if free is None:
        print("# no valid space manager found: free space unknown", file=sys.stderr)
        print("APFS_FREE_BLOCKS=")
        print("APFS_USED_BYTES=")
    else:
        print(f"APFS_FREE_BLOCKS={free}")
        print(f"APFS_USED_BYTES={(block_count - free) * bs}")


if __name__ == "__main__":
    main()
