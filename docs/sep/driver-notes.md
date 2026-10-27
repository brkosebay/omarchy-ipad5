# SEP access driver: protocol recovered from the iOS 16.7.10 kernelcache

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


All addresses are static in `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`
(Ghidra project `kernelcache.release.iPad6,11_12`, image base `0xfffffff005298000`).
This is the AP-side contract a Linux driver has to reproduce. It was recovered by
decompiling the A9 build, not inferred from Asahi/Aurora.

## 1. Mailbox (already known from the ADT + pongoOS)

- `sep@0x20DA00000`, registers at **`0x20DA04000`**, 32-bit layout:
  `dis_int 0x00`, `en_int 0x04`, `snd_sts 0x08`, `snd0 0x10`, `snd1 0x14`,
  `recv_sts 0x20`, `recv0 0x34`, `recv1 0x38`.
- IRQs: 98/99/100/101 (ADT `interrupts`), `compatible = "iop,s8000"`.
- Message is 64-bit little-endian: `ep[7:0] tag[15:8] opcode[23:16] param[31:24] data[63:32]`.
- `recv_sts & 0x20000 == 0` means a message is waiting (pongoOS's test).

## 2. Endpoint model (`AppleSEPDeviceService`)

`AppleSEPManager.kext` = **0xfffffff005599c40, 0x14a0c**.

`AppleSEPDeviceService::startService` (FUN_fffffff00668f40c) builds the personality
name from a **4-byte endpoint code**:

```
scnprintf(buf, 0x12, "sep-endpoint,%c%c%c%c", code[0..3])   // 0xfffffff00567c9a7
IOService::nameMatching() -> waitForMatchingService()
```

Observed personalities in the kext: `sbio`, `sks`, `scrd`, `sse`, `xars`, `xarm`,
`arts`, `artr`, `debu`, `hilo`, `log`, `pair`, `unit`. So the SEP advertises each
endpoint with a 4CC; the AP attaches one driver per endpoint. Our driver needs the
same table, not a fixed endpoint numbering.

Each endpoint owns two **OOL buffers** (out-of-line DMA buffers), one send and one
receive, page-sized (`1 << PAGE_SHIFT`), allocated through the SEP manager:

```c
setOOLBuffer(dir, buf):
    cmsg(dir, ep = this->endpointIndex, size = buf->getLength())
    cmsg(dir, ep, pa = buf->getPhysicalAddress() >> 12)
```

`AppleSEPManager::setSendOOLBuffer` FUN_fffffff0062ee000 and
`setReceiveOOLBuffer` FUN_fffffff0062ee0ec produce the control messages
(endpoint field **0**, the target endpoint goes in `param`):

| direction | opcode | param | data |
|---|---|---|---|
| send, size | `0x04` | endpoint | length in bytes |
| send, address | `0x02` | endpoint | physical address `>> 12` |
| recv, size | `0x05` | endpoint | length in bytes |
| recv, address | `0x03` | endpoint | physical address `>> 12` |

The AP passes a raw physical address, so the SEP reads AP DRAM directly (no DART
translation) — same model as pongoOS's `seprom_load_sepos`.

Re-setting a buffer that already exists is refused with
`AppleSEP:WARNING: endpoint %u attempting to re-set send/receive OOL buffer`
(0xfffffff00559e305) and `0xe00002bc`, i.e. the buffers are set once per boot.

## 3. Per-endpoint transfers (`AppleSEPGenericTransfer`)

`AppleSEPGenericTransfer::transfer` FUN_fffffff006690140:

```c
transfer(this, cmd /*u32*/, inData, inLen, &outData, &outLen, flags)
    lock(this+0x38, 10000)
    ensure buffers (this+0x50 in, this+0x68 out)
    reject if inLen > this->bufferSize (this+0x18)
    reject unless (outData != NULL) == (outLen != NULL)
    request at this+0x80: {data=inData, len=inLen&0xffffffff, cmd at +0xa4, flags at +0xac}
    submit -> wait completion (this+0x10, 10000 ms)
    verify this+0xa4 == cmd      // echo check
    verify this+0xa8 == 0        // status
    on success copy this+0x80/+0x88 to outData/outLen
```

So a "command" on an endpoint is: fill the send OOL buffer, kick, wait for the
completion, check the echoed command and status word. 10 s timeouts in both places.
Every command is logged as
`AppleSEPGenericTransfer::%s: command:0x%x, inData:%p, inLength:%zu, outDataOut:%p, outLength:%p, flags:0x%x`
— a very convenient Frida hook target for live tracing.

The OOL buffer is not raw payload. `buildSendChunk` at
`0xfffffff00668e2b4` writes seven little-endian `u32` values at offsets
`0x00..0x18`: version (`1`), total length, offset, flags, reserved, command,
and chunk length. Payload starts at `+0x1c`. The mailbox word carries tag
`0xfc`, the command, and a 16-bit chunk sequence in data bits 16..31; the
first single-chunk request therefore has mailbox data zero. A successful reply
uses the same OOL header. Tag `0xff` is the separate error form with status at
receive OOL `+0x10`.

## 4. SBIO (Touch ID endpoint)

`IOBiometricFamily.kext` 0xfffffff005680500, `AppleMesaSEPDriver` matches
`sep-endpoint,sbio`.

`IOSEPBiometricService::initSbioCommunication` FUN_fffffff0066bb114:

```c
u32 one = 1;
transfer(sbio, 0x73, &one, 4, NULL, NULL, 0);   // -> succeed => this->0x20c = 1
```

**Command 0x73 (4-byte input = 1) is the SBIO init/handshake on iOS 16 A9.**
Other SBIO commands are reached through virtual calls (`AppleSEPGenericTransfer`
is called via a vtable slot), so a direct xref sweep does not enumerate them.
Two ways to finish the table:

1. live: Frida-hook `AppleSEPGenericTransfer`'s transfer and walk through
   enroll/verify/delete on the jailbroken iPad — gives command, lengths and
   payload layout directly;
2. static: recover the vtable slot for `transfer` and sweep `blr` sites in
   `IOBiometricFamily`/`AppleMesaSEPDriver`.

## 5. Boot path (`AppleSEPBooter`)

Method inventory (strings in AppleSEPManager.kext): `initForSEP`, `bootSEP`,
`romCommand`, `_bootAction`, `generateROMNonce`, `_sendSEPART`,
`triggerRecoveryMode`, `_captureiBICKCV`, `get_iBIC_KCV`.
`AppleSEPARTStorage` / `AppleSEPARTService` handle the anti-replay token
(`getART`, `setART`, `captureSleepART`, `invalidateNonce`, `persist_art`).
Next step here is decompiling `bootSEP` to pin the exact opcode order
(`ping`/`boottz0`/`IMG4`/`resume`) against pongoOS's implementation.

## 6. What the Linux driver needs, in order

1. map the mailbox, enable IRQ, message send/recv (pongoOS-equivalent);
2. wait for the SEP's endpoint advertisements, parse the 4CC table;
3. allocate + publish one send and one receive OOL buffer per endpoint
   (opcodes 0x02-0x05 above);
4. implement the `transfer()` framing (cmd echo + status + 10 s timeouts);
5. `sbio` init 0x73, then the rest of the SBIO command set from §4.

## 7. Live trace on iOS 16.7.10 (first enrolment + verify)

`idevicesyslog` over USB relays the kext's own os_log without root or
entitlements. One first-ever enrolment (empty identity set) plus five
lock/unlock cycles produced 12,965 lines / 2.0 MB. Saved at
`build/sep/sbio-trace.log`.

### Transfer framing (confirmed)

```
transact: command:0x2b, inLength:4, outLength:<private>
sendRawMessage:   Message to SEP, request = 0xfc, command = 0x2b, 0: 4 = 0 + 4
sepMessageHandler: Message from SEP, request = 0xfc, command = 0x2b, 0: 32 = 0 + 32
```

- kick tag is **`0xfc`**, opcode = command, `0: total = in + out` is
  `offset: total = in + out`; payload lives in the endpoint OOL buffers.
- the SEP echoes the same tag and command; completion matching is
  `(tag, command)` plus the out-buffer status word.
- **large transfers are chunked**: `RELAY_CAPTURE` (0x65, 19668 bytes) went out
  as `request = 0xfd, command = 0x65, 1: 19668 = 16356 + 3312`, i.e. tag `0xfd`
  with a sequence index and the 16 KiB OOL buffer split. The SEP answered with
  tag `0xfe`. Any transfer larger than the OOL buffer must implement this.

### SBIO command table (Mesa layer -> SEP opcode)

| Mesa command | # | SEP opcode | in | out | notes |
|---|---|---|---|---|---|
| EnrollMode | 3 | `0x03` | 68 | 0 | start enrolment |
| MatchMode | 4 | `0x25` | 68 | 0 | verify / match mode |
| PrepareSaveCatacomb | 61 | `0x6c` | 24 | 0 | catacomb save, step 1 |
| CompleteSaveCatacomb | 62 | (`0x33`?) | 4 | 0 | provisional |
| ConfirmSaveCatacomb | 63 | `0x37` | 24 | 0 | catacomb commit |
| GetCatacombHash | 58 | `0x36` | 4 | 0 | |
| GetProtectedConfig | 46 | `0x2b` | 4 | 32 | unlock/login/ApplePay flags |
| RequestMessageData | 43 | `0x22`/`0x07` | 0/8 | 36..3234 | async results |
| GetEnabledForUnlock | 48 | `0x21`/`0x09` | 0 | 3214 | |
| DisplayStatusChanged | 20 | `0x09` | 0 | 3234 | |
| Cancel | 12 | `0x0b` | 0 | 0 | |
| (image pipeline) | - | `0x06` | 151/167 | 0 | image processing init |
| (capture relay) | - | `0x65` | 19668 | - | sensor frame to SEP |

Other opcodes seen with no confident Mesa pairing yet: `0x54` (20 in),
`0x27` (60 in), `0x2e` (4 in), `0x41` (16 in), `0x23` (0 in, SKS lock state
query candidate).

Note the opcode numbering matches Aurora's `sbio.rs` for the well-known ones
(`0x6c` SAVE_CATACOMB, `0x37` CONFIRM_SAVE, `0x65` RELAY_CAPTURE, `0x06`
IMAGE_PROCESSING_INIT), so their userspace/ioctl shape carries over; only the
values around enrolment differ in detail.

### Sensor transport (Mesa SPI), observed directly

The log prints the exact frames:

```
Wrote:  7B | 80 10 00 07 00 00 00            (poll state)
Read:  23B | 00 00 00 00 00 00 00 80 20 00 10 00 00 00 07 ... (data ready)
Wrote: 11B | 80 13 00 0b 00 00 00 d4 4c 00 00 (read capture)
Read: 19668B | ed fc 06 00 3a 80 ...            (encrypted frame)
```

That is the shape the userspace/sensor driver must reproduce on `spi2`.

### Keybag evidence (option B)

The trace confirms biometrics are gated by the SEP keystore lock state:

```
AppleSEPKeyStore: Unlock notification for volume (uuid: 61706673-7575-6964-0040-766F6C756D01, handle 0)
AppleSEPKeyStore: Sending lock change 0 for handle 0 value 0
IOSEPBiometricService: sksQueueProcess: userID = 501, lockState = 520
IOSEPBiometricService::sksLockStateChangeHandler
```

So the SBIO service consumes SKS lock-state changes; enrol/verify only work
while the bag is unlocked. That is exactly the model `docs/sep/keybag.md`
(option B) assumes.

## 8. What is still unknown

- Whether the user bag is *created* at first enrolment (SKS `CREATE` /
  `CHANGE_LOCK_STATE`) or is a passcode artifact. The trace shows SKS lock
  changes but the create path is not visible as an SBIO command; it likely
  happens at passcode setup. This decides keybag option B1 vs B2.
- Payload bytes: os_log redacts them. Structures must come from the
  kernelcache or a Frida trace of `biometrickitd`.

## 9. SEPROM bootstrap: the exact XNU sequence (iOS 16.7.10 kernelcache)

`AppleSEPBooter::bootSEP` is the reference implementation for the A9 path.
Decoded from the packed mailbox constants in the kernelcache:

| step | message (ep/tag/op/param/data) | ack | meaning |
|---|---|---|---|
| 1 | `0/1/0x02/0/0` | `0x66` | status check 1 |
| 2 | `0/1/0x24/1/tmm_pa>>12` | `0x88` | TMM manifest (only if the firmware has one) |
| 3 | `0/1/0x25/mode/patch_pa>>12` | `0x89` | SEP patches (same condition) |
| 4 | `0/1/0x05/mode/0` | `0x69` + event `0xd2` | boot_tz0 |
| 5 | `0/1/0x02/0/0` | `0x66` | status check 2 |
| 6 | `0/1/0x07/0/art_pa>>12` | `0x6b` | optional anti-replay token (ART), **before firmware** |
| 7 | `0/1/0x06/mode/pa>>12` | `0x6a` | cold load **and boot** of the sepOS IMG4 |
| 8 | endpoint `0xfe`, size/pages + PA | transport result | optional root shared-memory descriptor, submitted while op 6 is in flight |

Notes:

- The `0/1/...` column is the byte layout of the 64-bit **command
  descriptor** in `bootSEP` (`0x20100`, `0x50100`, `0x60100`, `0x70100`,
  `0x801`). It is not the raw mailbox header: the transport sends on the
  SEPROM endpoint **`0xff`**. The cold test below proved this — replaying
  the literal descriptor bytes with `ep=0` produced zero replies, while
  `ep=0xff` commands on the same boot were acked. The receive handler is
  registered for endpoint `0xff` and its check is literally
  `message.frame.endpoint == EP_BOOTSTRAP`, so `EP_BOOTSTRAP = 0xff`.
- `0x69` = "SEP accepted Tz0", `0x6a` = completion of the cold
  load/boot command, `0x6b` = "SEP accepted SEPART", and `0x6c` =
  "SEP resumed from RAM"
  (`AppleSEPBooter` message handler).
- **Opcode `0x08` is not part of a cold boot.** `bootSEP` emits the
  `0x08` descriptor only when its `AppleSEPFirmware *` argument is NULL,
  i.e. the resume-from-RAM branch. With a firmware object it emits `0x06`,
  optionally submits the endpoint-`0xfe` shared buffer, and then waits for
  the `0x06` operation to finish. Sending `0x08` after `0x6a` was the main
  error in all of the pre-20:09 test builds.
- `op 0x24/0x25` are only sent when `AppleSEPFirmware` reports a TMM
  manifest/patch blob. The A9 sep-firmware IM4P advertises only
  `impl/arms` and `tz0s` (both `0xcf8000`), so the normal A9 path is
  steps 1, 4, 5, optional 6, and 7. The shared-buffer argument is allowed
  to be NULL; XNU logs a warning and still waits for the cold boot.
- `0xcf8000` is also the TZ0 size iBoot programmed on the warm and cold
  boots; if a boot leaves TZ0 unlocked, program
  `base=0x87bc64000 size=0xcf8000` before `boot_tz0`.
- **`fuse_lock()` must run before `op 0x05`.** The stock A9 path
  (`seprom_boot_tz0_async`) calls it; XNU's bootSEP does the equivalent
  in its pre-boot setup. Without it SEPROM still acks `0x69` and then
  panics — the failure seen with the first two `ep=0xff` builds.
- The status checks are not required to get `0x69`/`0x6a` (the old
  `sep sboot` got both without them). The test build keeps them opt-in
  (`sep xnu status`) so a rejected `op 0x02` cannot wedge the ROM early.

### `tag=0x70 op=0x65` is a ping, not a request

`0x65` = `1 + 100`, the response to SEPROM opcode 1 (ping). XNU's handler
labels it `"%s: SEP ping response"` and ignores `tag` entirely. It is
**not** an ART, nonce, or integrity-tree request. In builds before the
safe-stat fix, `sep stat: raw` was a direct mailbox-register read, not the last
handled message, and could be stale. Worse, reading it with an empty outbox
latched underflow bit 19. Current builds never sample FIFO data from `sep stat`.
The old repeated `...6570ff` snapshots do not by themselves prove a live
keepalive stream. Neither a ping reply nor a stale register establishes that a
post-`0x06` resume opcode is required.

### ART (anti-replay token)

`op 0x07` loads the ART; pongoOS calls it "anti-replay token". XNU gets
it from `AppleSEPARTService`, which reads the kernel-only NVRAM variable
`com.apple.System.sep.art` (system partition, not present in the iBoot
`nvram-proxy-data` common snapshot). More importantly, the branch is gated
by `AppleSEPManager+0x140`, set from the registry property `has-art`; that
property is absent from the live j71t dump and all checked ADTs. The stock
cold path therefore skips `op 0x07` on this board.

User-client selector `0x43` is a different path: it reads the running sepOS
ART-storage endpoint with tag-1 `op 0x03` plus five `op 0x04` requests. It
does not directly read the NVRAM object used by the cold booter, so its
20-byte result is only a presence probe and is not assumed interchangeable.
Zero-page ART remains an explicit diagnostic only, not a boot default.

### Cold test 2026-10-24 19:22 (docs/sep/logs/cold-xnu-20261024)

> Historical note: the test descriptions below record what each build sent,
> but their conclusions about `op 0x08` and the ART/firmware order were
> superseded by the corrected static analysis at the end of this section.

The `--xnu` build sent the descriptor bytes literally (`ep=0, tag=1`).
The serial log looked like "no replies", but the screen photo shows what
actually came back: every `ep=0` command got `ep=0 tag=1 op=0`
(an endpoint error), and the one `ep=0xff tag=1 op=8` got
`ep=0xff tag=1 op=0xc7 data=8` (a state error). Crucially, that op 8
was sent **without** a successful boot_tz0/fwload, because those were
also sent on `ep=0` and ignored. So `0xc7/8` does not prove op 8 is
unsupported; the ROM was simply asked to boot with nothing loaded.

`tz0` was the expected `87bc64000 + cf8000`, the staged SEPFW was valid,
and the Linux boot was unaffected.

Historical build change (now known to be wrong): send the whole sequence on
`ep=0xff`, then use `boot_tz0 -> fwload -> ART -> boot`. This result is kept
only to identify the binary that generated the log; the corrected cold path
is in the static-analysis section below.

### Cold tests 2026-10-25 (docs/sep/logs/cold-xnu-20261025, ...-1111)

Two `ep=0xff` builds reached `boot_tz0` and then got a SEPROM panic:

```
09:36 (19:32 build): ping 0x65 -> boot_tz0 0x69 -> panic
11:15 (11:11 build): boot_tz0 0x69 -> panic
```

The 11:15 log also showed `sep art: pa=18003e2c0`, which is pongoOS's
SRAM image, not DRAM (the SEPROM cannot read it). But the panic happens
before `fwload`/`artload`, so that invalid ART pointer was not used yet.

The common missing step in both new runs is **`fuse_lock()`**. The old
known-good A9 path (`seprom_boot_tz0_async`, used by `sep sboot`) calls
`fuse_lock()` before sending op 0x05; the new `sep xnu` sent op 0x05
directly. Fix: call `fuse_lock()` before `boot_tz0`, and put the ART page
back in DRAM via `alloc_contig` (pongoOS's static/BSS image is SRAM and
not SEP-readable).

### Cold test 2026-10-25 11:19 (docs/sep/logs/cold-xnu-20261025-1119)

`fuse_lock()` fixed the panic. The run got through:

```
boot_tz0  -> 0x69, 0xd2 data=2
fwload    -> 0x6a
artload   -> op 7 gets no reply at all (no 0x6b, no error)
boot      -> op 8 gets no 0x6c (all ep/tag variants)
sep stat  -> ROM still answers pings (0x65 tag 0x70)
```

The screen photo confirms `op 7` is silently ignored, not NAKed.

This test correctly established that ART is not accepted here, but its
contemporaneous flat-constant reading still placed resume op 8 after fwload.
That ordering is wrong. The later control-flow analysis proves the
manager's `has-art` flag skips op 7 and the cold branch ends in op 6:

```
boot_tz0 -> optional ART -> op 6 (cold load/boot)
```

`art` remains an explicit diagnostic, while status checks are used by the
next test. TMM is not exposed because no valid manifest/patch physical
addresses are available; sending null addresses produced `0xc7`.

### Cold test 2026-10-25 16:17 (docs/sep/logs/cold-xnu-20261025-1609)

The no-ART default still got no boot, but the first attempt completed the
whole stock pre-boot sequence:

```
status 1 -> 0x66 data=1
boot_tz0 -> 0x69, 0xd2 data=2
status 2 -> 0x66 data=2
fwload   -> 0x6a
boot     -> no 0x6c (all ep/tag variants)
```

The screen photo explains the "no acks" on retries 2-3: their
`0x66/0x69/0xd2/0x6a` acks arrived **late**, in a burst after `sep stat`.
The ROM was still busy with the first boot attempt, and our 3-second wait
plus immediate retries were much shorter than XNU's
`boot-timeout-ms = 60000`.

New build: send op 8 **once** (`ep=0xff tag=1`, XNU's tag), do not send
the known-wrong `ep=0` variants or duplicates, and **wait up to 60 s** for
the sepOS `ep=0 op=0xd` message without poking the mailbox.
`pongo-sep.sh --xnu` keeps the link open for 90 s for this.

### Cold tests 2026-10-25 17:xx (16:24 build, 60 s wait)

Run 1 (`--xnu status`, no TMM/ART): the full pre-boot sequence acked
(`0x66 data=1`, `0x69` + `0xd2 data=2`, `0x66 data=2`, `0x6a`), then op 8
(`ep=0xff tag=1`) got no `0x6c` and no sepOS `ep=0 op=0xd` during the full
60 s. `sep stat` then showed the `0x65` keepalive; the ROM was alive in
stage 1.

Run 2 (`sep xnu tmm status`): op `0x24`/`0x25` got no `0x88`/`0x89`
(expected, the A9 sep-firmware has no TMM manifest), then the same
`0x66/0x69/0x66/0x6a` acks and op 8 silence. `sep stat` ended with
`recv=c701ff`: `ep=ff tag=1 op=0xc7`, data byte not printed by that build.
`sep msg ff 0 9 0 0` / `0a` got no reply.

The run-1/run-2 difference is the TMM attempt, so the `0xc7` is at least
as likely to be the TMM rejection as the op-8 reply. The diagnostic build
adds a 16-entry ring of non-`0x65` SEP->AP messages; `sep stat` prints
each as `ep/tag/op/param/data/raw`, so the `0xc7` data is visible even if
the IRQ handled it earlier.

The flat-constant reading made at the time incorrectly placed op 8 after
`0x6a` and the endpoint-`0xfe` buffer after op 8. Control-flow analysis below
supersedes that interpretation: the cold and resume branches are exclusive,
and cold op 6 is followed immediately by the optional root buffer.

Zero-page ART was already tested on 2026-10-25 11:19: op 7 was silently
ignored, so repeating it is not worth a DFU. Only the real
`com.apple.System.sep.art` token is worth testing.

The diagnostic build also has `sep bootprobe [all]`: after a loaded
boot_tz0/fwload state it sends op 0x08 (or 0x08..0x0d with `all`) on
`ep=0xff tag=1` with a 2 s wait each, and the `sep stat` ring records any
non-ping reply. Use it in an interactive `probe-sep.sh shell` session if
op 8 is silent, to see whether this ROM names a different boot opcode.

### Cold test 2026-10-25 20:02 (16:24/17:20 build, ring)

Run 1 (`--xnu status`, no TMM/ART) at 20:02: the same five pre-boot acks,
then op 8 (`ep=0xff tag=1`) was silently ignored; the ring count after the
60 s wait was **5**, i.e. exactly `0x66`, `0x69`, `0xd2`, `0x66`, `0x6a`
and **no `0xc7`**. The last raw mailbox value was the `0x65` ping
(`ep=ff tag=70 op=65`). So the earlier `0xc7` was the TMM probe, not op 8.

`sep bootprobe all` in the same session re-ran the pre-boot sequence (same
acks, count=15), then sent op `0x08..0x0d`; **none** got a reply, and no
sepOS message. The ROM stayed in stage 1, pinging.

`op 0x65` is `1 + 100`: the SEPROM's ping response. The tag `0x70` is the
ROM's own tag/keepalive; XNU's handler ignores the tag and labels the
message "SEP ping response". It is not a reply to anything we send and
not a request.

Contemporaneous interpretation (now disproven): silence after `0x6a` was
attributed to a missing prerequisite before op 8, most likely ART. The static
correction below proves op 8 was never part of this cold path and j71t lacks
the `has-art` property.

Cheap same-session discriminators:
- `sep msg ff 70 8 0 0` — op 8 with the ROM's own tag `0x70`
- `sep msg ff 1 8 1 0` — param=1
- `sep msg ff 1 8 0 344000` — data = image length
- `/send art-random.bin; sep art; sep xnu art` — nonzero random token
- `sep xnu status delay 5000` — 5 s gap between `0x6a` and op 8

If all are silent, stop pongo-side probing and get the real
`com.apple.System.sep.art`; the kext exposes
`AppleSEPUserClient::GetART` with entitlement
`com.apple.private.applesepmanager.allow`, which is a better route than a
raw NVRAM dump.

### Static correction after the 20:02 failure (2026-10-25)

Re-reading `AppleSEPBooter::bootSEP` at `0xfffffff0062ef5a0` as control
flow rather than as a flat list of command constants found the protocol bug:

1. The ART branch, when enabled by the manager's `has-art` property, runs
   **before** the firmware command.
2. With a non-NULL `AppleSEPFirmware *`, descriptor `0x60100` (`op 0x06`)
   is the cold load/boot command. The function sends it, optionally submits
   the root shared-memory descriptor on endpoint `0xfe`, and then waits.
3. Descriptor `0x801` (`op 0x08`) is selected only when the firmware pointer
   is NULL. That is the resume-from-RAM path, not the continuation of `op 6`.

The old implementation did the opposite: it waited for `0x6a`, optionally
sent ART after the firmware, and then always sent `op 0x08`. The total silence
for `0x08..0x0d` is therefore expected and does not implicate the ART.

The corrected `sep xnu` path now:

```text
status1? -> boot_tz0 -> TZ0-complete -> status2? -> ART? -> op6(load/boot)
                                                      \-> shmem? (ep 0xfe)
```

It stops on any missing prerequisite acknowledgment, uses tag 1 for the XNU
descriptors, allows the kernel's full command timeouts, never sends `op 0x08`,
and includes the same endpoint-`0xfe` root-buffer descriptor that XNU normally
passes. The minimal test buffer is not arbitrary: the
`AppleSEPSharedMemoryBuffer` constructor writes 16-byte channel descriptors,
then the exact terminator bytes `6c 6c 75 6e 00...` (`"llun"` in memory), and
aligns the object to a manager-selected granule. The selector is the SEP-node
`aarch64` property; j71t lacks it, so zero channels is one 4 KiB page. XNU's
normal manager buffer contains additional console/trace channels and is larger,
so this isolates the bootstrap descriptor rather than reproducing every
service channel. On the next fresh-DFU boot, use `pongo-sep.sh --inspect`
first to record the staged image bytes without changing SEP state, then make
exactly one `sep xnu status` attempt (root shmem, no ART). Only if `op 0x06` still completes
without the sepOS boot message should a second fresh run use `noshmem` as the
negative control. A real ART test is relevant only if the live manager exposes
`has-art` and an NVRAM-system-partition read returns a nonzero 20-byte value.
