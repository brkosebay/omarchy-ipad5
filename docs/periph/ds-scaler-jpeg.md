# A9 image scaler/CSC and JPEG codec — driver study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. iPad 5, iPad6,11, S8003 / J71t, iPadOS 16.7.10 (20H350).
**No device access was performed in this round**, nothing was committed, and no
kernel branch was touched. Every hardware statement below comes from the saved
ADT (`docs/adt/j71t.txt`, `j71t.json`), the private IODeviceTree dump
(`build/firmware/private/IODeviceTree.txt`), the shared iOS 16 kernelcache
analysis, or the Linux sources already in `build/`/Docker.

Evidence is tagged:

- **[E]** read directly from an artifact (ADT, ioreg dump, kext code, Linux tree).
- **[I]** inference that fits the evidence but is not proven.
- **[U]** unresolved; a measurement or a further static check is required.

Scope: the two blocks that could accelerate *output* work on this device —
the **image scaler / colour converter** (`scaler0`, Apple's "MSR" pipeline) and
the **JPEG codec** (`jpeg`, decode + encode). The H.264/H.265 **encoder** (`ave`)
is mentioned only where it changes the plan.

## 0. Result in one table

| | Scaler / CSC | JPEG codec |
| --- | --- | --- |
| DT node | `scaler0`, `device_type = "scaler"`, compatible `scaler,s8000` / `scaler,s5l8960x` **[E]** | `jpeg`, `device_type = "jpeg"`, compatible `jpeg,s8000` / `jpeg,s5l8920x`, `hw-type = "a0102"` **[E]** |
| MMIO | `0x07900000` + `0x4000` **[E]** | `0x07b00000` + `0x2000` **[E]** |
| IRQ | AIC 179 **[E]** | AIC 175 **[E]** |
| IOMMU | `dart-scaler` @ `0x07908000`/`0x07904000`, IRQ 181/180/182, 4 KiB pages, 10-bit stream index **[E]** | `dart-jpeg` @ `0x07b04000`, IRQ 176/177, 4 KiB pages, 10-bit stream index, `dead-mappings` **[E]** |
| PMGR | power gate 25, clock gate 25, clock id 291 **[E]** | power gate 26, clock gates 26 + 41, clock ids 289 + 290 **[E]** |
| iOS kext | `com.apple.driver.AppleM2ScalerCSCDriver` (also owns `IOSurfaceAcceleratorClient`) **[E]** | `com.apple.driver.AppleJPEGDriver` **[E]** |
| iOS HAL selected on this board | `AppleM2ScalerCSCHalMSR4` ("Alcatraz", MSR 4) — DT `hardware-version = 3` maps to cases 3/5 of `getHalByEDTValue` **[E]** | `AppleAJPEGHal` (the base/original HAL), not V8/V9/V11 — `hw-type a0102` takes the first dispatch branch **[E]/[I]** |
| Programming model | **APIODMA**: register writes become `header + N values` packets in a DART-mapped stream that the block replays; format recovered (§3.4), **per-pipeline register banks** ("register groups") **[E]** | plain MMIO: a fixed `(offset,value)` program, tables (Huffman/quant) fetched by the block from IOVA **[E]** |
| Formats proven in code | in/out: `ARGB8888`, `RGBA8888`, `PMARGB8888`, `RGB565`, `ARGB1555`, `ARGB2101010`, `ARGB8101010`, `ARGBV16`, `PARGBV16`, `2Planes420/422/444` (+10-bit and packed variants), `1PlaneYCBCR10444/10422`, `LumaOnlyL008/010/012/016`, `Interleaved422` **[E]** | decode out: `420v`, `420f`, `422v`, `422f`, `444v`, `444f` only **[E]**; encode in: JPEG-domain `IMG_*`/`CMP_*` enum, exact FourCC set **[U]** |
| Scaling / transform | fractional offsets, directional scaling, H/V filter taps+phases, 90/180/270 rotation, X/Y flip, CSC matrices at 14 defined points, gamma/HDR LUTs **[E]** | `AppleJPEGSupportsDCTScaling = 1` is published as a property **[E]**, but `startDecoderGated` rejects the job when output dimensions differ from the header ("x/y scale is not supported") **[E]** |
| Linux today | nothing; no scaler driver exists for any pre-M1 Apple SoC **[E]** | nothing; Asahi has no JPEG driver, and `ave` (encoder) has none either **[E]** |
| Best Linux template | `drivers/media/platform/rockchip/rga` (V4L2 m2m scaler with scaling+flip+tiling), `drivers/media/platform/imagination/e5010-jpeg-enc` | `drivers/media/platform/rockchip/rkjpegd` (VDPU720, in review), plus `drivers/media/v4l2-core/v4l2-jpeg.c` for header/table parsing **[E]** |
| Verdict | reusable; the packet format is known, the *register values* are not, so it starts with a dump-and-replay step → **rank 3** | smallest, most self-contained block, with an upstream helper that already produces the tables → **rank 1** |

## 1. Artifacts, addresses, reproduction

All kernel addresses are **unslid virtual addresses** in

```text
build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
SHA256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
Ghidra project: kernelcache.release.iPad6,11_12; AARCH64:LE:64:AppleSilicon
```

Kext identities (from `build/periph/kexts/prelink.json`, resolved with
`tools/ds-video/kc-kexts.py`):

| Bundle | `__TEXT` (header) | `__TEXT_EXEC` | `__DATA` | `__DATA_CONST` |
| --- | --- | --- | --- | --- |
| `com.apple.driver.AppleM2ScalerCSCDriver` | `fffffff00532ec00` + `0x347e2` | `fffffff005c00000` + `0x61628` | `fffffff007a13040` + `0x1bc21` | `fffffff006d77b00` + `0x1ae90` |
| `com.apple.driver.AppleJPEGDriver` | `fffffff0054c1500` + `0xa1e5` | `fffffff005fc4000` + `0x1fa74` | `fffffff007a63540` + `0x351` | `fffffff006e4cd00` + `0x25a8` |
| `com.apple.driver.AppleAVEH8` (encoder, out of scope) | `fffffff0053af980` + `0x95a25` | `fffffff005d4f000` + `0x33a34` | — | — |

The live iOS device tree confirms the same wiring and adds the class names that
are actually instantiated on this board: `AppleM2ScalerCSCDriver`,
`AppleM2ScalerCSCHal`, `M2ScalerDMACommand` (30 live), `M2ScalerScalingControl`,
`M2ScalerSrcDestCfgControl`, `M2ScalerCSCColorConversionControl`,
`AppleJPEGDriver`, `AppleJPEGHalV9`/`V8`/`V11` (0 instances each),
`AppleAJPEGHal` **[E]**.

Method used this round (`tools/ds-video/`, see its README):

1. `kc-kexts.py` for kext identity, address→bundle mapping and in-place
   disassembly of the kernelcache.
2. `catalog.py` to turn the kexts' `__PRETTY_FUNCTION__` strings into a
   method→address map (Ghidra xrefs).
3. `xrefs.py` to find code sites for literals Ghidra has no xref for, by
   decoding `adrp`+`add` pairs out of `__TEXT_EXEC`.
4. `regprog.py` to lift `(offset,value)` pairs out of `mov`/`str` sequences.
5. Ghidra decompilation for the handful of methods that need control flow
   (`getHalByEDTValue`, `AppleAJPEGHal::init`, `startDecoder`/`startDecoderGated`).

## 2. The hardware the device tree describes

### 2.1 Scaler

```text
scaler0:  reg 0x07900000 size 0x4000          interrupts 179
          hardware-version 3                  power-gates 25, clock-gates 25, clock-ids 291
          iommu-parent -> mapper-scaler       coprovider-group "scaler"
dart-scaler: reg 0x07908000 size 0x4000, reg 0x07904000 size 0x4000
          interrupts 181, 180, 182            page-size 0x1000, stt-idx-width 10
          fetch-config 0xe0303, diag-config 0x100, smmu-present 1
          power-gates 33, clock-gates 33
          mapper-scaler: reg 0, tail-padding 1, iomd-cache-size 64, iomd-cache-ttl 0x7d0
```

**[E]** from `docs/adt/j71t.json` (`arm-io/children[34]`, `[35]`). Three
things are worth spelling out:

- The scaler has **one** register bank in the ADT. `AppleM2ScalerCSCHal::
  mapRegisterMemory` maps device-memory index 0, and `IosaDPEControl::
  mapDPEMemory` maps index **2** [E: `FUN_fffffff005c28340` calls the
  `mapDeviceMemoryWithIndex` vtable slot with `0`; `FUN_fffffff005c04a5c`
  calls it with `2`]. On this board index 2 does not exist, so either the DPE
  path is compiled-in but inert for A9, or the DPE is reached through another
  provider. **[U]** — matters only for display-pipe-style colour work, not for
  scaling.
- `hardware-version = 3` is the key that selects the whole HAL:
  `AppleM2ScalerCSCDriver::getHalByEDTValue` reads the DT property
  `"hardware-version"` and switches on it (`FUN_fffffff005c2f5f4`) [E].
  Cases `3` and `5` allocate a 0x1e8-byte object whose vtable at
  `fffffff006d89880` contains `AppleM2ScalerCSCHalMSR4::scalerErrorInterruptStatus`
  (`fffffff005c457a0`) at slot `0x308` [E]. So **the A9 scaler is the MSR 4
  generation, codename "Alcatraz"** [E: `"Alcatraz MSR Register Dump"` string
  at `fffffff00535eb37`, class string at `fffffff00535eb53`].
- `coprovider-group = "scaler"` plus `M2ScalerDMACommand` being live (30
  instances) says the block is a **DMA master that drives its own pipeline**,
  not a passive register file [E]/[I].
- **The Linux DT already models both power domains.** `s800-0-3-pmgr.dtsi`
  has `ps_msr` (`power-controller@802e0`, label `"msr"`, parent `ps_media`)
  and `ps_jpg` (`power-controller@802d8`, label `"jpg"`, parent `ps_media`)
  **[E]**. That names the block in Apple's own vocabulary — **the scaler is
  "MSR"** — and it matches the ADT's adjacent gate indices 25 (scaler) and 26
  (JPEG), which sit 8 bytes apart in PMGR. The DART gates (ADT 33 and 34) are
  *not* adjacent PMGR entries (`0x802a0`/`0x80298` are `sf`/`smx`, always-on
  fabric), so their `ps_*` node is **[U]**; in practice a DART shares its
  client's island, so `power-domains = <&ps_msr>` / `<&ps_jpg>` on the client
  may be enough and the DART node can carry no domain until measured.

### 2.2 JPEG

```text
jpeg:     reg 0x07b00000 size 0x2000           interrupts 175
          hw-type "a0102"                      power-gates 26
          clock-gates 26, 41                   clock-ids 289, 290
          iommu-parent -> mapper-jpeg
dart-jpeg: reg 0x07b04000 size 0x2000          interrupts 176, 177
          page-size 0x1000, stt-idx-width 10   power-gates 34, clock-gates 34
          fetch-config 0xe0303, diag-config 0x100, smmu-present 1
          mapper-jpeg: reg 0, tail-padding 2, dead-mappings 0x7ffff000/0x1000
```

**[E]** from `docs/adt/j71t.json` (`children[36]`, `[37]`).

`hw-type "a0102"` is parsed as an integer and dispatched in
`AppleJPEGDriver::start` (`fffffff005fc6090`): the first test is
`(type - 0xa0000 - 0x102) < 2`, i.e. types `0xa0102` and `0xa0103` take the
branch that instantiates the **base** `AppleAJPEGHal` [E: disassembly of
`fffffff005fc60b0`–`fffffff005fc6138`]. `AppleJPEGHalV8/V9/V11::init`
each reject everything except their own constants (`0x100000`/`0x100008`,
`0xe0103`, `0xf0000`/`0xe0008`) [E]. So:

- **A9 = `AppleAJPEGHal`, the original s5l8920x-lineage HAL** [E]/[I].
  Consequence: `AppleAJPEGHal::init` only takes the "AXI register page at
  `physical - 0xfff900`" path when the hardware version register reads back
  `>= 0xd`; below that it uses the in-window idle registers instead
  **[E: `FUN_fffffff005fcdac8`, the `if (iVar1 < 0xd)` split]**. The version
  value this SoC actually reports is **[U]** (it is a register read, not a
  constant) — but since Apple ships the base HAL for `hw-type 0xa0102`
  while V8/V9/V11 exist for the later chips, `>= 0xd` would be surprising.
  Either way the decoder register map in §4.2 uses the in-window `+0x1000`
  registers, which both paths agree on **[E]**.
- `AppleJPEGNumCores` is computed as `table[hwType-5]` for hw types 5..13 and
  **1** otherwise, and is published as a property along with
  `AppleJPEGRequiresMCUAlignment = 0` and
  `AppleJPEGSupportsDCTScaling = 1` [E: `FUN_fffffff005fc7134`]. So **one
  codec instance, no MCU-aligned destination requirement**.

## 3. Scaler: what the iOS driver does

### 3.1 Method map (evidence addresses)

Selected entries from `tools/ds-video/catalog.py scaler`; the full list is
reproducible with that command.

| Method | Address |
| --- | --- |
| `AppleM2ScalerCSCDriver::start` / `free` | `fffffff005c2e6a0` / `fffffff005c2e264` |
| `AppleM2ScalerCSCDriver::transformGeneral(M2ScalerCSCRequest*)` | `fffffff005c31b8c` |
| `AppleM2ScalerCSCDriver::prepareAndMapBuffers` | `fffffff005c32d00` |
| `AppleM2ScalerCSCDriver::getIOSurfaceFromID` | `fffffff005c31a4c` |
| `AppleM2ScalerCSCDriver::copy_surface` / `blit_surface` | `fffffff005c31370` / `fffffff005c313a8` |
| `AppleM2ScalerCSCDriver::initPowerAndClock` | `fffffff005c2f7d0` |
| `AppleM2ScalerCSCHal::mapRegisterMemory` | `fffffff005c28340` |
| `AppleM2ScalerCSCHal::initScalerPropertiesAndExtents` | `fffffff005c28408` |
| `AppleM2ScalerCSCHal::initHalObjects` / `createHalObjects` | `fffffff005c2919c` / `fffffff005c600e4` |
| `AppleM2ScalerCSCHalMSR::detectHardwareVersion` | `fffffff005c3e9c4` |
| `AppleM2ScalerCSCHalMSR::activateMSR_gatedContext` | `fffffff005c3e1e4` |
| `AppleM2ScalerCSCHalMSR4::scalerErrorInterruptStatus` | `fffffff005c457a0` |
| `M2ScalerCSCRequest::validateAndInitializeCommAPI` | `fffffff005c53f24` |
| `AppleM2ScalerCSCDriverFilters::updateWithCoefficients` | `fffffff005c5810c` |
| `MsrAPIODMA::writeToPacket(MsrAPIODMA::PacketRequest*)` | `fffffff005c258bc` |
| `MsrAPIODMA::init(const APIODMAInitDescriptor*)` | `fffffff005c23f34` (string site) |
| `MsrAPIODMA::resetSequenceBuffer_gated_context` | `fffffff005c5fe3c` (string site) |
| `IOSurfaceAcceleratorClient::user_transform_surface` | `fffffff005c1e97c` |
| `IOSurfaceAcceleratorClient::k2k_setCustomFilter` | `fffffff005c5fa30` (string site) |

### 3.2 The pipeline is a chain of "pipe units"

`initHalObjects` (decompiled: `FUN_fffffff005c2919c`) initialises, in order,
and logs a distinct failure string for each **[E]**:

`ScalingControl` → `CSC control` → `Src/Dst cfg control` → `dither control` →
`color manager` → `internal pipe ctrl` → `HTPC control` (tiled/compressed
memory) → `ASE control` → `prescaler control` → `RDMA control` →
`WDMA control` → `film grain control`.

Every one of those is an `Iosa*` class (`IosaInternalPipeMSR*`,
`IosaColorManagerMSR*`, `IosaPrescalerControlMSR*`, `IosaRdmaPipeUnit`,
`IosaWdmaPipeUnit`, `IosaTiledCompressedMemMSR*`, `IosaChromaUpsamplePipeUnit*`,
`IosaPixelAveragingPipeUnit*`, `IosaFilmGrainPipeUnit*`, `IosaDPEControl*`,
`IosaConvertPipeUnit`, `IosaRevertPipeUnit`, `IosaReshapePipeUnit`) **[E]**.

That is the same vocabulary iOS uses for the **display pipe** (`adbe`), which
is why the scaler's kext carries `IosaDPEControl` at all: the A9 scaler is
built from the display-pipe IP blocks. Practical consequence: register
knowledge for the scaler and for the (still missing) `adbe` KMS driver overlap
**[I]**. Take it seriously when planning P6's display work — but do **not**
assume the register offsets are identical; only the block taxonomy is shared.

### 3.3 Register model: groups, aperture, per-instance bias

- Reads go through one helper: `MMIO_read(hal, off) = *(u32*)(hal->mmio
  [0x2b0] + ((hal->bias[0x298] + off) & ~3))` **[E: `FUN_fffffff005c23904`]**,
  so each pipeline instance has a **base bias** added to every offset.
- Writes go through `FUN_fffffff005c2391c` → `FUN_fffffff005c23944`, which
  takes `(hal, offset, value, mask, shift)` and does a read-modify-write plus
  bookkeeping **[E]**.
- Offsets observed being read on this hardware family: `0x0c`, `0x14`, `0x18`,
  `0x1c`, `0x20`, `0x68`, `0x9c`, `0xa4` (and `0x6378`/`0x637c` — but see
  below) **[E: 71 `bl` sites to the read helper; immediates recovered with
  `regprog.py`]**. Named ones: `MSR_CTRL_DBGSTS = 0x9c`
  **[E: `FUN_fffffff005c44b1c` reads offset `0x9c`, compare mask `0x2aa`]**.
  `0x6378`/`0x637c` are outside the ADT's 16 KiB window and MSR15/MSR16
  create a *separate* mapping for the SoC-version register
  **[E: `"SCALER[MSR15]: Cannot create mapping for SoC Version register
  address"`]**, so they are almost certainly offsets in that second map, not
  in `scaler0`'s bank **[I]**. That second map is the one unknown address in
  this block — it is not in the ADT **[U]**.
- Registers are addressed in named **groups**: `kRegisterGroupNone`, `Basic`,
  `Filters`, `GammaLuts`, `HdrLuts`, `Ase`, `Start`, each with a
  `getRegisterCount(group, …)` per HAL revision **[E]**. A group is therefore
  a contiguous register range with a per-revision length, which is why the
  driver needs a bounds-checked writer rather than a struct of offsets **[I]**.
- **`MSR_GLBL_IRQSTS` offset was not recovered statically** **[U]**. The status
  value is *passed into* `scalerErrorInterruptStatus` from the interrupt
  handler, so it is read somewhere Ghidra did not turn into a function. Read it
  from a device-side register dump (P0) rather than guessing.

### 3.4 Programming model: an APIODMA register-write stream

This is the single most important architectural finding for a Linux driver.
The HAL has **two** write paths and they are used for different things:
single masked register writes through the helper in §3.3, and bulk pipeline
programming through an **APIODMA packet stream**:

- `MsrAPIODMA` ("Apple Packet IO DMA") owns a **sequence buffer** that is
  mapped into the scaler's DART (`mapSequenceToDart_gatedContext`) and a
  **transform id**; `allocateBaseIndex_gatedContext` hands out base indices in
  that buffer, `MsrAPIODMA::writeToPacket` appends register writes, and
  `executeTransforms_gated` kicks the engine **[E: class/method strings at
  `fffffff0053562xx`–`fffffff0053568xx`]**.
- **The packet word format is now recovered** [E: `MsrAPIODMA::writeToPacket`
  at `fffffff005c258bc`, decompiled and cross-checked against the
  disassembly]. One packet is a 32-bit header followed by *N* 32-bit payload
  words, appended to a `u32` sequence buffer:

  ```text
  header bits [ 1: 0]  flags  (from PacketRequest+0x29; only 0..3 are sane)
  header bits [17: 2]  (request->base + request->offset) & 0x3fffc
                       -> destination register offset, 4-byte granular,
                          18 bits, i.e. 256 KiB of reachable aperture
  header bits [29:18]  (N - 1) & 0xfff   -> N = payload word count, 1..4096
  header bits [31:30]  a 2-bit field from the stream object (stream+0x6c)  [U]
  payload[0..N-1]      the values, read from the request with a byte stride
                       (PacketRequest+0x28, in words)
  ```

  The stream object holds `buffer` (+0x38), `write index` (+0x40),
  `used` (+0x44, payload words only) and `capacity` (+0x64); the capacity
  check is `count + used <= capacity` and overflowing it panics with
  `"APIODMA Sequence FIFO overflow"` **[E]**. So a packet is exactly one
  auto-incrementing register burst write, and a whole transform is a list of
  them. This is small enough to reimplement in a Linux driver in a few dozen
  lines **[I]**.
- The stream is **versioned**: `writeToPacket` only accepts
  `this->version ∈ 1..3` and otherwise panics with `"packet version %d not
  supported"`, and `MsrAPIODMA::init` separately validates a *header* version
  (`"Unsupported packet header version %d"`) **[E]**. The version gates which
  header encoding is used; the layout above is the one in this kernelcache.
- There is a debug path that dumps exactly what we want:
  `"APIODMA: Register sequence dump: Transform id = 0x%x"`, plus the runtime
  properties `TriggerOneTimeRegistersDump`, `EnableStickyRegistersDump`,
  `TransformDumpLevel`, `EnableNullKickoff`, `EnableNullDriver`,
  `EnableKernelTests` **[E]**. On a jailbroken device these are settable from
  userland via IORegistry, i.e. **the iOS driver can be made to print its own
  register program** without any hypervisor (which A9 cannot have — no EL2).
- The engine also reports parse/axis errors and PIO vs MEM traffic counters
  (`APIODMA: pioRdAddr`, `memRdAddr`, `Sequence FIFO overflow`,
  `command parse error`) **[E]**, which tells us the stream can address both
  MMIO ("pio") and memory ("mem") targets **[I]**.

Which registers are written through which path is **[U]**, but the packet
path clearly carries the per-transform pipeline setup (filters, CSC, DMA
addresses), because those are the values that change every job.

For Linux this is *not* a blocker any more: the packet format is a header plus
a burst of values, so either (a) replay the same pairs as plain CPU MMIO writes
and see whether the block cares, or (b) implement the 20-line stream builder.
What remains is that **the register values themselves must come from
somewhere** — and the cheapest source is the iOS driver's own dump facility.

### 3.5 Formats, transforms, filters

Format vocabulary is fully enumerated in the kext as property/format **names**
(these are the strings the driver publishes and parses, so the constants
themselves are not in the strings) **[E]**:

> Private graphics-dependent excerpt omitted from this source snapshot.


Transform vocabulary: `Rot`, `Flip X`, `Flip Y`, `Rotate 90`, `Rotate 180`,
`Rotate 270`, `Rotate 90/Flip X`, `Rotate 90/Flip Y` **[E]** — a generic
rotation field plus the seven derived orientations, applied by the
`IosaReshapePipeUnit`/`IosaConvertPipeUnit` **[I]**.

CSC: `IosaColorManagerMSR*::configureCSC_gatedContext(request, MatrixLocation)`
with 14 matrix points (`kMatrixPreScaler`, `PreCM`, `PreDegamma`,
`PostDegamma`, `PreToneMapping`, `PostToneMapping`, `PreCLUT`, `PostCLUT`,
`PreRegamma`, `PostCM`, `PreNormalization`, `PostNormalization`,
`GamutConversion`, `Accessibility`), plus `programGammaLUT_gatedContext`,
`convertToLinear`/`convertToGamma`, and HDR LUT validation
(`lumaShiftBits ≤ 16`, `lumaOutputSel ≤ 4`, coefficients in `[-32767:32767]`)
**[E]**. A minimal driver needs only the identity/pre-CM matrix and gamma-off.

Filters: per-`(plane ∈ {Luma,Chroma,Alpha}) × (dir ∈ {H,V})` coefficient sets
with `init`, `clean`, `updateWithAllCoefficients`, `updateWithCoefficients`,
`setCoefficient`, `getCoefficients`, and a userspace path
`IOSurfaceAcceleratorClient::setCustomFilter` / `k2k_setCustomFilter` **[E]**.
Coefficient **geometry** (taps, phases, pre/post point bits) is published as
`IOSurfaceAcceleratorFilter{CoefficientsPrePointBits, …PostPointBits,
HorizontalTapsCount, VerticalTapsCount, HorizontalPhasesCount,
VerticalPhasesCount}` **[E]** — a V4L2 driver can expose exactly these numbers
as its scaler capabilities instead of inventing limits.

Hardware limits are likewise published, not hard-coded in the driver:
`IOSurfaceAcceleratorCapabilities{Min,Max}{Source,Dest}{Width,Height}`,
`Min/MaxAseSource*`, `Max{H,V}{Up,Down}scale`,
`FractionalXYOffsetsSupported`, `DirectionalScaling(+Ratio)`, `HDR`,
`ASEMinRatio`, `Interchange{Stride,Base}Alignment`,
`InterchangeMacroblockSize{Normal,Large}`, `MMR`, `YCbCrAlpha`, `16bitYCbCr`,
`12bitYCbCr`, plus `IOSurfaceAcceleratorScalerVersion` **[E]**. All of them are
`setProperty`-published and therefore readable from the live device (P0).

Formats, straight from the driver's own enum-name table
(`fffffff005357d8a`–`fffffff00535860b`) **[E]**:

```text
in : 2Planes420, 2Planes422, 2Planes444, RGB565, ARGB1555, ARGB8888, RGBA8888,
     PMARGB8888, LumaOnlyL008/L010/L012/L016, Interleaved422, ARGB2101010,
     ARGB8101010, ARGBV16, PARGBV16, 1PlaneYCBCR10444, 1PlaneYonly8,
     1PlaneYonly10, 1PlaneYCBCR10422, 2Planes42010, 2Planes42210, 2Planes44410,
     2PlanesPacked42010, 2PlanesPacked42210, 2PlanesPacked44410
out: the same set with the `Out` prefix
```

(27 in-formats and their 27 out-format twins; the enum-name table is what the
driver parses userspace format strings against, so it is also the exact
`IOSurface` pixel-format set a Linux driver has to map to V4L2 fourccs.)

### 3.6 Mapping the format names to V4L2 (practical)

The iOS names above have V4L2 counterparts for the important ones. This is a
*draft* mapping for the P3 driver, with the one trap called out:

| iOS format | V4L2 | notes |
| --- | --- | --- |
| `2Planes420` | `V4L2_PIX_FMT_NV12M` | Y plane + interleaved CbCr; limited vs full range goes in `v4l2_pix_format.quantization` |
| `2Planes422` | `V4L2_PIX_FMT_NV16M` | |
| `2Planes444` | `V4L2_PIX_FMT_NV24M` | |
| `2Planes42010/42210/44410`, `…Packed…` | `P010`/`P012`/`P016` family | P010M etc.; packed variants are the `P210`/`P212`/`P216` family |
| `LumaOnlyL008/010/012/016` | `GREY`, `Y10`, `Y12`, `Y16` | |
| `Interleaved422` | `V4L2_PIX_FMT_YUYV` | verify UYVY vs YUYV order |
| `1PlaneYCBCR10444/10422` | `Y210`/`Y212` family | 10-bit 4:4:4 / 4:2:2 packed |
| `RGB565`, `ARGB1555` | `RGB565`, `RGB555` variants | |
| `ARGB8888`, `RGBA8888`, `PMARGB8888` | `V4L2_PIX_FMT_[A/X]RGB32` family | **trap:** IOSurface's `ARGB8888` names the *component* order, V4L2's `_ARGB32`/`_XRGB32` name the *byte* order — verify with a single-pixel test before trusting either |
| `ARGB2101010`, `ARGB8101010` | `ARGB2101010`, `XRGB2101010` | |
| `ARGBV16`, `PARGBV16` | — | FP16 RGBA has no standard V4L2 fourcc **[U]** |

For the JPEG decoder the mapping is simpler and exact:
`420v → NV12 (limited)`, `420f → NV12 (full)`, `422v/422f → NV16`,
`444v/444f → NV24`, with `quantization` carrying the v/f distinction
**[I]**, since V4L2 has no separate fourcc for range.

### 3.7 Buffers, DART, completion
`transformGeneral` (`fffffff005c31b8c`) is the whole submit path **[E,
decompiled]**: per-channel transform id in `request+0x1e4`, the client's DART
mapper in `request+0x60`, `prepareAndMapBuffers(src|dst, request)` →
`getPreparedMemoryDescriptor`, source/destination protection-bit match check
("mis-match in protection bits between source and destination IOSurfaces"),
then two iterations over the pipe stages, `syncDMA`, an optional
"accessibility" CSC pass, kickoff through the command gate, and completion
through `FUN_fffffff005c3345c`. Requests carry a tracepoint class of `0x31b…`
kdebug codes **[E]**.

The scaler **requires** the DART: every surface address handed to the block is
an IOVA, and the driver's own log strings are full of DART cache maintenance
(`kIODARTFunctionCacheFlushInactive`, `iomdEarlyReclaim`, `iomdEarlyPurge`,
`OverrideDARTPower`) **[E]**. Bypass mode would need the block to accept
physical addresses; nothing in the code suggests it does **[I]**.

## 4. JPEG: what the iOS driver does

### 4.1 Lifecycle

`AppleJPEGDriver` implements a small queue: `begin_io_gated`,
`queue_io_gated`, `interruptOccurred_gated`, `finish_io_gated`,
`release_codec_gated`, `checkDARTException` **[E]**. Per-request work is
`setupBuffersForCoding_gated` → `doRestOfBufferSetupFor{Decode,Encode}_gated`
→ `start{Decoder,Encoder}Gated` → `startDecoderGated` (the HAL) →
`finishDecode_gated` **[E]**. Power is per-request: `setPowerState_gated`,
`"wait for idle done, powering off"`, and a 1-second window on DART exceptions
from malicious files (`"too many dart exceptions from malicious file in
1sec"`) **[E]** — that string is worth remembering: the decoder enforces
DART fault rate limiting.

### 4.2 Register map (recovered, base-class decoder/encoder)

From `AppleAJPEGHal::reset_jpeg_block` (`fffffff005fce830`),
`AppleAJPEGHal::startDecoderGated` (`fffffff005fd1924`) and the encoder path
(`fffffff005fdfb04`), all as ordered `(offset, value)` pairs **[E: `regprog.py`
output in `build/ds-video/jpeg-dec-regs.txt`]**:

| Offset | Reset | Decode | Encode | Reading |
| --- | --- | --- | --- | --- |
| `0x000` | `0` | `1` (kick), `3`/`4`/`2` on error paths | `1` | control / start-stop **[I]** |
| `0x008` | `0x13e` → `0x17f` → `0x143` | `0x141 \| (flags << 1)` | `0x147` | main mode word: bit8 always set, low bits = reset/idle/decode/encode **[I]** |
| `0x118` | — | `0` | `1` | decode/encode select **[I]** |
| `0x11c` | — | `1` | `0` | inverse of `0x118` **[I]** |
| `0x120` | `0` | — | — | **[U]** |
| `0x19c` | — | `1` (only if >1 codec) | — | per-codec enable **[I]** |
| `0x1a0`/`0x1a4` | — | `1`/`0x20` (only if 2 codecs) | `1`/— | second-codec enable / table select **[I]** |
| `0x1ac`–`0x1d8` | cleared to 0 | table base addresses (6, or 11 with chroma tables) | ditto | **Huffman/quant table IOVAs** **[I, strongly]** |
| `0x1000` | — | `0x3f` | `0x30` | AXI interface enable mask **[I]** |
| `0x1004` | polled until `0` | `1` | `1` | AXI kick / DMA enable, `0` = idle **[I]** |
| `0x1014` | `1` (×2) then `0` | — | value | interrupt ack **[I]** |
| `0x1018`,`0x101c`,`0x1024` | — | — | values | AXI thresholds **[U]** |
| `0x1028` | — | `0` | — | **[U]** |

The table deliberately stops there. A `regprog.py` sweep also shows writes to
`0x028`, `0x02c`, `0x034`, `0x038`, `0x094`, `0x098`, `0x124`, `0x148` and
`0x14c`, but that scan window spanned more than one function, so those offsets
are *not* yet attributed to the codec block **[U]** — do not use them without
re-checking the enclosing function.

Two things to keep honest:

- The software reset is a **four-write sequence on `0x008`**
  (`0x100` → `0x13e` → `0x17f` → `0x143`), with a poll of `0x1004` in between
  **[E]**. A driver that only writes `0x143` will not reset the block.
- The base HAL writes a "custom quantisation table" block to `base + 0x2000`,
  i.e. **outside** the DT's `0x2000` window **[E: `FUN_fffffff005fd1924`,
  loop copying `request+0x1c0` to `mmio+0x2000`, `len = request+0x198 >> 2`]**.
  Either the A9 aperture is larger than the ADT advertises or that path is
  dead on this SoC. **[U]** — probe `0x07b02000` before using it.

