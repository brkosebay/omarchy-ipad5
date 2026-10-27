# GetProtectedConfig — Mesa 46 / SEP opcode `0x2b`

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Resolves the `0x2b` gaps left open by `sbio-runtime-re.md` §8.3 and
`sbio-commands.md` §2: the exact call chain, the 4-byte input, the 32-byte
reply layout and its consumers, read-only status, ordering against `0x73` /
sensor power / SKS / keybag, error handling, and what GenericTransfer reply the
Linux driver must accept. Ends with a one-command probe contract.

Evidence labels: **[V-code]** static (kernelcache / dyld-cache binaries),
**[V-trace]** observed capture, **[I]** inference, **[U]** unknown.

Artifacts analysed (all read-only):

| image | source |
|---|---|
| `AppleMesaSEPDriver`, `AppleSEPGenericTransfer`, `AppleBiometricServices`, `IOBiometricFamily` | 20H350 `kernelcache.release.iPad6,11_12`, carved from `__PRELINK_TEXT` / `__PLK_TEXT_EXEC` by VA (`com.apple.driver.AppleMesaSEPDriver` @ `0xfffffff005680500`) |
| `libBKDM1.dylib` | `build/dsc/out/libBKDM1.dylib` |
| `BiometricKit` | `build/dsc/out/BiometricKit` |
| `BiometricSupport` | extracted this pass from `build/dsc/com.apple.dyld/` (`ipsw dyld extract … --slide`) |
| capture | `build/sep/sbio-trace.log` (12,965 lines, 2026-10-24) |

Method note: the shared Ghidra MCP server accepted TCP but stopped answering new
sessions during this pass, so the addresses below were re-derived from an
independent headless import of the *same* kernelcache and dyld-cache images
(read-only, temp project outside the repo). Every address is reproducible from
the carved image alone; nothing was renamed or saved into the shared project.

---

## 1. Answers at a glance

| question | answer | ev |
|---|---|---|
| SEP opcode | `0x2b`, 32-bit command word in the GT header / mailbox word | V-code |
| Mesa command | **46** (`GetProtectedConfig`), dispatcher `case 0x2e` | V-code |
| request payload | **exactly 4 bytes**, a little-endian `u32` **user id**, passed through unmodified by every kext layer | V-code |
| reply payload | **exactly 32 bytes**, 8 × little-endian `u32`, two 16-byte configs ("set", "effective") | V-code |
| transport path | `transactToIOMD` → `AppleSEPGenericTransfer::transfer` — one `0xfc` chunk each way, no chunking, no `0xfe` | V-code, V-trace |
| gating in the kext | envelope: `version==1` and payload length ≥ 8 at the service layer; command: `inLength>3`, `out-size != NULL`; nothing else — no command mode, no sensor state, no lock state, no enrollment check | V-code |
| read-only? | no AP-side state is written and the reply is not retained anywhere in the kext; SEP-side effect unobservable | V-code / U |
| length enforcement | written length must be exactly `0x20`, else `0xe00002db` | V-code |
| called when | once, in the enrollment window, ~1.1 ms after the catacomb save returns and ~2.3 ms before `GetSKSLockState`; not per unlock, not per save | V-trace |
| `0x73` relation | the wrapper that sends `0x2b` also lazily sends `0x73` first if SBIO was never initialised | V-code |
| sensor power | none: 3.66 s from the previous SPI frame and 26 ms before the next | V-trace |
| SKS relation | orthogonal: `GetSKSLockState` is local-only and never touches the mailbox | V-code, V-trace |

---

## 2. Call chain, layer by layer

Addresses are image-relative VAs of the 20H350 build.

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

That the middle layer is real is proven by the dispatch, not assumed:
`IOBiometricService::performCommandGated` calls `*(*this + 0x568)` and
`0xfffffff006f4e3e8 + 0x568` holds `0xfffffff0066a504c`, i.e. exactly
`performSpecificCommandGated`. The same table's `+0x698` holds
`sepTransactToIOMD`; `+0x5f0` holds the header validator
(`0xfffffff0066b092c`: accepts `'BM'` or `'CMCM'` subtype 1/0x1b/0x36, else
`0x102`) and `+0x5e8` a hook that returns the constant `0x4d42` (`'BM'`).

