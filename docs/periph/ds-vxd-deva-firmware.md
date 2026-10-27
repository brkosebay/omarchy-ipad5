# VXD (MSVDX) DEVA firmware — does it walk the command allocation?

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Round 6, lane `a9/vdec`, **firmware side only**. No device access,
no commits, no changes outside this file.

Scope: the resident META/MTX image `REL_812.0_14_Deva`
(`build/video/fw/vxd_deva_fw.bin`, MINIM-expanded to
`build/video/vxdfw/deva.expanded.{bin,dis}`, base **0x01000000**), read against
the host-side model in `docs/periph/ds-vxd-decode-protocol.md` and the
command-stream model in `docs/periph/ds-vxd-cmdstream.md`, and against Intel's
open-source MSVDX driver (`third_party/psb_video`, `src/hwdefs/dxva_*.h`,
`src/psb_cmdbuf.c`, `src/psb_drv_debug.c`).

Marks: **[V]** verified in an artifact (address given, re-checkable) · **[I]**
inference that fits every observation · **[U]** unresolved.

Scratch tool written for this round (gitignored): `build/fwq.py`
(`r a b` range dump, `g regex [ctx]`, `x target` branch/call xrefs,
`calls a b`, `tab a n`) over `deva.expanded.dis`.

---

## 0. Bottom line, ranked by what unblocks a first real decode

1. **The DEVA firmware does not interpret the `CMD_*` words.** There is no
   opcode dispatch, no jump/branch table for the 16 control-allocation opcodes,
   no indexed register store, and no code that dereferences the message's
   buffer address. The command allocation is *handed to silicon* (address +
   context + a per-context hardware window), not executed by the MTX. The
   practical conclusion of `ds-vxd-cmdstream.md` §9 is therefore **right**, for
   a different and much stronger reason than the one it gives. **[V]** for the
   firmware side; **[I]** for "the hardware walks it" (the only remaining
   candidate). Details and every negative scan in §4.
2. **`0x81` w2 is the device address of the work, w1 low half is the flag word,
   and the two flags Apple emits are the whole story.** `0x4004` =
   `FW_VA_RENDER_HOST_INT | FW_VA_RENDER_IS_LAST_SLICE`; `0x2000` =
   `FW_VA_RENDER_NO_RESPONCE_MSG` and it *really does* suppress the completion
   message — verified inside the firmware, not just named in a header (§5, §6).
   So: N packets per frame is legal, but **exactly one 0xc0 comes back, and only
   for the packet that carried 0x4004**. **[V]**
3. **`w1[31:16]` is not used as a size by this firmware.** The field is stored
   to `desc+0x40` as a u16 (`1000d38`) and no reader of `desc+0x40` is reachable
   from the job or completion paths — the four `GETW +64` loads in the whole
   image (`100166c`, `1008614`, `100eecc`, `100ef7c`) are on other objects.
   Consequence: the
   allocation must be **self-terminating** (`CMD_COMPLETION` 0x6 /
   `CMD_NEXT_SEG` 0xd), and the driver can stop trying to compute a word count.
   **[V]** firmware-side, **[I]** for the driver consequence.
4. **`0x80` alone sizes the decode-context pool** — `count = (pipes+1)*8`
   contexts of 0x58 bytes — which is what every `0x81`/`0x86`/`0x87`/`0x88`/
   `0x8b` job needs before it can even allocate its descriptor. `0x82` is a
   *session/codec object*, not a prerequisite. This explains why Apple can go
   `0x80` → `0x81` with nothing in between. **[V]** (§7)
5. **Register 0x200c is the firmware PC** and 0x0 is a "words consumed / next
   read index" scratch the mailbox reader rewrites on every message; the doorbell
   (0x88) is a *wake-up*, not a handshake — the reader polls the TO_MTX write
   index. The live `0x0 = 0x01029821` is therefore **not** a PC. **[V]** (§8)

---

## 1. How this was read (so it can be re-checked)

Everything below is a static reading of `deva.expanded.dis`. Two decoding
quirks bite anyone grepping that file, both used throughout:

* **Immediate flavours.** `MOVT Rd,#0x480` = `0x04800000`; the plain `ADD
  Rd,Rd,#0x2018` form adds the *small* immediate, while the **`T`-suffixed ALU
  forms shift the immediate into the top half** — `ADDT Rd,Rd,#0x480` is
  `+0x04800000`. Proven by the mailbox code, where `MOVT #0x480` + `ADD
  #0x2018` and `ADDT #0x480` + `ADD #0x2000` + `ADDRd,#base` must land on the
  same address family (`0x04802000 + base`). **[V]**
