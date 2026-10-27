# SKS lock state and IOSEPBiometricService matching: kernelcache RE

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Workstream research note, 2026-10-26. **Rev 4 (second adversarial review).**
Rev 2 proposed a standalone `0x23` probe as the next device step; rev 3
retracted it; **rev 4 removes the standalone probe from the prerequisites and
the plan**, so the retraction holds everywhere rather than only in the ledger.
Rev 4 also
corrects the `0x23` census (20 transactions, not the earlier count), the
AlignmentData/`0x08` wording (correlation, not pairing), the catacomb chain
against
`enrollment-storage-re.md`, the `handleSEPMessage` anchor and frame size, and
the service-word field names, and adds the implementation contract in §14. The
next device step is the already-built framed `0x73` test (`a9/tri53-sep7`);
the only new work proposed here is a passive, no-send async-notification
window after it.

Scope: the advertised **`sks ` endpoint (ep 0x12)** and its relationship to
biometric matching on this A9 / iOS 16.7.10 kernelcache: how XNU/AppleSEPKeyStore
establishes user/keybag/lock state, what messages and OOL setup occur, which
SBIO opcodes consume or query that state (including observed `0x23`), what
Linux can represent without Apple keybags, the minimum honest state for a
read-only no-template match and a later real match, the exact payload layouts
recovered so far, and the corrected implementation ordering (§8–§9, §14).

Evidence: static RE of `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`
(shared Ghidra project, functions renamed there per this analysis), cross-checked
line-by-line against the live os_log capture `build/sep/sbio-trace.log`
(12,965 lines, first enrolment + 5 unlocks, 2026-10-24) and the live Linux
endpoint advertisement in `docs/sep/logs/tri53-sep6-combined-20261026-2021/`.
Every "verified" claim below has both a disassembly anchor and (where marked) a
matching live-trace line. This note proposes no device transmits at all: §11
references the existing `0x73` rung and documents a read-only observation
window; nothing here touched the iPad.

Confidence legend: **[V]** = verified (code + live trace), **[V-code]** =
verified from code only, **[I]** = strong inference, **[U]** = unknown.

## 0. Correction ledgers

Rev 3 audited every rev-2 claim against fresh disassembly, the live trace, and
the two sibling workstreams (`sbio-runtime-re.md` rev 4, `enrollment-storage-re.md`).
Corrections, with the evidence that decided each:

| # | rev-2 claim | rev-3 correction | evidence |
|---|---|---|---|
| C1 | a standalone `0x23` is "the next single-message probe" and its outcome decides whether keybag-less matching is unblocked (rev-2 §8.4, §11) | **retracted as a diagnostic.** `0x23` has exactly one producer in the bounded scan described in §5; in the trace all 20 transactions sit between the `kImageProcessingInit` log line and `0x06`/`0x65`, after an active EnrollMode/MatchMode and a captured sensor frame. Idle-`0x23` semantics are unknown; success would not prove keybag-less match is unblocked and rejection could mean wrong state/order, not a keybag prerequisite | bounded code scan (rev-3 pass); trace census (20× `sepTransact <- command:0x23`, every one preceded by `asyncCaptureHandler: kImageProcessingInit`) |
| C2 | next experiment ordering: `0x73`, then `0x23` probe, `0x2b` in reserved boots | **framed `0x73` on `a9/tri53-sep7` first, one command, fresh boot; if it succeeds, a passive async-notification window (60 s, record only)**. `0x23` is not a rung at all (rev 4, §5/§11); `0x2b` belongs to the SBIO ladder's later boot, not to this plan | `sbio-runtime-re.md` rev 4 §12.1 (the 21:03 request had no GT header — it was never well framed; `0x101` is consistent with malformed framing), §13 L3/L6; cross-workstream instruction |
| C3 | "the SBIO endpoint alone transports everything matching needs for lock state … the `sks ` endpoint becomes a Linux dependency only for keybag option B, not for read-only matching" (§1.5) | **downgraded to architectural separation.** Two transports and two field domains are code fact; whether sepOS *pushes* kind-1 or permits match when the AP never publishes/services `sks ` is **not** established by the code and stays an open prerequisite test (the §11 passive window answers the push half; the sks-servicing half needs its own later test) | §3/§4 establish the transports only; no code path proves the push/permission behavior of a sks-less AP |
| C4 | the iOS enrollment "lives in SEP-internal storage" and "may have survived the repartition" (§8.6) | **corrected.** Catacomb persistence is AP-hosted: `/var/mobile/Library/Catacomb`, exported by `0x6c` and restored via `0x55`/`0x6d`; sepOS holds only a per-boot working copy, and a cold boot needs a host restore. If the iOS data volume is gone (repartitioned), the blob is gone with it; on every fresh tethered Linux boot sepOS starts with no restored catacomb unless the host restores one | `enrollment-storage-re.md` §2, §5.2–§5.4 (libBKDM1 `loadCatacomb` → host cmd 5 = SEP `0x55` at boot) |
| C5 | "the catacomb state machine for templates (`0x6c`/`0x33?`/`0x37`/`0x36`) and userspace `catacomb_state_v1_t` storage" (§9.4) | **corrected everywhere.** `0x33` is **GetCatacombId** (4→16); **CompleteSaveCatacomb (host 62) performs no SEP transfer at all**; `catacomb_state_v1_t` is the SEP-side record, the AP file is an NSKeyedArchiver container with a legacy `CAT1` decoder. Save order: Prepare `0x6c` → durable host write + fsync → Confirm `0x37` → `0x33` → `0x36` | `performSpecificCommandGated` cases `0x3d/0x3e/0x3f/0x38/0x3a` (re-decompiled in rev 3); `enrollment-storage-re.md` §5.1–§5.3 |
| C6 | "It [0x23] appeared ~64 times in the live capture" (§5) | **20 transactions** (80 log lines over four logging layers: 20 `sepTransact`, 20 GT `transact`, 20 `sendRawMessage`, 20 `sepMessageHandler`); 8 enrolment frames (`matchFlags=0`), 6 auto-match frames (`matchFlags=2`), 6 match/unlock frames (`matchFlags=9`) | trace census (rev-3 and rev-4 passes) |
| C7 | `handleSEPMessage` "at 0xfffffff0066ae498 (function start)" (§3.1/§3.2/§13) | **the function starts at 0xfffffff0066ae4d8** (`sub sp,sp,#0x80` prologue, `stp` chain; `this`=x19 from x0, `msg`=x20 from x1). `0xfffffff0066ae498` is the `ret` of the preceding function's epilogue; the os_log cold block at 0x66ae49c belongs to that function and branches back. Ghidra has no function defined at either address; comments were placed, **not saved** (pending orchestrator save, see §13) | re-disassembled `0x66ae480..0x66ae540`: `ret` at 0x66ae498 (`d65f03c0`), prologue at 0x66ae4d8 (`d10203ff` + `stp` chain) |
| C8 | the kind-1 byte-exact example paired `sepMessageHandler: request = 0x1, command = 0x1f50208` with `handleSEPMessage: length = 4, info = 501` (§3.1) | **two different messages were merged.** The 12:10:21.962 pair is `command = 0x1f50004` (line 3090) → `length = 4` (line 3091); `0x1f50208` is the 12:10:49 message whose word is `0x000001f5_0208_0108` (length/lockState 0x208). Correct example below | trace lines 3090/3091 vs 6946/6947 |
| C9 | "In mailbox fields: `ep=0x08`, `tag=kind`, `op\|param = value16 (LE u16 in bits 16..31)`, `data = info32`" (§3.1) | **service words must not be described with the GT `op\|param` field table.** Per the handler itself, bits 16..31 are one u16 (logged "length", extracted `ubfx x8,x20,#16,#16`) and bits 32..63 are one **32-bit** info field (logged "info", extracted `ubfx x21,x20,#32,#32`); every observed value fits in 16 bits, which is an observation, not a narrower field. The kind-1 path *interprets* length as `lockState` and info as `userID`. Both the raw layout and the interpretation are now stated | `sbio-runtime-re.md` rev 4 §0.2 R7, §9; decode sites 0x66ae50c (info32), 0x66ae510 (tag), 0x66ae514 (value16) — re-disassembled in the rev-4 pass |
| C10 | "`0x06` … userID at offset `+0x67` (from this+0x1d4c, default 501)"; MatchMode "identity/UUID block at payload `+0x14/+0x24`" | **kept, with exact anchors and bases.** `stur w8,[x24,#0x67]` at 0x66a0e5c; the default applies to **any userID < 1** (`cmp w8,#1; csel w8,501`), not only `-1`. Identity offsets are in the *original userspace payload* for both forms (0x25: sel`+0xc`, len`+0x10`, data`+0x14`; 0x27: sel`+0x1c`, len`+0x20`, data`+0x24`), and the 0x27 wire message **starts at payload+8**. New [V-code] fact: the 0x27 branch requires `payload+8 == this+0x1d4c` (current userID) or refuses | raw disassembly 0x66a0e44..0x66a0e78; `matchInit` 0x66a8fd0 decompile |
| C11 | "Mesa `AlignmentData (8)` pairs with SEP opcode `0x08`" (§5 correction note for sbio-commands.md) | **refined: AlignmentData (8) performs no SEP transfer.** Case 8 memcpy's 3060 bytes from the driver cache (`this+0x3b8`) and copies them out; the cache is filled by the earlier SEP `0x08` result fetch (0 in / 3224 out) in the async result path — a correlation, not a pairing. Cross-workstream note: the `0x54` in `sbio-runtime-re.md` §8.3 is emitted by Mesa case 0x11 (**host 17**, RequestTopologyOfTemplate), not by AlignmentData | decompile of case 8 (`memcpy(&local_c70, this+0x3b8, 0xbf4)`) and case 0x11 (vtable+0x698, `0x54`); trace: 8× AlignmentData all answered locally, 8× SEP 0x08 (3224) precede them, 1× SEP 0x54 (20→3060) at 12:10:22 |
| C12 | "GetSKSLockState … default `0x15` … Mesa ORs bit `0x4`" presented without the fail-closed caveat | kept (all verified, case 0x27 at 0x66a5e60-area incl. flag at `this+0x2af7`), **plus** the explicit rule: no kind-1 push means *unknown/locked*; the local `0x15` default is an AP-side compatibility value, never evidence that sepOS authorizes match (§7) | case 0x27 decompile; cross-workstream instruction |
| C13 | duplicated paragraph in §6.1 (per review) | **no verbatim duplicated paragraph exists in the current working tree** (likely referring to the §6.1 intro restating the advertisement that item 3 also restates); the redundancy was consolidated into the intro sentence | rev-3 diff-scan of the file |