### 4.3 Decode parameters

`AppleJPEGDriver::startDecoder` (`fffffff005fca834`) copies the user struct
into `JpegRequest`; the checks and the copies give the parameter list
**[E, decompiled]**:

| struct word (u32 index) | lands at `JpegRequest+` | constraint | reading |
| --- | --- | --- | --- |
| `[0]` | `0x90` | — | image / `pixelX` |
| `[1]` | `0xf8` | `≥ 0x100` | output buffer size (≥ 256 B) |
| `[2]` | `0x94` | — | `pixelY` |
| `[3]` | `0xfc` | `≥ 7` | `decodeWidth`/`decodeHeight` low bound |
| `[5]`,`[6]` | `0x100`, `0x104` (u64 pair) | `≥ 4`, `≥ 2` | source dims |
| `[8]` | bits → `0xf0`,`0xf1`,`0xf3` | — | three flags (bit0/1/2) |
| `[9]`,`[0xa]` | `0x120` (u64 pair) | `[9] < 0x2000`, `[0xa] ≤ 0x1fff` | `xOffset`,`yOffset` |
| `[0xb]` | `0x118` via `convertToRequestSubSampleMode` | — | `AppleJPEGSubsampling` |
| `[0xc]`,`[0xd]` | `0x10` (u64 pair) | `[0xc] == 0` ⇒ synchronous, request freed | async refcon / context |
| `[0xe]`,`[0xf]` | `0x18` (u64 pair) | — | async callback |
| `[0x10]`,`[0x11]` | `0x20` (u64 pair) | — | **[U]** |
| `[0x12]` | `0x140` | — | codec / flags |
| `[0x13]`,`[0x14]` | `0x170`, `0x174` | `≥ 4`, `≥ 2` | output dims |

