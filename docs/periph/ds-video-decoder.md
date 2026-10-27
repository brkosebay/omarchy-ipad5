# A9 (S8003, j71t) hardware video decoder — driver study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. iPad 5 (iPad6,11, S8003 / J71t), iPadOS 16.7.10 20H350. **No device
access was performed in this round**; every hardware statement below comes from
the saved ADT, the live `ioreg -a` dump already in `build/firmware/private/`,
the shared iOS 16.7.10 kernelcache, and public Linux sources. No commit was made
and no kernel branch was touched.

Evidence is tagged:

- **[E]** read directly from an artifact (ADT, ioreg dump, kext code/strings,
  public source file).
- **[I]** inference that fits the evidence but is not proven.
- **[U]** unresolved; a measurement or a further static/runtime check is needed.

## 0. Result in one table

> Private graphics-dependent excerpt omitted from this source snapshot.

One-line verdict: this is a **1080p-class Imagination MSVDX with a DEVA
firmware**, driven by host-side H.264/HEVC *parsing* and a firmware *message
queue*; a Linux driver is feasible, and the cheapest first milestone is to boot
the resident DEVA image on the MTX and read the message rings, not to replay
VideoToolbox.

## 1. Evidence identity and method

Kernelcache (all kernel addresses below are **unslid virtual addresses** in it):

```text
build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
SHA256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
```

Kext identity from the prelink dictionary (`build/periph/kexts/prelink.json`;
`tools/ds-video/extract-kexts.py` writes its own copy to
`build/video/kexts/prelink.json`):

| Bundle | `_PrelinkExecutableLoadAddr` | Size | `__text` | `__const` |
| --- | --- | --- | --- | --- |
| `com.apple.driver.AppleD5500` | `fffffff00555fa40` | `0x37bd8` | `fffffff006288000` +`0x48900` | `fffffff00557d120` +`0x1a4f8` |
| `com.apple.driver.AppleAVEH8` | `fffffff0053af980` | `0x95a25` | — | — (encoder, for comparison) |
| `com.apple.driver.AppleJPEGDriver` | `fffffff0054c1500` | `0xa1e5` | — | — (separate JPEG block) |

Extracted image (repacked with linked VAs preserved, so every address here can be
re-checked with `xcrun llvm-objdump -d`):

```text
build/video/kexts/AppleD5500.macho
SHA256 e38af565adc0f1828085e839b4eef3fb02252d4993aef08e22528bb01c737e23
```

Method, and its limits:

- **The shared Ghidra project has no functions in kext text.** Its `__text`
  segment only covers the main kernel; `get_function_by_address` in
  `fffffff006288000…` returns `FUNCTION_NOT_FOUND`, so decompilation of this
  kext there is not possible today (this matches what the touch/ALS lanes hit).
  The kext bytes *are* mapped, so `read_memory` works; that is how the vtables
  were sampled.
- Everything below was therefore read from the **extracted Mach-O** with a
  symbolizer written for this lane:
  `tools/ds-video/anno.py` resolves every `adrp`/`add`/`ldr` pair to the
  `__cstring`/`__os_log`/`__const` string it points at, and labels every `bl`
  target. `tools/ds-video/funcfind.py` recovers enclosing bodies of
  vtable-only methods, `tools/ds-video/regmap.py` recovers the MMIO map,
  `tools/ds-video/fw.py` extracts the embedded firmware.
- No `IOExternalMethodDispatch` array exists in this kext (scanned `__const` for
  24/32/40-byte strides: only vtable runs). `externalMethod` is therefore a
  hand-written switch; **selector numbers are [U]** — and irrelevant for Linux,
  which needs the semantics, not Apple's numbers.
- Where a statement rests on a log string, the address is the code site that
  formats it, not a function entry.

## 2. Which block this is

### 2.1 Device tree, two independent dumps

`docs/adt/j71t.txt` and the live ioreg dump agree exactly:

```text
vxd@7F00000  (class AppleARMIODevice)
  compatible      "vxd,s8000", "vxd,s5l8920x"
  device_type     "vxd"
  vxd-version     7
  reg             0x7f00000, 1 MiB          (single region)
  interrupts      157  (interrupt-parent = AIC phandle 22, "aic,1", v2)
  power-gates     119, 40
  clock-gates     119, 40
  clock-ids       285
  h264-playback-level 42
  allow-oold      0
  (no iommu-parent)
```

The `vxd,s5l8920x` compatible string is inherited from the **A4 era** — Apple has
shipped this IP family since at least S5L8920. That is why the driver supports
`vxd-version` 1…12 and why the string table still contains `AppleVXD309` /
`AppleVXD393` log text from older cores **[E]**.

### 2.2 Versions across SoCs (from `docs/adt/`)

| SoC | ADT file | `vxd-version` | regions | `h264-playback-level` |
| --- | --- | --- | --- | --- |
| S8000 (A9) | `j71t` | 7 | `0x7f00000`+1 MiB | 42 |
| S8001 (A9X) | `j98a`, `j99a`, `j127` | 9 | + `0x7f30000`+4 KiB | 42 |
| T8010 (A10) | `j171`, `j172` | 10 | `0x8100000`+0x30000, +`0x8130000`+4 KiB | 42 |

The extra 4 KiB page from version 9 on is the **integration** register block:
`AppleD5500::start` maps `IODeviceMemory` index 0 as `m_pMemBaseRegVirtual_*` and,
only for `vxd-version` 9, 10 and 12, index 1 as
`m_pRegisterBaseVirtual_VDEC0_Integration` (`fffffff006288a88…0x6288b0c`) **[E]**.
A9 does not have it, so a j71t driver needs exactly one mapping.

