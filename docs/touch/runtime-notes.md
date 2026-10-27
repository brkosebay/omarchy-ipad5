# N1 runtime evidence and implementation handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Offline reverse engineering only. No device access by this agent.
**Verified** means code or an already-pulled unit property was checked; it does
not mean Linux hardware validation. **Inferred** marks a porting choice or an
unresolved hardware meaning.

## Sources and reproducibility

- Kernelcache SHA256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
  Shared Ghidra was identified with `get_program_info`. Missing selected
  functions were defined/decompiled in the parent's private auxiliary project;
  evidence is `build/touch/re/<address>.json` and `.asm`. Shared analysis was
  not replaced. No debugger attachments were made by this agent.
- Existing AppleMultitouchSPI source sections from `build/periph/kexts/`
  were disassembled into `build/touch/runtime-re/spi-disassembly.txt` when
  shared Ghidra lacked function definitions.
- Orchestrator pulled the matching `MultitouchHID.plugin/MultitouchHID`.
  Its contact callbacks import decoding from MultitouchSupport. The framework
  was extracted from the already-downloaded dyld cache, UUID
  `B49C2BA3-3742-37B7-937C-24820C283B57`, without modifying the cache:

  ```sh
  ipsw dyld extract build/dsc/com.apple.dyld/dyld_shared_cache_arm64 \
    /System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport \
    -o build/touch/runtime-re/framework
  ```

- Private framework Ghidra evidence is
  `build/touch/runtime-re/MultitouchSupport-<address>.json` / `.asm`;
  `framework-symbols.txt`, `framework-disassembly.txt`, and
  `decompile-user.py` preserve the method. Apple code/binaries remain in
  ignored `build/` only.

## Selected runtime, commands, and interrupt transaction

**Verified:** the N1 instance vtable at `fffffff006e12fe8` selects the
shared Z2 methods below. The legacy 0xAA-response implementation at
`fffffff005e966fc` / `fffffff005e96c0c` is not the selected N1 path.

| N1 vtable offset | Address | Operation |
|---|---|---|
| `+0x5e0` | `fffffff005ea1cf4` | result-length read |
| `+0x5f0` | `fffffff005ea1f24` | result-data read |
| `+0x600` | `fffffff005ea1ab8` | complete one-frame transaction |
| `+0x608` | `fffffff005ea2854` | reset runtime sequence, cache properties |
| `+0x618` | `fffffff005ea2880` | device info E2 |
| `+0x698` | `fffffff005ea2b70` | report info E3 |
| `+0x8d0/+0x8d8` | `fffffff005ea3110` / `fffffff005ea3374` | E6/E7 report read |
| `+0x8e0/+0x8e8` | `fffffff005ea3a74` / `fffffff005ea3c14` | E4/E5 report write |
| `+0x8f0` | `fffffff005ea3ec8` | E1 command status |
| `+0x910/+0x918` | `fffffff005ea7548` / `fffffff005ea75e8` | N1 wake helpers |

**Verified:** normal commands are 16 zero-initialized bytes. The checksum
is a wrapping additive byte sum, stored LE16 in bytes 14–15. It is not CRC.
All selected SPI calls use provider virtual `+0x578` with
`(provider, tx, tx_length, rx, rx_length, 0, 0)`. These last zero arguments
alone do not establish GPIO CS lifetime or interrupt polarity.

**Verified:** successful HBPP detection in `fffffff005ea0bb4` sets the
driver's repeat-read flag (`this+0x3d8`) to one. Runtime initialization at
`fffffff005ea2854` therefore chooses command **EB** and sequence **1**;
the non-repeat variant uses **EA**. The sequence alternates **1, 2, 1, 2**
after successful frame processing. It is not a general incrementing counter.

One-frame sequence, checked against instructions:

1. `fffffff005ea1cf4`: TX16 `{EB, sequence, 0, zeros..., sumLE16}`.
   RX16 must have `(rx[0] & 0xf0) == 0xe0` and LE16 checksum at 14 equal
   to sum of bytes 0–13. Result length **L** is LE16 at RX1.
2. `fffffff005ea1ab8`: if L is zero, no data transfer is made. Otherwise
   request **L + 5** bytes. A configurable short microsecond delay may occur
   between the transfers; there is no runtime bootloader ATN_ACK call.
3. `fffffff005ea1f24`: TX is zeroed, then `{EB, sequence, 1}`. Sum the first
   14 TX bytes and store the LE16 sum at **the final two transfer bytes**,
   rather than always at 14–15. TX/RX length is L+5 (bounded by Apple's
   allocated receive buffer).