Subsampling enum → hardware mode and MCU size
**[E: `convertToRequestSubSampleMode` `fffffff005fcaaa0`, `getMCUSize`
`fffffff005fcb330`, tables at `fffffff0054c6110`/`0x612c`/`0x6148` and
`fffffff0054c1ec8`/`0x1edc`]**:

| `AppleJPEGSubsampling` | hw mode (`0x118`) | MCU | meaning |
| --- | --- | --- | --- |
| 0 | 3 | 16×16 | 4:2:0 |
| 1 | 1 | 16×8 | 4:2:2 |
| 2 | 0 | 8×8 | 4:4:4 |
| 3 | 6 | 32×8 | 4:1:1 |
| 4 | 3 | 8×8 | second 8×8 mode (hw submode 4) **[U]** |

Output formats: the decoder accepts only `'420v'`, `'420f'`, `'422v'`,
`'422f'`, `'444v'`, `'444f'` — bit-identical checks on the IOSurface pixel
format in `startDecoderGated` **[E: `fffffff005fd1adc` and
`fffffff005fd1b30` in the base HAL, and `fffffff005fe2b2c` ff. in the V9 HAL]**.
There is **no BGRA/RGB decode output**; RGB output requires the scaler/CSC
after the decoder **[E]/[I]**. Minimum input stream is `0x100` bytes and the
destination plane sizes are validated against width/stride/height with three
distinct error strings (luma plane, chroma plane, whole surface) **[E]**.