### 2.3 Why this is MSVDX and not AVD

| | this block (`AppleD5500`) | Apple AVD (M1+) |
| --- | --- | --- |
| CPU in the block | **MTX/META** (`TO_MTX_RD_INDEX`, `m_ui32MtxMemorySize`, `Mtx Memory Config`) **[E]** | Cortex-M3 **[E]** (public Asahi material) |
| Programming model | command buffers + register banks (`MSVDXDriver`, `CmdBuf.c`, `RendecBlock.c`) **[E]** | one command register, ordered writes |
| Memory | host-built **GART** page tables **[E]** | Apple DART |
| Firmware | DEVA `REL_812.0_14_Deva`, 98 KB, in-kext **[E]** | separate, custom-written for Linux (Asahi `avd-fw`) |
| Codecs | H.264, HEVC, MPEG-4 **[E]** | H.264, HEVC, VP9, AV1 (M3+) |

The names in the binary are unambiguous Imagination **MSVDX** terminology:
`MSVDXDriver::addDescToGART`, `VXD_MSVDX_ERROR_DEVICE_NOT_POWERED_UP`,
`MSVDXISR_FLAG_HWPANIC`, `GetStoredMSVDXParamMemInfoFromPool`,
`AllocMSVDXMemory: Failed to create FE State Buffer`, `VLC table area` **[E]**.

## 3. Hardware integration

### 3.1 What the driver maps

`AppleD5500::start` (`fffffff006288408`, epilogue at `…0x6288734`, cold paths
follow) does, in order **[E]**:

1. `super::start`, command gate, **two** timer event sources (`core 0`, `core 1`),
   FairPlay service reference, `IOCoreSurfaceRoot`.
2. `IORegistryEntry::fromPath("/arm-io/vxd")`, read `vxd-version`, then a
   12-way jump table (`__text+0xf78`) that turns the version into a
   `deviceType` stored at `this+0xb88`. Unknown values log
   `"Unrecognized deviceType %x!"` (`fffffff006288a10`) and abort.
3. Map device memory index 0 (core registers); index 1 only for types 9/10/12.
4. Power on, `new CPVDECCore`, `PrepareCore`, `LoadFirmwareAndStartCore`.
5. Advertise capabilities as IORegistry properties (see §5.5), then power down
   again unless the boot-arg `m_forceVXDPowerOnAtBoot` is set.

Device addresses in the ADT use the `arm-io` window; Linux DTS numbers are
`0x2_00000000 + adt` (checked against `aic` `0xe100000` → `0x20e100000` and
`dwi` `0xe200080` → `0x20e200080` in `s800-0-3.dtsi`) **[E]**. So the VXD node
for Linux is `reg = <0x2 0x07f00000 0x0 0x100000>`.

### 3.2 Interrupt

`interrupts = <157>`, level, AIC v2 **[E]**. The handler object is created with
`addInterruptEventSource` on the provider; `ProcessInterrupt()` indexes per-core
state with a `0xb70`-byte stride (`umaddl x22, w2, #0xb70, x0`) and keeps a
history of recently submitted 32-bit command/message ids — count at `+0x27c8`,
entries from `+0x2804` — which the panic path scans for the ids `0x80`, `0x100`
and `0x8010` before printing
`"ProcessInterrupt() MSVDXISR_FLAG_HWPANIC i %d command %x"`
(`fffffff00628e6xx`) **[E]**.

Practical note for Linux: the AIC driver in this tree already matches
`apple,s8000-aic`, so `request_irq(157)` is all that is needed — but the block
will not assert anything until firmware is running (§4) **[I]**.

### 3.3 Power and clocks

| Item | Value | Meaning |
| --- | --- | --- |
| `power-gates` | 119, 40 | PMGR `VDEC` and `VDEC-FAST` **[E]** (the PMGR `devices` list names both, ids 119 and 40) |
| `clock-gates` | 119, 40 | same two gates double as clock gates **[E]** |
| `clock-ids` | 285 | PMGR clock id, i.e. the VDEC functional clock **[E]/[I]** |
| Linux `ps_vdec` | `power-controller@802f8`, parent `ps_media` | already present in `s800-0-3-pmgr.dtsi` **[E]** |

> Private graphics-dependent excerpt omitted from this source snapshot.

### 3.4 There is no DART — the core has its own MMU ("GART")

This is the single most important difference from every other peripheral on this
board. `vxd` is the only video-class node without `iommu-parent` **[E]**, and the
driver instead builds page tables itself **[E]**:

```text
AppleD5500::initialize_gart … "setting 2GB mapping" / "setting 1GB mapping"
"Failed to create directory table or page table descriptors: %p %p"
"initialize_gart -- pageTable %x  directoryTable %x"
"Physical Addresses - pageTableDesc %x  directoryTableDesc %x"
"MMU Config[0] %x 40 bit MMU? %d"   "MMU Config[1] %x tiling stride per memory context? %d"
MSVDXDriver::addDescToGART … clearMemoryDescriptor()
```

So the model is: allocate a **directory table + page table(s)** in normal memory,
write their *physical* addresses plus an MMU mode into the core, then map every
buffer (bitstream, output surfaces, parameter memory) into that 1–2 GB device
address space with a range allocator **[E]**. Page size is presumably 4 KiB
(`page-size` on the DARTs is `0x1000`; the GART code speaks of "segments")
**[I]**.

