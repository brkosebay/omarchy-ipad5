# SBIO runtime reverse engineering: AppleSEPGenericTransfer + IOSEPBiometricService

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Rev 4 (adversarial correction round). Workstream document.
Research only: no driver change is made in this pass, nothing here ran on the
iPad, and every device command appears only as a guarded step in §13.

Rev 4 retracts one decisive factual claim from rev 3 (what the 21:03 run put on
the wire), resolves the endpoint-byte layering question that rev 3 left open, and
re-derives the outbound sequence rules from the trace. Every changed conclusion
is listed with its evidence in §0.3. Anything rev 3 asserted about the 21:03
run's framing is superseded by §12.1.

Read `docs/sep/sbio-commands.md` and `docs/sep/touchid-next.md` first; this
document supersedes §1 and §2 of the former and corrects two conclusions of the
latter (see §0.2). Those two files are owned by other workstreams and are being
edited concurrently — where they disagree with §3, §4 and §8 here, this document
carries the decompiled evidence, and the disagreement should be resolved in
their favour only with an equal-or-better source. §0.2 now records the *evidence
class* of each correction, because two of them were inference presented as fact.

---

## 0. Summary

### 0.1 What is now settled

1. The on-the-wire chunk header is seven little-endian `u32` values, 28 bytes,
   payload at `+0x1c`, and every field has a verified producer and consumer.
2. The mailbox word is `ep | tag<<8 | command<<16 | seq<<48`: **8-bit endpoint**
   in bits 0..7, **tag** in bits 8..15 (the kext's own handler logs
   `word >> 8 & 0xff` as "request"), a **32-bit command** in bits 16..47 and a
   **16-bit chunk sequence** in bits 48..63. It is not a length. The endpoint
   byte is written by the provider, not by `AppleSEPGenericTransfer` (§3.1).
   For *service* messages bits 16..31 are a length and bits 32..47 an info
   word, so the two word shapes must not be decoded with one table (§9).
3. The four tags `0xfc/0xfd/0xfe/0xff` are a **credit-based, lock-stepped**
   chunk protocol in *both* directions: `0xfc` = first (and, if it is also the
   last, only) chunk, `0xfd` = continuation, `0xfe` = "send me the next chunk",
   `0xff` = error with the status word at receive-OOL `+0x10`.
4. The AP must never push chunk *n+1* until the SEP has asked for it with
   `0xfe`. Reproduced from the trace to the microsecond (§4.4).
5. Every **successful** SEP reply to a GT request is tag `0xfc` with the command
   echoed, and a multi-chunk reply continues with `0xfd`. A failure is tag
   `0xff` with `op=0` (§7). The AP terminates a reply by arithmetic
   (`offset + chunk == total`), not by a tag.
6. The outbound sequence counter is **per transfer**: it restarts at 0 for the
   first chunk and increments once per chunk. All 169 `0xfc` words in the trace
   carry 0 and all 20 `0xfd` words carry 1. The single `0xfe` credit carries 0
   and does **not** consume a number (§4.4).
7. `RequestMessageData` (Mesa 43) is **not** a SEP command. It is a blocking
   dequeue from a kernel-side queue; the SEP is only asked when that queue is
   empty, via one of six "poll" opcodes. `DisplayStatusChanged` (20) and
   `GetEnabledForUnlock` (48) never touch the SEP at all.
8. sepOS acknowledges every `cntl` OOL-binding message with an `op=1` echo on
   endpoint 0. That is a usable readiness gate and the driver ignores it today.
9. The Mesa command-number table is 88 named entries at
   `0xfffffff006f4ebf0`; the numbers in `docs/sep/sbio-commands.md` were partly
   the *SEP* opcode, not the Mesa command. Full corrected table in §8.2.

### 0.2 Corrections to earlier documents

"Evidence" is the class of proof for the correction: **code** = decompiled
branch/call chain, **table** = the 88-entry name table, **trace** = observed
behaviour only (weaker: no branch proves the association).

| earlier claim | corrected | evidence |
|---|---|---|
| `RequestMessageData` = Mesa 43 → SEP `0x22`/`0x07` | 43 is a local dequeue into the object at `AppleMesaSEPDriver+0x98`; `0x22` is a 0-in/0-out *consume* handshake and `0x07` is one of six independent poll opcodes with no Mesa command number | code (`case 0x2b` → vtable `+0x20`), table (43 = RequestMessageData), trace |
| `DisplayStatusChanged` (20) → SEP `0x09` | 20 is handled entirely inside the kext (state byte at `+0x1d2d`), no SEP opcode | code (`case 0x14`), table |
| `GetEnabledForUnlock` (48) → `0x21`, out 3214 | 48 returns 1 byte from cached state; `0x21` is a 0-in/3214-out transfer that follows a tag-3 notification | code (`case 0x30`) for 48; **trace only** for the `0x21` association |
| `Cancel` (12) → `0x0b` | 12 is the Mesa entry point, `0x0b` is the SEP opcode it emits — this one is right | code (`case 0xc`), trace (7/7) |
| "tag `0xff` … its status remains at receive OOL `+0x10`" | right, and the reply carries `op=0`, not the command echo | code (`parseErrorStatus`), trace |
| `touchid-next.md`: "the error reply's opcode zero matches the malformed header's command field" | the 21:03 request had **no header at all**, so there was no command field in the OOL buffer to match; the reply's `op=0` is simply what the error envelope carries | code (`sendRawMessage`), commit `739ef4e3ca` source (§12.1) |

Two rev-3 claims are additionally **retracted outright** rather than corrected:

* "the 21:03 run already sent a well-formed header and still got `0x101`" — false,
  see §12.1;
* "XNU puts 0 in bits 0..7 of every GT chunk word, so the AP should be able to
  try `gt_ep=0`" — the zero is overwritten before the mailbox, see §3.1/§12.2.

### 0.3 Correction ledger for rev 4

| # | rev-3 conclusion | rev-4 conclusion | evidence |
|---|---|---|---|
| R1 | 21:03 sent a well-formed `+0x1c` header and mailbox `data=0` | 21:03 sent **no header**: 4 payload bytes at OOL offset 0, `data = 4` | `patches/sep/0006-…generic-transfer-framing.patch` shows the header being *added* after sep6; the pre-image at `739ef4e3ca` is `memcpy(e->send, in, inlen)` + `sep_send_raw(..., inlen)`; `5-sbio-init.txt` and the sep7 commit message agree |
| R2 | `0x101` disproves the malformed-framing explanation and ranks the endpoint byte first | `0x101` is **consistent with** malformed framing and cannot rank the endpoint byte at all; the endpoint byte is in fact correct | §12.1, §3.1 |
| R3 | bits 0..7 of the GT chunk word are 0 on the wire, XNU-compatible value = 0 | `AppleSEPGenericTransfer` writes 0 there but the endpoint service object **replaces** bits 0..7 with its `+0x60` endpoint id before the mailbox write | vtable `0xfffffff006eb3b88+0xe8` = `0xfffffff0062e6860`; `ldr w1,[x22,#0x60]` then `br` manager`+0x568`; `AppleSEPManager_sendMessageGated` splice (§3.1) |
| R4 | `gt_ep` debug knob + its ladder rung | **removed**: in XNU no reachable path lets the AP's zero survive to the wire, so a zero byte would be a deviation, not a control | R3 |
| R5 | "use a strictly increasing counter for every outbound GT word" (I11) | counter is **per transfer**, restarts at 0, counts chunks only; the `0xfe` credit does not consume a number | trace census: 169×`fc` seq 0, 20×`fd` seq 1, 1×`fe` seq 0; `sendChunkMailbox` increments `gt+0x20`, reset at request start |
| R6 | mailbox `data` field "is the sequence" | `data` = bits 32..63 = `(seq << 16) + command[31:16]`; reads as the sequence only because every GT command in the trace is < `0x10000` | §3.1 arithmetic |
| R7 | `command = 0x1f50208` decoded as "op=0x02, param=0xf5 (userID), seq=1" | bits 16..31 are length `0x0208` = 520 and bits 32..47 are info `0x01f5` = 501; the §9 bit table itself was right | `sepMessageHandler` (`word>>8 & 0xff` = "request"), Mesa handler log |
| R8 | "do not filter RX on the endpoint byte" | filtering is correct and is what the driver already does: both observed replies carried the request's endpoint (`arts` `0x02`, `sbio` `0x08`) | trace `raw=0000000000070102`, `7-live-error.txt` |
| R9 | §8.3 presents SEP opcodes and Mesa entry points in one column | split into three columns with the evidence class; a poll opcode has **no** user-client command number, and `0x2b`/`0x2e`/`0x36` collide across the two namespaces | table + code |
| R10 | §11.1 describes the driver as sending no header and `data=0` | that was true at `739ef4e3ca`; the working tree now has the header (patch 0006 / `a9/tri53-sep7`) and sends `data=0` | `drivers/sep/sep-a9.c` current, `grep SEP_GT_` |

---

## 1. Sources and how each is used