### 4.4 Encode parameters

`startEncoder` (`fffffff005fcb3e0`) mirrors the decode path with
`iPad`-side quality handling: `"unsupported encoding quality %d detected,
quality limit is %d"`, `"Horz (%d) and Vert (%d) Resolutions are not set"`,
`"provided header byte size %d > header buffer size %d"` **[E]**. The HAL
converts the memory image to the JPEG domain through
`encoder_req2setup`, which logs `unknown pixeformat 0x%x (%c%c%c%c)` and is
validated against a `CMP_YUV_*`/`IMG_YUV_*` enum: `CMP_YUV_411` rejected
outright, `IMG_YUV_422_2P→CMP_YUV_444`, `IMG_YUV_420_2P→CMP_YUV_444`,
`IMG_YUV_422_1P→CMP_YUV_444`, `IMG_YUV_420_2P→CMP_YUV_422` rejected **[E:
`FATAL ERROR: Check setup` strings `fffffff0054c65d5` ff.]**. So the encoder is
**4:2:0 or 4:2:2 in, 4:2:0/4:2:2/4:4:4 out**, with a small set of accepted
input layouts; the exact FourCC list is **[U]**.

### 4.5 Tables and DART

Huffman and quantisation tables are **built in software and fetched by the
block from IOVA** (`jpeg_huffman_set()`, `jpeg_qtbl_set()` error strings, and
the `0x1ac`–`0x1d8` address registers) **[E]/[I]**. That is exactly the shape
of the Rockchip VDPU720 design, and exactly what `v4l2-jpeg.c` produces
(§5.2) — the single biggest reuse win in this study.

