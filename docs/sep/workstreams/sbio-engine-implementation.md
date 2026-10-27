# SBIO GenericTransfer engine: implementation note

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Status: implemented in the shared checkout, **not committed**, no device run.
Companion to `docs/sep/workstreams/sbio-runtime-re.md` (protocol) and
`docs/sep/linux-fprint-integration.md` (userspace side, untouched here).

Scope of this change: a bounded, reviewable **multi-chunk GenericTransfer engine**
plus **passive async-SBIO capture**, in `drivers/sep/sep-a9.c`. No biometric
policy, no Mesa, no keybags, no enroll/match, no persistent storage, no
`/dev/sep-bio` changes.

Every rule below is tagged with where it comes from: **[wire]** = observed in
`build/sep/sbio-trace.log` or read from the kernelcache (`sbio-runtime-re.md`),
**[XNU]** = the kernelcache's own behaviour, **[defensive]** = our choice, taken
because failing closed is safer than guessing and never because the evidence
said so.

---

## 1. What changed, by function

| where | change |
|---|---|
| `drivers/sep/sep-a9.c` header comment | describes the GT frame, the single-transaction rule, the two debugfs surfaces and the async word shape |
| `SEP_GT_*` core block (marker-delimited) | new: tags, header field offsets, bounds, `struct sep_gt_hdr`, `struct sep_gt_word`, `struct sep_gt_xact`, word build/parse, header pack/unpack, `sep_gt_cap()`, `sep_gt_classify()`, `sep_gt_gate()`, the chunk state machine (`sep_gt_xact_init/stage/rx_chunk/rx_credit/rx_error`), the receive-failure classification `sep_gt_is_protocol_error()`, and the strict `xfer` line parser `sep_xfer_parse()` |
| `sep_xfer_rx()` | **removed**; replaced by `sep_gt_rx()` + `sep_gt_classify()` |
| `sep_transfer()` | **replaced** by `sep_gt_transfer()` (multi-chunk, one absolute deadline, quarantine), with `sep_gt_send_chunk()`, `sep_gt_send_credit()`, `sep_gt_wait()` |
| `sep_send_raw()` | now a thin wrapper over the single store site `sep_send_word()`, so a GT word can carry the full 32-bit command and the sequence |
| `sep_drain()` | one classifier for every received word: raw ring → wake indication → discovery → ARTS → GenericTransfer → passive async capture |
| `sep_notify_push()`, `sep_gt_stale()`, `notify` debugfs node | new: fixed-size ring of non-transport words on the selected async endpoint; stale GT words restart the quarantine |
| `xfer` debugfs | same write syntax and same first four readback lines; input is now parsed strictly (`sep_xfer_parse()`), and chunk/counter lines were added |
| `sep->last_out` | `u8[16356]` → `u8 *` of `SEP_GT_MAX_TOTAL` (devm), so the engine limit bounds a reply and not the readback buffer |
| `sep_dbg_sbio_write()`, `sep_dbg_status_read()` | `sbio_ep`/`sbio_ready` are read and written under `sep->lock` (the RX side reads them); the GT counters in `status` are snapshotted under `sep->lock` |
| `tools/sep/gt_core_test.c`, `tools/sep/test_gt_core.py` | offline suite: 17 tests and 4 static checks (see §6) |

Nothing else in the tree was touched. `patches/sep/0006` was **not** regenerated,
`/dev/sep-bio` was not changed, and no device-facing script was added.

---

## 2. Wire protocol as implemented

Copied from the trace, not inferred (report §3, §4):

* mailbox word `ep | tag<<8 | cmd<<16 | seq<<48`; the endpoint byte is real
  (0x08 for SBIO), the sequence is **per transfer and per chunk** [wire];
* seven `u32` little-endian header at `+0x00..+0x18`, payload at `+0x1c` [wire];
* tags `0xfc` opener / `0xfd` continuation / `0xfe` credit / `0xff` error [wire];
* the AP never pushes chunk *n+1* before a `0xfe`; the `0xfe` is sent with
  sequence 0 and consumes no number, and the previous send header is left in the
  OOL buffer [wire];
* `0xff` carries the raw status at `+0x10` and `op = 0`; `0x101` is reported, never
  interpreted [wire + XNU: `parseErrorStatus` does not look at anything else];
* payload capacity is `ool_len - 0x1c` **of that endpoint's published buffer**
  (`sep_gt_cap()`); 4 KiB pages give 16,356, which the trace confirms [wire].

