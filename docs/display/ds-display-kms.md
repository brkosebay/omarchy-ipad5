# Display (KMS) on iPad 5 / A9 j71t: what the hardware is, and what a real driver costs

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Reverse engineering of the A9 display controller for a minimal DRM/KMS driver to
replace simpledrm. Sources: the live ADT (`build/firmware/private/IODeviceTree.txt`,
text copy `docs/adt/j71t.txt`), the live iOS ioreg (`build/firmware/private/IOService.txt`),
and the iOS 16.7.10 j71t kernelcache (`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`)
via the shared Ghidra program (image base `0xfffffff005298000`).

Evidence marks used throughout:

| mark | meaning |
|---|---|
| **[V]** | verified in this study: read from the live device (ioreg/ADT), or a decoded kext function whose effect is unambiguous |
| **[I]** | inferred from context, naming, or a neighbouring verified fact; **not** confirmed on hardware |
| **[?]** | unknown, with the exact kernelcache address to look at next |

---

## 1. Bottom line

1. **A9 has no display coprocessor.** The pre-M1 "CLCD/ADBE" path is what this
   device uses: `disp0` (`compatible = "adbe0,s8000"`, `device_type =
   "display-subsystem"`) is driven **directly by an AP kext** — `AppleH8ADBE0.kext`
   (`AppleMobileADBE0`, `AppleCLCD`, `ADBE0Driver*`, `AppleH7DisplayPipe`) plus
   `IOMobileFramebuffer`'s `AppleDisplayPipe` inside `IOMobileGraphicsFamily.kext`.
   There is no display firmware and no display coprocessor: the "DCP" strings in
   the cache are HDCP (`DCP2AuthSession`), not the M1 display coprocessor. **[V]**
2. **The display pipeline is a normal MMIO block plus one DART.** `disp0` owns a
   1 MiB window at `0x206200000` and five 32 KiB windows at
   `0x206400000…0x206500000`; `dart-disp0` (`0x206300000`, `0x206304000`) is its
   IOMMU. The panel is **eDP** (`displayport,s8003` + `lpdp-phy,s8003` + `lcd,edp`),
   not MIPI-DSI. **[V]**
3. **iBoot leaves a fully running pipe**, including the panel, the eDP link and
   the CLCD timing generator, scanning out a 1536×2048 XRGB8888 buffer at
   `0x87c95c000` (12 MiB). iOS itself has a code path for this
   (`AppleMobileADBE0::startWithIBoot`, `gatherIBootSettingsFromBlocksRegisters`,
   `ADBE0Driver::getIBootTimings`). **[V]** (Linux dmesg + kext strings/functions)
4. **A minimal one-plane CRTC is feasible** and does not need any panel or eDP
   init: adopt the timing registers, write a new scanout base, take vblank from
   the display IRQ, and implement DPMS as "layer off + backlight off" while
   keeping `ps_disp0`/`ps_dp`/`dart-disp0` powered. **[I→V]** The per-pipe layer
   register file is now decoded (§5), which was the main unknown. What is still
   missing is the **exact latch/commit semantics** (does a direct write to the
   base-address register take effect at the next VBI, or must it go through the
   ADP parameter FIFO?) and **which of the five disp0 interrupts is VBI** (§6).
5. **The risk is not "the panel breaks"** — it is a black screen until reboot,
   because Linux cannot re-train the eDP link. Three existing facts make this
   recoverable: the boot is tethered anyway, SSH/USB gadget is independent of
   the display, and the DT already keeps `ps_disp0`/`ps_dp` `apple,always-on`
   (`docs/pcie/suspend.md`, patch 0043) — a KMS driver must preserve that.
6. **Hardware rotation is only X/Y flip.** The pipe has reflect bits, not a
   rotate block (`IOMFBSupportsXFlip`/`YFlip` = Yes, no rotation property) **[V]**.
   The good news: the panel's native scanout is already **portrait** 1536×2048
   (`DefaultStride` 6144 = 1536×4), so the tablet UI needs no KMS-level rotation.

---

## 2. Where the picture comes from today

```
iBoot ── CLCD timings + eDP link + panel on ──┐
                                              ▼
                 ADP pipe0 layer0 ──base/stride/fmt──► CLCD ──► lpdp-phy ──► panel
                        ▲                         (0x2064x0000?)   (0x206780000)
                        │ DART (bypass)                     ▲
                 DRAM 0x87c95c000 (12 MiB)          displayport 0x206700000
                        ▲
              simpledrm / CPU memcpy (Hyprland)
```

* Linux never touches `disp0`; `simple-framebuffer` is bound to `ps_disp0 & ps_dp`
  and simpledrm memcpys into the iBoot buffer (`docs/first-boot-dmesg.txt:140`,
  console `192x128` → 1536×2048 with an 8×16 font).
* Consequence today: no vblank, no page flip, no hardware blanking — the lid
  handler zeroes the backlight instead (`docs/omarchy/tablet-usage.md`) — and no
  flips.
* `ps_disp0`, `ps_dp`, `dart-disp0` being powered off *resets the pipe and the
  eDP link* — observed during suspend work, fixed by making them always-on.
  This is the single most important constraint for a KMS driver: **never power
  these domains down unless you can redo eDP link training (we cannot).**

---

## 3. The controller: ADBE0 / AppleCLCD

`disp0`'s ADT compatible is `adbe0,s8000`; iOS matches it with
`AppleMobileADBE0` (bundle `com.apple.driver.AppleH8ADBE0`, prelink
`0xfffffff00539e700` + `0x11264`). The class inventory (from the cache's RTTI
strings, all in that kext) shows the structure:

| class | role |
|---|---|
| `AppleMobileADBE0` | `IOService` for `disp0`: power state machine, backlight, swaps, PLCH/ALSS handlers, interrupt setup |
| `AppleDisplayPipe` / `AppleH7DisplayPipe` | the pipe: layer composition, scaling, swap/verify, parameter FIFO packets |
| `AppleCLCD` | CLCD timing generator block (timings, gamma, dither, dark enhancer, KSF power-on) |
| `ADBE0Driver` / `ADBE0Driver_Impl` / `ADBE0H8Driver` | register-level driver: block enable/state, timings, panel mode |
| `ADBEParamFIFO` / `ADPParamFIFO` / `ADBEParamFIFODriver` / `ADBEParamFIFOHalV1` | the update FIFO used for atomic register updates |
| `IOMFBColorManagerDriverH7`, `DitherDriverH6`, `DPBDriverV1`, `AAPDriverH8`, `MIEDriver`, `SPLC` | colour/LUT, dither, dynamic pixel backlight, ambient-adaptive pixel, image enhancement |
| `IOMobileFramebuffer` / `IOMobileFramebufferLegacy` | the framebuffer service + user-client ABI (`IOMobileFramebufferUserClient`; 4 live instances from `backboardd`/`thermalmonitord`) |

Key verified entry points (kernelcache addresses, image base `0xfffffff005298000`):

