# VXD (MSVDX) decode protocol — host ↔ DEVA firmware, one H.264 frame

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Round 4, lane `a9/vdec`. **No device access, no commits.**
Everything here comes from three artifacts already in the tree, re-mined this
round:

| Artifact | What it gave |
| --- | --- |
| `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12` → `build/video/kexts/AppleD5500.macho` | the host side: every register write, ring access, message build, completion parse |
| the same kext, **decompiled** (`build/video/d5500-decomp.txt`, 697 functions) | readable structure instead of instruction soup; new tool `tools/ds-video/ghidra-kext.py` |
| `build/video/fw/vxd_deva_fw.bin` (`REL_812.0_14_Deva`), **MINIM-expanded** (`build/video/vxdfw/deva.expanded.dis`) | the firmware side: the ring reader, the message-type dispatcher, the register window base; new tool `tools/ds-video/fw-disasm.py` |

Marks: **[V]** verified in an artifact (address given, re-checkable),
**[I]** inference that fits, **[U]** unresolved.

Read `docs/periph/ds-video-decoder.md` first (block identity, boot ladder,
register map, GART) and `docs/periph/vdec-test-plan.md` (round-3 driver state).
This document does not repeat them; it corrects them where this round proved
something different (§12).

---

## 0. Bottom line

1. **There is no "one message per parameter" protocol.** Decoding one frame is
   **one command buffer in DRAM** plus **one 6-word mailbox message** that says
   "execute the buffer at device address *X* for context *C*". The message
   carries only fence/context/address/offsets and start-end flags **[V]**;
   SPS/PPS, scaling lists, slice headers, reference lists, the bitstream bytes
   and the output surface addresses therefore travel **inside the command
   buffer**, in MSVDX command format, not in the mailbox **[V]** for the picture
   and bitstream parts, **[I]** for reference lists and output addresses (the
   command stream itself is decoded in §4.3 only to the block level).
2. The mailbox is a **word window**, not a DMA descriptor queue: message word *i*
   lives in the VXD register file at `0x2000 + base + 4*i`, where `base` and the
   ring capacity are **published after boot** in `0x2018` (firmware→host ring)
   and `0x2024` (host→firmware ring), as `(base_bytes << 16) | capacity_words`.
   Read side **[V]** (host never writes these two); that the firmware writes
   them **[I]** (it is the only other agent, and it reads `0x2024` itself).
3. Message header is one word: `(fence << 16) | (type << 8) | (nwords & 0x7f)`.
   Host→firmware types are **0x80–0x8b** (the firmware's dispatcher range, 12
   entries). AppleD5500 uses four: **0x80** boot/init, **0x81** H.264 picture,
   **0x87** auxiliary block kind 3, **0x88** auxiliary block kind 4. **[V]**