The DART is mandatory in the iOS driver (`genIOVMSegments`, `setMemoryDesc`
per plane, DART error handler with fault-rate limiting) **[E]**. Note the
mapper properties: `mapper-scaler` declares `tail-padding = 1` and
`mapper-jpeg` `tail-padding = 2`, and `mapper-jpeg` additionally declares
`dead-mappings = 0x7ffff000/0x1000` **[E: ADT]**. Whatever the unit, JPEG is
declared to need *more* slack beyond the buffer end than the scaler, so a
Linux driver must over-map the destination generously (at least one extra
page, two to be safe) **[I]**.

## 5. Comparison with what Linux already has

### 5.1 IOMMU

`drivers/iommu/apple-dart.c` already contains `apple_dart_hw_s5l8960x`
(`apple,s5l8960x-dart`) **[E: local tree, `commit 6831bc701a`]**, which is the
exact DART generation in this SoC. **The DART side is a DT problem, not a
driver problem** — the A9 DTS has no IOMMU node today **[E: `grep -rn dart
s800-0-3.dtsi s800-0-3-pmgr.dtsi s800x-ipad5.dtsi s8000.dtsi s8003.dtsi` is
empty]**, while the compatible mainline expects is `apple,s5l8960x-dart` and
the ADT calls it `dart,s8000`/`dart,s5l8960x` **[E]**. `stt-idx-width = 10`
means 1024 stream table entries; the *stream ID* each engine uses is not in
the ADT **[U]** and must be probed (`iommus = <&dart_jpeg N>`; wrong N ⇒ first
DMA faults, which is recoverable).