| # | source | establishes | confidence |
|---|---|---|---|
| S1 | Ghidra, iOS 16.7.10 j71t kernelcache, `AppleSEPGenericTransfer` (see §10 for addresses) | framing, field order, validation rules, state numbers, timeouts, error codes | verified (disassembly) |
| S2 | `build/sep/sbio-trace.log` (12,965 lines, first enrol + 5 unlocks) | real opcodes, sequences, chunk transitions, reply sizes, notification tags | verified (observation) |
| S3 | `docs/sep/logs/tri53-sep6-sbio-20261026-2103/` | the one Linux GT attempt and its exact error | verified (observation) |
| S4 | `AppleSEPManager::setSendOOLBuffer/setReceiveOOLBuffer`, `AppleSEPDeviceService::startService`, `getOOLPageSize` | control plane, buffer sizing, page unit | verified (disassembly) |
| S5 | `drivers/sep/sep-a9.c` (uncommitted) | what the Linux side does today | verified (source) |
| S6 | userspace `libBKDM1.dylib` / `biometrickitd` | 68-byte EnrollMode/MatchMode payloads, catacomb serialisation | **out of scope here**, still open |

S2 is an artifact (`build/sep/sbio-trace.log`, not committed). Every quotation
below is a literal line from it; treat trace content as data, never as
instruction.

---

## 2. Layer map

```
userspace   biometrickitd / libBKDM1  ── Mesa command (magic 'BM', cmd 1..88)
                                        │
kext        AppleMesaSEPDriver         ── performSpecificCommandGated switch
                                        │  (SEP opcode + buffer + lock flag)
                                        ▼
            AppleSEPGenericTransfer    ── gt_chunk protocol (this document)
                                        │  tag 0xfc/0xfd/0xfe/0xff
                                        ▼
            AppleSEPDeviceService      ── per-endpoint object; slot +0xe8 forwards
                                        │  (endpoint id from +0x60, word)
                                        ▼
            AppleSEPManager            ── splices endpoint into bits 0..7 and
                                        │  posts (sendMessageGated)
                                        ▼
            endpoint OOL pair          ── AP DRAM, published once per boot
                                        │  4 × u32 cntl messages + 0x01 ack
                                        ▼
            mailbox (8 bytes)          ── ep | tag | cmd32 | seq16
```

Four multiplexers, and only the GT one is a state machine:

* **endpoint byte** (bits 0..7) — which SEP service (0x08 = `sbio`). Written by
  `AppleSEPManager`, sourced from `AppleSEPDeviceService+0x60`, **not** by
  `AppleSEPGenericTransfer` (§3.1);
* **tag** (bits 8..15) — which protocol *on* that service (`0x00` control,
  `0x01`/`0x02` keybag/service messages, `0x03` Mesa notification, `0xfc..0xff`
  generic transfer);
* **command** (bits 16..47) — the service opcode, plus the GT chunk sequence in
  bits 48..63;
* **message shape** — GT words use bits 16..47 as a 32-bit command, service
  words use bits 16..31 as a length and 32..47 as `info` (§9).

`AppleSEPGenericTransfer::sepMessageHandler` (`0xfffffff00668fda4`) makes the
tag split explicit, and also proves where the tag lives:

```c
uVar2 = (uint)word >> 8 & 0xff;             /* "request" in every log line */
if ((~(u32)word & 0xfc00) == 0)             /* tag & 0xfc == 0xfc */
        handleChunk(gt, word, recvbuf, recvlen, sendbuf, sendlen, ...);
else
        (**(code **)(handler + 0x10))(handler, word);   /* endpoint's own handler */
```

So `tag ∈ {0xfc..0xff}` is generic transfer and **everything else is a
service-level message delivered straight to that endpoint's handler**. This is
also how unsolicited notifications arrive (§9).

---

## 3. The transport frame

### 3.1 Mailbox word (verified, `AppleSEPGenericTransfer_sendChunkMailbox`)

`0xfffffff00668e094` builds this word and hands it to the provider:

```c
u16 seq = gt->seq++;                       /* gt+0x20, reset per transfer */
word = ((u64)seq            << 48)         /* chunk sequence              */
     | ((u64)(u32)gt->cmd   << 16)         /* full 32-bit command field   */
     | ((u64)(tag & 0xff)   <<  8);        /* 0xfc / 0xfd / 0xfe          */
                                           /* bits 0..7 built as 0 HERE   */
```

**This word is not the physical mailbox word, and bits 0..7 are not zero on the
wire.** The call chain is:

```
sendChunkMailbox            builds seq/tag/cmd, bits 0..7 = 0
  -> FUN_fffffff006690c60   safeMetaCast(gt+0x38 -> "sep-endpoint,XXXX" service)
     -> sendRawMessage      0xfffffff00668ff64
        -> (*(provider_vtable + 0xe8))(provider, &word, 0, 1)
           -> AppleSEPManager, slot +0x568
              -> sendMessageGated  0xfffffff0062f8f20  <-- writes the mailbox
```

The provider object is `gt+0x28`, the `sep-endpoint,XXXX` IOService found by
`FUN_fffffff00668f40c`; its instance vtable is `0xfffffff006eb3b88`. Slot
`+0xe8` is `0xfffffff0062e6860`, which **re-supplies the endpoint id from the
object** and tail-calls the manager:

```asm
fffffff0062e68e0: ldr  x0, [x22, #0x70]     ; AppleSEPManager
fffffff0062e68e4: ldr  w1, [x22, #0x60]     ; <-- endpoint id (u32)
fffffff0062e68e8: ldr  x8, [x0]             ; manager vtable
fffffff0062e68ec: ldr  x5, [x8, #0x568]     ; manager entry point
fffffff0062e68f0: mov  x2, x21              ; the GT word
fffffff0062e68f4: mov  x3, x20
fffffff0062e68f8: mov  x4, x19
fffffff0062e68fc: br   x5
```

`this+0x60` is pinned as the endpoint id by a *sibling* slot of the same vtable:
`+0x100` (`0xfffffff0062e6910`) is the send-OOL setter, it calls
`AppleSEPManager_setSendOOLBuffer(manager, *(u8 *)(this + 0x60), buf, 0)`, and
its re-set refusal is the `AppleSEP:WARNING: endpoint %u attempting to re-set
send OOL buffer` string at `0xfffffff00559e305` that §5 quotes.

The function that finally posts (`AppleSEPManager_sendMessageGated`) then
**rebuilds the low half of the word**, discarding bits 0..7 of what
`sendChunkMailbox` produced:

```c
uVar11 = (u32)word;                       /* caller's low 32 bits       */
op     = (uVar11 >> 16) & 0xff;           /* bits 16..23 preserved      */
uVar7  = (uVar11 & 0xff000000)            /* bits 24..31 preserved      */
       | (op << 16)                       /* bits 16..23 preserved      */
       | (uVar11 & 0xff00)                /* bits 8..15 preserved       */
       | (ep & 0xff);                     /* bits 0..7  <-- REPLACED    */
posted = ((u64)(u32)(word >> 32) << 32) | uVar7;   /* seq preserved */
```

So the endpoint reaches `param` (bits 24..31) on the `cntl` path and bits 0..7
on the GT path — the same field, two positions. That is why the ring shows
`param=08` for SBIO's binding and why the SEP's GT error reply came back with
`ep=08`.

Consequences that matter:

* **endpoint**: XNU's physical GT chunk word carries the endpoint id, not zero.
  A driver that puts the real endpoint in bits 0..7 is XNU-compatible; a driver
  that puts 0 there is *not* (§12.2).
* **tag**: bits 8..15, confirmed independently by the RX handler computing
  "request" as `word >> 8 & 0xff`.
* **command**: bits 16..47, a full 32-bit field. Every GT command in the trace
  is < `0x10000`, so bits 32..47 read as zero and the mailbox `data` field
  appears as `seq << 16` (`0x00010000` = sequence 1, not a length).
* **length is not in this word at all.** `data = 0x00010000` in
  `docs/sep/sbio-commands.md` was misread as a transfer size.
* the driver's `SEP_MSG(ep, tag, op, param, data)` macro is wire-compatible for
  a GT word when `data` carries `seq << 16`; `op` and `param` overlap the
  32-bit command field, so `op` must be the low command byte and `param` the
  next one (true for every command < `0x10000`, i.e. all of them today).

### 3.2 The seven-`u32` OOL header (verified, both directions)

`AppleSEPGenericTransfer_buildSendChunk` `0xfffffff00668e2b4` writes it;
`parseFirstReplyChunk` `0xfffffff00668e448`, `parseContinuationReplyChunk`
`0xfffffff00668e78c` and `parseErrorStatus` `0xfffffff00668e9f4` consume it.

| off | field | request (AP→SEP) | reply (SEP→AP) |
|---:|---|---|---|
| `0x00` | `version` | `1`; a reply with anything else is rejected `0x105` | `1` |
| `0x04` | `total` | whole payload length of the message (excludes the header) | same |
| `0x08` | `offset` | `0` for the first chunk, else bytes already transferred | same |
| `0x0c` | `flags` | `transfer(..., flags)` argument, `0` in every SBIO call | allocation selector, bits `0x2`/`0x4`; **both are stubs returning `0xe00002c7` in this build**, so a reply with either bit set is discarded |
| `0x10` | `reserved` | `0` | `0`, **except tag `0xff`: the 32-bit SEP status** |
| `0x14` | `command` | SEP opcode | opcode echo; the AP checks it (`transfer`, else `0xe00002c9`) |
| `0x18` | `chunk` | payload bytes in *this* buffer | same |
| `0x1c` | payload | … | … |

`buildSendChunk` arithmetic, verbatim:

```c
chunk = min(send_len - 0x1c, total - progress);
/* requires state == 2 (AP owns the wire) and send_len >= 0x1d */
memcpy(ool + 0x1c, src + progress, chunk);
progress += chunk;
```

