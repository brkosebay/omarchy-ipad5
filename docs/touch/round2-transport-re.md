# Round 2: post-execute E2/E3 wire comparison

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Offline RE only; no device access or kernel edits. Compared the
current `a9/touch` N1 driver at inspected head `e22fc2eb3f` with the matching
iOS kernelcache, identified through shared Ghidra `get_program_info` as
SHA256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Existing private decompilation in `build/touch/re/` and the instruction
listing `build/touch/runtime-re/spi-disassembly.txt` were checked against the
shared program. No private Ghidra project was reopened for this comparison.

**Verified material difference:** selected N1 initialization overrides the
provider's inter-word delay to **zero**, while the current Linux N1 driver
retains the original ADT value of **5µs per byte**. The underlying register
conversion is correct; applying the original tuple after N1's explicit
override is wrong. This supplies a concrete correction to test.

**Verified framing result:** Linux E2/E3 packet construction, repeated
exchange count, byte order, chip-select boundaries and second-response
validation match the selected iOS methods. No change to those bytes or
exchange boundaries is justified. **Inferred impact:** the additional5µs
gaps may be tolerated by HBPP and rejected by the running firmware. The
available hardware logs contain no failing control opcode or returned bytes,
so the timing mismatch is not yet proven to be the exact cause of
`-EBADMSG`.

## E2 device info

**Verified, iOS `fffffff005ea2880`:**

1. Call the N1 wake virtual `+0x918` at `fffffff005ea28e0`–`28ec`.
2. Zero sixteen TX bytes, set byte0 to E2, and place the little-endian
   additive sum of TX0–13 at TX14–15 (`2918`–`292c`).
3. Exchange sixteen TX/RX bytes through provider `+0x578`
   (`2948`–`296c`).
4. Exchange the **same unchanged** sixteen TX bytes again
   (`29b8`–`29dc`). There is no inserted 25µs wait, command high-bit
   modification, read-stage marker, or separate bootloader ATN_ACK.
5. Ignore the first response for protocol validation. Check the second
   response RX0 equals E2 (`2a0c`–`2a18`), and LE16 RX14–15 equals the
   additive sum of RX0–13 (`2a1c`–`2a30`). Interface version is RX2;
   maximum packet size is LE16 RX3–4 (`2b58`–`2b64`).
6. Permit four attempts. Failed attempts sleep3ms (`2ac0`–`2ad0`).

The command bytes, written explicitly, are:

```text
e2 00 00 00 00 00 00 00 00 00 00 00 00 00 e2 00
```

**Verified Linux comparison:** `n1_command16(n1, 0xe2, 0)` builds this
packet, sends it twice without an explicit interstage wait, validates only
the second echo/checksum, and `n1_discover()` retries four times with a
3–4ms sleep. `memset(rx,0,len)` before each Linux transfer is not a wire
difference: all sixteen full-duplex RX bytes are replaced by the transfer.

One policy difference is that Linux calls `n1_wake()` once before the loop,
while iOS invokes its wake helper at each attempt. **Verified relevance
limit:** N1's helper at `fffffff005ea75e8` sends EE only when at least about
100ms has elapsed since its previous wake. It writes EE at TX0 and LE16 EE
at TX14 (`768c`–`7694`), performs one16-byte exchange (`76b4`–`76d8`),
then sleeps2ms (`7724`–`7728`). Linux builds the same EE packet and waits
2–3ms. **Inferred:** for four short attempts within100ms, both normally
send one wake packet. This difference does not by itself explain the
observed failure; an unusually long scheduler or transport delay could make
the retry behavior differ and should be logged rather than assumed.

## E3 report info

**Verified, iOS `fffffff005ea2b70`:** an uncached request calls the same
wake helper (`2bec`–`2bf8`), zeroes TX16, sets TX0=E3 and TX1=report ID,
then stores LE16(E3+ID) at TX14–15 (`2c34`–`2c50`). It exchanges TX16/RX16
(`2c74`–`2c98`), calls the microsecond delay helper with `this+0xa60`
(`2cc8`–`2ccc`), then exchanges the **same unchanged** TX16 again
(`2cd0`–`2cf4`). The established default for this field is25µs. It checks
the second echo and additive checksum, returns status RX2 and LE16 length
RX3–4. The E3 implementation may retry or return cached metadata; neither
changes the first uncached request's two-exchange wire form.

For the first Linux report query, D1, the expected command is:

```text
e3 d1 00 00 00 00 00 00 00 00 00 00 00 00 b4 01
```

**Verified Linux comparison:** `n1_command16(n1,0xe3,id)` constructs the
same bytes, uses the25µs gap and second-response validation. E3 does not
use the E6/E7 second-stage TX2=1 convention. Adding that marker to E2/E3
would contradict the selected iOS code. An E3 validation failure returns
the same Linux `-EBADMSG` as an E2 validation failure. Later E6/E7 reply
checks can also return that errno in the same `n1_discover()` call.

## Chip select and repeat-read semantics

**Verified provider call chain:** AppleARMSPIDevice's virtual `+0x578`
resolves to `fffffff0059d2404` (instance vtable `fffffff006cf28b8`). It
forwards raw TX/RX pointers and lengths to virtual `+0x558`,
`fffffff0059d2160`. Instructions `59d2428`–`243c` place the caller's final
two zero arguments on the stack and supply zero DMA descriptors. The
builder stores the final options argument into command+0x80 at
`59d22cc`–`59d22d0`. For E2/E3 this value is zero.

