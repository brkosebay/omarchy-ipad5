# SBIO (Touch ID) command set

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Source of truth: a live os_log capture of iOS 16.7.10 on j71t during a **first**
fingerprint enrolment (empty identity set) followed by five unlocks, combined
with the iOS kernelcache. Raw capture: `build/sep/sbio-trace.log` (12,965 lines
/ 2.0 MB, artifact, not committed). Curated excerpt:
`docs/sep/traces/2026-10-24-first-enroll.md`.

The trace gives opcodes, direction, lengths and ordering. Payload **bytes** are
redacted (`<private>`), so structures come from the kernelcache; the anchors for
that are at the end.

## 1. Message framing (trace plus kernelcache reconstruction)

```
transact:          command:0x2b, inLength:4, outLength:<redacted>
sendRawMessage:    request = 0xfc, command = 0x2b, 0: 4 = 0 + 4
sepMessageHandler: request = 0xfc, command = 0x2b, 0: 32 = 0 + 32
```

- mailbox tag **`0xfc`** is the first request, opcode = command, param = 0.
  The 32-bit mailbox data is not a byte length. Its high half is a 16-bit
  sequence number and its low half is zero; therefore the first message has
  data **zero**. The SEP completion echoes tag and command with first-chunk
  data zero.
- each send and receive OOL buffer begins with this 28-byte little-endian
  `AppleSEPGenericTransfer` header; payload begins at `+0x1c`:

  | offset | field | first `0x73` request |
  |---:|---|---:|
  | `0x00` | version | `1` |
  | `0x04` | total payload length | `4` |
  | `0x08` | payload offset | `0` |
  | `0x0c` | flags | `0` |
  | `0x10` | reserved | `0` |
  | `0x14` | command | `0x73` |
  | `0x18` | this chunk length | `4` |
  | `0x1c` | payload | `01 00 00 00` |

- the logged `0: N = offset + length` is decoded from that OOL header, not
  from mailbox `param`/`data`.
- tag `0xff` is the error form; its status remains at receive OOL `+0x10`.
- payloads live in the endpoint's OOL buffers, never inline in the mailbox.

Large transfers are chunked at **16,356 bytes**:

```
send 0xfc cmd 0x65 sequence 0   19668 = 0 + 16356
recv 0xfe                                            "continue"
send 0xfd cmd 0x65 sequence 1   19668 = 16356 + 3312
recv 0xfc                                            done
```

So: `0xfc` first chunk, `0xfd` continuation, `0xfe` continue-ack, 16,356-byte
payload chunks (`0x4000 - 0x1c`). Sequence 1 is encoded as mailbox data
`0x00010000`, not param 1. The captured flow needs this for both the sensor
relay (`0x65`, 19,668 bytes) and a full catacomb export (`0x6c`, 16,379
bytes).

## 2. Command table (Mesa layer -> SEP opcode)

Mesa numbers are what `AppleMesaSEPDriver` logs; the SEP opcode is what actually
goes over the mailbox. Confidence: **verified** = paired in the trace by
adjacent `sepTransact` lines with matching lengths.

| Mesa command | # | SEP opcode | in | out | purpose | conf |
|---|---|---|---|---|---|---|
| EnrollMode | 3 | `0x03` | 68 | 0 | start enrolment / mode | verified |
| MatchMode | 4 | `0x25` | 68 | 0 | verify / match mode | verified |
| PrepareSaveCatacomb | 61 | `0x6c` | 24 | 236..16379 | SEP exports the catacomb blob | verified |
| CompleteSaveCatacomb | 62 | none | 24 | buffered blob | host-only key check and copy of the `0x6c` reply | verified |
| ConfirmSaveCatacomb | 63 | `0x37` | 24 | 0 | save commit | verified |
| GetCatacombId | 56 | `0x33` | 4 | 16 | current catacomb identity | verified |
| GetCatacombHash | 58 | `0x36` | 4 | 32 | catacomb hash | verified |
| GetProtectedConfig | 46 | `0x2b` | 4 | 32 | input is user ID; reply is set/effective unlock, identification, login, Apple Pay flags | verified |
| RequestMessageData | 43 | `0x22` / `0x07` | 0 / 8 | 36..3234 | async result fetch | verified |
| GetEnabledForUnlock | 48 | `0x21` / `0x09` | 0 | 3214 | unlock flags | verified |
| DisplayStatusChanged | 20 | `0x09` | 0 | 3234 | status push | verified |
| Cancel | 12 | `0x0b` | 0 | 0 | abort operation | verified |
| image processing init | - | `0x06` | 151 / 167 | 0 | per-frame config | verified |
| capture relay | - | `0x65` | 19668 | - | encrypted sensor frame | verified |
| SBIO init | - | `0x73` | 4 | 0 | `initSbioCommunication`, value 1 | kernelcache |

Also seen without a confident pairing: `0x54` (20 in), `0x27` (60 in),
`0x2e` (4 in), `0x41` (16 in), and `0x23` (0 in, internal per-frame
handshake; semantics unknown).