The service layer is **command-agnostic**: it validates the envelope and splits
the header, and has no idea what Mesa 46 is. That is why the raw mailbox command
that Linux sends carries only the 4 payload bytes — the 8-byte header exists
only between userspace and `performSpecificCommandGated`.

The case body, verbatim from disassembly (**[V-code]**):

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

Two details worth stating because they are easy to get wrong:

* the kext **always sends 4 bytes**, whatever `inLength` the caller passed; a
  longer `inData` is silently truncated to its first four bytes;
* the 32-byte figure is a **post-condition on the reply length**, not a buffer
  size the caller declares. Nothing in the Mesa driver knows the reply's fields.

`AppleBiometricServices::performCommand` is a two-line pass-through
(`0xfffffff006cbe4c8`: log, then `thunk_EXT_FUN_fffffff006693f38(*(this+0x88),
inData, outData, outSize)`), so the user client adds no validation and no
caching either. The only layers that look at the reply are the IOMD copy inside
`transactToIOMD` (bounds + short-write check) and userspace.

---

## 3. The 4-byte input

**[V-code]** The 4 bytes are the command's first scalar argument; no kext layer
reads or rewrites it. Every layer forwards the caller's buffer:

* `-[BiometricKitXPCServerMesa performGetProtectedConfigCommand:outSetCfg:outEffectiveCfg:]`
  takes it as its first argument (`param_3`), stores it with `str w22,[sp,#0x38]`
  (0x20a1f07b0) and passes `&that, 4` as `inData/inSize` (0x20a1f07b4, 0x20a1f07cc)
  to `performCommand:` with opcode `0x2e` and `inValue 0` (`mov w3,#0x0`, 0x20a1f07c8);
* the Mesa layer forwards `param_3` unchanged and always with length 4 (§2);
* inside the daemon the value is a `BKComponent`'s `userID`:
  `-[BiometricKitXPCServerMesa loadCatacombForComponent:]` (0x20a1ebf58) calls
  `getProtectedConfigurationForUser:[comp userID] withClient:0`, and
  `-[BiometricKitXPCServer getProtectedConfigurationForUser:withClient:]`
  (0x1eee478c0) logs it as `%d` and passes it straight through.

Corroboration: the same 4-byte-`userID` convention is used by Mesa 39
(`GetSKSLockState`), where the value is provably a per-user record key (§6).
The capture's only user is 501 (`checkCatacombForUser: 501`,
`sksQueueProcess: userID = 501`).

**[I]** The observed call therefore sent `f5 01 00 00` (= 501).

Not to be confused with two neighbouring numbers:

| value | what it is | where it comes from |
|---|---|---|
| `value:0` in the os_log line | BM header `+6`, i.e. `inValue`, **not** the payload | `w27 = *(u16*)(cmd+6)` (0xfffffff0066a5100), logged at 0xfffffff0066a5198 |
| `0x2e` | the **Mesa** command number (46) in the BM header | libBKDM1 0x20a1e940c |
| `0x2b` | the **SEP opcode** | Mesa case 0x2e |

The `value:` field is not payload-derived: `DisplayStatusChanged` and
`EnableBackgroundFdet` log `value:1` with `inLength:0`.

---

## 4. The 32-byte reply

### 4.1 Layout

Eight consecutive little-endian `u32`, two configs of four flags:

| off | field | width | consumer setter (BiometricKit) |
|---:|---|---:|---|
| `0x00` | `unlockEnabled` (set) | u32 | `-[BKUserProtectedConfiguration setUnlockEnabled:]` |
| `0x04` | `identificationEnabled` | u32 | `setIdentificationEnabled:` |
| `0x08` | `loginEnabled` | u32 | `setLoginEnabled:` |
| `0x0c` | `applePayEnabled` | u32 | `setApplePayEnabled:` |
| `0x10` | `EfUnlockEnabled` (effective) | u32 | `setUnlockEnabled:` (2nd object) |
| `0x14` | `EfIdentificationEnabled` | u32 | `setIdentificationEnabled:` |
| `0x18` | `EfLoginEnabled` | u32 | `setLoginEnabled:` |
| `0x1c` | `EfApplePayEnabled` | u32 | `setApplePayEnabled:` |