* **Memory-reference offsets print in decimal, ALU immediates in hex.**
  `GETD D0FrT,[D1.5+#68]` is offset **0x44**; `ADD D0FrT,D0FrT,#0x202c` is
  offset 0x202c. Grepping `#0x44` finds nothing. **[V]**
* The register file the firmware touches is at `0x04800000 + offset`, i.e.
  psb_video's `REGISTER_OFFSET()` macro (`msvdx_offsets.h`: `+ 0x04800000`)
  exactly. So **register numbers in this document are host MMIO offsets**
  (the same numbers the host driver writes and the same numbers that appear in
  `CMD_REGVALPAIR_WRITE`). **[V]**

---

## 2. Message → descriptor, decoded instruction by instruction **[V]**

`0x1000d24(desc, msg)` is the only message parser; every host→firmware type
0x80…0x8b goes through `0x10005e8` → pool-alloc `0x1000598` (0x58 bytes) →
`0x1000d24`. Field map, with the storing instruction:

| desc off | width | source | instruction |
| --- | --- | --- | --- |
| `+0x0c` | u8 | constant 1 ("job ready") | `1000e10`/`1000e18` |
| `+0x0e` | u16 | **`msg+2`** = `w0[31:16]` — **fence** | `1000d40`/`1000d58` |
| `+0x10` | u32 | **`msg+0xc`** = `w3` (masked `0xffffff00`, byte OR-ed back) | `1000d94`–`1000db0` |
| `+0x14` | u32 | `msg+0x10` = `w4` | `1000dcc` |
| `+0x18` | u32 | *(not written here — filled by a later stage)* | — |
| `+0x24` | u64 | `msg+0x14` = `w5 \| w6<<32` | `1000d74`/`1000d7c` |
| `+0x38` | u32 | `0xffffffff`, or `msg+0x18` = `w6` when `nwords > 6` | `1000dc0`–`1000de0` |
| `+0x3c` | u64 | `msg+8` = `w2 \| w3<<32` | `1000d84`/`1000d8c` |
| `+0x40` | u16 | `msg+6` = `w1[31:16]` (written, **never read**) | `1000d24`/`1000d38` |
| `+0x44` | u32 | **`msg+4`** = `w1 & 0xffff` — **the 16-bit flag word**, zero-extended by a `GETW` | `1000d60`/`1000d6c` |
| `+0x49` | u8 | `0x89/0x8a` "extra flags" latch (§7) | `10007e8` |

`nwords = w0 & 0x7f` (`1000d30`, `1000d48`); `nwords > 0x14` is rejected at
`1000b80`. `w0[15:8]` is the type byte (`GETB [msg+1]`, `1000dxx`) — the same
packing Apple uses.

Two things worth flagging:

* `+0x3c` (w2) and `+0x40` (size) overlap: the 64-bit store at `1000d8c`
  covers `0x3c…0x43`, so the u16 "size" written 20 instructions earlier is
  clobbered by `w3`. The most likely reading is that the descriptor is a
  **union of the per-type message layouts** (psb's `FW_VA_RENDER`,
  `FW_DEVA_DECODE`, `FW_VA_DEBLOCK` all reuse offsets 0x0c-0x14), so `+0x40` is
  the size for some types and the MMU word for others. For type 0x81 the size is
  dead (§4, third bullet) and nothing reads it **[V]**.
* The `0x89/0x8a` sub-opcode is `(u16 at msg+0x16) & 0xf` = **`w5[31:16]`**,
  not a byte in w0. When it equals `0xc`, `desc+0x49` is set to 1, which is
  later used as a *context index* (`byte[desc+0x49] << 16`, `10001e4`). **[V]**

---

## 3. The `0x81` path end to end

```
0x10008f0  mailbox reader: poll 0x2028 vs 0x2030, copy nwords words into a
           local buffer, publish rd, dispatch by (type - 0x80) <= 0xb
0x1000b08  type 0x81/0x86/0x87/0x88/0x8b → 0x10005e8(type, msg)
0x10005e8  pool-alloc 0x58-byte descriptor, 0x1000d24(desc,msg)  [§2]
           type 0x89 → desc flags |= 0x80 ; 0x8a → |= 0x10        [1000608..1000828]
           (msg+0x16 & 0xf) == 0xc → desc+0x49 = 1                [1000628..10007e8]
           tail-call chain [[A1GbP+0x10]+0x24]+0x10 with (desc,type)
           if !(w1 & 0x4) && !(0x202c & 0x2000) && !(w1 & 0x80) && !(w1 & 0x10)
               0x10002cc(desc)                                    [1000674..10006e0]
0x100b84c  0x10041d4(desc) → 0x100de18(...) ; this is the enqueue
0x1000154  scheduler loop over finished descriptors                  [§6]
```