For Linux this is good news: no IOMMU driver work, no `apple-dart` dependency —
just `dma_alloc_coherent` for the tables and a small allocator, or even a
1:1 identity map if the aperture is large enough (2 GB covers all of DRAM on a
2 GB iPad) **[I]**.

### 3.5 Register map (recovered, not guessed)

`tools/ds-video/regmap.py` walks every `mov w1, #imm` that feeds the two
accessors `CPVDECCore::ReadReg` (`fffffff0062902b0`) and `WriteReg`
(`fffffff0062902e0`) — both are `*(u32 *)(gRegBase + off)` with an optional
byteswap for a "big endian" core mode **[E]**:

| Offset | r/w | What the code does with it |
| --- | --- | --- |
| `0x0088` | w | written once during core init |
| `0x0200`, `0x0208` | w | VEC block setup |
| `0x0340`, `0x0344` | w | VEC/ENTDEC setup (entropy decoder) |
| `0x0400` | r/w | CMD block: polled and armed |
| `0x0410`, `0x0414`, `0x0418` | r/w | CMD/DMAC descriptors |
| `0x0440` | r/w | clock/control; written then read back (`SetClocks` sanity check) |
| `0x0460` | **w ×11** | command kick / doorbell |
| `0x0500`, `0x0504` | r/w | DMAC status |
| `0x0630`, `0x0640` | r | **core id** and **core rev** (`GetCoreRev`, `fffffff006298430`) |
| `0x1000`…`0x1084` | r/w | MTX block (control/status) |
| `0x2000`…`0x2030` | r/w | **host↔firmware message queues** (base, size, 4 indices) |
| `0x3020`, `0x3030`, `0x3038` | w | per-pipe setup |
| `0x4004`, `0x4020`, `0x4060`…`0x4078` | w | render/entropy pipe config |
| `0x40c0`, `0x40c4` | r | pipe status |
| `0x6a00`…`0x6a18` | w | **MTX boot / firmware load** (see §4.3) |

Names in the block are [I] (the offsets are [E]); they line up with the public
MSVDX bank layout used by Imagination-derived drivers (`0x200` VEC, `0x340`
VEC_ENTDEC, `0x400` CMD, `0x500` DMAC, `0x600` MMU) and with TI's
`img_msvdx_cmds.h` command-header definitions.

## 4. Firmware

### 4.1 It is in the kext, not on a filesystem

There is **no `__firmware` section** and no `firmware-name` in the ADT **[E]**.
Instead `__DATA` holds a record (found by `tools/ds-video/fw.py` by pattern, so
it survives kext version changes):

```text
record @ fffffff007a6e658
  +0x00 char *name     -> "REL_812.0_14_Deva"
  +0x08 char *build    -> "08/11/2016-17:08:44.79"
  +0x10 u32  mtx_need   0xa1a0   (41,376 B the MTX must have)
  +0x14 u32  blob_size  0x17e98  (97,944 B)
  +0x18 u8  *blob      -> fffffff00557d148  (== __const + 0x28)
  +0x20 u32  flags      0x04000000 static; the resolver ORs 0x80000000 in at boot [I]
  +0x24 u32  build_id   0x9c92d132   (not CRC32 of the blob [E]; meaning [U])
```

The blob is MTX code (`0x93fe9421 0x0e42c809 …`, 87,337 of 97,944 bytes non-zero),
`sha256 bf5167652e81540471aaca141943fd33d71c3990c137f145471305f5f5ade83b` **[E]**.

Load path (`AllocateFirmwareBuffer`, `fffffff006299f50`) **[E]**:

```text
kalloc_type(CFirmwareInstance)
  w1 = m_ui32MtxMemorySize          (from core register/init)
  create firmware info              ("Failed to find create firmware" on failure)
  check: if (mtxMemorySize <= need) -> "Insufficent MTX Space for Firmware"
  uiWords = blob_size >> 2; pui32 = record->blob
  IOBufferMemoryDescriptor(uiWords*4)
  fill 0xcdcdcdcd, then memcpy(firmware)     ("Copy firmware into firmware buffer")
```

**Licensing/practical**: the image is Apple's (Imagination's) property. Do not
commit it. `build/` is gitignored, and the extraction is reproducible from the
kernelcache we already keep; a distribution would have to either require the
user to supply their own dump or use a replacement firmware.

### 4.2 It is a real RTOS-ish firmware with a mailbox

`CPVDECCore` implements `FirmwareInteruptHandler()` and
`ProcessFWMessages()` **[E]**; the message plumbing is visible in
`DEVA`/`DEBUG` naming and in the ring indices that get dumped on every recovery:

```text
"indices TO_MTX_RD_INDEX %x TO_MTX_WRT_INDEX %x TO_HOST_RD_INDEX %x TO_HOST_WRT_INDEX %x"
"received oversize fw->host msg"     "%s too many messages in fw2host q"
"FirmwareInteruptHandler - FW_DEVA_PANIC, ui32MsgId %x, uiErrorInt %x (%s)"
"ProcessFWMessages: FW_DEVA_PANIC - ui32Fence [%d %x] uiErrorInt [%d %x]"
"ERROR: Unknown command received from the DEVA firmware"
"ERROR: Completion after core reset"        "Core WDT"
```