The **offsets and their order** are fixed by the libBKDM1 decoder
(0x20a1f07fc … 0x20a1f0874: `ldr w2,[sp,#0x10|0x14|0x18|0x1c]`, then
`[sp,#0x20|0x24|0x28|0x2c]`, each handed to a different property setter). The
**names** come from the `BiometricKit` client, which logs the same per-user
dictionary in this order (`0x1ab08d33a`):

```
getProtectedConfigurationForUser: -> %p(UnlockEnabled:%d,IdentificationEnabled:%d,
LoginEnabled:%d,ApplePayEnabled:%d,EfUnlockEnabled:%d,EfIdentificationEnabled:%d,
EfLoginEnabled:%d,EfApplePayEnabled:%d)
```

The name→offset join is positional (the daemon fills the dictionary in the same
order the decoder fills the objects), so treat the order as **[V-code]** and the
name assignment as **[V-code + positional]**. `Ef…` is the only self-describing
hint: it marks the second, "effective" half.

### 4.2 Consumers

1. **libBKDM1 decoder** — splits the 32 bytes into two
   `BKUserProtectedConfiguration` objects and writes them to the caller's two
   out-pointers (`outSetCfg`, `outEffectiveCfg`). It requires `outLen == 0x20`;
   on any other length it logs a fault and **returns 0 anyway** without writing
   the out-pointers (0x20a1f07d8/0x20a1f07dc → 0x20a1f09b8). Callers must
   therefore initialise their pointers, and cannot distinguish "no config" from
   success.
2. **BiometricSupport daemon** — `getProtectedConfigurationForUser:withClient:`
   (0x1eee478c0) converts both objects into an `NSMutableDictionary` of
   `NSNumber`s and returns it over XPC. Its per-field rule is the only
   "defaults" logic on the AP side:

   ```c
   v = [cfg unlockEnabled];            /* the u32 from the wire */
   if (v != -1)                        /* 0xffffffff = "not set" */
       dict[key] = @(v != 0);          /* any other value -> boolean */
   ```

   So **`0xffffffff` means "unset" and the key is omitted**; every other value is
   coerced to boolean. Nothing else masks, validates or substitutes values. The
   sentinel is a real compare, not a decompiler artefact: `cmn w0,#0x1` occurs
   once per field per config (12 sites, `0x1eee479c0` … `0x1eee47d04`).
3. **Clients** — `BiometricKit` (`-[BiometricKitXPCClient
   getProtectedConfigurationForUser:]`, `BKUserProtectedConfiguration`) and
   in-daemon callers: `-[BiometricKitXPCServerMesa loadCatacombForComponent:]`,
   `-[BioLogBase logUnlockedStateSyncWithIdentities:]`,
   `-[MesaCoreAnalytics getProtectedConfigurationMergedForAllUsers]`,
   `-[BiometricKitXPCServer getExpressModeState:forUser:withClient:]`.

Note that `BKUserProtectedConfiguration` has more properties than the wire
carries (`attentionDetectionEnabled`, `periocularMatchEnabled`, …); those come
from settings/XPC options, not from `0x2b`.

### 4.3 Sibling: system protected config

Mesa **67** (`GetSystemProtectedConfig`, dispatcher `case 0x43`) is a different
command: SEP opcode **`0x39`**, **0-byte** request, reply **`0x1c` = 28 bytes**,
and the kext enforces `0x1c` the same way (`mov w1,#0x39` at
`0xfffffff0066a66bc`, `cmp w8,#0x1c` at `0xfffffff0066a66e4`). 28 bytes is 7 ×
u32; the `BiometricKit` client surfaces five of them —
`TouchIDEnabled, UnlockEnabled, IdentificationEnabled, LoginEnabled,
UnlockTokenMaxLifetime` — and leaves two unnamed. This confirms both replies are
plain `u32` arrays and that "32" is specific to Mesa 46.