The `[[A1GbP+0x10]+0x24]+0x10` indirection is a **coroutine link**, not a
call: the value is loaded into `D1RtP` (`100066c`), so when the deep callee
returns, control resumes at the codec module's callback. `[A1GbP+0x10]` is the
codec-handler registry set up at boot; it is **not** a per-codec table keyed on
the message, so there is no "codec selection" in the dispatcher — the codec is
whatever module registered itself (§7). **[V]**

---

## 4. Q1 — Does the firmware walk the command buffer? **No.** **[V]**

Five independent negative scans, each cheap to re-run:

1. **No opcode dispatch.** The only branch table in the image is the 12-entry
   message dispatcher at `0x1000ad4` (found by scanning all 32 332
   instructions for runs of ≥ 4 consecutive `B` instructions — exactly one run,
   length 12). A `switch (word >> 28)` needs either a second branch table or
   compare chains against `0x90000000`/`0x70000000`/`0x60000000`/…; the complete
   set of `MOVT` immediates in the image contains **no** `0x6000`, `0x7000`,
   `0x9000`, `0xa000`, `0xd000`, `0xe000`, `0xf000` top halves. The nearest
   misses are harmless: `0xc000_0000` is a shift mask for the 15 × 2-bit context
   array (`10048dc`) and `0xb000_0004` is a constant stored to a stack slot and
   never compared (`102bd70`). The only other large constants are the register
   base `0x480/0x481`, trace tags `0x61xx`-`0x67xx`, the ready magic `0xa5a5`,
   and code/data link addresses `0x8090/0x8091/0x8288/0x8289`. **[V]**
2. **No shift-by-28.** Only five `LSR/ASR #0x1c` in the whole image
   (`1012614`, `101d084`, `1020030`, `1020034`, `102ee8c`); all are bitfield
   extraction on hardware status words, none is followed by a compare against
   1/2/5/6/7/9/0xa/0xb/0xc/0xd/0xe/0xf. **[V]**
3. **No indexed register store.** The register-replay (`CMD_REGVALPAIR_WRITE`)
   would have to be `*(0x04800000 + reg_from_stream) = val`. In the whole image
   there are exactly **two** `SETD [Ra+Rb]` two-register stores (`100449c`,
   `1004544`), both table initialisation into MTX memory with `0x8090_xxxx`
   pointers. Every other register write in the image uses a *constant* offset. **[V]**
4. **The firmware never touches the codec data registers.** Full inventory of
   VXD register offsets written by the image (from `MOVT #0x480` + `ADD #imm`
   pairs; 235 `MOVT` sites, 180 resolved): `0x0000, 0x0080, 0x0214, 0x0218,
   0x0344, 0x0348, 0x0350, 0x0400, 0x0410, 0x0438, 0x0440, 0x0444, 0x0448,
   0x0470, 0x0480, 0x0484, 0x0500, 0x0508, 0x0510, 0x0608, 0x060c, 0x0630,
   0x0640, 0x1000, 0x1010, 0x1070, 0x1088, 0x108c, 0x2000…0x2030, 0x30d0,
   0x60a4, 0x60e0, 0x6230, 0x6840, 0x6844, 0x6900` (+ `ctx<<16`, §9).
   Nothing in `0x0800–0x0fff` (VEC), nothing in the `0x1000–0x1fff` range
   except the four MMU/context registers, nothing in `0x4000–0x5fff`. A
   control-allocation interpreter would have to write hundreds of VEC/ENTDEC
   registers. It writes none. **[V]**
5. **Nothing dereferences the buffer address.** `desc+0x3c` (w2) is written once
   and never loaded as a pointer; the only two `+0x3c` read-modify-writes in
   the image (`1001788`, `1001c98`) are on *other* objects. Same for
   `desc+0x40`. **[V]**