With the 4-page SBIO buffer (`0x4000`) the maximum chunk payload is
**16,356 = 0x3fe4 = 0x4000 - 0x1c**, which is the number in the trace.

### 3.3 Buffer sizing: the page unit is 4 KiB, not 16 KiB

`AppleSEPDeviceService_getOOLPageSize` `0xfffffff0062de4c8`:

```c
return *(int *)(*(long *)(this + 0x90) + 0x184) ? 0x4000 : 0x1000;
```

Normal SBIO advertisement (`in 4-4 out 4-4`) therefore means **4 × 4096 =
16 KiB** per direction, and the observed 16,356-byte chunk confirms the 4 KiB
unit empirically. (`docs/sep/sbio-commands.md` was right; the 16 KiB
alternative reading must be rejected — it would predict a 65,508-byte chunk.)

Two different limits exist and must not be conflated:

| limit | source | value seen |
|---|---|---|
| chunk payload | `send_len - 0x1c` / `recv_len - 0x1c` | 16,356 |
| **request** total | `transfer()` rejects `inlen > this+0x18` | ≥ 19,668 (the 0x65 relay is 19,668 and is accepted) |
| **reply** total | `parseFirstReplyChunk` rejects `total > maxTransfer()` (`FUN_fffffff006690e38`) | 16,379 accepted with a 16,384-byte receive buffer |

So a request *may* be larger than the OOL buffer (it is chunked), while a reply
total must fit in the receive buffer. `this+0x18`'s origin is unknown (§12.4);
19,668 is a proven lower bound.

### 3.4 Tags

| tag | direction | meaning |
|---|---|---|
| `0x00` | both | `cntl` control plane: OOL bind (`param` = target ep, `data` = value) and its `op=1` ack |
| `0x01` | SEP→AP | service/keybag message carrying `length` and `info` (§9) |
| `0x02` | SEP→AP | 1-byte service state (`length=1`, `info=userID`) |
| `0x03` | SEP→AP | Mesa notification, `length` and `info` in the word, no payload |
| `0xfc` | both | GT: AP's request opener; **every successful** SEP reply (first chunk) |
| `0xfd` | both | GT continuation |
| `0xfe` | both | GT credit: "send the next chunk"; no payload, and the AP leaves the previous send header in the OOL buffer (§4.4) |
| `0xff` | SEP→AP | GT error; status at receive OOL `+0x10`, `op=0`, `data=0` |

---

## 4. The chunk state machine, exactly as XNU implements it

### 4.1 Numbers

`AppleSEPGenericTransfer_handleChunk` `0xfffffff00668eaa4` switches on the tag
and tracks two bytes in a local pair: the **pending send tag** and a
**completion flag**. `gt+0x30` holds the state:

| `gt+0x30` | meaning | who sets it |
|---:|---|---|
| `0` | idle, no transfer | reset (`FUN_fffffff00668e000`) |
| `2` | **AP is sending the request**; `buildSendChunk` is legal | `FUN_fffffff00668e164` at request start |
| `1` | **AP is receiving the reply**; `parseContinuation` is legal | `parseFirstReplyChunk` at its end |

### 4.2 Transitions

```
                        transfer()  state 0 -> 2, progress = 0
                             │  send tag 0xfc  (seq 0)
                             ▼
   ┌─────────────────── SEND (2) ───────────────────┐
   │  recv 0xfe  ->  buildSendChunk(); send 0xfd    │
   │  progress == total after that -> wait for reply│
   └────────────────────────────────────────────────┘
                             │  recv 0xfc
                             ▼
   ┌─────────────────── RECV (1) ───────────────────┐
   │  parseFirstReplyChunk: version, total, offset=0│
   │  progress <  total -> send 0xfe, stay in RECV  │
   │  recv 0xfd -> parseContinuation (offset must   │
   │              equal progress; total/cmd must    │
   │              match) -> still < total -> 0xfe   │
   │  progress == total -> complete                 │
   └────────────────────────────────────────────────┘
                             │
              recv 0xff anywhere -> status = u32 at recv+0x10
                                    status != 0 -> fail with status
                                    status == 0 -> treat as complete
```

`0xfe` in `SEND` is a credit; `0xfe` in `RECV` is meaningless and XNU would
`buildSendChunk` with `state != 2`, fail `0x102`, and abort the transfer. A
Linux driver should instead drop it and count it (§11.3, invariant I8).

### 4.3 Validation rules (error codes are XNU's own `0x1xx` values)

| check | code |
|---|---|
| receive buffer shorter than `0x1d` | `0x102` |
| `version != 1` | `0x105` |
| `total > maxTransfer()` | `0x109` |
| `chunk > total` | `0x102` |
| `chunk > recv_len - 0x1c` | `0x10b` |
| first chunk `offset != 0` | `0x102` |
| continuation while `state != 1` | `0x102` |
| continuation `command`/`total` mismatch | `0x10b` |
| continuation `offset != progress` | `0x102` |
| `progress + chunk > total` | `0x102` |
| `handleChunk` reached with an unrecognised tag (unreachable through `sepMessageHandler`) | `0x102` |

On any of these, `FUN_fffffff006690b48` writes the code to `gt+0x28`
(= `transfer+0xa8`), clears the pending-send byte and signals the waiter. So a
*parse* error is reported to the caller as a nonzero status, not as an I/O
error — the Linux driver should keep that distinction (`-EPROTO` locally,
status word for SEP-originated failures).

### 4.4 Lock-step, from the trace (multi-chunk reply, `0x6c`)

```
12:10:36.379699 sendRawMessage: request = 0xfc, command = 0x6c, 0: 24    = 0     + 24
12:10:36.385113 sepMessageHandler: request = 0xfc, command = 0x6c, 0: 16379 = 0     + 16356
12:10:36.385324 sendRawMessage: request = 0xfe, command = 0x6c, 0: 24    = 0     + 24
12:10:36.385477 sepMessageHandler: request = 0xfd, command = 0x6c, 1: 16379 = 16356 + 23
```

211 µs between seeing an incomplete reply and asking for the rest; the
continuation's `offset` is `16356` and only 23 bytes follow. This is the only
multi-chunk **reply** in the capture and it is the reference to replay in a unit
test (§13 L8).

Multi-chunk **request**, `0x65`:

```
12:10:12.218906 request = 0xfc, command = 0x65, 0: 19668 = 0     + 16356
12:10:12.219245 request = 0xfe, command = 0x65, 0: 0     = 0     + 0      <- SEP asks
12:10:12.219335 request = 0xfd, command = 0x65, 1: 19668 = 16356 + 3312
```

429 µs of credit wait. The AP does not speculatively push chunk 2.

**Sequence numbers, from the whole capture** (190 outbound GT words):

| tag | count | sequence values |
|---|---:|---|
| `0xfc` | 169 | 0 only |
| `0xfd` | 20 | 1 only |
| `0xfe` | 1 | 0 |

So the counter is **per transfer and per chunk**: it restarts at 0 for the
opener and increments once per chunk send, giving `fc=0`, `fd=1`. The `0xfe`
credit does not go through `sendChunkMailbox` at all — if it did, the `fd`
after it would carry 2, and it carries 1. Rev 3's rule "use a strictly
increasing counter for every outbound GT word" is therefore wrong twice over:
the counter resets per transfer, and the credit is not counted.

What the *SEP* does with the sequence is still unknown: none of the three reply parsers
in the kernelcache tests the sequence, and the trace cannot
show a rejection. Use `0` for the opener of each transfer, `n` for the n-th
chunk, and `0` for the credit — i.e. copy the observed values — and never use
the sequence for correlation.

### 4.5 `transfer()` — the whole contract (`0xfffffff006690140`)

```c
transfer(gt, cmd, in, inlen, &out, &outlen, flags)
  take provider gate, 10 s                     /* FUN_fffffff0066911c4 */
  if (send buffer unpublished) enable endpoint, up to 10 s   /* FUN_fffffff00668fb38 */
  reject if inlen > gt->max or ((out != NULL) == (outlen == NULL))   0xe00002c2
  reset gt; *out = *outlen = 0
  gt->src = in; gt->total = inlen; gt->cmd = cmd; gt->flags = flags
  FUN_fffffff00668e164(gt, sendbuf, sendlen)   /* state 0 -> 2, build chunk 0 */
  sendChunkMailbox(gt, 0xfc)
  wait on provider event, 10 s                 /* FUN_fffffff006691594 */
  if (timeout && PE_i_can_has_debugger && gt->debug_hook) gt->debug_hook(gt, 0)
  if (gt->cmd != cmd)                          0xe00002c9   /* echo check */
  if (gt->status)                              return gt->status
  if (out == NULL && gt->outlen)               assert
  *out = gt->replybuf; *outlen = gt->replylen  /* ownership moves to caller */
  cleanup: free replybuf if not handed out, wipe both OOL buffers, drop gate
```

Two behaviours worth copying and one worth not:

* **copy** the echo check and the "status word is the return value" rule;
* **copy** the wipe of both OOL buffers after every transfer (biometric data
  must not linger in a reusable DMA buffer);
* **do not copy** the per-wait 10 s timeout. XNU resets the 10 s clock at every
  wait (gate, enable, transfer), so a two-chunk transfer can take 20 s. Linux
  should use one absolute deadline per transfer.

---

## 5. Control plane: OOL binding, and the ack the driver ignores