Rev 3 also re-audited the shared Ghidra renames listed in §13 against the
decompilation: **all are supported by the code** (sksLockStateChangeHandler
re-arms + drains; sksQueueProcess calls Dequeue/getUserState/updateUserOperation
a/b/c; queue chain thunks match `handleSEPMessage`'s bl targets). One
disassembly-comment correction was applied (C7); it is placed in the shared
project but deliberately **not saved** — the orchestrator or the next review
round should save it.

### 0.1 Rev-4 ledger: contradictions this round had to fix

Rev 3's ledger was correct but several later sections still carried the
pre-correction text, which is what the parent review found. Each row is a
contradiction, its fix, and the section that now carries the fixed text.

| # | rev-3 inconsistency | rev-4 fix | where |
|---|---|---|---|
| R1 | C1/C2 retract standalone `0x23`, but §8 item 4 ("one gate probe … decides") and all of §11 still prescribed it | removed. §8 no longer contains a gate probe (its new-work item is the passive window); §11 prescribes no `0x23`, no `0x2b`, and no transmit at all | §8, §11 |
| R2 | C6 says 20 transactions; §5 still said `~64` | 20, with the layer-by-layer census and the 8/6/6 frame-class split | §5 |
| R3 | C11 says AlignmentData is local; §5 still said it "pairs with SEP opcode `0x08`" | local copy from `this+0x3b8`, correlated with an earlier separate `0x08` result fetch; no transfer in case 8 | §5 |
| R4 | C4/C5 corrected AP-hosted catacomb, but §8 item 6 and §9 item 4 still said SEP-internal survival, `0x33?`, and userspace `catacomb_state_v1_t` | rewritten to the `enrollment-storage-re.md` chain: `0x6c` → durable AP write/fsync → `0x37` → `0x33` → `0x36`; host 62 has no SEP opcode; restore `0x55`/`0x6d`; sepOS keeps a per-boot copy | §8, §9 |
| R5 | C7 corrected the start, but §3.2 and §13 still labelled `handleSEPMessage` at 0x66ae498; C7's own prologue size was wrong | 0x66ae4d8 is the start, 0x66ae498 the preceding `ret`, and the frame is `#0x80` | §3.1, §3.2, §13 |
| R6 | C9 forbids GT-style field names, but the §3.1 example and §10 table still used `op\|param`/`data`; C9 also narrowed info to u16 | `value16`/`info32` everywhere; `info32` is a 32-bit field whose observed values fit in 16 bits | §3.1, §10 |
| R7 | no normative statement of what the current driver may do | implementation contract added | §14 |
| R8 | `[V]` on naming/inference claims, and "exhaustive" language on a bounded scan | relabelled: observations stay `[V]`, naming and causality are `[I]`, negative code scans state their address range | throughout |

Bounded claims, restated: §5's producer search covers
`0xfffffff006668000..0xfffffff0066c2000` in this kernelcache only. It says
nothing about sepOS (encrypted) or about kexts outside that range.

## 1. Executive result

1. **The per-user lock state that `IOSEPBiometricService` tracks arrives on the
   SBIO endpoint (ep 0x08), not on `sks `.** sepOS pushes it as unsolicited
   non-GT messages; `AppleMesaSEPDriver::handleSEPMessage` turns a kind-1 word
   into an `IOSEPBiometricService` queue entry. **[V]** for the transport and
   the decode (code + trace). The reading that the `sksQueue*` method names
   refer to SEP keystore *state* rather than the `sks ` endpoint *transport* is
   **[I]**: it is a name, not a call edge.
2. The `sks ` endpoint (ep 0x12) is served by the **AppleSEPKeyStore kext**
   (`com.apple.driver.AppleSEPKeyStore`): volume/system keybag operations and
   per-handle lock-state notifications; it builds the vfs notification for the
   volume uuid. Its lock-event struct carries a 32-bit *handle*
   (`state = tag & 0x7f`, `value`, `flags`) and **no userID field**.
   **[V-code]**; the live log order is a **[V]** observation, and attributing
   both paths to one unlock is **[I]**.
3. **SBIO `0x23` has one producer in the bounded Mesa range and is per-frame,
   inside an active pipeline.** The scan
   (`0xfffffff006668000..0xfffffff0066c2000`, every `movz w1,#0x23` followed by
   a virtual `sepTransact`) finds a single emit site,
   `0xfffffff0066a1110` inside `asyncCaptureHandler`; all **20** transactions
   in the capture occur after an active EnrollMode/MatchMode and a captured
   sensor frame, immediately before the `0x06`/`0x65` pair, zero-in and
   zero-out, and a nonzero return aborts the frame (branch at `0x66a1134` →
   error path `0x66a1548`). **[V-code]** for the single producer, **[V]** for
   the 20-transaction trace and its placement, **[U]** for what sepOS does
   with it. There is no idle occurrence in the scanned code or the capture, so
   a standalone send has no reference behaviour and is not a diagnostic
   (§5, §11).
4. The only AP-side "query" of lock state, Mesa command **`GetSKSLockState`
   (39)**, is answered **entirely locally** (no SEP message): input is a
   4-byte userID, output a 4-byte cached lockState, default `0x15` when no
   per-user state exists, and the Mesa driver ORs in mask `0x4` (bit 2) when
   its enabled flag is set. **[V]** (case `0x27` of
   `performSpecificCommandGated`; flag at `this+0x2af7`).
5. **The two transports are architecturally separate — two endpoints, two
   field domains, per-user vs per-handle — and the per-user state Linux would
   need is the one observed on SBIO.** Separation is code fact (**[V-code]**).
   What is *not* established is any Linux prerequisite: whether sepOS will
   push kind-1 messages, or permit matching at all, when the AP never
   publishes/services the `sks ` endpoint is untested (**[U]**). The passive
   window in §11 can observe the push half on hardware; it cannot answer the
   permission half. Keybag option B in `docs/sep/keybag.md` is a separate
   workstream; nothing here decides whether matching needs it.

**Explicitly not claimed.** This note does not claim that `sks ` is required
for matching, nor that it is unnecessary. It claims (a) the transports and
field domains are separate, (b) the only observed per-user push is on SBIO,
(c) sepOS's prerequisite behaviour toward an AP that does not service `sks `
is unknown. Linux failing closed when no authoritative per-user state has been
observed is a defensive policy (§14), not a statement about sepOS.

## 2. Class layout (needed to read the rest)

`AppleMesaSEPDriver` **subclasses `IOSEPBiometricService`** (both classes live
in the IOBiometricFamily kext): `handleSEPMessage` is in
`AppleMesaSEPDriver.cpp` yet calls the `sksQueue*` methods with its own
`this`, and `performSpecificCommandGated` passes the same object to
`IOSEPBiometricService::getUserState`. **[V-code]**

Key object fields used below (one object, the Mesa driver instance):

| field | meaning |
|---|---|
| `+0xb0..+0x150` | array of 20 per-user state pointers |
| `+0x158` | biometric operation mutex (10 s) |
| `+0x178` | `IOTimerEventSource` for the sks queue drain |
| `+0x180..+0x1bc` | 10-entry ring of `{u32 userID; u16 lockState}` (6 B/entry) |
| `+0x1d0` / `+0x1d4` | ring count / ring lock |
| `+0x1d48` | u32 match/enroll **command-mode word** (word 0 of the MatchMode payload) |
| `+0x1d4c` | i32 current userID (any value < 1 → substituted 501/`0x1f5` in the `0x06` build; the 0x27 MatchMode form requires the payload's word 1 to equal this field) |
| `+0x2af7` | byte flag; when set, `GetSKSLockState` ORs `0x4` into the result |
| `+0x1e24`, `+0x2c08` | kind-3 async event flag / wake object |

Per-user state object: `+0x14` u32 userID, `+0x18` u16 lockState, `+0x1a`
cached bit 0, `+0x10` u16 written by async kind-2, `+0x3a` pending-transition
flag, `+0x28`/`+0x30` operation pointers.

## 3. Verified: the real lock-state transport (SBIO async messages)

### 3.1 Wire format

`AppleMesaSEPDriver::handleSEPMessage` — **true function start
0xfffffff0066ae4d8** (prologue `sub sp,sp,#0x80`, `stp` chain; `this`=x19 from
x0, `msg`=x20 from x1; 0xfffffff0066ae498 is the `ret` of the preceding
function's epilogue, and the os_log block that follows it belongs to that
function — §0 C7. Ghidra has no function defined at either address; comments
were placed, see §13) — decodes the 64-bit SEP→AP mailbox word as three
fields and logs
`"%s: Got message from SEP, request = %d, length = %d, info = %d"`:

```c
kind    = (msg >>  8) & 0xff    ; logged "request"  /* ubfx w23,w20,#8,#8   @ 0x66ae510 */
value16 = (msg >> 16) & 0xffff  ; logged "length"   /* ubfx x8,x20,#16,#16  @ 0x66ae514 */
info32  = (msg >> 32)           ; logged "info"     /* ubfx x21,x20,#32,#32 @ 0x66ae50c */
```

The address comments are the extraction sites, not the instruction order: the
function extracts info32, then the tag, then value16. `info32` is a full
32-bit field in the code; every value observed in the capture fits in 16 bits
(501, 1, 0, and the 512/520/521/552 state words), which is an observation
about these messages, not a narrower struct.

These are **not** GenericTransfer completions (those use tags `0xfc..0xff` on
the same endpoint; the GT-vs-service split happens in
`AppleSEPGenericTransfer::sepMessageHandler` `0xfffffff00668fda4`, per
`sbio-runtime-re.md` §2) — they are unsolicited SBIO *service* notifications.
The physical endpoint byte `0x08` is correct: the provider splices it from
`AppleSEPDeviceService+0x60` before the mailbox write
(`sbio-runtime-re.md` §3.1/§12.2).

**Raw layout and interpretation, stated separately (§0 C9).** On the wire a
service word is `ep (bits 0..7) | tag (bits 8..15) | value16 (bits 16..31) |
info32 (bits 32..63)`. This is *not* the GT word shape (a 32-bit command in
bits 16..47 plus a 16-bit chunk sequence in 48..63), and the two field tables
must not be mixed. The Mesa handler logs `value16` as "length"; the kind-1
path **interprets** it as `lockState` and `info32` as `userID` (kind 2: as the
`user->+0x10` flag value; kind 3: unused, observed 0). Capture census: 12
kind-1, 3 kind-2, 5 kind-3 words. Bits 48..63 are not printed by the kext; the
raw words examined in `sbio-runtime-re.md` §9 had them zero, and no AP log
line reports them, so treat "upper half zero" as observed-so-far, not
guaranteed.

| kind | meaning | handling (all verified from code + trace) |
|---:|---|---|
| 1 | SKS user lock-state change | `{u32 userID = info32; u16 lockState = value16}` → `sksQueueEnqueue` → `sksQueueProcessAsync` |
| 2 | per-user flag update | `getUserState(info32, create=1)` → `user->u16@+0x10 = value16` |
| 3 | operation event | `[this+0x1e24] = 1` + wake calls on `[this+0x2c08]` (vtable `+0xb0()`, `+0xf0(0)`) |

Field-exact live example (enrollment complete, 12:10:21.96):
`sepMessageHandler: Message from SEP, request = 0x1, command = 0x1f50004`
→ `handleSEPMessage: request = 1, length = 4, info = 501` → reconstructed word
`0x000001f5_0004_0108` = `ep=0x08, tag=0x01, value16=0x0004, info32=501`
(kind-1 path: lockState `0x4`, userID `501`). **[V]** for the fields; the AP
log does not dump the raw mailbox word, so this is a field-exact
reconstruction, not a byte capture. The GT layer's `command = 0x1f50004` is
exactly `info32[15:0] << 16 | value16` — the line that invited the earlier
op/param misreading.

### 3.2 Queue and drain chain

```
handleSEPMessage (kind 1), start 0xfffffff0066ae4d8
  ├─ sksQueueEnqueue(this, {userID, lockState})  0xfffffff00669dad4  (thunk 0xfffffff0066bc1cc)
  └─ sksQueueProcessAsync(this)                  0xfffffff00669e3e0  (thunk 0xfffffff0066bc1e4)
        re-arms the timer at this+0x178 (setTimeout 0-equivalents, vtable +0xb0/+0xf0)
             ↓ workloop
     sksLockStateChangeHandler(this, sender)     0xfffffff00669e49c  (timer action; was
        timer->vtable+0xb8();                    mis-renamed sksQueueProcessAsync in the
        sksQueueProcess(this)                    shared project — corrected + saved)
             ↓
     sksQueueProcess                             0xfffffff00669e098  (stub 0xfffffff0066bc1d8)
       loop: sksQueueDequeue                     0xfffffff00669ddf4
         entry {u32 userID; u16 lockState}
         user = getUserState(this, userID, create=1)   0xfffffff0066946a0
         if ((new & 0xc) == 4 && (old_user->lockState & 0xc) == 8):
             kind = user->+0x3a ? 2 (and clear it) : 3
             messageClients(0xe3ff8200, {u32 userID; u32 kind}, 8)
         user->lockState(+0x18) = new
         if (new & 0x20): updateUserOperation_a(1), _b(1), vtable+0x630
         always: updateUserOperation_a/b(0), _c(0), user->+0x1a = new & 1,
                 vtable+0x620, +0x628
```

`AppleBiometricServices::message` (the user-client service) receives type
`0xe3ff8200` (= 3825172480 decimal in the trace) with the 8-byte
`{userID, kind}` argument and forwards to biometrickitd, whose
`BiometricKitDStatistics lockStateUpdated` logs the same values. **[V]**

### 3.3 lockState values observed and bit usage

Live sequence during first enrolment + unlocks (kernel and userspace agree):

| time (12:) | value | bits | event |
|---|---:|---|---|
| — | `0x15` | 2,4,0 | code default (§4) when no per-user state exists |
| 10:21.962 | `0x4` | 2 | first kind-1 push, SBIO/Mesa coming up |
| 10:21.963 | `0x200` | 9 | second kind-1 push |
| 10:49.177 | `0x208` | 3,9 | after the system volume unlock (AppleSEPKeyStore action 1) |
| 10:50.475 | `0x228` | 3,5,9 | after unlock completes |
| 11:25.578 | `0x209` | 0,3,9 | later unlock cycle |

The "event" column is the AP-log order in this capture (the first row is the
code default, not a push); the attribution of a given push to the immediately
preceding volume/keybag action is a timing correlation (**[I]**), not a traced
causal edge. What is **[V]** is the value, the sequence, and the downstream
queue/broadcast behaviour.

Verified code usage of the 16-bit mask: bits `[3:2]` are a 2-bit sub-state
(transition `10`→`01` fires the userspace broadcast); **bit 3 (mask `0x8`)**
and **bit 9 (mask `0x200`)** are explicitly tested in the operation-update
paths (`updateUserOperation_b`/`_c`, 0xfffffff006694cec / 0xfffffff006694e54)
to decide whether match/enroll operation objects stay attached; **bit 5
(mask `0x20`)** triggers the operation-cleanup calls; **bit 0** is cached at
user `+0x1a`; the default with no user is `0x15`; Mesa ORs mask `0x4`
(bit 2) from its enabled flag. **[V-code]** Names for the bits are
sepOS-internal and not recoverable from the AP cache: the working reading
(biometric-unlock state pair `[3:2]`, "biometrics/SBIO enabled" bit 9,
"operation pending" bit 5) is **[I]**.

## 4. Verified: `GetSKSLockState` (Mesa 39) is local, never a SEP message

`AppleMesaSEPDriver::performSpecificCommandGated` **0xfffffff0066a504c**, case
`0x27` (Mesa command 39, `inLength:4, outLength:4` — both in the live trace):

```c
if (inLen < 4) return kIOReturnBadArgument;
sksQueueProcess(this);                      // drain pending pushes first
user = getUserState(this, *(u32 *)in, /*create=*/0);
lockState = user ? getUserLockState(user)   // u16 at user+0x18
                 : 0x15;                    // default when no user state
if (this->flag_at_0x2af7) lockState |= 4;
copy_out(&lockState, 4);
```

No `sepTransact` call is made anywhere in the case — which matches the live
trace, where every `GetSKSLockState (39)` line has `inLength:4, outLength:4`
and is never followed by a `sepTransact`. Userspace (biometrickitd) polls it
repeatedly (~every 1–4 s). The input word is the **userID** (501 in the
capture, from the calling context; the log does not print it). **[V]** for
the transfer shape, **[V-code]** for the field meaning.

## 5. Verified: `0x23` and the per-frame pipeline (placement only)

`AppleMesaSEPDriver::asyncCaptureHandler` **0xfffffff0066a09b0** (comment
placed; function undefined in Ghidra) drives each sensor frame:

```
per capture event:  sepTransact(0x22, in=0, out=0)          poll for async SEP message data
per frame:          sepTransact(0x23, in=0, out=0, lock=0)   observed here only; semantics [U]
                    sepTransact(0x06, in=payload, 0x97/0xa7) image-processing init
                    sepTransact(0x65, in=19668, chunked)     relay of the encrypted frame
async results:      0x07 / 0x08 fetch, 0x22 poll, 0x09/0x21 pushes, Cancel 0x0b
```

- `0x23` is emitted at **0xfffffff0066a1110** (`mov w1, #0x23` then the
  `sepTransact` virtual at vtable `+0x6a0`, all other args zero). Failure
  aborts the frame (branch to the error path at 0x66a1548). **[V-code]**
- Census: **20 transactions** in the capture — 80 log lines over four layers
  (20 `sepTransact`, 20 GT `transact`, 20 `sendRawMessage`, 20
  `sepMessageHandler`). By frame class: **8** enrolment frames
  (`matchFlags=0`, after the single EnrollMode `0x03` at 12:10:08), **6**
  auto-match frames (`matchFlags=2`, `_commandMode:2`, 12:10:27–36), **6**
  match/unlock frames (`matchFlags=9`, after the only MatchMode `0x25` at
  12:10:51). Every one is zero-in/zero-out, `lock:0`, and sits between
  `kImageProcessingInit` and the `0x06`/`0x65` pair. **[V]**
- **What sepOS does with it is unknown.** There is no idle or standalone
  occurrence in the code or the capture, so a standalone send has no
  reference behaviour to compare against. It cannot decide keybag presence,
  user state, or match permission, and this note does not propose one
  (§0 R1, §11).
- The `0x06` payload is built in the same function: length `0x97` (151) or
  `0xa7` (167) = `0x97 + identity_len` (16); word 0 = the **command-mode word
  from MatchMode** (this+0x1d48); byte at `+0x4d` from the image object; and
  the **userID at offset `+0x67`** (from this+0x1d4c, default 501). **[V-code]**
- `matchFlags` in the `kImageProcessingInit` log line (observed 0/2/9) are the
  low bits of that same mode word; the assert string
  `_matchFlagsACMContextLength` at 0xfffffff0066a1594 additionally shows the
  flags tie into an AppleSEPCredentialManager (`scrd`) context length — a
  cross-endpoint lead only. **[I]**

### MatchMode (`0x25`) and its alternate form (`0x27`)

`AppleMesaSEPDriver_matchInit` **0xfffffff0066a8fd0**: stores payload word 0
as the command-mode word, then:

- mode bit 1 == 0 → `sepTransact(0x25, payload, 0x44 /*68*/, out=0, lock=1)`
  — the observed MatchMode; identity selector/length/data at userspace
  payload `+0xc`/`+0x10`/`+0x14`;
- mode bit 1 == 1 → `sepTransact(0x27, payload+8, 0x3c /*60*/, out=0, lock=1)`
  — the alternate form: the wire message starts at payload+8, so the
  identity selector/length/data at `+0x1c`/`+0x20`/`+0x24` in the userspace
  payload are wire `+0x14`/`+0x18`/`+0x1c`. This branch also refuses unless
  `payload+8` (word 1) equals `this+0x1d4c` (current userID). **[V-code]**

The 68-byte payload itself is built by **userspace (biometrickitd /
libBKDM1.dylib)**, as `docs/sep/sbio-commands.md` Gap 1 already concluded; the
kernel only interprets word 0 (mode bits) and the identity-list length/offset
fields. `lock:1` on `0x25`/`0x21` vs `lock:0` elsewhere is observed; its
precise meaning inside sepTransact is **[U]**.

One correction for `docs/sep/sbio-commands.md`: Mesa `AlignmentData (8)`
performs **no SEP transfer**. Case 8 memcpy's 3060 bytes out of the driver
cache (`this+0x3b8`); that cache was filled by an earlier, separate SEP `0x08`
result fetch (0 in / 3224 out) in the async result path. They are
**correlated in time, not paired** — 8× AlignmentData were answered locally
with 8× `0x08` fetches preceding them, and the (0x54, 20→3060) at 12:10:22 is
emitted by Mesa case 0x11 (host 17, RequestTopologyOfTemplate), not by
AlignmentData. `RequestMessageData (43)` = `0x22` poll (0/0) + `0x07` fetch
(0/N); the Mesa-level `inLength:8` is the userspace selector, not the SEP
input. **[V]**

## 6. Verified: the `sks ` endpoint (AppleSEPKeyStore)

### 6.1 Advertisement and OOL setup

Live (Linux, tri53-sep6 combined run): `ep 0x12 'sks '`, advertised 4–8 send
pages and 4–8 recv pages, `max_send=0x8000 max_recv=0x8000` — the same
maximum as `xarm`/`xars`, twice `sbio`'s `0x4000`. **[V]**
`AppleSEPKeyStore_initSksEndpoint`
**0xfffffff0064b35e0**:

1. refuses to run if the SEP is not booted ("SEP is not booted. AKS request
   is not processed");
2. `nameMatching("sep-endpoint,sks")` + `waitForMatchingService`;
3. reads `getOOLInMaxPages()` / `getOOLPageSize()` / `getOOLOutMaxPages()`
   from the endpoint service (i.e. the `0xfd` advertisement values the Linux
   driver already parses);
4. creates the endpoint's `AppleSEPGenericTransfer` with a receive callback at
   **0xfffffff0064b39bc**;
5. allocates and publishes the OOL pair sized `in_pages*page_size` and
   `out_pages*page_size` through the standard manager control opcodes
   (0x04/0x02 send, 0x05/0x03 recv per `docs/sep/driver-notes.md` §2).
   **[V-code]**, like every step above; only the advertisement itself is
   **[V]** from the Linux run.

### 6.2 Receive path

The callback at **0xfffffff0064b39bc** dispatches on the message's tag byte
(bit 7 set → AP→SEP request completion dispatcher; `3`/`6`/`7` → special
handlers; **default** → builds a 32-byte event from a const template at
0xfffffff005616440 — `u32 kind=1` at `+0`, the raw 8-byte message word at
`+8` — and enqueues it).

The queue is an **`IOSharedDataQueue::withEntries(30, 0x20)`**
(0xfffffff0064d8290) at AppleSEPKeyStore `+0x1a0`, fed by
`AppleSEPKeyStore_enqueueEvent` **0xfffffff0064aa470** (drops the oldest on
full, `thread_call_enter` on the pump). The pump
(`AppleSEPKeyStore_messagePump` **0xfffffff0064a9ab0**, a thread call created
in init) dequeues 0x20-byte events and calls
`AppleSEPKeyStore_processQueuedEvent` **0xfffffff0064b3f50**:

- `kind==1` (the default case above): `lockStateChangeDispatch(this,
  handle = msg.data32, state = msg.tag & 0x7f, value = msg.byte2, flags =
  msg.byte3)`;
- `kind==4`: `_cp_key_store_action_for_volume(uuid@+0xc, action@+0x8)` +
  "Sent notification for volume %s (action %d)".

`lockStateChangeDispatch` **0xfffffff0064a9248** (logs "Sending lock change %d
for handle %d value %d"): case 0/1 (lock/unlock) can call the vfs
notification builder `queueVfsNotification` **0xfffffff0064aa270** — the
"%s**ock** notification for volume (uuid: %s, handle %d)" string is the
"Unl/L"ock notification in the trace — and broadcasts IOService messages
`0xe3ffd00f` / `0xe3ffd00d` (fall-through) / case 8 `0xe3ffd00c` to kernel
listeners, with the 4-byte handle as argument. **[V-code]**

Live sequence (12:10:47.91 → 49.18): the AppleSEPKeyStore receive path logs
"Unlock notification for volume (uuid: `apfs-uuid-…@volume`, handle 0)" →
"Sending lock change 0 for handle 0 value 0" → "Sent notification for volume
(action 1)" at 12:10:47.91; **1.26 s later** the SBIO kind-1 push carries
value16 `0x208` / info32 501 at 12:10:49.18. That each path reaches its log
through the SEP is **[V-code]** (one receive callback per kext, §6.2 and
§3.1); that the two describe the same unlock is a timing correlation **[I]**,
not a traced causal edge. The field sets differ in kind: per-handle on the
`sks ` path, per-user on SBIO.

### 6.3 AP→SEP requests and the user client

AppleSEPKeyStore has an AP→SEP request/response mechanism over the same
endpoint ("no sks endpoint to send to", "sks request timeout (%d strikes)"
strings at 0xfffffff005614327..0x56144bd) and an `AppleKeyStoreUserClient`;
the live capture shows userspace driving it (`sel: 17`, `sel: 57`, `sel: 87`
with `e00002c2/e00002bc/e00002d5` results, and "invalid handle -501
(_device_keybag_access: 0)"). The AP→SEP selector space was **not** recovered
here. It is required for keybag option B and for any future servicing of this
endpoint; nothing in this workstream sends it. **[U]**

## 7. What Linux can represent without Apple keybags

Representable now (no SEP-visible keybag required):

- the **userID namespace**: a plain u32; iOS used the POSIX uid 501. A Linux
  user (e.g. uid 1000) is representable, but sepOS's per-user state was
  created by iOS's enrollment path; whether sepOS will ever push a kind-1
  message for a uid it has never seen cannot be answered from AP code, and
  the §11 window can only observe whether a push happens under its
  conditions;
- the **queue + cache semantics** of §3.2/§4 verbatim: a 10-entry
  `{userID, lockState}` ring, a per-user table (20 slots), the `0x15`
  default, the "OR mask `0x4` when the driver is enabled" rule, and the
  `GetSKSLockState` shape (4-byte userID in, 4-byte lockState out) for the
  fprintd / `/dev/sep-bio` layer. These are the *shape* of an honest AP-side
  cache; they are not a license to populate one from the raw passive ring
  (§14 item 3);
- **routing** of the SBIO async messages: treated as unsolicited input (like
  xarm in `docs/sep/xart-handshake.md`), never matched as transaction
  completions — non-GT tags on the selected async endpoint go to the passive
  ring, and the GT tag range `0xfc..0xff` is the only thing a transaction may
  consume (§14);
- **fail-closed gating, as a defensive Linux policy rather than a model of
  sepOS**: if no authoritative per-user state has been observed, no path may
  report "unlocked", and enrol/verify must be refused. `sensor_present`
  reflects transport readiness (`sbio_ready`, set by the `0x73` rung), which
  is not lock state; using it as if it were would synthesize a state sepOS
  never sent.

Not representable without more work: creating or changing sepOS's per-user /
keybag state (sks AP→SEP selectors, keybag option B), and the `0x25` payload
(userspace data from `libBKDM1.dylib`, per sbio-commands.md Gap 1 — do not
guess it).

## 8. Minimum honest state: read-only no-template match

Ordered prerequisites, each individually verifiable:

1. SBIO endpoint advertised (`0x08`, 4/4 pages) and its OOL pair published —
   one shot per sepOS boot (existing rules).
2. `0x73` init proven (the tri53-sep7 plan in `docs/sep/touchid-next.md`).
3. **Passive observation window** after `0x73` (§11): record every unsolicited
   `ep=08` non-GT word (kinds 1/2/3) for 60 s and send nothing. Three bounded
   outcomes:
   - kind-1 observed → a per-user lock state was pushed without AP `sks `
     servicing. That makes a per-user state *source* observable; it does not
     make the raw ring authoritative — a future cache may be fed from it only
     after the values are reviewed (§14 item 3);
   - no kind-1 → **"not observed under these conditions."** Nothing follows
     about sepOS refusing, about the state being absent in general, or about
     `sks ` being required. The cache keeps its default and Linux stays
     fail-closed;
   - unexpected tag/ep or counter drift → stop and collect (§11 stop rules).
4. `GetProtectedConfig` (`0x2b`, little-endian user ID 501 in, 32 bytes out)
   is the next
   read-only *data* transfer in the SBIO transport ladder
   (`sbio-runtime-re.md` §13 L6), in a later boot after `0x73` succeeds. Its
   flags ("unlock/login/ApplePay") are the config-level view. **It is not this
   workstream's step and not part of §11.**
5. MatchMode `0x25` (68-byte payload) is **blocked** on recovering the payload
   from `libBKDM1.dylib` — sending a guessed 68-byte buffer violates the
   no-guessed-payload rule. Template state: the catacomb is AP-hosted (§9
   item 4), and a fresh tethered Linux boot restores none, so "no template" is
   the expected starting state (**[I]**, from `enrollment-storage-re.md`
   §5.4), not a question of what survived inside the SEP. A completed match
   with no identities is expected to fail with a "no identities" class error —
   that is the read-only no-template result to collect, but only with a real,
   recovered payload.
6. The async result machinery (`0x22` poll, `0x07`/`0x08` fetch, `0x09`/`0x21`
   pushes, `0x0b` cancel) must be implemented to close the operation, so even
   a failed match ends cleanly and leaves sepOS state quiescent.

**Deliberately absent: a standalone `0x23` probe.** Rev 2 put one here as a
"gate probe" that would decide keybag-less matching. That was wrong twice
over: `0x23` is only observed inside an active frame pipeline, and no
outcome of an idle send would be interpretable (§5). Nothing in this section
proposes it, and nothing in §11 sends it.

A "no-template match" without the sensor never produces frames; the honest
form is `0x25` + a short wait + `0x0b` cancel and collecting the SEP's
rejection reason. Anything further needs §9.

## 9. Minimum honest state: later real match

Everything in §8, plus:

1. the Mesa sensor front end on `spi2@A088000` (power, 8 MHz SPI, IRQ, the
   7-byte poll / 11-byte capture frames from sbio-commands.md §5);
2. the per-frame pipeline of §5, in the observed order:
   `0x23` → `0x06` (mode word + image metadata + userID at `+0x67`, length
   `0x97`/`0xa7`) → `0x65` relay (19668, chunked 16356+3312). The order is
   **[V]**; the meaning of `0x23` is **[U]** and is not needed to reproduce
   the order, though reproducing any of it is blocked on the sensor;
3. `RequestMessageData`/result handling and `Cancel`;
4. template persistence, exactly as `enrollment-storage-re.md` §5.1–§5.4
   recovered it: Prepare `0x6c` (24-byte key in, blob + u32 out) → durable
   **AP-hosted** write + `F_FULLFSYNC` (the `fcntl` command-51 binding is
   medium-confidence in that note, §5.2) → Confirm `0x37` (commit) →
   `GetCatacombId 0x33` (4→16) → `GetCatacombHash 0x36` (4→32). Host command
   62 (`CompleteSaveCatacomb`) performs **no SEP transfer** (memcmp + copy-out
   of the already-buffered `0x6c` result). The AP file is an NSKeyedArchiver
   container with a legacy `CAT1` decoder — **not** `catacomb_state_v1_t`,
   which is the SEP-side record. Restore at boot is host command 5 → `0x55`
   (template list), with `0x6d` (whole blob + 32-byte header) as the
   migration/restore-from-backup path; sepOS holds only a per-boot working
   copy. Storage must be atomic and must never touch syscfg/NVRAM/boot
   firmware;
5. lock-state consistency: sepOS is the authority. Linux observes kind-1
   pushes; any future match path must gate on an authoritative per-user state
   and on what sepOS actually accepts. No AP-side probe can establish
   permission. If no authoritative per-user state is observed, matching stays
   unavailable (fail closed, §14) — a defensive policy, not a claim about
   sepOS. Do not fake "unlocked" on the AP side: it would not change what
   sepOS checks and would only misreport to fprintd.

## 10. Payload layouts recovered so far

| structure | layout | confidence |
|---|---|---|
| SBIO async kind-1 word | `ep=08, tag=01`, `value16 = u16 lockState (bits 16..31)`, `info32 = u32 userID (bits 32..63; observed values ≤ 0xffff)`; e.g. `0x000001f5_0004_0108` | **[V]** fields, **[V-code]** interpretation |
| SBIO async kind-2 word | same shape, `value16` written to `user->u16@+0x10`, `info32` = userID | **[V]** |
| SBIO async kind-3 word | same shape, value/info observed 0/0; sets `+0x1e24` + wake | **[V]** |
| sks queue ring entry | `{u32 userID; u16 lockState}` (6 B), 10 entries | **[V-code]** |
| AppleSEPKeyStore event (32 B) | `u32 kind@0` (1 lock change), raw msg word `@+8` → `{handle=msg.data, state=msg.tag&0x7f, value=msg.byte2, flags=msg.byte3}`; kind-4: `{action@+8, uuid[16]@+0xc}` | **[V-code]** + trace |
| `GetSKSLockState` (Mesa 39) | in `{u32 userID}`, out `{u32 lockState}`; no SEP message | **[V]** |
| `0x06` image-processing init | `len = 0x97 (+0x10 if identity UUID present)`; word 0 = MatchMode mode word; `u32 userID @ +0x67`; image metadata bytes from the capture object (e.g. `+0x4d`) | **[V-code]** |
| MatchMode `0x25` | `{u32 mode @0; identity list…}` 68 B; selector/len/data at `+0xc`/`+0x10`/`+0x14`. Mode bit 1 selects `0x27`: 60 B from payload+8, selector/len/data at `+0x1c`/`+0x20`/`+0x24` (wire `+0x14`/`+0x18`/`+0x1c`), word 1 must equal `this+0x1d4c`. Full field semantics still userspace-owned | **[V-code]**, payload **[U]** |
| `0x23` | no request payload; observed reply zero-length (`0: 0 = 0 + 0`). No rejection was ever observed; the GT error envelope (tag `0xff`, status at receive-OOL `+0x10`) is the expected failure form | **[V]** observed, **[I]** failure form |

## 11. Guarded plan: framed `0x73`, then a passive window (proposal only)

Nothing in this section has been run. **This workstream proposes no transmit at
all.** Step 1 is the already-built rung owned by `docs/sep/touchid-next.md` and
`sbio-runtime-re.md` §13 L3; it is referenced here so the ordering is
unambiguous and must not be duplicated, re-derived, or varied here. Step 2 is
the only new work in this document: a read-only observation window.

### 11.1 Step 1 — framed `0x73` (existing rung, not repeated here)

Preconditions are the tri53-sep7 entry (`tri53-sep7-56c3395908`, payload
`build/boot/m1n1-linux-tri53-sep7-56c3395908.bin`), a fresh DFU, planner `GO`,
16/16/0, root shmem `0x87adb0000`, the four initial endpoints, the combined
gate, exactly eight complete endpoints with the known IDs and page ranges,
`published=2`, `sbio_ep=-1`, and both FIFOs empty (all as written in
`docs/sep/touchid-next.md` §2). The run then publishes the SBIO OOL pair
(`08 4000 4000`), selects ep 0x08 as the async endpoint, and requires
`tx=14 rx=21 tx_busy=0 dropped=0`,
`endpoints_complete=8 published=3 sbio_ep=8 sbio_ready=0`.

Its single message is the framed `0x73` with a 4-byte `01000000` payload. Pass:
`ret=0`, reply `tag=0xfc`, `version=1`, `total=0`, `offset=0`, `flags=0`,
`command=0x73`, `chunk=0`, both FIFOs empty, counters exactly +1/+1, and
`sbio_ready=1`. Any other status (including `0x101`) is a negative result to
record and stop; it is not a reason to vary the bytes
(`sbio-runtime-re.md` §12.1).

### 11.2 Step 2 — passive window (the only new proposal)

In the same boot, immediately after the `0x73` pass, read only:

```sh
D=/sys/kernel/debug/sep-a9
cat "$D/status"
cat "$D/notify"
sleep 60
cat "$D/status"
cat "$D/notify"
```

Requirements:

- `sbio` must already select ep 0x08 (the `0x73` run leaves it selected; it is
  a driver-local selector and writes no mailbox word). Without it, non-GT
  words are classified `OTHER` and never reach the ring.
- `notify` is 0444: it never replies, never reads a payload, and never maps a
  word to a lock state (`sep_dbg_notify_read`, `drivers/sep/sep-a9.c`).
- During the window: no `xfer`, no `gt`, no `ool`, no `sbio` write. Reads
  only.
- Record `sbio_ep`, `entries`, `captured`, `dropped`, and every
  `t_us/ep/tag/len/data` line verbatim. `len` is bits 16..31 and `data` is
  bits 32..63; the readback deliberately does not name them `lockState`/
  `userID`.

Outcomes and what each licenses:

| outcome | reading | what it does **not** license |
|---|---|---|
| ≥1 `tag=01` word | sepOS pushed a per-user lock state after SBIO init, with no AP `sks ` servicing: a source Linux can observe exists | nothing about a uid sepOS has never seen; nothing about match permission |
| only `tag=02/03` | the transport is alive; no per-user state was pushed in this window | absence is not proof — record "not observed under these conditions" |
| no non-GT words | the same; keep the counters | no statement about sepOS state at all |
| unexpected tag/ep, counter drift, `dead` | stop, dump, cold reset | do not probe to explain it |

Stop rules: any counter drift, unexpected endpoint traffic, FIFO residue,
`dead` status, timeout, or a second status read that disagrees with the first
→ stop, collect state, cold-reset. Never republish OOL and never resend `0x73`
in that boot. The OOL addresses belong to that Linux instance; end the session
with a cold reset. A quiet window is a result, not a failure — but it is only
"not observed under these conditions."

### 11.3 What stays blocked

- an active `0x23`: no reference frame and no established semantics (§5);
- `0x25`/`0x27` MatchMode and any real match: the userspace payload from
  `libBKDM1.dylib` is not recovered, the sensor front end is not built, and
  template storage is not implemented;
- any `sks ` AP→SEP selector, OOL publish, or servicing: the selector space is
  not recovered (§6.3) and the protocol is unknown;
- any keybag test: option B in `docs/sep/keybag.md`, separate workstream;
- the permission half of the `sks ` question: it needs its own test, with a
  recovered protocol, and cannot be inferred from this window in either
  direction.

## 12. Safety

- No writes to syscfg, NVRAM, or boot-firmware storage are proposed anywhere
  above, and this workstream sends no SEP message at all: step 1 is the
  existing, already-built `0x73` rung owned by another plan; step 2 is `cat`
  only.
- No bypass or weakening of authentication: the window can only *observe*
  what sepOS pushes. Faking lock state on the AP side is explicitly rejected
  (§9, §14).
- SEP panic remains per-boot and reset-recoverable; the passive window adds no
  message and no state change, and the teardown is the existing cold reset.
- This file documents and proposes only; nothing was executed on the device,
  and nothing was committed.

## 13. Anchors and comments in the shared Ghidra project

Renamed (saved to the shared project so other agents see them):
`IOSEPBiometricService_sksLockStateChangeHandler` (0xfffffff00669e49c — was
mis-renamed sksQueueProcessAsync; the real `sksQueueProcessAsync` is
0xfffffff00669e3e0), `IOSEPBiometricService_sksQueueEnqueue/Dequeue/Process*`
(0xfffffff00669dad4 / 0xfffffff00669ddf4 / 0xfffffff00669e098),
`IOSEPBiometricService_getUserState/getUserLockState/notifyUserChanged/init`
(0xfffffff0066946a0 / 0xfffffff0066938fc / 0xfffffff006696e84 / 0xfffffff00669e558),
`AppleMesaSEPDriver_performSpecificCommandGated/matchInit`
(0xfffffff0066a504c / 0xfffffff0066a8fd0),
`AppleSEPKeyStore_initSksEndpoint/sksReceiveCallback/messagePump/
processQueuedEvent/enqueueEvent/queueVfsNotification/lockStateChangeDispatch`
(0xfffffff0064b35e0 / 0xfffffff0064b39bc / 0xfffffff0064a9ab0 /
0xfffffff0064b3f50 / 0xfffffff0064aa470 / 0xfffffff0064aa270 /
0xfffffff0064a9248).

Not functions in Ghidra (analysis gap), comments placed, **not saved**:

- `AppleMesaSEPDriver_handleSEPMessage` — function start
  **0xfffffff0066ae4d8** (`sub sp,sp,#0x80`; x19=this from x0, x20=msg from
  x1). 0xfffffff0066ae498 is the preceding function's `ret` and is *not* the
  start; the decode comment belongs at 0x66ae4d8, and a note at 0x66ae498
  marks the tail.
- `AppleMesaSEPDriver_asyncCaptureHandler` — **0xfffffff0066a09b0**; the
  `0x23` emit site is 0xfffffff0066a1110 inside it and the error path is
  0x66a1548.

The rename set above is saved. The disassembly comments noted in C7 are
placed but not saved (the embedded JVM ignores signals); the orchestrator can
persist them with `scripts/ghidra-mcp.sh save`.

## 14. Implementation contract for the current driver

Binding for `drivers/sep/sep-a9.c` as it stands. Each item is either already
true of the code (reference in parentheses) or a prohibition this workstream
will not relax without new evidence.

1. **Tag routing.** `0xfc..0xff` are the only tags a transaction may consume
   (`sep_gt_classify`). A GT word whose endpoint is not the active
   transaction's endpoint (or that arrives with no active transaction) is
   `STALE` and must not be fed to it. Non-GT tags are never completions.
2. **Passive service words.** A non-GT word is recorded only when its endpoint
   equals the selected async endpoint (`sbio`); otherwise it is `OTHER` and
   dropped. Recording is `sep_notify_push`: raw word plus `ep/tag/len/data`, a
   32-entry ring, and a drop counter. It never replies, never reads a body,
   and never wakes a transaction.
3. **Display-only decode.** `value16` (bits 16..31) and `info32` (bits 32..63)
   are printed as `len`/`data`. The kernel must not name them `lockState` /
   `userID`, must not build a per-user lock table from them, and must not let
   them satisfy or abort a transfer.
4. **No synthesized unlocked state.** No path may report "unlocked" from the
   absence of a push, from `GetSKSLockState`'s `0x15` default, or from
   `sbio_ready`. `sensor_present` reflects transport readiness only. Until an
   authoritative per-user state source exists, match/enrol stay refused — a
   defensive policy, not a statement about sepOS.
5. **`sks ` is not published or serviced.** No OOL pair for ep 0x12, no
   receive handler, no AP→SEP selector, no event decoding — not until a
   protocol is recovered from iOS and reviewed. The driver has no protocol for
   it and will not guess one; `sks ` remains advertisement-only in the
   endpoint inventory.
6. **No sends from this workstream.** No `0x23`, no `0x2b`, no
   `0x25`/`0x27`, no `0x73` duplication, no `sks ` traffic. The only device
   work referenced is the existing `0x73` rung owned by
   `docs/sep/touchid-next.md`; the only new proposal is the read-only window
   in §11.
7. **Tests.** Host-side only: `tools/sep/gt_core_test.c`
   (`test_classification`, run by `tools/sep/test_gt_core.py`) must keep
   asserting that a non-GT word on the selected async endpoint is `NOTIFY`
   even with a transaction in flight, that a transport tag from another
   endpoint or with no transaction is `STALE`, and that an unselected
   endpoint's non-GT word is `OTHER`. This workstream adds no device test.