(The `0x39` row in `sbio-runtime-re.md` §8.3 is labelled "*(none)* — result
poll"; the dispatcher proves it is emitted by Mesa 67, like `0x2b` is emitted by
Mesa 46.)

---

## 5. Is it purely read-only?

**[V-code] Host side: yes.**
Case 0x2e performs exactly one transfer and one length check. It writes no
ivar, calls no state machine, no sensor routine, no SKS hook, does not cache the
reply, and does not touch `_commandMode`, `_sensorState` or the lock-state cache.
The only side effects of the path are:

* the mutex at `this+0x208` (held across the transfer, 10 s timeout),
* the lazy `initSbioCommunication()` call if SBIO was never initialised.

The corresponding *write* is a different opcode: Mesa 47
(`SetProtectedConfig`, `case 0x2f`) sends opcode **`0x2c` with 60 bytes** and
`lock:1` (`mov w1,#0x2c`, `mov w3,#0x3c` at `0xfffffff0066a5fe0`). No code path
was found that reads the `0x2b` reply inside the kernel at all.

**[I] SEP side:** consistent with a pure read — a `Get` with a selector, 4 bytes
in, 32 bytes out, no commit-style payload. **[U]** Whether sepOS updates
internal bookkeeping (access counters, cache timestamps) when answering is not
observable statically.

---

## 6. When it runs

### 6.1 Observed occurrence (the only one)

The 12,965-line capture contains **exactly one** `GetProtectedConfig`
(`grep -c` = 1), at 12:10:23.871, between the first completed catacomb save and
the SKS query that precedes the next match:

| time | event | src |
|---|---|---|
| 12:10:08.303 | `EnrollMode (3)` — enrolment starts | V-trace |
| 12:10:20.241 | last sensor SPI frame of the round (`readSensorData`) | V-trace |
| 12:10:23.199 | `saveCatacomb --> saveCatacombForComponents:` (component `(null)`) | V-trace |
| 12:10:23.455 | `master.cat` archived; `CompleteSaveCatacomb` returns 236 bytes | V-trace |
| 12:10:23.485 | `ConfirmSaveCatacomb` (`0x37`, 24 in) → XART user-keybag records written (`Saving USER-xART … Writing record type: 1`) | V-trace |
| 12:10:23.869 | `saveCatacombForComponents -> 0` (both images) | V-trace |
| **12:10:23.8706** | **user client entry for the config read** (`externalMethod <- selector:0`) | V-trace |
| **12:10:23.8710** | **`GetProtectedConfig (46), value:0, inLength:4, outLength:32`** | V-trace |
| 12:10:23.8713 | SEP replies `0xfc 0x2b, 0: 32 = 0 + 32` | V-trace |
| 12:10:23.8716 | `sepTransactToIOMD -> err=0x0`, 32 bytes written to the out IOMD | V-trace |
| 12:10:23.8733 | `GetSKSLockState (39), inLength:4, outLength:4` → `sksQueueProcess` (no mailbox traffic) | V-trace |
| 12:10:23.8925 | `MatchMode (4)` — the UI re-arms matching | V-trace |

It is **not** a per-operation call: four more `PrepareSaveCatacomb` cycles
(23.911, 23.936, 36.379, 36.418) and **70** further `GetSKSLockState` calls
follow in the same capture, with no second `0x2b`. It behaves like a one-shot
fetch on the first completed catacomb save / first-unlock window, which matches
the daemon's `loadCatacombAfterFirstUnlock` gate seen in `BiometricSupport`
strings.

**[I]** The trigger is a daemon-side refresh after the identity set changed, not
the Settings UI's XPC request: the frame sits on `biometrickitd`'s thread
immediately after both `saveCatacombForComponents` implementations returned, and
none of the candidate call sites
(`loadCatacombForComponent:`, `logUnlockedStateSyncWithIdentities:`,
`getProtectedConfigurationMergedForAllUsers`, the XPC exported object) emits an
os_log line of its own, so the caller frame cannot be named from the capture.
**[U]** the exact call site.