So `ds-vxd-cmdstream.md` §9 lands on the right answer ("the container magic and
every command word are consumed by the MSVDX command processor"), but its
stated evidence (absence of the `0xabcd` magic) is weak — the strong statement
is that **the whole allocation, magic included, never reaches the MTX**.

What the firmware *does* do with the job (all **[V]**, this is the useful part):

* it manages the **MMU context**: `0x1010` is a 15 × 2-bit context-state array
  (`1004a40`: `0x1e - 2*(0xf - ctx)` shift), `0x1070` bit 0 is a mode bit
  (`10047b8`), `0x1088/0x108c` are invalidate/control (`1003b3c`). The
  page-table base itself (`0x1020`) is host-programmed, which is why w3 can be
  just "(dir & ~0xff) | ctx";
* it writes one **per-context hardware window** word: `0x04800000 +
  (byte[obj+0x15] << 16) + 0x60e0 = 0xbc0` (`100ba04`–`100ba34`), where
  `obj+0x15` is a context/bank index — i.e. **the hardware register file is
  banked per context at `ctx*0x10000`**;
* it programs **DMA channels** with `{address, ctrl/size}` pairs at
  `+0x0000`/`+0x0018` inside a bank (`100f24c`–`100f274`: read a device address
  from a structure → `SETD [0x04800000+ch] = addr`, `SETD [0x04800000+ch+0x18]
  = 0x10`);
* it pulses the decode start (`0x404` ← 0 then 1) in the submit path at
  `102f5cc`/`102f5e4`.

That last group is the "hand it to silicon" path. **[I]** The pieces needed to
confirm it (which register receives w2) are listed in §12.

---

## 5. Q3 — meaning of `0x81` w1…w5, and the psb comparison

`0x81` is `FW_DEVA_DECODE` in psb_video's `dxva_cmdseq_msg.h` (20 bytes:
`MSG_SIZE` 0, `ID` 1, `MSG_ID`/fence 2, `FLAGS` u16 4, `BUFFER_SIZE` u16 6,
`LLDMA_ADDRESS` u32 8, `MMUPTD` 0xc with `MASK 0xffffff00 / SHIFT 8`,
`CONTEXT` u8 0xc, `OPERATING_MODE` 0x10). Apple's message is the same shape with
one extra word:

| w | Apple's expression (`ds-vxd-decode-protocol.md` §12) | psb field | firmware use **[V]** |
| --- | --- | --- | --- |
| 0 | `fence<<16 \| 0x81<<8 \| nwords` | `MSG_ID`/`ID`/`MSG_SIZE` | fence → `desc+0x0e`; type → dispatch; `nwords` → copy length |
| 1 | `(payload[0x0c]>>2)<<16 \| payload[0x04] \| 0x4004/0x2000` | `BUFFER_SIZE`(hi)/`FLAGS`(lo) | **lo = the 16-bit flag word** (only 16 bits reach the descriptor: `GETW` at `1000d60`); hi stored as "size", never read |
| 2 | `chunk_start + payload[0x10]` | `LLDMA_ADDRESS` | address of the work (see below) |
| 3 | `(mmu_dir & ~0xff) \| hwContextID` | `MMUPTD`+`CONTEXT` | MMU context; low byte used as context id, upper 24 bits as the page-table base |
| 4 | `payload[0x08]` | `OPERATING_MODE` | stored `desc+0x14`; **bits 13-14 = deblock mode (§5.1)**; `byte[+0x14]` passed to the enqueue (`100bc70`) |
| 5 | `payload[0x24]` | — (past the 20-byte struct) | stored `desc+0x24` |
| 6 | `payload[0x34]`, only when `nwords == 7` | — | stored `desc+0x38` |

**w2 = the address of the command work.** Cross-check: in psb the field is
literally named `LLDMA_ADDRESS` — the control allocation *is* the linked-list
DMA program (`CMD_LLDMA` 0xa / `CMD_SLLDMA` 0xc / `CMD_NEXT_SEG` 0xd). Apple's
arithmetic (`chunk_start + running_byte_offset`) yields exactly "the address of
this slice's command chunk inside the 64 KiB command buffer". **[V]** for the
arithmetic + name, **[I]** that the hardware is what consumes it. **w3 is not an
address of data** — it is the MMU page-table base + context, confirmed by the
firmware's own use of the context in `0x1010`.

### Flag bits (the part that actually matters)

From `dxva_fw_flags.h`, every bit tested by this firmware:

| bit | name | where tested in the image |
| --- | --- | --- |
| `0x0001` | `FW_VA_RENDER_IS_FIRST_SLICE` | flag shadow at `desc+0x48` (`100033c`) |
| `0x0004` | `FW_VA_RENDER_IS_LAST_SLICE` | `TST #0x4` at `100ba8c`, `100067c` |
| `0x0010` | `REPORT_STATUS` | `10006d0` |
| `0x0040` | `FW_INTERNAL_CONTEXT_SWITCH` | `100bbc0` |
| `0x0080` | (0x89 latch) | `100067c` |
| `0x0200` | (`0x202c` global pair) | `10010e8` |
| `0x0800` | `FW_VA_RENDER_IS_VLD_NOT_MC` | part of the `0x1800` wait gate (`1000240`) **[U]** |
| `0x1000` | (no psb name) | part of the `0x1800` wait gate **[U]** |
| `0x2000` | **`FW_VA_RENDER_NO_RESPONCE_MSG`** | **`100022c` — suppresses 0xc0 entirely** |
| `0x4000` | **`FW_VA_RENDER_HOST_INT`** | **`1001378` — raises the host IRQ** |

`0x1800` (bits 11-12) additionally gates the *wait* in the completion path
(`1000240`, `1000258`): `if (f & 0x1800) skip the wait-for-done loop`. No psb
header names those two bits, and Apple never sets them, so a driver should
leave them 0. **[V]** that the test exists, **[U]** its meaning.

### 5.1 `w4` controls the firmware-side deblock, and it is a real switch **[V]**

Right after the message is decoded (`1000e14`…`1000e50`), for any type other
than `0x88`/`0x86` and any `nwords != 8`:

```
1000e28  if ([desc+0x14] /* w4 */ & 0x6000) return      ; host chose a mode
1000e40  [desc+0x44] |= 0x0400                          ; FW_DEVA_DEBLOCK_ENABLE
```

So **`w4` bits 13-14 (`0x6000`) select the deblock stage, and if both are clear
the firmware turns `FW_DEVA_DEBLOCK_ENABLE` on by itself.** Apple's H.264
`w4 = payload[0x08]` (`*(u32*)(slice + 0x10c)`) is therefore *not* "0 for
H.264" — it is the operating-mode/deblock word, and it is the first thing to
check when a picture decodes with block-edge artefacts but no 0x87 was sent.
**[V]** for the code, **[I]** for the Apple field attribution.

---

## 6. Q4 — completion: one `0xc0`, only for the last packet, only if asked

The scheduler (`0x1000154`…`0x10002cc`) finishes each descriptor like this
**[V]**:

```
1000224  f = [desc+0x44]                     ; w1 & 0xffff (16-bit flag word)
100022c  if (f & 0x2000)                     ; NO_RESPONSE
1000238      if (!(f & 0x1800)) goto next    ;   -> no 0xc0 at all
1000250  if (!(f & 0x1800))
1000264      while (byte[desc+0x0d] != 1) 0x1011478()   ; wait for hw done
1000290  0x1001290(desc)                     ; build + post 0xc0
```

`0x1001290` builds a **4-word** message and calls the single host-post function
`0x1000edc`:

```
10012fc  msg[1] = 0xc0                        ; type
1001334  msg[2..3] = [desc+0x0e]              ; w0[31:16] = FENCE  <-- the fence
100133c  msg[4..5] = f([desc+0x1c])           ; w1 low   (debug word)
1001358  msg[6..7] = f([desc+0x18])           ; w1 high  (debug word)
100132c  msg[8..11] = w1 (via a stack slot)     ; 4th word, source = the request's flag word  [I]
1001378  irq = ([desc+0x44] >> 14) & 1        ; = HOST_INT -> arg2 bit 0
1001380  post(msg, irq, geometry)
```

`0x1000edc` is the **only** host-post function (two callers in the whole image:
`1001208` = the 0xc7 panic path, `1001380` = the 0xc0 path):

* ring data at `0x04802000 + (geom>>16)`, `wr` index `0x2020`, wraps at
  `geom & 0xffff` (`100104c`–`10010c8`);
* `0x2010` is incremented per posted message (`1000f98`–`1000fc8`);
* **`0x410 = 0x00100000` raises the host interrupt** (`10010f4`/`100110c`),
  skipped when `0x202c` bit 9 is set (`10010e8`).

So, to answer the question literally: **completion is per submitted message, it
is suppressed for every message that carries 0x2000, and the IRQ is raised only
for the message that carries 0x4000.** Apple's `0x4004` on the last packet and
`0x2000` on all the others is precisely the intended usage. `0xc6`/`0xc7` come
from the same post function (0xc7 = 2-word panic: `msg[0]=0x82`,
`msg[1]=0xc7`, then `0x2000 = 2` and `0x0 = 0` at `10011b0`…`1001234`).

