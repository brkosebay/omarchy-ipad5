# A9 apcie / NVMe-MMU: findings from the iOS 16.7.10 kernelcache

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Source: `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12` (iOS 16.7.10,
iPad6,11 = j71t, `apcie,s8003`). It is a merged (non-fileset) prelinked arm64
kernelcache, so `ipsw kernel extract` does not work. The kexts were located through
`__PRELINK_INFO` and disassembled with capstone. The PCIe kexts are built at **-O0**,
so every register access goes through a virtual call with constant arguments, which
makes them easy to read. AppleNVMeMMU and IONVMeFamily are optimized builds. The
scripts are in `/tmp/kc-re/scripts` (scratch, not committed).

Kext text ranges (unslid):

| kext | `__TEXT_EXEC.__text` | notes |
|---|---|---|
| AppleEmbeddedPCIE | 0xfffffff005ecd000-0xfffffff005ee486c | base classes AppleEmbeddedPCIE / AppleEmbeddedPCIEPort, MSI controller |
| AppleS800xPCIe | 0xfffffff005ee5000-0xfffffff005eeca04 | AppleS800xPCIe, AppleS800xPCIePort, **AppleNVMeMMU** |
| AppleS8000PCIe | 0xfffffff005eed000-0xfffffff005eee1dc | AppleS8000PCIe(+Port); also carries the `ApplePCIEHostBridge` personality (`apcie-bridge`) |
| AppleS8003PCIe | 0xfffffff0066e0000-0xfffffff0066e17fc | AppleS8003PCIe(+Port), what j71t uses |
| IONVMeFamily | 0xfffffff006778000-0xfffffff0067bce20 | AppleEmbeddedNVMeController |
| IOPCIFamily | 0xfffffff005eaa000-0xfffffff005ecbfb4 | consumes `pci-l1pm-control`, `pci-max-latency` |

Class chain: `AppleS8003PCIe : AppleS800xPCIe : AppleEmbeddedPCIE : IOPCIHostBridge`
and `AppleS8003PCIePort : AppleS800xPCIePort : AppleEmbeddedPCIEPort : IOInterruptController`.
`AppleNVMeMMU : IOMapper`. Vtables: AppleS8003PCIe 0xfffffff006f55090, AppleS8003PCIePort
0xfffffff006f55998, AppleS800xPCIe 0xfffffff006e1c168, AppleS800xPCIePort 0xfffffff006e1ca70,
AppleNVMeMMU 0xfffffff006e1d360.

Below, "slot N" means vtable index N (byte offset N*8 from the first function pointer).
Method names in quotes are names I picked from behaviour. Only names that appear in
`__PRETTY_FUNCTION__` strings come from Apple.

Register blocks: these are the ADT `apcie` reg indices that the code maps.

| idx | addr | how the code gets it | name in code |
|---|---|---|---|
| 0 | 0x610000000 | AppleEmbeddedPCIE slot 240 | `_configMemoryMap` (ECAM) |
| 2p+1 | 0x60{1..4}000000 | AppleS800xPCIe slot 241 `2*port+1` | port regs (`this+0xd8` in the port) |
| 2p+2 | 0x60{1..4}004000 | slot 242 `2*port+2` | `_mmuRegistersMemoryMap` (port+0xb90/0xb98); for port 0 this is the NVMMU |
| 2N+1 = 9 | 0x600000000 | slot 243 `2*getPortCount()+1` | `_commonRegistersMemoryMap` ("rc") |
| 2N+2 = 10 | 0x600008000 | slot 244(0) | `_phyRegistersMemoryMap` ("phy") |
| 2N+3 = 11 | 0x60a000000 | slot 244(1) | `_phyIPRegistersMemoryMap`, **S8003 only** |

All VERIFIED. AppleS800xPCIe slots 241-244 @0xfffffff005ee51f8/5220/5248/527c,
AppleEmbeddedPCIE::start @0xfffffff005ed03ec, S8003 configure @0xfffffff0066e0350.

---

## Implications for the Linux driver (corrections to the A10/T8010 model)

1. **Lane config.** Write `rc+0x4 = 0x10 | lane-cfg`, using the whole value. On j71t
   that is 0x11. Only lane-cfg 1 and 3 are accepted. 0 panics ("X1 on port 0 not
   supported") and any other value panics too. (VERIFIED)
2. **Common status is `rc+0x2c`, not `rc+0x28`.** Init polls `rc+0x2c` bit0. Per-port
   power-up polls `rc+0x2c` bit4. After common init the driver writes `rc+0x38 = 1`.
   S8003 also sets `rc+0x114 |= 1` during common init, which S8000 does not. (VERIFIED)
3. **NVMe mode** applies to port 0 when the bridge has `nvme-mode`, and it runs *before*
   the port hardware is enabled: `rc+0x14 = 0x1` (0x101 when `no-nand-sysclk-gating`
   is present), wait 100 us, then `rc+0x24 = 1`. To disable, write `rc+0x24 = 0`.
   No A9 code touches `rc+0x34`. (VERIFIED)
4. **The per-port common registers** are `rc + {0x100, 0x114, 0x124, 0x12c} + port*0x80`.
   A10 used 0x134; A9 uses 0x12c as the ready register. The bit meanings are in Q3.
   (VERIFIED)
5. **`rc+0x4020+port*0x40` is not the link speed.** The driver writes 3 there on port
   enable and 7 before sampling the link-state counters at `rc+0x4000/0x4010+port*0x40`.
   The target speed is set only through the root port's **LNKCTL2[3:0]** (16-bit config
   write, clamped against LNKCAP2), using the bridge's `maximum-link-speed`. (VERIFIED)
