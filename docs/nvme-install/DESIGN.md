# Omarchy on the iPad's internal NVMe: design

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Status 2026-10-22. Owner-run steps: [`test-plan.md`](test-plan.md). Files and
scripts: [`README.md`](README.md). Every claim is labelled **verified** (with
the evidence) or **inferred**.

## Decision

**Option B: shrink the iOS APFS container with iOS's own resizer, add a
Linux partition after it, and run Omarchy's ext4 root from that partition.**
This is HoolockLinux's documented route for internal storage. iOS stays
bootable, and the palera1n → pongoOS → m1n1 boot keeps working because it
needs a bootable iOS (see A). Linux gets a kernel write window that covers
only that partition.

- The resize runs on jailbroken iOS 16.7.10: `APFSContainerResize` from
  Apple's APFS.framework. That is Apple's own code, with SEP and the
  content-protection keys live, so it can move encrypted extents correctly.
- The GPT edit also runs on iOS, with a sector compare-and-swap tool.
- Linux writes only to the new partition, through a new boot-time NVMe
  write window (`nvme.write_window=1:FIRST-LAST`). Everything else stays
  refused in the driver, before submission. The system-namespace guard for
  NSIDs 2–7 is untouched.
- Option C (a preallocated file in a dedicated APFS volume, mapped with
  dm-linear) is the fallback if `APFSContainerResize` refuses.
- Option A (Linux only) is the last resort. It also takes away today's boot
  path.

A correction to the brief: `docs/pcie/read-only.md` has **no** boot or
runtime option that makes NSID 1 writable. `CONFIG_NVME_PCI_READ_ONLY` is
compile-time only. The one boot knob there,
`nvme.apple_ans_unlock_system_namespaces`, *lifts* the NSID 2–7 guard and
must never be used. The write window added here is new.

## Facts this rests on

> Private graphics-dependent excerpt omitted from this source snapshot.

## Options

### A. Linux only: replace the GPT

Put a Linux GPT on NSID 1 and give Linux the whole 32 GB.

> Private graphics-dependent excerpt omitted from this source snapshot.

### B. Shrink APFS, add a Linux partition (chosen)

1. On iOS: `resize_apfs disk0s1 <bytes>`. It is built from Hoolock's
   `tools/resize.c` and is a thin wrapper over `APFSContainerResize`.
2. On iOS: rewrite the GPT. p1 keeps its start, type, unique GUID, name and
   attributes, and ends where the container now ends. p2 (Linux filesystem
   type) is added behind a 1 MiB-aligned gap. The new sectors are computed
   on the Mac from a dump of the real GPT, and written by `gptcas`. It
   writes each copy only if it is still exactly the dumped bytes, or already
   the planned ones, so an interrupted run is finished by running again. It
   also refuses if the APFS container is still bigger than the new p1.
   `--reverse` undoes it.
3. On Linux: soak the new partition, write the Omarchy image into it, and
   boot with `omarchy.root=nvme`.

Why this one:
- The operation that can lose data (moving APFS extents) is done by
  Apple's code, which holds the keys.
- The iBoot/iOS tolerance of an extra partition is what HoolockLinux users
  rely on. iBoot finds the container as before: same partition 1, same
  start and GUIDs (**inferred**).
- Linux needs no APFS knowledge and no device-mapper.
- The fence is a single LBA range.

Costs and risks:
- The resize freezes iOS until it finishes (Hoolock README). The watchdog
  could panic a frozen iOS. APFS commits through checkpoints, so a panic
  mid-resize should leave the last consistent container (**inferred**). The
  test plan keeps the move small (target ≈ used + headroom) and says what
  to check.
- If iOS stops booting after the GPT edit, the exits are the reverse
  compare-and-swap (if iOS still runs) or a DFU restore. Linux data on p2
  would survive the first, not the second.
- Rollback: reverse CAS, then `resize_apfs disk0s1 0` (grow back to the
  partition) on iOS.

**Why not resize on the Mac**, by exporting the disk from Linux as USB mass
storage? macOS has none of this iPad's per-file keys, and the inline AES
tweak is tied to the extent (**inferred**). Relocating hardware-encrypted
extents without the keys risks silent data loss, or a refusal. No Linux
tool resizes APFS (apfsprogs has none, **inferred**).

### C. File-backed root inside APFS (fallback)