---

## 7. Q2 — which messages must precede the first `0x81`

Dispatcher (`1000aac`…`1000b78`): `if (type-0x80 > 0xb) drop`, then a 12-entry
`B`-table at `0x1000ad4`:

| type | target | what it does **[V]** |
| --- | --- | --- |
| `0x80` | `1000b18` → `0x1001464` | **init**: caches `msg[4..0xb]` into the core object, sets `memcfg[+0xc] = byte[msg+10]`, `memcfg[+8] = u16[msg+12]`, reads the `0x30xx`/`0x40xx` boot registers into globals, then `0x1004d48()` and **sizes the context pool**: `count = (byte[core+5]+1)*8`; if `pipes > 1` also creates object type 9 (`100156c`…`1001610`) |
| `0x81` | `1000b08` → `0x10005e8` | decode job (§3) |
| `0x82` | `1000b74` → `0x1001248` | **session open**: `0x100e0ac(type=1, msg+4, 1)` → handle stored at `[A1GbP+120]`, and `[A1GbP+12] = 8` |
| `0x83`,`0x84`,`0x85` | `1000958` | dropped |
| `0x86`,`0x87`,`0x88`,`0x8b` | `1000b08` | same path as `0x81`, differ only in the flag latch (§2): `0x88` and `0x86` get extra bytes at `desc+0x42`, `0x87` sets nothing, `0x8b` is the plain generic path |
| `0x89`,`0x8a` | `1000b28` | sub-opcode `w5[31:16] & 0xf`; `== 0xc` sets `desc+0x49 = 1` and falls into the same generic handler; otherwise `0x1000548` |

**Answer to "what must precede the first 0x81":** only `0x80`. It is what makes
`[A1GbP+12]` (the context count) non-zero, and `0x1000c60` then allocates
`count × 0x58` bytes as the descriptor pool that `0x1000598` hands out. Without
it every job message is dropped after `0x1000598` returns nothing. `0x82` is
*not* required (it re-sets the same count to 8 and creates a session object used
only to route the completion callback); Apple never sends it, and that is
consistent. **[V]**

The per-codec handler `[A1GbP+0x10]→[+0x24]→[+0x10]` is a **registered module
vtable**, set up at boot, not selected per message — the type only selects
flags. **[V]**

---

## 8. Q5 — `0x0`, `0x200c`, and the doorbell

Mailbox reader `0x10008f0` (prologue at `0x10008f0`, constants
`0x1000920`–`0x1000950`: `0x2028`, `0x2024`, `0x2000`):

```
1000958  idle: yield(1) ; [0x480] = g1 ; [0x484] = g2
10009ac  rd = [0x2028] & 0xffff
10009b4  wr = [0x2030] & 0xffff
10009cc  if (rd == wr) goto idle              <-- POLLS THE WRITE INDEX
10009d8  base = [0x2024] >> 16 ; ring = 0x04800000 + base + 0x2000
1000bf8  copy nwords words ring[rd..] -> local buffer (wrapping at cap)
1000a9c  [0x2028] = new rd
1000a88  [0x0]     = <words consumed / next index>
1000a4c  [0x200c]  = PC                        (also again in the error path, 1000b8c)
1000aac  dispatch
```

* **`0x200c` = firmware PC.** Written from the `PC` register as
  `MOV Dx,PC; SETD [0x200c],Dx` at **56 sites** (56 `MOV Dx,PC` vs 57 `0x200c`
  stores in the image — a 1:1 coverage/trace hook, e.g. `1000a4c`, `1000b8c`,
  `1004180`, `1004268`). It is re-published constantly, not just once. **[V]**
  So the round-4 doc is right.
* **`0x0` = "words consumed"**, written from the reader's own bookkeeping
  (`D1Re0`, which is `nwords` or the wrapped `rd+1`) at ~20 sites, all of the
  shape `MOVT #0x480; ADD #0x2000; ADD #-0x2000; SETD`. **[V]**
  The live value `0x01029821` therefore **cannot be a PC** (it is also not a
  PC: `0x200c` is the PC) and is not a plausible word count either — treat it
  as a stale/other-writer value and **do not gate the driver on 0x0** **[I]**.
  Most likely explanation: it is the tail of an unrelated write (the same
  register is used as a scratch "0" slot by the panic path, `1001234`), or a
  leftover from the boot loader; the experiment in §12 settles it in one read.