### 5.2 JPEG

Nothing Apple-specific exists; Asahi's video work is the **AVD decoder**
(`media: apple: add avd driver`, V4L2 stateless, M1/M2/M3) **[E: mailing-list
series, Sept 2026]** and does not cover JPEG or A9. Reusable pieces, best
first:

1. **`drivers/media/v4l2-core/v4l2-jpeg.c`** (713 lines) — parses a JPEG
   header and produces quantisation tables (zigzag→raster) and Huffman
   mincode/value tables. Used by `imx-jpeg`, `s5p-jpeg`, `hantro`, `coda`,
   `e5010-jpeg-enc` **[E: local tree]**. This is precisely the "side buffer"
   our block wants at `0x1ac`–`0x1d8`.
2. **`rockchip/rkjpegd`** (VDPU720, posted Aug 2026) — the closest structural
   match found: stateful V4L2 m2m JPEG→NV12, CPU-side header parse for
   `V4L2_EVENT_SOURCE_CHANGE`, Q-table/Huffman side buffers rebuilt per frame,
   start-byte offset, MCU-aligned height, DRI, grayscale chroma fill **[E]**.
   Copy its *shape* (queueing, source-change, error handling), not its
   registers.
3. **`s5p-jpeg`** — the only long-standing mainline driver that does **both**
   decode and encode on one block with `V4L2_JPEG_HELPER`; its encoder-side
   quality/marker plumbing is the model for our `0x4.4`.