| function | address | why it matters |
|---|---|---|
| `AppleMobileADBE0::start_hardware` | `0xfffffff005d29d2c` | creates `ADBE0Driver`, then `startWithIBoot()` or the ATV path |
| `AppleMobileADBE0::startWithIBoot` | `0xfffffff005d2a84c` | reads timings from hardware (`ADBE0Driver` vtable `+0x1e0`) or `display-timing-info`, then `gatherIBootSettings…` |
| `AppleMobileADBE0::gatherIBootSettingsFromBlocksRegisters` | `0xfffffff005d2f4a4` | recovers the per-block state iBoot left |
| `AppleMobileADBE0::do_power_state_change_gated` | `0xfffffff005d2c074` | the DPMS state machine (soft = panel, hard = power gate) |
| `AppleMobileADBE0::swap_begin_gated` | `0xfffffff005d2b080` | swap bookkeeping |
| `AppleH7DisplayPipe::program_hardware_gated` | `0xfffffff005d20858` | builds and pushes the per-frame register set |
| `AppleH7DisplayPipe::programFifoPacket_gated` | `0xfffffff005d21474` | packet build + `ADBEParamFIFODriver::programFIFO` |
| `AppleH7DisplayPipe::fifoCache` | `0xfffffff005d20624` | the single "write a register" primitive (direct path when pusher == NULL) |
| layer programming (unnamed, called from `program_hardware_gated`) | `0xfffffff005d219fc` | **contains the whole ADP layer register map** (§5) |
| gamma/LUT programming (unnamed) | `0xfffffff005d23c38` | `0x40b4/0x40b8/0x40e8` LUT registers |
| `ADBE0H8Driver::programMidporch_gated` | `0xfffffff005d3dde4` | CLCD mid-porch timing writes (`0x58/0x5c/0x60`, then `0x04` bit 15) |
| `ADBE0H8Driver::setBlockState` | `0xfffffff005d3dfe4` | block enables in register `0x04` (bits 31 / 30 / 29:28) |
| `AppleMobileADBE0::initBEInterrupts` | `0xfffffff005d2fa3c` | two filter interrupt sources; filter `0xfffffff005d2ff00`, action `0xfffffff005d30030` |
| `AppleMobileADBE0::initTconESDInterrupt` | `0xfffffff005d2fbe8` | third (panel ESD) interrupt source |
| `ADPParamFIFO::programFIFO` / `IOMFBParamFIFO::programFIFO` | `0xfffffff005d422a8` / `0xfffffff005d418d8` | FIFO kick (delegates to a block object's vtable `+0xe0`) |

Note on addresses: `_PrelinkExecutableLoadAddr` (`0xfffffff00539e700`) is where
the kext *was* prelinked; its code actually executes from the relocated range
`0xfffffff005d1xxxx–0xfffffff005d4exxxx`. All addresses above are the relocated
ones that Ghidra shows.

---

## 4. MMIO map

`disp0`'s `reg` in the ADT is six ranges; the live ioreg `IODeviceMemory` gives the
decoded physical addresses (ADT text prints them relative to the arm-io base
`0x200000000`):

| # | phys base | size | what | evidence |
|---|---|---|---|---|
| 0 | `0x206200000` | `0x100000` | ADBE main / global control; the CLCD timing block is **[I]** inside one of these windows | **[V]** ioreg `IODeviceMemory`; ADT `reg: 0x6200000 sz=0x100000` |
| 1 | `0x206400000` | `0x8000` | ADP pipe window A (?) | **[V]** address; **[I]** role |
| 2 | `0x206440000` | `0x8000` | ADP pipe window B (?) | **[V]** address; **[I]** role |
| 3 | `0x206480000` | `0x8000` | block window (?) | **[V]** address; **[I]** role |
| 4 | `0x2064c0000` | `0x8000` | block window (?) | **[V]** address; **[I]** role |
| 5 | `0x206500000` | `0x1c` | 7-register side block (?) | **[V]** address; **[I]** role |

Note: iOS maps each *sub-block* as a 256-byte aperture at the start of one of
these ranges — `Block::init` (`0xfffffff005d3d3a0`) calls
`mapDeviceMemoryWithIndex(index, 0x100)` and stores the physical address. That is
why the CLCD registers seen in code are `0x00–0x7f`: they are offsets inside a
block aperture, not inside the whole window. **[V]**

Surrounding blocks (all **[V]** from ADT + ioreg):

| block | base | IRQs | gates |
|---|---|---|---|
| `dart-disp0` | `0x206300000` (regs), `0x206304000` (tables), 16 KiB each | 141, 142 | pg 36 (0x24), cg 36 |
| `displayport` (`displayport,s8003`) | `0x206700000` | 149 | pg/cg 114 (0x72) |
| `lpdp-phy` (`lpdp-phy,s8003`) | `0x206780000` | — | — |
| `disp0` | see above | 140, 143, 144, 145, 146 | pg 112 (0x70); cg 112, 118, 116, 115, 117 |

Facts a driver needs, all **[V]** from the live ioreg (`AppleMobileADBE0` properties):

| property | value | meaning for KMS |
|---|---|---|
| `PixelClock` | 360 MHz | pipe pixel-clock limit/config; the live rate is iBoot's CLCD timing — never program it |
| `VideoClock` | 200 MHz | ADP video clock |
| `DisplayPipePlaneBaseAlignment.PlaneBaseAlignmentLinear` | 64 | scanout base must be 64-byte aligned |
| `…LinearX_Alignment` / `…StrideLinearHorizontal` | 64 | stride multiple of 64 |
| `…DefaultStride` | 6144 | 1536 × 4 ⇒ native mode is 1536 wide, 32 bpp |
| `IOMFBMaxSrcPixels.MaxSrcRectWidth` | 2048 | source rect limit |
| `IOMFBSupportsXFlip` / `YFlip` | Yes / Yes | reflect-x/y only, no rotation |
| `IOMFBDisplayRefresh` min = max interval | 71586818 | fixed refresh, no VRR |
| `ADP is generic pipe` | Yes | pipe programming is the generic ADP path |
| `DisplayPipeStrideRequirements` | linear H 64, tiled H 256, tiled V 16 | tiled formats exist but we do not need them |

**DART.** `dart-disp0` is a `dart,s8000`, 4 KiB pages, `stt-idx-width` 10,
`bypass-address = 0x80000000800`, and it sits between the pipe and DRAM.
The iBoot framebuffer at `0x87c95c000` is read by the pipe today, and the CPU
writes to the same physical address produce pixels, so at boot the DART is
either bypassed (identity) or has that buffer mapped. **[V]** address;
**[I]** bypass. This matters because mainline `apple-dart` supports a
`tcr_bypass` mode (`apple_dart_hw_enable_bypass()`), so "keep bypass, use
physical addresses" is a legitimate first implementation, and translation can
be added later.

---

## 5. Register map

### 5.1 ADP pipe registers — verified

The whole per-layer set comes from the single function at
`0xfffffff005d219fc` (called from `AppleH7DisplayPipe::program_hardware_gated`),
which funnels every write through `fifoCache(this, pusher, reg, value, mask, cb)`
(`0xfffffff005d20624`). Offsets are relative to the **ADP block base** — one of
the `disp0` `reg` windows, most likely the 32 KiB window at `0x206400000`
(globals `0x0000–0x3fff`, pipe0 `0x4000–0x5fff`, pipe1 `0x6000–0x7fff`) or
`0x206200000 + 0x4000`; D0.1 below settles it. The two pipes are 0x2000 apart
(`reg = pipe * 0x2000 + …`, `pipe ∈ {0,1}`):

| offset (pipe 0) | width | iOS use | meaning | evidence |
|---|---|---|---|---|
| `0x402c` | 32 | `fmt \| flags` | pipe control/format: `0xc6000` or `0xe4000` for the two 32-bpp orders, plus tiling/planar bits (21, 22, 24–25, 26–27), bit 0 set for some CSC modes | **[V]** write site; **[I]** bit meanings |
| `0x4030 + 4*layer` | 32 | `addr & ~0x3f \| 0x2a` | **plane base address** (low 6 bits flags) | **[V]** write site + alignment check; **[I]** exact low-bit encoding |
| `0x4038 + 4*layer` | 32 | `stride & ~0x3f \| 0x2a` | **plane stride / plane offset** (second 32-bit value of the pair) | **[V]** write site; **[I]** which of the pair is stride |
| `0x4040`, `0x4048` | 32 | `lo&0x3fff \| hi<<16 \| 0x80008000` | source/destination rectangle pair (16-bit fields, 0x8000 = "enabled") | **[V]** write site; **[I]** rect semantics |
| `0x4050`, `0x4054` | 32 | `x0\|y0`, `x1\|y1` | destination window (14/15-bit fields) | **[V]** write site |
| `0x4058 … 0x4064` | 32 | `0x3ff03ff` masked | 10-bit triples — background/border colour | **[I]** |
| `0x4068` | 32 | `0xaaaaaba0 \| (1<<n)` per active layer | **layer enable mask** (low nibble = layers, upper bits fixed) | **[V]** write site |
| `0x406c + 4*layer` | 32 | `x \| y<<16` | layer position (flip-adjusted) | **[V]** |
| `0x407c + 4*layer` | 32 | `h<<16 \| w`, 15-bit | layer size | **[V]** |
| `0x4094` | 32 | 16.16 fixed point | horizontal scale factor | **[V]** |
| `0x40a4` | 32 | 16.16 fixed point | vertical scale factor | **[V]** |
| `0x40b4` | 32 | `0xaaaaaba0` / `0xaaaaaaab` | gamma enable/descramble | **[I]** |
| `0x40b8 + 4*i`, `0x40e8 + 4*i` | 32 | 3× `0x7ff` fields | per-channel LUT entries (3 sub-units) | **[V]** write site |
| `0x40c4`, `0x40d0`, `0x40dc` (+0/4/8) | 32 | triples | additional LUT words | **[V]** |
| `0x5000`, `0x5800` | block | mask `0x120` | LUT upload windows (0x800 apart) | **[V]** |

The X/Y reflect bits are consumed right here: `local_fc >> 1 & 1` mirrors x,
`local_fc >> 2 & 1` mirrors y when computing the layer position. **[V]**

### 5.2 CLCD / main window — partially verified

From `ADBE0H8Driver::programMidporch_gated` (`0xfffffff005d3dde4`) and
`setBlockState` (`0xfffffff005d3dfe4`), register offsets inside *that block*:

| offset | use | evidence |
|---|---|---|
| `0x04` | block control: bit 31 = MIE/ambient-pixel, bit 30 = ADBE0 colour manager, bits 29:28 = dither; bit 15 set at the end of the mid-porch sequence | **[V]** read-modify-write in `setBlockState` |
| `0x50` | written with 0 before the porch loop | **[V]** |
| `0x58` | porch end (16-bit) | **[V]** |
| `0x5c`, `0x60` | two "mid-porch region" start/length pairs (16-bit start + 16-bit length each) | **[V]** |

`ADBE0Driver` vtable slots `+0x1d0` (set timings) and `+0x1e0` (get iBoot timings)
are the accessors iOS uses; they are reached through the object created by
`ADBE0Driver::createDriver` (`0xfffffff005d426bc`). **[V]** call sites,
**[?]** the timing register block layout beyond 0x50–0x60.

**A Linux driver never writes these** (§7).

### 5.3 Not yet decoded

| what | where to look |
|---|---|
| which reg index (0…5) is CLCD vs ADP vs the small block | `Block::init` `0xfffffff005d3d3a0` calls `(*profile+0xa8)(profile, blockType)`; that profile method is the block→DT-reg-index table |
| the FIFO ring registers (base/size/rd/wr ptr, kick) | `ADPParamFIFO::programFIFO` `0xfffffff005d422a8` → `IOMFBParamFIFO::programFIFO` `0xfffffff005d418d8` → block vtable `+0xe0`; object created in `ADBEParamFIFODriver::init` `0xfffffff005d42a54` → `0xfffffff005d4d1c4` |
| interrupt status/ack/mask registers | `ADBE0Driver` vtable `+0x148` (check), `+0x150` (ack), `+0x1f0` (cause), used by the filter at `0xfffffff005d2ff00` |
| latch/commit semantics for `0x4030` | experiment, or the FIFO completion action (`queueCommand`, `setCompletionAction`) |

---

## 6. Interrupts and vblank

`disp0` has five DT interrupts — **140, 143, 144, 145, 146** (`0x8c, 0x8f, 0x90,
0x91, 0x92`) on the AIC (`interrupt-parent` phandle 22). `dart-disp0` takes the
two in between (141, 142), which is why the display's list is not contiguous.
**[V]** (ADT + ioreg `IOInterruptSpecifiers`).

iOS installs **two** `IOFilterInterruptEventSource`s in
`AppleMobileADBE0::initBEInterrupts` (`0xfffffff005d2fa3c`), each with a filter
(`0xfffffff005d2ff00`, reads a cause, calls check/ack on the `ADBE0Driver`
object) and a deferred action (`0xfffffff005d30030`, which calls into the pipe
and advances the power-gate state machine), plus a third, separate one for
panel ESD in `initTconESDInterrupt` (`0xfffffff005d2fbe8`). The interrupt
indices it asks the provider for are 1, 16 and 18, which do **not** map onto the
five-entry `interrupts` array — so the provider's `getInterruptNumber()` is **[I]**
a per-SoC table, not a DT lookup. **[V]** (call sites), **[?]** (the table).
On Linux the practical route is: request all five in order, log which one fires
while the pipe is running, and pick the one that fires once per frame.

Vblank consumers: `AppleDisplayPipe::post_vbi_timer_action_gated`
(`0xfffffff005d33834`) and `IOMFBReporter::updateTotalVBI`
(`0xfffffff005d43c1c`) — a "VBI" (vertical blanking interval) counter exists and
is reported; the pipe's frame counter used for FIFO completion is a 16-bit value
at pipe object offset `+0x1720` (incremented in `0xfffffff005d24d1c`). **[V]**

---

## 7. Page flip, DPMS, backlight

### 7.1 How iOS swaps

`swap_begin_gated` → `program_hardware_gated` → per-layer register set (§5.1)
queued with `fifoCache` into the ADP parameter FIFO → `programFifoPacket_gated`
→ `ADBEParamFIFODriver::programFIFO` → the FIFO is kicked and the hardware
applies the queued writes in the next blanking interval; a completion action
(queued with `setCompletionAction`) produces the swap-done notification.
`fifoCache` has a **direct path** when no pusher is passed (`pusher == NULL`),
i.e. the same register writes can be issued immediately, without the FIFO.
**[V]** (all four functions decoded).

### 7.2 Minimum sequence for each operation

**Adopt the running pipe (probe, no writes):**

```
reg = read32(ADP + 0x402c)              # format/tiling/flip
base = read32(ADP + 0x4030) & ~0x3f     # current scanout base (iBoot FB)
stride = read32(ADP + 0x4038) & ~0x3f   # 6144 expected
en = read32(ADP + 0x4068)               # layer enable mask
```

**Page flip to a new buffer** (single plane, 32 bpp, same geometry):

```
new = new_phys                                            # or IOVA if the DART translates
write32(ADP + 0x4030, (new & ~0x3f) | (base & 0x3f))      # layer 0 base, flag bits kept
write32(ADP + 0x4038, stride)                             # only if it changed
write32(ADP + 0x4068, en)                                 # re-arm; may be the latch  [I]
wait_for_vblank()                                         # see below
```

If a direct write does not latch, the fallback is the FIFO: enqueue
`{0x4030, base}`, `{0x4038, stride}`, a completion action, then kick. **[?]**

If the DART is later put in translating mode, the value written to `0x4030` is
the **IOVA** the buffer was mapped at (dma-buf / `iommu_map`), not the physical
address; with bypass it is the physical address. Nothing else changes. **[I]**

**Vblank:**

```
# once, at probe: request the five disp0 IRQs, count them for a second,
# keep the one that ticks at ~60 Hz (the others are error/ESD/FIFO events)
cause = read32(DRV + status_reg)     # [I] ADBE0Driver vtable +0x1f0
write32(DRV + ack_reg, cause)        # [I] vtable +0x150 (check is +0x148)
drm_crtc_handle_vblank(crtc)         # -> page-flip completion
```

Fallback if the IRQ cannot be identified: a 60 Hz hrtimer driving
`drm_crtc_handle_vblank()`. That costs precision but keeps flips and fences
working, and it is what the driver should ship with first.

**DPMS off (standby):**

```
backlight = 0                    # lp8559 on i2c0 (already in Linux)
write32(ADP + 0x4068, en & ~0xf) # clear the layer mask, keep the fixed bits
# alternative: disable the CLCD block (0x04 in the timing block) - [I], risks the link
# do NOT power-gate ps_disp0 / ps_dp / dart-disp0
```

**DPMS on:** restore `0x4068` / base, then `backlight = saved`.
This mirrors iOS: `do_power_state_change_gated` has an explicit
"Turning off panel, leaving CLCD on" path (string at `0xfffffff0053a30a0`,
function `0xfffffff005d2c074`), and full power-down
(`powerDownHardware_gated`, `0xfffffff005d2c848`) is a separate, harder state.
**[V]**

**Backlight:** the `backlight` node's `function-backlight_enable` resolves to
`bklE` on the LP8559 (`i2c0`, address 0x26) and `function-backlight_update` to
`dwiB` (PMGR "display window interface"), with `function-disp_aap_ctrl` going
back into `disp0` (`ADPa`). Linux already has working brightness
(`brightnessctl`), so a KMS driver should just link the existing backlight
device and turn it off in DPMS off. **[V]** (ADT), **[I]** (which Linux driver
binds today — it works, so do not disturb it).

### 7.3 What must never be touched

| block | why |
|---|---|
| `displayport` `0x206700000`, `lpdp-phy` `0x206780000` | eDP link is trained by iBoot; Linux has no training code and no panel driver |
| CLCD timing registers `0x50–0x60` in the main window | re-programming loses sync with a panel we cannot re-initialise |
| PMGR domains `ps_disp0`, `ps_dp`, `dart-disp0` | power-off resets pipe **and** link (already seen, patch 0043) |
| DART `tcr` if you keep bypass | flipping modes mid-scanout faults the pipe |

---

## 8. Is a minimal KMS driver feasible?

**Yes, for a one-plane, one-CRTC, fixed-mode, page-flip + vblank + DPMS driver,
without touching panel/eDP init.** The pieces:

| KMS piece | plan | risk |
|---|---|---|
| mode | one fixed mode, read from `0x402c/0x4030/0x4038` at probe (fallback 1536×2048@60, XRGB8888, stride 6144) | low |
| memory | `drm_gem_dma_helper` (DMA-API contiguous / CMA), 64-byte-aligned base, stride multiple of 64 | low; 12 MiB × 3 buffers on a 2 GB device |
| CRTC enable | do **not** program timings; the pipe is already on. "Enable" = layer enable mask | low |
| page flip | `atomic_flush` writes base/stride (§7.2), completion at next vblank | **medium** — latch semantics unknown |
| vblank | request the disp0 interrupts, identify the VBI one; fallback: hrtimer at 60 Hz | medium — IRQ id unknown |
| DPMS | layer off + backlight off, keep domains powered | low |
| rotation | expose reflect-x/y; 90° stays in userspace (Hyprland transform) | low |
| DART | phase 1: bypass (physical addresses, no `iommus` in DT); phase 2: real IOVA | medium (phase 2) |
| handover | `drm_aperture_remove_conflicting_devices()` + drop `simple-framebuffer`, or keep simpledrm and let DRM take over the same memory | low |

> Private graphics-dependent excerpt omitted from this source snapshot.

**Why it can go wrong:** one bad write to the CLCD timing or to the eDP block
leaves a black panel with no recovery but reboot (tethered boot anyway), and a
wrong DART/latch guess can hang the pipe. Mitigations: read-only probe first,
auto-revert watchdog in the flip test module, never touch domains, and keep the
UART/USB gadget path as the console of last resort.

---

## 9. Implementation plan

### D0 — finish the register archaeology (offline, 2–4 h)

1. Block→window table: decompile the `IOMFBSoCProfile` method at vtable `+0xa8`
   (called from `Block::init`, `0xfffffff005d3d3a0`); record which of the five
   32 KiB windows is CLCD, which is ADP, and what the 28-byte block is.
2. FIFO registers: follow `ADBEParamFIFODriver::init` (`0xfffffff005d42a54`) →
   `0xfffffff005d4d1c4` → the block object's vtable `+0xe0` used by
   `IOMFBParamFIFO::programFIFO` (`0xfffffff005d418d8`); write down base/size/
   read/write pointer/kick registers and the packet word format
   (`queueCommand`, `setCompletionAction`).
3. Latch semantics: read `AppleMobileADBE0::swap_begin_gated`
   (`0xfffffff005d2b080`) and the pipe's flush path for any "shadow → active"
   register (a `0x4080`-range or a bit in `0x402c`/`0x4068`).
4. Interrupts: read the `ADBE0Driver` vtable slots `+0x148/+0x150/+0x1f0`
   (implementations are the `ADBE0Driver_Impl` methods around
   `0xfffffff005d407e0`–`0xfffffff005d40ec0`) to get the status/ack register
   offsets and bit meaning; then match to IRQ 140/143/144/145/146.

Deliverable: extend §5/§6 of this file. No device access needed.

### D1 — read-only probe module (device, safe)

* New DT node `disp0@206200000` (`compatible = "apple,adbe0"`), six `reg`
  ranges, five `interrupts`, `power-domains = <&ps_disp0 &ps_dp>`; remove
  `apple,always-on` only once the driver keeps them on itself.
* `probe()`: `devm_ioremap` all six windows, dump the registers in §5 to dmesg
  once, decode mode/stride/format, and **write nothing**.
* Exit criterion: decoded mode is 1536×2048, stride 6144, base = the address
  simpledrm uses (`0x87c95c000`); no visual change; no genpd warnings.

### D2 — flip experiment (device, risky, auto-reverting)

* Allocate one 12 MiB CMA buffer, fill with a recognisable pattern (e.g. four
  colour bars + a moving pixel), program `0x4030`/`0x4038` directly, wait two
  frames, and restore the original base after 10 s unless a module parameter
  says otherwise (a `delayed_work` auto-revert; also revert on module unload).
* Record: does the image appear? Does it tear? Does the value read back from
  `0x4030` change? This single test decides "direct write" vs "FIFO".
* If direct writes do not latch: implement the FIFO path from D0.2 and repeat.

### D3 — DRM/KMS skeleton

* `drm_simple_display_pipe` (or a small custom CRTC), one primary plane,
  `DRM_FORMAT_XRGB8888`, `drm_gem_dma_helper` + `drm_fbdev_dma`.
* `atomic_flush` → §7.2 flip; `enable_vblank`/`disable_vblank` on the confirmed
  IRQ (hrtimer fallback behind a module parameter).
* DPMS: layer mask + backlight; `atomic_disable` must leave the pipe scanning
  out a valid buffer (or the layer disabled) — never power-gate.
* Handover: `drm_aperture_remove_conflicting_devices()` so simpledrm releases
  the iBoot buffer; keep fbcon via `drm_fbdev_dma`.
* Exit criterion: Hyprland runs with `renderer=software` on the new CRTC, no
  tearing on a scrolling page, `vblank` counted at ~60 Hz, `dpms off` blanks
  the panel and backlight, `dpms on` restores.

### D4 — polish (optional)

* DART translation (`iommus = <&dart_disp0 0>`), so buffers need not be
  contiguous; keep bypass behind a DT property until proven.
* reflect-x/y plane properties; debugfs register dump; runtime PM that only
  gates the ADP front-end, never the link.
* `drm_panic`/`drm_client` for crash messages, so a panic is visible.

---

## 10. Open questions (with where to look)

| question | address |
|---|---|
| block type → DT reg index table | `(*IOMFBSoCProfile+0xa8)`, from `Block::init` `0xfffffff005d3d3a0` |
| which IRQ is VBI, and its status/ack bits | `ADBE0Driver` vtable `+0x148/+0x150/+0x1f0`; filter `0xfffffff005d2ff00`, action `0xfffffff005d30030` |
| does `0x4030` latch at VBI or need a shadow commit | `AppleMobileADBE0::swap_begin_gated` `0xfffffff005d2b080`; pipe flush in `0xfffffff005d20858` |
| FIFO ring registers and packet encoding | `IOMFBParamFIFO::programFIFO` `0xfffffff005d418d8`, `ADPParamFIFO::programFIFO` `0xfffffff005d422a8`, `queueCommand` |
| CLCD timing register layout (beyond `0x50–0x60`) | `ADBE0Driver` vtable `+0x1d0`/`+0x1e0`, reached from `startWithIBoot` `0xfffffff005d2a84c` |
| what the 28-byte window at `0x206500000` is | unknown; dump it in D1 |

---

## 11. Evidence index

| claim | source |
|---|---|
| `disp0` = `adbe0,s8000`, display-subsystem, 6 reg ranges, 5 IRQs | `docs/adt/j71t.txt:2490`; ioreg `IOService.txt:8188` (`IODeviceMemory`, `interrupts`) |
| decoded physical bases `0x206200000…0x206500000` | ioreg `IODeviceMemory` (8692695040 = 0x206200000 …) |
| `dart-disp0` at `0x206300000/0x206304000`, 4 KiB, bypass `0x80000000800` | ioreg `IOService.txt:8323`; `docs/adt/j71t.txt:2512` |
| eDP panel (`lcd,edp`), `displayport` `0x206700000`, `lpdp-phy` `0x206780000` | `docs/adt/j71t.txt:2466` |
| iBoot FB at `0x87c95c000`, 1536×2048 | `docs/first-boot-dmesg.txt:140` (console 192×128) |
| domains `ps_disp0`/`ps_dp` must stay on | `docs/pcie/suspend.md` (patch 0043, "Panel black after any genpd stage") |
| ADP layer register map `0x402c…0x40a4`, `0x4068` enable, flips | `0xfffffff005d219fc` (writes via `fifoCache` `0xfffffff005d20624`) |
| CLCD `0x04/0x50/0x58/0x5c/0x60` | `0xfffffff005d3dde4`, `0xfffffff005d3dfe4` |
| swap → FIFO → completion | `0xfffffff005d2b080`, `0xfffffff005d20858`, `0xfffffff005d21474`, `0xfffffff005d422a8` |
| DPMS state machine, "panel off, CLCD on" | `0xfffffff005d2c074`, `0xfffffff005d2c848`, string `0xfffffff0053a30a0` |
| iBoot-timings adoption path | `0xfffffff005d2a84c`, `0xfffffff005d2f4a4` |
| no display coprocessor | no DCP kext/strings in the cache (only HDCP "DCP2…"), `AppleH8ADBE0` is a plain `IOService` under `disp0` |
| no VRR, fixed refresh, flips only | ioreg properties of `AppleMobileADBE0` (`IOReportLegend`-adjacent block, `IOService.txt:8215`) |

Reproduction helpers used for this study (uncommitted, under `build/re/`):
`kc_dump.py` (Ghidra `read_memory` → file) and `method_map.py` (maps
`"virtual T C::m(...)"` strings back to the function that references them —
the assert strings live inside their own methods, which is how the display
kext's method table was recovered).

---

## 12. D0 resolved: the latch, the VBI IRQ, and the flip-completion register

Closes §10 rows 2 (`0x4030` latch) and 3 (which IRQ is VBI + ack/mask), plus the
"register to confirm a flip completed". Same source corpus as §1–§11 (iOS
16.7.10 j71t kernelcache, image base `0xfffffff005298000`). Read-only study;
nothing was written to the device.

### 12.1 Bottom line

1. **There is no latch bit, no "go"/"swap" bit, and no double-buffer select in
   the pipe register file.** The commit is the **ADP parameter FIFO kick**:
   iOS writes `0x402c/0x4030/0x4038/…` into a software ring
   (`queueCommand`), flushes it with `programFIFO` (`0xfffffff005d422a8`),
   the hardware applies the whole packet at the next frame boundary and raises
   **status bit `0x8` ("packet synced")**. **[V]**
2. **A direct write to `0x4030 + 4i` is a plain register store with no commit
   step after it.** iOS never does it while the pipe scans out — every
   steady-state write carries a non-NULL `ADPParamFIFO::Pusher`, which routes
   it into the FIFO. **[V]** Whether the store lands immediately (tearing) or is
   picked up by the pipe's own frame-boundary logic is **not decidable
   statically [?]**; D2 measures it, and it is safe either way (it cannot
   corrupt the buffer or the link). Implement the FIFO path.
3. **VBI is bit `0x1` of the display pipe's own status word `ADP+0x102c`**, and
   iOS reaches that word from the interrupt source it creates with provider
   index **0**. Enable mask `ADP+0x1028`; ack = write the handled bits **back to
   `0x102c`** (W1C). **[V]** for the source, the bit and the ack; **[I]** for
   "index 0 ⇒ ADT `interrupts[0]` ⇒ AIC 140" (it assumes
   `AppleARMIODevice::getInterruptNumber(n) == interrupts[n]`, which D1 confirms
   by counting). IRQ 143 (index 1) is the `AppleMobileADBE0` back-end source;
   indices 16 and 18 **do not exist** on this DT (5 interrupts), so iOS silently
   skips those two sources here. **[V]** (guarded by `if (n == -1) skip`).
4. **Flip completion is `ADP+0x102c` bit `0x8`.** iOS's wait loop polls it with
   a 1 ms sleep and drops the swap if it never appears. `ADP+0x1034` is a
   16-bit frame counter sampled on every VBI. **[V]**

### 12.2 Correction to §4/§5: one window holds ADPCONTROL *and* both pipes

`AppleDisplayPipe` reaches every register through one helper pair — read
`0xfffffff005d200b8(pipe, off)` → `*(u32*)(*(pipe+0x9d30) + off)` and write
`0xfffffff005d20000` — and the *same* base is used for `0x102c` (status),
`0x1034` (frame counter) **and** `0x4030`/`0x4038` (plane base/stride). So the
"ADP control" registers at `0x1000…` and the two pipe banks at `0x4000`/`0x6000`
are in **one** mapping, i.e. one ≥32 KiB window; `pipe*0x2000` offsets therefore
belong to that same window. **[V]** (`0xfffffff005d1f614` reads `0x1038` and
then `0x4030+4i`/`0x4038+4i` through the same accessor.)

The base pointer is `*(pipe + 0x9d30)` = `getVirtualAddress()` of the map made
in `AppleH7DisplayPipe::start_hardware` (`0xfffffff005d1e6dc`) by
`(*(*provider + 0x3d0))(provider, 0, 0x100)`. **Which of the six DT ranges that
is remains [?]** — `0x206200000` (index 0 of the 1 MiB window) is the leading
candidate; §12.7 gives a one-shot read-only discriminator.

### 12.3 The ADP control register file (`ADP+0x1000…0x1080`) — new, verified

Relative to the same base as §5.1. Offsets and accessors are **[V]** (decoded
write/read site plus the literal); the *meaning* column is **[V]** where iOS
itself names the field, **[I]** otherwise.

| offset | access | what iOS uses it for |
|---|---|---|
| `0x1000` | R/W | ADP soft control; `0xfffffff005d36bd4` writes `0x40` **[I]** |
| `0x1004` | R | FIFO ready, bit 0 = "can take a word" (`0xfffffff005d42574`) **[I]** |
| `0x1008` | R | FIFO state, 16-bit (`0xfffffff005d4250c`, called from `ADBEParamFIFODriver::init`) |
| `0x100c` | W | FIFO **header**: `value & 0x3fffff` = target register offset (`0xfffffff005d42530`) |
| `0x1010` | W | FIFO **data**: the 32-bit value for that register (`0xfffffff005d42530`; `0xfffffff005d19d38` writes `1`) |
| `0x1014` | R | **SPDS version**, low 16 bits must read `0x13` (`0xfffffff005d1f7f4`) |
| `0x1018` | R | ADP configuration word, read once and cached (`0xfffffff005d25070`) |
| `0x101c` | R | `ADPCONTROL_ADPCSTATE`; **bits 7:4 = `ParmSt`**, 6 legal values, anything else panics `"undefined ADP_ADPCONTROL_ADPCSTATE.ParmSt"` (`0xfffffff005d19a64`) |
| `0x1028` | W | **interrupt enable mask** — only writer in the whole kext is `0xfffffff005d1fe8c` |
| `0x102c` | R/W | **interrupt status, W1C** — read `0xfffffff005d1f8b0`, written back `0xfffffff005d20000` |
| `0x1034` | R | 16-bit **frame counter** — read on every VBI and at `start_hardware` |
| `0x1038` | R | ADP state-valid flags; bit 0 = "pipe state usable", bit 4 checked by the backlight path (`0xfffffff005d1f614`, `0xfffffff005d18f90`) |
| `0x1040` | R | FIFO write pointer; low 13 bits must be 0 at a packet boundary (`readCurrentParamFIFOState`) |
| `0x1048` | W | written `0x11` during ADP bring-up (`0xfffffff005d19d80`) |
| `0x1050…0x1060` | R | FIFO/ADP tunable dump (`0xfffffff005d1e0c4`) |
| `0x1064`,`0x1068` | R | per-VBI counter pair A (stored as one 64-bit sample) |
| `0x106c`,`0x1070` | R | per-VBI counter pair B (second 64-bit sample) |

Note this is a *different* register file from the CLCD block (`0x04/0x50/0x58/
0x5c/0x60` from §5.2; gamma ramp banks at `0x3000/0x3400/0x3800` and memcpy LUT
windows at `0x4000/0x4400/0x4800/0x4c00` + `0x5000/0x5400/0x5800`, all in
`0xfffffff005d4c390` / `0xfffffff005d4cc48` on `AppleCLCD`'s own base). **The two
blocks use overlapping offsets**, so they cannot be the same window — one base
per block. The D1 discriminator is unambiguous: the ADP window must read
`0xc6000`/`0xe4000` at `+0x402c` **and** a frame counter that advances at
`+0x1034`; a CLCD ramp port satisfies neither.

### 12.4 Gap 1 — how iOS updates plane state, in its own call order

**Every** plane register write in the steady state goes through one primitive,
`AppleH7DisplayPipe::fifoCache(pipe, pusher, reg, value, mask, cb)`
(`0xfffffff005d20624`). With a non-NULL `pusher` it is `queueCommand`
(`0xfffffff005d4237c` / `0xfffffff005d42398` → ring append
`0xfffffff005d42200`); with `pusher == NULL` it stores straight into the block
(`(*(*pipe+0x9d70)+0x540)` = `0xfffffff005d42034`). **[V]**

The layer set is built by `0xfffffff005d219fc`, called from
`AppleH7DisplayPipe::program_hardware_gated` (`0xfffffff005d20858`) via
`0xfffffff005d20d78`, and every call passes `&local_pusher`
(`0xfffffff005d41b28` constructs it) — i.e. **always the FIFO**. **[V]**

Exact order inside `0xfffffff005d219fc` (`uVar6 = pipe * 0x2000`, mask arg is
`1` on every write):

```
uVar6 = param_3 * 0x2000
fifoCache(0x402c+uVar6, fmt|flags, 1)                 # 0xc6000 or 0xe4000
for layer i in 0..n-1:
    fifoCache(0x4038+4i, bytes_per_row & ~0x3f | 0x2a, 1)   # stride / plane offset
    fifoCache(0x4030+4i, base        & ~0x3f | 0x2a, 1)     # plane base  <-- the flip
    fifoCache(0x4040+4i, rect pair, 1)                       # src
    fifoCache(0x4048+4i, rect pair, 1)                       # dst
    fifoCache(0x4094+4i, hscale 16.16, 1)
    fifoCache(0x40a4+4i, vscale 16.16, 1)
fifoCache(0x407c+4i, w<<16|h, 1) ; fifoCache(0x406c+4i, x|y<<16, 1)   # per layer
fifoCache(0x4068, 0xaaaaaba0 | layer_mask, 1)             # enable mask, written last
fifoCache(0x5000 / 0x5800, …)                             # only when gamma changes
fifoCache(0x4050/0x4054, dest window, 1)
fifoCache(0x4058..0x4064, background/border, 1)
```

then, in `program_hardware_gated`:

```
0xfffffff005d20760(pipe)              # flush/prepare
0xfffffff005d21474(pipe)              # = programFifoPacket_gated()
```

and `programFifoPacket_gated` (`0xfffffff005d21474`) does, in order:

```
pending = 0xfffffff005d42120(fifo)                    # ring depth
0xfffffff005d24d1c(pipe, &b, &w)                      # 16-bit frame counter (pipe+0x1720)
0xfffffff005d42188(fifo, b, w)                        # completion-action token
(*fifo + 0x548)(fifo, pipe[0x13a5])                   # = 0xfffffff005d422a8  <-- THE KICK
```

`ADPParamFIFO::programFIFO` (`0xfffffff005d422a8`) then:
1. refuses if the caller is not the IOMFB instance (`"only IOMFB can program the
   ADP param FIFO"`);
2. **polls `getCurrentState` (`vtable+0x550` = `0xfffffff005d417e0`, which reads
   `ADP+0x101c` bits 7:4) with `_IOSleep(1)`, up to 1002 iterations** — i.e. it
   waits for the previous packet to drain. This is the proof that the FIFO is
   consumed asynchronously by the display, not by the CPU. **[V]**
3. calls `IOMFBParamFIFO::programFIFO` (`0xfffffff005d418d8`) → block
   `vtable+0xe0`, the actual push loop (header to `0x100c`, data to `0x1010`,
   ready bit `0x1004[0]`).

**Minimal flip sequence, in iOS's order:**

```
# one-time, at probe/CRTC init
mask = read32(ADP+0x1028)                 # save what iBoot left
write32(ADP+0x1028, mask | 0x1)           # enable the VBI/frame bit
write32(ADP+0x102c, read32(ADP+0x102c))   # clear any stale status (W1C)

# per flip
write32(ADP+0x4030, (new_phys & ~0x3f) | 0x2a)
# (only if geometry changed: 0x4038 stride, 0x402c format, 0x4068 enable mask)

# completion: either
irq = wait_event_timeout(vbi_wq, status_bit_seen, 100 ms)   # 0x102c bit 0x8
# or poll (iOS does this, 1 ms steps):
while (!(read32(ADP+0x102c) & 0x8)) msleep(1);
write32(ADP+0x102c, 0x8);                                   # ack W1C
```

If D2 shows tearing, replace the single `0x4030` store with the FIFO packet:

```
while (!(read32(ADP+0x1004) & 1)) ;        # FIFO has space
write32(ADP+0x100c, 0x4030);               # header = target register offset
write32(ADP+0x1010, (new_phys & ~0x3f) | 0x2a);
poll ADP+0x101c bits 7:4 == idle  (or wait for 0x102c bit 0x8)
```

### 12.5 Gap 2 — which disp0 IRQ is VBI, and how iOS handles it

**IRQ numbering.** `AppleH7DisplayPipe::start_hardware` creates its own
`IOFilterInterruptEventSource` with interrupt **index 0** on the provider
(`0xfffffff005d1e6dc`: `filterInterruptEventSource(this, 0xfffffff005d1eb70,
0xfffffff005d1f0e0, provider, 0)`). `AppleMobileADBE0::initBEInterrupts`
(`0xfffffff005d2fa3c`) asks for indices **1** and **16**, and
`initTconESDInterrupt` (`0xfffffff005d2fbe8`) for **18**, each guarded by
`if (n == -1) skip`. The DT has exactly five interrupts
(`<8c 8f 90 91 92>` = 140, 143, 144, 145, 146), so on j71t:

| index | AIC IRQ | owner | fires |
|---|---|---|---|
| 0 | **140** (see note) | `AppleH7DisplayPipe` (filter `0xfffffff005d1eb70`, action `0xfffffff005d1f0e0`) | **every frame** — VBI + packet-synced + the fatal pipe errors |
| 1 | 143 | `AppleMobileADBE0` (filter `0xfffffff005d2ff00`, action `0xfffffff005d30030`) | rarely — back-end causes; drives the power-gate state machine |
| 2,3,4 | 144,145,146 | nobody | — |
| 16, 18 | — | requested but absent → `-1` → **source not created** | — |

The index→IRQ column assumes `getInterruptNumber(n) == interrupts[n]`, which is
**[I]**; D1's counter step turns it into a measurement. What is **[V]** is that
the pipe owns whichever line index 0 is, and that indices 16/18 are dead here.

So: **request 140 for vblank.** 143 is worth requesting only for error logging.
Correction to §6: iOS does *not* install a second BE source on this device. **[V]**

**Status / ack / mask.**

| what | register | detail |
|---|---|---|
| status | `ADP+0x102c` | 32-bit; read `0xfffffff005d1f8b0` (`AppleDisplayPipe::getInterruptStatus`) |
| ack | `ADP+0x102c` | **W1C**: `0xfffffff005d20000(pipe, bits)` writes the handled bits back |
| mask | `ADP+0x1028` | enable word; only writer `0xfffffff005d1fe8c` (`AppleDisplayPipe::setInterrupts`) |
| software accumulator | pipe `+0x13a3` | the ISR ORs the status in; the swap path fetch-and-clears it |

iOS's ack discipline (`0xfffffff005d1f0e0` / `0xfffffff005d250d4`): read the
status, `_OSBitOrAtomic` the raw value into `pipe+0x13a3`, and re-enable/ack
through `setInterrupts`. `setInterrupts(pipe, 0x10fff, 0)` is used to arm
everything while a packet is in flight; bit `0x1` is re-armed afterwards.
`getInterruptStatus` returns `status & enable`, so an unmasked bit is invisible.

**Status bits in `ADP+0x102c`** (from `0xfffffff005d1f0e0` and
`0xfffffff005d250d4`):

| bit | name (iOS string) | meaning |
|---|---|---|
| `0x0001` | VBI / frame | frame boundary; handler samples `0x1034`, `0x1064…0x1070` and bumps the VBI counter |
| `0x0002` | `OutUnder` | output underrun — **fatal**, iOS panics |
| `0x0008` | packet synced | **the parameter FIFO packet was applied = flip done**; missing → swap dropped |
| `0x0040` | `PFAlign` | prefetch align error — fatal |
| `0x0200` | `PFOver` | prefetch overflow — fatal |
| `0x0400` | (unnamed) | tested together with bit 0 (`& 0x401`) |
| `0x0800` | `RdErr` | read error — fatal |
| — | `0xa42` | the fatal mask iOS panics on |

A Linux driver must **not** copy the panic: log the fatal bits, ack them, keep
scanning. Do not enable `0xa42` in `0x1028` if you want to survive an underrun.

### 12.6 Registers iOS reads to confirm a flip completed

| register | what it tells you | used by |
|---|---|---|
| `ADP+0x102c` bit `0x8` | the FIFO packet was consumed at a frame boundary (the actual flip-done) | `0xfffffff005d250d4` (wait loop), `0xfffffff005d1f0e0` |
| `ADP+0x102c` bit `0x1` | this is a VBI (per-frame tick) | `0xfffffff005d1f0e0` |
| `ADP+0x1034` | 16-bit hardware frame counter, sampled at each VBI | `0xfffffff005d1e6dc`, `0xfffffff005d1f0e0` |
| `ADP+0x1064`/`0x1068`, `0x106c`/`0x1070` | two 64-bit per-VBI counter/timestamp pairs | `0xfffffff005d1f0e0` |
| `ADP+0x101c` bits 7:4 (`ParmSt`) | FIFO not busy (0 = idle) | `0xfffffff005d19a64`, `0xfffffff005d417e0` |
| `ADP+0x1040` | FIFO write pointer at a packet boundary (low 13 bits = 0) | `0xfffffff005d19a64` |
| `ADP+0x4030`+4i | read-back of the value you wrote (what D2 should log) | `0xfffffff005d1f614` (state adoption) |

`0xfffffff005d40d04` is the generic "write a register, then poll a status
getter until it reads back the value, 10 s timeout" helper — the same pattern,
for the ADBE0 back-end block.

### 12.7 D1 — probe (phase A read-only; phase B one bounded IRQ experiment)

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Rules for D1: phase A is strictly read-only — **no `writel` at all, and never
`0x1028`**. Writing `0x1028` can drop the bit the (still running) power-gate
logic needs. Phase B may write only the W1C ack to `0x102c` inside its own
handler, because enabling a level-triggered line without acking storms the CPU.
Reading `0x102c` has no side effect, so phase A never needs to ack.

### 12.8 D2 — auto-reverting flip test (risky, self-healing)

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Hardening that D2 must keep: `do_revert()` on unload, on probe failure, and from
a `reboot_notifier`; never `|=`/`&=` the saved `0x1028` (restore the exact word);
ack exactly the value you read from `0x102c` (writing `0xffffffff` is harmless on
a W1C but hides whether a bit was already pending); do not touch `0x402c`,
`0x4038`, `0x4068`, `0x4050…` unless geometry changed; never power-gate
`ps_disp0`/`ps_dp`/`dart-disp0`.

### 12.9 Evidence index for this section

| claim | source |
|---|---|
| no latch bit; all plane writes go through `fifoCache(pusher≠NULL)` | `0xfffffff005d219fc` (call sites), `0xfffffff005d20624`, `0xfffffff005d20d78` |
| ring append, no MMIO | `0xfffffff005d4237c` → `0xfffffff005d42200` |
| kick + async drain (proof the FIFO is hardware-consumed) | `0xfffffff005d21474`, `0xfffffff005d422a8`, `0xfffffff005d417e0` |
| FIFO push port `0xc` = header, `0x10` = data, `0x4[0]` = ready | `0xfffffff005d42530`, `0xfffffff005d42574`, `0xfffffff005d4250c` |
| class/block inventory, 19 `IOMFB::BlockType` names | data table `0xfffffff006dbdc48` (`"BlockType::…"`) |
| power-gate states incl. `PacketSynced`/`GotVBITEFilter` | data table `0xfffffff006dbbf60` (`"PowerGateWithDisplayOnState::…"`) |
| VBI = provider interrupt index 0 (IRQ 140 assumed); indices 16/18 skipped (`!= -1`) | `0xfffffff005d1e6dc`, `0xfffffff005d2fa3c`, `0xfffffff005d2fbe8`, ADT `interrupts` |
| status `0x102c`, W1C ack, mask `0x1028` | `0xfffffff005d1f8b0`, `0xfffffff005d20000`, `0xfffffff005d1fe8c` |
| bit 0 = VBI, `0xa42` fatal, bit 3 = packet synced | `0xfffffff005d1f0e0`, `0xfffffff005d250d4` |
| frame counter `0x1034`, per-VBI pairs `0x1064…0x1070` | `0xfffffff005d1f0e0`, `0xfffffff005d1e6dc` |
| `ParmSt` at `0x101c[7:4]`, 6 states, pointer at `0x1040` | `0xfffffff005d19a64` |
| SPDS version `0x13` at `0x1014` | `0xfffffff005d1f7f4` |
| state adoption reads `0x1038` then `0x4030+4i`/`0x4038+4i` | `0xfffffff005d1f614` |
| CLCD LUT windows `0x4000/0x4400/0x4800/0x4c00/0x5000/0x5400/0x5800` (other block) | `0xfffffff005d4c390` |

New helpers for this pass (uncommitted, `build/d0/`): `a64.py` (minimal AArch64
decoder over the raw kernelcache), `scan_calls.py` (find `mov w1,#imm` before a
`bl`), `scan_mmio.py` (constant register offsets per function), `refs.py`
(ADRP+ADD reachability, to turn assert strings into function addresses),
`strs.py`, `fns.json` (1651 function starts in `0x5d10000–0x5d50000`).

---

## 13. D3: the ADP parameter FIFO protocol

Same corpus (iOS 16.7.10 j71t kernelcache, image base `0xfffffff005298000`).
This is the D3 of the parameter-FIFO follow-up, not §9's D3 (the DRM/KMS
skeleton) — it is a prerequisite for it.
**Read 13.7 first if you already coded the snippet at the end of §12.4**: the
packet does not go to `0x100c`/`0x1010`, and there is no per-bit mask.

### 13.1 Bottom line

1. **The packet port is `ADP+0x10c0`, a single 32-bit push register.** Every
   word of a packet — preamble, headers, data — is written to the *same*
   address, in order, with **no handshake between words**. The whole kick is a
   handful of `writel()`s (two of them inside loops) in one locked function. **[V]**
2. **A packet is `[preamble][header][data…][header][data…]…`.** The preamble
   carries the total number of words that follow; each header carries the
   target register offset (low 16 bits) and the number of data words that
   belong to it (13 bits). **[V]**
3. **Grouping is implicit: everything written in one kick is applied together
   at the next frame boundary.** There is no "end of frame" word, no vblank
   token and no double-buffer select. The completion evidence is status bit
   `0x8` ("packet synced", the same bit §12 already identified). **[V]** for
   the encoding and the bit; **[I]** for "one kick = one atomic frame update"
   (it is what the word-count preamble + single `PacketSynced` event imply).
4. **The gate before a kick is `0x1040[12:0] == 0` *and* `0x101c[7:4] != 2`.**
   iOS polls that pair with `_IOSleep(1)`, 1002 times (~1 s), then **panics**
   (`"ADP FIFO still busy after 1000ms"`). It is the only pre-kick wait. **[V]**
5. **`ParmSt` is not idle/busy/full/error.** It is a 4-bit enum with 6 legal
   values; the state decoder maps them through a table to just two results and
   **only value 2 blocks programming** — 0,1,3,4,5 all read as "ready", ≥ 6
   panics `"undefined ADP_ADPCONTROL_ADPCSTATE.ParmSt"`. Full/error are *not*
   distinguishable there; overflow shows up in `0x102c` (`PFOver`) instead. **[V]**
6. **After a kick the FIFO code does not block.** The flip path separately waits
   for `0x102c & 0x408` (wakeup from the VBI ISR + 1 ms poll), treats bit `0x8`
   as success and anything else as the error branch. **[V]**
7. **iOS never does a base-only flip.** `0x4030` is only ever written together
   with the whole layer set (`0x402c`, `0x4038`, `0x4040…`, `0x4068`) in one
   packet, because every plane register write goes through `fifoCache`. A
   subset packet is what the hardware *allows*; it is not what iOS exercises,
   so D3 must confirm it. **[V]** for the iOS behaviour, **[?]** for the subset.
8. The "mask" argument of `fifoCache` is a **word count**, not a bit mask, and
   the 6th argument is a **data pointer**. Register writes are always full
   32-bit stores. **[V]**

### 13.2 The packet words

Decoded from `ADPParamFIFODriver::programFIFO`, `0xfffffff005d19b44`
(reached as the driver's vtable `+0xe0`; that vtable is at
`0xfffffff006db7bf0`, with `+0xb8` = read32 `0xfffffff005d3d588`, `+0xc0` =
write32 `0xfffffff005d3d5ac`, `+0xd8` = `readCurrentParamFIFOState`
`0xfffffff005d19a64`). Call chain: §12.4's `programFifoPacket_gated`
`0xfffffff005d21474` → `ADPParamFIFO::programFIFO` `0xfffffff005d422a8` →
`IOMFBParamFIFO::programFIFO` `0xfffffff005d418d8` → `*vtable[0xe0]` =
`0xfffffff005d19b44`.

Every word is `writel(word, adp + 0x10c0)`:

| word | encoding | meaning |
|---|---|---|
| **preamble** | `0x8000_0000` \| `armed<<30` \| `(total_words & 0x1fff) << 16` \| `token` | start of packet: how many words follow, plus the completion-action tag |
| **entry header** | `0x2000_0000` \| `(nwords & 0x1fff) << 16` \| `(reg & 0xffff)` | "write `nwords` words starting at register `reg`" |
| **entry data** | the value(s) | `nwords` 32-bit words, register `reg`, `reg+4`, … |
| **alt entry** | `0x6002_0000`, then `reg`, then `value` | alternate 3-word form (see below) |

Notes:

* The opcode is bits `[31:29]`: `100` = preamble unarmed, `110` = preamble
  armed, `001` = register entry, `011` = alternate entry. Bits `[28:16]` are a
  word count, bits `[15:0]` are a register offset or a token. **[V]** for the
  bit placement, **[I]** for calling it an opcode.
* `reg` is only 16 bits, so the packet can address `0x0000…0xffff` of the ADP
  window. All plane registers (`0x402c…0x40a4`, pipe 1 `0x602c…`) fit. **[V]**
* `total_words` counts headers *and* data words (the ring accounting in
  `0xfffffff005d42200` adds `nwords + 1` per entry), so the receiver knows
  exactly where the packet ends. **[V]**
* `token` = the pipe's 16-bit packet serial (`*(u16 *)(pipe+0x1720)`,
  incremented per kick in `0xfffffff005d24d1c`); `armed` = whether a
  completion action is armed for this packet (`0xfffffff005d42188` stores the
  bool at FIFO`+0x270` and the token at FIFO`+0x274`; the kick reads both back
  at `0x5d19b88`/`0x5d19b98`/`0x5d19ba4`). **[V]** What the hardware does with
  the echoed token is **[?]** — no reader was found.
* iOS's reset of the staging state after every kick sets `+0x270 = 1`,
  `+0x274 = 0` (`0xfffffff005d423d4`), i.e. the default is "armed, token 0".
  A driver should therefore always set bit 30 rather than leave it clear. **[V]**
* **Alternate entry** (`0x6002_0000` + `reg` + `value`, at `0x5d19bfc…0x5d19c44`):
  emitted only when the queueing pusher is not the IOMFB instance
  (type flag `!= 0` in the ring entry). The flip path never produces it. Both
  encodings were observed in the same function; the 3-word form's exact
  hardware meaning is **[?]**, so a Linux driver should not emit it.

### 13.3 How one register write is encoded, and how a frame is grouped

`AppleH7DisplayPipe::fifoCache` (`0xfffffff005d20624`) has the signature

```c
IOReturn fifoCache(AppleDisplayPipe *pipe, ADPParamFIFO::Pusher *pusher,
                   uint32_t reg, uint32_t value,
                   uint32_t nwords, const uint32_t *words);
```

* `words == NULL` → `queue_register(pusher, reg, value)` (`0xfffffff005d4237c`).
* `words != NULL` → `queue_registers(pusher, reg, nwords, words)`
  (`0xfffffff005d42398`).
* `pusher == NULL` → bypasses the ring and writes the block directly
  (vtable `+0x540`, `0xfffffff005d42034`). Never used in the steady state.

Both queue functions only touch a **software ring** (`0xfffffff005d42200`):
32-byte entries of `{u32 reg; u64 nwords; u8 type; u32 inline_value; u32 *words;}`
in a buffer of 0xa7f entries / 0x14fe words, allocated at
`0xfffffff005d41f08`. Nothing reaches the hardware until the kick. **[V]**

The layer set (`0xfffffff005d219fc`) calls `fifoCache(pipe, pusher, reg, val, 1, NULL)`
for every plane register — `0x402c`, then per layer `0x4038`, `0x4030`,
`0x4040`, `0x4048`, `0x4094`, `0x40a4`, `0x407c`, `0x406c`, then `0x4068`
(`0xaaaaaba0 | layer_mask`) last, then the dest window/background. One kick at
the end (`0xfffffff005d21474`) turns that into **one packet**. **[V]**

So one pipe's write of `0x4030 = V` is exactly:

```
0x2001_4030      header: opcode 001, nwords = 1, reg = 0x4030
V                data
```

with the little detail that the value is `(base & ~0x3f) | 0x2a` — iOS forces
the low 6 bits to `0x2a` on **every** overlay register, including the stride
(`0x5d21d50`, `0x5d21dd8`) and never preserves the read-back low bits. **[V]**

### 13.4 The state machine: `0x101c[7:4]` + `0x1040`

`ADPParamFIFODriver::readCurrentParamFIFOState` (`0xfffffff005d19a64`) is the
whole decoder:

```c
if ((read32(0x1040) & 0x1fff) != 0) { state = 1; goto out; }   /* mid-packet */
st = (read32(0x101c) >> 4) & 0xf;
if (st > 5) panic("%s: undefined ADP_ADPCONTROL_ADPCSTATE.ParmSt 0x%x");
state = table[st];              /* 0xfffffff0053ab410 = {0,0,1,0,0,0} */
```

| `ParmSt` | table | iOS uses it as |
|---|---|---|
| 0 | 0 | ready |
| 1 | 0 | ready |
| 2 | **1** | **not ready — the only value that blocks a kick** |
| 3 | 0 | ready |
| 4 | 0 | ready |
| 5 | 0 | ready |
| 6…15 | — | `_panic` |

`0x1040` is the FIFO's word pointer/occupancy: **low 13 bits non-zero means the
previous packet is still in flight**, and the check is skipped when it reads 0.
The 13-bit width matches an 8192-word FIFO; the probe below should log both the
pre-kick and post-drain values so the exact meaning (write index vs. words
pending) is settled by measurement. **[V]** for the check, **[?]** for the
interpretation.

Consequence for a driver: **there is no per-word ready handshake.** The
`0x1004[0]` "can take a word" bit in §12.3 does not belong to this block (13.7)
and iOS never reads it on this path. The only backpressure is the pre-kick
gate, so the whole packet must fit the hardware's buffer (iOS allows itself
5374 words).

