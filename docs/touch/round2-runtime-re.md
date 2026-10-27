# Round 2: N1 execute-to-runtime audit

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Offline audit, 2026-10-22. This is the runtime RE agent's handoff; it does
not claim a successful device test. No device commands or kernel edits
were performed by this agent.

**Result:** the inspected Linux execute, E2/E3 and E6/E7 packet bytes match
the selected iOS N1 path. Verified differences are the provider's
interword delay, an additional 2ms pause before discovery, the GET
helper's 2ms entry delay, wake placement/throttling, and report retry
behavior. The first is a transport setting correction; the others are
runtime sequencing corrections. Which difference caused the observed
failure remains **inferred** until another device test supplies tagged
responses.

## Evidence and source snapshot

- **Verified:** shared Ghidra target is the iOS 16.7.10 20H350 j71t
  kernelcache, SHA256
  `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
  `get_program_info` was called before RE.
- **Verified:** N1 SPI vtable is `fffffff006e12fe8`; N1 bootloader vtable is
  `fffffff006e13a18`. The virtual targets below were checked against these
  tables, rather than selected from a neighboring Z2/legacy implementation.
- **Verified:** the actual `a9/touch` driver was read through
  `scripts/build-kernel.sh --run touch`, without modifying the tree.
  Snapshot: `build/touch/runtime-re/round2/apple_n1-before-runtime-fixes.c`,
  SHA256 `780e35461e166f4889cee256f2bc30ad989e06e3e34ee32ff50c3020ecf3f9ec`.
  The parent is implementing fixes concurrently; this hash identifies
  the inspected revision, not the final staged driver.
- New decompilation evidence, selected raw instructions, and hashes are
  under `build/touch/runtime-re/round2/`. Existing missing-function
  decompilations are in `build/touch/re/`. Raw instructions settle cases
  where the private decompiler merges adjacent functions.
- Shared Ghidra functions were renamed for provider period/interword
  setters, configuration push/pop, enable_download, and bootload timer;
  comments were added at the relevant N1 initialization/runtime entries.

## Execute and selected controller path

**Verified:** cached live `build/firmware/private/IOService.txt` identifies
`AppleMultitouchN1SPI`, merge personality `C1F15,1`, constructed firmware
`0x0210.bin`, and `fw-execute-addr=268448768`, which is `0x10003400`.
The driver uses that address correctly.

**Verified:** bootloader slot `+0xa0` selects `fffffff005ea5fc8`. Its
12-byte execute packet for this property is:

```text
1d 53 34 00 10 00 00 01 00 00 00 45
      | address | | value   | |sum|
