# J71t N1 protocol and implementation evidence

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Round 3, 2026-10-22. Target: iPad6,11 / J71t / S8003, iPadOS 16.7.10
20H350, personality `C1F15,1`, `multi-touch,j72`. No device operations were
performed by this workstream. **Verified** below means verified in the
specified binary, saved registry dump, firmware asset, source, or build; it
means Linux device verification only when explicitly tied to an orchestrator
test log. **Inferred** marks an
implementation assumption that still needs the orchestrator's device test.

## Round 3 device verification and portrait mapping

**Verified, owner/orchestrator:** `b0270d0bb3` passed firmware, calibration,
descriptors and runtime touch with audio in integration, N1 at `spi1.0`.
The supplied20-second drag has9508 events /1151 frames, median16.667ms and
no SYN_DROPPED. Logs: `docs/test-logs/2026-10-22-touch2-*.txt`.
The complete timing correction set therefore works; individual changes
were not isolated to attribute the earlier failure to just one difference.

**Verified orientation:** owner-confirmed TL/TR/BL taps show X agrees
with portrait framebuffer and raw Y grows upward. Commit `6ca4db7756`
adds `touchscreen-inverted-y` for J71t only. After the existing signed-edge
clamp/origin normalization, the generic helper reports `Y=19538-Y`.
There is no X flip/swap. New DT output still requires a device capture.

**Implemented/audited:** normal default stage3, direct no-fallback firmware
loads before power-on, firmware metadata, DIRECT/type-B input at100 units/mm,
and synchronized IRQ/contact/power teardown including shutdown. See
[Round3 report](round3.md) for the source audit, exact capture statistics,
new tests and the explicit suspend/resume boundary. Cold default-stage3
boot, negative firmware latency and new cleanup are owner-run tests.

## Historical Round 2 finding and device boundary

**Verified, orchestrator logs:** stage1 detected HBPP and controller
`0x434d15c1`; stage2 uploaded the 1024-byte MtCl and 57016-byte image and
completed calibration transport with reply `0x4969`. Two probes then failed
with `-74`. See `docs/test-logs/2026-10-22-touch-stage2*.txt`.
The old driver returned that errno for E2/E3 or E6/E7 echo/checksum checks;
descriptor length, endianness and surface checks instead returned `-71`.
The old logs contain no offending packet, so they cannot establish which
echo/checksum check failed. Timing alone does not identify E2.

**Verified configuration bug:** N1 setup overrides the ADT inter-word gap
from 5000ns to **zero**. Round1 retained 5us. **Verified sequencing gaps:**
Linux also omitted the caller's extra 2ms before discovery, the 2ms GET
entry delay, and corrupt-report retries. Round2 corrects these while
preserving the verified wire layouts. The zero-gap correction is the
strongest candidate explanation for runtime rejection; its causal role in
the observed failure remains **inferred pending the next device test**.
There is no evidence for an alternate E2 format for `0x434d15c1`, a longer
execute-status poll, or an extra ATN_ACK after execute.

Full audits: [runtime](round2-runtime-re.md),
[transport](round2-transport-re.md). Clean kernel fix: `b0270d0bb3`, on top
of tested `e22fc2eb3f`. Control failures now name the command/report and
check, print expected/received echo and checksum, and dump the complete
offending reply at error level. `trace=1` also covers bounded boot/control
exchanges before runtime.

## Evidence and reproducibility

**Verified:** shared Ghidra identifies kernelcache SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Prior findings are in [touch-recon.md](../periph/touch-recon.md). New private
decompilation and instruction listings are under `build/touch/re/` and
`build/touch/runtime-re/`; transport evidence is under
`build/touch/transport-re/`. Apple binaries and calibration remain under
`build/`.

**Verified:** several true vtable targets are not functions in the shared
autoanalysis. `scripts/touch/ghidra-offline.py` defines only starts derived
from actual vtable pointers or AArch64 direct calls in a separate extracted
`AppleMultitouchSPI` project. It does not attach to or reopen the shared
project. Reproduce a missing-target decompilation with:

```sh
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec \
  ~/.local/share/uv/tools/ghidra-mcp/bin/python \
  scripts/touch/ghidra-offline.py fffffff005ea4e64 fffffff005ea5f58
```

**Verified:** the orchestrator supplied the DSC-extracted MultitouchHID
plugin and MultitouchSupport framework from this iPad. Parser addresses in
the framework below are original shared-cache virtual addresses, not kernel
addresses. DSC UUID: `B49C2BA3-3742-37B7-937C-24820C283B57`. Cross-image calls
are not automatically named by a standalone Mach-O import.

## Transport and power

**Verified, ADT / live registry:** SPI3 at AP physical `0x20a08c000`, IRQ191,
PMGR gate77. Touch reset GPIO75 is active low; LDO enable GPIO73; CS GPIO102;
ATN interrupt GPIO65; display-sync GPIO74. Analog supply is D2365 function
`pmuL(0x21f)`. The reference clock function is `TCLK(8,100,32768)`, not the
A10 J172 24MHz function. The matching iOS implementation is
`AppleMultitouchN1SPI` and `MTSPIBootloader_N1`.

**Verified, offline transport RE:** the legacy controller has a PIO FIFO
path, including byte loads/stores between the memory buffer and FIFO. N1
bootloader routines explicitly select either PIO or DMA; SIO DMA is not a
protocol requirement. **Verified, orchestrator stage2 logs:** the complete
PIO firmware/calibration upload was acknowledged on this unit. Runtime PIO frame reads and sustained
evdev output subsequently passed the Round3 device test.
The implementation does not boot SIO or upload SmartIO firmware.

**Verified:** the live arm-io clock table and AppleS8000IO lookup map
clock-id0x130 to index48, 61,538,461Hz. The selected SPI tuple is mode0,
8-bit, MSB-first, requested83ns period. The PIO driver rounds its divider
up to6, giving10,256,410Hz. See [transport notes](transport-notes.md) for
the lookup and controller addresses. SPI native-pin mux retention remains
**inferred** until tested.

**Verified, corrected in Round2:** the initial SPI tuple+12 contains a
5000ns inter-word delay, but selected N1 initializer `fffffff005ea0d98`
pushes this setup, selects period83ns, then calls `+0x7e8(0)`.
Setter `fffffff005e99ff4` writes setup+0x0c at `fffffff005e9a03c`.
`fffffff005b47a44` maps that field to input-clock ticks and
`fffffff005b48ddc` writes WORD_DELAY at+0x38. Thus the effective N1
inter-word gap is **zero** throughout boot and runtime; it is not restored
between execute and discovery. Round2 sets the SPI device minimum to zero;
the controller retains generic word-delay support. Tuple+16 supplies a common 10000ns
CS delay. `fffffff005b499cc` applies 10us after assertion and 10us before
deassertion. There is no separate inactive delay. This pinned Hoolock SPI
core applies GPIO setup twice, so DT 5000ns setup yields nominal 10us;
DT 10000ns hold applies once. Recheck that compensation if changing SPI core.

**Verified:** TCLK initializes a divider `floor(24000000/32768)=732` and
operates PMGR `+0x78000`, with disable bit31, enable bit19, busy bit18,
divider bits9:0, an 8us start delay and 100us stop delay. Relevant functions:
`fffffff006435fdc`, `fffffff006435f74`, `fffffff00643417c`.
The nominal output for that divisor is about 32786Hz.

**Verified:** D2365 `pmuL(0x21f)` is LDO index31, options2 (enable-only).
Its table entry controls register `0x031c`, bit0; voltage zero selects no
voltage programming. Functions `fffffff006bbe488`, `fffffff006bbe388`,
`fffffff006bbcdd8`. This is a PMIC regmap operation, not an AP MMIO address.

