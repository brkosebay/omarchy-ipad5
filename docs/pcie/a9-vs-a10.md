# A9 (S8003, j71t) vs A10 (T8010, j172) PCIe/NVMe: what the T8010 driver programs, and what changes

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Sources:
- ADT dumps `docs/adt/j71t.txt` (A9, our device), `j71s/j72s/j72t` (the
  other iPad 5 boards) and `j172.txt` (A10 iPad 7, the template's target).
- Pauli1Go's driver (`pcie-apple-h9p.c`, `pci-apple-ans.c`) and his m1n1
  T8010 bring-up.
- Static analysis of the iOS 16.7.10 iPad6,11 kernelcache
  (`kernelcache-notes.md`, with addresses).

Every claim is marked **verified** (with evidence) or **inferred**.

**Bottom line:** S8003 has the same MMIO map as T8010, but iOS drives it with a
different register sequence. The A10 Linux sequence would have:
- polled the wrong status register (rc+0x28 instead of rc+0x2c),
- used T8010-only per-port registers (rc+0x134),
- waited for link-up on the wrong register (port+0x208 instead of port+0x88),
- tuned the wrong PHY lane for port 1.

The driver now has a separate S8003 path that follows the iOS code. The T8010
path is unchanged.

## Summary table

| Item | A10 T8010 (j172) | A9 S8003 (j71t) | Port handling |
|---|---|---|---|
| apcie compatible | `apcie,t8010` | `apcie,s8003` (TSMC). `apcie,s8000` on j71s/j72s (Samsung) | `apple,s8003-pcie` match data. S8000 not described. **verified** (ADT) |
| MMIO (`reg`, 12 entries) | config 0x610000000/16M, port0..3 0x60N000000 (+0x4000 NVMMU/MMU blocks), rc 0x600000000/0x8000, phy 0x600008000, phy-ip 0x60a000000/0x40000 | **identical**. S8000 has no phy-ip | same reg list as T8010. **verified** (ADT, and the kext maps the same indices) |
| arm-io / PCI `ranges` | identity for 0x6xxxxxxxx; MEM window 0xc0000000→0x7_c0000000 | identical | template kept. **verified** (ADT) |
| `lane-cfg` | 0 | **1** on all four iPad 5 ADTs | `apple,lane-cfg = <1>`, rc+0x4 = 0x11. **verified** (ADT + kext) |
| Common init | rc+0x4 = 0x10, wait rc+0x28 b4/b0, rc+0x34/0x14/0x24 = 1 | rc+0x4 = 0x10\|lane-cfg, rc+0x114 \|= 1, wait **rc+0x2c** b0, **rc+0x38** = 1, then ADT PHY + common tunables | S8003 path, without tunables (§6). **verified** (kext) |
| Per-port RC regs | rc+0x100/0x124/0x134 + N*0x80 | rc+**0x100/0x114/0x124/0x12c** + N*0x80, port-ready bit rc+0x2c b4 | S8003 path. **verified** (kext `_commonRegOffset` table) |
| NVMe mode | n/a in template | port 0 with `nvme-mode`: rc+0x14 = 1, 100 µs, rc+0x24 = 1, *before* port power-up | `apple,nvme-mode` on pci@0,0. **verified** (kext) |
| Link speed | rc+0x4020 = 3 (Gen3) + LNKCTL2 | rc+0x4020 = 3 always (**link-state counter control, not speed**). Speed only via root-port LNKCTL2, clamped to LNKCAP2 | per-port `max-link-speed`: port0 Gen2, port1 Gen1. **verified** (kext; ADT `maximum-link-speed` 2/1) |
| Link status | port+0x208 LTSSM 0x11..0x14 | **port+0x88 bit0** = link up; LTSSM = port+0x804 bits 14:9 (0x11 = L0) | S8003 path. **verified** (kext) |
| PHY lanes | port N → phy-ip + N*0x800, phy+0xc bit N | **per physical lane**: lane start (0,2,3,4); lane-cfg 1 → port0 = lanes 0–1 (mask 0x3, +0x0/+0x800), port1 = lane 2 (mask 0x4, +0x1000) | S8003 path. **verified** (kext) |
| phy-ip values | 175/333/530 timers, 0x20400 always | 0xa9/0x14d timers (no 0x200a4/0x200e4), 0x10188 = 0x100 at Gen2, 0x20400 only at Gen1/2 | S8003 path. **verified** (kext) |
| port+0x124 | "PWRCTL" 0x31 | **MSI control** `1 \| log2(8) << 4` = 0x31 | same value, now named right. **verified** |
| PERST#/CLKREQ# | GPIO 12/16. Linux waits for CLKREQ# | GPIO **160/164** (port0), **161/165** (port1). iOS doesn't wait for CLKREQ# (no `clkreq-wait-time`) | PERST# only. CLKREQ# not requested, so it keeps its peripheral function. **verified** (ADT, kext) |
| Port IRQs | 270/273/276/279 | 244/247/250/253 | DT. **verified** |
| MSI | AIC 288+, 8 per port | AIC **256**+, same split, doorbell 0xbffff000 | `msi-ranges`. **verified** (ADT, kext) |
| NVMMU | `nvme-mmu,t8010`, irq 272, tags 64 | `nvme-mmu,s8000`, irq 246, tags 128 but **64 usable** (`cache-invalidate-workaround`), 2nd reg used only with `invalidate-fifo-workaround` (absent) | §5. **verified** (kext) |
| DARTs | dart-apcie0 (0x601008000, irq 271), dart-apcie3 (Wi-Fi) | dart-apcie0 (irq **245**), dart-apcie**1** (0x602008000, irq **248**). Window 0x80000000+0x3c000000 | two DART nodes, `dma-ranges`. **verified** (ADT) |
| PMGR | clock-gates 56–58 | clock-gates **57–62** (pcie, pcie_aux, link0..3), power-gates 56 | all six + pcie_ref as power-domains. ID↔domain mapping **inferred** (iOS enables gate 0,1 then port+2) |
| S3E PCI ID | 106b:2002 | **106b:2002**, class 0x0180xx | **verified** (the A9 IONVMeFamily personality matches only `0x2002106b`) |
| L1SS / LTR | `pci-l1pm-control 0x4055000f` | **0x4044000f** / CTL2 0 | written after capability lookup. **verified** (ADT, IOPCIFamily) |
| HMB | runtime `nvme-mmu0.sart-region` (D111 0x8bee00000/0x1200000) | same mechanism (16 bytes `{pa, size}`; the IPSW ADT has 16 zero bytes there) | loader patch, or DART-mapped HMB (§5). Runtime fill on A9 **inferred** |

## 0. S8003 bring-up as now implemented (follows iOS)

Common, once (`apple_s8003_init_common`, from AppleS8003PCIe slot 227):

    rc[0x004] = 0x10 | lane_cfg            (0x11)
    rc[0x114] |= 1
    poll rc[0x02c] & 1
    rc[0x038] = 1
    (iOS: apply apcie-phy-tunables, apcie-common-tunables: skipped, see §6)

Per port (`apple_s8003_setup_port`, from AppleEmbeddedPCIEPort::enableGated
and AppleS800xPCIePort::enablePortHardware):

    PERST# asserted (since GPIO request)
    port0 with nvme-mode: rc[0x14] = 1; 100us; rc[0x24] = 1
    rc[0x124+p*0x80] &= ~1
    rc[0x114+p*0x80] |= 1; poll rc[0x2c] & 0x10; 100us
    rc[0x100+p*0x80] |= 1; poll rc[0x12c+p*0x80] & 1; rc[0x100+p*0x80] &= ~0x100
    100us (t-refclk-to-perst)
    rc[0x124+p*0x80] |= 1
    rc[0x4020+p*0x40] = 3
    port[0x210] W1C
    root port LNKCTL2.TLS = min(max-link-speed, LNKCAP2)
    port[0x104] = 0xff002fff; port[0x100] = 0xffd000; port[0x140] |= BIT(31)
    port[0x124] = 0x31; port[0x128] = 8p | 8p << 16
    PERST# deasserted
    poll phy[0xc] & lanes == lanes
    phy-ip common + per-lane programming (lane*0x800)
    port[0x104] = 0xff002f0f; port[0x80] |= 1 (LTSSM)
    poll port[0x88] & 1 (500 ms)

After all ports: 100 ms before enumeration (PCIe spec. iOS lets its IOKit
timing cover this).

Deliberate deviations from iOS (all **inferred** safe):
- No ADT tunables (§6).
- iOS clears rc+0x114 bit 8 at this point ("allow refclk gating", inferred
  name). We leave it alone, so refclk stays on.
- iOS calls `function-dart_force_active` and `function-nvme_mmu_force_active`.
  The NVMMU equivalent is that Linux programs the NVMMU after port 0 is up;
  there is no DART equivalent.
- iOS powers the Wi-Fi port only on demand (`manual-enable`). Linux trains it
  at probe, and a failure only drops that port.

## 1. Lane configuration

- `lane-cfg` values: only 1 and 3 are accepted on S8003/S8000. 0 panics with
  "X1 on port 0 not supported". 3 = `kAPCIE_LANE_CFG_X2_X1_X2`. **verified**
  (setLaneCfg). The driver rejects anything else for S8003.
- Widths: lane-cfg 1 → x2, x1, x1, x1. lane-cfg 3 → x2, x1, x2, (port 3
  unusable). **verified**
- Written once: rc+0x4 = 0x10 | lane-cfg. **verified**. A10 j172 has
  lane-cfg 0, which matches the template's 0x10.

## 2. PHY ("phy-ip")

S8003 (TSMC) has the phy-ip block; S8000 (Samsung) doesn't map it and has no
lane-init routine. **verified** (kext). The S8003 sequence (`kernelcache-notes.md`
§3c) differs from the A10 template in its lane indexing, timer values, and
the speed-conditional writes. The driver now follows iOS exactly for S8003.

## 3. Link speed

The target speed is LNKCTL2[3:0] of the root port only, clamped against
LNKCAP2 (iOS `setMaximumLinkSpeed`). rc+0x4020 = 3 is written for every port
regardless of speed. **verified**

## 4. MSI

`port+0x124 = 1 | log2(n) << 4`, `port+0x128 = base | base << 16`, vector v →
AIC 256 + v, MSI data = vector, address `msi-address` 0xbffff000. Same scheme
as the template, with a different AIC base. **verified**

## 5. NVMMU and ANS

- Init: write 0x10000 to +0x4 and poll until **bit 16** clears. The template
  polled bit 4. The S8003 path uses bit 16; T8010 is left as it was.
  **verified** (kext)
- +0x10/+0x14 hold a zeroed 4 KB scratch page, not a TCB table. The TCB array
  is 128×0x80. Register order: scratch, TCB base, init, SART. **verified**.
  The driver now allocates all 128 TCBs and uses this order.
- TCB flags carry one direction: 0x200 when the device writes memory (NVMe
  Read), 0x100 when it reads (NVMe Write). iOS panics on InOut; the template
  set both. Page count is written last. Retire = count 0, barrier, clear,
  write the tag to +0x4. **verified**. Implemented for both SoCs (shared iOS
  code).
- `cache-invalidate-workaround`: 64 usable tags, with a second TCB at t+64 used
  only for inline-crypto I/O. Linux uses 36 tags and no crypto, so nothing
  more is needed. **verified**
- The 2nd NVMMU reg (port 0 block) is used only for
  `invalidate-fifo-workaround == 1` (poll port0+0x22c). j71t doesn't have that
  property. **verified**
- SART: VA start = sart-virtual-base & 0x3fffffff, end = start + size − 1 MB
  (≤ 0xbff00000), PA >> 20. **verified**. Matches the template.
- HMB ("scratch buffer"), in the order IONVMeFamily tries them:
  `nvme-scratch-virt-region` (iBoot's SART window), then
  `nvme-scratch-region` (physical), then a buffer allocated by the host and
  mapped through the DART. **verified**. Linux supports the first (via the
  loader patch → nvme-hmb) and the last (`nvme.apple_ans_dart_hmb=1`).
- ANS BAR: 0x1800 = 0 (regular init) before CC.EN, 0x1808/0x180c HMB size and
  alignment, 0x1810/0x1818 HMB address and size, 0x1824/0x1828 core mask and
  log size. **verified**. Same as the template.

## 6. Data only in the live ADT

The IPSW ADT dumps lack what iBoot fills in at boot: `apcie-phy-tunables`
(iOS panics without it), `apcie-common-tunables`, the per-bridge
`apcie-config-tunables`/`pcie-rc-tunables`, `nvme-mmu0.sart-region` and
`s3e.nvme-scratch-virt-region`. **verified** (code requires them, dumps
don't have them). Linux doesn't apply the PHY/common/RC tunables. It relies on
what iBoot left in the registers when it read the kernelcache from NVMe
(**inferred**). The A10 port works the same way. The test plan dumps the
live ADT through m1n1's phram MTD (`/dev/mtd*`, label `adt`) so the tunables
can be added later.

## 7. DMA windows

Inbound addresses are routed by range: 0x40000000.. NVMMU FlatDMA,
0x80000000..0xbbffffff DART, 0xbc000000.. NVMMU SART, 0xbffff000 MSI.
**verified** (ADT `virtual-base`, DART vm-offset/size, `sart-virtual-base`,
`msi-address`; kext SART limit 0xbff00000).

This hoolock tree's apple-dart doesn't read `apple,dma-range`. Pauli1Go's
older apple-dart.c did (line 1329). **verified**. So the S8003 DT limits IOVAs
with standard host-bridge `dma-ranges`, and `iova_reserve_pci_windows()`
reserves everything outside the window. The T8010 DT from the template has
the same latent problem on this tree; it isn't touched here.

## 8. Known gaps

- `iommu-map` RID 0x200 → dart1 assumes Wi-Fi enumerates as bus 2. If port 0
  is dropped, Wi-Fi becomes bus 1 and maps to dart0. Harmless until a Wi-Fi
  driver does DMA.
- The S8000 (Samsung) boards j71s/j72s aren't enabled. They have no phy-ip,
  and the lane init routine is missing there (iOS slot 229 is empty on S8000).
- The FlatDMA command flag (cdw0 bit 5) and the latency-timer write come
  from the A10 template and weren't checked in the A9 kernelcache.

## 9. Round 2: live ADT and device results

- The live ADT (from m1n1's phram `/dev/mtd`, decoded in
  `live-adt-decode.txt`) has what the IPSW ADT lacks:
  - `nvme-mmu0`: `invalidate-fifo-workaround = 1`, and `sart-region` =
    0x87ee00000/0x1200000.
  - `s3e`: `nvme-scratch-virt-region` = 0xbc000000/0x1200000.
  - The tunables: 4 PHY, 34 common, and per bridge 6 config and 2 rc
    records.

  **verified** (device dump).
- With `invalidate-fifo-workaround = 1`, iOS polls port0+0x22c bit 0 until
  the FIFO is empty before every NVMMU invalidate (20 ms, then panic) and
  never reads NVMMU+0x4 back. **verified** (fffffff005eebc9c). The driver now
  does the same, and §5's "2nd reg only for the fifo workaround" turns out to
  matter on this device.
- The NVMe enable is standard and identical to Linux's: CC = 0x460001
  (64-byte SQEs). **verified** (fffffff0067a16bc and vtable +0x6c0/+0x6e8).
- Device, round 1: port 0 trains Gen2 x2 with no tunables applied. The S3E is
  106b:2002 class 0x018002, as predicted. The AIC has exactly 288 IRQs, so the
  MSIs at 256–287 fit. **verified**
- Linux turned ASPM L1 on for the S3E link by itself (DT-platform default).
  The ports now carry `aspm-no-l0s`/`aspm-no-l1`.
- The B1 hang analysis is in `b1-hang.md`.