Two unidirectional rings (host→MTX, MTX→host), each with read/write indices in
the `0x2000` register bank, message IDs where `0x8010` and friends are
firmware→host classes **[E]/[I]**. This is the same `FW_DEVA_*` message family
that TI's `img_vdec_fw_msg.h` documents (`FW_DEVA_DECODE`, `FW_DEVA_COMPLETED`,
`FW_DEVA_PANIC`, `FW_DEVA_GENMSG`, `FW_DEVA_SIGNATURES`) **[E]** — strong evidence
that Apple's blob and TI's blob descend from one firmware codebase.

### 4.3 Boot sequence, as the driver performs it

From `PrepareCore` + `LoadFirmwareAndStartCore` (`fffffff006289a5c`,
`fffffff006289cc8`) and the register writes at `fffffff006298a40…0x6298b40` **[E]**:

```text
"Bring core into known state"      (reset, VEC/ENTDEC/CMD init at 0x200/0x340/0x400)
read MTX memory size, read MMU mode (40-bit if core+0x3c != 0)
HEVC dual-pipe limits              (m_muiHevcMinHForDualPipe, m_muiMaxHForPipeWait)
allocate firmware buffer, set "FirmwareVersion" property
power up MSVDX, initialise GART
program MTX boot block:
   0x6a00 = firmware buffer address field (from the buffer object at core+0xa8) [I]
   0x6a04 = 0x22000000 | fwWords          (fwWords must be < 0x10000; assert in code)
   0x6a08 = 0x01000000
   0x6a0c = 0
   0x6a14 = 0x350
   0x6a18 = 7                              (enable)
poll 0x2014 for "firmware ready"  -> "Firmware ready indication - Poll returns %d"
read 0x2024 / 0x2018 -> firmware build halves (stored at core+0x40..0x48)
```

Two details worth keeping: the 0x10000-word limit on the firmware (matches
24,486 words for this image), and the **polled** ready handshake rather than an
interrupt **[E]**.

## 5. The iOS driver

### 5.1 Layers

The kext is thin Apple glue over Imagination's driver, which is why the source
paths in the binary are so informative **[E]**:

| Layer | Files | Role |
| --- | --- | --- |
| Apple | `AppleD5500.cpp`, `AppleD5500UserClient.cpp` | IOService, user client, IOSurface, FairPlay, power management, scaler |
| IMG kmDrv | `IMG/source/kmDrv/CPVDECCore.cpp`, `CPVDECCore_Clocks.cpp` | core object, firmware boot, MMU/GART, interrupts, commands |
| IMG cmd_buf | `IMG/cmd_buf/CmdBuf.c`, `LLDMA.c`, `RegBlock.c`, `RendecBlock.c` | command buffer builder (register writes + DMA descriptors) |
| IMG codec | `IMG/codec/h264/{CH264Decoder,h264_host,h264CmdBuf,host_dpb,host_drpm,host_picture,host_rpl,rpl,slice,picture,addrman}.cpp`, `IMG/codec/hevc/{CHEVCDecoder,hevcCmdBuf}.cpp`, `IMG/codec/mpeg4/*` | **host-side bitstream parsing**, DPB, per-slice command generation |

Object inventory from the `__kalloc_type` site strings **[E]**:
`AppleD5500`, `AppleD5500UserClient`, `CPVDECCore`, `CFirmwareInstance`,
`CH264Decoder`, `CHEVCDecoder`, `CMPEG4Decoder`, `CPicture`, `AVC_decoder`,
`AVC_RBSP`, `HEVC_RBSP`, `PicParams_H264`, `PicParams_MPEG4`, `Qmatrix_H264`,
`Slice_H264_Long`, `VA_SliceInfo`, `VA_QmatrixData`, `H264CommandBufferParams`,
`H264_STORED_FRAME_PARAMS`, `HEVC_STORED_FRAME_PARAMS`,
`MPEG4CommandBufferParams`, `MPEG4_STORED_FRAME_PARAMS`, `msvdx_surface`,
`msvdx_client_mem_info`, `kern_mem_info`, `PriorityQueue`, `StreamType`.

The `VA_*` and `*_STORED_FRAME_PARAMS` naming is the giveaway: this is a
**VA-API-shaped** internal interface (`va_api: VAMapPixelBuffer`,
`VADecodeBeginFrameH264VLD`, `VAReleaseOutputSurfaceH264VLD`,
`VACommandBufferGenerateFrame`) where "surfaces" are IOSurfaces **[E]**. A
V4L2 stateless driver maps onto this almost one-to-one.

### 5.2 Userspace ABI (VideoToolbox side)

Methods visible as log sites and structure names **[E]**:

```text
AppleD5500UserClient::withTask / start / clientClose / clientDied
  registerNotificationPort(mach_port_t, type, ref)      "entered with mach_port_t 0x%08p"
  SetCallback(AppleD5500KernelCallbackInStruct*, …OutStruct*)   completionCallback + refCon + asyncWakePort
  CreateDecoder(codec, IOSurface input)     "input IOSurface map failed", "unsupported codec type"
  setResolutionInfo(contextID, …)
  StartDecode / StartDecodeFig              "bad input size %d"
  DecodeFrame / DecodeFrameFig              "bad input size %d", "frameNumber %d submitError %d"
  StopDecode, DestroyDecoder, DeallocateUserClientKernelMemory
AppleD5500 structure names: AppleD5500DecodeFrameFigInStruct / …OutStruct,
  AppleD5500KernelCallbackInStruct / …OutStruct
```

