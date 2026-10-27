# A9 SEP Linux bring-up after the first sepOS boot

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26

> **2026-10-26 update:** the arts+xarm gate succeeded. sepOS advertised all
> four delayed services: `sbio=0x08`, `scrd=0x0a`, `sse=0x0c`, and
> `sks=0x12`. The next one-command SBIO procedure superseding the early plan
> below is `docs/sep/touchid-next.md`; it tests `0x73` alone and deliberately
> defers `0x2b` to a later fresh boot.

## Result and immediate decision

The cold path in `bc839bd6` succeeded. `SEPOS BOOTED`, the endpoint-0
liveness message, and the endpoint-discovery advertisements prove that sepOS
is running and that the one-page root shared-memory handoff is accepted. ART
was not required for this boot.

The next Linux step is **passive endpoint discovery**, not a request on
`cntl`. There is no `cntl` command that asks sepOS to announce the remaining
endpoints or replay its endpoint table. Linux must consume the boot-time
messages that are still in the mailbox, or receive a preserved copy of those
messages from pongoOS. Only after a complete `sbio` advertisement pair has
been observed may Linux publish SBIO's OOL buffers and transact with it.

For the first SBIO proof, use `0x73` followed by `0x2b`. Do not send `0x09`
as a synthetic display-status command: `DisplayStatusChanged` is local Mesa
driver state, while `0x09` is a state-dependent result fetch used during an
active match.

## 1. Decode of the endpoint announcements

The mailbox word is:

```text
bits  0..7   endpoint
bits  8..15  tag
bits 16..23  opcode
bits 24..31  parameter
bits 32..63  data
```

AppleSEPDiscovery installs a passive handler on endpoint `0xfd`. The handler
decodes two messages for each service:

| `ep` | `op` | `param` | `data` | meaning |
|---|---:|---:|---|---|
| `0xfd` | `0` | endpoint ID | little-endian 4CC | name advertisement |
| `0xfd` | `1` | same endpoint ID | four byte counts | OOL requirements |

For `op=1`, the bytes of `data` from least to most significant are XNU's
`in_min`, `in_max`, `out_min`, and `out_max` page counts. A9 uses 4 KiB SEP
OOL pages, so `0x08080808` means exactly eight pages (32 KiB) for each range.
XNU does not publish the endpoint nub until it has received both messages.

The 12:09 capture therefore decodes as:

| messages | decode | consequence |
|---|---|---|
| `op=0 param=0 data=0x636e746c`; `op=1 param=0 data=0` | endpoint `0x00`, `cntl`, no OOL | SEP mailbox control service |
| `op=0 param=0x10 data=0x78617273`; `op=1 param=0x10 data=0x08080808` | endpoint `0x10`, `xars`, 32 KiB in and out | XART endpoint, claimed by `AppleSEPXARTService` on iOS |
| `op=0 param=0x13 data=0x7861726d` | endpoint `0x13`, `xarm`; OOL half not captured | second XART endpoint, also claimed by `AppleSEPXARTService` |
| `ep=0 op=0x0d` | boot/liveness notification | independent of discovery |

`xars` and `xarm` are both XART transport endpoints. The live iOS registry
binds the same `AppleSEPXARTService` class to each. `xarm` is probably the
master-facing instance and `xars` the service/storage-facing instance, based
on the names and `AppleSEPXART::checkInMaster`; that expansion is an
inference, not needed for SBIO bring-up.

The raw-register sample
`ep=fd op=0 param=2 data=0x08080808` is not a valid name advertisement: its
data is not a 4CC. The log explicitly marks that register read as possibly
stale, and the split 32-bit mailbox reads can be stale/torn. Ignore it; it is
not evidence for endpoint `0x02`.

### Endpoints still expected

The live j71t iOS registry contains these actual `AppleSEPDeviceService`
nubs:

```text
cntl  xars  xarm  arts  scrd  sbio  sks  sse
```

Therefore `sbio` and `sks` should be in the discovery stream. This does not
mean every generic SEP personality in the kernelcache must be advertised on
this board.