On iOS: `newfs_apfs -A` a dedicated, never-mounted volume (palera1n's
rootful "fakefs" does the same, so iBoot and iOS tolerate it, **inferred**).
Fill one file completely, make it immutable, and read its physical extents
with `fcntl(F_LOG2PHYS_EXT)`. Linux maps them with dm-linear (needs
`CONFIG_BLK_DEV_DM` and `dmsetup` in the initramfs).

- **Encryption is consistent for Linux.** Linux I/O carries no crypto
  bits, so it writes and reads back exactly what it stored (**inferred**
  from the TCB layout, and checked by the same soak). iOS would read the
  file through its per-file key as garbage. Harmless as long as nothing on
  iOS opens it. An unencrypted volume avoids even that.
- **The map can be checked before any write.** Compare per-extent sha256
  of raw reads on iOS (`/dev/rdisk0s1`) with Linux reads of the dm device.
- **Blocks don't move** while iOS doesn't write the file. APFS is
  copy-on-write, so one iOS write to the file would remap its blocks and
  leave Linux writing into free space APFS can reuse. Hence the immutable
  flag and a volume iOS never mounts. Snapshots and defrag don't move
  existing extents on iOS flash, and TRIM only touches free blocks
  (**inferred**).
- Costs: dm, an extent list (the fence becomes a list of ranges, or the
  command line gets long), and an iOS tool for `F_LOG2PHYS_EXT`. The safety
  of every Linux write then rests on APFS never relocating that file. In B
  it rests on a static GPT range.
- **Verdict:** use only if `APFSContainerResize` refuses (snapshots,
  minimum size).

### Other routes considered

- **Keep NBD, add a local cache** (bcache/dm-cache on a raw region): it
  needs a raw NVMe region anyway, so it is B plus complexity.
- **Linux-side APFS write** (out-of-tree apfs module): no resize, and it
  can't write encrypted iOS volumes. No.

## Layout

Computed by `scripts/nvme-install/plan-layout.py` from the live GPT dump
and the APFS container's own numbers. It writes one
`build/nvme-install/layout.env` that everything else reads: the GPT plan,
the image size, the payload bootarg and the on-device checks. Example for a
12 GiB container:

| Region | LBAs (4 KiB) | Size | Owner |
|---|---|---|---|
| PMBR + primary GPT | 0–5 | 24 KiB | unchanged |
| p1 APFS (iOS) | 6–3,145,733 | 12 GiB | iOS |
| gap A | 3,145,734–3,146,239 | ~2 MiB | nobody: fence negative test |
| p2 `omarchy-root` (8300) | 3,146,240–7,812,095 | 17.8 GiB | Linux, = `nvme.write_window` |
| gap B | 7,812,096–7,812,494 | ~1.6 MiB | nobody: fence negative test |
| backup GPT | 7,812,495–7,812,499 | 20 KiB | unchanged position |

- p2 starts and ends on 1 MiB boundaries, with at least 1 MiB free on each
  side. The gaps are the only sectors where a negative fence test is
  harmless even if the fence is broken. After the resize nobody owns them.
- The container size is `(P1_LAST − 5) × 4096` bytes, 4 KiB aligned, as
  `APFSContainerResize` needs.
- The default container size is the container's used blocks plus 3 GiB,
  rounded up to 1 GiB, and never below 12 GiB. The planner takes
  `--apfs-gib`.

## Kernel: the write window (`a9/nvme-install`, patch 0001)

Base: a9/pcie `946c7da17f`. New option `CONFIG_NVME_PCI_WRITE_WINDOW`
(default n; depends on `CONFIG_NVME_PCI_READ_ONLY`). New boot parameter
`nvme.write_window=NSID:FIRST-LAST` (LBAs, inclusive, 0444, also shown in
`/sys/module/nvme/parameters/write_window`).

What changes, gate by gate (`docs/pcie/read-only.md` numbering):

| Gate | Without the parameter | With `nvme.write_window=1:F-L` |
|---|---|---|
| 1, allowlist before submission | Read, Flush only | plus **Write** when nsid = 1 and the whole range `slba … slba+nlb−1` lies in F..L. Write Zeroes, DSM/trim, Write Uncorrectable, Compare, Verify, Copy, zone and vendor opcodes are still refused, and so is every write outside the window or to another NSID. The admin list is unchanged: no Format, Sanitize, firmware, namespace management, security or vendor commands. |
| 2, SQ-copy backstop | same check | same check (it calls the same function) |
| 3, read-only gendisks | `NVME_CTRL_FORCE_RO` on all | not set; `nvme_pci_ns_is_readonly()` keeps every namespace except the window's read-only, so nvme0n2..n7 stay `ro=1` |
| 4, system-namespace guard (patch 0026) | unchanged | unchanged: NSID ≠ 1 is Read/Flush only, and no destructive admin opcode on any NSID. So even `nvme.write_window=2:…` could not write NSID 2. |