* **The doorbell** (`0x88`) is `MTX_CORE_CR_MTX_KICKI` in psb's own register
  file (`msvdx_mtx_reg_io2.h`: `MTX_KICK = 0x80`, **`MTX_KICKI = 0x88`**) — a
  "kick interrupt", not a queue slot. The firmware **never reads 0x88**; it
  polls `0x2030`. So the doorbell is a latency optimisation only (wake the
  reader out of its `0x1011a74(yield)` idle loop). That is fully consistent with
  the device reporting every message consumed within 0 µs: the reader is
  spinning on the index, and even without the doorbell it would pick the message
  up. **[V]** for "polls 0x2030, never reads 0x88"; **[I]** for "wake-up only".
* `0x480`/`0x484` are poked once per reader iteration with two globals
  (`1000984`, `10009a4`). Undocumented in both docs, purpose unknown; watch
  them if the block misbehaves. **[U]**

---

## 9. What this changes for the driver

1. **Stop computing a word count for `0x81` w1.** Write
   `(0<<16) | flags` exactly like Apple (`0x4004` last, `0x2000` otherwise) and
   make the allocation self-terminating (`CMD_COMPLETION` 0x6 per packet,
   `CMD_NEXT_SEG` 0xd if chaining). The firmware ignores the high half. **[V]**
2. **Send `0x80` before the first `0x81` and make sure its `msg[10]` byte and
   `msg[12]` word are sane** — they become the memory config, and the pool size
   is derived from `byte[core+5]+1`, so a wrong value there silently caps the
   number of concurrent jobs. **[V]**
3. **Expect exactly one completion per submission.** Multi-packet frames are
   fine; only the `0x4004` packet answers. A timeout on a non-last packet means
   the *last* packet never ran, not that the completion was lost. **[V]**
4. **Raising the IRQ is a firmware decision** (`w1 & 0x4000`), not something the
   host can force by touching `0x410`. **[V]**
5. **The kick to watch on the device**: the per-context window write
   `0x04800000 + ctx*0x10000 + 0x60e0 ← 0xbc0` and the `0x404 ← 0/1` pulse. If
   neither appears after a `0x81`, the job never reached the submit path and the
   command stream is irrelevant. If they do appear and nothing completes, the
   fault is in the allocation (or the MMU context), not in the mailbox. **[I]**
6. The whole command stream can still be developed and diffed **host-side** —
   that conclusion of `ds-vxd-cmdstream.md` §9 survives, and is now stronger. **[V]**
7. **`w4` bits 13-14 are the deblock switch**, and the firmware turns
   `FW_DEVA_DEBLOCK_ENABLE` on by itself when they are clear (§5.1). If a picture
   decodes but looks blocky, that is the knob — not the missing 0x87. **[V]**

---

## 10. Corrections to earlier documents

| Earlier statement | This round |
| --- | --- |
| `ds-vxd-cmdstream.md` §9: "the firmware never parses command-buffer words, because there is no `0xabcd` materialisation" | conclusion stands, evidence replaced: no opcode dispatch/branch table, no indexed register store, no VEC/ENTDEC register writes at all, and the buffer address is never dereferenced. §4 |
| §3.2 "`w4` = 0 for H.264" | `w4` is the operating-mode word: bits 13-14 select the deblock stage, and the firmware sets `FW_DEVA_DEBLOCK_ENABLE` (0x400) itself when they are clear (`1000e28`/`1000e40`). §5 |
| `ds-vxd-decode-protocol.md` §3.2 "`w1[31:16]` = size in words" | stored to `desc+0x40` as u16 and never read; do not treat it as a size. It cannot carry flags either: only `w1 & 0xffff` reaches the flag word (`GETW` at `1000d60`). §4/§5 |
| §3.2 "`w2` = 0 for H.264" / §12 "w2 = the word that carries the chunk's location" | the latter is right: `w2` = `LLDMA_ADDRESS` = address of this packet's command chunk. §5 |
| §3.4 "0xc0 carries fence in w0[31:16] and status in w2" | fence in `w0[31:16]` confirmed; `w1` = two debug words, `w2` = **echo of the request's flag word** (`desc+0x44`), and the whole message is 4 words. §6 |
| §3.5 "0xc6/0xc7" | confirmed: `0xc7` is emitted by `0x10011b0` as a 2-word message and then parks the core (`0x2000=2`, `0x0=0`). §6 |
| §2 table "`0x0` = consumed word count, `0x200c` = fw PC" | both correct — but the live `0x0 = 0x01029821` is not explainable as either; see §8/§12 |
| `ds-vxd-cmdstream.md` §9.1 "+0x42 = msg[28] for 0x88/0x86/0x8a" | consistent with `1000df0`/`1000e00` branches on the type byte. `0x89/0x8a` additionally latch `desc+0x49` from `w5[31:16]&0xf == 0xc`. §2 |
| "message types 0x89/0x8a need a sub-opcode" | true, and it is `w5[31:16] & 0xf`, not a w0 byte. §2 |