### 13.5 Completion, rejection and timeouts

| moment | iOS does | constant |
|---|---|---|
| before queueing | software ring space check (`*pusher+0x80`); failure → `0xe00002c2`, and an append that overruns the ring → `0xe00002db` | `0xfffffff005d42040`, `0xfffffff005d42200` |
| before the kick | poll `0x1040[12:0]==0 && ParmSt!=2`, `_IOSleep(1)` | 1002 tries, then `_panic` `"ADP FIFO still busy after 1000ms"` (`0xfffffff005d4e4b4`, fmt `0xfffffff0053a9939`) |
| concurrent kick | returns `0xe00002d5` `"FIFO is busy"` if the software flag (`FIFO+0x218`) is set | `0xfffffff005d418d8` |
| not the IOMFB owner | `0xe00002e2` `"only IOMFB can program the ADP param FIFO"` | `0xfffffff005d422a8` |
| after the kick | **returns immediately**; the packet drains asynchronously | — |
| flip done | sleep on event `pipe+0x9d18` (the ISR wakes it), then `_IOSleep(1)` + read `0x102c`, exit when `acc & 0x408` | `0xfffffff005d250d4` |
| success | `acc & 0x8` → call the "packet applied" handler | same |
| failure | `0x400` seen without `0x8` → the pipe's error handler (vtable `+0x8c8`) | same |
| back-end timing writes | write register, poll readback, 10002 ms, then panic | `0xfffffff005d40d04` |