- **Verified in QEMU** (`scripts/nvme-install/qemu-test.sh fence`, 103
  checks, with QEMU's NVMe and 4 KiB LBAs). The host-side compare of the
  namespace images shows that only the allowed blocks changed. Writes below,
  above, straddling either edge, at LBA 0, at the last LBA and to the other
  namespace are refused. Not covered: the Apple ANS paths (gate 4, FlatDMA),
  because QEMU's controller isn't Apple's.
- **Trim and Write Zeroes are not advertised** while a window is set
  (`NVME_QUIRK_DISABLE_WRITE_ZEROES`; `oncs &= ~DSM`, `dmrl = dmrsl = 0`,
  and the namespace scan's re-read of Identify CNS 06h skipped, since it
  would bring DMRSL back: found in QEMU). Otherwise
  ext4 or `blkdev_issue_zeroout` would get `EIO` from the gate instead of
  `EOPNOTSUPP` and its fallback to plain writes. Trim comes later, once the
  DSM payload path (`RQF_SPECIAL_PAYLOAD` through FlatDMA) has been tested
  and the fence can check DSM ranges.
- **Logs.** At probe: `read-only except NVMe Write to NSID 1 LBAs F-L
  (nvme.write_window) …`. After identify: `write window: controller ONCS …
  VWC … DMRSL …`. That shows whether the controller has a volatile write
  cache; we have no nvme-cli. Refusals: `read-only: refused write nsid N LBA
  X+K outside the write window`.
- A bad parameter is rejected at parse time and leaves the controller fully
  read-only (fail closed).
- The whole-disk node `nvme0n1` becomes writable at the block layer. Gate 1
  still refuses any write outside p2 with `EIO`, so `dd of=/dev/nvme0n1`
  cannot reach the GPT or iOS.

### Write path vs the read-only bring-up

| Item | Status |
|---|---|
| TCB direction 0x100 for Write | **verified** against iOS (above). Linux sets it from `rq_dma_dir()`; never exercised on hardware |
| Crypto bits / IV in the TCB | Linux leaves 0x1/0x4 and +0x18 at 0: no inline AES, so writes land as plaintext and read back as written (**inferred**; the soak checks it) |
| FlatDMA PRP, flag bit 5, 256 × 4 KiB per command, 36 slots | same code as reads (**verified for reads**) |
| Data path addresses | the SGL holds physical pages; the NVMMU, not the DART, translates data. The DART's direction bits (no-read on `DMA_TO_DEVICE` maps) only matter for queue memory (**inferred**, `HANDOFF.md` open point 3) |
| CPU cache maintenance | `dma_map_bvec(DMA_TO_DEVICE)` cleans lines unless the device is coherent. The PCIe node has no `dma-coherent` (**verified**, `s8003.dtsi`), so writes get cleans. The soak's buffered path (page cache → write → drop caches → read) checks it. |
| Flush (opcode 0) | already allowed and sent for fsync; whether the ANS has a volatile cache (VWC) is printed at boot. The soak times fsync. |
| FUA (Write CDW12 bit 30) | part of an allowed Write; used only if VWC is set |
| Discard / Write Zeroes | not advertised, refused |
| Shutdown | normal `CC.SHN` on poweroff, as on every read-only boot (**verified**: no issue so far) |
| Power loss / hard reset | ext4 journal plus Flush. Whether the ANS loses acknowledged-but-unflushed data is unknown (**inferred**: like any SSD) |

### Merging into a9/integ

```sh
scripts/integrate.sh 64221b189e d67fe465f2 f9910570b7 6ca4db7756 fbe859a481
```

`a9/nvme-install` (head `d67fe465f2`) contains pcie `946c7da17f`, so it takes pcie's slot. A trial merge of exactly this set is clean (**verified**, in a throwaway worktree). The
orchestrator also adds `patches/nvme-install/nvme-install.config` to
`FRAGMENTS` in `scripts/integrate.sh`; without it the option is off and the
kernel stays fully read-only. When a9/pcie moves on, run
`git rebase --onto <new pcie> 946c7da17f a9/nvme-install`. The patch only
touches `drivers/nvme/host/{Kconfig,pci.c}`.

## Boot from NVMe (initramfs)

`scripts/omarchy/initramfs/init` gets `omarchy.root=nbd|nvme`. The default
is `nbd`, with exactly today's path.

- `omarchy.root=nvme` finds the root as the partition that is exactly
  `/sys/module/nvme/parameters/write_window`: NSID through sysfs, start and
  size through sysfs. Device names aren't trusted (above).
  `omarchy.nvme=DEV` names it instead, and DEV must still be the window.
- The wait is bounded (`omarchy.nvme_wait`, default 60 s): the controller
  comes up in the background (`reset_work`). It gives up after 3 s once the
  namespace is there but no partition matches (a stale bootarg). It fails
  at once if there is no window: a read-only disk can't be the root.
- It reads the ext4 superblock with `dd` and checks the magic and the label
  `omarchy.label` (default `omarchy-nvme`).
- It then mounts `rw,noatime` (ext4 replays its journal there) and
  switch_roots.
- Any failure falls back to NBD, then to the rescue shell
  (`omarchy.fallback=nbd|rescue`).
- The USB gadget (ECM + ACM), DHCP for the Mac, the shells and netconsole
  (best effort, with a short bounded wait for the Mac) behave as today. SSH
  comes from the rootfs as before.
- The rootfs needs no change: its `/dev/nbd0` fstab line is harmless (QEMU,
  above), and the usb0 DHCP, ufw and conntrack settings are just as right
  with a local root. The image copy gets its own label and UUID
  (`omarchy-nvme`), so NBD and NVMe roots are never confused.
- **Verified in QEMU** (`qemu-test.sh init`, 16 checks) on the real Omarchy
  initramfs with the new `/init`:
  - NVMe root, by window and by explicit device;
  - NBD by default and on request, unchanged;
  - fallback to NBD, with the reason logged, for each of: no window, stale
    window, wrong label, wrong device, no NVMe;
  - `omarchy.fallback=rescue`.

## Validation and install steps (test plan)

1. A read-only backup from Linux before any write: GPT, first and last MiB,
   the APFS numbers, optionally the whole of NSID 1.
2. The resize and the GPT edit on iOS, each followed by an iOS boot.
3. One Linux session on the window kernel, with the NBD root:
   - guards;
   - hashes of everything outside p2 (GPT copies, p1, both gaps, NSIDs
     2–7);
   - the fence test, writing only in the unowned gaps;
   - a pattern soak on p2;
   - the same hashes again.

   Each 4 KiB block of the soak pattern carries its own LBA and a tag, so
   misdirected or stale writes show. The soak covers O_DIRECT, four
   parallel writers, random 4 KiB writes with fsync, and the page-cache
   path through `drop_caches`.
4. Persistence: power off, boot iOS, then re-verify the soak region in a
   second Linux session. This also shows iOS leaves p2 alone.
5. The image goes over SSH (zstd) into p2. A full read-back sha256 against
   the Mac's is the end-to-end check of the write path. Then the NVMe boot.
6. `scripts/nvme-install/run.sh` puts every step on the Mac's command line,
   and `nvme-install.py` refuses any write unless every guard holds.
   `qemu-test.sh tool` ran the whole tool against the planned GPT layout.

## What can go wrong, and the way back

| Event | Effect | Way back |
|---|---|---|
| `resize_apfs` refuses | nothing changed | Option C, or a smaller move (free space, delete snapshots) |
| panic or watchdog reboot during the resize | APFS recovers from its last checkpoint (**inferred**) | boot iOS, check `df`, retry; worst case DFU restore |
| iOS won't boot after the GPT edit | iOS and the pongoOS boot are both down | DFU restore (iOS, Linux data and GPT all reset); n2–n7 are untouched and dumped |
| fence test or soak fails | stop: no filesystem on NVMe | report; next step is IONVMeFamily's write-TCB setup in Ghidra |
| NVMe root won't mount | initramfs falls back to NBD | fix from the NBD-booted system (`e2fsck`, rewrite the image) |
| remove Linux, keep iOS | | `gptcas` reverse plan, then `resize_apfs disk0s1 0` on iOS |
