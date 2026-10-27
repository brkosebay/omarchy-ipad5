# xART gate after the A9 sepOS boot

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Evidence is the 13:27 capture in
`docs/sep/logs/20261026-1327-xnu-discovery-capture.log`, the iOS 16.7.10
j71t kernelcache, the j71t ADT, and the live iOS registry dump. This is a
plan for the orchestrator; it does not touch the device by itself.

## Result

> **2026-10-26 tri53-sep5 correction:** the capability-0 xarm reply was
> consumed, but no later endpoints appeared. Kernelcache reconstruction shows
> that xarm produces no separate completion, so consumption was success rather
> than an incomplete exchange. The next missing XNU action is the independent
> `arts` MANIFEST transaction documented in
> [`docs/sep/arts-manifest.md`](arts-manifest.md). The earlier claim below that
> xarm alone was likely the discovery gate is superseded by that result.
>
> **20:10 follow-up:** the personalized arts MANIFEST also succeeded in
> isolation, with reply `0000000000070102`, but discovery still stayed at four.
> Both individual prerequisites are now proven; the next test combines them in
> XNU endpoint-initialization order. See
> [`docs/sep/arts-xarm-combined.md`](arts-xarm-combined.md).

The last two messages are not one request split across two endpoints.

- `ep=0 tag=0 op=0x10 param=0 data=0` is an unsolicited SEP power-state
  notification. XNU clears `SEPCameraDisable`, if set, and asks the
  `AppleSEPManager` to enter power state 1. It sends no mailbox reply.
- `ep=0x13 tag=2 op=0x0e param=0 data=0` is a SEP-driven xART request on
  `xarm`. XNU turns it into a success response whose byte 5 says whether the
  AP provides protected-data access. It carries no nonce, token, ART, or OOL
  payload.

For the Linux bring-up, the smallest truthful response is therefore the
single zero-length xarm reply with protected-data access disabled:

```text
ep=13 tag=02 op=00 param=00 data=00000000
raw=0000000000000213
```

The iOS-faithful j71t response advertises protected-data access instead:

```text
ep=13 tag=02 op=00 param=00 data=00000100
raw=0000010000000213
```

Do not send both. The first response is the recommended discovery probe with
the current Linux driver, because Linux does not yet provide persistent xART
storage. The second is appropriate only once Linux can service and persist
the storage requests which that capability may enable.

The capture makes it highly likely that sepOS gates later discovery on this
reply: it advertised exactly `cntl`, `xars`, `xarm`, and `arts`, issued the
xarm request, and then remained quiet. The XNU kernelcache cannot prove the
SEP-firmware side of that dependency. Seeing `scrd`, `sbio`, `sks`, and `sse`
after the reply is the decisive test.

## 1. Message format and kernelcache decode

`AppleSEPXART` interprets the eight-byte mailbox word as:

| byte | meaning |
|---:|---|
| 0 | endpoint |
| 1 | tag |
| 2 | request opcode / response result |
| 3..4 | little-endian OOL length |
| 5 | flags, or the protected-data capability for opcode `0x0e` |
| 6..7 | CRC16 for storage writes |

In the generic mailbox fields used by pongoOS and the Linux driver, this is:

```text
param = length & 0xff
data  = ((length >> 8) & 0xff) | (byte5 << 8) | (crc16 << 16)
```

### Endpoint 0 opcode `0x10`

`AppleSEPManager_ep0Receive` at `0xfffffff0062edac4` handles only untagged
endpoint-0 traffic in this switch arm. For opcode `0x10` it:

1. clears the manager's `SEPCameraDisable` state/property if it was set;
2. invokes the manager power-management method with state `1`;
3. returns without calling the mailbox send method.

It is not an xART message and Linux must not acknowledge it.

### xarm opcode `0x0e`

`AppleSEPXART_handleSEPDrivenMsg` at `0xfffffff0062f5188` handles xarm's
SEP-driven requests. Its opcode `0x0e` arm:

1. reads `/chosen/protected-data-access`;
2. reads `/chosen/sepfw-load-at-boot`;
3. checks the SEP provider's `sepfw-loaded` property;
4. changes byte 2 from request opcode `0x0e` to result `0`;
5. writes the boolean `protected-data-access` value to byte 5;
6. sends the same endpoint and tag back with length zero.

On this j71t, the ADT contains both `protected-data-access: 1` and
`sepfw-load-at-boot: 1`, and the live iOS registry contains
`sepfw-loaded = 1`. In that combination XNU saves the already-modified reply
instead of sending it immediately. The xART backing-store initialization at
`0xfffffff0062f5c78` later sends the saved word and clears the pending state.
That is why the iOS-faithful reply is capability 1 but is delayed until the
filesystem-backed xART service is ready.

This is distinct from endpoint `arts` and the 20-byte legacy ART value.
`xarm`/`xars` are the xART anti-replay storage protocol; `arts` is not the
answer to this request.

### OOL ordering and the two xART endpoints

`AppleSEPXARTService_start` at `0xfffffff0062f4ea4` accepts both names, but
constructs `xarm` as the SEP-driven instance and `xars` as the AP-driven
instance. For xarm, `AppleSEPXART_enable` schedules
`AppleSEPXART_initOOLBuffers` (`0xfffffff0062f4abc`). The latter allocates one
send and one receive buffer of exactly `0x8000` bytes, matching the captured
8/8/8/8-page advertisement. The SEP-driven receive handler waits for that
initialization before processing the request.

Thus the faithful order is:

1. publish xarm's 32 KiB send and receive OOL buffers;
2. initialize the AP xART backing store;
3. reply to xarm opcode `0x0e`.

The zero-length response does not itself consume OOL data, so a minimal
mailbox experiment could send it without OOL. The live Linux test should
still publish xarm first, matching XNU and leaving the endpoint ready for the
next SEP-driven storage request. xars is AP-driven and is not required for
this one reply; its buffers can remain unallocated until Linux initiates an
xART request.

## 2. Minimal responder and fresh-store behavior

### Stage A: continue discovery without claiming storage support

For the present driver, report capability 0. This is not an error reply: byte
2 is still result 0. It truthfully says that this AP software does not yet
offer protected xART storage. There is no token, blob, nonce, or CRC to
provide, and no `arts` operation is involved.

Exact response to the captured request:

```text
request:  13 02 0e 00 00000000    raw 00000000000e0213
response: 13 02 00 00 00000000    raw 0000000000000213
```

If that releases discovery, record all new endpoint `0xfd` pairs before doing
anything else. It confirms the gate without falsely promising persistent
storage.

### Stage B: a real empty/fresh xART store

Opcode `0x0e` does not ask whether a record exists. A fresh store becomes
relevant only if SEP follows with the storage opcodes handled by the same
xarm receive path:

| opcode | direction | operation |
|---:|---|---|
| `0x00` | SEP -> AP | fetch the USER/SEP xART record |
| `0x01` | SEP -> AP | save the USER/SEP xART record |
| `0x05` | SEP -> AP | fetch a UUID-keyed xART locker |
| `0x06` | SEP -> AP | save a UUID-keyed locker |
| `0x07` | SEP -> AP | delete a UUID-keyed locker |
| `0x0e` | SEP -> AP | protected-data capability / boot gate |

Byte 5 bit 0 selects the namespace: zero is `USER`, one is `SEP` in XNU's
logging and storage keys.

For an opcode `0x00` fetch when no main xART record exists, XNU's exact
not-found behavior is a *successful* response with 17 zero bytes in the xarm
send OOL buffer:

```text
send OOL [0x00..0x10] = 00 repeated 17 times
response endpoint      = 0x13
response tag           = request tag T
response result/op     = 0x00
response length        = 0x0011
response byte 5        = request byte 5 (USER/SEP namespace)
response CRC field     = request CRC field (normally zero for a fetch)
```