4. **`e5010-jpeg-enc`** — newest encoder-only m2m driver, good reference for
   the encode-only control set (`V4L2_CID_JPEG_COMPRESSION_QUALITY` etc.).
5. `imx-jpeg`, `hantro_jpeg.c` — secondary references for slot management and
   table layout checks.

### 5.3 Scaler / CSC / 2D

No Apple scaler driver exists anywhere. Closest mainline analogues **[E]**:

- **`rockchip/rga`** — V4L2 m2m 2D engine: scaling, CSC, rotation, flip,
  alpha, tiled formats, per-job command lists. Structurally the nearest thing
  to "MSR4 + APIODMA".
- `verisilicon/hantro_*` and `mediatek/mtk-jpeg` — job-descriptor driven
  m2m blocks, useful as m2m plumbing references.
- `v4l2-mem2mem` core + `vb2-dma-contig` — for the buffer model. Note the
  scaler needs **IOVA** buffers (`dma-contig` with the DART attached), and
  `dma-contig`'s SG handling must tolerate the block's alignment rules
  (`IOSurfaceAcceleratorCapabilitiesInterchange{Stride,Base}Alignment`
  publishes them) **[E]**.

For a compositor, note honestly: a V4L2 m2m scaler is **not** what Wayland
compositors use for scanout scaling; it can accelerate GStreamer's
`v4l2convert` path and app-side image scaling today, and becomes interesting
for the desktop when the display pipe (`adbe`) lands and the scaler can be a
DRM/KMS plane helper or a writeback/capture engine **[I]**.

## 6. Ranked Linux plan

Ordered by *value per unit of risk*, not by hardware order. Each step is
independently useful and ends with a device test that can fail safely.

### P0 — Device-side oracle (no kernel code, ~1 round)

Goal: replace `[U]`s with measurements before writing a driver.

1. On the jailbroken iPad read the published properties:
   `ioreg -l -w0 -p IODeviceTree -n scaler0 -r` and `-n jpeg -r`, grepping
   `IOSurfaceAccelerator*` and `AppleJPEG*`. Expected: the capability block
   from §3.5, `IOSurfaceAcceleratorScalerVersion`, and
   `AppleJPEGNumCores = 1` / `AppleJPEGRequiresMCUAlignment = 0` **[E: these
   are `setProperty` calls in the kext]**.
2. Turn on the built-in dumps (`TriggerOneTimeRegistersDump`,
   `TransformDumpLevel`, `EnableStickyRegistersDump`) and trigger one
   `IOSurfaceAccelerator` transform from a small test app, then capture
   `dmesg`/kprintf. Expected: `MSR N Register Dump` and/or
   `APIODMA: Register sequence dump: Transform id = 0x…`, i.e. **a real
   `(offset,value)` program**.
3. Same for JPEG: decode a 16×16 and a 1920×1080 baseline JPEG, capture the
   "input struct … pixelX … decodeWidth" log and the register dump.
4. Probe `0x07b02000` (the table window, §4.2) and read `MSR_GLBL_IRQSTS`
   candidates in `0x0c`–`0xa4`.

Pass criteria: capability values captured, at least one full register program
per block, no panic. This step alone de-risks everything below and needs no
kernel work.

### P1 — JPEG decode, V4L2 m2m (rank 1)

Why first: one block, one direction, plain MMIO, an upstream helper that
already produces the tables, and an existing test corpus (any JPEG file).

1. DT: add `dart-jpeg` (`apple,s5l8960x-dart`, `reg = <0x7b04000 0x2000>`,
   `interrupts = <AIC_IRQ 176 …>, <AIC_IRQ 177 …>`, `#iommu-cells = <1>`) and
   `jpeg` (`reg = <0x7b00000 0x2000>`, `interrupts = <AIC_IRQ 175 …>`,
   `iommus = <&dart_jpeg SID>`, `power-domains = <&ps_jpg>`, `clocks`) to
   `s800-0-3.dtsi`, in the style of the existing `0035-arm64-dts-apple-add-
   isolated-J71t-peripheral-wiring.patch` **[E: that patch exists]**. The
   power domain is already there: `ps_jpg` in `s800-0-3-pmgr.dtsi` **[E]**;
   the clocks (ADT clock ids 289/290) need `apple,pmgr-clk` nodes if they are
   not already modelled **[U]**.
2. Bring up **without** the IOMMU first (no `iommus`, physical addresses): the
   block is simplest to debug, and it isolates "does the register program
   work" from "is the SID right".
3. Driver `drivers/media/platform/apple/apple-jpeg.c`: `v4l2-mem2mem`,
   `V4L2_CAP_VIDEO_M2M_MPLANE`, output = `V4L2_PIX_FMT_JPEG`, capture = NV12
   (`420v`/`420f` → limited/full range), plus NV16 for `422*`. Header parse
   with `v4l2_jpeg_parse_header()`; tables into a coherent side buffer.
4. Program the block exactly as §4.2/§4.3: reset sequence → mode word →
   `0x118/0x11c` → table IOVAs → `0x1000 = 0x3f` → `0x0 = 1` → `0x1004 = 1`;
   wait for IRQ 175, then `0x1014` ack and `0x0 = 0`.
5. Attach the DART once the program works: `iommus = <&dart_jpeg SID>`,
   probe SID 0 first, then 1..3; a wrong SID gives a DART fault on the first
   descriptor fetch (recoverable, log it).