## 3. Enrolment sequence as observed

```
EnrollMode (0x03, 68)
  -> per-frame common prefix:
                0x23 internal handshake (0)
                0x06 image-processing init (151/167)
                0x65 capture relay (19668, chunked)
                0x07 result fetch (192 out)
     state-dependent tail: 0x08 or 0x09; sometimes 0x1f; 0x22 in 18/20 cycles
  -> PrepareSaveCatacomb (0x6c, 24)
     CompleteSaveCatacomb (host-only; returns the buffered 0x6c blob)
     AP writes and flushes the blob
     ConfirmSaveCatacomb (0x37, 24)
     GetCatacombId (0x33, 4 -> 16)
     GetCatacombHash (0x36, 4)
     GetCatacombState
```

The `0x6c` reply is the AP-persisted catacomb blob. CompleteSaveCatacomb makes
no SEP transaction: it validates the 24-byte key and returns the reply already
buffered by Prepare. The AP writes and flushes that blob before `0x37`
confirms the save. This cycle ran six times, so it is a repeatable flush rather
than a one-shot commit. A 16,379-byte observed reply proves that multi-chunk
receive is required for persistence.

## 4. Verify sequence as observed

```
MatchMode (0x25, 68)
  per-frame common prefix: 0x23 + 0x06 + 0x65 + 0x07
  state-dependent result/ack tail
  match result: 0x09 (out 3234) and 0x21 (out 3214)
  Cancel (0x0b) when the operation ends
```

## 5. Sensor transport (AppleBiometricSensor, SPI on spi2)

Printed verbatim by the kext:

```
Wrote:  7B | 80 10 00 07 00 00 00              poll state
Read:  23B | 00 00 00 00 00 00 00 80 20 00 10 00 00 00 07 ...
Wrote: 11B | 80 13 00 0b 00 00 00 d4 4c 00 00   read capture
Read: 19668B | ed fc 06 00 3a 80 ...             encrypted frame
```

This is the exact exchange the userspace/sensor driver must reproduce on
`spi2@A088000`; 19,668 bytes is also the size relayed to the SEP via 0x65.

## 6. Keybag gating (confirms option B)

```
AppleSEPKeyStore: Unlock notification for volume (uuid: 61706673-..., handle 0)
AppleSEPKeyStore: Sending lock change 0 for handle 0 value 0
IOSEPBiometricService: sksQueueProcess: userID = 501, lockState = 520
IOSEPBiometricService::sksLockStateChangeHandler
```

Enrol/verify follow the SKS lock state, exactly as `docs/sep/keybag.md` assumes.

## 7. Kernelcache anchors for interpreting the trace

`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`, Ghidra project
`kernelcache.release.iPad6,11_12`.

| what | address |
|---|---|
| `AppleSEPGenericTransfer::transfer` (framing, timeout, echo check) | `0xfffffff006690140` |
| `AppleSEPGenericTransfer::sendChunkMailbox` (tag/command/sequence word) | `0xfffffff00668e094` |
| `AppleSEPGenericTransfer::buildSendChunk` (28-byte OOL header) | `0xfffffff00668e2b4` |
| `AppleSEPGenericTransfer::parseFirstReplyChunk` | `0xfffffff00668e448` |
| `AppleSEPGenericTransfer::parseContinuationReplyChunk` | `0xfffffff00668e78c` |
| `AppleSEPGenericTransfer::parseErrorStatus` (`+0x10`) | `0xfffffff00668e9f4` |
| `AppleSEPManager::setSendOOLBuffer` (control opcodes 0x04/0x02) | `0xfffffff0062ee000` |
| `AppleSEPManager::setReceiveOOLBuffer` (0x05/0x03) | `0xfffffff0062ee0ec` |
| `AppleSEPDeviceService::startService` (4CC personality, OOL alloc) | `0xfffffff00668f40c` |
| `IOSEPBiometricService::initSbioCommunication` (0x73) | `0xfffffff0066bb114` |
| Mesa command-name table (`EnrollMode`=3, `MatchMode`=4, ...) | `0xfffffff005684a01`, `0xfffffff005684a0c` |
| AppleSEPManager.kext range | `0xfffffff005599c40` + `0x14a0c` |

### Results (2026-10-24, second RE pass)

**Gap 1 — the EnrollMode/MatchMode payloads are *userspace* data, not kext
data.** Both are 68 bytes on the wire (the kext only enforces a 0x30 minimum
for EnrollMode). Mesa command dispatch is one big switch in FUN_fffffff0066a504c
(`performSpecificCommandGated`), keyed on the command number:

- **case 4 (MatchMode)**: only checks `inLength >= 0x44` and forwards
  `(payload, len)` unchanged to FUN_fffffff0066a8fd0. The kext never
  interprets the 68 bytes.
