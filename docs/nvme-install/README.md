# NVMe install workstream: Omarchy on the iPad's internal disk

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Read [`DESIGN.md`](DESIGN.md) for the decision and the evidence, and
[`test-plan.md`](test-plan.md) for the owner-run steps.

**Shape:** iOS shrinks its own APFS container. iOS writes the new GPT
(p1 = iOS, p2 = Linux). Linux's NVMe driver can write only p2, through the
boot parameter `nvme.write_window`. The initramfs boots from p2 with
`omarchy.root=nvme`, and falls back to NBD.

## Files

| Path | What |
|---|---|
| `docs/nvme-install/DESIGN.md` | options A/B/C, the decision, the kernel gate, the write-path review, risks |
| `docs/nvme-install/test-plan.md` | owner-run steps with expected output, stop points, recovery |
| `patches/nvme-install/kernel/0001-*.patch`, `SERIES` | the kernel change, on a9/pcie `946c7da17f` |
| `patches/nvme-install/nvme-install.config` | `CONFIG_NVME_PCI_WRITE_WINDOW=y`: add it to `scripts/integrate.sh` FRAGMENTS |
| `scripts/omarchy/initramfs/init` | `omarchy.root=nbd\|nvme` (owned by this workstream this round) |
| `scripts/nvme-install/backup.sh` | read-only dumps from Linux before any write: first/last MiB, geometry, APFS numbers, optional full image |
| `scripts/nvme-install/apfs-nx.py` | read-only: APFS container size and used space from its newest checkpoint |
| `scripts/nvme-install/plan-layout.py` | the layout, and the new GPT sectors, from the dumps: writes `build/nvme-install/layout.env` and `gpt-plan/` |
| `scripts/nvme-install/ios/gptcas.c` | iOS (and macOS/Linux) GPT compare-and-swap writer with the APFS-fits guard and `--reverse` |
| `scripts/nvme-install/build-ios-tools.sh` | builds `gptcas` and Hoolock's `resize_apfs` for iOS, ldid-signed |
| `scripts/nvme-install/nvme-install.py` | on-device (Linux) checks, region hashes, fence test, soak, image write/verify |
| `scripts/nvme-install/run.sh` | runs one `nvme-install.py` step on the iPad from the Mac and keeps the log |
| `scripts/nvme-install/prep-image.sh` | `rootfs.img` → `rootfs-nvme.img`, exactly p2's size, own label and UUID |
| `scripts/nvme-install/mkpayload.sh` | `ro` / `window` / `nvme` payloads from `layout.env` |
| `scripts/nvme-install/build-initramfs.sh` | the Omarchy initramfs with the new `/init` → `build/nvme-install/initramfs-omarchy.gz` |
| `scripts/nvme-install/build-kernel.sh`, `ktree.sh` | build and edit the `a9/nvme-install` tree (`/src/wt/nvme-install`) |
| `scripts/nvme-install/test-gpt.sh`, `qemu-test.sh`, `qemu/` | offline tests (results below) |

## Offline results (2026-10-22, all verified on this Mac)

> Private graphics-dependent excerpt omitted from this source snapshot.

Found along the way (**verified in QEMU** with this kernel):
- **`nvmeXnY` is not the NSID.** In 2 of 4 fence boots NSID 2 came up as
  `nvme0n1`. Every tool and the initramfs therefore find namespaces by
  `/sys/block/*/nsid`. The initramfs finds the root as "the partition that
  is exactly the write window".
- **Read-only gendisks:** `open(O_RDWR)` succeeds, and the write, discard
  or zeroout then fails with `EPERM`. `docs/pcie/read-only.md` says
  `EACCES` at open, which doesn't hold for this kernel.
- `nvme_scan_work()` re-reads DMRSL (Identify CNS 06h) at scan time, so the
  patch also skips that re-read while a window is set.
- A loop device on a file bind-mounted from the Mac silently lost sgdisk's
  writes in Docker. Test disks are built inside the container.