**Verified startup ordering:** analog then LDO (`fffffff005e985dc`),
5ms CS-helper delay, reference clock on, 15ms, REQ_WAKEUP `19 c1`, 10ms,
display-sync enable, S_CLK-high packet `1a a1` + `18 e1` seven times, reset
deassert, 15ms (`fffffff005ea100c`, `fffffff005ea0ecc`). The driver follows
this order; asserting reset before the cold power-on remains an **inferred**
initialization choice. GPIO74 display-sync uses peripheral route2, not an
output-high GPIO: iOS abstract mode3 writes mask0x270/value0x240. An explicit
S8000 pinctrl variant also reproduces the off-state mask0x27e/value0x202.
The analog operation changes only the enable bit; both enable and disable
are verified, and no voltage is programmed. Error unwinding holds reset,
stops display-sync/clock and disables LDO/analog. No factory-storage write
is involved.

## Bootloader packets

**Verified:** byte layouts in this section are the iOS SPI memory buffers.
The legacy PIO byte path is the reason Linux sends these bytes as 8-bit SPI
words. Sending the little-endian buffer as 16-bit SPI words would swap each
pair a second time.

**Verified:** controller 32-bit values use the byte order
`[value>>8, value, value>>24, value>>16]`. This is neither a normal BE32 nor
LE32. All checksums below are additive byte sums, not CRCs.

| Operation | Buffer and response | Evidence |
|---|---|---|
| HBPP detect | TX `1a a1`, then `18 e1` repeated 7 times; 16 bytes duplex. First two returned BE16 words must each be one of `18e1,1aa1,1f01,4879,4969,4ad1,4bc1`. | `fffffff005ea21a8`, validator `fffffff005ea2348`, constant `fffffff00547ec60` |
| ATN_ACK | TX `1a a1`, two bytes duplex; reply BE16. | `fffffff005ea23c0` |
| Register read | TX `1c 73`, encoded32 address, BE16 sum of four address bytes; 8 bytes duplex. Then result transaction TX `1a a1 18 e1 18 e1 18 e1`, 8 bytes duplex; value is encoded32 at RX+2. | `fffffff005ea2564`, `fffffff005ea248c`, constant `fffffff00547ec70` |
| Masked register write | TX `1e 33`, encoded32 address, encoded32 mask, encoded32 value, BE16 sum of preceding 12 operand bytes; 16 bytes duplex. Follow by ATN_ACK, require `0x4ad1`. | `fffffff005ea268c` |
| Execute | TX `1d 53`, encoded32 `0x10003400`, bytes `00 01 00 00`, BE16 sum of preceding 8 operand bytes; 12 bytes duplex. Bootloader waits 40ms; caller adds 2ms before discovery. | `fffffff005ea5fc8`, `fffffff005ea47f4`, `fffffff005e95b3c` |

**Verified:** Apple's register wrappers sometimes ignore transport errors;
the Linux implementation propagates them. An all-zero/all-ones identity is
rejected. Calibration checks transport after its final ACK, but Apple does
not establish a specific calibration-success ACK word.

## Firmware and same-unit calibration

**Verified:** `J71t.mtprops` / `C1F15,1` has a single `Constructed Firmware`
OSData, 57016 bytes, version `0x0210.bin`, `PreconstructedBootloadPacketType`
`Z2`. Its SHA256 is
`98f73566489fed958a04bb73b87f63d8abd6948b7ceacd89d1bc638e64b8998d`.
The word `Z2` selects an inherited preconstructed packet path; it does not
select the A10 HBPP14 boot sequence.

**Verified:** the matching single-OSData path `fffffff005ea4e64` sends the
whole asset without conversion, then ATN_ACK expecting `0x4bc1`, at most
five attempts. Its outer dispatch is `fffffff005ea4bf4`, selected through
N1 bootloader vtable `fffffff006e13a18`, slot+0x120, then slot+0x130.
The alternative array-of-packets path is `fffffff005ea4c28`; unconstructed
firmware uses `fffffff005ea4ffc`. Neither is selected by this plist.

**Verified:** this asset is exactly one packet:

```text
offset  length  meaning
0       2       18 e1 (NOP prefix)
2       2       30 01 (HBPP DATA)
4       2       BE16 payload word count: 0x37aa (57000 bytes)
6       4       encoded32 target address: 0
10      2       BE16 sum(bytes 4..9): 0x00e1
12      57000   payload, each adjacent byte pair already swapped
57012   4       encoded32 sum(payload bytes): 0x004e2905
```