```

The special boot address encoding is bytes `34 00 10 00`; the following
four bytes are the literal little-endian `0x100`, `00 01 00 00`. The last
two bytes are the big-endian byte sum over offsets2..9, `0x0045`.
Linux's `n1_execute()` matches. iOS sends this once and does not read a
separate execute ACK or poll an HBPP status afterward.

**Verified:** `fffffff005ea8830` reads controller identity at
`0x10008ffc`. At `fffffff005ea8930`–`fffffff005ea8944`, identity
`0x434d11a0` overrides the configured clock-enable register value with2;
every other identity, including the reported `0x434d15c1`, uses the
configured value3 for this personality. This branch does not select an
alternate runtime E2 format. The current Linux `version == 0x434d11a0 ?
2 : 3` matches that distinction.

**Verified:** after REQ_CALIBRATE and 65ms, iOS checks transport success
for the two-byte ATN_ACK exchange, without validating its returned
word. Thus the reported calibration status `0x4969` is not rejected by
iOS's selected N1 calibration routine. It also does not independently
prove that firmware later executed successfully.

## Exact transition to discovery

**Verified:** the selected call chain is:

1. `fffffff005ea19a4` checks HBPP and invokes the selected bootloader.
2. Shared N1/Z2 `bootloadDevice()` at `fffffff005ea47f4` calls
   `enable_download(1)` via SPI slot`+0x8c0`, then optional base/proximity
   calibration, MtCl upload, firmware upload, N1 calibration, and execute.
3. Immediately after execute, it calls `enable_download(0)`, then sleeps
   **40ms** on success. Slot`+0x8c0` is `fffffff005ea1780`, which resolves
   the optional ADT `function-enable_download`. The cached J71t child has
   no such property; its absent-function error is ignored by this caller.
   This is not CS handling or a provider configuration restore.
4. `bootloadTimerFiredHandler()` at `fffffff005e95b3c` marks firmware
   present, notifies registered clients with `(1,0)`, sleeps another
   **2ms**, then calls N1 slot`+0x608` at `fffffff005ea2854`.
5. The N1 cache initializer sets its runtime frame command/sequence state
   (`EB` when repeat support was established, sequence1), then calls
   common `cacheProperties()` at `fffffff005e976e0`.
6. Common cacheProperties first constructs the registry Multitouch ID
   from the phandle via `fffffff005e977e4`—there is no SPI exchange here—
   then queries E2, D1, D3, runs the registry-only serial-number
   slot`+0x630` (`fffffff005e97c58`), then queries D0, A1, D9.
7. Only after successful caching does the timer call default-report
   setup slot`+0x5a0` and notify clients `(2,0)`. This is not a prerequisite
   for the first E2 response.

**Verified:** setting firmware-present at `fffffff005e990b8` updates
accounting and schedules a reset timer. Slot`+0x5c8` at
`fffffff005e993a8` updates uptime counters. Notification slot`+0x6c8` at
`fffffff005e98160` delivers messages to registered clients through
`fffffff005e9d1e4`, which enqueues a small notification packet. These
functions contain no provider SPI configuration changes or required
controller commands on the selected path.

**Verified correction to Round1:** selected hardware init at
`fffffff005ea0d98` pushes the provider configuration, sets period83ns,
and sets **interword delay0**, not CS delay0. Setter`+0x7e8` at
`fffffff005e99ff4` stores its argument to provider-configuration
DWORD`+0xc` (`str w19,[sp,#0xc]` at `fffffff005e9a03c`). Period setter
`+0x7f8` at `fffffff005e9a0c8` updates DWORD`+4`. Push/pop are
`fffffff005e9a138` / `fffffff005e9a1c0`. There is **no selected N1 config
pop between execute and discovery**. CS DWORD`+0x10` remains10000ns.
The transport agent traced DWORD`+0xc` through Samsung setup to
WORD_DELAY register`+0x38`; see [round2-transport-re.md](round2-transport-re.md).

## E2 and wake bytes

**Verified:** slot`+0x618` at `fffffff005ea2880` performs exactly four
E2 attempts. Every attempt calls wake slot`+0x918`, then sends these
same16 bytes twice, with distinct provider transfers and no explicit
control delay between them:

```text
e2 00 00 00 00 00 00 00 00 00 00 00 00 00 e2 00
```

Only the second response is interpreted: `RX[0]` must equal`0xe2`,
`LE16(RX+14)` must equal the sum of the first14 bytes, interface version
is`RX[2]`, maximum packet is`LE16(RX+3)`. Failure sleeps3ms before the
loop continues; iOS also sleeps after the final failed attempt. The
Linux checksum/echo test is correct. iOS does not use HBPP word swapping
for these runtime packets.

**Verified:** N1 wake at `fffffff005ea75e8` uses:

```text
ee 00 00 00 00 00 00 00 00 00 00 00 00 00 ee 00
```

It suppresses a wake when fewer than100000µs have elapsed since the last
sent wake. When sent, it sleeps2ms and records the wake time. Its return
data is not validated. Calling wake once per E2/report attempt therefore
does not imply transmitting repeated EE packets inside100ms.

## Report helper and retry audit

**Verified:** all five cached report getters call gated wrapper
slot`+0x6b0` (`fffffff005e964c4`) with retry argument4 and an initially
zero two-byte flags structure: `allowKill=false`, `allowCached=false`.
It gates to N1 GET slot`+0x6a8`, `fffffff005ea2e7c`. The **getter itself**
sleeps2ms at `fffffff005ea2ec8`–`fffffff005ea2ecc`, on every invocation;
this delay is not in a userspace syscall wrapper.

**Verified:** GET then asks E3 report-info slot`+0x698`,
`fffffff005ea2b70`, with retry argument4 and the cache flag. E3 builds
`{e3,id,zeros...,LE16(e3+id)}` and sends it twice, with the configured
25µs delay between stages. Echo/checksum tests and response status at2,
length at3 are implemented correctly in Linux. E3 retries corrupt echo
or checksum recursively with `retry-1` after1ms: argument4 means **up to
five E3 attempts**. Direct transport errors leave this helper; the outer
GET can retry them.

**Verified:** after successful supported E3 info (status0, length<=512),
GET calls the wake helper again, then chooses E6 for length<12 or E7
otherwise. E6 (`fffffff005ea3110`) and E7 (`fffffff005ea3374`) match the
current Linux framing:

- First TX16 starts `{cmd,id,0,lengthLE16}`; byte-sum over these first5
  bytes is LE16 at14. Send, then delay25µs.
- Second TX changes byte2 to1 **without recomputing that checksum**.
  E6 keeps its16-byte transfer/checksum position. E7 clears offsets14/15
  and moves the original checksum to offsets`length+3/length+4`, then
  transfers`length+5` bytes.
- Response echo is the E6/E7 opcode; data begins at3. The final two
  bytes are the little-endian sum of all preceding response bytes.

**Verified:** outer GET retries transient errors with1ms between
recursive calls, retry4..0: **up to five GET invocations**. Each starts
with the2ms pause. Recursive retry preserves allowKill and sets
allowCached=true. Unsupported and no-power errors are not retried. E3
and GET have separate nested budgets in iOS; faithfully reproducing
both can yield up to25 E3 transactions for a persistently corrupt report
info response. A single shared five-attempt Linux budget is a deliberate
bounded policy deviation, not the exact iOS count.

## Implementation recommendation and remaining uncertainty

1. **Verified correction:** set N1 interword delay to0; retain the10µs
   CS delay. The initial driver's5µs interword minimum differs from iOS.
2. **Verified correction:** preserve40ms after execute and add the
   caller's2ms before initial wake/discovery. Add the GET helper's2ms
   entry pause and1ms transient retry spacing.
3. **Verified correction:** call the100ms-throttled wake helper at each
   E2/report-info attempt and before report data retrieval. Keep all
   attempts and SPI waits bounded.
4. **Verified need for diagnosis:** tag failure by operation/phase and
   report ID; capture the failing16-byte response plus expected/received
   checksum and echo, under a bounded trace budget. The existing generic
   `-EBADMSG` cannot distinguish E2 from E3/E6/E7.
5. **Inferred likely fix candidate:** zero interword delay removes a
   verified wire-timing mismatch that can affect strict runtime framing
   while the HBPP loader remains tolerant. The missing initial2ms may
   also expose a readiness race. Neither explanation is proven by the
   reported50.8ms interval.

The next owner-run stage2 test should first establish whether E2 returns
a checksummed interface1/max-packet2454 reply, then identify the first
failing descriptor if discovery advances. A still-HBPP-looking response,
all-zero response, wrong echo, and correct echo with bad checksum imply
different next investigations; the current log does not distinguish
them. No arbitrary polling extension or relaxed checksum/echo validation
is justified by this offline audit.