6. **The phy-ip stride is per lane, not per port.** The lane start table is (0, 2, 3, 4)
   for ports 0..3, and the width comes from lane-cfg. With lane-cfg 1, port 0 = lanes 0-1
   (phy+0xc ready mask 0x3, phy-ip regs at +0x0000 and +0x0800) and port 1 = lane 2
   (mask 0x4, phy-ip regs at +0x1000). The phy-ip sequence runs on every port enable,
   right before LTSSM enable, because j71t has `no-phy-power-gating`. (VERIFIED)
7. **Extra phy-ip steps compared with the A10 list:** write `lane+0x10188 = 0x100` only
   when the root port's TLS is 2 (Gen2), and write `lane+0x20400 = (x & ~0x1f) | 0xa`
   when TLS is 1 or 2. The common writes are 0x180/0x184 `|= 0x4000`,
   `0x90 = (x & ~0xfff) | 0x64` and `0x98 = (x & ~0xfff) | 0x19`. (VERIFIED)
8. **Link status is `port+0x88` bit0, not +0x208.** The LTSSM state is
   `(port+0x804 >> 9) & 0x3f`, using the DesignWare encoding where 0x11 = L0. The full
   table is in Q3. (VERIFIED)
9. **`port+0x124` is MSI control, not pwrctl.** It holds `1 | log2(#msi-vectors) << 4`
   (0x31 for 8 vectors), and `port+0x128 = base | base << 16` (port 1: 0x00080008).
   AIC irq = `msi-vector-offset`(256) + vector, and the MSI doorbell is `msi-address`
   (0xbffff000). (VERIFIED)
10. **NVMMU corrections.** Init writes +0x4 = 0x10000 and polls until **bit 16 clears**.
    +0x10/+0x14 hold a zeroed 4 KB scratch page, not a TCB table. TCB base is at
    +0x8/+0xc. With `cache-invalidate-workaround` only **64** tags are used (each tag has
    a second TCB at tag+64 for crypto I/O). The NVMMU registers lose state when port 0 is
    powered off and must be rewritten after the port powers on (`nvme_mmu_force_active`).
    The second reg range (0x601000000) is mapped only for `invalidate-fifo-workaround == 1`,
    which j71t does not have. (VERIFIED)
11. **The IPSW ADT is missing data that iBoot fills in at boot.** `apcie-phy-tunables` is
    *required*: `AppleS800xPCIe::configure` panics without it. `sart-region` and
    `nvme-scratch-virt-region` are only zero placeholders in the IPSW ADT, and
    `apcie-common-tunables`, `apcie-config-tunables` and `pcie-rc-tunables` are absent.
    The Linux driver or bootloader needs the **live ADT** (m1n1/pongoOS) and has to apply
    these tunables. (VERIFIED that the code requires them; INFERRED that iBoot is what
    fills them.)
12. **L1SS and LTR are handled by IOPCIFamily, not by the NVMe driver.** `pci-l1pm-control`
    is 8 bytes, `{u32 CTL1, u32 CTL2}`. IOPCIFamily writes CTL2 first, then
    `CTL1 & enable-mask`. On the s3e that is CTL1 0x4044000f / CTL2 0; on pci-bridge0
    it is 0x4044190f / 0x28. `pci-max-latency` (0x10081008) goes to LTR cap + 4. The
    absolute offsets 0x188/0x18c/0x17c come from the A10 port and are INFERRED (they
    depend on the S3E capability walk).

---

## Q1. lane-cfg

**Parsing.** `AppleS800xPCIe::start` @0xfffffff005ee52bc reads the `apcie` property
`lane-cfg` (u32) through the DT helper sub_0xfffffff005ee202c, then calls slot 230
(`setLaneCfg`). It also reads `muxed-auxclk-auto-dis` into `this+0x22c` and
`perst-to-refclk-delay-us` into `this+0x228`. (VERIFIED)

**setLaneCfg, slot 230.** S8003 @0xfffffff0066e0540, S8000 @0xfffffff005eeda80:
```
this->_laneCfg (+0x188) = v;
switch (v) { case 1: case 3: ok;
  case 0: panic("X1 on port 0 not supported")          // S8000 adds "rdar://18091121"
  default: panic("unrecognized lane-cfg value %u"); }
```
(VERIFIED) `getLaneCfg` is AppleEmbeddedPCIE slot 246 @0xfffffff005ed406c, which returns `+0x188`.