### 6.2 Relation to `0x73` (SBIO init)

**[V-code]** `sepTransactToIOMD` always takes the Mesa mutex and then, if
`*(this+0x20c) == 0`, runs `initSbioCommunication` — which sends opcode `0x73`
with `u32 1` (0xfffffff0066bb168: `mov w22,#0x1`, `mov w1,#0x73`, `mov w3,#4`,
NULL out) and sets `*(this+0x20c) = 1` on success. So on iOS, any
`GetProtectedConfig` issued before the first SBIO init would carry a `0x73`
immediately before it.

On Linux the kext is not in the path, so this lazy init does not exist: **the
probe must be preceded by a successful `0x73` in the same boot** (it is, in the
proven `tri53-sep7` run). Sending `0x2b` without it would be an ordering no iOS
boot ever produces.

### 6.3 Relation to sensor power

**[V-trace]** None. In the whole capture there is not one `setSensorPower`
line, and the `0x2b` call is 3.66 s away from the previous SPI frame and 26 ms
before the next `StartCaptureGated` (which belongs to the following `MatchMode`,
not to this command). **[V-code]** Case 0x2e touches no power/regulator/SPI
code. The command is a pure mailbox exchange and does not require the sensor to
be powered or capturing.

### 6.4 Relation to SKS / keybag

**[V-code]** `GetSKSLockState` (Mesa 39, `case 0x27`) is **not a SEP command**:
it calls `IOSEPBiometricService::sksQueueProcess` (`0xfffffff00669e098`), reads
the per-user state record and returns 4 bytes. It never reaches
`sepTransact*`. This matches the trace, which shows the Mesa log line but **no
`sendRawMessage`** for it, and shows `sksQueueProcess` immediately after.

**[V-code]** `IOSEPBiometricService::sksQueueProcess` dequeues
`(userID, lockState)` events and updates per-user service state; it issues no
Mesa/SEP command. The trace shows the same shape
(`sksQueueProcess <-`, `sksQueueDequeue <-`, `-> err:0xe00002f0`,
`sksQueueProcess -> err:0x0`) with zero mailbox words. The object that carries
`0x2b` (`IOSEPBiometricService::performCommand/performCommandGated`, §2) is the
same object, but the SKS queue methods are unrelated to it: one is a generic
command envelope, the other is the local lock-state machine.

So: keybag state reaches the biometric stack through `AppleSEPKeyStore` →
`IOSEPBiometricService` (in-kernel, local), while the unlock/login/ApplePay
*policies* are what SEP reports over `0x2b`. The two are adjacent in time
(catacomb commit → keybag XART records → config read → lock-state read) but
mechanically independent.

---

## 7. Error handling

| layer | condition | result | ev |
|---|---|---|---|
| libBKDM1 decoder | `performCommand` returned non-zero | os_log fault with the raw code, out-pointers untouched, returns that code | V-code |
| libBKDM1 decoder | success but written length ≠ 32 | os_log fault (line 0x1b0d), out-pointers untouched, **returns 0** | V-code |
| user client | caller's out buffer smaller than the reply | `AppleSEPGenericTransfer::transactToIOMD` `getLength() < len` → local error `0x109` | V-code |
| user client | no out descriptor and no out size | `0x102` from `transactToIOMD` | V-code |
| Mesa 46 | transfer error | returned unchanged (no translation, no retry) | V-code |
| Mesa 46 | written length ≠ 0x20 | assert log (line 0x6dc) + `0xe00002db` | V-code |
| Mesa 46 | `inLength <= 3` or out-size pointer NULL | assert + `0xe00002c2` | V-code |
| Mesa ToIOMD | mutex not acquired within 10 s | error from the lock helper, assert log | V-code |
| Mesa ToIOMD | `initSbioCommunication` failed | assert log, its error returned | V-code |
| GT `transfer` | reply command echo ≠ request | `0xe00002c9` | V-code |
| GT `transfer` | wait timed out (10 s) | timeout error from the wait helper | V-code |
| GT `transfer` | tag `0xff` reply | `+0xa8` status (from reply `+0x10`) returned **raw** | V-code |
| GT `transactToIOMD` | `kIOReturnUnsupported` (`0xe00002c7`) | passed through without the extra assert | V-code |