4. Completion is a firmware→host message **0xc0** carrying the fence in
   `w0[31:16]` and status/error bits in `w2`, delivered with IRQ 157. The ISR
   **acknowledges by writing `0x414 = 0x100000` *before* draining the ring**
   (that is Apple's order), and reads the status back from `0x410`. **[V]**
5. The command buffer is a chain of packets:
   `{0x80000007, payload_size, 0xabcdefab}` followed by `payload_size` bytes.
   The H.264 picture payload is **exactly 0x78 (120) bytes** — the size the kext
   allocates for its `H264CommandBufferParams` (name from the
   `__kalloc_type` sites; identification by size + role is **[I]**) — followed by
   per-slice blocks that start with `0x90000000` (0x28 bytes) and the bitstream
   bytes themselves. **[V]**
6. Command buffers must be **64 KiB aligned**: the message transmits
   `cmd_buf_va >> 8` in 32 bits with the low byte donated to the hwContextID, so
   the firmware can only rebuild a 64 KiB-aligned base. Apple allocates
   `8 × 0x10000` per decoder. **[V]** (arithmetic + Apple's allocation sizes)
7. **The one thing still missing for pixels is the command-stream generator**
   (`h264CmdBuf`/`RendecBlock`: picture command, entropy/VLC setup, render
   target, deblock, slice blocks). The mailbox layer is now fully specified and
   implementable; the command stream is a bounded next rung, and the firmware
   image is now readable — it is MINIM-encoded META, and once expanded **96 % of
   its 33 499 instructions decode**, which puts its command parser in reach
   (§11). **[V]/[U]**
8. Consequence for the driver: `apple_vxd_submit()` becomes ~150 lines
   (ring write + doorbell) plus a ring-drain ISR; `device_run()` needs a
   command-buffer builder, which is where the work is. §10 has the plan.

---

## 1. Two planes, one picture

```
        user space                       kernel                        VXD block
  ┌────────────────────┐   V4L2 ctrls  ┌──────────────────────┐
  │ ffmpeg/mpv         │──────────────▶│ apple-vxd            │
  │ (stateless H.264)  │               │  SPS/PPS/SCALE/DEC/  │
  └────────────────────┘               │  SLICE + DPB masks   │
                                       │                      │
                                       │ ① build command buf  │  DRAM (GART)
                                       │   {0x80000007,...}   │◀── MSVDX command list
                                       │   + H264 params 0x78 │    + bitstream bytes
                                       │   + slice blocks     │    + output/reference
                                       │   + bitstream        │      surface VAs
                                       │                      │
                                       │ ② mailbox message    │   register file
                                       │  6 words @0x2000+…   │──▶ TO_MTX ring
                                       │ ③ doorbell 0x88=1    │──▶ MTX interrupt
                                       │                      │
                                       │ ⑤ drain TO_HOST ring │◀── 0xc0 completion
                                       │ ④ ISR: ack 0x414     │◀── IRQ 157
                                       └──────────────────────┘
                                              THE MTX (DEVA firmware)
                                              reads ②, walks ①, programs
                                              VEC/ENTDEC/CMD/PIXEL pipes,
                                              posts 0xc0.
```

The firmware therefore **is in the data path as a scheduler + hardware driver**,
not as a bitstream decoder: it never sees SPS/PPS fields as fields — it sees the
command list **[I]** (the message tables in §3 are the [V] part). This finally
answers §6.1 of the study: the host owns parsing and command generation; the MTX
owns sequencing, hardware programming, fences and error reporting. The remaining
host work is large but *host* work.

Address spaces to keep apart:

| Space | Unit | Who uses it |
| --- | --- | --- |
| CPU physical / `dma_addr_t` | byte | Linux |
| **GART device VA** (vxd core MMU) | byte, 4 KiB pages, 40-bit | command buffer, firmware image, every surface |
| VXD register file | 4-byte register index | host (`regs + off`) and firmware (`0x480 + off`) |
| MTX memory | word/byte | firmware code+data, mailbox rings |

---

## 2. Register map, round-4 additions and corrections

All host accesses go through `CPVDECCore::ReadReg`/`WriteReg`
(`0xfffffff0062902b0` / `0xfffffff0062902e0`, base pointer at
`0xfffffff007a6e7f8`), so every offset below is a **verified** kext fact. The
firmware's own register window base is **0x480**: it computes
`0x480 + 0x2024` at `0x1000948`, `0x480 + 0x2028` at `0x1000940`,
`0x480 + 0x2030` at `0x100099c`, `0x480 + 0x2000` at `0x1000a74`. **[V]**

| Off | Name (best fit) | Access | Round-4 evidence |
| --- | --- | --- | --- |
| `0x0000` | MTX→host "message consumed" word | fw writes | fw `0x1000a90` writes `nwords` here after consuming a message **[V]** |
| `0x0088` | **doorbell host→MTX** | host writes 1 | written once, at the end of every message push (`0xfffffff0062990bc`) **[V]** |
| `0x0200` | VEC block start / kick | host writes 1 | `vxd_known_state_*`, `0xfffffff00629972c` (reset path) **[V]** |
| `0x0400` | CMD block control/status | r/w | bit 0 busy; `0xffff0000` reset pulse **[V]** |
| `0x0410` | **CMD/IRQ status** (read in ISR) | r | bit 20 = completion/IRQ cause; also the per-core status word `ProcessInterrupt` receives **[V]** |
| `0x0414` | **IRQ clear** (write-1-to-clear) | w | `0xfffffff00629a0ac` writes **`0x100000`** before draining **[V]** |
| `0x0418` | **IRQ enable** | w | boot writes `0x100000` (`0xfffffff0062992cc`) **[V]** |
| `0x0440` | clock enable (SetClocks) | r/w | unchanged from round 1 **[V]** |
| `0x0460` | pipe select | w | unchanged **[V]** |
| `0x0500` | MTX memory config | r | unchanged **[V]** |
| `0x0504` | DMAC flush (`0x80000000`) | w | unchanged **[V]** |
| `0x0608` / `0x060c` | internal register-file **data / index** port (debug RTM dumps) | r/w | new this round (`0xfffffff00628c588`) **[V]** |
| `0x1000` | MTX control | r/w | unchanged **[V]** |
| `0x1008` | MTX clock select; **re-written with `0xf00` before the next submit after the GART changed** (`AppleD5500+0x27a0` is set by `allocateKernelMemory`, consumed and cleared by the submit) | w | `0xfffffff00628af2c…0x628af50` **[V]** |
| `0x2000` | comms state; **also the APB flush read** the host does after ring writes | r (host), w=3 (fw) | host reads at 6 sites; fw writes 3 (`0x1000a80`) **[V]** |
| `0x2008` | written 0 at setup, never used again | w | `0xfffffff006298b60` **[V]/[U]** |
| `0x200c` | "firmware PC" (fw writes its PC), cleared by host at setup; read on `REATTEMPT_SINGLEPIPE` | r/w | fw `0x1000a64`, host `0xfffffff00628b0dc:10360` **[V]** |
| `0x2010` | fw message/heartbeat counter, host seeds it (`core[0x50]`, 200) | r/w | `0xfffffff00629973c` waits for it to settle **[V]/[I]** |
| `0x2014` | **firmware-ready magic**: host writes 0, fw writes `0xa5a5a5a5` | r/w | fw `MOVT 0xa5a5` at `0x10033b0`/`0x10033c0`; host poll `0xfffffff006298fc8` **[V]** |
| `0x2018` | **TO_HOST ring geometry** = `(base_bytes<<16) \| capacity_words` | r (after ready) | `0xfffffff006298fc8` → `core+0x48/0x4c`, used by pop **[V]** |
| `0x201c` | **TO_HOST rd index** (host-owned) | r/w | pop reads+advances it **[V]** |
| `0x2020` | **TO_HOST wr index** (fw-owned) | r | pop reads it **[V]** |
| `0x2024` | **TO_MTX ring geometry** = `(base_bytes<<16) \| capacity_words` | r (after ready) | `0xfffffff006298fc8` → `core+0x40/0x44`, used by push; fw reads it at `0x1000948` **[V]** |
| `0x2028` | **TO_MTX rd index** (fw-owned) | r | free-slot calc; fw writes it at `0x1000a9c` **[V]** |
| `0x202c` | comms flags, host sets `0x1000000 \| 0x60` (`\|0x3000` on the big-endian branch) | w | `0xfffffff006298d40`, `0xfffffff006298b60` **[V]** |
| `0x2030` | **TO_MTX wr index** (host-owned) | r/w | push advances it **[V]** |

Ring data lives in the *same* window: word `i` of a ring at MMIO
**`0x2000 + base + 4*i`**. Host push (`0xfffffff0062990bc`):

```c
reg = 0x2000 + core->ring_base + 4 * idx;      /* idx in words, wraps at cap */
WriteReg(reg, msg_word[j]);
idx = (idx + 1 < cap) ? idx + 1 : 0;
WriteReg(0x2030, idx);                          /* publish write index */
ReadReg(0x2028); ReadReg(0x2030);               /* APB flush */
ReadReg(0x201c); ReadReg(0x2020);               /* fw-side mirror, flush */
WriteReg(0x88, 1);                              /* doorbell */
ReadReg(0x2000);                                /* final flush */
```
The firmware does the mirror-image read (`0x10008f0`–`0x1000aa4`) and publishes
the consumed index into `0x2028`, plus `0x0 = nwords` and `0x200c = PC`.

**Rule for the Linux driver: never cache `base`/`cap`; read 0x2018/0x2024 once
after the ready magic, and always flush with a read of 0x2000 after a burst of
ring writes and before the doorbell.**

---

## 3. Message format

Word 0 is the header, always little-endian, in a register window:

```
 31        16 15     8 7        0
┌─────────────┬────────┬──────────┐
│    fence    │  type  │ nwords   │   nwords = total words including w0 (≤ 0x14)
└─────────────┴────────┴──────────┘
```

* `nwords` is `w0 & 0x7f` on the receive side (`0xfffffff0062997b0`), and the
  firmware rejects anything over **0x14 words** (`CMP #0x14`, `0x1000b80`) **[V]**.
* `type` is the byte at offset 1 both sides (`*(u8*)(msg+1)`, fw `GETB [..+1]`)
  **[V]**.
* `fence` is the 16-bit completion tag, `w0[31:16]`; the firmware echoes it in
  the completion message **[V]**.
* Bit 7 of `nwords` exists transiently in Apple's builder (OR of the previous
  byte, then masked off) and is always 0 on the wire **[V]**.

Fence construction, host side (`0xfffffff00628a8d4`, per packet `k` of a
submission):

```c
fence = (submission_counter << 4) | (core & 1) | ((packet_index * 2) & 0xe);
```
Apple keeps `submission_counter` at `AppleD5500+0x278c` and increments it per
submission; the low 4 bits therefore identify core + packet slot, the rest is a
rolling counter. A single-job Linux driver can use a plain monotonic counter
**[I]** — the firmware treats it as an opaque tag.

### 3.1 Host → firmware, type 0x80: boot / init message **[V]**

Sent once, by `InitMessageBuffers` (`0xfffffff006298d40`, build at
`0x6298f1c`, push at `0x6298f84`), **after** the ready magic and **after**
reading the ring geometry.

| w | Value (as built) | Meaning |
| --- | --- | --- |
| 0 | `0x00008005` | type 0x80, 5 words |
| 1 | `*(u32*)(fw_instance + 0x10)` | **firmware image device address** (the `0x6a00` value) **[V]** |
| 2 | `(seq << 16) \| (fw_size >> 12)` | `seq` = `core[0x70]`, a 16-bit counter that wraps at 0x10000; low half = firmware size in 4 KiB pages (0x17 for the 0x17e98 blob) **[V]** |
| 3 | `0xffff0000` | sentinel (checksum/CRC placeholder; the driver never varies it) **[I]** |
| 4 | `0` | — |

### 3.2 Host → firmware, type 0x81: decode picture (H.264) **[V]**

Built by `AppleD5500::SubmitCommandBufferInternal` (`0xfffffff00628a8d4`, the
`kind == 1 / else` branch at `0x628ae88`), one message **per command-buffer
packet**, and sent through the gated wrapper `0xfffffff00628f2a4`.

| w | Expression in the kext | Meaning |
| --- | --- | --- |
| 0 | `(fence<<16) \| (0x81<<8) \| nwords` | header, `nwords` = 6, or 7 when `core[0x25]` (second pipe / large-MB flag) is set |
| 1 | `(params[0x18] >> 2) << 16 \| (params[0x10] \| end_flags)` | see below; `end_flags` = **0x4004** for the last packet of the submission, **0x2000** otherwise |
| 2 | `params[0x1c]` | 0 for the H.264 builder (unused slot) |
| 3 | `((cmd_buf_va >> 8) & 0xffffff00) \| hwContextID` | **command-buffer device address**, 64 KiB granular; low byte = context 0…11 (validated against mask `0x73e`) |
| 4 | `params[0x14]` | 0 for H.264 |
| 5 | `params[0x30]` | 0 for H.264 |
| 6 | `params[0x34]` | only when `nwords == 7` |

`params[0x10]` is the running **byte offset of this packet's data inside the
command buffer**, accumulated per slice (`0x62968a0` adds the slice size plus
64-byte alignment padding to the same counter). `params[0x18]` is never written
by the H.264 builder; `params[0x18] >> 2` is Apple's "size in words" slot. The
fact that the low 16 bits are OR-ed with `0x4004`/`0x2000` means the firmware
reads bits 0–1 and 13–14 as flags **[I]**, and that the offset must be treated
as a small window index **[U]**; the safe implementation is to reproduce Apple's
arithmetic exactly rather than to re-interpret it.

Before the first submission of a frame Apple also does
`WriteReg(0x1008, 0xf00)` (`0xf0000` on the big-endian core) —
`0xfffffff00628af2c` **[V]**.

### 3.3 Host → firmware, types 0x87 / 0x88: auxiliary packets **[V]**

Same builder, `kind` read from `params[0x4c]`:

| `params[0x4c]` | type | packet producer |
| --- | --- | --- |
| 1 | **0x81** | `CH264Decoder` picture packet (`0x6296814`) |
| 3 | **0x87** | `InsertDeblockInCommandBuffer` (`0x62c44b4`) — **H.264 deblock block**; its `w4` gets `& ~0x6000 \| 0x4000`, `w1` is not OR-ed |
| 4 | **0x88** | `0x62b7ab0` (`DecodeGetRenderTargetRef`) — render-target / second-pipe block |
| anything else | — | the builder still sends, but the firmware's dispatcher only knows 0x80…0x8b |

So a first IDR needs: 0x81 (+ 0x87 if the deblock block is emitted separately).
Apple emits the deblock block only for streams/limits where it is needed
**[I]**; a first-cut driver can start with 0x81 alone and add 0x87 when the
picture shows block-edge artefacts.

### 3.4 Firmware → host, type 0xc0: completion **[V]**

Drained by `FirmwareInteruptHandler` (`0xfffffff00629997c`, byte at `msg+1` =
0xc0) and `ProcessFWMessages` (`0xfffffff006299bb4`).

| w | Field in the kext | Meaning |
| --- | --- | --- |
| 0 | `type = 0xc0`, `[31:16]` = fence | completion for the picture submitted with that fence **[V]** |
| 1 | not read | — |
| 2 | `uStack_168`, bits | `0x18000000` mask = fault class; bit 27 (`>>28 & 1` path) = "after core reset"; bit 31 (`(int)uVar2 < 0`) = `REATTEMPT_SINGLEPIPE` needed **[V]** |
| — | `%x` logged | "VDEBCR", "Flags", "Last MB", "errorFlags" (log string at `0x556…`), i.e. the firmware reports the hardware error registers verbatim **[V]** |

Per-core routing: `if (fence & 1) → core 1 else core 0` **[V]**.

### 3.5 Firmware → host, 0xc6 / 0xc7 **[V]**

| type | Name | Fields |
| --- | --- | --- |
| 0xc6 | contiguity warning | two values logged as `Contiguity warning ( %i - %i )`; corresponds to Apple's `FW_DEVA_CONTIGUITY_DETECTION` flag |
| 0xc7 | **DEVA panic** | `ui32Fence`, `uiErrorInt`, a string index → `FirmwareInteruptHandler - FW_DEVA_PANIC` / `ProcessFWMessages: FW_DEVA_PANIC` |

### 3.6 The firmware's dispatcher **[V]**

`0x10008f0`–`0x1000b40` in the image (with `base = 0x01000000`, §11):

```
read 0x2024 → base = hi16, cap = lo16
if (rd == wr) { 0x200c = PC; 0x2000 = 3; 0x0 = nwords; 0x2028 = rd; idle }
else { copy msg words from the ring, type = byte(msg+1);
       if (type - 0x80 > 0xb) drop;
       jump table at 0x1000ad4, 12 entries:
         0x80 → 0x1000b18   (call 0x1001464(dev))
         0x81 → 0x1000b08   (call 0x10005e8(type, msg))
         0x82 → 0x1000b74   (call 0x1001248(dev))
         0x83,0x84,0x85 → ignore
         0x86,0x87,0x88 → 0x1000b08
         0x89,0x8a → 0x1000b28  (4-bit sub-opcode compared with 0xc)
         0x8b → 0x1000b08 }
```
`0x1000b08` is the **generic "hand this command buffer to the decoder"** path;
it tail-calls a handler registered in a global object
(`[A1GbP+24] → [+36] → function pointer`, `0x1000b40`), which is how the codec
module (H.264) receives its work.

---

## 4. The command buffer

### 4.1 Packet container **[V]**

`CmdBuf::addCommand` (`0xfffffff0062982f8`):

```c
struct cmd_packet { u32 magic0; u32 size; u32 magic1; u32 payload[]; };
magic0 = 0x80000007, magic1 = 0xabcdefab, step = size + 0xc
```
The submit path validates both magics (`0x628ac78`) and walks `size+0xc` at a
time. Apple allocates **8 command buffers × 0x10000 bytes** per H.264 decoder
(`0xfffffff0062c3d98`) — that is where the 64 KiB alignment requirement in §0.6
comes from.

### 4.2 H.264 picture payload, 0x78 bytes (the kext's `H264CommandBufferParams` **[I]**)

Filled at `0x6296814`–`0x62968b4` (inside `CH264Decoder::HOST_H264DecodeFrame`,
`0xfffffff0062960a0`), one per slice; zeroed first.

| Off | Value written by the kext | Role |
| --- | --- | --- |
| `0x00` | 0 | — |
| `0x04` | `bit0` = first slice of the picture; `bit4` = first picture after (re)start, set once and latched in decoder state | slice/picture start flags **[V]** |
| `0x08` | `*(u32*)(x25 + 0x10c)` | per-slice field (slice index / first-MB group) passing through from the caller's slice struct **[I]** |
| `0x0c` | `w24` | this slice's bitstream size in bytes **[V]** |
| `0x10` | `*(u32*)(x25 + 8) + cmdbuf->total` | running byte offset of the slice data in the command buffer **[V]** |
| `0x14`, `0x1c`, `0x30`, `0x34` | 0 | slots used by kinds 3/4 (deblock, render target) **[V]** |
| `0x18` | 0 | "size in words" slot used by kinds 3/4 **[V]** |
| `0x24` | `*(u32*)(decoder + 0xc28)` | DPB/sequence parameter block, i.e. the "SPS/PPS instance" currently selected **[I]** |
| `0x4c` | 1 | **packet kind** (selects the mailbox type, §3.3) **[V]** |
| `0x70` | `*(u32*)(*(u64*)(decoder + 0x6010) + 4)` | sequence/bitstream tag passed through to the firmware **[I]** |
| `0x74` | not written here | — |

Note what is **not** here: no SPS/PPS structure, no reference list, no output
address. Those live in the **command stream that follows the payload** and in
the per-slice blocks (§4.3).

### 4.3 Slice block and the command stream **[V] structure, [U] full encoding**

Per slice, before the picture payload, the decoder appends a 0x28-byte block
(`0x6296774` + `0x62b37d4`):

```
+0x00  u32 0x90000000         ; block header (kind 9)
+0x04 … +0x27                 ; filled by h264CmdBuf helpers
```
and `h264CmdBuf` (`0xfffffff0062b37d4`) then emits, in order:

| Word | Emitted at | Content |
| --- | --- | --- |
| `0x90000000` | `0x6296774` | slice block header |
| — | `0x62b2620`, `0x62b38d0`, `0x62b55f0`, `0x62b5c8c`, `0x62b5d44`, `0x62b6010` | picture / entropy / render-target / deblock / prediction command groups; prefixes observed: `0x70000000`, `0xb0000001`, `0xf0000000` |
| `0x60000000` | `0x62b37d4` tail | end-of-slice |

Bitstream bytes are appended to the same buffer, and the write pointer is padded
to 64 bytes between blocks (`0x40 - (offset & 0x3f)` zero bytes, `0x62967e0`),
which is why the picture payload carries a byte **offset** and a **size** rather
than an address: the firmware derives the absolute address from the command-
buffer base in message word 3.

This is the boundary of what this round proved. The six command generators are
~90 KB of decompiled code; their field layout is the next rung (§11) and it is
readable — the firmware parses exactly these words, and the firmware is now
disassemblable.

---

## 5. Where each V4L2 stateless control ends up

The mapping is **not** "control → message field"; it is "control → host
structure → command stream". Use this as the contract for the builder.

| V4L2 stateless control | Apple host structure (kext lineage) | Consumed by | Status |
| --- | --- | --- | --- |
| `V4L2_CID_STATELESS_H264_SPS` | `PicParams_H264` / SPS instance, `AVC_RBSP::parseSPS` | entropy/VLC setup (`VlcPackedTableData`, `PreloadBuffer`), picture dimensions, and `params[0x24]` (selected SPS/PPS instance) | [V] that it is host-parsed and pushed into the command buffer; exact command fields [U] |
| `V4L2_CID_STATELESS_H264_PPS` | PPS instance + `h264fw_SetPpsAndSps` (`0x62b083c`) validation that the PPS references a known SPS | same | [V] |
| `V4L2_CID_STATELESS_H264_SCALING_MATRIX` | `Qmatrix_H264` | **inside** the SPS/PPS blob pushed to the firmware (not a separate control path) | [V] (object name in `__kalloc_type`; the parser stores it with the PPS) |
| `V4L2_CID_STATELESS_H264_DECODE_PARAMS` | per-picture block (`H264_STORED_FRAME_PARAMS`, 0x80 of them) | frame_num / POC / DPB indices / ref lists → picture command; `first_picture` → `params[0x04] bit4` | [V] for the flag, [U] for the per-field command slots |
| `V4L2_CID_STATELESS_H264_SLICE_PARAMS` | `Slice_H264_Long` / `VA_SliceInfo` | `params[0x08]`, `params[0x0c]`, `params[0x10]`, the 0x28 slice block, the slice-header bytes in the stream | [V] for offsets/sizes, [U] for field mapping |
| `V4L2_CID_STATELESS_H264_PRED_WEIGHTS` | part of the picture block | picture command | [U] |
| DPB (`v4l2_h264_dpb_entry`) | `CPicture` + `msvdx_surface`; **22 surfaces total** (index guard `< 0x16` at `0xfffffff0062969ac`, logged as `DPB_MAX_BUF + MAX_BACK_BUFFERS = 22`) | reference list inside the picture command, by **GART device VA + stride** | [V] for the bound and the address source, [U] for the command slot |
| output surface | `msvdx_surface`: `+0x18` = GART VA, `+0x20` = stride; `kern_mem_info[2]` = GART VA | render-target command | [V] for the struct fields, [U] for the command slot |

Surface/stride source (`AppleD5500::allocateKernelMemory`, `0xfffffff00628b238` —
the same function that validates the hwContextID and allocates the GART range): `IOMemoryDescriptor::getLength`, `getPhysicalSegment`, IOSurface
`width`/`height`/`bytesPerRow` per plane (`0xfffffff005bd5ab0`,
`0xfffffff005bd5934`), and `addMemoryDescToGart` (`0xfffffff00628f320`) which
allocates the device VA from a **range allocator over the GART aperture** — not
identity-mapped, unlike the round-1 assumption **[V]**.

Practical consequence: a stateless Linux driver can use
`vb2-dma-contig`-allocated surfaces, GART-map each one once, and keep the
returned device VA in its own DPB entry — exactly Apple's model.

---

## 6. Interrupts, completion, timeouts

ISR sequence, all **[V]** (`0xfffffff00629a0ac` + `0xfffffff00628e5a0`):

```
1. status = ReadReg(0x410)                     /* per-core status word */
2. WriteReg(0x414, 0x100000)                   /* ACK, before draining */
3. FirmwareInteruptHandler(core)               /* 0xc0 / 0xc7 / 0xc6 sweep  */
4. ProcessFWMessages(core, out0, out1)         /* per-core fence+status lists */
5. match fence → vb2_buffer_done(dst, DONE) ; release the surface
6. counter--; if 0 → power down / autosuspend
```
Details worth copying:

* If `ReadReg(0x410) & 0x100000` is clear on entry, Apple skips the ack+drain
  (`0xfffffff00628b0dc`) and only runs the per-core pending-submission path; with
  the bit set it acks, drains, and *then* runs it. A Linux ISR should just always
  ack and drain.
* `0x410` bit 20 is also polled by the *host* before re-submitting after a
  timeout (`0xfffffff006299300`), i.e. it doubles as the "hardware idle" bit.
* Timeout handling flushes the rings, zeroes the fence slots and calls
  `ProcessInterrupt` manually (`0xfffffff00628a6bc`, log line
  `indices TO_MTX_RD_INDEX %x TO_MTX_WRT_INDEX %x TO_HOST_RD_INDEX %x
  TO_HOST_WRT_INDEX %x` — the cleanest confirmation of the register names).
* Recovery after `0x2014` is cleared means a full re-boot of the MTX
  (`0xfffffff00628c474`: `0x2014 = 0`, `0x200 = 1`, known-state v1, reload).

---

## 7. Buffers the firmware expects

| Buffer | Size (Apple) | Alignment | Where |
| --- | --- | --- | --- |
| firmware image | 0x17e98 (97 944 B) | 4 B | GART VA published in msg 0x80 and `0x6a00` |
| **command buffer** | 0x10000 × 8 | **64 KiB** | GART VA in msg w3 |
| FE state buffer | `width << 16` | 4 KiB | GART, referenced by commands |
| VLC packed table area | `width << 19` (H.264) | 4 KiB | GART |
| stream-state buffer | `width << 12` | 4 KiB | GART |
| preload buffer | `height * 0x288` | 4 KiB | GART |
| VLC packed tables (0x440 B blob memcpy'd from `0x5596d08`) | 0x1000 | 4 KiB | GART |
| render buffers | 0x40000 × 8 | 4 KiB | GART |
| `H264_STORED_FRAME_PARAMS` | 0x80 objects | — | kernel heap |
| output / DPB surfaces | as allocated | 4 KiB | GART, VA + stride |
| MTX memory | ≥ 0xa1a0 (`mtx_need`) | — | core-internal, `0x500` reports 66 MB |

All sizes above are from `DecodeCreateH264VLD` (`0xfffffff0062c3d98`), **[V]**.
The *names* are assignments for the size-scaled H.264 buffers (**[I]**): they
line up with the MPEG4 counterpart (`0xfffffff0062c019c`, which logs "FE State
Buffer", "VLC table area", "render buffer", "command buffer" explicitly), but
the H.264 function only logs the render/command/stream-state ones. The "width"
used in those formulas is `*(int*)(decoder + 0x28)`; take it as "scales with
picture width" until the command-stream round pins it **[I]**.

The driver's current GART is adequate: identity map, 4 KiB pages, PTE
`((phys>>12)<<4)|1`, dummy page for holes. The **one change needed** is a
64 KiB-aligned command buffer and, if we stop identity-mapping, a range
allocator like Apple's (§5).

---

## 8. Minimal sequence to decode one IDR (j71t, H.264, NV12)

Preconditions: block powered (`ps_vdec`), boot ladder of
`docs/periph/vdec-test-plan.md` §6 succeeded, `0x2014 == 0xa5a5a5a5`.

```
 1  ring_base = ReadReg(0x2024) >> 16 ; cap = ReadReg(0x2024) & 0xffff
 2  send msg 0x80: {0x8005, fw_va, (seq<<16)|fw_pages, 0xffff0000, 0}
      → write words into 0x2000+base+4*i, 0x2030 = idx, flush, 0x88 = 1
 3  build command buffer (64 KiB aligned, GART-mapped):
      packet hdr   {0x80000007, size=0x78+slice, 0xabcdefab}
      params[0x4c] = 1 ; params[0x04] = 0x11 (first slice + first picture)
      params[0x0c] = slice_bytes ; params[0x10] = 0
      params[0x24] = SPS/PPS instance tag
      slice block  {0x90000000, …} + MSVDX command groups + slice+IDR bytes
 4  WriteReg(0x1008, 0xf00)                      /* only if the GART changed since the
                                                   last submit (Apple's +0x27a0 rule) */
 5  send msg 0x81: {0x8106, params[0x10] | 0x4004, 0, (cmd_va>>8) | 0, 0, 0}
 6  wait IRQ 157 → ReadReg(0x410) ; WriteReg(0x414, 0x100000)
 7  drain TO_HOST ring (0x2018 geometry, 0x201c rd, 0x2020 wr):
      type 0xc0 → fence match → check w2 error bits → buffer done
      type 0xc7 → panic path (log fence + error int, reset the decoder)
 8  if no completion in N ms: log 0x2018/0x201c/0x2020/0x2024/0x2028/0x2030,
    write 0x2014 = 0 and re-run the boot ladder (Apple's recovery)
```

Steps 1–2 and 5–7 are fully specified by this document. Step 3 is the command
generator (next rung).

---

## 9. What the round-3 driver must change (small, independent of step 3)

| Current code | Change |
| --- | --- |
| `fw_build_1/2` from `0x2018`/`0x2024` | those are **ring geometry**, not build halves. Read them into `ring_*` fields after the ready poll and log `base/cap` instead |
| `apple_vxd_mbox` capacity hard-coded 200 | read from `0x2024 & 0xffff`; also fix the free-slot formula: Apple returns `rd - wr - 1` (`~wr + rd`) in both cases, i.e. the wrap branch is `rd - wr + cap - 1`, one less than the driver's `rd - wr + cap` (`0xfffffff0062996b4`) |
| push path returns `-EAGAIN` | implement §2 push (`0x2000+base+4*i`, `0x2030`, flush, `0x88=1`) |
| no init message | add §3.1 after the ready poll; without it the firmware has no command-buffer base and never schedules |
| `VXD_REG_CMD_DESC_ADDR/SIZE/STATUS` names | rename `0x410` = irq/status, `0x414` = irq clear, `0x418` = irq enable |
| IRQ handler | read `0x410`, ack `0x414 = 0x100000`, then drain; assert `0x418 = 0x100000` at boot (already done) |
| GART map | keep identity map for step 3, but **check capacity ≥ DRAM**; the command buffer must be **64 KiB aligned** (over-allocate + align, §10 R3b) |
| `va_offset` module param | keep; it is the knob for a wrong aperture, and the firmware-side base (`0x480`) says the MTX sees registers at a fixed offset, not via the GART |

---

## 10. Implementation plan for `device_run()`

**R3a — make the mailbox real (½ day, no pixels yet).**
`apple_vxd_ring_open()` after the ready poll: read `0x2018`/`0x2024`, store
`base/cap` per direction; `apple_vxd_msg_push()` per §2; `apple_vxd_msg_pop()`
per §3.4; `apple_vxd_init_message()` per §3.1. Gate: send 0x80 and observe with
`debugfs mbox` that `TO_MTX_RD` advances to the write index (the firmware
consumed it) and that `0x200c` becomes non-zero (the firmware publishes its PC).
That single observation proves the whole mailbox layer without a decoder.

**R3b — command-buffer infrastructure (1–2 days).**
`struct apple_vxd_cmdbuf { void *vaddr; dma_addr_t dma; u32 va; size_t size; size_t used; }`
allocated as one coherent block of `2 * 0x10000` and manually aligned up to
64 KiB (keep the original pointer for the free), GART-mapped once. Implement
`cmdbuf_add_packet(magic, size)` (§4.1) and `cmdbuf_append(bytes)`.

**R3c — one IDR from a hand-built command stream (the real rung).**
Read the six generators in the decompilation dump —
`0x62b37d4` (slice), `0x62b2620`, `0x62b38d0`, `0x62b55f0`, `0x62b5c8c`,
`0x62b5d44`, `0x62b6010` — and cross-check every emitted field against the
**firmware parser** in `deva.expanded.dis` (the 0x81 handler chain
`0x10005e8` → codec handler). Deliver a `h264_cmdbuf.c` that emits, for a
single-slice IDR: picture header, entropy/VLC setup, render target, slice block,
slice data. Then submit with §8 and check the completion fence.

**R3d — wire V4L2.** `device_run()`:

```c
static int apple_vxd_h264_run(struct apple_vxd_ctx *ctx)          /* replaces the stub */
{
        struct apple_vxd_run run;
        int ret;

        apple_vxd_run_preamble(ctx, &run);              /* src/dst from vb2 */
        ret = apple_vxd_prepare_job(ctx);               /* surfaces → GART VA/stride */
        if (!ret)
                ret = h264_build_cmdbuf(ctx);           /* §4 */
        if (!ret)
                ret = apple_vxd_msg_push(ctx, 0x81, ...);/* §3.2 */
        if (ret)
                apple_vxd_run_postamble(ctx, &run);      /* error out both buffers */
        return ret;                                      /* completion in the ISR */
}
```
with `fence` stored in the job, `ISR → fence match → vb2_buffer_done`. Keep
`m2m` one-job-at-a-time (`hw_ready` gate) until the fence logic is trusted.

**R3e — DPB.** 22 surfaces max (hardware bound), `V4L2_H264_DPB_ENTRY_FLAG_*`
→ reference list; one GART mapping per surface, kept for the buffer's lifetime.

Order matters: R3a proves the mailbox on the device with zero risk of a bad
command stream wedging the block; R3b/R3c are host-only work that the existing
QEMU smoke harness can exercise; only R3d needs the device.

---

## 11. Open questions, ranked

1. **Command-stream encoding** (blocks R3c): the six generators + the firmware
   parser. Now tractable: `tools/ds-video/fw-disasm.py` expands and disassembles
   the DEVA image (32 332 instructions, 1 167 undecoded), and the codec handler
   is reachable from `0x1000b08`.
2. **`w1` field semantics** (§3.2): whether the low 16 bits are a byte offset
   window or a word count. Answer comes from the 0x81 handler in the firmware.
3. **Types 0x82 / 0x89 / 0x8a** and the 4-bit sub-opcode: probably
   stream create/destroy and query; not needed for the first IDR.
4. `0x2008` and `0x2010`: written, never read by the host in a way that matters.
5. Does the firmware need the deblock block (0x87) for correctness on this core,
   or is it an Apple quality knob? One experiment with/without.
6. The 40-bit GART mode (`0x1070 = 0x10`): the driver currently programs it from
   `0x1080`; the firmware-side base of `0x480` suggests the MTX does **not** use
   the GART for registers, only for data.

---

## 12. Corrections to earlier documents

| Earlier statement | Round-4 finding |
| --- | --- |
| "`0x2018` / `0x2024` are firmware build halves" (`ds-video-decoder.md` §4.3, driver `fw_build_1/2`) | they are **ring geometry**: `(base_bytes << 16) \| capacity_words`, written by the firmware, read after the ready magic |
| "message content transport is LLDMA over the comms window, `[U]`" (`§Appendix C`) | it is a plain **word window**: `0x2000 + base + 4*i`, no DMA descriptors; opcode "push" is 20 instructions |
| "`0x2010` = message count, `0x202c` = flags" | `0x2010` is a firmware message counter the host seeds; `0x202c` = flags, unchanged |
| "`0x410/0x414/0x418` = CMD descriptor addr/size/status" | they are **IRQ status / clear / enable** (bit 20 = `0x100000`); acks are `0x414 = 0x100000` |
| "`H264FW_NalPushSliceData` puts bitstream bytes into a firmware message" | bitstream goes into the **command buffer**; the message carries offset+size. (`h264fw_*` are host-side helpers, not message senders) |
| "GART is identity-mapped by Apple" | Apple uses a **range allocator** over the aperture for surfaces; identity mapping still works for us but is not what iOS does |
| "the DEVA image is opaque MTX code" | it is **MINIM-encoded**; expanding it yields normal META and 96 % decode |
| (this doc, §0.6/§3.2) "w3 = `(cmd_buf_va >> 8) & 0xffffff00 \| hwContextID`; command buffers must be 64 KiB aligned" | **w3 is the MMU page-table context, not a buffer address.** Raw code at `0xfffffff00628ae5c`: `ldr x0,[x19,#0x27a8]; bl getPhysicalAddress; lsr x8,x0,#8; str w8,[sp,#0xcc]; strb w21,[sp,#0xcc]`. `+0x27a8` is the GART **directory** descriptor (`initialize_gart`: `withOptions(0x403, 0x1000, alignment 0x10000)`, its mapping at `+0xb20` receives the page-table PTEs), and `ProcessorInitialise` programs `0x1020` (MTX_MMU_DIR_ADR) from the same object through `0xfffffff0062903d0` (`ldr x0,[x0,#0x27a8]; b getPhysicalAddress`). So w3 = `(MMU_DIR_ADR value & ~0xff) \| hwContextID`, and the 64 KiB alignment belongs to the **directory**. Found in round 4 of the driver (`a9/vdec` R3b, `apple_vxd_mmu_ctx_word()`) **[V]** |
| (this doc, §0.5/§4.1, and `ds-vxd-cmdstream.md` §3.1/§11) "the packet header + 0x78 payload follow the slice commands in the command buffer" | **The packet list is a separate, host-only buffer.** At `0xfffffff0062967cc` the pad goes onto the command object (`x19`: `+0x80` base, `+0x88` wptr), then `addCommand` (`0x62982f8`) is called on **another** object (`x23`: `+0x8` base, `+0x30` capacity, `+0x38` offset). `SubmitCommandBufferInternal` walks that object from its base, checking both magics on every step; a command buffer starts with `0x90000000` and would fail the check **[V]**. Per-packet message words, header-relative offsets as in §3.2: w1 = `(payload+0x0c >> 2) << 16 \| payload+0x04 \| 0x4004/0x2000` where payload+0x0c = the slice's **command byte count** (so w1[31:16] is a word count, consistent with `ds-vxd-cmdstream.md` §9.1 but not with its §13 "Apple leaves it 0"); w2 = payload+0x10 = `chunk_start + cmd->[0x18]`, the one word that can carry the command chunk's location **[I]**; w4 = payload+0x08; w5 = payload+0x24 **[V]** |
| (§4.3) "padded to 64 bytes: `0x40 - (offset & 0x3f)` zero bytes" | correct, and it applies when already aligned too: an aligned chunk gets a full 64 zero bytes (`sub w28, #0x40, w8; and x8, x28, #0x7c`, `0x62967e4`) **[V]** |

---

## Appendix A — address index (unslid VAs / image PCs)

| Address | What |
| --- | --- |
| `fffffff0062902b0` / `…2e0` | `ReadReg` / `WriteReg` (base `fffff007a6e7f8`, byteswap if `DAT_…a6e588 == 2`) |
| `fffffff006298b60` | `ProgramCore`/ring setup: `0x200`, `0x208`, `0x1010/1020/1070`, `0x2014=0`, `0x2010`, `0x200c=0`, indices 0, `0x2008=0`, `0x202c=flags` |
| `fffffff006298d40` | `InitMessageBuffers`: `0x202c`, `0x1000`, pipe regs, wait-ready, geometry, msg 0x80 (build `0x6298f1c`, push `0x6298f84`) |
| `fffffff006298fc8` | poll `0x2014 == 0xa5a5a5a5`, read geometry from `0x2024`/`0x2018` |
| `fffffff0062990bc` | message push (+`0x88 = 1`) |
| `fffffff0062996b4` | free slots in TO_MTX |
| `fffffff0062997b0` | pop from TO_HOST |
| `fffffff00629973c` | wait for `0x2010` to settle |
| `fffffff00629997c` | `FirmwareInteruptHandler` (0xc0/0xc7) |
| `fffffff006299bb4` | `ProcessFWMessages` (0xc0/0xc6/0xc7, fence routing) |
| `fffffff00629a0ac` | ISR prologue: `0x410` read, `0x414 = 0x100000`, drain |
| `fffffff00628a8d4` | `SubmitCommandBufferInternal` — builds msg 0x81/0x87/0x88 (kind-1 branch `0x628ae88`, magic check `0x628ac78`, push `0x628affc`) |
| `fffffff00628b238` | `allocateKernelMemory` — hwContextID check, IOSurface width/height/stride, GART range + `msvdx_surface` (`+0x18` VA, `+0x20` stride), sets `+0x27a0` |
| `fffffff00628f2a4` | gated wrapper around `allocateKernelMemory` |
| `fffffff0062982f8` | packet header writer (`0x80000007` / `0xabcdefab`) |
| `fffffff0062960a0` | `HOST_H264DecodeFrame`: per-slice loop, payload build at `0x6296814` |
| `fffffff0062b37d4` | `h264CmdBuf` per-slice command emission (`0x60000000` terminator) |
| `fffffff0062c3d98` | `DecodeCreateH264VLD` (all buffer sizes) |
| `fffffff00628f320` | `addMemoryDescToGart` (range allocator → device VA) |
| `fffffff0062969ac` | DPB bound guard (`< 0x16`) |
| `010008f0`–`01000b40` (fw) | ring read, msg dispatch (table at `01000ad4`), fw register base `0x480` |
| `010033b0`/`010033c0` (fw) | `0xa5a5a5a5` ready magic |

## Appendix B — tools added by this round

```sh
# decompile the whole kext into build/video/d5500-decomp.txt (one-off, ~4 min)
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec \
  ~/.local/share/uv/tools/ghidra-mcp/bin/python -I tools/ds-video/ghidra-kext.py \
  --macho build/video/kexts/AppleD5500.macho \
  --project build/video/ghidra-d5500 --analyze --dump build/video/d5500-decomp.txt
# one function, fast:  --func fffffff00628a8d4

# expand MINIM and disassemble the DEVA firmware, then grep it
python3 tools/ds-video/fw-disasm.py                 # -> build/video/vxdfw/deva.expanded.{bin,dis}
python3 tools/ds-video/fw-disasm.py --skip-expand --grep '0x2028|0x2030|0xa5a5'
```
> Private graphics-dependent excerpt omitted from this source snapshot.