4. Data RX0 is EA, or EB when repeat-read is enabled. The sum of its
   first five bytes must be zero modulo 256. **P** is LE16 RX2; it includes
   a two-byte checksum. Payload begins RX5 and has P−2 bytes; its byte sum
   equals the LE16 tail checksum. Apple uses the transfer's final two bytes;
   Linux should additionally require P+5 equal to the expected wire size,
   or explicitly validate any permitted padding before dispatch.

**Verified:** host IRQ handler `fffffff005e953a8` checks power/firmware and
calls the complete-frame wrapper through `+0x5f8` with retry count five.
Its path does not call bootloader `0xa11a` ATN_ACK. Payload dispatch
`fffffff005e981ec` processes special IDs 0x50/0x53 then forwards the raw
payload to clients. `fffffff005b324c4` forwards frames to userspace rather
than decoding coordinates in the kernel.

**Inferred Linux policy:** serialize command/IRQ SPI with one mutex, use a
threaded oneshot IRQ, cap each drain at eight frames, and stop/disable IRQ
after a bounded repeated-error threshold. A stuck ATN must not cause an
infinite drain or retry. Release active slots when stopping after an error.
The GPIO65 ADT specifier is `[65, 0]`. **Verified:** GPIO controller class
constructor `fffffff00606e418` selects vtable `fffffff006e73298`;
its flags-to-register-mode helper at `fffffff006070294` maps flags0 to
register bits `[3:1]=5` (value `0x0a`), flags1/2/3/4 to mode3/4/2/6.
**Verified by matching the target Linux pinctrl-apple enum:**
HI=2, LO=3, UP=4, DN=5, ANY=6. Therefore mode5 is **falling edge** and
the Linux interrupt specifier is `IRQ_TYPE_EDGE_FALLING`, not level-low.
The ATN GPIO's active-low asserted meaning is **inferred** from the falling
notification and still needs an observed level/read-deassert handshake.
No runtime wire trace was available to this agent.

**Verified limitation:** E2's advertised Max Packet Size is **2454** on
this unit, but the selected Z2 result-length routine does not compare L
against that property. Only the legacy routine does. The precise advertised
limit's inclusion of transport header/checksum remains unresolved. Use a
strict allocated-buffer bound and structurally validate both length fields;
do not silently truncate to the advertised value.

## Runtime descriptor/report discovery

**Verified E2:** TX16 `{E2, zeros..., LE16(sum14)}` twice; validate the
second response's RX0=E2 and checksum. Interface version is RX2, maximum
packet size LE16 RX3. iOS permits four bounded attempts, sleeping 3 ms
between failed attempts (`fffffff005ea2880`).

**Verified E3:** TX16 `{E3, report_id, zeros..., LE16(E3+id)}` twice,
with `this+0xa60` microseconds between transactions. Validate RX0=E3 and
checksum; report status RX2, size LE16 RX3 (`fffffff005ea2b70`).
**Verified default:** constructor/property loader `fffffff005ea0940`
sets this delay to25µs; `fffffff005ea099c` defaults the optional frame
interstage-delay flag at `this+0xa64` tofalse. The selected personality
does not override either.

**Verified E6 short GET** for size below 12 (`fffffff005ea3110`): first TX16
`{E6,id,0,sizeLE16,zeros...,sumLE16}`. After the configured delay set TX2=1
and send the same 16 bytes **without recalculating the original checksum**.
Second response echoes E6; validate sum14 and copy `size` bytes from RX3.

**Verified E7 long GET** for size 12–512 (`fffffff005ea3374`): same first
request with E7; second request sets TX2=1, clears old checksum at 14,
moves the original checksum to offsets size+3/size+4, and transfers size+5
bytes. Validate echo E7 and LE16 checksum over RX0..size+2, data at RX3.

**Verified E4 short SET** below size12 (`fffffff005ea3a74`):
TX16 `{E4,id,size,data...,sum14LE16}` then E1 status16. E1 response requires
echo/checksum and RX8=0. iOS logs mismatched report/opcode history but its
status-success test is RX8 (`fffffff005ea4070`). Linux may enforce the
matching current report/opcode when first bring-up confirms them.

**Verified:** after boot, `fffffff005e976e0` caches E2, then GETs D1 family,
D3 sensor identity, D0 region descriptor, A1 region params, D9 surface.
D7 buttons is a separate optional getter.

| Report | Verified content | Unit snapshot |
|---|---|---|
| D1 | family byte | 21 |
| D3 | endian byte, rows byte, columns byte, BCD version BE16; at least 5 bytes | 1,40,30,0x0210 |
| D0 | region descriptor; queried size | `0101002801001e00` (8 bytes) |
| A1 | region params; queried size | `000005000002` (6 bytes) |
| D9 | two LE32 dimensions + four signed LE16 edges | `99390000c94c000090ff90ffac38e24b` (16 bytes) |