No layer retries. Nothing translates `0xe00002db` into something friendlier, so
a 0-byte or short reply surfaces in userspace as `0xe00002db` rather than as
"empty config". The Linux driver has no such post-condition: it reports
`out_len=<n> ret=0` for any well-formed reply, which is why the probe must check
`out_len=32` itself (§9).

---

## 8. GenericTransfer reply shape the Linux driver must accept

**[V-code]** `drivers/sep/sep-a9.c: sep_gt_xact_rx_chunk()` accepts a reply only
if all of the following hold — for `0x2b` this means one `0xfc` chunk,
`seq 0`, `offset 0`:

| header field | required |
|---|---|
| `version` (`+0x00`) | `1` |
| `flags` (`+0x0c`) | `flags & 0x6 == 0` |
| `total` (`+0x04`) | `<= 0x10000`; on the first chunk also `<= out_cap` |
| `offset` (`+0x08`) | `0` on the first chunk, exactly `out_done` on continuations |
| `reserved` (`+0x10`) | `0` in a chunk reply (status word only in tag `0xff`) |
| `command` (`+0x14`) | equal to `0x2b` |
| `chunk` (`+0x18`) | `<= total`, `<= capacity`, `<= total - out_done` |
| tag / seq | first reply `0xfc` with seq `0`; continuations `0xfd` with strictly greater seq |

The proof that the shape is right for this command family is in the capture:
`sendRawMessage … request = 0xfc, command = 0x2b, 0: 4 = 0 + 4` and
`sepMessageHandler … request = 0xfc, command = 0x2b, 0: 32 = 0 + 32` — one
chunk each way, `total = 32`, and the whole reply copied with
`writeBytesToIOMD … inLength:32`.

The Linux `xfer` readback prints exactly the fields needed to check this
(`out_len=`, the hex body, `gt_reply version=… total=… offset=… flags=…
reserved=… command=… chunk=… mailbox_data=…`, `gt_sent`, `gt_recv`,
`gt_counters … dead=…`), so no new driver code is needed for the probe.

---

## 9. Bounded negative searches

Each of these was looked for and *not* found; they bound the claims above.

| search | scope | result |
|---|---|---|
| any Mesa dispatcher case other than `0x2e` emitting `0x2b` | `performSpecificCommandGated`, 13,144 bytes, full disassembly | none — `mov w1,#0x2b` occurs once, at `0xfffffff0066a5f68` |
| any read of the 32-byte buffer inside the kernel | full path: service gate → Mesa case 0x2e → `transactToIOMD` → `transfer` | none — the bytes are copied into the caller's IOMD and never parsed; only the length is checked |
| any command-specific handling of Mesa 46 in `IOBiometricFamily` | `performCommand` / `performCommandGated`, full decompile | none — envelope validation and header split only; the dispatch by command number lives in Mesa |
| a SEP opcode behind `GetSKSLockState` (Mesa 39) | `case 0x27` body, full trace | none — local state query only, no `sendRawMessage` |
| `GetProtectedConfig` anywhere else in the capture | 12,965 lines | 1 occurrence (12:10:23.871) |
| `loadCatacombForComponent` in the capture | 12,965 lines | 0 occurrences (its log is Debug-level and may simply be off) |
| `SetProtectedConfig` (Mesa 47) in the capture | 12,965 lines | 0 occurrences — only the getter ran |

---

## 10. Unknowns

* **[U]** Whether sepOS interprets the 4 bytes as a uid, a slot index or an
  opaque key; whether an unknown/absent user gets `0xffffffff`, zeros, or a
  `tag 0xff` error.
* **[U]** The value domain of each field: `0`/`1`, arbitrary non-zero, or
  bitmasks. The AP side only does `!= -1` and `!= 0`, so anything else is
  invisible to it.
* **[U]** Whether the reply depends on enrollment/catacomb state — the AP-side
  "set" vs "effective" distinction implies SEP derives `Ef*` from something
  (system config, enrollment, policy), but that is SEP-internal.
