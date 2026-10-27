# VXD (MSVDX) H.264 command-stream generator — decoded

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Round 5, lane `a9/vdec`. **No device access, no commits.**
Companion to `docs/periph/ds-vxd-decode-protocol.md` (round 4: mailbox,
message format, buffer sizes). That document ended at "the command-stream
generator is not decoded". **This document closes that gap**: it decodes
`h264CmdBuf` and its six helpers out of `build/video/d5500-decomp.txt`,
cross-checks them against the DEVA firmware and the emitter library, and
gives a driver-ready recipe for one H.264 IDR frame.

Marks: **[V]** verified in an artifact (address or offset given, re-checkable),
**[I]** inference that fits every observation, **[U]** unresolved. Section 14
lists the [U]s with the one experiment that settles each.

Read `ds-vxd-decode-protocol.md` first. This document *corrects* it in §13.

New tool: `tools/ds-video/cmdstream.py` (`--emit` / `--slots`), which turns the
7 KB of bitfield-assert soup that is `FUN_fffffff0062b38d0` into readable
(bit, source) tables.

---

## 0. Bottom line

1. **The stream is a register program, not a picture description.** Decoding a
   slice emits ~680 words (~2.7 KiB), of which all but ~250 are "write
   register *R* = *V*" pairs. The H.264 picture content reaches the silicon
   as values written into the MSVDX `CMD` register block (`0x30xx`/`0x34xx`) and the `ENT` register
   block (`0x50xx`/`0x54xx`/`0x58xx`/`0x5bxx`), not as a data structure the
   hardware walks. **[V]** (§5–§7)
2. **Four commands are not register writes**, and they are the interesting
   ones: `0x9` (per-slice descriptor block, 0x28 bytes), `0xb` (slice
   descriptor, 3 words), `0xa` (load *N* bytes from device address *A* — **this
   is how the slice bitstream gets in**), `0xf` (transfer the generated table
   blob to the entropy decoder), plus the `0x6` terminator. **[V]** (§2)
3. **The bitstream is not copied into the command buffer.** Apple emits
   `{0xa0000000 | slice_bytes, base + slice_offset}` where `base` is a device
   address read from an object at `ctx+0x13c890` (`+0x18`) and `slice_offset`
   is `bitstream_position - object->[0x10]`. A driver therefore needs no
   memcpy and no alignment work for the slice bytes — it needs the source
   buffer's GART VA. **[V]** for the arithmetic (`0xfffffff0062b5c8c`, §3.3);
   **[I]** for "that object is the input surface" (the field is only ever read,
   and its `+0x10`/`+0x18` shape matches `msvdx_surface`).
4. **Per-slice word order** (exactly, one slice):
   `0x90000000`+9 zero words → `0xb0000001`+2 → `0xa…`+1 → CMD register group
   (~69 words) → `0x3090/0x3094` placeholders → `0xf…` table transfer + 2 →
   ENT register group (~590 words, mostly the CAVLC table addresses) →
   `0x60000000`. **[V]** (§3, §11)
5. The SPS/PPS do **not** travel as a blob in the picture path. Two families of
   values do: (a) six small "syntax width" fields (`0x13c36a..0x13c36f`) packed
   into picture word `0x5400`, which look like the slice-header widths the
   hardware needs in order to parse the slice header itself; and (b) the
   CAVLC/VLC table *addresses* (`0x5800..0x5bd0`) computed from the SPS/PPS
   block by two helper routines. **[V]** for (a) and the addresses; **[I]** for
   "the hardware parses the slice header". (§5.1, §7.4)
6. **Frame-based decode, one mailbox message per slice packet.** The
   `appear once per picture` values (`first_picture`, DPB instance) live in the
   packet payload; everything else is re-emitted per slice, so a multi-slice
   frame is just the loop repeated. **[V]**
7. The DEVA firmware **does not parse the command words**. It contains no
   `0xabcdefab`/`0x80000007` materialisation anywhere (scanned the whole
   expanded image: no `0xabcd`/`0xabab` immediate), it treats the command
   buffer as opaque bytes described by (address, offset, size) from the
   message, and the command words are consumed by the MSVDX command processor.
   The container magic is a **host-side** invariant. **[V]** for "the firmware
   does not look at them", **[I]** for "the hardware does". (§9)

---

## 1. Where the stream is produced

`CH264Decoder::HOST_H264DecodeFrame` (`0xfffffff0062960a0`) runs the per-slice
loop at `0xfffffff0062967e8`–`0xfffffff0062968d8`. For each slice *i*:

```c
/* 0x62967d4 .. 0x6296844 */
blk = cmdbuf->wptr;                       /* struct apple_vxd_cmdbuf  */
ctx->slice_block = blk;
memset(blk, 0, 0x28);
cmdbuf->wptr += 0x28;
*(u32 *)blk = 0x90000000;
h264CmdBuf(ctx, cmdbuf, i);               /* 0x62b37d4               */
```

`h264CmdBuf` (`0xfffffff0062b37d4`, 63 instructions) is a plain sequential
composition — this is [V] from the disassembly, and it defines the whole
command order:

```c
/* 0x62b37d4 */
cmdbuf->[0x98] = ctx->0x13c24c;                  /* per-slice bookkeeping   */
ctx->0x13c380 = (ctx->0x13c380 & 0xfff8ffff)
              |  ctx->0x13c46c << 16
              |  ctx->0x13c46b << 17
              | (ctx->0x13c470 == 2) << 18;
memset(scratch, 0, 0x400);                       /* P[] parameter block     */
slice_params(ctx, scratch, i);                   /* 0x62b2620  prepare      */
ctx->0x13dbd8 = ctx->0x13c334 + (s16)scratch[0x64];  /* hdr size in words   */
build_pic_words(ctx);                            /* 0x62b38d0  prepare      */
build_ent_word(ctx, scratch);                    /* 0x62b55f0  prepare      */
emit_slice_desc(ctx, i);                         /* 0x62b5c8c  0xb + 0xa    */
emit_cmd_regs(ctx);                              /* 0x62b5d44  0x30xx/0x34xx*/
emit_ent(ctx, scratch, i);                       /* 0x62b6010  0xf + 0x5xxx */
emit_word(ctx, 0x60000000);                      /* end of slice            */
```

Two of the six write nothing: `0x62b38d0` and `0x62b55f0` are **pure
prepare** functions. Verified by scanning every `+0x88` (write-pointer) access
in their bodies — there is none. §5/§7 describe what they fill in.

### 1.1 The command-buffer object **[V]**

```c
struct apple_vxd_cmdbuf {          /* ctx + 0x136758, one per decoder      */
    u8     pad00[0x18];
    u32    offset;                 /* +0x18  base offset of this buffer    */
    ...
    void  *base;                   /* +0x80  buffer start                  */
    void  *wptr;                   /* +0x88  current write pointer         */
    ...
    u32    field_98;               /* +0x98  = ctx->0x13c24c               */
};
```

Three further objects use the same `{base @0x80, wptr @0x88}` shape and show
up in the generators **[V]**:

| ctx offset | role | evidence |
| --- | --- | --- |
| `ctx+0x136758` | the command buffer (goes to the firmware) | all six generators |
| `ctx+0x13d660` | **entropy table blob** buffer, filled by `0x62b6010`, then DMA'd | `0x13d660` writes at `0xfffffff0062b6a57` + the `0xf0000000` transfer |
| `ctx+0x13c8a0` | per-slice record buffer (0x18 bytes/slice) + surface at `+0x30` | §3.5 |
| `ctx+0x13c480` | optional "colocated" record (16 bytes), appended to the slice record | `0x62b6010` tail |

`cmdbuf_add_packet` = `FUN_fffffff0062982f8(buf, magic, size)` **[V]**: writes
`{magic, size, 0xabcdefab}` at `base+offset`, advances `offset += size + 0xc`,
returns the pointer *after* the header. The allocation gate is
`offset + size + 0xc < capacity` (`buf+0x30`), with the log line
`"AppleD5500: No space in command buffer - need to flush first"`.

`FUN_fffffff006298250(buf, nbytes)` is the single allocation primitive **[V]**:
`p = wptr; wptr += nbytes & ~3; return p;` — it asserts `nbytes % 4 == 0`.
Every emitter below is a thin wrapper over it.

---

## 2. Command grammar

All emitters live in `0xfffffff00629a34c` … `0xfffffff00629b15c` and were
verified instruction-by-instruction with Ghidra (`scripts/ghidra.py
disassemble_function`). **The opcode is the top nibble for `0x5/0x9/0xa/0xb/0xe`
and `0x0`; the register-write class additionally sets bit 28.**

| Word 0 | Words | Encoding | Meaning | Emitter |
| --- | --- | --- | --- | --- |
| `0x1…` | 1+N | `0x10000000 \| (count<<16) \| (reg & 0xffff)` + *count* values | **write *count* consecutive registers** starting at `reg`. Consecutive `reg`+4 writes are merged by incrementing the count field (`*p += 0x10000`). | `0x629b068` |
| `0x000f0000` | 2 | `0x000f0000 \| ((flags & 0xf0) << 16) \| (reg & 0xffff)`, value | **write one register**, second flavour; `flags` observed ∈ {0, 4, 0x10}. `0x10` correlates with values that are **device addresses** (see §2 note and §7). | `0x629aa5c` |
| `0x5` | 1 | `0x50000000 \| v` | single word (v must be < 0x10000000 in practice) | `0x629a568` |
| `0x9` | 0xa | `0x90000000` + **9 payload words** (0x28 bytes total) | **per-slice descriptor**. Apple emits it **all zero**. | open-coded at `0x6296774` |
| `0xa` | 2 | `0xa0000000 \| size`, `addr` | **load `size` bytes from device address `addr`** (`size < 0x10000000`, asserted). This is the slice-bitstream DMA. | `0x629a444` |
| `0xb` | 3 | `0xb0000001`, `w1`, `w2` | slice descriptor/context triplet (see §3.4) | open-coded at `0x62b5b00` |
| `0xe` | 2 | `0xe0000000 \| (size & 0xffff) \| flags`, `addr` | load `size` bytes (< 0xffff) with flags in bits 16..27 (< 0x100000). **Not used by H.264**; used by the MPEG/VP9 paths. | `0x629a34c` |
| `0xf` | 3 | `0xf0000000 \| len`, `addr`, `dst` | **transfer/execute `len` bytes at `addr` into destination page `dst`** — Apple uses it to install the generated entropy table blob. | open-coded at `0x62b5fcc` |
| `0x6` | 1 | `0x60000000` | **end of slice** (terminator) | `0x62b37d4` tail |

Field-width checks the emitters perform (they are the best evidence for the
field widths) **[V]**:

* `0xa` — `assert(size >> 28 == 0)`.
* `0xe` — `assert(size <= 0xfffe)`, `assert(flags < 0x100000)`.
* `0x1` — register address is truncated to 16 bits; count field is 12 bits.
* `0xf` — `len` is the *full* 28-bit field (no mask).

**Note on `0x1` vs `0x000f0000`.** Both write one register and both carry a
16-bit register address, and the register sets are disjoint: `0x1` is used for
`0x30xx/0x34xx/0x60xx/0x66xx`, `0x000f0000` for `0x50xx/0x54xx/0x5bxx/0x70xx/
0x74xx` (all 59 `0x1` call sites and 27 `0xf` call sites enumerated). The two
encodings differ in that `0x1` supports a *run* of registers while `0xf` has a
per-write 4-bit flag field. Treat them as two distinct write ports and do not
try to unify them. **[V]** for the observation, **[U]** for why (§14 Q1).

---

## 3. Per-slice framing, byte for byte

### 3.1 The 0x28-byte slice block **[V]**

```c
/* emitted by the caller, then h264CmdBuf runs, then: */
p = cmdbuf_add_packet(cmd, 0x80000007, 0x78);   /* 0xc + 0x78 */
memset(p, 0, 0x78);
p[0x4c/4] = 1;                        /* packet kind -> mailbox type 0x81  */
p[0x08/4] = ctx->[0x2786b];           /* slice number / first-MB group     */
p[0x0c/4] = slice_cmd_bytes;          /* bytes emitted for this slice      */
p[0x10/4] = ctx->0x13c254 + cmd->[0x18];  /* logical offset of that chunk  */
p[0x24/4] = ctx->[0x27a15];           /* SPS/PPS instance tag              */
if (i == 0)          p[4/4] |= 0x01;  /* first_slice_segment_in_pic        */
if (!ctx->[0x136254]) { p[4/4] |= 0x10; ctx->[0x136254] = 1; } /* first pic */
p[0x70/4] = *(u32 *)(ctx->slice_block + 4);   /* = 0 (block is zeroed)     */
ctx->0x13c254 += slice_cmd_bytes + pad;
```

`slice_cmd_bytes` is `cmdbuf->wptr - cmdbuf->base - ctx->0x13c254`, i.e. the
number of bytes the six generators produced for this slice — **the bitstream is
not counted here, because it is not in this buffer** (§3.3). `pad` is
`0x40 - (wptr & 0x3f)` zero bytes, written before the packet header.

**The 0x28 block is emitted empty.** Its word 0 is `0x90000000`, words 1..9 are
zero; nothing in the kext ever writes them (the only accesses to
`ctx->slice_block` are the store at `0x6296774` and the load at
`0x6296860` which copies word 1 into `params[0x70]`). So a driver reproduces it
with `0x90000000` followed by nine zeros. Whether the firmware or the MSVDX
command processor fills it is open (§14 Q2) — but *emitting it zeroed is
bit-exact with Apple*, which is what matters for bring-up.

### 3.2 Alignment **[V]**

```
… commands for slice i … | 0..63 zero bytes | {0x80000007,0x78,0xabcdefab} | 0x78 payload
```
The pad is computed on the *command* write pointer, so the 0x78 payload always
starts 64-byte aligned relative to the buffer if the buffer itself is. Apple
allocates eight `0x10000`-byte command buffers per decoder, which is also why
the mailbox transmits `cmd_va >> 8` (§3.2 of the round-4 doc).

### 3.3 The 0xa command carries the slice bytes **[V]** except where marked

`0xfffffff0062b5c8c` (184 bytes, fully decoded):

```c
/* 0x62b5c8c */
src  = ctx->0x13c890;                  /* msvdx_surface of the input buffer */
iVar5 = *(int *)(ctx + i*0x5c0 + 0xdfe38) - src->[0x10];   /* slice offset  */
iVar4 = src->[0x18];                                        /* GART VA       */

wptr = cmd->wptr;
cmd->wptr = wptr + 3;
uVar1 = *(u32 *)(ctx + i*0x5c0 + 0xe03e4);
wptr[0] = 0xb0000001;
wptr[1] = uVar1;
wptr[2] = *(u32 *)(ctx + i*0x5c0 + 0xdfe40);
emit_0xa(cmd, iVar4 + iVar5, *(u32 *)(ctx + i*0x5c0 + 0xdfe40));
```

* `0xe03e4` is filled by `FUN_fffffff0062a5c08(decoder->0x13c260)` at
  `0x6296620` — a bitstream-consumer query (bit length of the consumed slice
  header) **[I]**.
* `0xdfe40` is filled by the NAL walk at `0x62965f4` and is used both as the
  `0xa` size and as `0xb` word 2, so it is the **slice payload length in
  bytes** **[I]** — the one value `0xb` and `0xa` agree on.
* `0xdfe38` is the running bitstream position, `0xdfe44` the slice length in
  the NAL, `0xdfe30` the `nal_unit_type` **[V]** (all set in the same block).
* `ctx+0x13c890` is only ever *read* (`+0x10` as an int, `+0x18` as a
  pointer-sized device address), so its identity is **[I]**: the only object
  in the decoder with that shape is a surface. The driver's substitution is
  exact: `object->[0x10]` := the byte offset of the buffer start in the GART
  mapping (0 if the mapping starts at the buffer), `object->[0x18]` := its
  GART VA.

So `0xa` = "decode `slice_bytes` at `src_va + offset`". For a driver,
`src_va` is the GART VA of the `V4L2_PIX_FMT_H264_SLICE` capture buffer and
`offset` is the start of the slice data (`v4l2_h264_slice` bitstream offset).

### 3.4 The 0xb triplet **[I]**

`{0xb0000001, bit_length_of_consumed_header, slice_bytes}`. The `1` in the
first word is constant across every H.264 and VP9 use; the two payload words
are the two numbers the hardware needs to find the slice body (header bits
already consumed, body bytes). Treat as opaque and reproduce.

### 3.5 The per-slice record buffer (`ctx+0x13c8a0`) **[V]**

`0x62b6010` writes a 0x18-byte record per slice and reuses it as the
"colocated picture" source:

```
+0x00  u32  POC-delta word 0   (mirror of reg 0x5440)
+0x04  u32  POC-delta word 1   (reg 0x5444)
+0x08  u32  POC-delta word 2   (reg 0x5448)
+0x0c  u32  POC-delta word 3   (reg 0x544c)
+0x10  u32  mirror of reg 0x5438
+0x14  u32  mirror of reg 0x5434
```
plus `+0x7c` = previous slice's picture index, `+0x84` = max |POC delta| seen so
far, `+0x30` = surface descriptor whose `+0x10` is the CAVLC table blob address.

---

## 4. The parameter block `P[]` (scratch, 0x400 bytes)

`0x62b2620` fills a 0x400-byte stack block from the per-slice struct
(`ctx + i*0x5c0 + 0xdfe48`, 0x5c0 bytes per slice) and from the SPS/PPS block
(`ctx + sps_idx*0x4898`). Everything downstream reads `P[]`.
All offsets **[V]**; the "meaning" column is **[I]** unless marked.