Per-context state is addressed through a **0x410-byte-stride** array in the decode
submit path (`mov w8, #0x410; madd x8, x27, x8, x21`), and `ProcessInterrupt`
uses a **0xb70**-byte stride per core (`umaddl x22, w2, #0xb70, x0`), with the
command history at `+0x27c8` **[E]**. `m_numActiveUserClients` /
`m_hwContextIDCount` show the block arbitrates several decode contexts through
an `AppleD5500PriorityQueue` **[E]** — a Linux driver needs exactly one.

### 5.3 Per-frame flow

```text
CreateDecoder      -> allocate surface pool, mb-params / render buffers, command buffer
StartDecode        -> VADecodeBeginFrameH264VLD  (dest surface, width/stride checks)
DecodeFrame(Fig)   -> parse (host) -> submit (command buffer) -> firmware messages
ProcessInterrupt   -> ProcessFWMessages -> completion / error, release surface
StopDecode         -> flush DPB, release surfaces
```

Host-side parsing is real work, not a stub: `AVC_RBSP`/`HEVC_RBSP` walk SPS/PPS/VPS
and slice headers (`HEVC_RBSP::parseSPS`, `parsePPS`), and the codec objects build
per-picture parameter blocks (`h264CmdBuf.cpp`, `hevcCmdBuf.cpp`). Errors such as
`"SPS chroma format 4:2:2 not supported! Bitstream requested %x"`,
`"Frame resolution change not supported"`, `"SPS changes not supported!"` come
from that parser **[E]** — so the iOS path is **one stream configuration per
decoder instance**, not mid-stream reconfiguration.

The slice path is the interesting one for a Linux driver:

```text
CH264Decoder::DecodePictureInternal  -> H264FW_NalPushSliceData / H264FW_NalPushAllSlice
DecodeExecuteH264VLD                 -> GetStoredMSVDXParamMemInfoFromPool
                                        DecodeGetRenderTarget, bitstream surface
vaEndFrame
```

`H264FW_NalPushSliceData` puts **bitstream bytes** into a firmware message **[E]**;
that is the evidence that the DEVA firmware sits in the data path at least for
NAL delivery (§6.1).

### 5.4 Surfaces, DPB, output layout

- Output/DPB buffers are **IOSurfaces** mapped into the GART; the driver locks and
  unlocks them (`deviceLockSurface`, `unlockSurface`, `kAllocMapTypeIOSurface`)
  and gets `lumaOffset`, `chromaOffset`, `lumaStride` per surface
  (`ChooseAltConcealmentFrame - pMemInfo->lumaOffset …`) **[E]**.
- DPB bound is **22** surfaces: `cmp w1, #0x16` guards the path that logs
  `"Index Out of Bounds [uiSurfaceIndex %d, (DPB_MAX_BUF + MAX_BACK_BUFFERS) %d]"`
  **[E]**.
- **Tile decode**: `H264DecoderSupportsTileDecode` is set true, and the user client
  takes a tile rectangle (`kVASetTileDecodeParams`, `"tile offset out of bounds!
  [%u %u] + [%u %u] …"`, `"Rejecting non-IDR frame in tiled decode"`,
  `"Rejecting non-IRAP frame in tiled decode"`) **[E]**. My reading: VideoToolbox
  can ask for a *sub-rectangle* of a picture, which is how 4K content is decoded
  on a 1080p-class core, and the driver forces IRAP-only inside a tile
  **[I]** — worth confirming, because it also tells us the real max picture size.
- Chroma: 4:2:0 is the baseline; 4:2:2/4:4:4 exist in the parser and are
  advertised as capability properties (§5.5), but the parser can still reject a
  specific stream (`"SPS chroma format 4:2:2 not supported!"`) **[E]**.

### 5.5 Capability gating by `vxd-version`

`start` sets these IORegistry properties (`fffffff006288b50…0x6288be0`) **[E]**:

| Property | Value | Note |
| --- | --- | --- |
| `H264DecoderCanDo444` | true | unconditional for rev 1–12 |
| `H264DecoderCanDo422` | true | unconditional |
| `H264DecoderSupportsTileDecode` | true | unconditional |
| `HEVCSupported` | `((rev-7) < 4) && ((0xD >> (rev-7)) & 1)` | **true for rev 7** (A9), false for rev 8, true for 9 and 10 |