* **[U]** Whether `0x2b` requires a prior `0x73` on sepOS's side (the kext always
  provides one; Linux bypasses the kext).
* **[U]** Whether `0x2b` mutates any SEP-internal accounting.
* **[U]** Whether the ApplePay/Login flags have any meaning on this hardware
  configuration (j71t, no Apple Pay provisioning under Linux).

---

## 11. One-command safe probe

One command, one attempt, no retries in the same boot. Everything below mirrors
the proven `tri53-sep7` SBIO run and uses only existing debugfs files.

### 11.1 Preconditions (all must hold; stop otherwise)

```sh
D=/sys/kernel/debug/sep-a9
grep -q 'ran=1 result=2 ret=0 ep=2 manifest_len=6157' "$D/arts-start"   # MANIFEST ok
grep -q 'endpoints_complete=8 published=3'            "$D/status"      # xarm+arts+sbio
grep -q '^sbio_ep=8 sbio_ready=1$'                    "$D/status"      # 0x73 acked
grep -q '^ap->sep snd_sts=.* (empty)$'                "$D/fifo"
grep -q '^sep->ap recv_sts=.* (empty)$'               "$D/fifo"
grep -q 'dead=0'                                      "$D/xfer"        # mailbox not poisoned
```

Why each matters: `0x2b` is only ever issued by iOS *after* SBIO init, so
`sbio_ready=1` reproduces the real ordering; an unpublished SBIO endpoint means
no OOL buffer to receive into; a non-empty FIFO means a stale word could be
taken for the reply; `dead=0` keeps the probe from being the third strike
against a mailbox that is already poisoned for this boot.

### 11.2 The command

```sh
echo '08 2b f5010000' > /sys/kernel/debug/sep-a9/xfer
```

* endpoint `08`, opcode `2b`, **4-byte** payload `f5 01 00 00` = 501
  little-endian (the only user id observed in the capture).
* Do **not** prepend the 8-byte `'BM'` user-client header: that header is
  consumed by `AppleBiometricServicesUserClient`/Mesa and never reaches the
  mailbox. The wire payload is exactly the 4 bytes.
* Do not append padding or a second word: Mesa always emits exactly 4 bytes for
  this command (a longer user-client payload is truncated), so an 8-byte request
  would be a shape sepOS has never seen from iOS.

### 11.3 Pass criteria

Read back and require all of:

```sh
cat /sys/kernel/debug/sep-a9/xfer
# out_len=32
# <64 hex characters>
# ep=0x08 cmd=0x2b ret=0 sep_status=0x0
# gt_reply version=1 total=32 offset=0 flags=0x0 reserved=0x0 command=0x2b chunk=32 mailbox_data=0x0
# gt_recv n=1 fc:0:0:32
# gt_counters ok=<prev+1> ... timeouts=0 proto=0 dead=0
```

The 64 hex characters are the 8 `u32` fields of §4.1 in order; record them
verbatim. A pass proves transport, framing, the 4-byte request and the 32-byte
reply contract, and gives the field values for this boot.

### 11.4 Fail and stop rules

| readback | meaning | action |
|---|---|---|
| `ret=-110` (`-ETIMEDOUT`) | SEP did not answer within 10 s | **stop.** Two consecutive timeouts poison the mailbox for the boot. Log the `xfer`/`fifo`/`status`/`dmesg` and cold-reset. |
| `ret=-71` (`-EPROTO`) | reply frame violated §8 | **stop.** Three consecutive protocol errors poison the mailbox. Keep the `gt_reply` line — it names the offending field. |
| `ret=-5` with `sep_status=0x…` | clean SEP-side refusal (tag `0xff`) | **stop after recording `sep_status`.** This is an answer, not a desync; do not retry in this boot, and do not interpret the status symbolically (there is no symbol table for it). |
| `ret=0` and `out_len` ≠ 32 | sepOS answered a differently-sized reply | **stop.** Record `out_len` and the hex; do not resend. |
| `ret=0` and `out_len=32` | pass | record the 32 bytes; continue with at most a read-only follow-up. |