| Off | Type | Source | Used at | Meaning |
| --- | --- | --- | --- | --- |
| `0x40` | u8 | `slice+0xdfe50` | `0x62b6010` | "block valid / continuation" — if 0 the whole `P[]` is re-zeroed |
| `0x41` | u8 | `slice+0xdff38` (1 bit) | `0x5010`? / word0 bit 4 | — |
| `0x42` | u8 | `slice+0xdff39` (≤1) | word0 bit 2 | — |
| `0x43` | u8 | `slice+0xdffc8` (≤1) | word0 bit 3 | — |
| `0x44` | u8 | `slice+0xe0331` (≤1) | word0 bit 16, `0x3414` bit 16 | — |
| `0x45` | u8 | `slice+0xe0331` hi? | `0x3414` bit 17 | — (read as `P+0x45`) |
| `0x46` | u8 | `slice+0xe033c` (≤1) | word0 bit 18 | — |
| `0x47` | u16 | `slice+0xe0330` | — | — |
| `0x49` | u8 | `slice+0xdfe50`-derived (≤1) | word1 bit 24 | — |
| `0x4a` | u8 | `ctx+0x13c46b`? (≤1) | word0 bit 19 | — |
| `0x4c` | u32 | `slice+0xdfe54` | `0x62b6010` decision | — |
| `0x50` | u32 | `slice+0xdfe5c` (0..3) | word0 bits 0-1, loop counts | slice **type** (P/B/I/I**) **[I]** |
| `0x54` | s16 | `slice+0xdfe64` | — | — |
| `0x56` | u16 | `slice+0xdff3c` (or `ctx+0x13c372`) | word0 bits 8-11, `0x5418` loop count | **first_mb_in_slice / MB count** |
| `0x58` | u16 | `slice+0xdff40` (or `ctx+0x13c373`) | word0 bits 12-15 | number of MBs in the slice |
| `0x5a` | s16 | `slice+0xdffcc` | — | — |
| `0x5c` | u16 | `5 - slice+0xe0320` (3 bits) | word0 bits 20-22 | — |
| `0x5e` | s16 | `slice+0xe0340` | — | — |
| `0x60` | s16 | `slice+0xe0344` | — | — |
| `0x62` | s16 | `slice+0xe0348` | — | — |
| `0x64` | **s16** | `(s16)@0x48c44 + (s16)@0xe0324 + 0x1a` | `0x62b37d4` | **this slice's header size in words** |
| `0x66` | 4×s16 | `@0xe0328`,`@0xe032c`,`@0xe0334`,`@0xe0338` | — | four 16-bit QP/offset pairs |
| `0x70+i*4` | u32[] | `slice+0xdff48+i*4`, `i < @0xdff3c` | — | per-MB-group list (`0x5418` addresses) |
| `0xac+i*4` | u32[] | `slice+0xdff88+i*4`, `i < @0xdff40` | — | second list |
| `0x128` | u32 | `@0xdffd0 + @0xdffd4` | — | shift/round constant |
| `0x130/0x1a8/0x220/0x310` | u32[26] | `@0xdffd8`, `@0xdfff8`, `@0xe0038`, `@0xe0078`, `@0xe00f8` | entropy tables | CAVLC coefficient tables |
| `0x16c/0x1e4/0x298/0x388` | u32[] | `@0xe0178`, `@0xe0198`, `@0xe01d8`, `@0xe0218`, `@0xe0298` | entropy tables | second (chroma) coefficient set |
| `0xe8`,`0xe9` | u8 | `slice+0xdff44/45` | — | — |
| `0xea`,`0xee`,`0xf2`,`0xf6`,`0xf9`,`0xfd` | u8 | — | `0x58xx` block | per-table min/max for the CAVLC tables |

The 0x220/0x310 and 0x298/0x388 groups are computed as
`(int)((n + d) - (n * (v + 2^s)) >> s)` clamped to ±n — the classic
**"signed residual after multiplying by a power-of-two step"** pattern, i.e.
the delta decode of the CAVLC coefficient tables. **[I]**

---

## 5. Picture words: `CMD` block `0x3400..0x3414`

Built by `0x62b38d0` (prepare) from the SPS/PPS block and the picture flags
word `ctx+0x13c380`. Bit positions are from the `|= expr << n` sequence; the
width in brackets is from the accompanying range check (`if (max < v) assert`).

### 5.1 `ctx+0x13c29c` → **reg `0x3400`** **[V]**

| Bits | Width | Source | Note |
| --- | --- | --- | --- |
| 0..2 | 3 | `ctx+0x13c364` bits 6..8 | chroma-related (§5.5) |
| 4..6 | 3 | `ctx+0x13c364` bits 3..5 | chroma-related |
| 8..9 | 2 | `ctx+0x13c36a` | syntax width field 1 |
| 12..13 | 2 | `ctx+0x13c36b` | syntax width field 2 |
| 16..17 | 2 | `ctx+0x13c36c` | syntax width field 3 |
| 20..21 | 2 | `ctx+0x13c36d` | syntax width field 4 |
| 24..26 | 3 | `ctx+0x13c36e` | syntax width field 5 |
| 28..30 | 3 | `ctx+0x13c36f` (only if bit 3 clear) | syntax width field 6 |
| 31 | 1 | `ctx+0x13c378` bit 0 | — |

The six "syntax width" fields come from `*(u8*)(SPS + 0x4fc + 4k)`, i.e. six
u32 slots holding small values **[V]**. Their widths (2,2,2,3,3,3) and the fact
that they are packed identically into `0x5400` (§6.1) make them almost
certainly the **slice-header element widths** (`log2_max_frame_num`,
`log2_max_pic_order_cnt_lsb`, `pic_order_cnt_type`, `num_ref_idx_*`) that the
hardware needs to parse the slice header itself **[I]** — an important design
fact for the driver: the hardware *does* parse the slice header, so the driver
must pass these numbers, not the parsed header fields.

`ctx+0x13c298` (very nearly a copy of `0x13c29c`, → reg `0x3408`) additionally
carries **[V]**:

| Bits | Width | Source |
| --- | --- | --- |
| 4..5 | 2 | `ctx+0x13c3dc` |
| 8..10 | 3 | `ctx+0x13c380` bits 6..8 |
| 12..13 | 2 | `ctx+0x13c44d` (SPS field, <4) |
| 16..20 | 5 | `ctx+0x13c38a` (≤0x1f) |
| 24..28 | 5 | `ctx+0x13c38b` (≤0x1f) |
| 31 | 1 | `ctx+0x13c378` bit 25 |

### 5.2 `ctx+0x13c2a0` → **reg `0x3404`** **[V]** — picture size in MBs

`0x62b38d0` builds it as four "dimension minus one" nibbles:

```
bits 12..15 = (ctx+0x13c378 << 4) & 0xf000  + 0x1000
bits  8..11 = (ctx+0x13c378 << 4) & 0xf00   + 0x100
bits  4..7  = (ctx+0x13c378 << 8) & 0xf0    + 0x10
bits  0..3  =  ctx+0x13c378        & 0xf    + 0x1
```
with `ctx+0x13c378` bits 4..11 also feeding `0x13c274` (§6.2). The `+0x1000/
+0x100` form is a **width/height-in-MBs minus one** pair **[I]**: `0x13c360`
and `0x13c362` (computed in `0x62b2bb8` as `width >> (log2+3)` and
`height >> (log2+3)`) are the MB dimensions, and this word is where they land.

### 5.3 `ctx+0x13c2a4` → **reg `0x3410`** **[V]**

| Bits | Width | Source |
| --- | --- | --- |
| 13..14 | 2 | `ctx+0x13c44e` (SPS field, <8 but masked to 2 bits here) |

plus bits set through the same accumulate path from `0x13c380` bits 7 and 9
(the `if ((0x13c380 >> 7) & 1)` / `>> 9` branches gate the **reference-list
reordering** code, §8).

### 5.4 `ctx+0x13c2a8` → **reg `0x3414`** (built by `0x62b55f0`) **[V]**

| Bits | Width | Source |
| --- | --- | --- |
| 2..3 | 2 | `P[0x42]` |
| 3 | 1 | `P[0x43]` |
| 8..11 | 4 | `P[0x56]` |
| 12..15 | 4 | `P[0x58]` |
| 16..17 | 2 | `P[0x44]` |
| 17..18 | 2 | `P[0x45]` |
| 19..20 | 2 | `P[0x5c]` |
| 20..22 | 3 | `P[0x5c]` |

(the overlap is in the original; the later write wins). This is the
**slice-level** word, so it is the one that changes between slices of a
multi-slice frame.

### 5.5 Where `ctx+0x13c364` comes from **[V]**

`0x62b2bb8` packs it from the SPS block:

| Bits | Source |
| --- | --- |
| 0..1 | `*(u16 *)(SPS+0x474) & 3` |
| 2 | `*(u8  *)(SPS+0x478) & 1` |
| 3..5 | `*(u16 *)(SPS+0x498) & 7` |
| 6..8 | `*(u16 *)(SPS+0x49c) & 7` |
| 9..12 | `*(u16 *)(SPS+0x4a0) & 0xf` |
| 13 | `*(u32 *)(PPS+0x4c4) == 0` |
| 14 | `*(u8  *)(ctx + pps*0x25c8 + 0x48c5a) == 0` |

Bits 0..1 are `chroma_format_idc`: they are re-exported to the entropy block as
`reg 0x5000 = (chroma & 3) << 28 | 0x10c0000` **[V]** and to `0x30a0` as
`(chroma & 3) << 16 | 0x10c` **[V]**, and `0x13c334 = ((table[chroma]) & 7) * 6`
is the per-MB chroma size in words **[V]**. That triple makes
`chroma_format_idc ∈ {0,1}` (4:2:0, 4:2:2) the only safe assumption; the
existing driver validation already rejects >1, keep it.

`SPS+0x47c` is `width << 4 | height << 36`, `SPS+0x484` = `profile_idc`,
`SPS+0x488/0x490` = two per-pixel size factors (`VAGetParams` case 4,
`0xfffffff006297128`, exports them to VideoToolbox as
`param[3..4] = @0x488 * @0x4ae0`, `param[5..6] = @0x490 * @0x4be0`) **[V]**.

---

## 6. Render / reference registers: `CMD` block `0x30xx`, `0x31xx`, `0x34xx`

### 6.1 `ctx+0x13c26c..0x13c284` → regs `0x5010`, `0x5400..0x5414` **[V]**

Order as emitted by `0x62b6010` (`0xfffffff0062b69fc`…`0xfffffff0062b6a90`):

| Reg | Source / expression | Content (bit table in §5.1 for `0x13c270`) |
| --- | --- | --- |
| `0x50c0` | `ctx->0x13c8a0->[0x40]->[0x10] & ~0x3f` | CAVLC table blob base, 64-byte aligned (flag `0x10`) |
| `0x5010` | `ctx+0x13c26c` | picture word A — **no visible writer** in the decompiled listing (the pointer is taken at `0x62b38d0` but never stored through), so it is most likely 0; **[U]**, §14 Q5 |
| `0x5400` | `ctx+0x13c270` | picture word B — same six syntax widths as `0x3400`, plus `0x13c378` bit 0 at 31 |
| `0x5404` | `ctx+0x13c274` | `(0x13c378 & 1) << 31` … `(0x13c378 >> 2 & 1) << 16` |
| `0x5408` | `ctx+0x13c278` | `P[0x58] << 12`, `0x13c38b << 24`, `(0x13c380 & 2) << 1` |
| `0x540c` | `ctx+0x13c27c` | `P[0x5c] << 20`, `0x13c3df`, `(0x13c389 & 0x1f) << 8 \| (0x13c388 & 0x1f)` |
| `0x5410` | `ctx+0x13c280` | **scaling list** summary 1: six 5-bit fields `0x13c44f..0x13c454` |
| `0x5414` | `ctx+0x13c284` | **scaling list** summary 2: six 5-bit fields `0x13c455..0x13c45a` |

`0x5410/0x5414` are only written when `(*(u16 *)(ctx+0x13c448) >> 10) & 1` is
set, i.e. when the PPS has a scaling matrix **[V]**; each is six 5-bit fields,
one per 4×4/8×8 list. That is the **only** path by which
`V4L2_CID_STATELESS_H264_SCALING_MATRIX` reaches the hardware — as six
per-list summary values, not as a matrix. What the hardware does with the
per-coefficient matrix is not visible in the kext (§14 Q4); the practical
answer for R3c is: keep `scaling_matrix4x4 == 0` (i.e. flat matrices) so the
"present" bit is 0, exactly like the baseline streams the driver will test
with.

### 6.2 `ctx+0x13c2ac[0..11]` → regs `0x5480..0x54ac` **[V]** — reference list L0

12 words = 24 references, **two 12-bit values per word**:

```
bits [11:0]  = ref[2k]    POC LSB (accumulated, see below)
bits [27:16] = ref[2k+1]  POC LSB
```
filled by the loop at `0xfffffff0062b69a0`…`0xfffffff0062b6a30`:

```c
acc = 0;
for (k = 0; k < ctx->0x13c38a; k++) {         /* 0x13c38a = count of refs  */
    acc += *(u16 *)(ctx + 0x13c38c + 4*k) + 1;   /* per-ref delta           */
    word = acc << (bits-1);                      /* bits = 0x13c36a+0x13c36b */
    ctx->0x13c2ac[k/2] |= (k & 1) ? (word << 16) : word;
}
```
and when the "uniform spacing" flag (`0x13c380` bit 9) is set instead, the same
slot gets `(int)(k * (ctx+0x13c32c)/(0x13c38a+1)) << (bits-1)` — i.e. evenly
spaced POC. `ctx+0x13c32c` is `MB_height * 8 << log2` (set by `0x62b7a18`).

`ctx+0x13c2dc[0..11]` → regs **`0x3480..0x34ac`** (cmd block, written by
`0x62b5d44`) and **`0x54c0..0x54ec`** (ent block, written by `0x62b6010`) is the
identical structure for **reference list L1** (`0x13c38b` is the L1 count).
Note the pair: the same 12-word list goes out **twice**, once per register
window. **[V]**

So: **reference picture lists are transmitted as POC (or frame_num) values,
not as addresses.** The addresses are in the two DPB blocks below.

### 6.3 The two DPB-address blocks **[V]**

`0xfffffff0062b5d44` writes one, `0xfffffff0062b6010` the other; both read the
same two sources:

```c
/* 2b5d44: regs 0x3100..0x311c / 0x3110..0x312c   (cmd block)  */
/* 2b6010: regs 0x5040..0x505c / 0x5050..0x506c   (ent block)  */
base = *(u32 *)(ctx + 0x13cf28) & 0xfffff000;
for (i = 0; i < 8; i++) {
    reg(R0 + 4*i)      = base;
    reg(R0 + 0x10 + 4*i) = *(u32 *)(ctx + 0x13cd58 + i*0x78) & 0xfffff000;
}
```
* `ctx+0x13cd58` is an array of eight 0x78-byte records (the
  `H264CommandBufferParams` size), field `+0x00` = a surface address, masked to
  4 KiB. These are the **8 DPB reference surfaces**.
* The two runs overlap (8 registers each, 4 apart); that is what Apple emits,
  do not "fix" it.
* In `2b5d44` the second run uses `0x3110`, in `2b6010` `0x5050` — same values,
  two register windows. `0x13cf28` carries a 28-bit assert, i.e. it is a
  device address.
* When `ctx+0x13dc69 == 0`, `0x3120..0x312c` are zeroed and `reg 0x3080 = 1`
  beforehand.

### 6.4 Address registers **[V]**

| Reg | Value |
| --- | --- |
| `0x3090` | **destination offset** of the entropy table blob in its window (patched, §7.4) |
| `0x3094` | **length**: `min(((bytes-1) >> 6), 0xf)` — i.e. number of 64-byte lines, capped at 15 (960 bytes) |
| `0x3098` | `ctx->0x13c8a0->[0x30]->[0x10] & ~0x3f` |
| `0x30a0` | `(chroma & 3) << 16 \| 0x10c` |
| `0x30a8` | picture word `0x13c310` |
| `0x30b0` | `0x13c314` (from `0x62b55f0`: `(P[0x4c] - k*step) * scale`, see §14 Q6) |
| `0x3400/0x3404/0x3408` | §5.1 / §5.2 |
| `0x3410/0x3414` | §5.3 / §5.4 |

---

## 7. Entropy (`ENT`) block: `0x5000..0x5bfc`

This is the bulk of the stream and the part that makes the decoder actually
produce coefficients. Order is exactly as emitted (`0x62b6010`,
`0xfffffff0062b6890`…`0xfffffff0062b6a2c`) **[V]**:

| # | Reg(s) | Value | Flags |
| --- | --- | --- | --- |
| 1 | `0x50c0` | CAVLC table blob VA `& ~0x3f` (`0x13c8a0->[0x40]+0x10`) | `0x10` |
| 2 | `0x5010` | `ctx+0x13c26c` | 0 |
| 3 | `0x5400,0x5404,0x5408,0x540c` | picture words | 0 |
| 4 | `0x5410,0x5414` | scaling summaries — **only if the PPS has a matrix** | 0 |
| 5 | `0x5040+4i`, `0x5050+4i` | DPB base + 8 surfaces `& ~0xfff` (§6.3) | 0 |
| 6 | `0x5030` | second blob VA `& ~0x3f` (`0x13c8a0->[0x30]+0x10`) | `0x10` |
| 7 | `0x5480..0x54ac` | **RefPicList0** POCs, 12 words (§6.2) | 0 |
| 8 | `0x54c0..0x54ec` | **RefPicList1** POCs, 12 words | 0 |
| 9 | `0x5438` | `sign_flags \| (2 - prev_pic_idx)` (bits 14:0 = per-ref negative-delta flags, bits 16+ = `2 - idx`) | 0 |
| 10 | `0x5440..0x544c` | 4 words × 4 POC deltas (`(int8)`, clamped ±0x7f) | 0 |
| 11 | `0x5434` | colocated surface VA (from `surface->[0x10]`) | `0x10` |
| 12 | `0x543c`,`0x5450..0x545c`,`0x5430` | colocated record (`ctx+0x13c480`), else 0 | 0 |
| 13 | `0x5000` | `(chroma & 3) << 28 \| 0x10c0000` | 0 |
| 14 | `0x5800,0x5840,0x5880,0x58c0,0x5900,0x5940,0x5980,0x59c0` | table VAs from `SPS/PPS_blob + 0x1200,0x1300,0xc00,0xd00,0xe00,0xf00,0x1000,0x1100` (4×4 transpose each) | 0 |
| 15 | `0x5a00,0x5a40,0x5a80,0x5ac0,0x5b00,0x5b40` | same, `+0x600,0x700,0x800,0x900,0xa00,0xb00` | 0 |
| 16 | `0x5b80,0x5b90,0x5ba0,0x5bb0,0x5bc0,0x5bd0` | same, `+0x000,0x100,0x200,0x300,0x400,0x500` (4 words each) | 0 |
| 17 | `0x5be0`,`0x5be4` | `@+0x1818`, `@+0x181c` | 0 |
| 18 | `0x5be8..0x5bfc` | 6 words `@+0x1800..` | 0 |

### 7.1 Where the table VAs come from **[V]**

```c
if (/* 0x13c378 bit 0 */) {
    lVar10 = (PPS_has_own_tables)
           ? ctx + pps_idx*0x25c8 + 0x48d28     /* PPS-local tables */
           : ctx + sps_idx*0x4898 + 0x518;      /* SPS tables       */
    /* … lVar10 + {0x1200,0x1300,0xc00,…} → regs 0x5800.. */
}
```
The offsets are **inside** Apple's SPS/PPS block, and `DecodeCreateH264VLD`
allocates `width << 19` bytes for it. In Apple's world that block is
GART-mapped, so `lVar10 + 0x1200` is a device address. **A Linux driver has to
reproduce that**: allocate one `width << 19` device buffer per SPS/PPS
generation, fill the CAVLC/VLC tables at the offsets above, and write the
resulting *device* addresses. The table *contents* are not in this document —
they are generated by `0x62b6*` from `P[]` and are the remaining chunk of work
(§14 Q3).

### 7.2 `0x62b6010`'s table transposition **[V]**

```c
/* FUN_fffffff00629ab2c(state, reg, src, n) */
for (k = 0; k < n; k++)
    reg_[reg + 4k] = (u32)src[perm_a] | src[perm_b]<<8 | src[perm_c]<<16 | src[perm_d]<<24;
```
with the 4-byte permutation taken from `DAT_fffffff005595040` (n == 4) or
`DAT_fffffff005595050` (n == 0x10). It is a 4×n word transpose, i.e. the four
coefficient sub-tables are written as four interleaved runs.

### 7.3 The colocated / POC-delta encoding **[V]**

```c
for (k = 0; k < 15; k++) {
    d = ctx->0x13c3e0 - *(int *)(ctx + 0x13c3e4 + 4*k + 0x10);  /* POC delta   */
    d = clamp(d, -0x7f, 0x7f);
    tmp[k/4] |= (d & 0xff) << (8 * (k & 3));                    /* 4 per word  */
}
/* 0x5438: bits[14:0] = per-ref "delta is negative" flags (from the byte
 * array at ctx+0x13c3e4), bits[16..] = 2 - previous_picture_index */
for (k = 0; k < 4; k++)
    emit(0x5440 + 4*k, tmp[k]);
emit(0x5438, sign_flags | (2 - prev_pic_idx) << 16);
```
That is the **`RefPicList0` + `RefPicList1` short-term reference POC deltas**,
16 entries, int8 packed.

### 7.4 The entropy blob transfer (`0xf0000000`) **[V]**

```c
/* 0x62b6010 tail, 0xfffffff0062b6ef8 */
n      = (P[0x50] == 0) ? 0x30 : (P[0x50] == 1) ? 0x20 : 0x10;  /* hdr bytes */
wptr  += n;                                  /* reserve the cmd prefix      */
nwords = state.nwords;                       /* words written to side buf  */
pad    = (nwords * 0x38 - n) & 0x38;
memset(state.base, 0, pad);
bytes  = n + nwords * 8;
state.wptr += pad;                           /* publish                    */
reg_0x3094 = min(((bytes-1) >> 6), 0xf);
emit_3words(0xf0000000 | (bytes + pad),
            side_va + side->[0x18],
            ctx->0x13d67c);                  /* dest offset, accumulates   */
reg_0x3090 = ctx->0x13d67c;
ctx->0x13d67c += bytes + pad;
```

Note the **two-pass trick**: `0x3090`/`0x3094` are emitted as `0` at the *start*
of `0x62b6010` and then **patched in place** (`*(u32 *)(saved_wptr - 4) = …`),
because their values are only known after the blob is built. A driver can
simply emit them at the end — the firmware reads the buffer linearly, not the
patch history.

---

## 8. Order of business for the picture-level values

`0x62b7a18` (`DecodeGetRenderTargetRef` support) precomputes, once per picture
**[V]**:

```c
s   = SPS;
bits = s->[0x13c36a] + s->[0x13c36b] + 3;
ctx->0x13c324 = 1 << bits;
ctx->0x13c328 = (8 << (s->0x13c36a)) * s->0x13c362 / (1 << bits) + …;   /* MB rows */
ctx->0x13c32c = (8 << (s->0x13c36a)) * s->0x13c360 / (1 << bits) + …;   /* MB cols */
ctx->0x13c330 = cols * rows;
ctx->0x13c334 = (chroma_size_table[s->chroma & 3] & 7) * 6;   /* hdr words  */
ctx->0x13c338 = …;  ctx->0x13c33c = …;
ctx->0x13c348 = chroma & 3;
ctx->0x13c34c = (u64 perm << 4) & 0x700000007;
```
`ctx->0x13c360` / `0x13c362` are `pic_width_in_mbs` / `pic_height_in_map_units`
**[V]** (computed in `0x62b2bb8` as `width >> (…)`).

---

## 9. Firmware cross-check

What the DEVA image actually does with all of this, from
`build/video/vxdfw/deva.expanded.dis` (base `0x01000000`, 43 529 disassembled
instructions, 96 % of the image decoded):

### 9.1 The 0x81 handler parses the *message*, not the command words **[V]**

`0x1000b08` (generic command-buffer path) does
`D1Ar1 = type; D0Ar2 = msg; CALLR 0x10005e8`. `0x10005e8` allocates a
0x4c-byte descriptor (`0x1000598` → `0x10118f0` + `memset(+0xc,0,0x4c)`), then
`0x1000d24` decodes the message into it. Verified field-by-field:

| desc | from | meaning |
| --- | --- | --- |
| `+0x0e` | `msg+2` (u16) | **fence** |
| `+0x0c` | computed | type class byte (`msg[1]`; `0x88`→`-0x68+…`, `0x86`→`-0x84`, `0x8a`→`-0x68`) |
| `+0x40` | `msg+6` (u16) | **w1[31:16] = size in words** |
| `+0x44` | `msg+4` | w1 |
| `+0x48` | `msg+8` | w2 |
| `+0x10` | `msg+0xc` (64-bit) | **w3 \| w4<<32** — the `(va>>8)\|ctx` pair |
| `+0x14` | `msg+0x10` (64-bit) | w4 \| w5<<32 |
| `+0x24` | `msg+0x14` (64-bit) | w5 \| w6<<32 |
| `+0x38` | `msg+0x18` if `nwords > 6`, else `0xffffffff` | w6 \| w7<<32 |
| `+0x3c` | `msg+8` (64-bit) | w2 \| w3<<32 |
| `+0x42` | `msg[28]` (type 0x88/0x86/0x8a only) | — |
| `+0x0c` byte = 1 | always | "job ready" |

Two facts fall out that matter for the driver:

* **`w1[31:16]` is a count in words** — this settles open question 2 of the
  round-4 doc (it is *not* a byte offset window). The low half of `w1` is the
  byte offset ([I]: the `& ~0x6000 | 0x4000` flags survive in bits 13-14).
* The descriptor is filled from the message **only**; nothing in this path
  reads the command buffer. Combined with the constant scan (no `0xabcd`/
  `0xabab` materialisation anywhere in the image), the conclusion is that the
  **container magic and every command word are consumed by the MSVDX command
  processor, not by the DEVA/MTX firmware**. **[V]** for the firmware side; **[I]**
  for the hardware attribution — the hardware has no other candidate.

`0x10005e8` then dispatches on codec type through
`[A1GbP+0x10] → [+0x24] → [+0x10]` and only looks at descriptor *flags*
(`+0x44` bits 2/4/7), never at stream contents **[V]**.

### 9.2 What the firmware *does* need from the host

* the command buffer's device address and the packet's word offset/size
  (message w1/w3) — **[V]**
* `TO_MTX` ring geometry, doorbell, IRQ — round-4 doc, unchanged **[V]**
* nothing about SPS/PPS/slices — **[V]**