**Status bits that mean the packet was not applied** (`ADP+0x102c`, W1C):

| bit | name | verdict |
|---|---|---|
| `0x0008` | packet synced | applied — this is the only positive ack |
| `0x0002` | `OutUnder` | output underrun; iOS panics |
| `0x0040` | `PFAlign` | prefetch alignment error; iOS panics |
| `0x0200` | `PFOver` | prefetch overflow → **the packet was dropped**; iOS panics |
| `0x0400` | unnamed | the "stop waiting" companion bit: iOS exits the wait with `0x408` and takes the error branch when `0x8` is absent |
| `0x0800` | `RdErr` | read error; iOS panics |
| `0xa42` | fatal mask | the set iOS panics on |

A Linux driver must log `0xa42`, W1C everything it read, and keep scanning
(§12.5). Treat "no `0x8` within N frames, or `0x8` plus any `0xa42` bit" as a
dropped flip and retry once before giving up — do not panic.

### 13.6 Minimal packet: "set plane base at the next vblank"

Two words, no stride change:

```
0xc002_0000 | n         preamble: armed, 2 words, serial n        → 0x10c0
0x2001_4030             entry: 1 word, register 0x4030            → 0x10c0
(base & ~0x3f) | 0x2a   data                                      → 0x10c0
```