**Enum values.** The only enum name present as a string is `kAPCIE_LANE_CFG_X2_X1_X2`.
It equals **3** because `AppleS8003PCIePort::enablePortHardware` @0xfffffff0066e1634
compares `getLaneCfg()` with 3 in
`REQUIRE(_apcie->getLaneCfg() != kAPCIE_LANE_CFG_X2_X1_X2 || _portIndex != 3)`.
The names for 0 and 1 are not in the binary. Judging from the width function, 0 would
be "X1_X1_X1_X1" and 1 would be "X2_X1_X1_X1" (INFERRED names).

**Lane width per port, slot 247.** S8003 @0xfffffff0066e060c, S8000 @0xfffffff005eedb4c,
identical code:

| lane-cfg | port0 | port1 | port2 | port3 |
|---|---|---|---|---|
| 0 | x1 | x1 | x1 | x1 |
| 1 (j71t) | **x2** | x1 | x1 | x1 |
| 3 (X2_X1_X2) | x2 | x1 | x2 | not allowed |
| other | panic | | | |

(VERIFIED) The result is cached in the port at `+0x1c4` by
`AppleEmbeddedPCIEPort::initWithAPCIeAndRegistryEntry`.

**Hardware programming of lane-cfg.** It is used in exactly one place: the common init
(slot 227, S8003 @0xfffffff0066e0e94) does `writeCommon(0x4, 0x10 | _laneCfg)`. So it
lands in the **low bits of rc+0x4**, with bit4 always set and nothing masked. j71t
gets `rc+0x4 = 0x11`. (VERIFIED) Every other use of lane-cfg goes through the lane mask
below.