So the "cross-check against the firmware parser" the round-4 doc asked for
comes out **negative**, and that is a useful result: it means the whole
command stream can be developed and diffed entirely on the host (byte-for-byte
against Apple's output for the same stream), with the firmware only ever
reporting success/failure through the 0xc0 completion. **[V]**

---

## 10. V4L2 stateless → command-stream mapping

Controls are the ones the driver already declares
(`.vdec-r3/apple-vxd-h264.c`). "Where" is the field that must change.

| V4L2 control / field | Lands in | Confidence |
| --- | --- | --- |
| `SPS.profile_idc` | `SPS+0x484`; validated only | **[V]** |
| `SPS.chroma_format_idc` | `0x13c364` bits 0-1 → `reg 0x3400` bits 0-2, `reg 0x30a0` bits 16-17, `reg 0x5000` bits 28-29, `0x13c334` header size | **[V]** |
| `SPS.pic_width_in_mbs_minus1` / `pic_height_in_map_units_minus1` | `0x13c360`/`0x13c362` → `reg 0x3404` (four nibbles), `0x13c32c`/`0x13c328` | **[V]** for the path, **[I]** for which nibble is which axis |
| `SPS.frame_mbs_only_flag`, `mb_adaptive_frame_field_flag` | `0x13c364` bit 2 | **[I]** |
| `SPS.log2_max_frame_num_minus4`, `log2_max_pic_order_cnt_lsb_minus4`, `pic_order_cnt_type`, `num_ref_frames` | `SPS+0x4fc…0x510` → `0x13c36a..0x13c36f` → `reg 0x3400` bits 8..30 **and** `reg 0x5400` bits 8..30 | **[V]** for the path; the six individual assignments are **[I]** |
| `SPS.max_num_ref_frames` | bounds the DPB (`< 0x16` guard) → `reg 0x3100/0x3110` block | **[V]** |
| `PPS.entropy_coding_mode_flag` | `P[0x50]`-family → `reg 0x3400` bits 0-1 | **[I]** |
| `PPS.pic_init_qp_minus26`, `chroma_qp_index_offset`, `second_chroma_qp_index_offset` | `P[0x66]` (four s16) → entropy block | **[I]** |
| `PPS.deblocking_filter_control_present_flag` + slice deblock fields | trigger the separate **0x87 deblock packet** (`0xfffffff0062c44b4`), not a command word | **[V]** for the packet, **[U]** for the fields |
| `PPS.pic_scaling_matrix_present_flag` | `(*(u16 *)(0x13c448) >> 10) & 1` gate → `reg 0x5410/0x5414` (six 5-bit summaries) | **[V]** |
| `SCALING_MATRIX` coefficient values | **not in the command stream**; only the six per-list summaries go out. Flat lists (all 16 for 4×4, 64 for 8×8) avoid the path entirely | **[V]** |
| `DECODE_PARAMS.top_field_order_cnt` / `bottom_field_order_cnt` | POC deltas → `reg 0x5438`, `0x5440..0x544c`, list words `0x3440..0x34ac` | **[I]** |
| `DECODE_PARAMS.frame_num` | `0x13c3e0`-family (used in the delta subtraction) | **[I]** |
| `DECODE_PARAMS.dpb[i]` → surface | `ctx+0x13cd58 + i*0x78` → `reg 0x3110+4i` (address) **and** `0x13c2ac/0x13c2dc` (POC) | **[V]** for the address block, **[I]** for the POC ordering |
| `DECODE_PARAMS.num_slices` | loop count of `h264CmdBuf` (= number of `0x9…0x6` groups and packets) | **[V]** |
| `SLICE_PARAMS.first_mb_in_slice` | `P[0x56]` (`SPS+0xdff3c`) → `reg 0x3414` bits 8-11, `reg 0x3400` bits 12-15 | **[I]** |
| `SLICE_PARAMS.slice_type` | `P[0x50]` → `reg 0x3400` bits 0-1 | **[I]** |
| `SLICE_PARAMS.frame_num` | `P[0x4c]` → `reg 0x30b0` | **[I]** |
| `SLICE_PARAMS.slice_qp_delta`, deblock alphas | `P[0x5a..0x62]` | **[I]** |
| `SLICE_PARAMS.header_bit_size` | `slice+0xe03e4` → `0xb` word 1 | **[I]** |
| slice bitstream bytes | `slice+0xdfe38` + `0xdfe40` → **`0xa` command** (address + length), **not** copied into the buffer | **[V]** |
| `DECODE_MODE` | frame-based only (one message per slice packet, `first_slice_segment_in_pic` bit 0 of payload `+0x04`) | **[V]** |
| `START_CODE` | Annex-B: the NAL walk at `0x6295xxx` expects start codes in the source buffer; keep `ANNEX_B` | **[V]** |
| `PRED_WEIGHTS` | `0x13c44d`/`0x13c44e` (2/3-bit fields in `reg 0x3408`/`0x3410`) | **[U]** |

**The single most useful consequence for the driver**: the frame-based path
does not need the driver to re-serialise PPS/SPS into the stream. What it needs
is (a) the six syntax widths, (b) the MB dimensions, (c) the chroma format,
(d) the CAVLC/VLC tables at `SPS+0x518 + {0x600…0x1300}`, (e) the DPB surface
addresses, and (f) the slice bitstream *address and length*. (d) and (f) are
where all the remaining work is.

---

## 11. The exact word sequence for one baseline/main IDR, one slice

Assuming: 1920×1088 4:2:0 8-bit, IDR, one slice, `first_picture`. `cb` =
command buffer, `S` = the SPS/PPS block, `A` = the CAVLC blob, `src` = the
GART VA of the input buffer. `§` refers to the section that defines the word.

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Word count for a single-slice IDR, from the emit sites **[I]** (each figure is
the emitted words *including* the command headers, and runs of consecutive
registers collapse via the `0x1` count field):

| Part | Words |
| --- | --- |
| slice block (`0x90000000` + 9) | 10 |
| `0xb` + `0xa` | 5 |
| CMD block (`0x3080`…`0x3414`, 7 merged runs) | ~69 |
| `0x3090`/`0x3094` placeholders | 4 |
| `0xf` transfer | 3 |
| ENT block, fixed part (`0x5010`…`0x5430`, `0x5000`) | ~82 |
| ENT block, CAVLC/VLC table addresses (`0x5800`…`0x5bfc`, **two words each**, no merging) | ~507 |
| terminator | 1 |
| **total** | **~680 words ≈ 2.7 KiB** |

Plus `0xc + 0x78` for the packet. So the 64 KiB buffer Apple allocates holds
~20 single-slice pictures; a driver should reserve `0x10000` (Apple's size) and
assert rather than compute. The table block dominates — if the tables were
written with the `0x1` run form instead, the stream would shrink by ~250 words,
but that is *not* what Apple does and is not worth the risk for bring-up.

---

## 12. C-level pseudocode

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Call site in `device_run()`: one call per `v4l2_ctrl_h264_slice_params`, then
one mailbox message **per packet** (round-4 doc §3.2) with
`w1 = (logical_off | 0x4004 /* last */ | 0x2000 /* else */)` and
`w3 = ((cmd_va >> 8) & 0xffffff00) | hwctx`.

---

## 13. Corrections to `ds-vxd-decode-protocol.md`

| Round-4 statement | Round-5 finding |
| --- | --- |
| "`params[0x0c]` = this slice's **bitstream** size" | it is the size of the **command bytes generated for the slice** (`wptr - base - 0x13c254`); the bitstream is a separate `0xa` command with its own address+length |
| "bitstream bytes are appended to the same buffer" | they are **not**: `0xa0000000\|len` + GART VA of the source buffer. No memcpy, no 64-byte pad on the payload |
| "the 0x28 slice block is filled by `h264CmdBuf` helpers" | it is emitted **all zero** except `0x90000000`; nothing in the kext writes words 1..9 |
| "`params[0x70]` = sequence/bitstream tag `[I]`" | `params[0x70] = slice_block[1]` = **0** (the block is zero) |
| "`params[0x24]` = SPS/PPS instance `[I]`" | it is `ctx+0x13d0a8` (`param_1[0x27a15]`); still only an *instance tag* — which index it is remains **[U]** |
| "`w1` high half = `params[0x18] >> 2` is Apple's size-in-words slot `[U]`" | the **firmware reads it as a word count** (`desc+0x40`, `0x1000d38`); for H.264 Apple leaves it 0, so the firmware falls back to the packet header |
| "the six command generators are ~90 KB of decompiled code, field layout is the next rung" | they are 1.4 + 7.5 + 1.7 + 0.2 + 0.7 + 6.7 KB, and two of them emit nothing; the field layout is §5–§7 above |
| "0x90000000 slice block / 0x60000000 end-of-slice" | correct, plus the missing `0xb`, `0xa`, `0xf` commands and the `0x1`/`0x000f0000` register-write pair |
| "the firmware parses exactly these words" | **it does not**; the DEVA firmware never looks at stream contents (§9) |

---

## 14. Open questions, ranked, with the experiment that settles each

1. **Why two register-write encodings?** (`0x1…` for `0x30xx/0x34xx/0x60xx`,
   `0x000f0000` for `0x50xx/0x54xx/0x5bxx/0x70xx`). Sets are disjoint, so both
   work; the difference is either a second write port or a bank selector.
   *Settle*: emit a `0x1`-form write to a `0x5xxx` register on hardware and see
   whether it lands. No driver risk either way — reproduce Apple's choice.
2. **Who fills the 9 payload words of `0x90000000`?** Nothing in the kext does.
   *Settle*: after R3a, dump the command buffer from the host *after* a
   completion and diff against what was written. If the firmware/hardware
   wrote it, the words change.
3. **The CAVLC/VLC table contents** at `S+0x600…0x1300` and `S+0x1800`. The
   generator that fills them is `0x62b6*` reading `P[]`; not decoded here. This
   is the largest remaining piece of R3c and it is *self-contained* host work.
   *Settle*: decompile `0x62b6010`'s first half (`0x62b6010+0x40` …
   `0x62b6010+0x300`) with the `--slots` view; the coefficient tables are
   written to `P[0x130/0x1a8/0x220/0x310]` and `P[0x16c/0x1e4/0x298/0x388]`.
4. **Do the `0x5410/0x5414` scaling summaries have to match the actual
   matrices?** They are six 5-bit per-list values; the matrices themselves
   never leave the host. *Settle*: decode `0x13c44f..0x13c45a`'s producer
   (search `+ 0x13c44f` in the decompilation) — if it is `max(coef) - min(coef)`
   the driver must compute it from `SCALING_MATRIX`; if it is a constant for
   flat lists, flat lists need nothing.
5. **`0x5010` (`ctx+0x13c26c`) and `0x30a8` (`ctx+0x13c310`)** — `0x5010` has no
   visible writer; `0x30a8`'s bit table could not be separated from the
   accumulate/assert noise. *Settle*: run `cmdstream.py --slots` on
   `0xfffffff0062b38d0` and read the `0x13c310` block (around `0x62b38d0+0x4c0`);
   for `0x13c26c`, search for a store through the pointer at `0x62b38d0`.
   Both are safe to emit as 0 for the first bring-up.
6. **`P[0x50]`'s 2-bit value** (word0 bits 0-1): slice type or entropy mode.
   *Settle*: decode one I and one P slice of the same stream and diff the
   emitted word — one experiment, no hardware.

---

## Appendix A — address index for this round

| Address | What |
| --- | --- |
| `fffffff006298250` | `cmdbuf_alloc(buf, nbytes)` — the only allocation primitive |
| `fffffff0062982f8` | `cmdbuf_add_packet(buf, magic, size)` → `{magic,size,0xabcdefab}` |
| `fffffff00629a34c` | emit `0xe` (load size/flags + addr) |
| `fffffff00629a444` | emit `0xa` (load len + device addr) |
| `fffffff00629a568` / `…610` | emit `0x5` (one/two-field word) |
| `fffffff00629a6c0` | emit a raw word |
| `fffffff00629a81c` | patch packet size / close writer |
| `fffffff00629a9b4` | writer state init on a buffer |
| `fffffff00629aa5c` | emit `0x000f0000 \| flags \| reg` + value |
| `fffffff00629ab2c` | emit an n-word transposed table into `reg..reg+4n` |
| `fffffff00629afc4` / `…b068` / `…b15c` | begin / add / flush a `0x1`-form register group |
| `fffffff0062b2344` | scaling-list helper (called from `0x62b2620`) |
| `fffffff0062b2620` | fill `P[]` (the 0x400 parameter block) |
| `fffffff0062b2bb8` | build `ctx+0x13c360…0x13c36a` from the SPS/PPS block |
| `fffffff0062b37d4` | **`h264CmdBuf`** — the per-slice composition |
| `fffffff0062b38d0` | build picture words `0x13c26c…0x13c2a4`, `0x13c30c…0x13c314` |
| `fffffff0062b55f0` | build `0x13c2a8`, `0x13c314` |
| `fffffff0062b5c8c` | emit `0xb` + `0xa` (slice descriptor + bitstream load) |
| `fffffff0062b5d44` | emit the `CMD` register block (`0x3080…0x3480`) |
| `fffffff0062b6010` | entropy blob + `0xf` transfer + `ENT` register block |
| `fffffff0062b7a18` | per-picture MB geometry (`0x13c324…0x13c34c`) |
| `fffffff0062b7ab0` | `DecodeGetRenderTargetRef` (render-target packet, kind 4) |
| `fffffff0062960a0` | `HOST_H264DecodeFrame`: NAL walk, per-slice loop, packet |
| `fffffff0062967e8`–`0x62968d8` | the slice loop itself (the code in §3.1) |
| `fffffff0062965f4` | NAL record fill (`slice+0xdfe30/38/40/44`, `0xe03e4/8`) |
| `fffffff006297128` | `VAGetParams` — SPS field semantics (`profile`, `width/height`, …) |
| `01000d24` (fw) | message → descriptor decode (§9.1) |
| `010005e8` (fw) | `0x81` handler, codec dispatch |

## Appendix B — tool added by this round

```sh
# the six generators, as an ordered "what leaves" list
python3 tools/ds-video/cmdstream.py --emit \
    FUN_fffffff0062b37d4 FUN_fffffff0062b5c8c FUN_fffffff0062b5d44

# the bit layout of every ctx+0x13cXXX word
python3 tools/ds-video/cmdstream.py --slots FUN_fffffff0062b38d0
```

It reads `build/video/d5500-decomp.txt` (produced by
`tools/ds-video/ghidra-kext.py`) and needs no Ghidra session. For anything the
compaction loses, the shared Ghidra server has the same kext analysed:
`scripts/ghidra.py disassemble_function address=0xfffffff0062b5c8c`.

---

## 15. VLC tables resolved

2026-10-24. Round 6, lane `a9/vdec`. **No device access, no commits.**
Closes §14 Q3, corrects §7.1 and §11, and narrows §14 Q1/Q4/Q6.

New tool: `tools/ds-video/vlc_tables.py` (+ generated
`tools/ds-video/vxd_h264_tables.h` for the kernel driver), and
`tools/ds-video/macho.py`, which maps an unslid VA to a file offset so the
kext's constant tables can be read without a Ghidra session.

### 15.0 Bottom line

1. **There are no CAVLC tables at `S+0x600`.** The `0x5800..0x5bfc` register
   block that §7.1/§11 called "the CAVLC/VLC table addresses" is the
   **scaling-matrix (dequant weight) area**: 20 tables of 8-bit coefficients
   plus 8 DC words, generated *once per SPS/PPS parse*, not per slice. **[V]**
2. **The real CAVLC table is one constant blob**: 544 × 16-bit entries
   (1088 = 0x440 bytes) at kext `__TEXT 0xfffffff005596d08`.
   `DecodeCreateH264VLD` `memcpy()`s it once, at decoder create, into a
   0x1000-byte 8-byte-aligned GART buffer; the command stream then only
   publishes that buffer's device address, once per picture, in ENT register
   **`0x50c0`** (write form `0x000f0000 | 0x10<<16 | reg`, i.e. flag `0x10` =
   "device address"). **[V]**
3. That blob is **byte-identical, 544/544 entries, to Imagination's
   `h264_vlc_table_data[]`** shipped in the open-source VXD driver
   (`TexasInstruments/img-vxe-vxd`, `decoder/h264_vlc.h`), whose companion
   index (`h264_idx.h`) names all 38 logical tables it contains — every
   H.264 9.2 CAVLC table. Content is **constant**: no SPS/PPS/slice
   dependence whatsoever, so a driver embeds it as a `const` array. **[V]**
4. The scaling-matrix area is **also constant for any stream without scaling
   matrices — and in that case it is not emitted at all**: `0x13c378` bit 0
   gates the entire `0x5800` block, so a baseline/main stream with
   `seq_scaling_matrix_present_flag = 0` writes **none** of its 512 command
   words. **[V]**
5. §14 Q3's premise is wrong twice: `0x62b6010`'s first half builds the
   per-slice **entropy blob** (the `0xf` transfer payload, a different
   buffer), and the `S` area is generated in **`parseSPS`** by `0x62a9344`
   (defaults) or `0x62a8e90` (scale lists present). **[V]**
6. **Bonus correction (§7/§11)**: the whole ENT block — `0x5000`, `0x50c0`,
   `0x5030`, `0x5400…`, `0x5480…`, `0x5438/0x5440`, `0x5800…0x5bfc` **and**
   the `0x70xx/0x71xx` window — is not emitted by `0x62b6010` at all. It is
   built by **`0x62b7ab0` (`DecodeGetRenderTargetRef`)**, called **once per
   picture** from `HOST_H264DecodeFrame` (`0x62968e8`), into its **own
   `0x78`-byte payload with kind 4 → mailbox type `0x88`**. The per-slice
   `0x81` packet carries only the `0x90000000` block, `0xb`, `0xa`, the CMD
   block (`0x3080…0x34ac`), `0x3090/0x3094`, the `0xf` blob transfer and the
   `0x60000000` terminator. **[V]**

| Question §14 asked | Answer |
| --- | --- |
| which H.264 CAVLC tables does it build? | none — that is the scaling-matrix generator. The CAVLC set is a shipped constant (§15.2) |
| exact on-device table format? | 8-bit entry per u32 word in the staging area; 4 entries per 32-bit register, one **2×2 block in z-order**; 64 entries per 8×8 list, 16 per 4×4 list, 8 DC words (§15.4) |
| index scheme? | staging image is in raster order; the source constants are in scan order and the generator deposits them at `scan[i]` (§15.3) |
| constant per stream, or SPS/PPS/slice-dependent? | **constant** when no scaling matrices; **SPS/PPS-dependent** when they are present; never slice-dependent (§15.5) |
| precompute once at driver init? | yes for the CAVLC blob (always) and for the scaling area (common case), and then the scaling area is not even emitted (§15.6) |

### 15.1 Three things were conflated; only one is a VLC table

```
  init, once per decoder                  per SPS/PPS parse            per slice
  ┌───────────────────────────┐          ┌──────────────────────┐    ┌──────────────┐
  │ 0x440-byte CAVLC blob     │          │ S+0x000 … S+0x181f   │    │ entropy blob │
  │ __TEXT 0x5596d08          │          │ scaling matrices     │    │ 0x13d660     │
  │ → 0x1000 buffer 0x6338    │          │ → regs 0x5800..0x5bfc│    │ → 0xf xfer   │
  │ → reg 0x50c0 (flag 0x10)  │          │ (gated, see §15.6)   │    │ 0x13d67c     │
  └───────────────────────────┘          └──────────────────────┘    └──────────────┘
        the CAVLC tables                   the scaling lists          the stream state
                                           emitted per picture         emitted per slice
                                           in the 0x88 packet          in the 0x81 packet
```

* `0x5596d08` → `0x1000` buffer: `DecodeCreateH264VLD` (`0x62c3d98`) allocates
  `(…, 0x1000, 8 /*align*/, 1, ctx+0x6338)` at `0xfffffff0062c3f9c`, then
  `memcpy(*(ctx+0x6340), 0xfffffff005596d08, 0x440)` at
  `0xfffffff0062c4028…0x62c4038`. **[V]**
* `S` = `ctx + sps_idx*0x4898 + 0x518` = (SPS block at `ctx+0x2a8+sps*0x4898`)
  `+ 0x270`, which is exactly the `dest` passed to the generators at
  `0xfffffff0062a8618`. **[V]** (this pins the round-5 `S` to the generator.)
* The `0x5800` block is read back by `0x62b7ab0` (kind 4 → type `0x88`), the
  same function that emits `0x50c0`, the DPB base/8 surfaces, the two
  RefPicList POC runs and the `0x70xx` window — i.e. the entire entropy /
  render-target message is **one per picture**. §7 attributed it to
  `0x62b6010`; that attribution is wrong (`0x62b6010` emits no `0x5xxx`
  register at all). **[V]**
* The entropy blob (`0x13d660`, `0xf0000000` transfer) is **not** any of the
  above; it is built by `0x62b6010`'s first half from `P[]` and carries the
  per-slice `slice_type`-dependent control words. **[V]**

### 15.2 Artifact 1 — the CAVLC code table (constant, 1088 bytes)

Format of one 16-bit entry, from the open-source driver's
`decres_pack_vlc_tables()` (`decoder/dec_resources.c:40`): **[V]**

```
  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 ┌──────┬──────┬──────────────────────────────────┐
 │opcode│width │            symbol                │
 └──────┴──────┴──────────────────────────────────┘
   packed = opcode<<12 | width<<9 | symbol
```

The 38 logical tables inside the 544 entries, with their start offsets (the
`h264_idx.h` index, valid for Apple's blob because the blobs are identical):
**[V]**

| # | table (ITU-T H.264 clause 9.2) | entries | start |
| --- | --- | --- | --- |
| 0–5 | `coeff_token` (Table 9-5): nC 0–1, 2–3, 4–7, fixed-length nC ≥ 8, chroma DC 4:2:0, chroma DC 4:2:2 | 76, 84, 71, 13, 17, 40 | 0, 76, 160, 231, 244, 261 |
| 6–20 | `total_zeros` (Table 9-7) `TotalZeros_00..14` | 25,19,18,16,15,12,12,11,9,8,6,4,3,2,2 | 301 … 461 |
| 21–23 | `total_zeros` chroma DC 4:2:0 (Table 9-8) | 4,3,2 | 463, 467, 470 |
| 24–30 | `run_before` (Table 9-10) `Run_00..06` | 2,3,4,6,7,8,18 | 472 … 502 |
| 31–37 | `total_zeros` chroma DC 4:2:2 (Table 9-9) | 6,4,4,4,2,2,2 | 520 … 542 |

Total 544. The blob is a **decode-tree encoding**, not a flat `(code, length)`
list **[I]**: e.g. the `nC ≥ 8` "fixed length" table spans 13 entries whose
`symbol` fields run 6, 10, 14, 18, 22, 26, 30 — an index-like progression, not
62 leaves — so the VEC's VLC engine walks `opcode`/`width` nodes and only the
terminal ones produce a `(TotalCoeff, TrailingOnes)` pair. The *naming* of the
38 tables is the driver's and is exact; the *node semantics* would need the
MSVDX TRM and is not needed to reproduce the bytes.

**Driver recipe** (this is all of R3c's "VLC setup"):

```c
/* once, at decoder create */
vlc = dma_alloc_coherent(dev, 0x1000, &vlc_dma, GFP_KERNEL);   /* 8-byte align */
memcpy(vlc, vxd_h264_vlc_table, VXD_H264_VLC_BYTES);           /* 1088 bytes */

/* per slice, in the ENT block, where Apple emits it */
cb_reg1(cb, 0x50c0, vlc_dma & ~0x3f, 0x10);    /* 0x000f0000|0x10<<16|0x50c0 */
```

No other register, table, index or per-picture value is needed for CAVLC on
this core: the 38 table boundaries are implied by the hardware (the D5500
does **not** use the `VLC_TABLE_ADDR/INITIAL_WIDTH/INITIAL_OPCODE` register
triple that the same driver programs on the VXE/VXD through its firmware —
Apple never writes those registers, and §9 already showed the DEVA firmware
does not touch the command stream). **[V]** for "Apple writes only `0x50c0`",
**[I]** for "the hardware's table split is hard-wired".

### 15.3 Artifact 2 — what `S+0x000 … S+0x181f` really is

The generators are two functions in `parseSPS` (`0x62a7680`), reached from
`0x62a8620`/`0x62a8630` after two 1-bit syntax elements have been stored in
`sps+0x26c`/`sps+0x26d`: **[V]**

| fn | when | what |
| --- | --- | --- |
| `0xfffffff0062a9344` (344 B) | list absent | fills defaults (this is the "first half" shape §14 Q3 described) |
| `0xfffffff0062a8e90` (2328 B) | list present | reads `se(v)` coefficients and stores them |

Both write the same `dest = S` in the same layout, and the layout is
**HEVC-shaped**, which is the giveaway: 4 block sizes × 6 lists (2 for the
largest), and a DC coefficient **only for the two largest** — exactly
`scaling_list_data()`'s `sizeId` loop with
`if (sizeId > 1) scaling_list_dc_coef_minus8`. **[V]**

```
 g  entries  lists   S range             meaning (H.264)      (HEVC)
 0    16      6      0x000..0x5ff       4x4 lists  0..5      4x4
 1    64      6      0x600..0xbff       8x8 lists  0..5      8x8
 2    64      6      0xc00..0x11ff      unused               16x16 + DC
 3    64      2      0x1200..0x13ff     unused               32x32 + DC
                    0x1400..0x17ff      never written
 DC  8 words         0x1800..0x181f      S+0x1800+0x18*(g-2)+4*sub
```

* entry = one **u32 per coefficient, low byte**, so a 64-entry list occupies
  `0x100` bytes, and the whole area is `0x1820` bytes.
* the DC area holds 6 words for g=2 and 2 for g=3 (`0x10` = 16 = "flat"),
  and the emit sends `S+0x1818/0x181c` to `0x5be0/0x5be4`, `S+0x1800..0x1814`
  to `0x5be8..0x5bfc`. **[V]**
* for H.264 only groups 0 and 1 are meaningful (H.264 has no 16×16 or 32×32
  transform, so groups 2/3 are inert **[I]**). Groups 2 and 3 exist because
  the same `AVC_RBSP` object is shared with the HEVC path (the object owns the
  4×4/8×8/16×16/32×32 matrix scratch: ctor `0x62a56c4`, allocator `0x62a5634`,
  `IOMallocZeroData` of `(4<<k)²·4` bytes). **[V]**

### 15.4 The generator, decoded (`0x62a9344`, and the layout shared by `0x62a8e90`)

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

* `TBL_A` = `__TEXT 0xfffffff005595f34`, `TBL_B` = `0xfffffff005596034`, both
  64 u32. They are the default **8×8 intra / inter** weightings in the
  hardware's normalised domain, in **scan order**. `TBL_B` is exactly a
  function of the frequency diagonal, `f(i+j) = 16, 16, 16, 16, 17, 18, 20,
  24, 25, 28, 33, 41, 54, 71, 91`; `TBL_A` is a slightly steeper, symmetric
  but non-diagonal matrix. Both are reproduced byte-exactly by the tool; the
  closed form that produced them from ITU-T Table 7-4 is **[U]** — but it is
  not needed, because they are constants. **[V]** for the values, **[U]** for
  the derivation.
  (Note they are *not* Table 7-3/7-4 verbatim: the H.264 default 8×8 intra
  list is 6…42, this is 16…115. Whatever the transform is, the kext applies it
  at SPS-parse time, not at decode time.)
* `scan4`/`scan8` come from `0x62a2af7e4`, called by the `AVC_RBSP`
  constructor `0x62a5634` for all four sizes; the arrays at `rbsp+8`,
  `rbsp+0x10`, `rbsp+0x18` (and `+0x20…`) are the **scan / raster /
  column-major** orders of that block size. The scan walks each anti-diagonal
  from the bottom-left corner up, i.e. it is the transpose of the classic JPEG
  zig-zag: `n=4 → 0,4,1,8,5,2,12,9,6,3,13,10,7,14,11,15`. **[V]**
* The "present" path `0x62a8e90` has the identical skeleton; instead of the
  constants it stores the `se(v)`-decoded coefficients at the same
  `4*scan[i]` positions, and stores the DC word for `g > 1`. Its
  delta/`nextScale` arithmetic is **[U]** in detail (the decompiled `+ 9`
  constant does not match H.264 7.3.2.1.1.1's start-at-8 rule literally);
  this only matters if a stream actually carries scaling lists.

### 15.5 On-device format of the `0x5800` block

`0x5800..0x5bff` is a 0x400-byte register window — [V] for the addresses, [I]
for reading it as **16 slots × 64 registers** (the 14 64-entry tables sit one
per 0x40-register slot at `0x5800+0x40k`; the six 4×4 tables and the 8 DC
words share the last two slots at 0x10 strides). The host fills it with
`0x629ab2c(state, reg, src, n)`: **[V]**

```c
/* 0x629ab2c */
perm = (n == 0x10) ? &DAT_55595050 : &DAT_55595040;   /* 0x5595050 / 0x5595040 */
for (i = 0; i < n; i++) {
    u32 v = 0;
    for (k = 0; k < 4; k++)
        v |= src[perm[4*i + k]] << (8*k);      /* 4 table bytes per reg */
    emit_0x000f0000(reg + 4*i, v, /*flags*/ 0);
}
```

* `src` is the `S` staging image read as **u32 words**, so `src[j]` is
  `S[4*j]` = one 8-bit coefficient. All values are ≤ 0xff by construction
  (assert in the packer).
* For the 8×8 lists the gather makes each register one **2×2 block in z-order**
  (top-left, bottom-left, top-right, bottom-right) and the 16 registers walk
  the four 2×2 block-rows in Apple's fixed order — e.g. for `TBL_B`
  (`f(i+j)`): `0x5b00=0x10101010`, `0x5b04=0x11101010`, `0x5b08=0x11101010`,
  `0x5b0c=0x14121211`. For the 4×4 lists (`n = 4`) the same rule with a
  4-entry perm.
* Register order in the stream is **group 3 first**, then 2, 1, 0:

| reg | S offset | list |
| --- | --- | --- |
| `0x5800`, `0x5840` | `0x1200`, `0x1300` | g3 l0, l1 |
| `0x5880`…`0x59c0` | `0x0c00`…`0x1100` | g2 l0…l5 |
| `0x5a00`…`0x5b40` | `0x0600`…`0x0b00` | g1 l0…l5 |
| `0x5b80`…`0x5bd0` | `0x0000`…`0x0500` | g0 l0…l5, **4 words each** |
| `0x5be0`, `0x5be4` | `0x1818`, `0x181c` | DC g3 l0, l1 |
| `0x5be8`…`0x5bfc` | `0x1800`…`0x1814` | DC g2 l0…l5 |

  14 × 16 + 6 × 4 + 8 = **256 registers = 512 command words** (each
  `0x000f0000|reg, value`, never merged into a `0x1` run). §11's "~507 words"
  was the right order of magnitude with the wrong composition.

### 15.6 When it is emitted, and what to do

`0x62b7ab0` wraps the whole block in one test — `0xfffffff0062b7fec`,
`ldrb w8,[x26]` with `x26 = ctx + 0x13c378`: **[V]**

```c
if (*(u8 *)(ctx + 0x13c378) & 1) {  ...all 256 register writes...  }
```

and bit 0 of `0x13c378` is set in `0x62b2bb8` from the SPS/PPS: **[V]**

```c
bit0 = SPS_has_scaling_matrix || PPS_has_own_tables;
```

so:

| stream | scaling block | CAVLC blob |
| --- | --- | --- |
| `seq_scaling_matrix_present_flag = 0`, `pic_scaling_matrix_present_flag = 0` (every baseline/main stream we will test with) | **not emitted at all** | `0x50c0` + the blob |
| matrix declared, some lists absent | emitted from the constant image in §15.4 | same |
| matrix declared, lists present | emitted from the parsed lists (§15.4) | same |

**R3c decision**: the 0x88 packet is needed anyway (it also carries the DPB
surfaces and both RefPicList POC runs), so implement it — but leave the
scaling part out until a stream actually carries scaling lists: for the first
row of the table above there is nothing to generate, 512 command words
disappear, and no scaling-list parser is needed. Keep
`V4L2_CID_STATELESS_H264_SCALING_MATRIX` rejected until then, exactly as §6.1
already recommended. The generated header carries the constant image and a
ready-made `vxd_h264_scl_regs()` for the day it is needed.

### 15.7 Cross-checks

| check | result |
| --- | --- |
| Apple's 0x440 blob vs `h264_vlc_table_data[]` (TI/Imagination, 544 entries) | **544/544 identical** (`vlc_tables.py --verify`) **[V]** |
| table boundaries vs `h264_idx.h` (38 named tables) | all 38 present, offsets 0…544, no gap **[V]** |
| entry field split vs `decres_pack_vlc_tables()` | `opcode<<12｜width<<9｜symbol` reproduces all 544 words **[V]** |
| scaling area vs `h264fw_picture_ps` in the same driver | `scalinglist4x4[6][16]` + `scalinglist8x8[6][64]` — same 6-list/16+64 shape, so the group semantics agree **[V]** |
| scaling area vs ITU-T H.264 Tables 7-3/7-4 | **not** the raw tables (16…115 vs 6…42); the kext applies an unidentified normalisation, `[U]`, value-exact in the tool |
| CAVLC `symbol`/`opcode`/`width` semantics vs ITU-T 9.2 | reproduced bit-exactly but **not decoded semantically** `[I]`; unnecessary, since the bytes are a shipped constant |
| `0x62b6010` as the generator (round 5) | refuted: its first half fills `0x13d660`, not `S` **[V]** |
| Python vs C generator | 256/256 registers identical (`cc` build of the header vs `--regs`) **[V]** |

Reproduce:

```sh
python3 tools/ds-video/vlc_tables.py --extract --verify --selftest
python3 tools/ds-video/vlc_tables.py --default     # S image, non-zero rows
python3 tools/ds-video/vlc_tables.py --regs        # the 256 (reg,value) pairs
python3 tools/ds-video/vlc_tables.py --header > tools/ds-video/vxd_h264_tables.h
```

Reference digests of the two constant artifacts (for a byte-exactness test on
the device later): CAVLC blob `crc32 = 0xa0d60f2e`; default scaling image
(0x1820 bytes) `crc32 = 0x593efb28`.

### 15.8 What else in §14 this settles

* **Q3 — closed.** See above. The remaining `[U]` is only the algebraic origin
  of `TBL_A`/`TBL_B` and the exact `se(v)` delta rule for present lists; both
  are irrelevant while scaling matrices are unsupported.
* **Q4 — narrowed, not closed.** `0x5410`/`0x5414` are twelve 5-bit fields
  (`ctx+0x13c44f..0x13c45a`) filled at SPS/PPS parse time in `0x62b2bb8`
  (copied out of the PPS block at `pps+0x4a590…`), i.e. they are a
  **per-list summary computed by the parser**, not by the command generator,
  and they are emitted only when `(*(u16*)(ctx+0x13c448) >> 10) & 1` — the
  same "PPS has a matrix" gate as §15.6. So flat/absent lists need nothing;
  the exact formula stays **[U]** and is moot while the control is rejected.
* **Q6 — narrowed.** `P[0x50]` is a 2-bit value (asserted `< 4`) that
  (a) selects the entropy blob's header size `0x30/0x20/0x10`
  (`0x62b6010`), (b) is written into stream-state word 0 bits 0–1 and
  word 3 low bits as `2 - P[0x50]`, and (c) in the `P[]` filler `0x62b2620`
  selects the *explicit per-MB-group list* path when `< 2` versus a pair of
  SPS-level scalars (`ctx+0x13c372/0x13c373`) when `≥ 2`. That is consistent
  with a **slice-type rank with 0/1 = inter and ≥2 = intra**, i.e. word0
  bits 0–1 are the slice type **[I]**; the round-5 experiment (diff one I and
  one P slice) still settles it and is still cheap.
* **Q1 — unchanged** (`[U]`): the `0x1`/`0x000f0000` split is still a port
  question. New datum: the flag field's `0x10` is applied to exactly the
  *device addresses* (`0x50c0`, `0x5030`, DPB surfaces, `0x5434`, `0x7018`,
  `0x71ec`) and to nothing else, which matches Imagination's
  `CMD_REGISTER_BLOCK_FLAG_VLC_DATA` — i.e. it looks like a **data-path
  selector** for table/address writes, not a spare bit. **[I]**
* **Q5 — unchanged**: `0x5010` still has no visible writer; keep emitting 0.
  `0x30a8`'s producer is `0x62b38d0`'s `ctx+0x13c310` block and is emitted
  before `0x3410`; still safe as 0 for bring-up.
* **New `[U]` for the driver**: the two blob surfaces at `ctx+0x13c8a0`
  `+0x40` and `+0x30` are the CAVLC blob (`0x50c0`) and the "aux table" that
  goes to **both** `0x3098` (CMD block) and `0x5030` (ENT block). Which of the
  two holds the 0x440-byte blob cannot be separated statically — both are
  non-NULL surfaces in the H.264 decoder. Settle with one post-init dump of
  the two GART VAs: the CAVLC one starts `00 40 05 42 0a 44 04 22`. **[U]**

### 15.9 Corrections to this document

| Round-5 statement | Round-6 finding |
| --- | --- |
| "`0x5800,0x5840,…` = table VAs from `SPS/PPS_blob + 0x1200…`" (§7.1, §11) | they are **table payloads** packed 4 bytes per register, not addresses; `0x629ab2c` gathers bytes, it does not emit pointers |
| "the CAVLC/VLC table contents at `S+0x600…0x1300` and `S+0x1800`, generated by the first half of `0x62b6010` from `P[]`" (§14 Q3) | the `S` area is the **scaling matrices**, generated in `parseSPS`; `0x62b6010`'s first half builds the per-slice entropy blob |
| "ENT block emitted by `0x62b6010` (`0xfffffff0062b6890…0x62b6a2c`)" (§7, §11) | emitted by **`0x62b7ab0`**, once per picture, into its **own kind-4 / type-`0x88` packet**; `0x62b6010` emits no `0x5xxx` register |
| "ENT block, CAVLC/VLC table addresses `0x5800…0x5bfc`, ~507 words" (§11) | 256 registers = 512 words, **zero** of them emitted for a stream without scaling matrices, and they are not in the per-slice stream |
| "a first-cut driver can start with 0x81 alone and add 0x87 when the picture shows artefacts" (round-4 §3.3) | 0x81 alone carries no entropy configuration: an IDR needs the kind-4 `0x88` packet as well (§15.1, §15.6) |
| "the one thing missing for pixels is the command-stream generator … (d)" (§10 of the round-4 doc) | (d) is now zero host work: one 1088-byte `const` memcpy plus one register write |

## 16. Driver field map (round 7)

### 16.0 Read first: the named path is HEVC, not AVC **[V]**

**Do not use §1–§15's `ctx+0x13cxxx` recipe as an H.264 register program.**
The dump is authoritative over the earlier prose. This is a codec-identity
error, not merely an unassigned bitfield:

| Evidence in `build/video/d5500-decomp.txt` | Consequence |
| --- | --- |
| L7752–7780: the NAL dispatcher calls `0x62a5eb0` for type `0x20`, `0x62a7680` for `0x21`, `0x62aa954` for `0x22`; L7740 explicitly says `HEVC bad slice header`; L7389 calls `0x62960a0` | VPS/SPS/PPS = HEVC types 32/33/34. `0x62960a0` is the HEVC picture path. **[V]** |
| `0x62a7680`, L19371: `sps_max_sub_layers_minus1 > 6`; L19636–19677: per-sub-layer buffering/reorder/latency arrays | This is HEVC `parseSPS`; no H.264 `log2_max_frame_num_minus4` or `pic_order_cnt_type` can be assigned to its six block-size fields. **[V]** |
| `0x62b87dc`, L29311–29332: `CHEVCDecoder::AllocatePictureMemory` identifies temporal-MV, above-parameter, slice-parameter and fragment-pointer buffers | The `ctx+0x13c8a0` record belongs to HEVC. **[V]** |
| `0x62a15f0`, L14705–14741: uses `decoder+0x30f8` AVC parser, `decoder+0x3100` AVC state; error names `CH264Decoder::HOST_H264DecodeFrame` | This is the actual AVC host entry; its buffers differ from `0x62960a0`. **[V]** |
| `0x62c3d98`, L37484–37525: `decoder+0x3100`, `+0x6338`, 0x440-byte VLC constant | The H.264 allocation function is real, but was combined with a different codec's emitters. **[V]** |

The ordering supplied for `0x62960a0` is correct **for that function**:
L8014–8030 call `0x62b8cd8` then reserve 0x28 bytes and call `0x62b37d4`;
L8067–8069 finish all slice chunks before `0x62b7ab0`. This does **not** prove
H.264 uses those chunks. No faithful AVC IDR/P sequence follows by substituting
H.264 controls into this HEVC program. Every such substitution below is
explicitly **[U]** unless independently tied to the actual AVC path.

### 16.1 P1 — exact render/window program in `0x62b7ab0`

Notation: `C[o]` = ctx field, `R = *(ctx+0x13c898)` = current render-surface
record, `Q = *(ctx+0x13c8a0)` = current HEVC picture record. `F(r,v,f)` emits
`{0x000f0000 | ((f & 0xf0)<<16) | r, v}`; it never merges writes
(`0x629aa54`/`0x629aa5c`). All values below are u32. The V4L2 column describes
which Linux input could supply a buffer/format value; **it does not establish
AVC register compatibility**.

| Order after `0x5000` | Register/value | Flag/form | Linux input / status / dump evidence |
| --- | --- | --- | --- |
| 1, conditional | `0x5800..0x5bfc`: 256 packed scaling-list values as §15.5 | 0 / F | HEVC scaling-list enable `C[13c378]&1`; no direct AVC mapping. **[V]** L29038–29074 |
| preparation only | if `C[13d684]`: update `C[13c340]`, set `C[13c344]=12` (formula below) | no words | driver/hardware scheduling policy **[U]** for AVC; **[V]** L29076–29103 |
| 2 | `0x70a0 = C[13c358]` | 0 / F | packed render configuration, also kind-4 payload `+8`; layout below. **[V]** L29104 |
| 3 | `0x70a4 = C[13c288]` | 0 / F | HEVC tile columns/rows summary, not frame_num. **[V]** L29105, L27688 |
| 4 | `0x70b0 = C[13c28c]` | 0 / F | `(height-1)<<16 | (width-1)` of coded samples. **[V]** L27685–27700, L28841–28854, L29106 |
| 5 | `0x70b4 = C[13c290]` | 0 / F | `(align_up(height,8)-1)<<16 | (align_up(width,8)-1)`. **[V]** L27699–27709, L29107 |
| 6, conditional | `0x71ec = C[13cff8]` if `C[13d8a2]` | 0x10 / F | driver buffer, exact allocation link pending; **[V]** `0x62b859c` L29196–29200 |
| 7 | `0x701c = C[13d684]<<24 | C[13c340]<<12 | C[13c344]` | 0 / F | scheduling policy, not a slice control. **[V]** L29202–29215 |
| 8 | `0x7040 = R.u32[0x50] & ~63` | 0 / F | luma byte stride, `va.dst_stride` **[I]** as Linux substitution; raw expression **[V]** L29223–29228 |
| 9 | `0x71b4 = (C.u16[13c364]&0x1c0)<<6 | chroma_arg&3 | ((C.u16[13c364]>>3)&7)<<8`, plus bit 3 if `C[13d88c] && C[13d8a3]` | 0 / F | chroma format and bit depths; `sps.chroma_format_idc`, `bit_depth_{luma,chroma}_minus8` semantically match. **[V]** L29229–29234 |
| 10 | `0x703c = *(C.ptr[13c240]+0x20)` | 0 / F | value **computed** by `0x62b859c`/`0x62b8b0c`, below; not a buffer address. **[V]** L29118, L29240 |
| 11 | `0x71ac = stride | stride<<16`; stride = `R.u32[0x54]` for chroma!=0, else `[0x50]` | 0 / F | chroma stride (NV12 usually same as luma); **[V]** L29113–29119 |
| 12 | `0x700c`, `0x7010` = PAIRF(`R+8`) or PLANE(`R+8`,x,y,chroma) | 0x10 / F, two writes | current CAPTURE luma/chroma device VAs, formulas below. **[V]** L29120–29127 |
| 13, conditional | for k=0..14, `0x7100+8k`, `0x7104+8k` = PAIRF(reference record+8), only if `(C.u8[13c3e4+k]&127)!=127` **and** `Q.u32[0x7c]<2` | 0x10 / F, two writes per populated slot | `decode_params.dpb[index]` → driver-held CAPTURE VAs by timestamp; ordering is HEVC RPS order **[U]** for AVC. **[V]** L29130–29149 |
| 14 | `0x7018 = C[13cf48]` | 0x10 / F | driver buffer, allocation identity pending; **[V]** L29151 |
| 15 | zero memory from current pointer, then END2 | no terminator word | see tail below; **[V]** L29152–29155 |

`PAIRF(S)` (`0x629ad38`, L10994–11007) writes
`reg = S.u32[0x10]+S.u32[0x40]`,
`reg+4 = S.u32[0x10]+S.u32[0x44]`. With `S=R+8` this is
`R.u32[0x18]+R.u32[0x48]` and `R.u32[0x18]+R.u32[0x4c]`.
Thus R's useful surface layout is **[V]**: `+0x08` embedded memory descriptor,
`+0x18` common device base, `+0x48/+0x4c` plane offsets,
`+0x50/+0x54` plane byte strides. `S+0x10` and `R+0x10` must **not** be
confused; the former is the VA, the latter is not the VA used here.
A Linux driver may use separate `dst_luma`/`dst_chroma` VAs directly **[I]**.

`PLANE(S,x,y,chroma)` (`0x629ae20`, L11032–11057) instead writes
`Y=base+offY+x+strideY*y`,
`UV=base+offUV+(chroma==3 ? 2*x : x)+
(chroma==1 ? ((strideUV*y+1)>>1) : strideUV*y)`.
Offsets x/y come from `C[13dc7c]/C[13dc80]`; enabled by `C[13dc78]`.
For whole-frame NV12 capture use x=y=0; crop controls do not silently supply
this tile-offset path. `chroma_arg` to `0x62b859c` is zero if
`(C.u16[13c364]&7)==7`, otherwise `C.u16[13c364]&3` (L29108–29112).

`0x703c` policy (`0x62b8b0c`, L29348–29385), as called with argument 1:
if `C[13d88c]==0`, zero; otherwise
`8 | ((R.u32[0x50]>>6)<<22) | C[13d89c] |
((!C[13d88d] && !C[13d8a1]) ? 0x100 : 0x380) |
(C[13d8a0] ? 0x800 : 0)`, additionally `0x400` if `C[13d8a2]`.
These are surface-layout/platform settings **[U]** for the Linux policy;
they are **not** SPS/PPS flags. `0x71ec` follows the same extra-bit gate.

The `13d684` branch: B=`C[20]<10 ? 0xc00 : 0x1800`;
T=`chroma==2 ? B/2 : chroma==3 ? B/3 : (B*0x15556)>>17`;
T=`(T*15)>>5`; if `Q[84]>1`, T=`T/Q[84]`;
`C[13c340]=align_up(T,(chroma&2)?4:8)`, `C[13c344]=12`.
This is **[V]** arithmetic (L29076–29102); an AVC interpretation is **[U]**.

`0x70a0` pack (`0x62b38d0`, L27615–27684):
`(C[13c380].bit18<<30) | (type2<<28) | (C[13c354]<<27) |
0x001c0000 | ((C.u16[13c364]&0x1c0)<<2) |
((C.u16[13c364]&0x38)<<1) | (!!C[13db94]<<3) | chroma`.
`type2` needs its reaching definition (not an AVC slice-type assignment).
For 8-bit 4:2:0 the known fixed low part is `0x001c0001`;
unknown policy/type bits must remain **[U]**. `0x70a4` is
`C.u8[13c388]&31 | (C.u8[13c389]&31)<<8` (HEVC tile counts).
Examples of the verified dimensions: 64x64 → both `0x003f003f`;
176x144 → both `0x008f00af`. These are sample dimensions, not MB counts.

**Kind-4 tail and packet** (`0x62b7ab0`, L29152–29175):
`n=GETCOUNT(state)` (`0x629afb4`, L11086–11092),
`memset(GETPTR(state),0,64-8*(n&7))`; `END2` (`0x629af50`,
L11070–11082) only publishes `state.ptr` to `cmdbuf+0x88` and clears state.
**END2 emits no word and does not advance over the memset**. Therefore those
zeroes are not included in the chunk byte count. With only F writes,
`n` counts pairs (verify emitter below), not bytes. Do not append a second
`0x60000000` to this chunk.

The host-only separate packet payload is 30 u32, initially zero:
`[0x4c/4]=4`, `[0x08/4]=C[13c358]`, `[0x0c/4]=current_cmd_bytes-C[13c254]`,
`[0x10/4]=chunk_start+cmdbuf.u32[0x18]`,
`[0x24/4]=C[13d0a8]`, `[0x70/4]=0`; `+4=0`.
The final `C[13c254]` is the unpadded current command offset.
The per-slice chunk pad is different: 64-(absolute_wptr&63), including a
full 64 bytes when already aligned (L8040–8043); it is in `cmd` but excluded
from kind-1 payload byte count. Packet headers never enter `cmd`.

This P1 map was written first for the driver reader. P2–P5 below will retain
the codec distinction; no unverified HEVC→AVC field correspondence is implied.

### 16.2 P2 — entropy-side structure and the CMD writes (HEVC path)

`P[0x50]` is **HEVC slice_type: B=0, P=1, I=2**, not AVC's P=0/B=1/I=2.
The parse chain is `0x629537c` → `0x62abc88` (HEVC slice parser, L22422,
L22737–22747) → `0x62b2620` (L26205–26235). `P[56]/P[58]` are
`num_ref_idx_l0_active_minus1` / `num_ref_idx_l1_active_minus1`, **not MB
positions/counts**: the filler copies those parser fields and loops inclusively
(L26210–26229); `P[64]` is `pps.init_qp_minus26 + slice_qp_delta + 26`
(L26248–26256), **not a header length**. The P/B weighted-prediction gate is
L28706–28707. These observations plus the HEVC-only caller establish the
codec; see also §16.3's SPS map. All raw bit layouts below are **[V]** in
`0x62b6010`, L28093–28670. AVC semantic substitutes are **[U]**.

Let T=`P.u32[50]`, H=`T==0 ? 48 : T==1 ? 32 : 16`. The independent slice
structure starts at the current side-buffer write pointer and has H bytes:

| Word | Exact value (u32; OR of fields) | Meaning / source |
| --- | --- | --- |
| 0 | `T | P.u8[42]<<2 | P.u8[43]<<3 | P.u8[41]<<4 | P.u16[56]<<8 | P.u16[58]<<12 | P.u8[44]<<16 | P.u8[46]<<18 | P.u8[4a]<<19 | P.u16[5c]<<20` | slice type; HEVC SAO/temporal-MVP/CABAC/deblock flags and reference counts; **[U]** per-flag semantic labels until parser offsets are traced; L28105–28223 |
| 1 | `(P.u8[49]<<24) | (C[13c334]+P.s16[64]) | ((P.u16[66]&31)<<8) | ((P.u16[68]&31)<<16)` | bit-depth-adjusted SliceQpY; chroma QP offsets; extra flag at bit24; L28224–28258 |
| 2 | `C[13c31c]<<10 | C[13c318] | N<<20` | C[13c318/31c] are slice CTB coordinates (each ≤511), N = number of appended F register-write pairs; L28259–28279, L28769–28770. `N` remains readable after END2; END2 does not clear count. |
| 3 | `C.u8[13c320]<<16 | C.u8[13c321]<<12 | (P.u16[6c]&15)<<8 | (P.u16[6a]&15)<<4 | 8 | (2-T)` | QP/deblock control; low 2 bits invert slice rank; L28280–28321 |
| 4..7 (P/B) | each packs four byte-spaced nibbles from `P.u8[ea..f8]`: word4 ea/eb/ec/ed, word5 ee/ef/f0/f1, word6 f2/f3/f4/f5, word7 f6/f7/f8/**0** | mapped reference indices, not CAVLC tables. Four-bit checks and final byte left zero; L28322–28496 |
| 8..11 (B only) | word8 f9/fa/fb/fc, word9 fd/fe/ff/100, word10 101/102/103/104, word11 105/106/107/**0** | second HEVC reference-index list; L28496–28670 |

`P[ea..107]` are produced by `0x62b2344` (L26065–26160), which also creates
byte lists `P[108..125]` for locating the collocated picture; they are not
per-table CAVLC minima/maxima. The weighted-prediction arrays `P[130..]`
are luma/chroma weights and offsets: `0x62b2620`, L26263–26354, computes
chroma offset correction/clipping from denominators and bit depth. They are
not coefficient decode tables (§4's interpretation is wrong).

Following H bytes, append F pairs only when weighted prediction is enabled:

1. `F(0x7024, P.u32[12c] | P.u32[128]<<4, 0)`.
2. For L0 (and L1 only for T=0), j=0..`P.u16[56+2*list]` inclusive:
   `F(0x7200+0x100*list+4*j, (luma_delta_weight&511) |
   (cb_delta_weight&511)<<9 | (cr_delta_weight&511)<<18,0)`;
   `F(0x7280+0x100*list+4*j,(luma_offset&255) |
   (cb_offset&255)<<8 | (cr_offset&255)<<16,0)`.
3. If `C[13dbdd]`, append `F(0x7474,0,0)` **twice**.

This order and packing are **[V]** L28705–28753. List0 luma deltas/offsets
start at `P[130]/P[1a8]`, chroma deltas/offsets at `P[220]/P[310]`;
list1 starts at `P[16c]/P[1e4]`, `P[298]/P[388]` (L28721–28745).
A no-weight, no-quirk I header is 16 bytes then 48 zero bytes;
a no-weight P header is 32 bytes then 32 zero bytes. Both transfer **64
bytes**, not 16/32. Exact arithmetic:
`pad=(N*56-H)&56`, `unpadded=H+8*N`, `transfer=unpadded+pad`.
Side pointer advances over pad. `0x3094=((unpadded-1)<960) ?
((unpadded-1)>>6) : 15`; it excludes pad (L28754–28768).

**Addresses/reset**: L7885–7888 reset the selected entropy-side buffer and
`C[13d668]=0`; L7888 resets `C[13d680]=0` each picture. At the first
nondependent slice, `C[13d67c]=Q.ptr[60].u32[10]` (or 0), then increments
by **transfer bytes**, L28803–28821. This is the VA in the picture's 1 MiB
slice-parameter destination buffer, **not a zero-based hardware-window
offset**. Source is `C[13d668] + side.u32[18]` (L28815), where `C[13d668]`
is the previous slice's side-buffer used bytes (L8063–8064). Side buffer is
a separate ring resource; exact allocation/size belongs to the HEVC create
routine, not `DecodeCreateH264VLD`. It must hold all rounded-up per-slice
structures, with each transfer length a multiple of 64. Initial alignment
and allocation size are **[U]** here, not assumed from AVC's 64 KiB buffers.

**Exact CMD group order**, `0x62b5d44` L27977–28040. `B[...]` means one
BEGIN-REG/END-REG group, merging only consecutive +4 register writes into
`0x10000000 | count<<16 | reg`:

```
B[3098=Q.ptr[30].VA&~63, 30a0=C[13c30c], 30a8=C[13c310]]
B[30b0=C[13c314], 3410=C[13c2a4]]
if !C.u8[13c469]: B[3120=0,3124=0,3128=0,312c=0,3080=1]
B[3400=C[13c29c],3404=C[13c2a0],3408=C[13c298]]
B[3440+4*k=C[13c2ac+4*k] (k=0..11),
  3480+4*k=C[13c2dc+4*k] (k=0..11)]
B[3414=C[13c2a8]]
B[3100+4*i=C[13cf28]&~4095,
  3110+4*i=C[13cd58+0x78*i]&~4095 (i=0..3, interleaved)]
C.u8[13c469]=1
```

**Four**, not eight, iterations: `0x1e0 / 0x78 = 4` at L28035–28037.
No overlaps; no CAPTURE/DPB surface addresses in this loop. Gate is
`13c469`, **not** §6.3's `13dc69`.

`0x62b6010` then emits `B[3090=0,3094=0]` (one `0x10023090` header), saves
both value-word pointers, and patches them with destination VA / line count
before submission. For a dependent segment (`slice+0xdfe50 != 0`) it
copies prior `13d674/13d678` into these values and **emits no transfer**
(L28088–28091). Otherwise after side construction it appends
`{0xf0000000 | transfer, side.VA+slice_side_offset, destination.VA}`
(L28800–28821). `0x62b37d4` appends `0x60000000`. Do not move `3090/3094`
to the end (§7.4's suggestion changes hardware execution order).

### 16.3 H.264 (AVC) path — A0: submission identity and order **[V]**

**AVC does build a host-side MSVDX register program. It does not submit the
HEVC `0x30xx/0x34xx/0x50xx` program or IMG `h264fw_*_ps` structures.**
The helper name `h264fw_SetPpsAndSps` is a host parser/state adapter; it is
not evidence that firmware consumes those structures. The verified call chain
in this dump is:

```
62a15f0 HOST_H264DecodeFrame             L14705–14729
  629bc68 AVC NAL/header parser          L14711
  62a10ac DecodePicture                  L14528–14556
    62b1718 (slice 0 host state)         L25736–25776
      62b083c (SPS/PPS → host fields)    L25738
    62a0c00 DecodePictureInternal        L14434–14453
      62be538 (pack picture controls)
      62beb94 (first slice) → 62be348    L33617–33635
      62bec28 (remaining slices) → 62be348  L33654–33687
        62c4c98 DecodeExecuteH264VLD      L33213, L38794–39431
          62c5fb8 (slice register values)
          62c6564 (0x600c/0x6200 register group)
          62c66b0 (0x6010/0x62d8/.../0x7000 register blocks)
      62beb30 vaEndFrame → 62c4a00       L33593–33601
        [optional 62c44b4 deblock packet]
        62982e4 → shared command submit  L38153–38170
```

| Packet | Count per normal AVC picture | Command chunk | Evidence |
| --- | --- | --- | --- |
| kind 1 → mailbox `0x81` | **one per slice** | populated 0x28-byte `0x90000000` descriptor; `0xe` transfer of constant 0x440-byte VLC table; 26 fixed register writes; `0xb` triplet + `0xa` bitstream DMA; `0x1` run to `0x600c/0x6200..0x6218`; `0x5` register blocks from `0x62c66b0` including output/reference VAs; `0x60000000` | L39307–39369, L39392–39430; helpers L39657–39680, L39727–40269 |
| kind 3 → mailbox `0x87` | **zero or one**, after all slices | separate 0x90000000 descriptor, render registers and `{0x70000001, stored-parameter VA, output-Y VA, output-UV VA}`, then `0x60000000` | `0x62c4a00` gate `state[6324]!=0 && state[6a64]==0`, L38153–38161; `0x62c44b4`, L37767–37859 |
| kind 4 → mailbox `0x88` | **zero in this AVC path** | the HEVC picture-tail chunk does not occur | no kind-4 call in either AVC emitter or AVC end-frame; the kind-4 writer is the separate `0x62b7ab0` path (L29156–29175) |

The normal one-slice baseline IDR and following P picture therefore each
submit one kind-1 packet; an optional configured second deblock pass adds
kind 3. `state+0x6324` is selected in `0x62a15f0` from decoder mode
`decoder+0x7c` and parser state (L14714–14725), **not simply** from
`pps.deblocking_filter_control_present_flag`. Whether the Linux policy needs
that second pass for pixels remains **[U]**; the no-second-pass host path is
real and is the first baseline recipe to implement.

**Framing differs from HEVC**: the AVC emitter records its exact used bytes
and immediately starts the next chunk; L39396–39409 contain **no 64-byte pad**.
The AVC 0x90000000 descriptor is populated, not nine permanent zero words.
The AVC static CAVLC table travels via `{0xe0000440, vlc_va}` (L39319),
not `0x50c0`. No `0xf0000000` side-blob transfer appears in the AVC slice.
The 0x1 writes use the same counted-run emitter as HEVC; the remaining
register blocks use `{0x50000000 | count<<16 | reg, values[count]}` from
`0x629a568`/`0x629a6c0`/`0x629a81c`, preserving block boundaries.

The DEVA common dispatch still receives the command chunk location/count,
not SPS/PPS pointers: `deva.expanded.dis` `0x1000b08` → `0x10005e8` →
`0x1000d24` descriptor unpack; the codec callback path follows
`[A1GbP+0x10]→[+0x24]→[+0x10]` (see §9). Host evidence above already
settles A0 independently of whether the firmware or the hardware executes
individual register-block words. **[U]**: a complete firmware-side interpreter
trace; absence of container magics alone does not prove it never interprets
command words (contrary to §0.7/§9).

For A1/A2 use `D=*(decoder+0x3100)`, `PIC=*(D+0x63e8)`,
`SL=*(D+0x63f0)`, `RT=*(D+0x6528)`, `ST=*(D+0x6530)`. These are AVC state
and packed control records. The offsets `ctx+0x13cxxx` in §16.1–16.2 must
not be copied into this map.

### 16.4 AVC A1 — baseline frame field map and exact emission order

Cross-check source: `third_party/psb_video/src/pnw_H264.c` (MIT),
`psb__H264_build_register`, `psb__H264_build_rendec_params`,
`psb__H264_preprocess_slice`, and `src/hwdefs/{dxva_fw_ctrl.h,
msvdx_vec_h264_reg_io2.h,msvdx_cmds_io2.h}`. In this AVC path, the matching
register fields pin the bank translations: Apple `0x6000+off` ↔ MSVDX VEC
`0x0800+off`, Apple `0x7000+off` ↔ MSVDX CMDS `0x1000+off`. **[I]** for
physical-bank equivalence; the **Apple emitted addresses and bitfields are
[V]** in the dump. Do not apply that translation to HEVC's other banks.

Definitions for the supported baseline recipe: W=`sps.pic_width_in_mbs_minus1+1`,
H=`sps.pic_height_in_map_units_minus1+1`, frame_mbs_only=1, 8-bit 4:2:0,
no scaling-list overrides, no weighted prediction, no B slices, no FMO,
no alternate output or second deblock pass. Slice code is I=0/P=1/B=2,
confirmed by the three bytes at `0xfffffff007a6e7b4` = `01 02 00` for AVC
slice types P/B/I (L39539–39546). All masks below act before shifts.

**Apple forces its internal profile selector to 3 even for baseline input**:
`0x62be538` L33382 sets PIC.flags bit11; `0x62c4c98` L38859 sets D[64f8]
to 3, and `0x62c6564` L39664–39668 translates that to FE control 0x11.
This also enables the flat scaling-list block at register `0xe000` on
baseline streams (L39756–39766). Omitting it because §15 said baseline
has no scaling block would not reproduce AVC's kext path.

| Logical word / D offset | Exact bit layout for AVC | V4L2 input / dump evidence |
| --- | --- | --- |
| SPS0 / `650c` → FE `6200`, BE `62d8` | `((chroma>>1)&1)<<28 | depthC<<25 | depthY<<22 | default_matrix<<18 | two_pass<<16 | 1<<14 | 3<<12 | min_bipred8<<11 | direct8<<10 | frame_only<<8 | (chroma&1)<<9 | (W-1)` | SPS chroma/bit depths/flags; min_bipred8=`W>80 && level_idc>30`; default_matrix=0, two_pass=0 for Apple's forced profile. **[V]** L38955–39076; original PIC flag producers L33279–33385. Baseline 8-bit 420: `0x7300 | direct8<<10 | (W-1)`. |
| PPS0 / `6508` → FE `6204`, BE `62dc` | `(second_chroma_qp_index_offset&31)<<24 | (chroma_qp_index_offset&31)<<16 | weighted_bipred_idc<<8 | num_slice_groups_minus1<<4 | entropy<<2 | constrained_intra<<1 | transform8x8` | PPS fields/flags; FMO zero. **[V]** L39078–39129, L33367–33376 |
| PIC0 / `6504` → FE `6208`, BE `62e0` | `(W*H-1)<<16 | (H-1)<<8 | reference<<3` for progressive frame | `decode_params.nal_ref_idc!=0`; general version adds MBAFF<<2, field_pic<<1, bottom_field at bit0 and uses field height. **[V]** L39130–39175; PIC producers L33271–33300 |
| SLICE0 / `64fc` → FE `620c`, BE `62e4` | `direct_spatial<<31 | disable_deblocking_filter_idc<<28 | (slice_alpha_c0_offset_div2&15)<<24 | (slice_beta_offset_div2&15)<<20 | 2<<18 | slice_code<<16 | cabac_init_idc<<14 | slice_index` | SLICE_PARAMS direct flag, deblock offsets, type/cabac fields; progressive field_type=2. **[V]** L39497–39569; SL producers L33167, L33204–33206 |
| SLICE1 / `6500` → FE `6210`, BE `62e8` | `first_mb_y<<24 | first_mb_x<<16 | (26+pps.pic_init_qp_minus26+slice_qp_delta)<<10 | l1_minus1<<5 | l0_minus1` | `first_mb_x=first_mb_in_slice%W`, y=`/W`; I slices force both ref counts to zero; otherwise SLICE_PARAMS counts. **[V]** L39484–39498, L39570–39636 |
| FE `6218` | `first_mb_in_slice<<16` | SLICE_PARAMS.first_mb_in_slice. **[V]** L39674 |
| BE `62ec` | `va.colocated` | per-CAPTURE stored-MB/temporal-MV parameter buffer, **not luma**. **[V]** L39747–39753 |
| BE `62f0` | 16-bit long-term reference mask by hardware DPB slot | DECODE_PARAMS.dpb[].flags LONG_TERM. **[V]** L38940–38953, L39754 |
| BE `633c+4*k` | four packed bytes of L0 reference indices per word (`0xff` unused; bottom-field flag bit7) | SLICE_PARAMS.ref_pic_list0 entries mapped to picture DPB slots, k through `floor(l0_minus1/4)`; emitted for P/B only. **[V]** L39888–39892, L33175–33193 |
| CMDS `7100+8*k` / `7104+8*k` | reference Y / UV device VAs | count = occupied DPB high-water mark D[64f4]; only references marked used by L0/L1 receive real VAs; Apple emits `0xdeadbeef` pairs for unused slots. **[V]** L39894–39945, L40077–40084. Header carries flag `0x08000000`, i.e. `0x58000000 | count_words<<16 | 0x7100`. |
| CMDS `7000`,`7004` | `((16*H-1)<<12) | (16*W-1)` | coded sample dimensions. **[V]** L40135–40161. **12-bit height shift**, not 16 as HEVC's 70b0. |
| CMDS `7008` | `reference_special<<30 | 2<<20 | async<<13 | (chroma!=0)<<12 | 0x10010000` | profile=2 after forced-profile conversion; async=`second_pass||two_pass` (baseline zero); special from PIC.flags bit15 (baseline zero). **[V]** L40162–40187. Baseline = `0x10211000`. |
| CMDS `700c`,`7010` | `va.dst_luma`, `va.dst_chroma` | CAPTURE RT: `RT[18]+RT[48]`, `RT[18]+RT[4c]`. **[V]** L40188–40196. If alternate output is configured, these are internal reconstruction VAs instead. |
| CMDS `7018` | `D[6128] = va.intra` | IntraBuffer device VA. **[V]** L40198–40200, L37495–37505 |
| CMDS `7024` | `chroma_log2_weight_denom<<4 | luma_log2_weight_denom` | Apple uses 5/5 when prediction is unweighted (L33200, L39473–39476), so baseline = `0x55`. **[V]** L40201–40215 |
| CMDS `701c` | `0x013c000c` | constant cache policy; not the HEVC dynamic policy. **[V]** `0x62c4788`, L37929–37933 |
| CMDS `7040` | `va.dst_stride & ~63` | extended luma byte stride. **[V]** L37934–37949 |
| CMDS `71b4` | `sps.chroma_format_idc` | format for ordinary CAPTURE layout. **[V]** L37950–37954 |

`frame_num`, IDR picture ID, POC type and SPS frame/POC syntax bit widths
are **not** packed into these baseline words: host parsing already resolved
the slice and DPB. POC values are transmitted in BE `6250...` only for
B slices (L39776–39797). SPS max_num_ref_frames constrains the driver DPB;
it is **not** the command's active-DPB count for an empty IDR.

**The ten-word CTRL_ALLOC_HEADER is populated as follows** (L39307–39320,
L39425–39428, L40235–40263, L37893–37920):

| Offset | Value, normal frame path | Meaning / source |
| --- | --- | --- |
| `00` | `0x90000000` | CTRL_ALLOC_HEADER |
| `04` | `(constrained_intra<<23) | (weighted_pred | weighted_bipred_idc<<1)<<17 | deblock_idc<<12 | (alpha&15)<<8 | (beta&15)<<4 | 2<<2 | slice_code` | SliceParams from `0x62c66b0`; baseline I=8, P=9 |
| `08` | **0** in final stream | initially D[7cd4]=0x10000, then explicitly zeroed at L39428; no external-state VA in the baseline header |
| `0c` | `dst_chroma_stride | dst_chroma_stride<<16` | two render strides, unlike the older psb header's MacroblockParamAddr |
| `10` | `first_mb_x | first_mb_y<<16` | slice start MB position |
| `14` | `(W-1) | (H-1)<<16` | picture last MB position; D5500 expanded this older packed field to its own word |
| `18`,`1c` | 0,0 | alternate-output Y/UV VAs if configured, otherwise zero |
| `20` | 0 | alternate-output rotation/scale flags if configured, otherwise zero |
| `24` | `bit_depth_chroma_minus8<<12 | bit_depth_luma_minus8<<8 | !!D[6328]<<3 | chroma` | normal NV12 =1; driver policy D[6328] is **[U]** unless that alternate mode is requested |

**Byte-exact normal baseline ordering**, one slice:

```
CTRL_ALLOC_HEADER (10 words, above)
{e0000440, va.cavlc}
R1[60ec..6144 = first 23 fixed values; 61c0..61c8 = last 3]  (two runs)
{b0000001, header_bits&31, size-4*(header_bits//32)}
{a0000000 | (size-4*(header_bits//32)), src+offset+4*(header_bits//32)}
R1[600c=11; 6200=SPS0,6204=PPS0,6208=PIC0,620c=SLICE0,6210=SLICE1;6218=first_mb<<16]
R5[6010=11]
R5[62d8=SPS0,PPS0,PIC0,SLICE0,SLICE1,va.colocated,longterm_mask]
R5[e000 = 56 words of 0x10101010]   (flat baseline; Apple's forced profile)
if P: R5[633c = packed L0 bytes]
if dpb_highwater>0: R5_flag08000000[7100 = 2*dpb_highwater reference VA words]
R5[7000 = dims,dims,10211000,dst_luma,dst_chroma]
R5[7018 = va.intra]
R5[7024 = 55]
R5[701c = 013c000c]
R5[7040 = dst_stride&~63]
R5[71b4 = 1]
60000000
```

Here `R1` merges consecutive +4 registers as in `0x629b068`;
`R5[reg=values]` is one `0x50000000 | count<<16 | reg` block.
The 56 flat words are two 8x8 lists (16 words each) plus six 4x4 lists
(4 each), in order offsets `a0,60,00,30,40,10,50,20` of D[63f8]
(L39756–39766, L33394–33440). All default bytes used here are 0x10,
verified at `0xfffffff0055964e0` (16-byte 4x4 constant) and
`0xfffffff0055964f0` (64-byte 8x8 constant). These are actual AVC matrices,
not §15's HEVC normalised defaults.

The 26 fixed `(reg,value)` pairs are at `0xfffffff005597148`, 208 bytes,
read back with Ghidra; ordered values (hex) for consecutive `60ec..6144`:
`26000,738a0,828f4,a312d,b5959,c517b,d1196,db1ad,e21be,e59c8,e79cd,
eb1d3,ed1d8,f09dd,f71e7,1041f6,1256a4dd,01489292,11248050,22a2,
2a02,8108282a,12`; then `61c0=10920e,61c4=10d216,61c8=10f21c`.
**[V]** call/loop L39325–39333. These are VLC table addresses/initial
widths/opcodes; §15.2's claim that Apple never writes that register family
is false for AVC.

### 16.5 AVC A2 — buffers, allocation units, and packet payload

**The old size formulas used a false variable name.** `decoder+0x28` is not
picture width: `0x62bd7fc` L32764–32770 initializes it to 1 or to
`{2,2,1,2}[decoder_create_arg5-9]`. Those four u32 constants are at
`0xfffffff0055964d0` (read back with Ghidra). Call this multiplier M.
`decoder+0x24` is create argument 6, call it K; it is copied to
`D+0x7cd1` (L32784), not established as picture height. Thus interpreting
`width<<19` as a per-picture VLC-table allocation would vastly overallocate.
M's platform meaning and K's meaning are **[U]**, their numeric producers
are **[V]**. Actual picture sample dimensions later reside at D[64b0/64c8].

| AVC resource | Allocation/requested size | Referenced by baseline submission | Evidence |
| --- | --- | --- | --- |
| IntraBuffer, resource `D+6118`, VA `D+6128` | `M<<16` (64/128 KiB) | CMDS `7018`; `va.intra` | L37495–37499; L40198–40200 **[V]** |
| AuxLineBuffer, resource `D+61c8`, VA `D+61d8` | `M<<19` (512 KiB/1 MiB) | CMDS `71ec` only when alternate/second-pass path needs it; not ordinary baseline | L37500–37504, L37599; L37921–37926 **[V]** |
| StreamStateBuffer, resource `D+6278`, VA `D+6288` | `M<<12` (4/8 KiB) | kind-1 payload `+24`, then mailbox w5; `va.stream_state` | L37505–37509; L39410 **[V]** |
| PreloadBuffer, resource `D+6a70` | `K*0x288` | no direct reference in `62c4c98/62c6564/62c66b0` normal baseline sequence; do not invent a register for it | L37510–37514 and L37587 **[V]** allocation; hardware/firmware use **[U]** |
| VLC packed table, resource `D+6338`, VA `D+6348` | 0x1000; memcpy 0x440-byte constant `fffffff005596d08` | `{e0000440, va.cavlc}` every slice | L37515–37525, L39319 **[V]** |
| render/control allocations | eight ×0x40000, resources `D+6548+i*0xa0`, buffer objects `D+6540+i*0xa0` | **these hold the actual register command bytes**, chosen at L39280–39287 | L37529–37542 **[V]** |
| command-list allocations | eight ×0x10000, resources `D+490+i*0xb58`, list objects `D+428+i*0xb58` | **host-only packet containers**, distinct from register command bytes | L37545–37559, L39373–39392; `62982f8` list base at +8 **[V]** |
| per-CAPTURE stored-MB / collocated buffer | `(W*H*0x80+0x41ff)&~0xfff` = page-round-up(`W*H*128+0x3200`) | BE `62ec=va.colocated` (current); B temporal predictor also reads saved references | L39255–39262, `62c3b8c` L37412–37432, L39747–39753 **[V]** |
| optional reconstruction surface | luma plane page-aligned before chroma; NV12, stride from CAPTURE or align_up(width,64) | replaces ordinary reconstructed output addresses only under D[7ab0] | `62c3a50` L37326–37364; L40188–40196 **[V]** |

For 64x64, colocated size = `0x4000`; for 176x144, `0x7000`.
Keep the per-CAPTURE collocated allocation with the DPB buffer lifetime.
No HEVC `entropy_side`, fragment-pointer, above-parameter or slice-parameter
buffer belongs in this AVC recipe.

Each host packet is a zero-initialized 30-u32 payload. For kind 1, exact
nonzero/conditional fields (`62c4c98`, L39392–39429):

```
+04 = (slice_index==0 ? 1 : 0) | (two_pass ? 8 : 0)
      | (first_picture && slice_index==0 ? 0x10 : 0)
+08 = operating_mode                         // 7008, baseline 10211000
+0c = slice_chunk_bytes                       // excludes source bitstream
+10 = chunk_start + command_buffer_device_VA   // buffer object +18
+24 = stream_state_device_VA
+4c = 1
+50 = !reference_picture
```

All other words, including +70, remain zero. **[V]** First-picture bit is
stored once in D[7cd2]; no first-picture bit belongs on the later slices.
The kind-1 chunks concatenate without padding. Unlike HEVC's stream, no
picture-wide kind-4 chunk follows them. `va.cmd` (new generator VA key) supplies
the command-buffer object +18 so packet +10 is a complete device address,
not just a byte offset within the buffer.

`SubmitCommandBufferInternal` (`0x628a8d4`, protocol §12 corrections) produces
mailbox 0x81 with `w1=(chunk_bytes/4)<<16 | payload[+04] |
(last_packet ? 0x4004 : 0x2000)`,
`w2=payload[+10]`, `w3=MMU-directory-context word`,
`w4=payload[+08]` (operating mode), `w5=payload[+24]` (stream-state VA).
Thus **the command-buffer VA is w2**, not w3; the packet payload and its
0x80000007/0xabcdefab envelope stay host-side.

### 16.6 AVC A3/A4 — bitstream and entropy modes

**Headers are parsed on the host; macroblock entropy data stays in OUTPUT.**
`629bc68` calls the AVC slice parser `62a0058` (L11875–11883), stores a bit
count after adding back the bytes removed for emulation prevention
(L11915–11933), and `62be348` copies it to SL.u16[14] (L33209–33212).
Let B be that EBSP-relative bit count, including any NAL header bytes before
slice_data; L39358–39363 emits:

```
skip_bytes = (B & 0xffe0) >> 3    // same as 4*(B//32), B is u16 here
SR_SETUP  = {b0000001, B&31, NAL_bytes-skip_bytes}
BITSTREAM = {a0000000 | (NAL_bytes-skip_bytes),
             OUTPUT_device_VA + NAL_offset + skip_bytes}
```

`0x62c5b18`–`0x62c5b4c` disassembly confirms the length argument, omitted
from the decompiled `29a444` call. `CMD_ENABLE_RBDU_EXTRACTION=1` in
`psb_video/src/hwdefs/dxva_fw_ctrl.h` names the set flag; VERIFY_STARTCODE=4
is clear. **[V]** Byte transport is from the original mapped input, with
emulation-prevention bytes still present; the stream reader extracts RBSP.
The host count compensates for skipped EPBs; it does not DMA a rewritten
RBSP buffer. NAL start-code/length-prefix bytes are outside the selected
NAL (L11852–11857, L11931–11933). The API may accept Annex-B outside this
path, but this AVC parser also handles length-prefixed NALs; §10's assertion
that it requires Annex-B is not established.

For the generator, `slices[].offset` points to the **NAL header byte**, after
any Annex-B prefix, and `size` counts from that header through the NAL end.
`params.header_bit_size` must be measured from that same byte in **EBSP**
bits. If userspace reports RBSP-only header size, add the NAL header bits and
8 per removed EPB before slice_data. The Linux control documents it as the
offset to slice_data from the beginning of the slice, so verify the client
parser convention once; do not silently mix conventions.
[UAPI reference](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html).
For CABAC, include the cabac_alignment_one_bit padding up to slice_data.

**CABAC changes the entropy flag, QP and cabac_init_idc, not this packet
architecture.** PIC.u8[d9] (PPS entropy_coding_mode_flag) becomes PPS0 bit2
(L33371, L39083–39092); SL.u8[362] becomes SLICE0 bits14..15
(L33204, L39551–39558). QP is SLICE1 bits10..15. There is no
CAVLC-vs-CABAC branch around the 0x440 VLC DMA or its 26 registers: both are
emitted unconditionally at L39319–39333. No separate uploaded CABAC-context
blob is visible in this AVC host path. **[V]** Context initialization by the
silicon/firmware is **[I]** from those control inputs; not independently
traced inside DEVA. CAVLC I/P is the verified initial generator scope;
CABAC output is marked unvalidated, and weighted/B/interlaced/scaling-override
branches need their own validation before enabling in the kernel.

### 16.7 AVC reference generator — input contract and validation

`python3 tools/ds-video/cmdstream.py --gen IN.json` is an independent,
stdlib-only implementation of §16.4, using V4L2 field names directly.
It does **not** read the dump, driver tree, Ghidra, or a device at generation
time. `--text` adds byte offsets and per-word register/opcode meanings.
`--emit` now recognizes multiline emitter calls, including `29a760`,
`29aa54`, and the actual AVC helpers. Existing `--slots` remains available.

Input, one picture per file:

```json
{
  "sps": {"profile_idc": 66, "level_idc": 10, "chroma_format_idc": 1,
          "bit_depth_luma_minus8": 0, "bit_depth_chroma_minus8": 0,
          "pic_width_in_mbs_minus1": 3,
          "pic_height_in_map_units_minus1": 3, "flags": 80},
  "pps": {"pic_init_qp_minus26": 0, "chroma_qp_index_offset": 0,
          "second_chroma_qp_index_offset": 0, "flags": 8},
  "decode_params": {"nal_ref_idc": 3, "flags": 1,
                    "dpb": ["16 objects using the UAPI dpb entry fields"]},
  "slices": [{"params": {"header_bit_size": 32, "first_mb_in_slice": 0,
                         "slice_type": 2, "slice_qp_delta": 0,
                         "ref_pic_list0": [], "ref_pic_list1": []},
              "offset": 0, "size": 100}],
  "first_picture": true,
  "va": {"cmd": "0x10000000", "src": "0x11000000",
         "dst_luma": "0x12000000", "dst_chroma": "0x12001000",
         "dst_stride": 64, "dpb": ["16 plane-address objects or null"],
         "cavlc": "0x13100000", "intra": "0x13000000",
         "stream_state": "0x13200000", "colocated": "0x13300000"}
}
```

The two string array entries above describe the schema, **not runnable
examples**; the complete runnable inputs are in `build/vdec-r5-ref/`.
Each `decode_params.dpb[i]` has `reference_ts,pic_num,frame_num,fields,
top_field_order_cnt,bottom_field_order_cnt,flags`. `va.dpb[i]` is null or
`{"luma":VA,"chroma":VA}` for that same index. `ref_pic_list0/1` entries
are `{"fields":3,"index":i}` for progressive frame references.
V4L2 flag masks are the numeric UAPI flags, not separate invented bool keys.
Active DPB entries use flag 2; long-term adds flag 4. Hardware slot i is chosen
as V4L2 DPB index i in this implementation (**[I]** driver policy, valid because
all corresponding lists/masks/VA pairs use the same index). Apple uses its own
host DPB ordering; no claim is made that V4L2's order matches iOS's byte for byte.

`va.dst_chroma_stride` optionally overrides the NV12 default of dst_stride.
Strides must be multiples of 64 and cover coded width; do not use width=176
as a 176-byte stride (choose 192). Missing required addresses emit zero and
appear in `unresolved`; unsupported weighted/B/FMO/interlaced/scaling-override
controls also populate that key. CABAC sets its real scalar fields and records
that execution remains unvalidated. `complete=true` means all fields required
by the supported recipe were supplied; **it does not mean hardware decoding
has been tested**. Omitted optional zero-valued syntax fields are treated as
zero-initialized controls. `frame_num`, POC syntax and other fields not consumed
by the I/P recipe may be retained in inputs without producing guessed words.

Output:
`{"cmd":[u32,...], "packets":[{"kind":1,"payload":[30 u32]}],
"unresolved":[{"field":"...","reason":"..."}], "complete":bool,
"recipe":"..."}`. Payloads describe the host writer's values **before**
SubmitCommandBufferInternal ORs its last/intermediate-packet flags into +04.
There are no packet-envelope words or artificial alignment pads in `cmd`.
No HEVC buffers, side blob or kind-4 packet are generated.

Examples use actual two-frame x264 baseline/CAVLC Annex-B bitstreams with
`ref=1,bframes=0,qp=20,keyint=30,scenecut=0,threads=1`; ffmpeg's
`trace_headers` independently supplies their SPS/PPS/slice fields and raw
header-bit positions. VA values are explicit **example addresses**, not
live-device addresses. Files retained in `build/vdec-r5-ref/`:

| Input | Slice NAL offset / bytes | Header bits / QP | Output |
| --- | --- | --- | --- |
| `64x64-idr.json` | `0x25b / 2113` | `32 / 17` | `64x64-idr.cmd.json`, 139 words = 556 bytes |
| `64x64-p.json` | `0xaa0 / 1511` | `26 / 20` | `64x64-p.cmd.json`, 144 words = 576 bytes |
| `176x144-idr.json` | `0x25c / 4974` | `32 / 17` | `176x144-idr.cmd.json`, 139 words = 556 bytes |
| `176x144-p.json` | `0x15ce / 3531` | `26 / 20` | `176x144-p.cmd.json`, 144 words = 576 bytes |

Common controls: profile 66, level 10, POC type 2, max_num_ref_frames=1,
log2_max_frame_num_minus4=0, SPS flags=0x50, PPS flags=8,
pps.pic_init_qp_minus26=-6, both chroma offsets=-2. For each dimension the
P picture references the preceding IDR in DPB slot0. The IDR's QP17 follows
x264's I-frame QP adjustment; it is not assumed to be 20 from the encoder's
nominal qp option.

Validation performed on the host: all four streams parse as valid command
blocks with exact packet byte counts; current/reference VA locations and
SPS bit packing checked against psb register masks; 26/26 VLC pairs match
the Ghidra-extracted constant; missing VA produces zero plus an unresolved
entry; CABAC fields set correct bits but remain marked unvalidated;
two-slice job concatenates at the previous chunk end (even when unaligned)
and sets first-picture only once. Scratch reproducer:
`build/vdec-r7-research/validate_reference.py`.

### 16.8 Literal AVC IDR and P command/packet words

These are the complete 176x144 examples above, four u32 per row; the left
column is a byte offset. No elisions or omitted padding. Only one kind-1
packet per picture; **there is no 0x88 chunk in AVC**. Scalar and address
sources are §16.4–16.7. The 64x64 examples use the same word positions; their
full vectors are retained alongside the inputs.

**176x144-idr: 139 words / 556 bytes**

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Kind-1 payload (30 u32):

```text
00: 00000000 00000011 10211000 0000022c
10: 10000000 00000000 00000000 00000000
20: 00000000 13200000 00000000 00000000
30: 00000000 00000000 00000000 00000000
40: 00000000 00000000 00000000 00000001
50: 00000000 00000000 00000000 00000000
60: 00000000 00000000 00000000 00000000
70: 00000000 00000000
```

**176x144-p: 144 words / 576 bytes**

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Kind-1 payload (30 u32):

```text
00: 00000000 00000001 10211000 00000240
10: 10000000 00000000 00000000 00000000
20: 00000000 13200000 00000000 00000000
30: 00000000 00000000 00000000 00000000
40: 00000000 00000000 00000000 00000001
50: 00000000 00000000 00000000 00000000
60: 00000000 00000000 00000000 00000000
70: 00000000 00000000
```

### 16.9 Corrections, remaining uncertainty, and cheapest validation

These supersede earlier prose; the dump wins when numbers differ.

| Location | Wrong / obsolete statement | Verified correction |
| --- | --- | --- |
| §1, §10; protocol doc's address index | `62960a0` = HOST_H264DecodeFrame; `62b37d4` = H.264 generator | These are HEVC. Actual AVC host entry `62a15f0`, execute `62c4c98`, FE `62c6564`, RENDEC `62c66b0`; §16.0/16.3 evidence. |
| §3.1, §13, §14 Q2 | 0x90000000 descriptor stays zero; nothing in kext fills it | AVC fills all documented fields in §16.4. Even HEVC writes through `ctx+13c240`: `62b55f0` L27913–27914 and `62b859c` L29197, L29240–29241. The pointer's writes were missed. |
| §6.3 | eight overlapping runs are DPB surfaces | HEVC loops are four iterations, no overlap (L28035–28037/L28924–28926), backed by four Genc resources (L25850–25863). Actual AVC references are in flagged `R5[7100]` (§16.4). |
| §6.2 / §7 / §15.1 | `13c2ac/13c2dc` are L0/L1 POC arrays | HEVC tile-column/row boundaries: PPS tile enable/counts/spacing copied at L26493–26537, cumulative boundary formation at L27152–27312. Actual AVC L0 bytes are `R5[633c]`; B POCs are `R5[6250]`. |
| §6.1 / §14 Q5 / §15.8 | 5010 and 30a8 have no producer / safe zero | HEVC `*puVar1` at L27330/L27339 writes 13c26c: aligned sample dimensions; 13c310 at L27314–27328 is unaligned sample dimensions. Ghidra `62b48b8`/`62b4918` store through x26=ctx+13c26c. Neither belongs in AVC. |
| §7.3 | sign bits mean negative POC delta, clamp ±127 | HEVC byte bit7 is long-term state (`picture+14==2`, L26619–26621), not delta sign; low saturation is **-128** (L28964–28965). High 5438 term is `2-min_slice_type`, not previous picture index (L28771–28798). |
| §7.4 | transfer destination is a window offset; 3090/3094 may move to end | HEVC destination starts at the current picture's slice-parameter buffer VA, L28803–28818; retain original execution order and patch values in place. No such side transfer in AVC. |
| §7 / §11 | ENT writes are per slice in `62b6010` | §15 correctly moved them to HEVC kind4/88. **AVC has neither that ENT block nor a kind4 packet**; its FE/RENDEC words are §16.4. |
| §11 | exact baseline IDR stream | Not an AVC stream: wrong codec registers, missing VLC DMA/index words, empty header, wrong buffers and scaling path. Replace it with §16.8's literal AVC vectors. |
| §12 pseudocode | inline packet envelope; MMU w3 derived from cmd VA; logical offset in w1; padding; 8-entry runs; guessed slice fields | Packets stay host-side; command VA is mailbox w2; w1 high half is command word count; w3 is MMU directory context. AVC has no inter-chunk pad. This pseudocode is obsolete in full for AVC (§16.3–16.6). |
| §13 mailbox correction | Apple leaves command word count zero | False: `628a8d4` L1260, L1280–1281 uses payload+0c >>2 for AVC; final/intermediate flags are 4004/2000 (L1276–1279). No firmware fallback to a host-only packet header is possible. |
| §15.2 | Apple only writes 50c0; never programs VLC address/width/opcode registers | That emitter is HEVC. AVC unconditionally DMAs 0x440 bytes and writes **26** index/config values, L39319–39333. Compared with psb's 22 pairs: **3 changed existing values** (6128,6138,6140), **4 added pairs** (6144,61c0,61c4,61c8). All 26 are emitted even for 4:2:0, not conditional on 4:2:2. |
| §15.3–15.8 | scaling area is a shared AVC parser object; 5410/5414 are scaling summaries; +40 holds CAVLC | The area is HEVC parseSPS output. 5410/5414 pack HEVC chroma-QP-offset lists (L26754–26780; PPS range-extension labels L41770–41787), not scaling summaries. HEVC Q+40 is the above-parameter buffer (L29284–29294/L25962), not AVC's constant VLC buffer. AVC matrices go to R5[e000], including flat defaults on baseline; its VLC buffer is D+6338. |
| protocol §7, §15 / original allocation prose | width<<16/19/12 and height*288 | M<<16/19/12, where M is 1 or 2 from a constructor table; K*288 with K an unclassified create argument. Command bytes occupy 8×0x40000, host packet lists 8×0x10000. §16.5 pins these producers. |

The original first-round HEVC paragraphs §16.1–16.2 are retained as requested.
Two clarifications to their raw interpretation: the `type2` in 70a0 is
`C.u8[13c36a]+C.u8[13c36b]` (Ghidra `62b422c`/`62b4230`, then
`62b5280`/`62b5284`), **not a slice type**; `P[4c]` is a CTB address whose
13c318/31c coordinates are converted to 16-sample MB units by CTBSize/16
(L27730–27741). These do not change the AVC recipe.

Ranked remaining checks, to be run by the device orchestrator only:

1. **[V] host recipe; [U] actual decoding success.** Cheapest decisive test:
   submit the single 64x64 IDR vector with real VAs and the 0x440 constant,
   one 81 packet, no 88. Log the 139 command words before submission, require
   a completion with no firmware error, and compare active NV12 pixels to
   the software golden. This validates descriptor strides, profile setting,
   opcode parsing, buffers and all baseline register fields together.
2. **[I] same-bank MSVDX register naming / hardware slot policy.** Names match
   psb masks and call order; no speculative MMIO writes are necessary. After
   IDR succeeds, submit the 144-word P vector with its one slot0 reference.
   Correct P pixels validate the L0 index and VA list convention. Then test
   a reference in sparse slot3 to validate direct V4L2-index slot assignment.
3. **[U] second deblock pass / alternate output.** Ordinary path exists in
   Apple's code. Start with it; compare first IDR/P pixels including MB
   edges. Only if completion succeeds but edge pixels differ, try the same
   stream with the actual kind3/87 second pass. No evidence requires 87 just
   because PPS advertises deblocking syntax.
4. **[I] CABAC initialization; [U] execution validation.** Same tiny stream,
   re-encoded with CABAC; set PPS0 bit2 and real cabac_init_idc/QP, supply the
   CABAC-aligned header bit count, keep VLC DMA/index setup. Compare pixels
   and first error/completion. No uploadable CABAC context blob was found.
5. **[U] preload K, weighted/B/interlaced/custom scaling.** K can be settled
   statically by tracing decoder constructor argument 6; no device experiment
   is needed for that. Unsupported codec features require separate vectors,
   starting with a two-slice progressive picture before adding B/weights.

Software goldens retained: `64x64.nv12`, `176x144.nv12`,
`software-goldens.json`. CRC32 over **active Y bytes then active UV bytes**,
excluding row padding: 64x64 IDR `ea8500f0`, P `8fabcea1`;
176x144 IDR `3c0d365e`, P `a44fbde0`. The 176x144 software file has stride176;
read only 176 bytes from each hardware row of the example's stride192.
These are ffmpeg CPU decode results, not measurements from the iPad.