With stride (5 words, iOS order — stride first):

```
0xc004_0000 | n
0x2001_4038
(stride & ~0x3f) | 0x2a
0x2001_4030
(base    & ~0x3f) | 0x2a
```

For pipe 1 add `0x2000` to the register field (`0x6030`/`0x6038`). Then wait for
`0x102c` bit `0x8`: that *is* "applied at the next vblank". Do **not** add a
vblank field to the packet — there is none, and a vblank wait before the kick
only adds latency (the packet lands on whichever frame boundary comes next).

If geometry changes, add `0x402c` (format) and `0x4068` (layer enable, written
last, `0xaaaaaba0 | mask`) to the same packet. If the base is the only change,
`0x402c`/`0x4068`/rects/scales need not be resent — but this is the one part of
the sequence iOS never exercises, so validate it in D3 before relying on it.

### 13.7 Corrections to §12.3 / §12.4

Two claims in §12 do not survive the driver decode:

1. **`0x100c` is not the packet header and `0x1010` is not its data port.**
   The pair `(write 0xc, write 0x10)` lives in a *generic* FIFO class method
   `0xfffffff005d42530` that addresses its block with **class-relative offsets
   4/8/0xc/0x10/0x14** (`ready` = block`+4`[0] at `0xfffffff005d42574`, `state`
   = block`+8` at `0xfffffff005d4250c`, `reset` = block`+0x14` ← `0x10000` at
   `0xfffffff005d42500`). That class is the `ADBEParamFIFOHalV1` family
   (`ADBEParamFIFODriver`, `getParamFIFOsBusy`, `_frontendParamFIFO` /
   `_backendParamFIFO` strings), i.e. the **back-end** FIFO, not the ADP packet
   port. Supporting facts: `0x100c` appears as a literal **nowhere** in the
   display kext; `0x1010` appears exactly once, as `writel(1, 0x1010)` in ADP
   bring-up (`0xfffffff005d19d38`); the ADP push loop writes `0x10c0` at six
   sites and never touches `0x1004`/`0x100c`/`0x1010`. **[V]** for the
   immediate/usage census, **[I]** for the block attribution.