Reply progression, added this round [wire for the values, defensive for the
check]: the first reply chunk must be `0xfc` with sequence 0, and continuations
must be `0xfd` with a strictly greater sequence. Every inbound `0xfc` in the
capture carries 0 and the single inbound `0xfd` carries 1
(`request = 0xfd, command = 0x6c, 1: 16379 = 16356 + 23`). XNU's three reply
parsers ignore the sequence, so this is a **defensive** rule. It is monotonic
rather than "+1" on purpose: a duplicate or reordered continuation is caught, a
gap is not (so a sepOS numbering replies differently is not refused), and it is
the first thing to relax if a real reply is ever refused here. The comment in
`sep_gt_xact_rx_chunk()` says so.

No `gt_ep` knob exists, and none was added (report §12.2).

---

## 3. Bounds and validation

| limit | value | where |
|---|---|---|
| request bytes | `SEP_GT_MAX_TOTAL` 65,536 (proven ≥ 19,668) [wire] | `sep_gt_transfer()` |
| reply bytes | `min(SEP_GT_MAX_TOTAL, caller buffer)` | `sep_gt_xact_rx_chunk()` |
| chunks per transaction | `SEP_GT_MAX_CHUNKS` 8, both directions | stage + rx |
| chunks per request | `in_len <= 8 * cap_send`, implied by the above | `sep_gt_transfer()` |
| transaction deadline | one absolute `SEP_GT_TIMEOUT_MS` 10,000, never restarted | `sep_gt_wait()` |
| quarantine | `SEP_GT_QUIET_MS` 100 after the **last** stale GT word (or after the failure, whichever is later), plus an empty AP→SEP FIFO | `sep_gt_gate()`, `sep_gt_stale()` |
| retries | none, anywhere | — |
| dead for this boot | 2 consecutive timeouts or 3 consecutive protocol failures | `sep_gt_transfer()` |

**Output contract** (`sep_gt_transfer()`, `sep_gt_xact_init()`): the pair
`(out, out_cap)` must be `(NULL, 0)` or `(buffer, > 0)`; anything else is
`-EINVAL` before a byte goes out [XNU: `(out != NULL) == (outlen == NULL)` is
rejected with `0xe00002c2`]. A received chunk that carries bytes with no output
buffer is `-ENOBUFS` before the copy and before `done` [XNU asserts on the same
condition after the fact].

Validation on every received chunk (`sep_gt_xact_rx_chunk()`, report §4.3), all
before anything is copied or any state says "complete":