---

## 11. Register inventory (for the next device session)

Firmware-written VXD offsets (banked ones get `+ ctx<<16`), from
`MOVT #0x480`+`ADD #imm`, complete as far as that pattern resolves (180 of 235
sites; the remainder pass a pre-computed base into a helper):

| offset | count | likely role |
| --- | --- | --- |
| `0x0000` | 20+ | "words consumed" scratch (§8) |
| `0x0080` | 1 | `MTX_KICK` |
| `0x0214/0x0218` | 2 | MTX misc |
| `0x0344/0x0348/0x0350` | 4 | `MTX_SYSC_CDMAC/CDMAA/CDMAS0/CDMAT` — the MTX code/data DMA |
| `0x0400` | 1 | VDMC control |
| `0x0410` | 2 | **IRQ raise (`= 0x100000`)** — §6 |
| `0x0438/0x0440/0x0444/0x0448/0x0470` | 8 | clocks / pipe / deblock-adjacent |
| `0x0480/0x0484` | 6 | reader heartbeat (§8) |
| `0x0500/0x0508/0x0510` | 4 | DMAC flush/control |
| `0x0608/0x060c` | 6 | internal register-file data/index port |
| `0x0630/0x0640` | 2 | misc |
| `0x1000` | 1 | MMU/context control |
| `0x1010` | 3 | **15 × 2-bit context-state array** |
| `0x1070` | 1 | MMU mode bit 0 |
| `0x1088/0x108c` | 2 | MMU invalidate/control |
| `0x2000` | 26 | comms state (3 = idle, 2 = panic) |
| `0x2004` | 10 | live-buffer mask (OR/AND with a job address) |
| `0x2008` | 1 | — |
| `0x200c` | 57 | **firmware PC** |
| `0x2010` | 1 | posted-message counter |
| `0x2018/0x201c/0x2020/0x2024/0x2028/0x2030` | 8 | mailbox geometry/indices |
| `0x202c` | 30 | comms flags (bit 9 = IRQ coalesce, bit 13 = suppress completion) |
| `0x30d0` | 1 | VEC-VLC adjacent |
| `0x60a4/0x60e0/0x6230/0x6840/0x6844/0x6900` | 8 | **per-context hardware window (DMAC channels + sequencer FIFO)** |

---

## 12. Open items, each with the one experiment that settles it

1. **Which register receives w2 (the allocation address)?** Searched all
   constant-offset stores; the candidates are the banked window
   (`0x60e0/0x6230/0x6840`) and the DMA channel pairs (`ch+0x0`/`ch+0x18`,
   `100f24c`). *Experiment:* on the device, after a `0x81`, read the whole
   `0x04800000 + ctx*0x10000 + 0x6000…0x6900` window before and after the kick;
   the register that becomes `cmd_va` is the answer.
2. **What is `0x0 = 0x01029821`?** *Experiment:* read `0x0` and `0x200c` twice —
   before and after a single `0x80` — and once more after a `0x81`. If `0x0`
   tracks `nwords` (5 then), the reader wrote it; if it stays at the same odd
   value it is boot residue and belongs to nobody.
3. **Does `0x82` matter at all for decode?** The pool is already sized by
   `0x80`; `0x82` only makes a session object. *Experiment:* decode with `0x80`
   only (Apple's sequence) — expected to work — then compare the completion
   routing with a `0x82` first.
4. **`0x480`/`0x484` heartbeat** — two globals written every reader iteration.
   Unknown. *Experiment:* dump them; if they are counters, they are a free
   "firmware alive" signal better than `0x2014`.
5. **Banked register file base.** The `ctx<<16` banking means the aperture is
   at least `0x04800000 + 12*0x10000 + 0x6900`. Confirm the aperture size from
   the host's memory-map / ADT node before trusting any `ctx > 3`.