**Verified:** N1 shared boot method `fffffff005ea47f4` orders its virtual
stages as follows. Success of every stage gates the next:

1. Slot+0x150, `fffffff005ea5bf0`: optional named calibration-key arrays,
   each with length and target address. Missing arrays are allowed.
2. Slot+0x148, `fffffff005ea55e8`: optional prox calibration. Missing data
   or version is allowed; no synthetic PxCl is needed.
3. Slot+0x140, `fffffff005ea52d0`: Calibration Data at the selected
   `cal-dl-addr` (`0x10009000`). This is the unit's MtCl.
4. Slot+0x120: selected preconstructed firmware packet as above.
5. Slot+0x118: N1 register calibration below.
6. Slot+0xa0: firmware execute, followed by 40ms.

**Verified:** calibration uses up to `0x3f0` (1008) payload bytes per packet,
rounded to four bytes by iOS. The actual 1024-byte MtCl therefore takes
1008-byte and 16-byte payloads, at `0x10009000` and `0x100093f0`. Prefix and
header match the firmware DATA packet: `18 e1 30 01`, BE16 payload word
count, encoded32 address, BE16 sum(count/address bytes), then adjacent-pair
swapped calibration and encoded32 additive payload checksum. Each packet
has five bounded attempts and requires `0x4bc1` after ATN_ACK. Builder:
`fffffff005ea5f58`; generic named-calibration sender:
`fffffff005ea5900`. No checksum incorporates uninitialized padding for the
actual four-byte-aligned MtCl.

**Implemented:** offline `scripts/touch/stage-firmware.py` checks firmware
identity, header length/address/checksum and payload checksum, then writes
unmodified `apple/j71t-n1.bin` and actual
`apple/j71t-n1-calibration.bin` under `build/firmware/lib/firmware/`.
The kernel accepts either `apple,n1-calibration` DT bytes (exactly 1024) or
the `apple,calibration-firmware` filename. Both firmware requests use
`request_firmware_direct()` so a missing file cannot invoke a 60-second
sysfs fallback. All-zero/all-ones calibration is rejected.

**Implemented:** loading the private calibration from initramfs avoids
changing m1n1. A later generic m1n1 port can copy the ADT's resolved
`multi-touch-calibration` into `apple,n1-calibration` on the Linux node.
It must copy the actual bytes, not the unresolved `syscfg/MtCl/...` string
or a different unit's record. Calibration never enters a committed DT or
patch. PxCl is absent and deliberately omitted, as allowed by the traced
optional stage.

## N1 register calibration

**Verified:** `fffffff005ea8830`, defaults `fffffff005ea8ab0`, inherited
properties `fffffff005ea6a50`, N1 overrides `fffffff005ea8b00`:

1. Read controller version at `0x10008ffc`.
2. Write `0x10003060 = 6089` (C1F15,1 override).
3. Write `0x1000305c = 0x20`.
4. Write `0x10003058 = 6`.
5. Write `0x10003000 = 3`, or `2` for version `0x434d11a0`.
6. Write `0x10003518 = 1` (C1F15,1 clk32 override).
7. Read `0x10003800` for the `SPI_APU_EN` diagnostic.
8. Send two bytes `1f 01`, sleep 65ms, perform ATN_ACK.

**Verified:** the last ACK's transport status is checked, its value is not
validated by Apple. The Linux log says **calibration transport complete**
and prints the status; that message alone is not a physical-calibration
success claim. All addresses here belong to the touch controller, not the
AP or PMIC.

## Runtime control and ATN

**Verified:** the N1 wake override `fffffff005ea75e8` sends 16 bytes: `ee`,
13 zeroes, `ee 00`, followed by 2ms. iOS suppresses duplicate wakes within
100ms. It then discovers properties through the shared byte protocol.