Read literally: **this A9 advertises HEVC decode to VideoToolbox** **[E]**. The
bitmask `0xD` = rev 7, 9, 10, which is exactly S8000/S8001/T8010. It also matches
the fact that the kext carries a full HEVC parser and DPB for all revisions. It is
still **[U]** until we watch the VDEC power domain and interrupt count while
playing HEVC on iOS. (Public Apple guidance — "HEVC hardware decoding requires an
A9-class device or later" — is consistent with this.)

## 6. What a Linux driver must do

### 6.1 The one question that decides the architecture

Everything else is bookkeeping. The question is: **does the DEVA firmware decode,
or does it only schedule?**

Evidence for "firmware is in the data path":

- `H264FW_NalPushSliceData` / `H264FW_NalPushAllSlice` push NAL payloads into
  firmware messages **[E]**.
- `DecodeExecuteH264VLD`, `AllocMSVDXMemory: … VLC table area`,
  `"svec_base [] index out of range"` suggest the firmware programs the entropy
  decode tables **[E]/[I]**.
- TI's driver for the same IP family also runs a firmware (`vxd_fw.bin`) between
  the host parser and the hardware **[E]**.

Evidence for "the hardware decodes, firmware is a scheduler":

- The CMD/DMAC/VEC/ENTDEC register banks and the `MSVDX_CMDS` command-header
  format (`CODED_PICTURE_SIZE`, `OPERATING_MODE`, coefficient tables) describe a
  complete hardware picture-command interface **[E]** (TI `img_msvdx_cmds.h`).
- Apple's own AVD lesson: Asahi found that a firmware which only installs
  interrupt handlers was enough, because the *hardware* interface was
  understood **[E]** (public Asahi report).

**So the cheapest decisive experiment is not on iOS at all**: boot the resident
blob (§4.3 is a complete recipe), dump MTX memory and the two rings, then submit
one NAL and watch what the firmware writes back. If the host→firmware messages
contain only *addresses and parameters*, a host-only stateless driver is viable
and a small replacement firmware (Asahi's `avd-fw` is the model) becomes realistic. If
they contain bitstream bytes and per-slice commands, we either keep Apple's blob
(local-only) or reimplement that layer.

### 6.2 API choice

- **V4L2 M2M stateless** (`V4L2_CID_STATELESS_H264_*`) is the right target: the
  iOS driver is already stateless-shaped (host parses, per-picture/per-slice
  parameters, DPB managed by the driver), and FFmpeg's `v4l2request` hwaccel plus
  mpv/Chromium can consume it with no driver-specific userspace. rkvdec is the
  closest template.
- Stateful would require us to own a full H.264 parser in-kernel — that is exactly
  what Apple does *outside* the kernel ABI, and it would duplicate ffmpeg.
- Output formats: NV12 first (the iOS layout is plane-offset + stride based, so a
  linear NV12 surface is natural), then P010/NV16 if HEVC 10-bit and 4:2:2 are
  real on this core.

### 6.3 Things that are already solved for us

| Need | Status |
| --- | --- |
| Power domain | `ps_vdec` exists in `s800-0-3-pmgr.dtsi` **[E]** |
| IRQ controller | `apple,s8000-aic` in DTS, mainline AIC v2 driver **[E]** |
| IOMMU | none needed — own GART **[E]** |
| Command/parameter layout | TI's `img_msvdx_cmds.h`, `h264fw_data.h`, `hevcfw_data.h` document the field encodings **[E]** |
| Firmware image | in the kext we already have, extractable, hash above **[E]** |
| Reference driver structure | Apple's own file layout = Imagination's, and TI's tree mirrors it **[E]** |

## 7. Comparison with existing Linux work

| Project | Block | Relationship to us |
| --- | --- | --- |
| **TI `img-vxe-vxd`** (`drivers/media/platform/img/vxe-vxd`, out of tree, TI SDK 6.1/6.6/6.12) | D5520MP2 VXD + VXE384 | **Same IP family and same firmware architecture**: host H.264/HEVC "secure parsers" in the kernel, `fw_interface.h`, `img_vdec_fw_msg.h` (`FW_DEVA_*`), MSVDX command headers, `vdec_mmu_wrapper.c` for the core's own MMU. Its V4L2 interface is **stateful** (41 `v4l2_m2m_*` uses, zero `V4L2_CID_STATELESS_*`), i.e. the vendor design puts the parser in the kernel; we should not copy that choice, only the hardware structures. **Not mergeable as-is** (different core revision, different firmware packaging), but the best field guide for every structure we need. **[E]** |
| **Asahi AVD** (`[PATCH 00/14] media: apple: add avd driver`, Sep 2026) | Apple AVD (M1–M3) | Different block (CM3, single command register, needs `apple-dart`), so no code reuse. Reusable ideas: V4L2 stateless + rkvdec-derived framework, and the "replace the vendor firmware with our own" strategy (`AsahiLinux/avd-fw`). **[E]** |
| **rkvdec / hantro / meson-vdec** | Rockchip / Hantro / Amlogic | Framework templates only: rkvdec for stateless H.264/HEVC control handling, meson for a stateful design we are *not* choosing. **[E]** |
| Upstream mainline | — | No MSVDX/VXD driver exists. A Linux driver here would be the first, which means bindings + MAINTAINERS work but also no legacy constraints. **[E]** |

## 8. Ranked implementation plan

Each rung is independently useful, and each ends with a measurable result.

**R0 — probe without firmware (half a day).**
DTS node (`apple,s8000-vxd`, `reg = <0x2 0x07f00000 0x100000>`,
`interrupts = <157>`, `power-domains = <&ps_vdec>`, plus whatever clock phandle
the clock work settles on), a platform driver that powers the domain, maps MMIO,
reads `0x640`/`0x630`, prints `GetCoreRev`-style values, requests IRQ, and asserts
reset. Prove: power domain on, registers readable, no bus error. Risk: none (no
writes beyond the reset).

**R1 — boot the resident DEVA image (1–2 days).**
Implement §4.3 exactly: allocate firmware buffer (`dma_alloc_coherent`, 0x17e98
bytes, 4-byte aligned), program `0x6a00/04/08/0c/14/18`, poll `0x2014`, read
`0x2018/0x2024`, and compare the resulting build halves with what iOS reports.
Prove: our boot recipe is right. This is the gate for everything else.

**R2 — map the mailbox (2–3 days).**
Find the ring layout (registers `0x2008…0x2030` + the indices dumped by the
recovery path), dump MTX memory after boot, and get one benign firmware message
round-trip. Prove: the host↔firmware ABI, and answer §6.1 by *looking at what the
firmware expects*.

**R3 — one H.264 I-frame to NV12 (1–2 weeks).**
Minimal host parser (SPS/PPS/slice header only), per-picture parameter block in
the `H264CommandBufferParams`/`VA_SliceInfo` shape, one output surface via the
GART, IRQ completion. Prove: pixels. This is the first demo worth showing.

**R4 — DPB + P-frames + V4L2 stateless ABI (2–4 weeks).**
DPB (≤22 surfaces), reference lists, `V4L2_CID_STATELESS_H264_*` controls, M2M
with `vb2-dma-contig`, `v4l2-compliance`, then ffmpeg `-hwaccel v4l2request`.

**R5 — the rest.**
HEVC (parser we can lift conceptually from TI's `hevc_secure_parser.c`, but the
DPB/RPS logic must match this core), MPEG-4 Part 2, 10-bit/4:2:2 output, tile
decode (only if we ever need >1080p), and eventually a replacement firmware so
the driver is distributable.

**Fallback worth stating plainly**: if R2 shows the firmware owns too much of the
data path, the honest interim answer for Omarchy is a *software* H.264/HEVC
decoder (ffmpeg/dav1d class) — 1080p30 H.264 on two 1.85 GHz Typhoon cores is
plausible, HEVC is marginal. That keeps video playback working while the hardware
path matures, and it is the right thing to ship in the meantime.

## 9. Live data that would shorten this most

Ranked by value per hour of device time:

1. **MTX memory + both message rings after boot** (R1/R2). We can read this
   ourselves on Linux — no iOS needed. This alone answers §6.1.
2. **One-frame firmware message capture from iOS**: `AppleD5500` has verbose log
   points at every stage; a boot-arg/`AppleD5500` debug enable plus
   `log stream --predicate 'process == "kernel"'` would show command IDs,
   fence IDs and error codes for a known clip. (Userspace-visible only.)
3. **VideoToolbox user-client trace** (Frida on a media process): confirms the
   `AppleD5500*Struct` layouts and the exact per-frame call order. Gives the ABI
   but not the firmware protocol.
4. **Power/interrupt observation while playing**: `ioreg` VDEC domain state and
   the `vxd 0` IOReport interrupt counters, with H.264 vs HEVC clips. Settles the
   HEVC question in §5.5 in ten minutes.
5. **An IOSurface dump of a decoded frame** (luma/chroma offsets and strides), to
   confirm the linear-NV12 assumption before writing the capture queue.

## 10. Open questions

1. Is the DEVA firmware in the data path, or only scheduling? (**§6.1** — decides
   everything; answered by R2.)
2. Does `vxd-version 7` really decode HEVC? Property says yes; silicon unverified.
3. What is the real maximum picture size — level 4.2 / 1088 lines, or larger with
   tile decode? Are tiles a hardware feature or an Apple API convenience?
4. Which MSVDX core revision is "7"? (`GetCoreRev` requires major > 6; the
   `AppleVXD393`/`VXD309` strings are older cores.) The exact marketing name does
   not matter, but the *feature set* of rev 7 does.
5. Is the GART page size 4 KiB, and is a 1:1 identity map acceptable (2 GB
   aperture vs 2 GB DRAM)?
6. What does `MMU Config[1] "tiling stride per memory context"` do — is there a
   tiled output mode we should support, or is linear NV12 enough?
7. `clock-ids = <285>`: which PMGR clock is it, and does it need a rate set or only
   gating?
8. FairPlay decryption sits in the same parse path (`decryptFrame`,
   `decryptOneSample`). Not needed for local playback, but it explains some
   structure fields.

## Appendix A — tools added by this round

All output goes to `build/video/` (gitignored); nothing here touches the kernel.

```sh
tools/ds-video/extract-kexts.py                 # prelink inventory -> build/video/kexts
tools/ds-video/extract-kexts.py --list-re 'VXD|D5500|AVE'
tools/ds-video/anno.py  build/video/kexts/AppleD5500.macho  > …/AppleD5500.anno.txt
tools/ds-video/anno.py  … --grep VXD_MSVDX      # which instructions reference a string
tools/ds-video/funcfind.py …/AppleD5500.anno.txt 0xfffffff006292414
tools/ds-video/regmap.py  …/AppleD5500.anno.txt --merge
tools/ds-video/fw.py      build/video/kexts/AppleD5500.macho    # -> build/video/fw/vxd_deva_fw.bin
```

Notes: `anno.py` needs `xcrun llvm-objdump`; `regmap.py` defaults to the two
`CPVDECCore` accessors, so re-point `--read-helper/--write-helper` for a
different kext; `fw.py` finds the firmware record by pattern, not by address.

## Appendix C — round-1 driver addendum (this round's mining)

The driver round re-mined the kext with `tools/ds-video/anno.py` on the
extracted `AppleD5500.macho` and upgraded several [I]s to [E]. Results
live in the driver's comments and commit messages; summary:

- **MTX boot block order** (`LoadFirmwareAndStartCore` at
  `fffffff006289cc8`): `0x6a18 = 7` first (MTX clock enable), then
  `0x6a0c = 0`, `0x6a00 = fw buffer address` (`*(u32 *)(CFirmwareInstance
  + 0x10)`), `0x6a14 = 0x350`, `0x6a08 = 0x01000000`, and `0x6a04`
  written **twice**: `0x22000000 | fwWords` then `0x22010000 | fwWords`.
  The boot is preceded by two "bring core into known state" variants:
  v1 (`CPVDECCore` `fffffff006297f94`: `SetClocks(-1)`; per pixel pipe
  `0x460 = i`, `0x4020 = 0xffffffff`; per entropy pipe `0x460 = i`,
  `0x3020 = 0xffffffff`; `0x400 = 0xffff0000`, read, `0`, `1`, poll
  bit0 clear; `0x1008 = 0x10000000`, poll bit28 clear; tail `0x418 = 0`,
  `0x414 = 0xffff0000`) and v2 (`fffffff0062980fc`: same until the
  `0x400 = 0xffff0000, read, 0` pulse, `0x1008` block on the first
  attempt only, tail `0x414 = 0xffffffff`).
- **SetClocks** (`fffffff0062ba1fc`): `0x440` = `CR_PVDEC_MAN_CLK_ENABLE`;
  writes the default `0x11`, read-back checks, then the requested value.
  Default/known values from the kext constants: `0x11`, `0xffffffff`,
  `0x1010113` (`core[0x64] | 0x1000000`).
- **Poll helper** (`PollMSVDX fffffff0062c00c4`): `(val & mask) == match`
  with a 1 ms step; the ready poll is `0x2014`, mask -1, match
  `0xa5a5a5a5`, 1000 iterations.
- **GART** (`AppleD5500::initialize_gart fffffff0062891a8`): 4 KiB
  pages, 1024-entry directory, 512 page tables for a ≥ 2 GB device
  ("2GB mapping"; hw.memsize < 0x77359400 → 256), unmapped entries → a
  dummy page, **PTE = ((phys >> 12) << 4) | 1**.  The directory table's
  physical address is programmed at `ProcessorInitialise` as
  `0x1020 = dir_phys >> 8`, with `0x1010 = 0` and
  `0x1070 = (mmu40) << 4` (`mmu40 = ((0x1080 & 0xf0) == 0x80)`; MMU
  config read from `0x1080`/`0x1084`, bit 29 of `0x1084` = the tiling
  flag).  This settles study open question 6 partially: the tiling
  stride flag is read, its use is not.
- **Core discovery**: `coreId` (0x630) `& 0xf` = entropy pipes,
  `(coreId >> 4) & 0xf` = pixel pipes, and `coreId >> 16 > 6` is the
  unsupported gate — open question 4 answered structurally.  Device run
  1 confirmed the layout: `coreId = 0x03030811` (gate value 0x0303,
  pipes 1/1), `coreRev = 0x70b00` (printed only, never parsed by
  AppleD5500); the test plan's "0x0007xxxx" expectation was wrong, and
  the ADT `vxd-version 7` plausibly matches coreRev[23:16] = 0x7 [U].
- **MTX memory size** comes from `0x500` (bank count [11:8], bank size
  [19:16], last bank [27:24]) — the "DMAC status" name was wrong; it is
  the MTX memory configuration.  `0x504` is the DMAC status (written
  `0x80000000` before boot).
- **Comms ring indices** confirmed [E]: `0x201c`/`0x2020` = TO_HOST
  rd/wr, `0x2028`/`0x2030` = TO_MTX rd/wr; `0x2010` = message count
  (200-ish); `0x202c` = flags (`0x1000000 | 0x60` on the !core[0xc0]
  branch).  Message content transport (LLDMA over the comms window)
  remains [U] — R2.
- `core[0xb4] = 0xc8` (200): the MTX message count feeding the
  `0x208 = 0xc7` write.

## Appendix B — address index (all unslid VAs)

| Address | What |
| --- | --- |
| `fffffff00555fa40` | `AppleD5500` Mach-O header (prelink load address) |
| `fffffff00557d120` | `__TEXT.__const` — firmware blob at `+0x28` |
| `fffffff0055600e8` | `__TEXT.__cstring` |
| `fffffff005564d33` | `__TEXT.__os_log` |
| `fffffff006288000` | `__TEXT_EXEC.__text` |
| `fffffff006288408` | `AppleD5500::start` (entry; epilogue `…0x6288734`) |
| `fffffff0062888c8` | `vxd-version` property read |
| `fffffff006288a10` | `"Unrecognized deviceType %x!"` |
| `fffffff006288a88` | integration-register branch (rev 9/10/12) |
| `fffffff006288b50`–`…0x6288be0` | `H264DecoderCanDo444/422`, tile, `HEVCSupported` |
| `fffffff006288c50` | `"gart initialization failed"` |
| `fffffff006288e10` | `"start->LoadFirmwareAndStartCore returned %x"` |
| `fffffff0062891a8` | `initialize_gart` call |
| `fffffff006289a5c` | `PrepareCore` |
| `fffffff006289a5c`…`0x6289c6c` | firmware record → `AllocateFirmwareBuffer` |
| `fffffff006289cc8` | `LoadFirmwareAndStartCore` |
| `fffffff0062902b0` / `…2e0` | `ReadReg` / `WriteReg` |
| `fffffff006290310` | memory-descriptor allocation helper used for the firmware buffer |
| `fffffff006298368` | firmware-record resolver (returns the static record) |
| `fffffff006298430` | `GetCoreRev` (regs `0x640`, `0x630`) |
| `fffffff006298fc8` | firmware-ready poll + version read (`0x2014`, `0x2018`, `0x2024`) |
| `fffffff006299f50` | `AllocateFirmwareBuffer` |
| `fffffff006292328`–`…0x62928ac` | `DecodeFrameFig` body (per-context 0x410 stride) |
| `fffffff00628e6xx` | `ProcessInterrupt` / panic command dump |
| `fffffff007a6e658` | firmware record in `__DATA` |
| `fffffff007a6e7f8` | `gRegBase` (set by `start`) |