Their absence from the screenshot is not a request/response stall. The
captured `xarm` name arrived even though the AP never published `xars` OOL
buffers, which directly shows that discovery continues without AP OOL setup.
The 12:09 status was also a mid-stream snapshot:

- pongoOS had retained 11 messages in a 16-entry ring;
- `recv_sts=0x3701` had the `0x20000` empty bit clear, so another message was
  waiting;
- pongoOS continued draining after the snapshot, and later warm kexecs do not
  recreate the boot-time advertisements.

The likely state now is that pongoOS consumed the rest and its small ring no
longer holds it. Linux may inherit no discovery messages at all. If so, do
not guess the SBIO endpoint and do not send a probe to candidates.

## 2. Linux bring-up after mailbox arming

### Required driver corrections before the test

The current `drivers/sep/sep-a9.c` is useful for the first smoke test, but the
following must be fixed in the test branch first:

1. Increase the endpoint namespace from `SEP_OEP_MAX=16` to all 256 IDs (or
   at least through `0xfc`). Endpoint `0x10` and `0x13` already disprove the
   current limit.
2. Parse and retain `0xfd/op=0` plus `0xfd/op=1` pairs. Record name, ID, and
   all four OOL page counts; expose them in debugfs. Do not consider a service
   usable after only the name message.
3. Allocate OOL buffers from the advertised counts. The fixed 16 KiB maximum
   is already too small for `xars`'s 32 KiB. Support different send and
   receive sizes rather than the current single-size `ool` interface.
4. Check mailbox TX readiness/backpressure before each write. OOL publication
   sends four control messages consecutively.
5. Match a completion on endpoint as well as tag and command. Decode a
   `tag=0xff` error from receive OOL offset `0x10`.
6. Treat the completion's `data` field as the payload byte count; the
   seven-byte OOL header is not part of that count. The current code subtracts
   seven and would truncate the 32-byte `0x2b` reply to 25 bytes.
7. Invalidate receive cache over `[recv, recv + 7 + payload_len)`. The current
   `[recv, recv + payload_len)` invalidation can leave the last seven payload
   bytes stale before copying `payload_len` bytes from `recv + 7`.

The 10 ms poller is sufficient for the two-command smoke test. An IRQ is not
required yet.

### Arm and collect

Boot the warm-kexec-safe kernel with the root shared-memory page at
`0x87adb0000` reserved `no-map` and `sep_a9.enable=1`. On Linux:

```sh
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
D=/sys/kernel/debug/sep-a9

# The unarmed status path must not touch SEP MMIO.
cat "$D/status"
echo 1 > "$D/start"
sleep 1
cat "$D/status"
cat "$D/log"
```

Drain until the mailbox reports empty and discovery has been quiet for at
least 100 ms. Build the endpoint table only from paired messages with the
same `param` ID. Find the entry whose 4CC is exactly `sbio`; its numeric ID
must come from this run, not from an old note. Endpoint `0x08` has not been
established for j71t.

There is deliberately no send to `cntl` in this discovery phase. If `sbio`
is not in a complete pair, stop the run. On the next cold boot either:

- enlarge the pongoOS retention ring to at least 256 entries and dump it only
  after discovery becomes quiescent; or
- have pongoOS pass its parsed discovery table/ring to Linux in reserved
  handoff memory.

Keep draining the hardware FIFO while recording it; disabling the pongoOS
drain risks FIFO overflow and is not the fix.

### Publish SBIO OOL buffers through `cntl`

Choose buffer sizes within SBIO's advertised ranges, preferably the maximum
advertised page count for the first test. Allocate physically contiguous,
page-aligned memory, clean the send buffer before SEP reads it, and publish
these four messages in this order for target endpoint `E`:

```text
ep=0 tag=0 op=0x04 param=E data=send_size_bytes
ep=0 tag=0 op=0x02 param=E data=send_phys_addr >> 12
ep=0 tag=0 op=0x05 param=E data=recv_size_bytes
ep=0 tag=0 op=0x03 param=E data=recv_phys_addr >> 12
```

These are the only `cntl` messages needed here. They configure SBIO's OOL
buffers; they do not unlock or continue endpoint discovery.

With the corrected debugfs implementation, the operator-facing form can be:

```sh
SBIO_EP=0xNN              # copied from the paired sbio advertisement
SBIO_SEND_SIZE=0xNNNN     # selected from advertised in range
SBIO_RECV_SIZE=0xNNNN     # selected from advertised out range

echo "$SBIO_EP" > "$D/sbio"
echo "$SBIO_EP $SBIO_SEND_SIZE $SBIO_RECV_SIZE" > "$D/ool"
```

Do not copy that three-field command to the present driver: it currently
accepts only `ep size`, parses both numbers as hexadecimal, and forces equal
directions.

OOL bindings appear to be sticky in a running sepOS, and XNU warns on an
attempt to replace them. After any Linux kernel has published SBIO OOL
addresses, another warm kexec must reserve and reuse the same physical pages
and hand off their metadata. Otherwise cold-reset sepOS. Reserving only the
root shared-memory page does not solve OOL reuse.

## 3. Minimal armed-mailbox to first SBIO response

The generic transfer framing for a single chunk is:

```text
AP -> SEP: ep=SBIO tag=0xfc op=command param=0 data=chunk_sequence
SEP -> AP: ep=SBIO tag=0xfc op=command param=0 data=chunk_sequence
```

The first/single chunk uses sequence zero. Both OOL buffers start with seven
little-endian `u32` fields: version, total length, offset, flags, reserved,
command, and chunk length. Payload begins at `+0x1c`; the mailbox `data`
field is not a byte length. Match the reply on `(endpoint, tag, command)` and
validate the OOL header before copying its payload. See `sbio-commands.md`
for the reconstructed multi-chunk sequence.

### A. Initialize SBIO

`IOSEPBiometricService::initSbioCommunication` sends command `0x73` with a
little-endian `u32 1` and expects an empty reply:

```sh
echo "$SBIO_EP 73 01000000" > "$D/xfer"
cat "$D/xfer"
```

Success is a completion from SBIO with `tag=0xfc`, `op=0x73`, and zero reply
payload. This is the first transport/liveness proof.

### B. GetProtectedConfig

The Mesa call passes the four-byte biometric user ID to SEP opcode `0x2b` and
receives 32 bytes. The captured user is 501; `value:0` in the trace is a
separate command-header field:

```sh
echo "$SBIO_EP 2b f5010000" > "$D/xfer"
cat "$D/xfer"
```

Success is `out_len=32` and exactly 64 hex digits after the seven-byte OOL
header has been removed. This proves bidirectional OOL and cache handling.

### C. Display status

There is no third SEP transaction for `DisplayStatusChanged`. In both traced
cases (Mesa command 20, values zero and one), `AppleMesaSEPDriver` only:

1. updates `_displayStatus`;
2. calls `checkSensorState`;
3. calls `checkAutoMatchConditions`.

Linux should represent this as a local display/sensor state transition and
event into the later biometric state machine. Do not map it to `0x09`.
Opcode `0x09` returns about 3234 bytes only during an active match/status
flow; sending it cold is state-dependent and adds no bring-up evidence.
`GetEnabledForUnlock` is likewise normally a cached one-byte Mesa result;
`0x21` is fetched only when the SEP's asynchronous background-result event
says data is ready.

## 4. Shortest observed enrol and match flows

These are protocol skeletons, not commands that can yet be run with dummy
zero payloads. The capture verifies opcodes and lengths, but redacts the
operation/configuration bytes.

### Enrol

1. Initialize SBIO (`0x73`) and establish the SKS/user-bag state expected by
   SEP. Configure/calibrate the Mesa sensor.
2. Send EnrollMode `0x03` with the real 68-byte userspace payload.
3. For each fingerprint frame:
   - acquire the real 19,668-byte encrypted frame from the Mesa SPI sensor;
   - send `0x06` with the corresponding real 151- or 167-byte frame config;
   - send `0x65` using the two-chunk protocol below;
   - service asynchronous notifications and the observed `0x07`/`0x22`
     result/progress path.