2. **`fifoCache`'s third numeric argument is a word count, not a mask.** The
   call sites pass `w4 = 1, x5 = 0` (`0x5d21ca4`/`0x5d21ca8` and 40 more), and
   the callee routes `x5 != 0` to `queue_registers(reg, nwords, ptr)` — there
   is no read-modify-write, no per-bit enable and no shadow copy anywhere. A
   register write is `reg = value`, full 32 bits, and the only "modification"
   is the `| 0x2a` low-bit tag. **[V]**

Everything else in §12.3/§12.4 stands: `0x101c[7:4]`, `0x1040`, `0x1028`,
`0x102c` (W1C), `0x1034`, `0x1038`, `0x1048` ← `0x11`, the ring/queue/kick
order `fifoCache → queueCommand → programFifoPacket_gated → programFIFO`, and
the 1 ms poll shape.

### 13.8 Kernel-C: `adbe_fifo_set_plane()`

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Notes on the code:

* `adp_fifo_kick()` can be called from the IRQ-disabled atomic path: the push
  is `p->n` MMIO stores, no sleeps. Only `wait_idle()` sleeps, and that is the
  caller's (CRTC's) problem — on a healthy pipe it returns on the first read.
* `serial` is only a tag. Keeping it monotonic makes a stuck flip identifiable
  in a register dump; its meaning inside the hardware is **[?]**.