One attempt per boot in every case. A second datapoint (e.g. a different user
id) needs a new boot, so that a rejection cannot be confused with a poisoned
mailbox.

### 11.5 What remains opaque after a pass

Even a perfect 32-byte reply leaves these open, and they must be reported as
unknowns rather than interpreted:

* what the four "set" flags mean when they disagree with the four "effective"
  flags (SEP's derivation is internal);
* whether `0xffffffff` is ever returned and what it means at SEP level (the AP
  side reads it as "unset");
* whether the values depend on enrollment, keybag state or system config;
* whether the reply changes across boots or after enrollment;
* whether the input is validated at all (a wrong uid may simply return a
  default/zero record).

---

## 12. Anchor index

| symbol / data | address | image |
|---|---|---|
| `performSpecificCommandGated` | `0xfffffff0066a504c` | AppleMesaSEPDriver |
| Mesa 46 case body (`case 0x2e`) | `0xfffffff0066a5f38`–`0xfffffff0066a5fb4` | AppleMesaSEPDriver |
| Mesa 67 case (`case 0x43` → op `0x39`, 28 bytes) | `0xfffffff0066a66bc` / `0xfffffff0066a66e4` | AppleMesaSEPDriver |
| Mesa 47 case (`case 0x2f` → op `0x2c`, 60 bytes) | `0xfffffff0066a5fe0` | AppleMesaSEPDriver |
| `'BM'` magic getter / validator (vtable `+0x5e8`/`+0x5f0`) | `0xfffffff0066b0924` / `0xfffffff0066b092c` | AppleMesaSEPDriver |
| `sepTransactToIOMD` | `0xfffffff0066bb1f0` | AppleMesaSEPDriver |
| Mesa vtable base / slot `+0x698` | `0xfffffff006f4e3e8` / `0xfffffff006f4ea80` | AppleMesaSEPDriver |
| `initSbioCommunication` (`0x73`, u32 1) | `0xfffffff0066bb114` | AppleMesaSEPDriver |
| `sepTransact` (vtable `+0x6a0`) | `0xfffffff0066bb3c4` | AppleMesaSEPDriver |
| `AppleSEPGenericTransfer::transactToIOMD` | `0xfffffff0066905bc` | AppleSEPGenericTransfer |
| `AppleSEPGenericTransfer::transfer` | `0xfffffff006690140` | AppleSEPGenericTransfer |
| `AppleBiometricServices::performCommand` | `0xfffffff006cbe4c8` | AppleBiometricServices |
| `AppleBiometricServicesUserClient::extPerform` | `0xfffffff006cbe78c` | AppleBiometricServices |
| `IOBiometricService::performCommand` (gates the work) | `0xfffffff006693f38` | IOBiometricFamily |
| `IOBiometricService::performCommandGated` (splits the `'BM'` header) | `0xfffffff006693fd4` | IOBiometricFamily |
| `IOSEPBiometricService::sksQueueProcess` | `0xfffffff00669e098` | IOBiometricFamily |
| `performGetProtectedConfigCommand:outSetCfg:outEffectiveCfg:` | `0x20a1f06f8` | libBKDM1 |
| `performCommand:inValue:inData:inSize:outData:outSize:` | `0x20a1e9794` | libBKDM1 |
| `performCommand:version:inValue:inData:inSize:outData:outSize:` | `0x20a1e940c` | libBKDM1 |
| `loadCatacombForComponent:` | `0x20a1ebf58` | libBKDM1 |
| `getProtectedConfigurationForUser:withClient:` | `0x1eee478c0` | BiometricSupport |
| `saveCatacombForComponents:` | `0x1eee65504` | BiometricSupport |
| XPC exported `getProtectedConfigurationForUser:client:replyBlock:` | `0x1eee44e20` | BiometricSupport |
| `BKUserProtectedConfiguration` setters | `0x1ab07bc90`, `0x1ab07bca4`, `0x1ab07bcb8`, `0x1ab07bccc` | BiometricKit |
| 8-field description format (`0x1ab08d33a`) | referenced from `0x1ab061c2c` | BiometricKit |