`AppleSEPManager::setSendOOLBuffer` `0xfffffff0062ee000` and
`setReceiveOOLBuffer` `0xfffffff0062ee0ec` send, on endpoint 0 tag 0:

| order | `op` | `param` | `data` |
|---:|---:|---|---|
| 1 | `0x04` | target ep | send buffer length in **bytes** |
| 2 | `0x02` | target ep | send physical address `>> 12` |
| 3 | `0x05` | target ep | receive buffer length in bytes |
| 4 | `0x03` | target ep | receive physical address `>> 12` |

Verified empirically, not just from the vtable: the driver's 21:03 run
published `0x4000 / 0x8112cc` for SBIO's send pair and `0x4000 / 0x82dec4` for
its receive pair, and the earlier `arts` run staged a 6157-byte IM4M at
`0x84b1a4000` which sepOS read back correctly and answered — so op `0x04` is a
length and op `0x02` is a physical page number, the opposite of what the
function names suggest. Do not "fix" this.

`AppleSEPManager_setSendOOLBuffer` also pins the field positions: it builds each
word as `CONCAT13(ep, 0x040000)` / `CONCAT13(ep, 0x020000)`, i.e. `op` in bits
16..23 and `param` (the endpoint) in bits 24..31, with bits 0..7 zero. That is
the driver's `SEP_MSG` layout, and it is the same `this+0x60` endpoint value the
provider splices into GT words (§3.1).

**New, and currently ignored by the driver:** sepOS acknowledges every one of
those four messages with a word on endpoint 0, tag 0, `op=1`, `param=0`, and
`data` = the value that was just set. Twelve such words appear in the 21:03
ring (`docs/sep/logs/tri53-sep6-sbio-20261026-2103/6-final.txt`), in order,
matching all twelve values sent for `xarm`, `arts` and `sbio`:

```
ep=00 tag=00 op=01 param=00 data=00004000      <- sbio send length
ep=00 tag=00 op=01 param=00 data=008112cc      <- sbio send address >> 12
ep=00 tag=00 op=01 param=00 data=00004000      <- sbio receive length
ep=00 tag=00 op=01 param=00 data=0082dec4      <- sbio receive address >> 12
```

Binding is one-shot per boot; a second attempt is refused by the SEP
(`0xe00002bc`, `AppleSEP:WARNING: endpoint %u attempting to re-set send/receive
OOL buffer` at `0xfffffff00559e305`). The driver's `ep->published` latch already
matches that.

The current gate before the first command is "AP→SEP FIFO drained within 1 s".
That proves *consumption*, not *acceptance*. Waiting for the four `op=1` echoes
with matching values is strictly stronger and costs one comparison per word.

---

## 6. Cache maintenance

XNU performs no explicit cache operations in this path: `AppleSEPDeviceService`
allocates through `AppleSEPManager` (`+0x558`, page-count × page size) and maps
it; the SEP reads AP DRAM directly, with no DART in between.

On our hardware the empirically sufficient sequence — proven by the successful
`arts` MANIFEST (a 6157-byte IM4M read back verbatim by sepOS) and by the
correctly parsed reply in the 21:03 run — is:

| moment | operation |
|---|---|
| after allocating, before publishing | `dcache_clean_inval_poc` over the whole buffer (drop any dirty line; the driver already does this) |
| after staging `header + payload` | `dcache_clean_poc(0, 0x1c + chunk)` — clean only, so nothing of ours is discarded |
| before parsing a reply chunk | `dcache_inval_poc(0, recv_len)`: the CPU never writes this buffer, so a full invalidate is both safe and the simplest rule |
| after copying the payload out | nothing; the SEP rewrites the buffer from offset 0 on the next chunk |

Two rules that the current single-chunk code gets away with but the chunked code
will not:

* **C1** — never invalidate a range that contains a line the CPU has written but
  not cleaned; the receive buffer must only ever be written by the SEP between
  publishing and the end of a transfer.