For the usual zero-CRC fetch this becomes:

```text
USER: ep=13 tag=T op=00 param=11 data=00000000
SEP:  ep=13 tag=T op=00 param=11 data=00000100
```

The next likely operation is opcode `0x01`, asking the AP to save the newly
generated record. Its payload is in xarm's receive OOL buffer, its length is
bytes 3..4, and the request's bytes 6..7 carry a CRC16 checked by XNU. The AP
must durably and atomically save the payload under the selected USER/SEP
namespace before replying. On success XNU sets result 0 and length 0, while
preserving the request's namespace and CRC bytes in the response word.

That means a useful “fresh store” is not merely an all-zero GET. It must at
least implement GET-not-found followed by durable SAVE and later GET of the
same bytes. For Touch ID beyond discovery it should also implement locker
GET/SAVE/DELETE (`0x05`/`0x06`/`0x07`). The backend can be a Linux file or a
dedicated restoreable partition; it must not use or modify syscfg, NVRAM, or
boot-firmware storage.

The current debugfs driver is enough for opcode `0x0e`. Its newly allocated
send OOL page is also zeroed, so an observed first opcode `0x00` can be
answered experimentally with the 17-zero response. It is **not** yet a real
store: it cannot expose and persist an opcode `0x01` receive buffer or serve
saved data back. Do not advertise capability 1 as the production behavior
until that SEP-driven dispatcher and backing store exist.

## 3. Historical 14:00 procedure (failed: wrapper not activated)

This section records the procedure used for
`20261026-1400-linux-xart-reply-unconsumed.log`; do **not** repeat it as
written. The four OOL-control words and the xarm reply entered the AP-to-SEP
FIFO but remained there for 40 seconds. The missing prerequisite was the
AppleA7IOPV2 wrapper activation described in section 6.

Run the AOP experiments and every intended warm kexec first. Publishing OOL
addresses is the final operation in this sepOS session: sepOS retains those
physical addresses, so no further warm kexec is safe afterward. End the
session with a cold reset.

This procedure is valid only for the still-running sepOS instance from the
13:27 capture. Its pending request tag is `0x02`. After any SEP reset or new
DFU boot, do not replay tag 2 from this document; use the tag captured in that
boot.

### Boot and arm Linux

```sh
ENTRY=tri53-sep-759c7ffcd4 \
  scripts/boot/kexec-host.sh build/boot/m1n1-linux-tri53-sep-759c7ffcd4.bin

scripts/sep/sep-dump.sh
scripts/sep/sep-dump.sh --arm
```

Apply all reservation and probe preconditions from
`docs/sep/linux-live-test.md`. Arming will probably find an empty receive FIFO
because pongoOS already drained the request; that is expected. sepOS itself
still holds the pending tag internally.

### Declare the captured endpoints, publish xarm, and reply

The command below uses capability 0, the recommended one-shot discovery
probe. `xfer` is deliberately not used: it invents a host-request tag and
waits for a completion, while this operation must answer SEP's existing tag
2 with the xART message layout.

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' <<'EOF'
set -eu
D=/sys/kernel/debug/sep-a9

# Exact op0/op1 advertisement pairs from the 13:27 capture.
echo "raw 00 636e746c 00000000" > "$D/advertise"
echo "raw 02 61727473 04040404" > "$D/advertise"
echo "raw 10 78617273 08080808" > "$D/advertise"
echo "raw 13 7861726d 08080808" > "$D/advertise"
cat "$D/endpoints"

# xarm only: exact 8-page send and receive buffers.
echo "13 8000 8000" > "$D/ool"
sleep 1

# Reply to this boot's pending xarm tag 2, result 0, capability false.
echo "13 02 00 00 00000000" > "$D/send"
cat "$D/status"
EOF