`tag ∈ {0xfc, 0xfd}` [defensive, the driver's classifier already splits them],
`version == 1`, `flags & 0x6 == 0`, `total <= 65536`, `chunk <= total`,
`chunk <= cap`, `reserved == 0`, first chunk is `0xfc` with `seq == 0` and
`offset == 0`, continuation is `0xfd` with a strictly greater `seq` and
`offset == out_done`, `command` and `total` fixed by the opener,
`out_done + chunk <= total`, chunk-count ceiling, and a buffer for the body.

Nothing is copied into the caller's buffer until all of them pass. A rejected
header is **sticky** (`x->err`), and a word that arrives after the transaction
ended is stopped before it is even parsed (see §4).

Credits (`sep_gt_xact_rx_credit()`): a `0xfe` is accepted only in `SEND` with
request bytes still queued and no earlier credit still pending. A credit while
a reply is arriving, after the request is complete, after the transaction ended,
or a duplicate handed to us before the waiter consumed the first is `-EPROTO`
[XNU: `0xfe` becomes `buildSendChunk`, which fails `0x102` outside state 2; a
second unconsumed credit would otherwise collapse into one bool].

**One failure classification** (`sep_gt_is_protocol_error()`): `-EPROTO`,
`-EMSGSIZE`, `-ENODEV` and `-ENOBUFS` all mean "a received frame we could not
believe, or a state it broke", and all four enter the same quarantine and
`gt_proto` accounting. `-ETIMEDOUT` has its own counter, `-EIO` is the AP→SEP
store (a local transport failure, not a received frame), and `-EINVAL` is a
caller mistake before anything was sent. This exists so a later rejection path
cannot silently skip quarantine just because its local errno is not `-EPROTO`.

**Strict `xfer` input** (`sep_xfer_parse()`): `EP CMD [HEXBYTES]` where EP and
CMD are hex without a prefix or sign, and the payload is an even number of hex
digits in byte pairs with optional whitespace between them. An odd nibble, a
non-hex character, junk after the payload, an out-of-range field or an oversized
field is an error (`-EINVAL`, or `-E2BIG` when a value or the payload does not
fit), never a silently truncated request. No third field is the legal
zero-length request.

---

## 4. Locks, and why RX cannot sleep

```
cfg_lock -> { tx_lock, sep->lock }        /* no other nesting in the file */
```

* `cfg_lock` **is** the mailbox-wide GenericTransfer mutex: every transfer entry
  point (`xfer`, plus `ool`/`send`/`arts-start`) holds it for the whole
  transaction, which is what makes "exactly one transaction per mailbox" true.
  It serialises whole transactions; it is **not** the lock that protects any
  individual GT field.
* `sep->lock` owns everything RX can see: `gt_active`, `gt_ep`, the transaction
  fields RX writes (`last`, `last_msg_data`, `err`, `credit`, `want_more`,
  `status`, `done`, `out_done`, `recv*`), and the whole gate/quarantine state
  (`gt_quiet_until`, `gt_dead`, `gt_tmo`, `gt_proto`, `gt_ok`, `gt_stale`,
  `gt_stale_logged`). Writers: the RX path (already holding it) and the waiter
  (takes it at detach and when consuming an event). Readers: `status`, `xfer`,
  and the next transfer's gate.
* `tx_lock` and `sep->lock` are never taken together, and no path takes
  `cfg_lock` while holding either. `xfer`'s readback takes `cfg_lock` and then
  `sep->lock` for the counters, which is the allowed order; while it holds
  `cfg_lock` no transaction can be active, so the parts of `sep->gt` it prints
  unguarded cannot change under it.
* RX (`sep_drain()` and the functions below it) takes `sep->lock` only: no
  allocation, no sleeping, no mailbox write, no lock of its own; it invalidates,
  parses, copies the payload out (before the waiter can emit the `0xfe`, rule
  C2) and signals `gt_ev`.
* the waiter reads and clears the event flags **under `sep->lock`** and re-arms
  the completion before unlocking, so no word that arrives in between is lost.
  Two events drained in one pass are therefore seen together, and a duplicate
  credit or duplicate opener is a failure rather than a collapsed bool (§6,
  `back_to_back_events`).
* `sep_gt_wait()` waits in 2 ms slices and calls `sep_drain()` itself, so a
  multi-chunk transfer does not depend on the 10 ms poll cadence — and it can
  never wait past the one absolute deadline.

**Quarantine means quiet.** `sep_gt_stale()` restarts `gt_quiet_until` on every
stale GT word, and it runs under `sep->lock`, so the next gate test sees the new
deadline immediately. The window is therefore 100 ms after the last late word,
not 100 ms after the failure that abandoned the transaction. A GT word that
arrives for a transaction that has already completed (a full reply or a SEP
error) is routed to the same stale path (`sep_gt_rx()` checks
`sep_gt_xact_closed()` before parsing), so it cannot be followed at once by a
transfer that would race it either.

`tools/sep/test_gt_core.py` enforces the RX side statically: the core block is
compiled on the host with no kernel headers at all, `sep_gt_rx`,
`sep_notify_push` and `sep_gt_stale` are checked for allocator, sleeping and
locking tokens, `sep_gt_stale()` must restart the deadline, and
`sep_gt_transfer()` must classify receive failures through
`sep_gt_is_protocol_error()` rather than a literal `-EPROTO`.

---

## 5. debugfs surface

`xfer` keeps its exact write syntax and its first four readback lines
(`out_len=`, hex, `ep=… cmd=… ret=… sep_status=…`, `gt_reply …`), so the pending
framed `0x73` procedure and the `linux-live-test.md` expectations still apply.
Added lines: `gt_xact state=… sent=… recv=… credits=… ignored=… violations=…`,
`gt_sent`/`gt_recv` chunk lists (`tag:seq:offset:len`), and `gt_counters`.

Write errors are now specific: `-EINVAL` for a malformed line (odd nibble,
non-hex, trailing junk, bad EP/CMD), `-E2BIG` for a payload above
`SEP_GT_MAX_TOTAL` or a field that does not fit 32 bits.

`notify` (0444) is the passive ring: `t_us`, `ep`, `tag`, `len` (bits 16..31, the
16-bit value/length field) and `data` (bits 32..63, the **whole** 32-bit
info/data field, never truncated), plus `raw`. Nothing is replied, nothing is
decoded further, and no lock state is synthesised from any tag.

The handler and capture show tags 1, 2 and 3 as self-contained service words
[wire/static]. Later GenericTransfer traffic may be causally related to the
same biometric operation, but it is not a payload continuation owned by the
notification. The readback records fields only and promises no body.

**No `gt` node was added.** It would have been a second write path for the same
engine, and everything it was proposed to show (§11.6) is now in `xfer`'s
readback. If the parent prefers the separate node for the ladder's wording, it is
a rename away.

Capture requires `echo EP > sbio` first; then a non-transport word on that
endpoint is recorded. ARTS (tag 1 on the `arts` endpoint) and endpoint-0 traffic
can never be filed as SBIO async traffic: `sep_gt_classify()` filters on the
selected endpoint before the tag.

---

## 6. Offline tests

```sh
python3 tools/sep/test_gt_core.py            # clang, ASan+UBSan, no device
```

`tools/sep/test_gt_core.py` extracts the marker-delimited core from
`drivers/sep/sep-a9.c`, prepends a typedef prelude, compiles
`tools/sep/gt_core_test.c` and runs it. The tests therefore drive the shipping
code, not a model of it. Cases:

| test | covers |
|---|---|
| `word_layout` | `000000000073fc08` (21:03 request), `0000000000070102` (arts reply), `fc` seq 0 / `fd` seq 1 / `fe` seq 0 words from the capture |
| `header_and_capacity` | little-endian header bytes, `cap(0x4000) == 16356`, other buffer sizes, `chunk_bytes()` |
| `opener` | `0xfc` sequence 0, header before the doorbell, no speculative second chunk, `total = chunk = 0` reply (the `0x73` shape) |
| `zero_length_opener` | one `0xfc` with `chunk = 0` (ladder L9 shape) |
| `send_19668` | 16,356 + 3,312 at offsets 0 / 16,356, credit-gated, credit does not consume a sequence, `fc`/`fd` words |
| `recv_16379` | 16,356 + 23 at offsets 0 / 16,356, exactly one `0xfe`, payload copied out before the credit, `out_cap` boundary |
| `single_chunk_reply` | old one-chunk path unchanged (32-byte `0x2b` reply) |
| `malformed` | version, flags `0x2/0x4/0x6`, `total >` cap, `chunk > total`, `chunk >` buffer, `reserved != 0`, first-chunk offset, command echo, duplicate opener, continuation without opener, offset/total/command continuity, `progress + chunk > total`, chunk ceiling, stickiness; three rejections also assert the quarantine class |
| `error_envelope` | `0x101` and `0x102` reported raw, status 0 = complete, no resurrection afterwards |
| `classification` | async tags 1/2/3 captured only on the selected endpoint, ARTS reply not captured, no capture with nothing selected, transport tags stale when no transaction, stale for a foreign endpoint |
| `gate_and_quarantine` | `-ENODEV` dead, `-EBUSY` active, `-EBUSY` FIFO not empty, `-EAGAIN` inside the quiet window |
| `output_contract` | `(NULL, >0)` and `(buffer, 0)` refused with `-EINVAL` and not counted as a wire failure; `(NULL, 0)` + a zero-length reply is the `0x73` shape; a 16-byte body with no buffer is `-ENOBUFS`, nothing copied, not complete |
| `credit_rules` | one legal credit; duplicate before consumption; credit after the request is complete; credit in `RECV`; credit after a complete reply; credit after a SEP error — all the invalid ones `-EPROTO` |
| `back_to_back_events` | two credits in one pass (second aborts, first preserved); opener + continuation in one pass (both payloads, complete); opener + opener (second `-EPROTO`, first reply intact); credit + reply in one pass (reply wins, no lost state) |
| `reply_sequence` | first reply must be `fc`/seq 0, continuations must increase (gaps tolerated, duplicates refused), chunks under `0xfe` or a service tag are rejected |
| `protocol_class` | `-EPROTO`/`-EMSGSIZE`/`-ENODEV`/`-ENOBUFS` are the quarantine class; 0, `-ETIMEDOUT`, `-EIO`, `-EINVAL`, `-EAGAIN` are not |
| `hex_input` | live-test lines, spaced pairs, upper case, zero-length payload; odd nibble, non-hex, trailing junk, `-1`/`0x` prefix, EP/CMD range, 9-digit field, payload overflow |

Static checks in the same script: core is kernel-free; RX glue allocates nothing,
sleeps nowhere and takes no lock; exactly one `writeq()` store site and no
endpoint override knob; `sep_gt_stale()` restarts the quarantine and the transfer
uses the single protocol-failure classification.

---

## 7. Validation run

| check | result |
|---|---|
| `python3 tools/sep/test_gt_core.py` | 17/17 tests, "all checks passed"; 4 static checks pass |
| `scripts/sep/build-driver.sh` | `CC drivers/soc/apple/sep-a9.o`, no warnings |
| same with `W=1` | `CC drivers/soc/apple/sep-a9.o`, no warnings |
| `git diff --check` | clean |
| `checkpatch.pl --no-tree --file` | 0 errors, 5 warnings — identical to the pre-change file (3× `%px`, 1× `mb()` in `sep_arm`, 1× `-ENOSYS` in the `/dev/sep-bio` stub) |

---

## 8. Residual risks, stated plainly

1. **No framed `0x73` on a device yet.** The engine's first rung is the pending
   single-chunk framed `0x73`; nothing here has been observed against a real
   sepOS. The header field set, the reply offset rule and the credit protocol are
   trace-verified, but a multi-chunk exchange with this driver has never happened.
2. **A stale completion of the same command cannot be told apart on the wire.**
   The sequence is per transfer and sepOS appears not to check it, so a late
   `0xfe`/`0xfc` of an abandoned transaction is byte-identical to one of the next
   transaction for the same command. Mitigations: atomic detach under
   `sep->lock`, a full drain before and after, a 100 ms quiet window that
   restarts on every stale word, an empty FIFO requirement, and per-transfer
   state that a stale word cannot satisfy while no transaction is installed.
   After the quiet window the residual risk remains and is why the policy is
   "record and stop", not "retry".
3. **The reply sequence rule is defensive, not evidence-backed.** XNU ignores
   `seq` on replies; the trace (one `fd`, sequence 1) is consistent with "the
   first reply is 0 and continuations increase" but that is one sample. The
   check is monotonic rather than "+1" for exactly that reason. If a real reply
   is refused with `-EPROTO` and `gt_reply … tag=fd seq=N`, the check in
   `sep_gt_xact_rx_chunk()` is the one to relax — from a log, not pre-emptively.
   Same for the `tag ∈ {0xfc, 0xfd}` guard, which the classifier already makes
   unreachable.
4. **`reserved != 0` on a chunk reply is rejected** (`-EPROTO`). This is stricter
   than XNU, which does not examine the field. It is deliberate (a status word
   outside a `0xff` envelope means we mis-parsed something), but it is the one
   *shape* rule that could reject a real reply; relaxing it is a two-line change.
5. **The request ceiling 65,536 is self-imposed** (report §12.4): XNU's
   `gt+0x18` is unknown, 19,668 is only a proven lower bound.
6. **Invalid credits now abort instead of being dropped and counted.** That is
   the XNU behaviour (`0x102` → fail) and it is what stops a duplicate from
   collapsing into a bool, but it means a SEP that emits one spurious `0xfe`
   costs a whole transaction plus a quarantine window instead of being ignored.
   The counters in `xfer` (`credits`, `violations`, `gt_proto`) are what a log
   needs to tell the two apart.
7. **Both OOL buffers are wiped after every transfer**, as XNU does. A
   GenericTransfer on the `arts` endpoint therefore discards a staged IM4M; the
   two paths are not meant to be used together.
8. **`xfer`'s reply buffer is 65,536 bytes end to end**, so debugfs can exercise
   any reply the engine accepts; the readback is a 128 KiB `kvzalloc` per read,
   which is fine for a debug surface but is not a userspace ABI.
9. **Poll latency**: a transfer completes as fast as the mailbox is drained,
   which the engine now does itself in 2 ms slices; the 10 ms delayed work
   remains as the idle path.
10. **`xfer`'s parser is stricter than before.** A line that used to be silently
    truncated (`8 73 010`, `8 73 01zz`) is now an error, and `-E2BIG` replaces
    the old length `-EINVAL`. Every syntax the live-test plan documents still
    works (`8 73 01000000`).
11. **`/dev/sep-bio` is deliberately unchanged**: no `.poll`, no `.release`, no
    single-open, no new ioctl. That belongs to the fprint integration stream.