* Do not "fix" `OVL_TAG` to `*0` — iOS writes `0x2a` on every overlay register
  and never preserves the read-back low bits (§13.3).
* If D3 finds that a base-only packet is not picked up, fall back to the full
  layer set of §12.4 in one packet; the encoding above does not change, only
  the number of entries.

### 13.9 What D3 still has to measure

| question | measurement |
|---|---|
| Does `0x10c0` accept words when `ParmSt != 0`? | log `0x101c[7:4]` and `0x1040` immediately before and after a kick |
| Does `0x1040[12:0]` go non-zero while the packet drains and return to 0 at the frame boundary? | 1 ms poll for 3 frames around one kick |
| Is one kick exactly one `0x8`? | count `0x8` events per kick; expect 1 |
| Does a base-only packet move the picture? | paint a distinct buffer, send 3 words, check for tearing |
| Is `total_words` enforced? | send a deliberately wrong count once, with the seatbelt, and watch for `PFOver` |
| Is the `0x6002_0000` form ever needed? | assert `type == 0` in `queue_register`; if a foreign pusher shows up, log it |

All of these are read-only except the kick itself, which is the same write
`adbe_fifo_set_plane()` performs in production, and which D2's auto-revert
already covers.

### 13.10 Evidence index for this section

| claim | source |
|---|---|
| packet port `0x10c0`, six write sites, no handshake | `0xfffffff005d19b44` (`0x5d19bc0…0x5d19cd4`) |
| preamble = `armed<<30 \| total<<16 \| token`, bit31 set | `0xfffffff005d19ba0…0x5d19bd0` |
| entry header = `0x20000000 \| nwords<<16 \| reg` | `0xfffffff005d19c68…0x5d19c8c` |
| alt entry `0x6002_0000`, reg, value | `0xfffffff005d19bf4…0x5d19c44` |
| state decode `0x1040[12:0]`, `0x101c[7:4]`, table `{0,0,1,0,0,0}`, panic > 5 | `0xfffffff005d19a64`, table `0xfffffff0053ab410` |
| 1002 × `IOSleep(1)` then panic "still busy after 1000ms" | `0xfffffff005d422a8`, `0xfffffff005d4e4b4`, fmt `0xfffffff0053a9939` |
| software busy flag → `0xe00002d5` | `0xfffffff005d418d8` |
| ring: 0xa7f entries × 32 B, 0x14fe words | `0xfffffff005d41f08`, `0xfffffff005d41e5c` (`0x14ff`) |
| ring append / entry layout | `0xfffffff005d42200`, `0xfffffff005d4242c` |
| `fifoCache` = (reg, value, nwords, words*) | `0xfffffff005d20624`, call sites `0x5d21ca4`, `0x5d21d70` |
| plane order: 0x402c, 0x4038, 0x4030, rects, scales, 0x4068 last | `0xfffffff005d219fc` |
| forced `0x2a` tag and `& ~0x3f` on overlay regs | `0x5d21d50`, `0x5d21dd8` |
| completion wait mask `0x408`, success = bit 3 | `0xfffffff005d250d4` (`0x5d25134`) |
| ISR returns handled on `0x401` or bit 3, wakes `pipe+0x9d18` | `0xfffffff005d1f0e0` (`0x5d1f324`) |
| fatal `0xa42` path | `0xfffffff005d1f0e0` (`0x5d1f1bc`) |
| `0x100c` never written, `0x1010` written `1` once, `0x1004` unused | immediate census over `0x5d00000–0x5e00000` |
| generic FIFO class: block `+4/+8/+0xc/+0x10/+0x14` | `0xfffffff005d42500…0x5d42574` |
| token source: `*(u16 *)(pipe+0x1720)` | `0xfffffff005d24d1c`, `0xfffffff005d214c4` |
| iOS never writes `0x4030` alone | only two `0x4030` literals: `0x5d1f6fc` (read), `0x5d21de0` (layer set) |

Decoding note: the shared Ghidra server stopped answering mid-pass, so this
section was decoded with a capstone sweep over the same kernelcache
(`build/d3/kc.py`, `scan.py`, `callers.py`, `callsite.py`, `imm.py`,
`refs.py`; VA → file offset `(va & 0xffffffff) - 0x4924000`). Every function
previously decompiled by Ghidra (`0x5d422a8`, `0x5d418d8`, `0x5d419a64`,
`0x5d20624`, `0x5d42530`) reproduces identically in both, including the
`0x3ea` loop and the vtable slots.