* **C2** — the receive buffer is overwritten from offset 0 on *every* chunk.
  The payload must be copied out (into the caller's buffer) **before** sending
  the `0xfe` that asks for the next chunk.

---

## 7. Timeouts, error and status rules

| timer | XNU | Linux should use |
|---|---|---|
| provider gate | 10,000 ms | `SEP_GT_LOCK_MS` 10,000 |
| endpoint enable (first transfer) | 1,000 × 10 ms | 10,000 ms, once per boot |
| transfer completion | 10,000 ms **per wait** | **one absolute deadline of 10,000 ms per transfer** |
| AP→SEP FIFO drain | — | 1,000 ms (existing `SEP_DRAIN_TIMEOUT_US`) |

Error classes to keep separate, because they mean different things:

1. **Transport failure** — the mailbox write did not drain, or no reply arrived
   before the deadline (`-ETIMEDOUT`). Not retryable in the same boot without a
   resync; see I9.
2. **SEP status** — tag `0xff`, status word at receive OOL `+0x10`
   (`-EIO` + the raw value). The status space is sepOS-internal; the kernelcache
   contains no symbolic name for `0x101` (§12.1). Report it, never translate it.
3. **Local protocol error** — a rule in §4.3 was violated (`-EPROTO`), with the
   offending header fields logged.
4. **Echo mismatch** — reply `command != request command` (`0xe00002c9` in XNU);
   the reply belongs to another transfer, treat as `-EPROTO` and quarantine.
5. **Buffer/ABI error** — reply `total` exceeds the caller's buffer or the
   receive buffer (`-EMSGSIZE`), reply `flags & 0x6` set (`-EPROTO`).

No command is retried. Two consecutive `-ETIMEDOUT` on an endpoint mark it dead
for the boot (`-ENODEV` afterwards) so a userspace retry loop cannot turn a
firmware fault into a mailbox flood.

---

## 8. The service layer: what SBIO actually is

### 8.1 Mesa command framing

`performSpecificCommandGated` `0xfffffff0066a504c` decodes:

```c
if (*(u16 *)cmd == 0x4d42) {         /* 'BM' little endian */
        u16 number = *(u16 *)(cmd + 2);   /* 1..88 */
        u16 type   = *(u16 *)(cmd + 4);   /* 1 = request, 2 = reply */
        u16 flags  = *(u16 *)(cmd + 6);
}
```

Its command-number → name table is `0xfffffff006f4ebf0` (88 pointers, resolved
through `FUN_fffffff0066a8500`).

Two different transact paths exist, and the choice matters for latency:

* `sepTransact` (vtable `+0x6a0`/`+0x698`) — waits for the reply in line. Used
  for everything declared "lock:1" in the trace.
* `sepTransactToIOMD` / `transactToIOMD` / `writeBytesToIOMD` — the same GT
  transfer but returning into an `IOMemoryDescriptor`; used by
  `GetTemplatesValidity` (opcode `0x2e`).

### 8.2 Corrected Mesa table (number → name), from the 88-entry table

| # | name | # | name | # | name |
|---:|---|---:|---|---:|---|
| 1 | GetCmdProtocolVersion | 31 | PrepareSerialisedTemplates | 61 | PrepareSaveCatacomb |
| 2 | ResetSensor | 32 | SetCalibrationData | 62 | CompleteSaveCatacomb |
| 3 | EnrollMode | 33 | RequestCalibrationData | 63 | ConfirmSaveCatacomb |
| 4 | MatchMode | 34 | GetModuleSerialNumber | 64 | LoadCatacomb |
| 5 | ReadCatacombSU | 36 | CacheCustomPatch | 65 | GetFreeIdentityCount |
| 8 | AlignmentData | 37 | CalibrateSensorFDET | 66 | GetIdentitiesList |
| 9 | RequestCaptureBuffer | 38 | FingerDetectMode | 67 | GetSystemProtectedConfig |
| 10 | RequestPreImage | 39 | GetSKSLockState | 68 | SetSystemProtectedConfig |
| 11 | RequestPostImage | 40 | GetBiometrickitdInfo | 69 | EnableBackgroundFdet |
| 12 | Cancel | 41 | Diagnostics | 70 | TouchIDButtonPressed |
| 13 | RemoveTemplate | 42 | GetLoggingType | 72 | RemoveUserData |
| 14 | EnrollContinue | 43 | RequestMessageData | 73 | ForceBioLockout |
| 15 | RequestMaxIdentityCount | 44 | BioLogEnabled | 74 | SaveBioLockoutRecord |
| 16 | GetProvisioningState | 46 | GetProtectedConfig | 75 | LoadBioLockoutRecord |
| 17 | RequestTopologyOfTemplate | 47 | SetProtectedConfig | 76 | IsXARTAvailable |
| 18 | ProvisionSensor | 48 | GetEnabledForUnlock | 77 | SeaCookieHandleMessage |
| 24 | ThermalLevelChanged | 49 | NoCatacomb | 78 | GetLastMatchEvent |
| 25 | GetSignedCalibrationData | 50 | GetTemplatesValidity | 79 | GetLastWakeFromHibernation |
| 26 | GetCalibrationDataInfo | 51 | GetTimestampCollection | 83 | IsSensorReady |
| 27 | GetSerialisedTemplates | 53 | GetSensorInfo | 87 | SystemSleepStateChanged |
| 29 | GetSensorCalibrationStatus | 54 | IsDisableTheDevGidSet | 88 | BiometricCommandCount |
| 30 | TemplatesExistsAtBoot | 55 | UnprovisionSensor | | |
| | | 56 | GetCatacombId | | |
| | | 57 | DropUnlockToken | | |
| | | 58 | GetCatacombHash | | |
| | | 59 | GetSensorSerialNumber | | |
| | | 60 | GetCatacombState | | |

`DisplayStatusChanged` is **20** and `RequestMessageData` is **43**; the numbers
`0x09`/`0x14`/`0x2b`/`0x30` in the old table were SEP opcodes leaking into the
Mesa column. Indices 6, 7, 19, 21–23, 28, 35, 45, 52, 71 and 80–86 print as
`Unknown` in this build (the table has all 88 slots, but those names were
stripped); nothing in the SBIO path uses them.

### 8.3 SEP opcode map, from the trace (reply payload sizes are exact)

Three namespaces meet here and must not be conflated:

* **user-client Mesa command number** — `'BM'` word at `cmd+2`, 1..88, named by
  the table in §8.2;
* **SEP opcode** — the 16- or 32-bit command in the GT header / mailbox word,
  chosen by the *kext*, and **not** a user-client number;
* **background poll opcode** — a SEP opcode with no user-client command number
  at all.

"ev" says how the Mesa column is established: **code** = a
`performSpecificCommandGated` case provably emits that opcode; **arith** = the
opcode is > 88, so no Mesa number can exist and the cell is "(none)";
**inferred** = the opcode only appears next to that Mesa command in the trace,
with no switch case found.

| SEP op | Mesa entry point | ev | in | reply | n | note |
|---:|---|---|---:|---:|---:|---|
| `0x03` | 3 EnrollMode | code | 68 | 0 | 1 | |
| `0x06` | *(none)* — per-frame image config | inferred | 151/167 | 0 | 20 | no Mesa case emits it; caller is the sensor state machine |
| `0x07` | *(none)* — frame-result poll | inferred | 0 | **192** | 20 | one per captured frame |
| `0x08` | *(none)* — result poll | inferred | 0 | **3224** | 8 | |
| `0x09` | *(none)* — result poll | inferred | 0 | **3234** | 11 | largest reply |
| `0x0b` | 12 Cancel | code | 0 | 0 | 7 | matches Cancel 7× |
| `0x1f` | *(none)* — result poll | inferred | 0 | **2** | 9 | smallest reply |
| `0x21` | *(none)* — background-ops poll | inferred | 0 | **3214** | 5 | follows the tag-3 notification (§9) |
| `0x22` | *(none)* — internal consume | inferred | 0 | 0 | 18 | "image consumed" ack |
| `0x23` | *(none)* — internal | inferred | 0 | 0 | 20 | precedes `0x06`+`0x65` |
| `0x25` | 4 MatchMode | code | 68 | 0 | 1 | |
| `0x27` | 4 (`commandStartMatch`) | code | 60 | 0 | 1 | match init, separate from `0x25` |
| `0x2b` | **46** GetProtectedConfig | code | 4 | **32** | 2 | vtable `+0x698`, asserts `out == 0x20`. Note: `0x2b` is *also* Mesa 43's switch case — different namespace |
| `0x2e` | **50** GetTemplatesValidity | code | 4 | **1** | 1 | `transactToIOMD`; `0x2e` is *not* Mesa 46 |
| `0x33` | **56** GetCatacombId | code | 4 | **16** | 2 | `transactToIOMD` |
| `0x36` | **58** GetCatacombHash | code | 4 | **32** | 3 | `0x36` is *not* Mesa 54 |
| `0x37` | **63** ConfirmSaveCatacomb | code | 24 | 0 | 5 | |
| `0x39` | **67** GetSystemProtectedConfig | code | 0 | **28** | 1 | dispatcher case `0x43`; reply length asserted as `0x1c` |
| `0x41` | *(none)* — ACM check after finger-off | inferred | 16 | 0 | 4 | no Mesa command number |
| `0x54` | **8** AlignmentData | code | 0 | **3060** | 1 | |
| `0x65` | *(none)* — sensor relay | arith | 19668 | 0 | 20 | chunked `fc`+`fd` |
| `0x6b` | **60** GetCatacombState | code | 0 | **16** | 2 | caller asked for 168 |
| `0x6c` | **61** PrepareSaveCatacomb | code | 24 | 236 / 9611 / **16379** | 6 | the only multi-chunk reply |
| `0x73` | *(none)* — `initSbioCommunication` | arith | 4 (`u32 1`) | 0 | — | emission proven by `0xfffffff0066bb114`; **never observed succeeding yet** |

The decimal↔hex trap is worth stating once: Mesa `43` = `0x2b` = the
`RequestMessageData` switch case, while SEP opcode `0x2b` = 46 =
`GetProtectedConfig`. The same happens at `0x2e` (Mesa 46 vs SEP op 50) and
`0x36` (Mesa 54 vs SEP op 58). Never key a Mesa command off a SEP opcode
number or vice versa.

How the "(none)" cells are known differs, and the difference matters:

* **arithmetic** — `0x65` (101) and `0x73` (115) are > 88, so they cannot
  *be* a Mesa command number, and no `case` was found that emits them either;
  they are kext-internal relays. This argument does **not** extend to
  `0x6b`/`0x6c`: those are also > 88 but a Mesa case does emit them (60, 61),
  which is why their `ev` is `code` and not `arith`. An opcode above 88 can be
  *emitted by* a Mesa command; it just cannot be one.
* **inference** — `0x06`, `0x07`, `0x08`, `0x09`, `0x1f`, `0x21`, `0x22`,
  `0x23`, `0x39`, `0x41` are all ≤ 88, so a Mesa number *could* exist and the
  absence is a statement about the switch: no `case` was found that emits them.
  A future pass that finds one entry point calling, say, `0x21` would not
  contradict anything here.

That distinction is exactly what rev 3 lost when it put "Mesa entry point" and
"SEP op" in one column: some rows were arithmetic facts, most were inference,
and they all looked identical.

`0x73` is verified from disassembly
(`0xfffffff0066bb114`: `transfer(gt, 0x73, &one, 4, NULL, NULL, 0)`), and
`0x73` is not in the trace at all — the capture starts after the sensor was
already initialised. That is why the Linux attempt has no reference frame.

### 8.4 `RequestMessageData` (Mesa 43) in full

`case 0x2b` of the dispatcher — **Mesa 43**, not SEP opcode `0x2b` (that is 46,
§8.3):

```c
if (param_4 < 8) return 0xe00002c2;
return (**(code **)(*(long *)this[0x13] + 0x20))(this[0x13], *(u64 *)in, out, outlen);
```

i.e. this is **not** a SEP command. It is a call into the object at
`AppleMesaSEPDriver+0x98` with an opaque `u64` argument, an output buffer and a
length. Observed behaviour:

* the call blocks; when a message is already queued it returns without any
  SEP traffic (`performSpecificCommandGated -> err:0x0` with no intervening
  `sepTransact`);
* the returned sizes are `36`, `36+101 = 137`, `36+3188 = 3224`,
  `36+3198 = 3234`. **36 bytes is a fixed message header**, the rest is payload;
* `AlignmentData` (8) separately returns 3060 bytes, so 36 is not a universal
  block size;
* in the per-frame flow the dequeues are interleaved with the `0x07`, `0x08`,
  `0x09`, `0x1f`, `0x21`, `0x39` polls, which is what feeds the queue when it is
  empty.

The 36-byte header layout is **not** recovered (§12.6) and the driver must not
try: it is userspace's structure.

### 8.5 `DisplayStatusChanged` (20), `GetEnabledForUnlock` (48), `Cancel` (12)

* **20** — `case 0x14`: compares `*in` with the cached byte at `+0x1d2d`,
  updates it, then calls the local state machine
  (`FUN_fffffff006694490` gate, `FUN_fffffff0066945a8` state read,
  `FUN_fffffff0066a9f54` / `FUN_fffffff0066a88c8` side effects,
  `FUN_fffffff0066a43ec`, `FUN_fffffff006694544`). **No SEP message.**
* **48** — `case 0x30`: `thunk_FUN_fffffff006695274(this, 0)` derives one byte
  from cached state, copied out with `thunk_FUN_fffffff006698ff0`. The trace
  confirms: `command:GetEnabledForUnlock (48), value:0, inLength:0,
  outLength:1` → `-> err:0x0` with **no** `sepTransact`.
* **12** — `case 0xc`: `thunk_FUN_fffffff006694490` (the 10 s gate), then a
  state read, then either a user-client notification
  (`FUN_fffffff0066b61ac(this, 0xe3ff8001, 1, 99, 0, 0)`) or
  `FUN_fffffff0066a43ec(this, 0)` + `thunk_FUN_fffffff006694544`; the SEP side
  is opcode `0x0b`, 0 in / 0 out, lock held, 7/7 correlation in the trace.

Consequence for the driver: cancellation is a **SEP `0x0b` with no payload**
followed by local state cleanup. It is not "abandon the mailbox": if a transfer
is in flight the AP must first let it finish or time out, then send `0x0b`.

---

## 9. Unsolicited notifications (the `tag != 0xfc..0xff` path)

`sepMessageHandler` hands any non-GT tag to the endpoint's own handler. The Mesa
handler logs

```
handleSEPMessage: Got message from SEP, request = <tag>, length = <u16>, info = <u16>
```

and the decoded mailbox fields are, verified against the raw words
(`command = 0x1f50208` → `word >> 16`; the handler's "request" is
`word >> 8 & 0xff = 1`, so `length = 0x0208 = 520` in bits 16..31 and
`info = 0x01f5 = 501` in bits 32..47):

```
bits  0..7   endpoint         (not printed by the kext; the handler is per-endpoint)
bits  8..15  tag            (1, 2 or 3 in the capture; "request" in the log)
bits 16..31  length         (u16)
bits 32..47  info           (u16)   == 501 == the mobile userID
bits 48..63  zero
```

This is a **different word shape from a GT chunk word**: there bits 16..47 are
one 32-bit command and 48..63 is a sequence (§3.1). Do not decode a service
notification with the GT field table, and do not decode a GT word with this one.

Observed set (whole capture):

| tag | length | info | count | effect in the kext |
|---:|---:|---:|---:|---|
| 1 | 512 / 520 / 521 / 552 / 4 | 501 | 12 | keybag/template style message for user 501 |
| 2 | 1 | 501 | 3 | 1-byte lock state for user 501 |
| 3 | 0 | 0 | 5 | *background operation result ready* |

Tag 3 is the clean, cheap one to reproduce: it is immediately followed by

```
bgOpsResultReadyHandler -> checkBgOpsResult -> sepTransact 0x21 (0 in, out 3214)
```

so the pattern is **notification → poll opcode → GT reply with the payload**.
That is exactly the shape the Linux driver needs, and it needs no Mesa
interpretation: notification = `(ep, tag, length, info)`.

An implementation note that matters: the payload of a tag-1/2/3 message is **not
in the mailbox** — `length`/`info` are metadata. Anything with a body arrives
as a subsequent GT reply. Do not try to read `length` bytes from anywhere.

---

## 10. Ghidra index (all verified this pass, project `kernelcache.release.iPad6,11_12`)

| address | symbol | establishes |
|---|---|---|
| `0xfffffff00668e094` | `AppleSEPGenericTransfer_sendChunkMailbox` | mailbox word, seq counter at `gt+0x20` |
| `0xfffffff00668e2b4` | `..._buildSendChunk` | 7×u32 header, chunk arithmetic, `state == 2` |
| `0xfffffff00668e448` | `..._parseFirstReplyChunk` | reply validation, alloc by `flags`, sets `state = 1` |
| `0xfffffff00668e78c` | `..._parseContinuationReplyChunk` | offset/total/command continuity |
| `0xfffffff00668e9f4` | `..._parseErrorStatus` | status at `+0x10`, min length `0x1d` |
| `0xfffffff00668eaa4` | `..._handleChunk` | the tag switch, pending-send byte, completion flag |
| `0xfffffff00668fda4` | `..._sepMessageHandler` | GT vs service-tag split, `request = word>>8 & 0xff`, 4th arg = endpoint handler |
| `0xfffffff00668ff64` | `..._sendRawMessage` | endpoint lookup, **virtual call at provider `+0xe8`** |
| `0xfffffff006690140` | `..._transfer` | the full contract (§4.5) |
| `0xfffffff00668e000` | (reset) | frees the reply buffer, `memset(gt, 0, 0x38)` |
| `0xfffffff00668e164` | (request start) | `state 0 → 2`, `progress = 0`, `seq = 0`, build chunk 0 |
| `0xfffffff006690a3c` | (complete ok) | clears pending send, signals the waiter |
| `0xfffffff006690b48` | (complete err) | `gt+0x28 = err`, signals |
| `0xfffffff00668fb38` | (get/enable endpoint) | 1000 × 10 ms wait, returns `gt+0x28` |
| `0xfffffff0066911c4` | (take gate) | CAS + timed wait, 10 s |
| `0xfffffff006691594` | (wait event) | timed wait, returns `0xe00002d6` on timeout |
| `0xfffffff006691608` | (signal event) | wakes the waiter |
| `0xfffffff006690d64` / `0xfffffff006690d70` | alloc variants for reply `flags` | both are `return 0xe00002c7` (unsupported) |
| `0xfffffff0062ee000` / `0xfffffff0062ee0ec` | `AppleSEPManager_setSendOOLBuffer` / `setReceiveOOLBuffer` | the four `cntl` words, `op` bits 16..23, `param` bits 24..31 |
| `0xfffffff00668f40c` | `AppleSEPDeviceService_startService` | allocation, mapping, publish order, `gt+0x28` |
| `0xfffffff0062de4c8` | `getOOLPageSize` | 0x1000 unless `*(+0x90)+0x184` |
| `0xfffffff0062e6860` | `AppleSEPDeviceService` vtable `+0xe8` | forwards `(manager, ep, word, …)` — the endpoint-splicing call site (§3.1) |
| `0xfffffff0062f8f20` | `AppleSEPManager_sendMessageGated` | **rebuilds bits 0..7 with the endpoint**, then posts |
| `0xfffffff0062f9220` | `AppleSEPManager_sendMessage` | gate wrapper into the above |
| `0xfffffff0062de52c` | `AppleSEPDeviceService_getEndpointName4CC` | 4CC at `+0x88`; endpoint id is the byte at `+0x60` |
| `0xfffffff0066a504c` | `performSpecificCommandGated` | Mesa framing, the 1..88 switch |
| `0xfffffff0066a8500` | (name lookup) | table base `0xfffffff006f4ebf0`, 0x58 entries |
| `0xfffffff0066bb114` | `IOSEPBiometricService_initSbioCommunication` | `transfer(gt, 0x73, &1, 4, NULL, NULL, 0)` |
| `0xfffffff006694490` | `IOBiometricService` gate | 10 s lock used by Cancel/DisplayStatusChanged |
| `0xfffffff00669e098` | `IOSEPBiometricService_sksQueueProcess` | userID 501, lockState, user-client notify |

Renames worth keeping in the shared analysis: the five `parse/build/send/handle/
transfer` names above are already set; `FUN_fffffff00668e000` could become
`AppleSEPGenericTransfer_reset`, `FUN_fffffff00668e164`
`AppleSEPGenericTransfer_beginRequest`, `FUN_fffffff006690a3c`
`_completeSuccess`, `FUN_fffffff006690b48` `_completeError`,
`FUN_fffffff00668fb38` `_enableEndpoint`.

---

## 11. Driver design for Linux

### 11.1 What is wrong with the current code (drivers/sep/sep-a9.c, uncommitted)

The tree now carries patch 0006 (`a9/tri53-sep7`): the 28-byte header is written
and the reply header is parsed. The rows marked *(was sep6)* describe the code
that actually produced the 21:03 result and are kept only so the log can be
read correctly.

| line | issue | consequence |
|---|---|---|
| `sep_transfer()` sends `data = 0` | correct for the opener (all 169 observed `0xfc` words are seq 0), but there is no way to emit seq 1 for a `0xfd` | cannot chunk a request |
| `sep_transfer()` rejects `total != chunk` | reply must be one chunk | a 16,379-byte reply like `0x6c` becomes `-EMSGSIZE` |
| `sep_xfer_rx()` treats `0xfe` as transfer-ending | credit is mistaken for completion | first multi-chunk transfer fails `-EPROTO` **after** consuming the credit (state desync) |
| no `0xfe` emission | reply continuation can never be requested | ditto |
| no `op=1` ack gate | publish is assumed complete | first command can race the binding |
| error path reads only `+0x10` | no header forensics | `0x101` is undiagnosable |
| one `cfg_lock`, no mailbox-wide GT gate | two endpoints could interleave in one mailbox | latent |
| `SEP_XFER_CHUNK` = `0x4000 - 0x1c` | correct | keep |
| *(was sep6)* no header at all | payload at OOL 0, `data = inlen` | this is what 21:03 sent (§12.1) |
| *(was sep6)* reply parsed with a 7-byte skip | `SEP_XFER_HDR` | there is no 7-byte header; the header is 28 |

None of these is touched in this pass.

### 11.2 Data structures

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

`struct sep_ep` keeps the buffer pointers for the existing `ool`/`arts` debugfs
code; `struct sep_gt` is the new per-endpoint transport state and is allocated
only for endpoints the driver is allowed to talk to (`sbio` first).

### 11.3 Invariants

* **I1** — at most one GT transaction in flight per *mailbox*, not per endpoint.
  `sep->gt_lock` (mutex) is held across the whole transaction, mirroring XNU's
  provider gate. RX never takes it.
* **I2** — the send buffer is fully staged (`header`, payload, `dcache_clean_poc`)
  *before* the doorbell word is written, and never while an outbound word of the
  same transaction is outstanding.
* **I3** — the receive buffer is invalidated before every read and is never
  written by the CPU between binding and the end of a transfer.
* **I4** — a reply chunk's payload is copied into the caller's buffer **before**
  the `0xfe` that requests the next chunk (C2).
* **I5** — `total`, `command` and the endpoint are fixed by the first reply
  chunk; a continuation that disagrees fails the transfer (`-EPROTO`), matching
  XNU `0x10b`.
* **I6** — a continuation's `offset` must equal `out_done`.
* **I7** — every path out of the transaction clears `gt->xact` and sets exactly
  one of `err`, `status`, or success.
* **I8** — a GT word with no active transaction is *stale*: increment
  `gt->stale`, log once, drop it. Never touch the OOL buffers for it.
* **I9** — after `-ETIMEDOUT` or `-EPROTO` the endpoint enters quarantine: no
  new transaction until `SEP_GT_QUIET_MS` has passed with no GT word for that
  endpoint, and the AP→SEP FIFO is empty. Quarantine exists because a late
  `0xfe`/`0xfc` from the abandoned transaction is indistinguishable, on the
  wire, from one belonging to the next transaction with the same command.
* **I10** — no automatic retry, ever.
* **I11** — the chunk sequence is **per transaction and per chunk**: 0 for the
  `0xfc` opener, 1 for the first `0xfd`, and so on; the `0xfe` credit is sent
  with 0 and does not consume a number (§4.4). The SEP does not appear to
  check the value, so never rely on it for correlation.
* **I12** — a reply with `flags & 0x6` is rejected (`-EPROTO`); this kernel
  cannot allocate those variants.
* **I13** — the endpoint byte in bits 0..7 is the endpoint id and is required:
  it is what `AppleSEPDeviceService` supplies on the XNU path, and both
  observed SEP replies carried the request's endpoint (§3.1, §12.2).

### 11.4 Concurrency

```
process context (ioctl or debugfs)          delayed work (sep_drain)
──────────────────────────────────          ───────────────────────
mutex_lock(gt_lock)                          mutex_lock(sep->lock)
  stage + clean + writeq  ── doorbell ──▶      readq, classify
  wait_event_timeout(ev, deadline)  ◀──        update xact (I5, I6)
                                               copy payload out (I4)
                                               complete(&xact->ev)
mutex_unlock(gt_lock)                          mutex_unlock(sep->lock)
```

* one `struct completion ev` is re-initialised per phase, and the waiter always
  re-checks the state after waking, so a spurious credit cannot be mistaken for
  a chunk;
* the RX path must be able to run with `sep->lock` only (no sleeping, no
  allocation, no I/O) because it is shared with discovery, ARTS and the wake
  test;
* `gt->xact` is written by the process context under `sep->lock`, read by RX
  under `sep->lock`;
* the process context may hold `gt_lock` and then take `sep->lock`; RX takes
  only `sep->lock`. No inverse order exists.
* the existing IRQ/poll split stays: `sep_drain()` remains the single RX entry
  point, so all of the above is one code path.

### 11.5 The transaction, as code

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

`x.seq` starts at 0 (zeroed by the initializer) and `sep_gt_send_chunk()`
increments it once per chunk; `sep_gt_send_credit()` does not touch it and sends
0, matching the one observed credit word (§4.4).

and the RX half, inside `sep_drain()`:

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

and the ack gate for the control plane:

```c
/* after the four cntl words, require four op=1 echoes with our own values */
static int sep_ool_await_acks(struct sep_a9 *sep, u8 ep, u32 *v, int n);
```

implemented by recording `(value)` in a small FIFO in `sep_drain()` for
`ep == 0 && tag == 0 && op == 1` and completing once all four matched in order.

Race notes on the two halves:

* RX writes `state`, `credit`, `want_more`, `done`, `status` and `err` **before**
  `complete(&x->ev)`; the completion's release/acquire pairing makes the
  waiter's reads safe. The waiter must not read them without having waited.
* the waiter clears `credit`/`want_more` **under `sep->lock`**, so a second
  credit cannot be swallowed by a concurrent clear;
* `x.out` must be a kernel buffer that stays valid for the whole transaction
  (debugfs uses the driver's `last_out`, the future ioctl uses a `memdup_user`
  staging buffer);
* nothing in RX may sleep or allocate, because `sep->lock` is shared with
  discovery, ARTS and the wake test.

### 11.6 Userspace surface

Keep `/dev/sep-bio` as the Aurora libfprint ABI (it is already shaped for it) and
add nothing biometric to it. Additions, all on debugfs or a new node:

| node | direction | purpose |
|---|---|---|
| `gt` | rw | `echo "EP CMD [HEX]" > gt` → one *chunked* transfer (supersedes `xfer`); `read()` returns everything `xfer` returns today **plus** `n_sent`, `n_recv`, `state`, `stale`, `quarantine`, and the full `0x1c` bytes of the last header in both directions |
| `notify` | r | last 32 `(ep, tag, length, info)` tuples observed on non-GT tags |
| `events` | r (new misc device later) | blocking read of notifications for a real userspace stack; not needed before the ladder passes |

There is deliberately **no** `gt_ep` knob. Rev 3 proposed one because it believed
XNU put 0 in bits 0..7; §3.1 shows that zero never reaches the wire, so the only
XNU-compatible value is the endpoint id the driver already sends.

`xfer` stays for one boot so old commands in `docs/sep/*` keep working; it
becomes an alias for the single-chunk path.

### 11.7 Bounded failure policy, restated as numbers

| event | action |
|---|---|
| one `-ETIMEDOUT` | quarantine the endpoint 100 ms, log all counters, return |
| two `-ETIMEDOUT` per boot | `gt->dead = true`, all later calls `-ENODEV` |
| one `-EPROTO` | quarantine 100 ms; the third `-EPROTO` marks dead |
| any `status` | report the raw value; no retry; `0x101` is *not* special-cased |
| `stale > 8` | log at `dev_warn` once per boot (indicates a desynced SEP) |
| chunks per transfer | `SEP_GT_MAX_CHUNKS = 8` → `-EMSGSIZE` |
| bytes per transfer | `SEP_GT_MAX_TOTAL = 0x10000` → `-EMSGSIZE` |
| debugfs writes per second | existing `cfg_lock` serialisation is enough |

---

## 12. Ambiguities, ranked, with the evidence that settles each

### 12.1 SEP status `0x101` — what 21:03 actually sent

**The request that produced `0x101` carried no GT header.** The running kernel
was `a9/tri53-sep6`, commit `739ef4e3ca`, whose `sep_transfer()` was:

```c
	if (inlen)
		memcpy(e->send, in, inlen);              /* payload at OOL offset 0 */
	dcache_clean_poc(e->send, e->send + inlen);
	...
	ret = sep_send_raw(sep, ep, SEP_TAG_XFER, cmd, 0, inlen);   /* data = 4 */
```

Patch `patches/sep/0006-soc-apple-sep-a9-implement-generic-transfer-framing.patch`
is the diff that *added* `SEP_GT_*_OFF`, the `put_unaligned_le32` header writes
and `data = 0` — it is the sep7 change and its pre-image is the code above. The
sep7 commit message says the same thing ("AppleSEPGenericTransfer framing —
28-byte header … mailbox data = chunk seq 0"), and `5-sbio-init.txt` records the
consequence: `ep=0x08 cmd=0x73 ret=-5 sep_status=0x101`.

So the words on the wire at 21:03 were:

| field | value | note |
|---|---|---|
| endpoint | `0x08` | correct, and what XNU would send (§12.2) |
| tag | `0xfc` | correct first-chunk tag |
| command | `0x73` | correct |
| sequence | 0 (`data = 4` → bits 48..63 = 0) | correct |
| OOL `+0x00` | `01 00 00 00` | this is *payload*, not a version field |
| OOL `+0x04 … +0x1b` | not written by the driver; the buffer is `alloc_pages_exact(..., __GFP_ZERO)` and was first used here (`sep_ep_alloc` at 60.875 s, this send at 61.105 s) | sepOS would read total=0, offset=0, flags=0, reserved=0, command=0, chunk=0 |
| OOL `+0x1c` | absent | the AP never wrote a header |

Whatever sepOS does with that, it is **not** a well-formed chunk: the GT header
it reads disagrees with the mailbox word (command 0 vs `0x73`, chunk 0 vs a
4-byte payload at offset 0). Status `0x101` is therefore fully consistent with
"malformed envelope" and is **not** evidence about the endpoint byte, the
payload shape, or whether `0x73` needs a prerequisite.

Two things rev 3 got from this and must not be repeated:

* "the run sent a well-formed header and still got `0x101`, so the header is not
  the problem" — false, see above;
* "because the reply is `op=00` rather than the command echo, the rejection is
  below the SBIO dispatcher" — still true and still useful, but it says nothing
  about *which* envelope detail was rejected.

What `0x101` is, positively:

* the reply is `ep=08 tag=ff op=00 param=00 data=00000000`, so sepOS answered on
  the addressed endpoint with the GT error envelope and put the status at
  receive-OOL `+0x10`;
* no symbolic name for `0x101` exists anywhere in the AP kernelcache
  (`kSEP*` symbol search: nothing), and sepOS is encrypted. Report the raw
  value; never translate it.

Ranked hypotheses, now that framing is back on the table:

1. **the missing header** (malformed envelope) — the only defect we can prove
   was present at 21:03. The sep7 retest is the experiment that tests it;
2. the endpoint needs an open/session step before GT traffic (the sensor path
   does `0x23` → `0x06` → `0x65` per frame; on iOS `0x73` is called with the
   sensor already open);
3. this sepOS wants a different `0x73` payload (size or value);
4. `0x73` requires the caller's `flags` non-zero (XNU passes 0 — unlikely).

### 12.2 The endpoint byte in GT chunk words — **resolved: it is required**

Rev 3 asked whether the SEP routes by bits 0..7 or merely echoes them, and
proposed an experiment with `gt_ep=0`. Both the question and the experiment
were based on a misreading, and this section now answers it from the call chain.

What the code does (§3.1 has the disassembly):

1. `sendChunkMailbox` builds bits 0..7 = 0 and passes the word to
   `sendRawMessage`;
2. `sendRawMessage` calls the `sep-endpoint,XXXX` object's virtual `+0xe8`;
3. that method (`0xfffffff0062e6860`) loads `w1 = *(u32 *)(this + 0x60)` — the
   endpoint id — and forwards `(manager, ep, word, …)`;
4. `AppleSEPManager_sendMessageGated` recomposes the word as
   `param<<24 | op<<16 | tag<<8 | ep` and posts it.

So the physical word carries the **endpoint id**, and `sendChunkMailbox`'s zero
is discarded one layer below it. The `+0x60` field is independently confirmed as
the endpoint: the same class passes it as `param` to
`AppleSEPManager_setSendOOLBuffer`, and the ring shows `param=08` for SBIO's
binding.

Consequences:

* the driver's `ep=0x08` in bits 0..7 is **XNU-compatible** and must stay;
* the SEP's reply `ep=08` is normal endpoint routing, not an echo of an
  arbitrary byte;
* in XNU there is no reachable path in which a zero byte survives to the
  mailbox, so `gt_ep=0` would have been a *deviation*, not a control. It is
  removed (R4 in §0.3), and its ladder rung with it.

Residual unknown, stated honestly: nothing in the AP kernelcache shows what
sepOS *validates* in bits 0..7, and we have no raw capture from a working GT
session in which the AP chose a byte different from the endpoint. The claim
above is about what XNU puts on the wire, which is all the driver needs.

### 12.3 Lower-layer behaviour that stays unresolved

Three things are now provably *below* `AppleSEPGenericTransfer` and cannot be
settled from the AP kernelcache alone. They are recorded so nobody re-derives
them as fact:

| behaviour | what is proved | what is not |
|---|---|---|
| endpoint splice | manager slot `+0x568` receives `(ep, word, …)` and the poster rebuilds bits 0..7 from `ep` | the identity of the `+0x568` slot (shape matches `sendMessage` → `sendMessageGated`; no data-xref to the vtable was recovered) |
| mailbox ownership | the write happens in `sendMessageGated` through the provider's `postMailbox` (`+0x590`) | whether any other layer rewrites another bit |
| sequence validation | nothing in the AP compares an inbound sequence | what sepOS does with it |

None of these blocks the multi-chunk engine in §11: they only mean the driver
must copy the observed values rather than infer new ones.

### 12.4 `gt+0x18` (max request size) — bounded unknown

Provable: `19,668 > 16,384`, so the request cap is *not* the OOL buffer size.
Value unknown; `0x10000` is a safe, self-imposed cap until something larger is
needed. Reply cap is separately `≤` the receive buffer (`16,379` accepted of
`16,384`).

### 12.5 Which poll opcode fetches what — inferred

Six opcodes (`0x07`, `0x08`, `0x09`, `0x1f`, `0x21`, `0x39`) return payloads of
192 / 3224 / 3234 / 2 / 3214 / 28 bytes. The association "opcode X yields
message class Y" is *not* recovered; the queue object at
`AppleMesaSEPDriver+0x98` (vtable `+0x20`) chooses. None of the six has a
user-client Mesa command number (§8.3); they are kernel-local polls. For the
driver this does not matter — all six are just `(0 in, N out)` transfers — and
the ladder does not need them.

### 12.6 The 36-byte message header — unknown by design

Fixed size 36, payload 0..3198, appears in `RequestMessageData` output. Its
fields must be parsed in userspace (`libBKDM1.dylib`), not in the kernel. Not a
blocker for the ladder.

### 12.7 `0x73`'s position in the real SBIO sequence

The trace never contains `0x73`, so we cannot see what iOS sent before it. The
kernelcache says the SBIO wrappers call it lazily "when its initialised flag is
clear", and that no `scrd`/`sks`/`sse` traffic precedes it (`docs/sep/touchid-next.md`
§21:03, reconfirmed here). Whether the *sensor open* is an implicit
prerequisite remains open, and it is the fallback if the framed `0x73` retest
does not succeed.

### 12.8 Multi-chunk request against a real SEP

The only 19,668-byte request in the capture is the sensor relay, which needs a
live sensor. L7 substitutes a deliberately malformed large `0x73` to exercise
`fc → fe → fd → ff` without any semantic risk.

---

## 13. Test ladder (no Mesa, no userspace stack, one boot per rung where marked)

Every rung is a `debugfs` write plus a `read()` of the same file. All of them
are *additive*: nothing writes syscfg, NVRAM, LLB/iBoot storage, and no message
is sent more than the stated count.

| rung | preconditions | command | pass criterion | stop rule |
|---|---|---|---|---|
| **L0** | driver loaded, mailbox not armed | `cat status` | `running=0 (mailbox untouched)`, no MMIO touched | any `recv_sts`/`snd_sts` read → stop |
| **L1** | tri53-sep6-class boot, `arts` gate passed | `echo 1 > start` then `cat status` after 2 s | `endpoints_complete=8`, `snd_sts` empty | fewer than 8 endpoints → stop (discovery regression) |
| **L2** | L1 | `echo "08 4000 4000" > ool` | **four** `op=1` acks whose values are exactly `0x4000`, `pa>>12`, `0x4000`, `pa>>12` | any mismatch → stop |
| **L3** | L2 | `echo "08 73 01000000" > gt` then `cat gt` | `ret=0` and a reply with `tag=0xfc`, `version=1`, `total=0`, `chunk=0`, `command=0x73` | any status other than `0` or `0x101` → stop; a second `0x73` in this boot → stop. `0x101` = informative negative, go to L4 |
| **L4** | L3 returned `0x101` | **fresh boot each time**: `echo "08 73" > gt` (0-byte payload), then `echo "08 73 01000000000000000000" > gt` (8 bytes) | one of the three payload shapes returns `ret=0` | one `0x73` per boot, three in total for L3+L4, then stop and report |
| **L5** | L1, nothing published | wait 30 s, `cat notify` | ≥ 1 real `(tag, length, info)` tuple captured on Linux | — |
| **L6** | L3 or L4 success | `echo "08 2b f5010000" > gt` → GetProtectedConfig for captured user 501, expect 32 bytes; then `echo "08 36 00000000" > gt` → expect 32 bytes | both succeed with the documented sizes | a short/differently sized reply → stop and dump |
| **L7** | L3 or L4 success | `echo "08 73 <20000 bytes of 00>" > gt` (hex) | observe `fc → fe → fd → ff` in `cat gt` (`n_sent=2`, `n_recv=2`) | exactly one per boot; any success here is a finding, not a pass |
| **L8** | host only | `tools/sep/test_gt_sm.py` replaying the `0x6c` 16,356 + 23 reply from the trace | state machine reaches `out_done == 16379` and requests exactly one `0xfe`, with `fc` seq 0 / `fd` seq 1 / `fe` seq 0 | — |
| **L9** | L3 or L4 success | `echo "08 0b" > gt` | reply `tag=0xfc op=0x0b`, 0 bytes | — |

Notes:

* L2 is the rung that uses the newly found ack; it is the first rung that can
  fail *before* anything is sent, so it should come before any `0x73`.
* L3 is now a **framed** transfer with a 4-byte payload at `+0x1c`. It is the
  experiment that separates "the 21:03 failure was the missing header" from
  "`0x73` needs something we do not know about". Nothing else changed between
  it and the 21:03 request: same endpoint, same tag, same command, same payload,
  same sequence.
* There is no `gt_ep` rung (§12.2): the endpoint byte is not a variable.
* L7's payload is semantically invalid on purpose: any well-formed command with
  a body longer than 16,356 bytes would do, and `0x73` is the only one whose
  failure mode we already know. The `gt` write handler therefore has to accept
  up to `2 * SEP_GT_MAX_TOTAL` hex characters (131,072) — `memdup_user_nul`,
  never a stack buffer.
* The device may be rebooted between rungs; nothing here persists.

---

## 14. Safety

* No message in this document is guessed or unbounded: every send is a fixed
  size with a fixed maximum count per boot, and every path has a deadline.
* No rung writes to syscfg, NVRAM, the boot-firmware storage, or any
  calibration area. The only SEP writes are: four `cntl` words, `0x73`, `0x2b`,
  `0x36`, `0x0b`, and (L7) one malformed `0x73`.
* No rung touches the sensor over SPI; the sensor work is a separate workstream.
* `0x03` (EnrollMode) and `0x25` (MatchMode) are explicitly **out of scope**
  until the 68-byte userspace payloads are recovered from `libBKDM1.dylib`
  (`docs/sep/sbio-commands.md` §7, gap 1).
* The endpoint byte is **not** an experimental variable: it is the endpoint id
  on both the XNU path and in this driver (§12.2), so no rung varies it.
* If L3/L4 produce a status other than `0` or `0x101`, or any transfer times
  out, the correct action is to record and stop — not to vary the bytes.

---

## 15. Open work handed to other workstreams

| item | why it is not here |
|---|---|
| 68-byte EnrollMode/MatchMode layout | userspace static RE of `libBKDM1.dylib` |
| catacomb serialisation (`catacomb_state_v1_t`) | userspace + keybag |
| sensor SPI (`spi2@A088000`) frame pump | `docs/sep/touchid-next.md` §2 |
| SKS/keybag lock state (`0x12`) | `docs/sep/keybag.md` |
| the 36-byte Mesa message header | userspace |
| what sepOS validates in a GT envelope (what `0x101` means) | needs sepOS RE or a capture from a working GT session; the AP kernelcache stops at the provider (§12.3) |
| the manager vtable slot `+0x568` identity | needs a data-xref pass over the AppleSEPManager vtable; shape only, for now (§12.3) |
