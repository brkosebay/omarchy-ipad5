# VXD decoder: device run 1 (2026-10-23 ~19:30)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

## T1, read-only probe (boot_firmware=0): PASS for safety, two surprises
```
apple-vxd 207f00000.vxd: coreId 0x3030811 coreRev 0x70b00, entropy pipes 1, pixel pipes 1, mtx mem 67584 KiB, mmu40 1, tiling-ctx 0, pipe status 0x700c67
apple-vxd 207f00000.vxd: GART initialized: dir 0x803758000, 512 page tables, dummy 0x80375c000
probe of 207f00000.vxd with driver apple-vxd returned 0 after 40064 usecs
```
- No "Bring core"/"known state" lines (no register writes), no abort/SError.
- IRQ: `38: 0 0 AIC 65693 Level 207f00000.vxd` (quiet).
- debugfs /sys/kernel/debug/apple-vxd/{mbox,regs} present.
- Surprise 1: coreId 0x03030811, not the expected 0x0007xxxx; high half
  0x0303 (> 6, the "unsupported" gate as described in the study).
- Surprise 2: **no /dev/video\* or /dev/media\*** after probe.

> Private graphics-dependent excerpt omitted from this source snapshot.