These snapshot values are from `build/firmware/private/IOService.txt`,
lines 4755 onward. **Verified:** Endianness=1 selects LE contact decoding;
other values make the framework swap multibyte fields.

**Verified:** report AF is **face detection mode**, not a generic report
enable. Framework `_MTDeviceSetFaceDetectionModeEnabled` at `1f48f2238`
sets report AF with one boolean byte. There is no justification to copy
J172/iPhone7 report initialization into N1. The selected personality/mtprops
has no DefaultReports. **Inferred:** start with wake, E2/descriptors and
IRQ/frame handling, without an AF write; verify any further report setup
from runtime evidence.

## Exact classic contact format

**Verified:** framework `_mt_ProcessMultitouchFrame` at `1f48df8c8`
routes IDs **43, 44, 45** through the same classic binary parser
`_MTParse_BinaryPathOrImage` at `1f48ee900`. A Gen2 SPI payload carrying
one of these reports is passed directly to this parser. The A10 0x90/HID
envelope is not needed for these reports. Which ID the N1 firmware emits
on hardware is still unobserved; accepting all three is justified by the
matching Apple parser.

Header parsing at `1f48ee708`:

| Offset | Meaning |
|---|---|
| 0 | report ID |
| 1 | frame/sequence byte |
| 2 | header byte length H |
| 3 | header version |
| 4 | LE32 timestamp |
| 8–11 | image/path content/options bytes |
| 12 | LE16 image metadata field |
| 14 | LE16 image payload byte count I |
| 16 | number of contacts N |
| 17 | contact stride S |
| 18 | signed/LE16 metadata |
| 20+ | version-dependent fields |

**Verified:** contact count/stride are parsed only if H≥22; older shorter
headers produce no contacts. Contact records begin at **H+I**, not a
hardcoded byte24. Require H≤payload length, I≤length−H, and
N≤(length−H−I)/S before accessing records. The host's binary contact array
has 32 entries. **Inferred Linux guard:** N≤32, S≥20, and a modest S upper
bound (64) reject malformed frames while supporting known classic records.
The hardware's maximum simultaneous touch count is not proven to be32.

Record fields, verified by `1f48ee900` and
`_mt_FillMTContactDirectFromBinary` at `1f48f3b1c`:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | contact/path ID |
| 1 | u8 | path state |
| 2 | u8 | identity/classification field |
| 3 | s8 | hand/identity field |
| 4,6 | s16 LE | X,Y positions in hundredths of mm |
| 8,10 | s16 LE | velocity components; conversion is separate |
| 12,14 | u16 LE | ellipse major/minor radius in hundredths of mm |
| 16 | s16 LE | ellipse angle, scaled by π/32768 |
| 18 | u16 LE | total proximity ZTot, /256 in MTContact |
| 20 | u16 LE when S≥22 | proximity density ZDen, /256 |
| 22 | u16 LE when S≥24 | extra proximity field |
| 24 | s16 LE when S≥26 | extra angle |
| 26 | u16 LE when S≥28 | force/pressure in grams |
| 28 | u16 LE when S≥30 | contact flags |

If the proximity density field is absent, Apple's parser computes it from
the total proximity and ellipse radii. It fills absent tail fields with
defaults. A Linux touchscreen does not need those derived fields.
**Inferred Linux mapping:** report ellipse radii as touch dimensions, and
use force only when S≥28; publishing ZTot as ABS_MT_PRESSURE is a
capacitive proxy and must be described as such rather than verified force.

**Verified states:** symbol `_mt_PathStageNames` at `20f4a1d50` contains
the following ordered strings; raw state is copied into MTContact by
`1f48f3b1c`:

```text
0 NotTracking     1 StartInRange    2 HoverInRange  3 MakeTouch
4 Touching        5 BreakTouch     6 LingerInRange 7 OutOfRange
```

Only **3/4** should assert a Linux touching slot. Other states release it.
Apple normally uses IDs1–31 with a fallback internal entry0. Linux should
use tracked IDs as keys, reject duplicate IDs in one report, and release
missing contacts with type-B DROP_UNUSED synchronization.

**Verified:** parser-type/options are cached at device+0x818/+0x81c.
The classic byte/header/contact decoders do not inspect those values;
endianness and report/header fields determine this raw layout. Options16
is used by higher-level gesture/HID handling. Its full semantic meaning
was not reconstructed and is not needed for byte parsing.

## D9 coordinate bounds

**Verified:** `_MTParseSensorSurfaceDescriptor` at `1f48dea30` reads
exactly16 bytes. Width/height are unsigned32 and the four tail values are
signed16. The unit has physical width14745, height19657, edges
`minX=-112,minY=-112,maxX=14508,maxY=19426`.

`_mt_InitImagerGridAndTimeState` at `1f48e80b0` puts the four edges into
the algorithm's `{maxX,minX,maxY,minY}` screen bounds. The raw-contact
converter at `1f48f3ce4` computes:

```text
normalizedX=(clampedRawX-minX)/(maxX-minX)
normalizedY=(clampedRawY-minY)/(maxY-minY)
```

`_alg_ClipPosPointToScreenEdge` at `1f48e89d0` supplies the clamp unless
an explicit frame flag disables it. Thus raw Linux limits `-112..14508`
and `-112..19426`, or origin-normalized `0..14620` / `0..19538`, follow
Apple's bounds more faithfully than physical dimensions alone. **Verified:**
this conversion itself has no Y inversion. **Verified in Round3:** the
owner-confirmed portrait TL/TR/BL capture requires only Y inversion, with
X unchanged. The J71t DT now supplies touchscreen-inverted-y, applied by
the generic Linux helper after edge normalization. Later iOS display
transforms were not reconstructed; see [round3.md](round3.md).

## Reset/wake/power sequence relevant to runtime

**Verified:** common power function `fffffff005e985dc` enables analog
power first, then LDO; shutdown reverses them. Optional mamba and
power-ana-enable-delay-us paths exist but the selected J71t properties
do not supply them.

After a successful off→on change, N1/Z2 `fffffff005ea100c` performs:

1. `enable_cs(1)` via `fffffff005ea1658`; helper sleeps5ms before calling
   the platform GPIO function with argument1 pointing to integer1.
2. `clock_enable(1)` via `fffffff005ea1530`; dynamic argument3 is integer3.
3. Sleep15ms (`+0x790` → `fffffff005ea18d8`).
4. N1 REQ_WAKEUP `fffffff005ea7548`: two bytes `19 c1`, then sleep10ms.
5. `display_sync(1)` via `fffffff005ea140c`; dynamic argument3 is integer3.
6. `reset(false)` via `fffffff005ea0ecc`:
   - resolve function-reset;
   - send S_CLK-high packet unconditionally before the old reset-state test;
   - call reset platform function with integer0;
   - call enable_download(false) and sleep selected reset-deassert-delay15ms.

**Verified S_CLK-high / HBPP-check packet** (`fffffff005ea10d8` /
`fffffff005ea21a8`):

```text
1a a1 18 e1 18 e1 18 e1 18 e1 18 e1 18 e1 18 e1
```

The initial two bytes come from constant `fffffff00547ec60`; seven words
are then overwritten with in-memory LE16 `0xe118`. The reset helper sends
this packet whenever reset(false) resolves the reset function; only the
extra no-op transition callbacks depend on the prior reset state.

**Verified:** successful boot-signature check accepts each of the first
two received BE16 words if it belongs to
`{18e1,1aa1,1f01,4879,4969,4ad1,4bc1}`. Check transfer errors explicitly;
Apple's function ignores one SPI result.

**Verified distinction:** vtable+0x8c0 (`fffffff005ea1780`) controls
**function-enable_download**, not CS. J71t's ADT has no such function, so
its optional effect is not evidence for holding CS across SPI messages.
CS is +0x8b8; clock is +0x8b0; display-sync is +0x8a8. Dynamic GPIO
argument3 is configuration/mux data, not permission to replace it with a
plain output-high write. **Verified by the parallel transport workstream:**
AppleARMGPIOFunction `fffffff00606dc40` maps argument3=3 to function3;
provider `fffffff00606fba4` writes mask0x270/value0x240, i.e. input-enable
bit9 plus peripheral mux2. J71t display-sync is therefore GPIO74 mux2.
Its enable_cs argument1=1 drives physicalGPIO102 high; the SPI controller
then owns transaction assertion. Do not hold the pin low across all boot
and runtime messages.

The later runtime wake `fffffff005ea75e8` is TX16 with EE at byte0,
LE16 EE at14 and zeros elsewhere; iOS suppresses repeats within100ms and
sleeps2ms after sending it. This is distinct from the two-byte boot wake.

## Remaining hardware-only or transport gaps

- Verify GPIO65 asserted level and behavior after each frame read;
  IRQ_TYPE_EDGE_FALLING is established by the controller-mode mapping.
- Verify selected native SPI mode/clock/byte order against provider reg
  interpretation. Live child reg bytes have positions8..11=`00 00 01 08`;
  touch hardware init `fffffff005ea0d98` sets period83ns and interword delay0.
  Round2 correction: provider config DWORD+0xc is interword delay, whereas
  DWORD+0x10 retains the ADT's10µs CS delay; see `round2-runtime-re.md`.
- Verify physical CS/display-sync GPIO function/mux behavior, and first
  runtime report ID/header/stride from a bounded capture.
- Verify four-corner orientation, release, repeated-ID stability and
  error recovery. No device readiness or touch operation is claimed here.