The Samsung PIO method at `fffffff005b485d0` asserts CS through virtual
`+0x620` before each exchange. At the end it leaves CS asserted only if
command+0x80 bit0 is set; with the verified zero flags it deasserts CS.
The concrete CS method `fffffff005b499cc` applies setup after assertion and
hold before deassertion, as documented in `transport-notes.md`.

Therefore **each E2/E3 stage is a separately selected SPI transaction**.
Linux's two separate `spi_sync_transfer()` calls have the correct boundary.
Combining them under one held CS, adding a third exchange, or substituting
RX-only clocking is not supported by this iOS path.

**Verified repeat-read setting:** successful HBPP finish at
`fffffff005ea0c48` stores1 at the touchscreen object's `this+0x3d8`.
Runtime initializer `fffffff005ea2854` reads that byte and writes opcode
EA or EB at `this+0xa59`, plus initial sequence fields0x0101 at+a5a.
This flag belongs to the touchscreen object, not AppleSamsungSPI.
E2/E3 methods do not consult it. Its selected-frame impact is EB instead
of EA and alternating sequence1/2, already implemented by Linux.
No controller FIFO receive-discard mode, extra E2/E3 exchange, CS hold,
clock phase change, or 16-bit word switch follows from setting this byte.

**Verified ATN boundary:** E2/E3 call wake and SPI transfers; they do not
read the touch ATN GPIO or issue bootloader command1a a1. The runtime EB
frame read is a separate protocol path. There is no evidence for inserting
an ATN_ACK before an E2 retry. The runtime workstream owns tracing any
post-execute caller behavior outside these methods.

## Current transport timing and limits of the failure evidence

**Verified N1 override:** hardware initialization `fffffff005ea0d98` first
pushes the whole SPI provider configuration through virtual+0x800, sets
period83ns through+0x7f8 (`0dc0`–`0dc4`), then calls+0x7e8 with zero
(`0dcc`–`0dd8`). The N1 instance vtable resolves+0x7e8 to
`fffffff005e99ff4`. This setter obtains the32-byte provider setup through
+0x548, writes its argument to **setup+0x0c** at instruction
`fffffff005e9a03c`, and reapplies setup through+0x550. It does not write
the common CS delay at setup+0x10.

**Verified register meaning:** `fffffff005b47a44` loads setup+0x0c at
`47af8`, multiplies by the input-clock rate and rounds up to cycles,
storing instance+0x184 at `47b04`. Prepare `fffffff005b48ddc` writes that
field to **WORD_DELAY(+0x38)** at `48e00`–`48e14`. Therefore the N1
override is an inter-word-delay override, not a CS-delay override. The
common CS value10000ns remains unchanged. The selected shared boot wrapper
does not reconfigure this field around calibration/execute; the runtime
workstream independently traced the setter and wrapper.

**Verified Linux mismatch:** current N1 probe sets byte-wide SPI and a
device minimum inter-word delay of5µs; the core propagates it to transfers.
The S8000 driver correctly converts it to308 input-clock cycles at
61,538,461Hz. That is about5.005µs between bytes instead of iOS's zero.
The explicit correction is to set the N1 device minimum to zero before
its first transfer, retaining the generic controller's word-delay support
for clients that request it. No kernel changes were made by this RE task.

DT requests mode0 and12,048,192Hz, selecting divider6. DT setup5000ns is
deliberate because the pinned Linux core applies GPIO CS setup twice;
hold10000ns is applied once and there is no inactive-delay property.
Those CS settings match the still-active common CS delay. Physical SCK,
inter-word delay and CS edges were not measured in this round.

**Verified logs:** both `2026-10-22-touch-stage2.txt` and the trace rerun
show firmware/calibration upload completion, calibration reply4969, then
`N1 stage2 failed: -74`. The calibration-to-failure gaps are50.802ms and
50.840ms. No E2/E3 RX bytes, failed opcode, checksum operands, or ATN level
are present. The `trace` flag only logs successfully checksummed EB frame
headers later; it cannot diagnose this control failure.

**Inference boundary:** the timing alone cannot identify E2. Current source
contains a40ms execute sleep,2ms wake sleep and up to four3ms E2 retry
sleeps, before adding transfer/scheduler overhead. Nominal all-four E2
failure timing is at least about54ms; actual scheduling, kernel timer
behavior and whether the shipped artifact matches inspected source must
be considered. E2 success followed by an E3/E6/E7 checksum failure is also
consistent with the shared errno. Neither an opcode failure nor a firmware
execution failure can be asserted as the exact cause from these logs.

The smallest discriminating next capture is bounded control tracing:
command, argument, attempt, transfer status, both sixteen-byte responses
for E2/E3, second-response echo, stored checksum and calculated checksum;
plus stage labels before/after wake, execute and each report request.
This prints protocol headers only, not firmware or calibration. It needs
no additional hardware transaction and must preserve the existing retry
bounds. If the second E2 reply is a bootloader word stream rather than an
E2 frame, execution/state-transition investigation becomes justified. If
E2 is valid and D1's E3 fails, it narrows to the next control transaction.
If echo is right but checksum wrong, retain all16 bytes before considering
alignment or electrical timing. Do not change framing speculatively.