Tests: `v4l2-compliance -d /dev/video0` (expect 0 failures on the m2m ioctls),
GStreamer `v4l2jpegdec ! videoconvert ! fakesink` over a small corpus
(baseline 4:2:0/4:2:2/4:4:4, progressive = must fail cleanly), and md5
comparison against `djpeg` output. Grayscale must produce a neutral chroma
plane (the decoder leaves it alone — same trap the Rockchip driver documents).

### P2 — JPEG encode (rank 2)

Same driver, second video node. Reuse `v4l2-jpeg`'s table generation in the
other direction and the encoder register path (§4.2, `0x147` mode, `0x1000 =
0x30`). Start with 4:2:0 in → 4:2:0 out at a fixed quality, validate by
decoding the result with the P1 decoder, then with an independent decoder.
Blocked on **[U]** items only: exact input FourCC set, quality field encoding,
and whether `0x2000` table upload is needed.

Tests: encode 4:2:0 and 4:2:2 stills, compare byte size/PSNR against
libjpeg-turbo at the same quality, then run 30 encodes back-to-back to shake
out the power/idle sequencing (`"wait for idle done, powering off"`).

### P3 — Scaler as a V4L2 m2m converter (rank 3)

Why third: highest value for the desktop eventually, but it depends on P0's
captured register program and on the APIODMA question.

1. First milestone is **1:1 copy of one BGRA surface** with identity CSC —
   proves DART mapping, kickoff, completion and IRQ handling without touching
   scaling maths.
2. Then scaling with the default filter set (`initDefaultFilters`), then
   NV12→BGRA CSC (`configureCSC_gatedContext` with a pre-CM matrix), then the
   7 flip/rotate combinations.
3. Expose as `V4L2_CAP_VIDEO_M2M_MPLANE`: output/capture = the §3.5 format
   list restricted to what we have verified; publish filter taps/phases and
   min/max sizes from the capability properties (P0) rather than constants.
4. APIODMA: the packet layout is known (§3.4), so build `apple_msr_stream`
   (header + burst values + DART-mapped buffer) as the *only* submit path from
   the start — retrofitting it later would mean rewriting every programming
   function. Keep a `msr_direct_mmio` debug option to test the cheaper
   hypothesis on day one (P0 step 4).

Tests: `v4l2-compliance`, then a scaling/CSC matrix over a 1:1, 2× down, 2× up
and 90°-rotate set with per-pixel comparison against a software reference
(`v4l2convert` and a `swscale` reference), and a 2048×1536 → 1024×768 loop to
measure throughput on the A9's memory bandwidth.

### P4 — Wire it into the desktop (rank 4)

Only after P1–P3: GStreamer `v4l2convert`/`v4l2jpegdec` autoplugging via
`/dev/video*`; a still-image "decode+scale" fast path for the image viewer;
and, once `adbe`/KMS exists, evaluate the scaler as a plane scaler or
writeback engine. Also revisit **`ave`** (`0x7cd0000` + `0x130000`, IRQs
166/160/159/162/161, `dart-ave` [E]) — a hardware H.264 encoder is a bigger
prize for "video output" than either block here, and it is a separate study.

## 7. Open questions

| # | Question | How to close it |
| --- | --- | --- |
| 1 | `MSR_GLBL_IRQSTS` offset, and the full MSR4 register map | P0 register dump; `regprog.py` over the MSR4 interrupt handler once its function start is known |
| 2 | Does the MSR block accept CPU-written registers, or only APIODMA packets? | P0 sequence dump + a `devmem` write experiment on `0x07900000` |
| 3 | Meaning of the packet header's `flags` field and of the 2-bit `stream+0x6c` field; how a transform tells the engine which base index to start at | read a real sequence from P0, then diff it against a single-packet transform |
| 4 | DART stream IDs for `scaler0` and `jpeg` | probe `iommus` N = 0..3; wrong SID faults cleanly |
| 5 | Is the JPEG aperture larger than the ADT's `0x2000`? (the decoder writes a table block at `base+0x2000`) | read `0x07b02000` on the device; if it faults, that path is dead on A9 and the P2 encoder must load tables through the `0x1ac`–`0x1d8` addresses only |
| 5b | Where is the scaler's separate SoC-version register map? (MSR15/16 create one; the `0x6378` reads are outside the 16 KiB bank) | grep the device tree for a second `reg` on the scaler provider, or read the HAL's mapping log line `Scaler %d RegisterMemoryMap` with `TransformDumpLevel` on |
| 6 | Which `ps_*` PMGR nodes correspond to the DART gates (ADT 33/34) and to the JPEG's second clock gate (ADT 41) | `ps_msr`/`ps_jpg` are already identified **[E]**; for the rest, read the PMGR gate registers on the device and match against `s800-0-3-pmgr.dtsi` |
| 7 | Encoder input FourCC set and quality encoding | decompile `AppleAJPEGHal::encoder_req2setup`, or P0 encode log |
| 8 | Does the scaler's 30-entry extents table have an A9 row, and which? | `initScalerPropertiesAndExtents` logs `Found properties and extents entry %u`; read it from P0 |
| 9 | Is the "DPE" aperture reachable on A9 (device-memory index 2 is absent)? | only matters for display-pipe colour work; check `AppleM2ScalerCSCHal`'s provider list |
| 10 | `AppleJPEGSubsampling` value 4 (8×8, hw mode 4) | irrelevant for a first driver; skip |

## 8. Reproduction appendix

```bash
# identities and ranges
tools/ds-video/kc-kexts.py list --filter Scal
tools/ds-video/kc-kexts.py list --filter JPEG
tools/ds-video/kc-kexts.py segs AppleM2ScalerCSCDriver
tools/ds-video/kc-kexts.py segs AppleJPEGDriver

# method maps (needs the shared Ghidra MCP server)
tools/ds-video/catalog.py scaler --mode sig
tools/ds-video/catalog.py jpeg   --mode sig

# find code for a literal Ghidra has no xref for
tools/ds-video/xrefs.py 0xfffffff0054c9983      # AppleAJPEGHal::reset_jpeg_block
tools/ds-video/xrefs.py 0xfffffff0054c9a50      # executeOperation_gated

# register programs
tools/ds-video/regprog.py --start 0xfffffff005fce830 --end 0xfffffff005fce998 --only-str
tools/ds-video/regprog.py --start 0xfffffff005fd1924 --end 0xfffffff005fd23a4 --md

# the scaler extents table (30 entries of 0x230 bytes)
tools/ds-video/kc-kexts.py dump 0xfffffff007a13170 --length 0x41a0 --out /tmp/props.bin
```

Decompiled snippets referenced above are saved in `build/ds-video/`
(gitignored): `jpeg-startdec-base.c`, `jpeg-dec-regs.txt`, `scaler-strings.txt`,
`jpeg-strings.txt`, `scaler-props-table.bin`.