**Verified post-execute order:** the bootloader sends execute once, disables
the optional download platform function (absent on J71t), and sleeps40ms.
Caller `fffffff005e95b3c` marks firmware present and sleeps another2ms;
N1 initializer `fffffff005ea2854` resets EB sequence state and invokes
cacheProperties. E2 comes first, then D1/D3/D0/A1/D9. There is no intervening
status poll, ATN wait, SPI setup pop, or new ACK exchange. E2 permits four
attempts with3ms gaps. Each E2/E3 attempt and data GET invokes the
100ms-throttled wake helper. GET `fffffff005ea2e7c` sleeps2ms on entry;
the cache getters pass retry4, allowing five E3 and five outer GET attempts
with1ms retry waits. Linux uses a stricter **single aggregate five-attempt
budget per descriptor**, retrying echo/checksum failures only. Valid
unsupported/malformed descriptors and transport timeouts abort immediately.
E3 and E6/E7 interstage waits remain25us; E2 repeats immediately.

**Verified:** runtime control commands have a 16-byte stage-one buffer,
command at0, report ID at1 when applicable, zeroed remainder, and LE16 sum
of bytes0..13 at14. A second SPI transaction obtains the response; command
echo and response checksum must match.

| Command | Details | Evidence |
|---|---|---|
| E2 | Sent twice unchanged; response interface byte2 and LE16 max packet at3. | `fffffff005ea2880` |
| E3 | Report-info query, sent twice with 25us control delay; status byte2, LE16 report length at3. | `fffffff005ea2b70` |
| E6 | GET for length<12; byte2=0 and length LE16 at3 for stage1. Stage2 changes byte2 to1 but preserves original checksum. Both transfers16. Result data starts RX+3, checksum LE16 at14. | `fffffff005ea3110` |
| E7 | GET for length>=12; same stage1. Stage2 zeroes former checksum, changes byte2 to1 and moves original checksum to final2 bytes; length+5 transfer. Response data RX+3, checksum over length+3 bytes. | `fffffff005ea3374` |

**Verified:** descriptor initialization queries D1 (family), D3
(endianness, rows, columns, firmware version), D0 (region descriptor), A1
(region parameters), D9 (surface), and optionally D7 (buttons). Registry
properties already pulled from this unit report interface1, maxpacket2454,
endianness1, rows40, columns30, and surface descriptor
`99390000c94c000090ff90ffac38e24b` (physical14745x19657 with active edges).
Cache-properties method: `fffffff005e976e0`; full runtime notes provide
descriptor consumer addresses.

**Verified:** successful HBPP detection sets RepeatReadEnable, so N1 uses
EB rather than EA for length/data requests (`fffffff005ea0bb4`). Sequence
starts at1 and alternates1/2 after a successful frame read. Length request:
16 zero-initialized bytes, EB at0, sequence at1, LE16 sum(first14) at14.
Response top nibble must be E, LE16 length at1, LE16 sum(first14) at14.
Data request: same command/sequence with byte2=1, checksum computed over
first14 but placed in final2 bytes of a `length+5` transfer. Data response:
EA (or EB in repeat mode), header5 has byte sum zero modulo256, LE16
payload-plus-checksum length at2, payload starts5, payload checksum in final2
bytes. Methods `fffffff005ea1cf4`, `fffffff005ea1f24`, wrapper
`fffffff005ea1ab8`.

**Verified:** runtime IRQ processing does not send a separate bootloader
ATN_ACK; reading the EB frame is the runtime handshake. IRQ entry is
`fffffff005e953a8`. ADT flags0 translate to GPIO mode5 at
`fffffff006070294`, which is falling edge in the matching Linux GPIO
register map. The DT uses `IRQ_TYPE_EDGE_FALLING`; electrical active-low
ATN is an **inferred** line interpretation checked after the initial drain. The extracted framework identifies AF as face-detection
mode enable (`0x1f48f2238`), not a generic touch-report enable. It is not sent.

## Classic contact reports

**Verified, extracted MultitouchSupport:** classic frame IDs43/44/45 are
dispatched into `MTParse_BinaryFrameHeader` at `0x1f48ee708`, bounded by the
classic packet parser at `0x1f48ee900`. N1's parser type1/options16 is
distinct from the newer HID-over-SPI wrapper. Important header fields:

| Offset | Field |
|---|---|
| 0 | report ID |
| 2 | header length |
| 3 | report version |
| 4 | LE32 timestamp with this unit's endianness1 |
| 14 | LE16 image byte count |
| 16 | contact count, present when header length>=22 |
| 17 | contact record stride |

**Verified:** contact records begin at `header_length + image_bytes`; they
do not unconditionally begin at24. The framework checks the complete
header/image/contact extent. `mt_FillMTContactDirectFromBinary` at
`0x1f48f3b1c` consumes ID byte0, state byte1, signed LE16 X/Y at4/6,
major/minor unsigned LE16 at12/14, signed LE16 orientation at16, total
proximity (ZTot) unsigned LE16 at 18, density at 20, and force/pressure at 26
when present. The framework scales ZTot by 1/256; it is not the force field.
Linux exposes force at 26 only for stride>=28 and reports zero pressure
otherwise. It does not relabel proximity as physical pressure. Coordinates
and ellipse axes use 100 units/mm. States 3 (MakeTouch) and 4 (Touching) are
active; 5 (BreakTouch) and other hover/out-of-range states release the slot.
The framework supports 32 records; actual simultaneous contact capacity on
this hardware is untested. Linux validates stride>=20 for nonempty frames and the
full extent before emitting any events, rejects duplicate IDs, uses type-B
slots, and releases omitted contacts on frame synchronization. D9 active
edges are signed16 values at 8/10/12/14: left=-112, bottom=-112,
right=14508, top=19426. The driver clips to these edges and subtracts the
minima, yielding axes 0..14620 and 0..19538 before optional DT orientation.

**Verified since the first Linux captures:** portrait needs only Y inversion,
and sustained runtime PIO/evdev works. Practical maximum contact capacity and
new corrected-DT corner output still need unit validation. Parser dispatch, contact state semantics, edge normalization,
IRQ polarity and any remaining transport limitations are recorded in the
separate runtime/transport notes and the test plan; they must not be
silently treated as hardware-tested.

## Failure bounds and validation boundary

**Implemented:** hardware transfer deadlines belong to the legacy PIO
controller; upload retries are five, E2 attempts four, descriptor attempts
five in total, firmware fallback is disabled,
65/40/15ms protocol delays are finite, IRQ drain is at most eight frames,
and three immediate IRQ read/ATN failures disable the IRQ and release all slots.
Recovery is an explicit unbind/rebind, with reset asserted on failed probe.
No automatic infinite firmware reboot loop or suspend/resume claim is made.

**Verified by host tests:** `scripts/touch/test-protocol.py` compiles the
actual C word-order, firmware/calibration, IRQ-retry and contact-parser functions with
input stubs under ASan/UBSan. It exercises the real private firmware,
corrupt checksums, both 20/32-byte records, duplicate IDs, truncated/empty
frames, and 51,200 malformed exact-allocation packets. This checks memory
bounds and decoded fields; it cannot prove SPI timing or physical touch.

**Verified by host tests:** `scripts/touch/test-control.py` compiles actual
execute/wake/discovery/diagnostic helpers under ASan/UBSan. Independent
iOS-derived wire fixtures exercise the complete descriptor sequence,
post-boot/GET delays, 100ms wake suppression, shared retry limit, transport
abort, wrong interface/endian/surface/length, echo/checksum failures and
complete raw-packet dumps. Tracing is capped at64 boot/control exchanges
(32-byte previews) plus eight runtime headers per probe. Complete offending
control replies, up to517 bytes, are always dumped in32-byte chunks even
with tracing disabled. Firmware and MtCl payloads are never traced.

**Implemented:** `scripts/touch/mkinitramfs.sh` emits
`build/touch/initramfs-touch.gz` as **one gzip member**, containing the
unchanged original HoolockRD newc bytes followed by the touch-only firmware newc
archive. It validates single-member framing and every appended file.
Multiple gzip members are invalid for m1n1 handoff because each cpio member
would replace the previous initrd.