- **case 3 (EnrollMode)**: requires `inLength >= 0x30`, then copies unaligned
  u64 fields from the caller's buffer at offsets 0, 2, 4, 6, 8, 10 into a local
  struct (built at `local_c70`) before FUN_fffffff0066a8ca4. Still caller-
  defined data, just repacked.

So the struct layout lives in **`biometrickitd` (userspace)**, not in
`AppleMesaSEPDriver`. That is the correct RE target for enrolling from Linux,
and it is static work: extract `biometrickitd` from the iOS dyld shared cache
and decompile the callers of `AppleBiometricServicesUserClient::extPerform`
(selector 0). The os_log trace can never show these bytes (`<private>`), and a
kernel-side trace would not either.

**Gap 2 — completion/status.** In the transfer object:

| offset | meaning |
|---|---|
| `+0xa4` | command echo, written from the reply message |
| `+0xa8` | **error status**, 0 on success |
| `+0xac` | flags from the reply message |
| `+0x18` | bytes received |
| `+0x30` | request state (1 = completed) |

`+0xa8` is written only on failure, by FUN_fffffff006690b48:
`*(u32*)(req + 0x28) = err` (transfer+0xa8). `err` is either the return value
of the per-chunk handler or, for a tag `0xff` message, the u32 at **reply
buffer + 0x10** (FUN_fffffff00668e9f4). On success the field stays 0.

**Payload offset (corrected 2026-10-26):** disassembly of
`AppleSEPGenericTransfer_buildSendChunk` (`0xfffffff00668e2b4`) and
`AppleSEPGenericTransfer_parseFirstReplyChunk` (`0xfffffff00668e448`) shows a
seven-**u32** header, not seven bytes. Payload starts at **`+0x1c`**. The old
`+7` reading conflated the field count with its byte size and caused the first
Linux SBIO probe to be rejected with tag `0xff`, status `0x101`.

**Chunk state machine** (FUN_fffffff00668eaa4, tag switch):
`0xfc` first/complete, `0xfd` continuation, `0xfe` "send next chunk",
`0xff` error carrying the status word.

### Endpoint discovery result (2026-10-26)

After a successful personalized `arts` MANIFEST followed by the captured-tag
xarm capability-0 reply, sepOS advertised `sbio` at endpoint **`0x08`**, with
exactly 4-page input and output OOL buffers. The same discovery batch contained
`scrd=0x0a`, `sks=0x12`, and `sse=0x0c`. Evidence is in
`docs/sep/logs/tri53-sep6-combined-20261026-2021/`.

Kernelcache reconstruction also establishes that `0x73` is a lazy SBIO-local
prerequisite: the generic biometric command wrappers call
`IOSEPBiometricService::initSbioCommunication` when its initialized flag is
clear, then perform the requested transfer. They do not initialize `scrd`,
`sks`, or `sse` first. SKS becomes relevant later for per-user/keybag lock
state, not for the SBIO hello.

### Userspace binaries (extracted 2026-10-24)

From the iPad's dyld shared cache (`build/dsc/com.apple.dyld/`, 3.0 GB), using
`ipsw dyld extract --slide --objc`:

| binary | path in cache | note |
|---|---|---|
| `libBKDM1.dylib` | `/usr/lib/libBKDM1.dylib` | the Mesa userspace stack (loaded by biometrickitd) |
| `BiometricKit` | `/System/Library/PrivateFrameworks/BiometricKit.framework/BiometricKit` | BKMatch/BKEnroll operation objects |

`biometrickitd` itself is an image in the cache but not in the `dylibs` table,
so `ipsw dyld extract` cannot pull it directly; `libBKDM1.dylib` is where the
Mesa command payloads are built anyway.

What `libBKDM1.dylib` already shows (strings/ObjC metadata and reconstructed
save/load paths):

- an AP-side `NSKeyedArchiver` container with `CatacombIdentityList`,
  `CatacombTemplateList`, `CatacombEnrollmentCount`, `CatacombCurrentDate`,
  `LoadDataFromCatacomb()`, and ivar `_hasLegacyCatacomb`; the legacy decoder
  accepts a `CAT1` container. `catacomb_state_v1_t` is an SEP-side record, not
  the format of the modern AP file;
- ObjC classes `BiometricMatchOperationMesa`, `BiometricKitXPCServerMesa`
  with match/enrol state ivars (`_matchOperationInProgress`,
  `_haveMatchResult`, `_lastMatchedIdentity`, `_enrollmentCount`).

So the catacomb is not SEP-only: userspace persists an archive containing the
SEP-exported secure blob and loads it during biometric initialization. That is
the same division of labour Aurora uses for its on-disk catacomb and is the
missing persistence half of the Linux path.

**Still to extract from that dylib:** the 68-byte `EnrollMode`/`MatchMode`
field layout and the catacomb serialization. Until then the driver accepts a
raw payload override:

```
echo "enrol <hex>"  > /sys/kernel/debug/sep-a9/bio_payload
echo "verify <hex>" > /sys/kernel/debug/sep-a9/bio_payload
```

so a layout recovered later (or captured live) can be tested without a rebuild.