**Port to PHY lane mapping.** In S8003 slot 229 @0xfffffff0066e0700 ("init phy-ip
for port"), called from `AppleEmbeddedPCIEPort::enableGated`:
```
width = this->laneWidth(port)                       // slot 247
mask  = ((1 << width) - 1) << laneStart[port]       // laneStart @0xfffffff00568f4d8 = {0,2,3,4}
while ((readPhy(0x0c) & mask) != mask) IODelay(10);  // phy block 0x600008000
... phy-ip common writes ...
for (lane = 0; mask; lane++, mask >>= 1)
    if (mask & 1) program lane registers at phy-ip + lane*0x800 + {0x10xxx, 0x20xxx}
```
(VERIFIED) **The stride follows the physical lane index**, not the port index:

| lane-cfg | port | lanes | phy+0xc ready bits | phy-ip lane blocks |
|---|---|---|---|---|
| 1 | 0 (NVMe) | 0,1 | 0x3 | +0x0000, +0x0800 |
| 1 | 1 (wlan) | 2 | 0x4 | +0x1000 |
| 1 | 2 | 3 | 0x8 | +0x1800 |
| 1 | 3 | 4 | 0x10 | +0x2000 |
| 0 | 0 | 0 | 0x1 | +0x0000 (lane 1 unused) |
| 3 | 2 | 3,4 | 0x18 | +0x1800, +0x2000 |

S8000 does not override slot 229 (it inherits the empty AppleEmbeddedPCIE default
@0xfffffff005ed4034), has no lane table and never maps phy-ip. On S8000, lane-cfg only
reaches rc+0x4. (VERIFIED)

## Q2. `_commonRegOffset` and the other subclass overrides

`_commonRegOffset(id, port)` is slot 260. AppleS800xPCIe leaves it **pure virtual**:
slot 260 → 0xfffffff0076fca94 = `__cxa_pure_virtual`. This kernelcache has no T8010
class. S8000 @0xfffffff005eedefc and S8003 @0xfffffff0066e1008 use the same 6-entry
jump table, both asserting `port < getPortCount()` and panicking with
"Unknown register id" for id > 5:

| id | offset (S8000 = S8003) | used by (AppleS800xPCIe slot) | meaning (see Q3) |
|---|---|---|---|
| 0 | 0x24 (ignores port) | 253 @0xfffffff005ee56d0: `w(0x24, !arg)` | NVMe-mode enable |
| 1 | 0x2c (ignores port) | 256: poll bit4 | common status |
| 2 | 0x100 + port*0x80 | 255 (bit0), 257 (bit8), 258 (bit20) | port ctl 0 |
| 3 | 0x114 + port*0x80 | 256 (bit0 + poll), 250 (bit8, inverted) | port power / refclk |
| 4 | 0x124 + port*0x80 | 254 (bit0, inverted) | refclk / PERST-side enable |
| 5 | 0x12c + port*0x80 | 259 (bit0 read) | port ready |

(VERIFIED) **S8000 and S8003 do not differ here.**

Overrides on AppleS8003PCIe beyond the dtor and `getMetaClass` (VERIFIED addresses,
INFERRED names):

| slot | S8003 | S8000 | what it does |
|---|---|---|---|
| 187 `configure` | 0x…66e0350 | 0x…5eed864 | S8003 maps **phy-ip** (idx 11) to +0x210/+0x218. S8000 maps phy (idx 10) to +0x1e0 and zeroes the phy-ip fields. Both read `no-nand-sysclk-gating` (boot-arg `nand-sysclk-gating`) into +0x22d, then call AppleS800xPCIe::configure, which needs `apcie-phy-tunables` and maps phy (idx 10). |
| 215, 216 | return 0xe00002c7 | same | unsupported hooks |
| 223 | 0x…66e0324 | 0x…5eed838 | creates the platform's `AppleS800{0,3}PCIePort` (0xba0 bytes) |
| 227 "initCommon(resume)" | 0x…66e0e94 | 0x…5eeddd0 | rc+0x4, (S8003: rc+0x114\|=1), poll rc+0x2c b0, rc+0x38=1, PHY + common tunables. See Q3. |
| **229 "phy-ip lane init"** | 0x…66e0700 | not overridden | S8003 only. See Q1/Q3. |
| 230 setLaneCfg | 0x…66e0540 | 0x…5eeda80 | identical apart from the panic string |
| 231 | returns +0xf8 | same | `msi-address` getter |
| 245 | returns +0x200 | same | `early-end-point-perst-ctrl` flag |
| 247 laneWidth | 0x…66e060c | 0x…5eedb4c | identical |
| 251 `_enableNVMeMode` | 0x…66e0d04 | 0x…5eedc40 | identical |
| 252 `_disableNVMeMode` | 0x…66e0df4 | 0x…5eedd30 | identical |
| 260 `_commonRegOffset` | 0x…66e1008 | 0x…5eedefc | identical table |

Port classes (`AppleS800{0,3}PCIePort`) override only slot 230 `enablePortHardware`,
which adds the X2_X1_X2/port-3 REQUIRE and then calls AppleS800xPCIePort's version,
and slot 262, which returns `true` (same as the base). (VERIFIED)

**Summary of S8000 vs S8003:** S8003 adds phy-ip mapping and init (slot 229) and the
`rc+0x114 |= 1` step in common init. Everything else is identical.

## Q3. Port bring-up sequence (S8003)

### 3a. Controller init, once at `AppleEmbeddedPCIE::configure` @0xfffffff005ed2abc
1. AppleS800xPCIe slot 224 @0xfffffff005ee55f0: `provider->slot173(1, 0)`,
   `provider->slot173(1, 1)`, then `provider->slot173(1, port+2)` for each existing port.
   These are the six ADT `clock-gates` (57,58 common; 59..62 port 0..3). (VERIFIED
   calls; INFERRED that slot 173 on AppleARMIODevice enables gate index N)
2. slot 227(false), S8003 @0xfffffff0066e0e94: (VERIFIED)
   ```
   rc[0x004] = 0x10 | lane_cfg                  // 0x11 on j71t
   rc[0x114] |= 1                               // S8003 only (= port0 id3 bit0)
   while (!(rc[0x02c] & 1)) IODelay(10);
   rc[0x038] = 1
   applyTunables(apcie-phy-tunables, readPhy/writePhy, "PCIe PHY")      // phy block 0x600008000
   applyTunables(apcie-common-tunables, readCommon/writeCommon, "APCIe common")
   ```
   On resume, AppleEmbeddedPCIE slot 248 @0xfffffff005ed4270 first reapplies
   "APCIe common" and then calls slot 227(true).
3. Every port that is not `manual-enable` is enabled through `enableGated` (3b), and the
   driver waits for link-up on that port mask (`ltssm-timeout`, default 100 ms, at
   `+0x164`). pci-bridge1 (wlan) has `manual-enable`.

**Tunable format** (AppleEmbeddedPCIE slot 236 @0xfffffff005ed340c): 24-byte records
`{u32 offset; u32 size (must be 4, 0 = skip); u64 mask; u64 value}`, applied as
`r = (r & ~mask) | (value & mask)` and written only when the value changes. (VERIFIED)

Tunable property → target region (VERIFIED):

| property | node | region |
|---|---|---|
| apcie-phy-tunables | apcie | phy block 0x600008000 (required) |
| apcie-common-tunables | apcie | rc 0x600000000 |
| apcie-config-tunables | bridge | port block 0x60X000000 |
| pcie-rc-tunables, pcie-rc-gen3/gen4-shadow-tunables | bridge | root port config space (ECAM, bus 0 dev=port) |
| dbi-overrides | bridge | root port config space (port +0x230/+0x238) |

None of these appear in the IPSW ADT (see implication 11).

### 3b. `AppleEmbeddedPCIEPort::enableGated` @0xfffffff005ed954c, in order
`p` = port index, `apcie` = AppleS8003PCIe. Everything here is VERIFIED unless marked otherwise.

1. If the bridge has `nvme-mode`, call port slot 231 → apcie `_enableNVMeMode(p)`
   (p must be 0):
   `rc[0x14] = sysclkGating ? 0x1 : 0x101; IODelay(100); rc[0x24] = 1`.
2. If the port is off, call **enablePortHardware** (AppleS800xPCIePort slot 230
   @0xfffffff005ee73dc, behind the S8003 REQUIRE wrapper):
   1. `rc[0x124+p*0x80] &= ~1` (slot 254 with true)
   2. `rc[0x114+p*0x80] |= 1; while (!(rc[0x2c] & 0x10)) IODelay(10);` (slot 256)
   3. `IODelay(100)`
   4. `rc[0x100+p*0x80] |= 1` (port slot 191 → apcie slot 255)
   5. `while (!(rc[0x12c+p*0x80] & 1));`, a busy poll with no delay (slot 259)
   6. `rc[0x100+p*0x80] &= ~0x100` (slot 257 with false)
   7. If `muxed-auxclk-auto-dis`: `rc[0x100+p*0x80] |= 0x100000` (slot 258)
   8. `IODelay(t-refclk-to-perst)`, which is port `+0x1b0` = 100 on j71t
   9. `rc[0x124+p*0x80] |= 1` (slot 254 with false)
   10. `rc[0x4020+p*0x40] = 3` (port slot 236 → apcie slot 263; INFERRED to be the
       link-state counter enable)
   11. `v = port[0x210]; if (v) port[0x210] = v` (W1C of "linkpmgrsts")
   12. Unless `no-refclk-gating`: `rc[0x114+p*0x80] &= ~0x100` (port slot 234 → apcie
       slot 250 with true; INFERRED to allow refclk gating)
   13. Call `function-dart_force_active` (port +0x140) with 1, then
       `function-nvme_mmu_force_active` (port +0xb88) with 1. The second one makes
       AppleNVMeMMU reprogram its registers (Q4).
   14. Find the root port PCIe cap (`findPCICapability(0x10)` → `+0x22c`, REQUIRE
       nonzero). If `+0x1c0` (runtime override) or `maximum-link-speed` (`+0x1b4`) is
       set, call `setMaximumLinkSpeed` @0xfffffff005ee1118:
       `sup = (LNKCAP2 >> 1) & 0x7f; while (s && !(sup & 1<<(s-1))) s--;`
       `LNKCTL2 = (LNKCTL2 & ~0xf) | s` (cap+0x30, 16-bit). j71t: port0 = 2, port1 = 1.
   15. Apply `dbi-overrides`, then "PCIE RC" (`pcie-rc-tunables`, root port config),
       then "APCIE Config" (`apcie-config-tunables`, port block).
3. Port slot 186 @0xfffffff005ee6e3c: `port[0x104] = irqmask (+0x224, 0xff002fff or
   0xff002f0f)`, `port[0x100] = 0xffd000` (ack all).
4. Unless debug flag 0x1000000 is set: `port[0x140] |= 0x80000000` (port slot 196).
5. MSI, port slot 198 @0xfffffff005ee6808: `port[0x124] = 1 | log2(n) << 4` for
   n ∈ {1,2,4,8,16,32} (any other n panics "Invalid msiVectors"), then
   `port[0x128] = base | base << 16`. When n = 0 it writes `port[0x124] = 0`.
6. If `clkreq-wait-time` > 0, wait for the endpoint to assert CLKREQ# through
   `function-clkreq` ("timed out waiting for endpoint to assert CLKREQ#"). j71t has no
   `clkreq-wait-time`.
7. If state == 1 (powering up):
   1. **PERST# deassert.** When `early-end-point-perst-ctrl` is absent (j71t): port
      slot 194 (no-op), then slot 195(0) calls `function-perst` with value 0
      ("EP PERST = 0"). This happens *after* step 2.8 (`t-refclk-to-perst`).
   2. If `no-phy-power-gating` (j71t has it): **S8003 slot 229(p)**, the phy-ip lane
      init below.
   3. **LTSSM enable**: `port[0x80] |= 1` (port slot 226).
   4. Arm the `ltssm-timeout` timer, then wait for link-up (`port[0x88]` bit0, see
      below).

PERST# is *asserted* in `disableGated` (sub_0xfffffff005ed9c98) with slot 195(1).
Before that, the root port gets cfg 0x04 = 0 and cfg 0x18 &= 0xff0000ff. The port is
then powered down by AppleS800xPCIePort slot 229 @0xfffffff005ee72c8: dart and mmu
force-active set to 0, `rc[0x124+p*0x80] &= ~1`, `IODelay(perst-to-refclk-delay-us = 60)`,
`rc[0x114+p*0x80] &= ~1`, `rc[0x100+p*0x80] &= ~1`. I did not find where PERST# is
first asserted at boot. It is presumably the GPIO default or iBoot. (INFERRED)

### 3c. phy-ip lane init (S8003 slot 229 @0xfffffff0066e0700), VERIFIED
`spd = rootport.LNKCTL2 & 0xf` (sub_0xfffffff005ee12d0 reads `expressCap + 0x30`). `L`
loops over the lanes in the port's mask (Q1). phy-ip offsets are relative to 0x60a000000.
```
poll phy[0x0c] & mask == mask (IODelay 10)
pip[0x180] |= 0x4000;  pip[0x184] |= 0x4000
pip[0x090] = (pip[0x090] & ~0xfff) | 0x64
pip[0x098] = (pip[0x098] & ~0xfff) | 0x19
for each lane L in mask:  b = L*0x800
  pip[b+0x10088] |= 0x4000
  pip[b+0x10784]  = 0
  pip[b+0x10004]  = (x & ~0xfff) | 0x600
  pip[b+0x20788]  = 0x3105
  pip[b+0x207a0]  = (x & ~0xff) | 0x9f
  pip[b+0x207a8]  = (x & ~0xff) | 0x01
  if (spd == 2)            pip[b+0x10188] = 0x100
  if (spd == 1 || spd == 2) pip[b+0x20400] = (x & ~0x1f) | 0xa
  pip[b+0x2009c] = 0xa9;  pip[b+0x200dc] = 0xa9
  pip[b+0x200a0] = 0x14d; pip[b+0x200e0] = 0x14d
  pip[b+0x20330] = 0;     pip[b+0x20340] = 0;  pip[b+0x20350] = 0
```
Access is through `_readPhyIPReg`/`_writePhyIPReg` (AppleS800xPCIe slots 261/262
@0xfffffff005ee5c30/5d38). These are plain `ml_io_read32/ml_io_write32` on
`+0x218` and panic if phy-ip is not mapped.

### 3d. Port block registers (0x60X000000), AppleS800xPCIePort slots 255/256 (VERIFIED offsets)

| off | use |
|---|---|
| 0x80 | bit0 LTSSM enable (slot 226) |
| 0x84 | bit0 set by slot 201 (INFERRED: PME_Turn_Off request) |
| 0x88 | "linksts": bit0 link up (slot 187), bit6 checked by slot 216 (INFERRED: PME_TO_ACK) |
| 0x8c | slot 224 writes 0x31; slot 225 checks bit0/bit5 (INFERRED: L2/deep-sleep handshake) |
| 0x100 | "pcielint" irq status, W1C (0x1000 acked by slot 197, 0xf0 by slot 199, 0xffd000 = ack all) |
| 0x104 | irq mask: 0xff002fff all masked, 0xff002f0f normal |
| 0x124 / 0x128 | MSI control / vector base (Q6) |
| 0x140 | bit31 enable (slot 196) |
| 0x210 | "linkpmgrsts", W1C |
| 0x804 | "linkcdmsts": LTSSM = bits 14:9 |

Offsets ≥0x800 cannot be accessed in deep sleep (state 5), per slot 214.

LTSSM names, from the table @0xfffffff007a60538 (VERIFIED): 0 DETECT_QUIET, 1 DETECT_ACT,
2 POLL_ACTIVE, 3 POLL_COMPLIANCE, 4 POLL_CONFIG, 5 PRE_DETECT_QUIET, 6 DETECT_WAIT,
7 CFG_LINKWD_START, 8 CFG_LINKWD_ACEPT, 9 CFG_LANENUM_WAIT, 0xa CFG_LANENUM_ACEPT,
0xb CFG_COMPLETE, 0xc CFG_IDLE, 0xd RCVRY_LOCK, 0xe RCVRY_SPEED, 0xf RCVRY_RCVRCFG,
0x10 RCVRY_IDLE, **0x11 L0**, 0x12 L0S, 0x13 L123_SEND_EIDLE, 0x14 L1_IDLE, 0x15 L2_IDLE,
0x16 L2_WAKE, 0x17-0x19 DISABLED*, 0x1a-0x1d LPBK*, 0x1e/0x1f HOT_RESET*, 0x20-0x23 RCVRY_EQ0-3.

Root-port config accesses go through ECAM idx 0 with BDF = (0, port, 0), and the
extended register number is placed in bits 27:24 (sub_0xfffffff005ede888/80c).

## Q4. AppleNVMeMMU (AppleS800xPCIe kext, `start` @0xfffffff005ee8bf0)

**Properties** (VERIFIED):
- `cache-invalidate-workaround` sets `+0x120`.
- `invalidate-fifo-workaround` (u32, 0..2) goes to `+0x190`. j71t does not have it, so it is 0.
- The `tags` value is clamped to 1..128 and stored at `+0x100`. With
  cache-invalidate-workaround the usable tag count `+0x104` is tags/2 = **64**;
  otherwise it equals tags.
- `sgl-entries` must be a multiple of 64 and at most 0x800 (`+0x130`).
- `virtual-base` must be 64 MB aligned (`+0x150`).
- Boot-args: `nand-no-wrapped-keys`, `nvme-invalidate-delay` (default 8 us), `nvme-mmu-trace`.

**Mappings** (VERIFIED): reg[0] (0x601004000) is the NVMMU register block (`+0xd8`).
**reg[1] (0x601000000, the port 0 block) is mapped only when
`invalidate-fifo-workaround == 1`**. It is then used for one thing: before each
invalidate, poll `port0[0x22c]` bit0 (FIFO empty) with a 20 ms timeout and panic
"NVMe invalidate FIFO failed to empty". Mode 2 instead enforces `nvme-invalidate-delay`
between two invalidates. On j71t (mode 0) the driver never touches reg[1].

**Buffers** (VERIFIED):
- TCB array: 0x4000 bytes (128 × 0x80), contiguous, aligned to 0x80.
- SGL pool: `sgl-entries × usableTags × 4` bytes, aligned to 0x40. Tag t's SGL is at
  `sgl + t*sgl_entries*4`.
- A zeroed 4 KB scratch page.

**Register map** (writes via sub_0xfffffff005eebfe8 = `ml_io_write32(+0xd8 + off)`):

| off | value | status |
|---|---|---|
| 0x04 | init: write 0x10000, then poll until `(r & 0x10000) == 0` (no delay). Invalidate: write tag number. | VERIFIED |
| 0x08 / 0x0c | TCB array phys lo / hi; the init above runs right after this | VERIFIED |
| 0x10 / 0x14 | 4 KB scratch page phys lo / hi (**not** a TCB table) | VERIFIED value, INFERRED purpose |
| 0x20 | SART enable (bit0) | VERIFIED |
| 0x24 | SART VA start = `sart-virtual-base & 0x3fffffff` (0x3c000000 on j71t) | VERIFIED |
| 0x28 | SART VA end = start + size - 0x100000 (last MB, inclusive). Must be ≥ start and ≤ 0xbff00000 | VERIFIED |
| 0x2c | SART PA >> 20 | VERIFIED |
| 0x30, 0x34(&0x7fffffff), 0x38, 0x3c/0x44 (64-bit addr), 0x40 (bits 6:0 in-crypto tag, 14:8 out-crypto tag), 0x48 | error/irq status, read by the handler @0xfffffff005eea2fc | VERIFIED reads, INFERRED meanings |

`sart-region` is 16 bytes `{u64 pa, u64 size}` (pa and size MB-aligned,
size ≤ 1 GB). The IPSW ADT only has a 4-byte 0 placeholder, which would fail the
length REQUIRE. iBoot must fill it in (INFERRED).

The order of these writes in `setActive(true)` (sub_0xfffffff005eebf3c, reached through
the "Fact" platform function behind `function-nvme_mmu_force_active`) is:
scratch page (0x10/0x14) → TCB base (0x08/0x0c) → init (0x04) → SART (0x24, 0x28, 0x2c,
then 0x20). Registers are written only while active (`+0x155`).

**TCB, 0x80 bytes** (map path slot 170 @0xfffffff005eeaf20), VERIFIED:

| off | field |
|---|---|
| 0x00 | flags. IOKit `kIODirectionIn` (device writes memory = NVMe read) → **0x200**; `kIODirectionOut` (device reads memory = NVMe write) → **0x100**. `\|0x1` when inline crypto is used, `\|0x4` for a SEP-wrapped key. `InOut` panics. |
| 0x04 | number of 4K pages. Written **last**, after `dmb ish`. |
| 0x08 | u32 sgl[0] = first PFN (pa >> 12) |
| 0x10 | u64 SGL physical address, written when the page count is > 0 |
| 0x18 | u64 crypto IV / offset (`(cpoff + off) >> 12`) |
| 0x40-0x7f | AES key material (32 or 64 bytes) |

SGL entries are u32 PFNs. A 4K-aligned segment contributes `pa>>12, pa>>12+1, ...`.
More than sgl-entries pages panics "Too many segments".

**IOVA** = `virtual-base | tcb_index << 23`, which gives 0x40000000 + index × 8 MB.
The page count is encoded in IOVA bits 22:12 on unmap. (VERIFIED)
With cache-invalidate-workaround, a tag's descriptor owns TCBs `t` and `t+64`. For
**crypto I/O only**, the driver alternates between them on every map (separate toggles
for In and Out), so a TCB that may still be cached is never rewritten in place.
Non-crypto I/O always uses TCB `t`. (VERIFIED)

**Unmap / invalidate** (slot 171 @0xfffffff005eeb9e8 + sub_0xfffffff005eebc9c), VERIFIED:
```
tag = (iova >> 23) & 0x7f
tcb[tag].npages (+0x04) = 0;  dmb ish;  memset(tcb[tag], 0, 0x80)
[fifo-workaround 1: poll port0[0x22c] bit0 | mode 2: wait nvme-invalidate-delay]
nvmmu[0x04] = tag            // no completion poll afterwards
```
`iovmInsert` and `mapToPhysicalAddress` panic as unimplemented.

## Q5. NVMe side (IONVMeFamily, AppleEmbeddedNVMeController)

**Match** (personality, VERIFIED): `IOPCIMatch 0x2002106b` (vendor 0x106b, device
0x2002) plus `IOPCIClassMatch 0x01800200&0xffffff00`, with `IOPCIUseDeviceMapper`.

**ANS BAR registers** (accessor vtable 0x640 = read32, 0x638 = write32, 0x628 = write64), VERIFIED:

| reg | where | what |
|---|---|---|
| 0x1800 | `PreCCEnableHook` @…67800a0 → slot 272 @0xfffffff0067ad6ac | written before CC.EN: **0** for "Regular init", **3** for "Restore boot. Restricted init." |
| 0x180c | slot 318 @0xfffffff00677fa84 | read: scratch/HMB alignment (0 is treated as 1; must be a power of two) |
| 0x1808 | same | read: scratch/HMB size |
| 0x1810 (64-bit) / 0x1818 | sub_0xfffffff00677f9b4 | write: scratch buffer DMA address / size |
| 0x1824 / 0x1828 | slot 281 @0xfffffff00677faf4 | read: core mask / log size. Both must be != 0xffffffff. Log buffer size = (val << 12) + 0x1000. |

**Where the scratch buffer comes from** (slot 317 @0xfffffff00677eb08), checked in this order:
1. `nvme-scratch-virt-region` = `{u64 dva, u32 size}`, used as is.
2. `nvme-scratch-region` = physical `{base, size}`, wrapped with `withPhysicalAddress`
   and mapped through the device mapper (DART).
3. Otherwise a contiguous buffer of size 0x1808, aligned to 0x180c, mapped through
   DART.

The j71t IPSW ADT has `nvme-scratch-virt-region: 0`, a placeholder (INFERRED that iBoot
fills or removes it).

**L1SS / LTR.** IONVMeFamily has no code for these. IOPCIFamily handles them generically:
- `pci-l1pm-control`, sub_0xfffffff005eac4dc. The device's L1 PM Substates cap offset
  is at `reserved+0x12`. When the OSData is at least 8 bytes:
  `cfg[l1ss+0xc] = data.u32[1]` (CTL2) first, then
  `cfg[l1ss+0x8] = data.u32[0] & enableMask` (CTL1). Disabling writes 0 to both.
  (VERIFIED)
  For j71t: s3e 0x4044000f/0x0, pci-bridge0 0x4044190f/0x28, pci-bridge1 0x4044190f/0x10.
  These follow from reading the 8-byte LE properties (VERIFIED values from the ADT,
  INFERRED split). The value is **not** derived from a fixed 0x40550000. The A10 Linux
  constant probably came from a different ADT value.
- `pci-max-latency`, @0xfffffff005eb0228. The property is looked up recursively in the
  DT plane; `cfg[ltrCap + 4] = value` (0x10081008), then LTR is enabled through a
  bridge vfunc (0x6a0). (VERIFIED)
- Whether the S3E's L1SS cap is at 0x180 and its LTR cap at 0x178 (so 0x188/0x18c/0x17c)
  can't be checked statically; the A10 port's numbers suggest it (INFERRED).
- `pci-aspm-default` (2 = L1) is also an IOPCIFamily property.

**Not investigated:** the FlatDMA cdw0 BIT(5) flag in command submission, and
config 0x0d (latency timer) = 0x40.

## Q6. MSI

- `apcie` properties (AppleEmbeddedPCIE::start, VERIFIED): `msi-address` (8 bytes) →
  `+0xf8`, `msi-crypto-address` → `+0x100`, `#msi-vectors` (32) → `+0x108`,
  `msi-vector-offset` (256) → `+0x10c`, `msi-parent-controller` → AIC.
  `ApplePCIEMSIController(numVectors = 32, firstVector = 256, parent = AIC)` maps
  vector `v` to AIC interrupt `256 + v` (`deallocateInterrupt` computes
  `v = aicVector - firstVector`).
- Bridge properties (VERIFIED): `#msi-vectors` → port `+0x1d4`, `msi-vector-base` →
  port `+0x1d8`. An `IORangeAllocator` hands out `[base, base + n)`: port 0 gets 0..7,
  port 1 gets 8..15.
- Hardware (port slot 198, VERIFIED): `port[0x124] = 1 | log2(n) << 4` and
  `port[0x128] = base | base << 16`. j71t: port0 0x31 / 0x0, port1 0x31 / 0x00080008.
- Endpoint programming (AppleEmbeddedPCIE `callPlatformFunction`
  "GetMessagedInterruptAddress" @0xfffffff005ed456c, VERIFIED): MSI address =
  `msi-address` (0xbffff000), or `msi-crypto-address` for nodes with `crypto-queue-msi`.
  MSI data = the vector number.
- 0xbffff000 is above the DART window (0x80000000 + 0x3c000000) and above the SART
  limit (0xbff00000). I think it is an untranslated doorbell that must be used exactly
  as given (INFERRED).