4. Repeat frames until SEP reports enrolment complete.
5. Persist each required catacomb component:
   - `0x6c` with 24 input bytes (PrepareSaveCatacomb); retain its variable
     OOL reply, observed from 236 bytes through 16,379 bytes;
   - `CompleteSaveCatacomb` copies that already-buffered reply to userspace;
     it is not a verified standalone SEP opcode;
   - `0x37` with 24 input bytes confirms the save;
   - `0x36` with four input bytes fetches the 32-byte catacomb hash (the Mesa
     API allocates 33 bytes).

Do not use `0x33` for CompleteSaveCatacomb. The trace directly identifies
`0x33` as GetCatacombId (`4` input, `16` output).

### Match

1. Initialize SBIO, load the catacomb/user state, and satisfy the SKS lock
   state gate.
2. Send MatchMode `0x25` with the real 68-byte payload.
3. Do not call the observed `0x23` blindly; it is still only a candidate for
   the SKS lock-state gate/event.
4. For each frame, send real `0x06` configuration followed by chunked `0x65`
   sensor data.
5. Dispatch unsolicited SBIO messages. Fetch `0x09` (about 3234 bytes) for an
   active-match result and `0x21` (about 3214 bytes) for a background result
   only when the corresponding notification/state says it is ready.
6. Send `0x0b` to cancel/end the operation when required.

Delete was not captured, but is not on the first-enrol/first-verify critical
path.

### Required chunk/completion state machine

The 19,668-byte `0x65` request uses a 16,356-byte payload limit:

```text
copy bytes 0..16355 to send OOL; clean cache
send tag=0xfc op=0x65 param=0 data=19668
wait for tag=0xfe op=0x65                       # next-chunk kick
copy bytes 16356..19667 to send OOL; clean cache
send tag=0xfd op=0x65 param=1 data=19668
wait for tag=0xfc op=0x65                       # final completion
```

`tag=0xff` is an error and carries the status word at receive OOL offset
`0x10`. Result transfers can themselves be multi-chunk. For example, the
16,379-byte `0x6c` result arrived as SEP `0xfc` with 16,356 bytes, AP `0xfe`
to request the next output chunk, then SEP `0xfd param=1` with the final 23
bytes. The receive side therefore needs full `0xfc`/`0xfe`/`0xfd` accounting
rather than treating every `0xfc` as a completed request.

## 5. What Linux still lacks

For the `0x73`/`0x2b` smoke test, polling plus correctly sized OOL buffers is
enough. A usable enrol/match path additionally needs:

- a complete endpoint-discovery table and warm-kexec handoff;
- mailbox TX-ready/backpressure handling;
- endpoint-aware completion matching and decoded `0xff` status;
- the `0xfc`/`0xfd`/`0xfe` multi-chunk kick/completion state machine;
- an unsolicited SBIO event dispatcher and operation state machine;
- real 68-byte EnrollMode/MatchMode payload layouts and real `0x06` payloads;
- Mesa SPI setup, calibration, IRQ handling, and 19,668-byte capture reads;
- catacomb serialization/storage plus SKS/keybag integration;
- persistent OOL allocation metadata across warm kexec;
- mailbox IRQ handling for normal operation.

Polling at 10 ms is acceptable for the first two synchronous commands, but
not for production enrol/match: it can miss timing expectations around sensor
frames and adds latency to unsolicited progress/result messages. IRQ support
should follow the synchronous smoke test, before attempting a real match.

## Evidence anchors

- Successful boot and partial discovery:
  `docs/sep/logs/20261026-1209-xnu-status-sepos-booted.log`.
- Captured SBIO framing and operations: `docs/sep/sbio-commands.md` and
  `docs/sep/traces/2026-10-24-first-enroll.md`.
- Live endpoint registry: `build/firmware/private/IOService.txt`, around the
  `AppleSEPManager` node.
- Discovery handler: kernelcache `0xfffffff0062de838` inside
  `AppleSEPDiscovery_init` (`0xfffffff0062de7f0`).
- OOL control sends: `AppleSEPManager_setSendOOLBuffer`
  (`0xfffffff0062ee000`) and `AppleSEPManager_setReceiveOOLBuffer`
  (`0xfffffff0062ee0ec`).
- SBIO init: `IOSEPBiometricService_initSbioCommunication`
  (`0xfffffff0066bb114`).