sleep 5
scripts/sep/sep-dump.sh
sleep 5
scripts/sep/sep-dump.sh
```

Do not retry the `ool` write in this boot, even if a later command fails. Do
not warm-kexec after it.

Success for this stage is new endpoint-`0xfd` advertisements, ideally the
known iOS set `scrd`, `sbio`, `sks`, and `sse`, followed by mailbox quiet. If
another xarm request arrives first, record its complete raw word and stop;
the next response must be derived from its opcode, tag, length, flags, and
CRC rather than guessed.

For a controlled iOS-faithful experiment, replace only the final send line
with the following, and be prepared to stop on the first storage request:

```sh
echo "13 02 00 00 00000100" > "$D/send"
```

Again, do not send the capability-0 and capability-1 replies in the same
sepOS boot.

## 4. Linux versus pongoOS

The response can be sent by Linux after the wrapper has been activated as in
section 6. PongoOS consumes the SEP-to-AP request word, but sepOS retains the
matching endpoint/tag in its state machine across the warm kexec. The 14:00
attempt did not test that statement successfully because Linux's AP-to-SEP
words never left the hardware FIFO.

pongoOS is therefore not required to answer opcode `0x0e`. For future cold
boots it only needs to preserve enough information for Linux to know the
current endpoint, tag, and advertisements, either by printing them as it does
now or by a reserved-memory handoff. Handling xART in Linux is preferable:

- xarm OOL buffers then belong to the kernel that will keep running;
- a persistent filesystem-backed store is available;
- SEP-driven SAVE requests can be cache-maintained and acknowledged only
  after durable storage.

A pongoOS responder is useful only as a temporary discovery experiment. It
must not allocate handoff-unsafe OOL buffers and claim capability 1, because
Linux would inherit SEP pointers to memory it does not own and no persistent
store would exist.

## 5. Driver work after the discovery proof

Before making capability 1 the default, add a SEP-driven xarm path separate
from the existing host-driven `xfer` path:

1. route unsolicited xarm tags/opcodes to a worker without consuming them as
   host transaction completions;
2. invalidate the receive OOL range before reading SAVE payloads;
3. verify the xART CRC16: CRC-16/CCITT polynomial `0x1021`, initial value
   `0xffff`, no final XOR;
4. atomically persist USER and SEP records plus UUID-keyed lockers;
5. clean the send OOL range before replying to GET requests;
6. echo the request tag and preserve the xART flags/CRC fields exactly;
7. acknowledge SAVE/DELETE only after durable commit;
8. keep OOL allocations valid for the complete sepOS lifetime, or require a
   cold reset before another kernel boot.

Until then, the capability-0 reply is enough to answer the question a newly
booted SEP asks and to test whether xART is the discovery gate.

## 6. A9 wrapper activation

### What the 15:00 retest disproved

The AOP result applies directly to SEP, not merely by analogy. The saved j71t
iOS registry shows `/arm-io/sep` as `compatible=iop,s8000`, role `SEP`, with an
`AppleA7IOPNub` and `AppleSEPManager` below the same `AppleA7IOPV2` provider
used by AOP. The SEP wrapper resource is `0x20da00000..0x20da0ffff`.

The relevant kernelcache paths are:

- `AppleSEPManager_sendMessageGated` at `0xfffffff0062f8f20` ultimately calls
  its AppleA7IOP provider's vtable slot `+0x590`.
- `AppleA7IOPV2::postMailbox` at `0xfffffff005b4e4a8` is only one 64-bit store
  to provider base `+0x4010`. There is no second SEP-specific kick register.
- `AppleA7IOP::start` at `0xfffffff005b4b738` calls the optional platform
  service `setActive(true)`, then the V2 helpers which configure both wrapper
  interrupt-mask views and enable the outbox.
- Those helpers are `0xfffffff005b4e530` (mask `0x1111` in both views),
  `0xfffffff005b4e548` (unmask bit 0), `0xfffffff005b4e5a8` (unmask bit 12),
  and `AppleA7IOPV2_setOutboxEnable` at `0xfffffff005b4e5d8`.

The transaction-width comparison corrects an earlier conclusion here. XNU's
`postMailbox` emits one 64-bit `str` at `+0x4010`; the then-current PongoOS and
`sep-a9.c` paths emitted two 32-bit stores to `+0x4010/+0x4014`. The 15:00 run
(`docs/sep/logs/20261026-1500-linux-wrapper-arm/`) still disproves the claim
that replaying the ordinary V2 mask sequence is sufficient: the primary mask
changed from `0x1111` to `0x110`, but the first split-store xarm OOL-control
word remained queued (`0x2cc01 -> 0xc001`). A missing wake edge caused by the
MMIO access width is now the leading transport hypothesis; there is still no
separate post-write doorbell in XNU.

Endpoint-0 opcode `0x10` is not a sleep or wake command. As decoded in section
1, it asks `AppleSEPManager` for PM state 1 and receives no mailbox reply. In
`AppleSEPManager_setPowerStateGated` at `0xfffffff0062f9df8`, a nonzero to
nonzero PM transition only updates bookkeeping. It does not account for the
five retained words.

### Exact Linux arm/activation sequence

Run this once when debugfs `start` changes the driver from unarmed to armed,
before starting polling or sending any mailbox word. All offsets below are
from SEP base `0x20da00000`:

```text
write32 +0x4000 = 0x00001111    # mask the four known primary sources
write32 +0x0c00 = 0x00001111    # same, mirrored wrapper view
write32 +0x4004 = 0x00000001    # unmask inbox/send notification, primary
write32 +0x0c04 = 0x00000001    # unmask inbox/send notification, mirror
write32 +0x4004 = 0x00001000    # unmask outbox/receive notification, primary
write32 +0x0c04 = 0x00001000    # unmask outbox/receive notification, mirror
v = read32(+0x4020)
write32 +0x4020 = v | 1         # AppleA7IOPV2 outbox-enable RMW
read32  +0x4020                 # flush the posted writes
MMIO barrier
```

In `sep-a9.c`, use `writel()`/`readl()` against `sep->base`; do not derive
`+0x0c00` from `sep->mbox`, which is already `base+0x4000`. Preserve every
other status bit in the `+0x4020` read-modify-write. Log primary and mirror
mask readbacks plus `+0x4008` and `+0x4020` after activation. On hardware that
reports the mask through the clear register, the old primary `0x111` should
become `0x110`; the two views are not guaranteed to have identical readback
aliases, so FIFO consumption remains the decisive result.

The call at `0xfffffff005b4b6f4` previously described as an optional platform
service is now resolved. `AppleA7IOP` field `+0x128` is filled at
`0xfffffff005b4b4f4` by `IOMapper::copyMapperForDevice(provider)` and
`setActive(true)` is invoked on that **IOMapper**. It affects DMA translation;
it is not a PMGR, wrapper, clock, or SEP-CPU wake operation and cannot explain
why the first inline mailbox word did not drain. It will matter if Linux later
uses mapped rather than physical OOL addresses.

In the warm handoff, still require
`/sys/kernel/debug/pm_genpd/pm_genpd_summary` to show `sep on` and PMGR
actual/target `0xf`. `0xf` is the active state on this PMGR generation. If the
domain is not on, abort rather than guessing a PMGR write. Do not write CPU RUN
at `+0x44`, the NMI pair at `+0xc004/+0xc024`, reset, auto-boot, or any SEP PMGR
register. Linux polling is sufficient to observe the proof, so AP-side AIC
interrupt registration is not required for this experiment.

### Why the `+0xc00/+0xc04` zero readback is not a failure predicate

The V2 helpers use the same wrapper mapping (`AppleA7IOP+0x100`) for the
primary and `+0xc00` ports; this is not a separately mapped bank. XNU writes
`0x1111` to both set ports and writes `1` and `0x1000` to both clear ports, but
its status formatter reads only `+0x4000/+0x4004/+0x4008/+0x4020`. It never
reads `+0xc00/+0xc04` back. The SEP result is therefore consistent with those
secondary set/clear aliases being write-only or read-as-zero. There is no
kernelcache evidence for a clock that must be enabled merely to retain their
readback. Keep writing them, but decide success only from FIFO consumption.

## 7. Nap protocol, failed split-store probes, and the next experiment

### Exact XNU decision and indication

`AppleSEPManager_sendMessageGated` at `0xfffffff0062f8f20` calls the nap-wake
path only when its private state at manager `+0xd4` equals 8. XNU enters state
8 when it sends untagged endpoint-0 opcode `0x12`; receiving endpoint-0 opcode
`0x10` does **not** set it. Opcode `0x10` only requests IOKit PM state 1 and
gets no mailbox reply. PongoOS sent no opcode `0x12` in the captured boot, so
the captured host-side sequence would not make XNU choose the wake path.

When state is 8, `AppleSEPManager_waitUntilCanSendMessage` at
`0xfffffff0062f92c0` does the following:

1. clear the wake flag at manager `+0x170`;
2. arm a 5-ms wait;
3. post raw word `0x00000000000e0000` twice, i.e.
   `ep=0 tag=0 op=0x0e param=0 data=0`;
4. sleep and accept SEP endpoint-0 opcode `0x0f` or `0x15` as the wake
   indication;
5. repeat for at most 200 waits (about one second) before returning timeout.

The mailbox receive loop at `0xfffffff0062f73a0` atomically sets `+0x170` and
wakes the waiter for either indication. Opcode `0x0f` is consumed there;
opcode `0x15` also continues into normal endpoint-0 dispatch and
`AppleSEPManager_osActiveWork`.

This state predicate means the nap exchange is not required by the observed
host transcript. The 15:42 Linux test and 16:18 PongoOS split both posted one
wake word; each remained queued. The Pongo result exonerates warm kexec, Linux
PMGR/genpd, m1n1 handoff, and AP AIC routing, but both senders used paired
32-bit stores rather than XNU's one 64-bit store.

### There is no additional SEP doorbell

`AppleA7IOPV2::postMailbox` at `0xfffffff005b4e4a8` performs exactly one
64-bit store to wrapper `+0x4010`. There is no SEP-specific per-message kick,
SEP interrupt-controller poke, or post-write doorbell in the XNU path. CPU RUN
`+0x44` is used for cold start and is skipped on resume. The
`+0xc004/+0xc024` NMI pair is a recovery path, not normal send. PongoOS boot
traffic could drain because SEPROM/early sepOS was polling; that does not prove
the later xART wait polls.

### Why the 16:42 `msg64` result is confounded

The 16:42 probe
([discovery](../release/OMISSIONS.md),
[wake](../release/OMISSIONS.md)) did execute the intended one
64-bit store and the AP→SEP word remained queued. However, the SEP→AP side was
already faulted before that store: discovery ended at `recv_sts=0x28801`,
while the first probe snapshot read `0xa8801`. Bit 19 in `+0x4020` is the V2
**outbox-underflow** latch.
`AppleA7IOPV2_outboxEmpty` at `0xfffffff005b4e578` treats either bit 18 or 19
as a fatal mailbox fault rather than a notification.

The trigger is deterministic in the old Pongo build. `pongo-sep.sh` queued a
`sep stat` after `sep xnu`; that command first sampled and printed the clean
status, then unconditionally read `+0x4034/+0x4038` even though bit 17 said the
outbox was empty. Thus the printed `0x28801` precedes the underflow caused by
the same command, and the next stat observes the newly latched `0xa8801`.

There is no discovery-message acknowledgement missing from Pongo. XNU's V2
receive primitive at `0xfffffff005b4e498` is one 64-bit load from `+0x4038`;
that FIFO pop is the whole hardware acknowledgement. The generic bulk reader
at `0xfffffff005b4bda8` advances eight bytes per successful pop and performs no
receive-side register write. The repeated 15-message block is therefore
consistent with sepOS reacting/retrying after an AP outbox fault, not evidence
that endpoint `0xfd` messages need mailbox replies. Whether the retry is caused
by that fault remains an inference until the clean run below.

### Exact next-DFU experiment

The new Pongo build makes `sep stat` status/ring-only: it never reads the FIFO
data window, and the linked build audit rejects any `+0x4034/+0x4038` load in
that command. Preserve the known-good split boot handshake so removal of the
empty-FIFO peek is the only transport change exercised:

```sh
scripts/sep/pongo-sep.sh --xnu status split32
scripts/sep/pongo-msg64-wake.sh
```

The second script issues two safe stats across a 65-second passive hold. It
refuses to send if either `recv_sts` has bit 18 or 19, or if AP→SEP is already
nonempty. Only a clean preflight sends one `sep msg64 0 0 e 0 0`, waits one
second, and takes one final safe stat. It never clears a fault and performs no
OOL, wrapper-mask, PMGR, RUN, NMI, or Linux transition.

- `RESULT pongo-msg64=drained`: the prior empty-outbox read was the blocker.
  Use the following cold boot for xarm OOL plus the captured-tag reply.
- `RESULT pongo-msg64=stuck` with no RX fault: the underflow was real but not
  causal for AP→SEP consumption. Stop and investigate the xART protocol state.
- `rx-fault-before`: no AP word was sent. Preserve the two status/ring snapshots;
  the fault has another trigger in the unchanged receive path.
- `rx-fault-after`, `busy-before`, or incomplete output: stop and cold-reset.
  Never retry after the `AP->SEP64` marker appears.

The default `sep xnu` path still supports an all-64-bit boot-handshake test,
but omit `split32` only in a later boot; it is not part of this discriminator.

## Reverse-engineering anchors

| item | kernelcache address |
|---|---:|
| `AppleSEPManager_ep0Receive` | `0xfffffff0062edac4` |
| `AppleSEPXART_xmsgReceived` | `0xfffffff0062f44d4` |
| `AppleSEPXART_initOOLBuffers` | `0xfffffff0062f4abc` |
| `AppleSEPXARTService_start` | `0xfffffff0062f4ea4` |
| `AppleSEPXART_enable` | `0xfffffff0062f5138` |
| `AppleSEPXART_handleSEPDrivenMsg` | `0xfffffff0062f5188` |
| deferred reply / backing-store initialization | `0xfffffff0062f5c78` |
| `AppleSEPManager_sendMessageGated` | `0xfffffff0062f8f20` |
| `AppleSEPManager_waitUntilCanSendMessage` | `0xfffffff0062f92c0` |
| `AppleSEPManager_setPowerStateGated` | `0xfffffff0062f9df8` |
| AppleSEPManager mailbox drain callback | `0xfffffff0062f73a0` |
| `AppleA7IOP::start` | `0xfffffff005b4b738` |
| `AppleA7IOPV2` one-word receive/pop | `0xfffffff005b4e498` |
| `AppleA7IOPV2::postMailbox` | `0xfffffff005b4e4a8` |
| `AppleA7IOPV2_outboxEmpty` / fault check | `0xfffffff005b4e578` |
| AppleA7IOPV2 mask both views | `0xfffffff005b4e530` |
| AppleA7IOPV2 unmask bit 0 / bit 12 | `0xfffffff005b4e548` / `0xfffffff005b4e5a8` |
| `AppleA7IOPV2_setOutboxEnable` | `0xfffffff005b4e5d8` |

The understood xART and SEP send/wake functions above were renamed in the
shared Ghidra project; the instruction anchors were commented and the
analysis database was saved.
