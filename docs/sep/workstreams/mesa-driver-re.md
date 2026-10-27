# Mesa / Touch ID AP-side hardware path: reconstruction for a Linux driver

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Workstream research note, 2026-10-26. Scope: everything between the A9
application processor and the fingerprint sensor on iPad 5 (j71t, S8003,
iOS 16.7.10), plus the exact SEP relay interleaving it must feed. This
document is the design input for a patch-ready Linux driver; no driver code
is changed here. Companion documents: `docs/sep/driver-notes.md` (SEP
mailbox/GenericTransfer framing), `docs/sep/sbio-commands.md` (SBIO command
table), `docs/sep/touchid-next.md` (bring-up sequencing),
`docs/sep/userspace.md` (fprintd ABI).

Confidence levels used below:

- **[V]** verified: live trace (`build/sep/sbio-trace.log`, 12,965 lines) *and*
  decompilation or the live ADT agree, or two independent sources agree;
- **[S]** strong inference: single-source decompilation with consistent call
  chains, not yet device-confirmed;
- **[U]** unknown, with anchors for the next pass.

## Parent audit constraints for implementation

The recovered SPI framing and power ordering are sufficient for an inert,
opt-in status-transport driver, but not yet for capture or matching. The first
implementation must preserve these boundaries:

- The mandatory sensor patch's provenance is unresolved. Static code shows an
  iOS client-driven path and a direct `AppleMesaResources` path; it does not
  establish that sepOS is the patch source. No Linux patch loader is justified
  yet.
- Status byte `+8 == 0x29` is an observed value and a strong boot-state
  candidate, not proof of bootROM or patch readiness. Linux must report it raw.
- ADT GPIO mode `3` has no proven Linux IRQ-type translation. Trigger selection
  belongs in the future board DT and must not be guessed or changed at runtime
  by the client driver.
- SPI mode 0 is an expectation from related clients, not a live Mesa result.
  There is no automatic mode-3 fallback in the safe first slice.
- LDO7/LDO15 register mapping is static-analysis evidence pending hardware
  readback. The SPI client consumes named regulators; it does not program PMIC
  registers directly.
- The example DT in §11.1 is illustrative only. Its I2C controller label and
  IRQ trigger are explicitly unresolved and it must not be shipped unchanged.
- A valid status packet proves only AP↔sensor transport and parsing. It does not
  establish that Touch ID, capture, the sensor patch, or SEP relay is ready.

All kernelcache addresses are static, image base `0xfffffff005298000`
(`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`, Ghidra project
`kernelcache.release.iPad6,11_12`). The Mesa functions were renamed in the
shared Ghidra analysis during this pass (section 9).

---

## 1. Three-layer architecture (who drives what)

iOS splits Touch ID across three pieces **[V]** (live registry class counts +
trace call chains):

| layer | iOS class | role | instance on j71t |
|---|---|---|---|
| 3. SBIO client / orchestrator | `AppleMesaSEPDriver` (in `IOBiometricFamily.kext`) | matches `sep-endpoint,sbio`; sends SBIO opcodes to sepOS; issues start/pause-capture to layer 2 | 1 |
| 2. AP sensor driver | `AppleMesa` + provider `AppleSPIBiometricSensor` (in `AppleBiometricSensor.kext`) | owns `mesa@0` on SPI2: power, IRQ, poll/capture state machine, opaque frame read | 1 / 1 |
| (AOP variant) | `AppleSPUMesaDriver` | Mesa behind the AOP on other products; looked up via `function-mesa-reset-ntfy` | **0** — absent on j71t |

`AppleMesa::start` looks for the AOP driver only if the provider publishes
`function-mesa-reset-ntfy` ("AppleBiometricSensor: Looking for AOP
driver..."); j71t's ADT has no such function, and there is no mesa node under
`/arm-io/aop` in `docs/adt/j71t.txt` **[V]**. The SEP side (sepOS firmware)
does the matching; the AP side only transports opaque frames. The Linux
equivalent of layers 3+2 is one kernel driver plus the existing `sep-a9`
SBIO transport.

Glue classes on iOS: `AppleMesaShim` (1 instance) connects
`AppleMesaSEPDriver` → `AppleMesa` via `AppleMesaUserClientBase`; method names
recovered from the trace: `StartCaptureGated`, `PauseCaptureGated`,
`LoadPatchGated`, `PatchGated`, `ClearPatchOverrideGated` **[V]** (names only).

---

## 2. ADT facts (j71t, `docs/adt/j71t.txt` + live `build/firmware/private/IODeviceTree.txt`)

### 2.1 SPI2 controller node `spi2@A088000` **[V]**

| property | value | decode |
|---|---|---|
| `reg` | `0x0a088000 / 0x4000` | MMIO `0x20a088000`, 16 KiB |
| `compatible` | `"spi-1,samsung"` | Samsung SPI v1 block (`spi-version: 1`) |
| `interrupts` | `[190]`, parent 22 (AIC) | AIC IRQ 190 (Linux `AIC_IRQ 190`) |
| `clock-gates` | `[76]` | PMGR gate 76 = `ps_spi2` in the Linux pmgr dtsi |
| `clock-ids` | `[303]` (0x12f) | nclk = `clock-frequencies[47]` = **61,538,461 Hz** [V, live dump] |
| `dma-channels` | `[24, 25]` | SIO DMA endpoints **0x18 = SPI2 TX, 0x19 = SPI2 RX** (S matches the T8010 binding's documented "0x1a SPI3 TX, 0x1b SPI3 RX" and spi3's `[26,27]` here) |
| `dma-parent` | phandle 50 | `sio-dma` under `iop-sio-nub` (SmartIO DMA controller) |
| `function-spi_cs0` | `1:llun` ("null") | **no GPIO chip select** — CS is controller-internal (PIN register bit 1) |

The clock value is decoded from the live `arm-io` `clock-frequencies` array
(index = clock-id − 0x100): indices 46/47/48 are all 61,538,461 Hz, matching
the `spi1_nclk`/`spi3_nclk` fixed clocks already used by the audio and touch
DT work **[V]**.

### 2.2 Sensor node `mesa@0` **[V]**

```
mesa:
  compatible: "biosensor,mesa"
  device_type: "mesa"
  AAPL,phandle: 68
  reg: (32-byte custom blob; unit address 0 → CS 0; see §2.3)
  spi-frequency: 0x7a1200          # 8,000,000 Hz
  interrupts: [44 3]               # parent 30 = gpio,s8000 @ 0x20f100000
  function-mesa_pwr:    79:pmuL(0x307)
  function-mesa_pwr1v8:79:pmuL(0x30f)
  function-hid_event_dispatch: 131:ButD()
  power-off-delay: 0x3e8           # 1000 ms
  time-between-scans: 0x20000     # 131072 ms
  scan-timer-reset-time: 0x9999   # 39321 ms
  max-scan-time: 0x13333          # 78643 ms
```

- Provider phandle **79** = the D2365 PMU (`compatible: "pmu,d2365"`, I2C
  `0x9c40000003c`, i.e. bus `i2c1` address `0x3c` — the same physical device
  the Linux DT calls `pmic@3c` / `apple,angel-pmic`) **[V]**.
- Provider phandle **131** = the `buttons` node (`AppleButtonsDispatchFunction`,
  4CC `ButD`) — the sensor's finger events are dispatched through the button
  event path (`mesa-dispatch-hid`); not needed for the relay-only Linux driver.
- Provider phandle **30** = the s8000 GPIO block: `gpio,s8000`/
  `gpio,s5l8960x`, `#gpio-pins: 208`, reg `0xf100000/0x100000` = `0x20f100000`,
  `#interrupt-cells: 2`, 7 group IRQs 42–48 to the AIC — Linux `pinctrl_ap`
  **[V]**.
- `MesaSelfTest = "OK"` in the live dump is an **IOKit-published property**
  from `AppleMesa::start` (values seen in code: `OK`, `SPIError`,
  `SensorTypeMismatch`, `InterruptError`) — it is not an ADT property.
- A10 comparison: j172's mesa node is byte-identical except phandles/GPIO
  (`interrupts: [81 3]`, pmuL providers differ) — the sensor protocol is
  shared across A9/A10 iPads **[V]**, so Pauli1Go's A10 work is a second
  potential source.

### 2.3 The custom 32-byte child `reg` **[S/U]**

All Samsung-SPI children here carry a 32-byte reg blob beyond the unit
address. Comparing live decodes (`(addr64 LE, size64 LE)` pairs, parser =
`scripts/get-adt.sh`):

| child | tuple 1 addr hi | tuple 1 size | tuple 2 addr |
|---|---|---|---|
| audio-codec@0 (spi1) | 0x53 (83) | 0x08010000 | 0x14 (20) |
| **mesa@0 (spi2)** | **0x1f4 (500)** | **0x08010100** | 0x14 (20) |
| multi-touch@0 (spi3) | 0x7c (124) | 0x1388_08010000 | 0x2710 (10000) |

The only field with a confirmed meaning: **bit 8 of the size word is set only
for mesa** (0x08010100 vs 0x08010000), and mesa is the only child whose
`function-spi_cs0` is "null" — so bit 8 plausibly marks controller-internal
CS **[S]**. The remaining fields (0x53/0x1f4/0x7c and 0x14/0x2710) are
undecoded **[U]**. Nothing blocks the Linux port: both the audio and touch
Linux DTs use plain `reg = <0>` and work.

---

## 3. Power architecture **[V/S]**

### 3.1 The two supplies, decoded end-to-end

`function-mesa_pwr` / `function-mesa_pwr1v8` are D2365 PMU LDO controls:

- Function data `<phandle 0x4f><4CC "pmuL"><args 4 bytes>`; args for mesa are
  `07 03 00 00` and `0f 03 00 00` (little-endian u32 0x307 / 0x30f).
- `AppleD2365PMULDOFunction` ctor (`FUN_fffffff006bbe488`) parses the blob as
  `ldo# = byte 0`, `flags = byte 1` (bit 1 must be set or "illegal device tree
  options"), `u16 options` = bytes 2–3 (bit 15 = a "skip enable write" flag).
  So: **pmuL(0x307) = LDO 7, pmuL(0x30f) = LDO 15**, both enable-only
  (voltage field 0) **[S]**, flags 0x03 = valid + enable-at-init.
- `d2365_set_ldo` (`0xfffffff006bbcdd8`, already renamed by the touch
  workstream) walks a 12-byte-per-row LDO table at **`0xfffffff005915b12`**;
  row layout: `+0 u16 min-mV, +2 u8 step, +3 u8 steps, +4 u8 volt-mask,
  +5 u8 volt-reg-delta, +6 s16 volt-reg, +8 u16 enable-reg, +10 u8
  enable-mask` **[S]**. Read from the kernelcache:

| function | LDO | enable register | mask | role (kext log) |
|---|---|---|---|---|
| `mesa_pwr` | 7 | **0x0312** | bit 0 | "3V" (sensor core/analog) |
| `mesa_pwr1v8` | 15 | **0x0319** | bit 0 | "1V8" (I/O) |
| (touch analog, reference) | 31 | 0x031c | bit 0 | verified pair: the working `apple-d2365-touch.c` maps pmuL(0x21f) exactly this way |

Cross-check: the touch-analog LDO31 row decodes to enable reg 0x031c bit 0,
matching the Linux driver that runs on hardware today **[V]**. So LDO7/LDO15
are strong-inference (same table, same decoder), pending one device read-back.

Both are enable-only RMW writes through the PMU I2C regmap (`vtable+0x5d0`
write8 / `+0x5c8` read8 on the `AppleD2365PMU` object) — identical shape to
the existing `apple-d2365-touch.c` regulator **[S]**.

### 3.2 Power-on/off ordering (`AppleMesa_setSensorPower`, `0xfffffff005ef37e4`) **[S]**

```
power ON:  [over-current check if function-over-current exists]
           1V8 on (LDO15/0x319)          → log "%s: 1V8 on"
           IOSleep(power-on-1v8-3v0-delay)   (default 1 ms)
           3V0 on (LDO7/0x312)           → log "%s: 3V on"
           IOSleep(power-on-delay)            (default 100 ms)
           [deassert reset function if present]

power OFF: [assert reset function if present]
           3V0 off
           IOSleep(power-off-3v0-1v8-delay)  (default 1 ms)
           1V8 off
           IOSleep(power-off-delay)          (default 100 ms; j71t ADT 1000 ms)
```

- The optional "reset function" (`function-mesa-reset-ntfy`, field
  `this+0x278`) is **absent on j71t** — no reset GPIO; reset happens over SPI
  (commands 0x14 short / 0x18-class long reset, see §6) **[V, ADT + S, code]**.
- Delay fields `this+0x3b0/0x3b4/0x3b8/0x3bc`, overridable by boot-args
  `mesa-power-off-3v0-1v8-delay`, `mesa-power-off-delay`,
  `mesa-power-on-1v8-3v0-delay`, `mesa-power-on-delay`; DT defaults come from
  the provider properties of the same names (j71t only sets
  `power-off-delay` = 1000) **[S]**.
- Over-current/monitor functions (`function-monitor-current-3v0/1v8`,
  `function-over-current`) are looked up but **absent on j71t** (not in the
  ADT), so no power monitoring thread is started **[V ADT / S code]**.
- `mesa-drive-1v8` boot-arg defaults **on**: the 1V8 function is used
  (`this+0x270`) unless explicitly disabled **[S]**.

`AppleMesa_powerOnReset` (`0xfffffff005ef3394`) wraps this: with the 3V
function present it does the off→on cycle via the transport (or the fallback
above); when no 3V function exists (other products) it instead does the
"Long Reset" SPI frame (§6, cmd 0x04) with the optional reset-function
toggled around it **[S]**.

### 3.3 Regulator summary for the Linux DT

Two fixed, enable-only regulators on the existing `pmic@3c` (angel-pmic)
regmap: LDO7 @ `0x312` bit 0 ("mesa-3v0") and LDO15 @ `0x319` bit 0
("mesa-1v8"). Voltages are never programmed (the rails are pre-configured by
the PMU default state), so no voltage tables are needed **[S]**.

---

## 4. Interrupt and reset GPIOs

- **Sensor IRQ: AP GPIO pin 44, specifier mode 3**, on `gpio,s8000`
  (`pinctrl_ap`, `0x20f100000`) **[V, ADT]**. Every sensor state change raises
  this line; the driver then polls status (§5.1) — the trace shows
  `handleInterrupt: entry: state=N` immediately before each poll **[V]**.
- **The Linux IRQ type for iOS mode "3" is not yet proven** **[U]**. Known
  data points: iOS `[65 0]` (touch attn) → Linux `IRQ_TYPE_EDGE_FALLING`
  (works on hardware) **[V]**; iOS buttons use modes 7/5 (gpio-keys in Linux
  uses both edges for those) **[S]**. Candidates for `[44 3]`:
  `IRQ_TYPE_EDGE_BOTH` (if mode = bit0 rising | bit1 falling) or a level
  trigger. Linux must not guess this in the client driver; narrow it from the
  GPIO controller translation, then test one board-DT hypothesis per boot.
  Anchor for a definitive answer: `AppleGPIOICController::start`
  (`FUN_fffffff00606ca00`) and its interrupt-type method — not yet decoded.
- **No reset GPIO on j71t**: no `function-mesa-reset*` in the ADT; resets are
  SPI frames **[V]**. (Other boards can have `function-mesa-reset-ntfy`,
  which also gates the AOP handshake.)

---

## 5. SPI transport and protocol (all frames seen on the wire) **[V]**

SPI2 runs the Samsung SPI v1 controller. The touch workstream already wrote
`drivers/spi/spi-apple-s8000.c` for it (registers: CTRL 0x00, CFG 0x04 with
base 0x4018 + CPHA bit1 + CPOL bit2 + PIO bit5, STATUS 0x08 with TX level
bits 6–10 / RX level bits 11–15, PIN 0x0c with CS bit 1, TXDATA 0x10, RXDATA
0x20, CLKDIV 0x30, RXCNT 0x34, WORD_DELAY 0x38, TXCNT 0x4c; 16-byte FIFOs;
max divisor 2047; separate RXCNT/TXCNT). That driver is the natural base.

- **Mode/phase**: the mesa ADT sets only `spi-frequency`; there is no
  `spi-clock-phase` property (that string exists only as a kext override,
  together with boot-arg `mesa-spi-clock-phase`; `mesa-spi-freq` likewise).
  Default Apple framing = mode 0 (CPOL=0, CPHA=0, CFG 0x4018), which is what
  the working audio-codec and N1-touch clients use **[S]**. First bring-up
  should assume mode 0 and fall back to mode 3 if the status poll returns
  garbage **[U]**.
- **Frequency**: `spi-frequency` = 8,000,000 Hz; from the 61,538,461 Hz nclk
  the controller divisor is 8 → effective 7,692,307 Hz **[V/S]**. A 19,668-
  byte frame at that clock is ≈20.4 ms — fine for bounded PIO (the s8000-spi
  driver caps transfers at 64 KiB, which covers it).
- **Chip select**: controller-internal (`function-spi_cs0` "null"); the
  driver's `set_cs` PIN-register path already handles the no-`cs-gpios` case
  **[V/S]**.

### 5.1 Frame formats

All writes are 7-byte (or longer) frames `{0x80, opcode, 0x00, u32_le
total_len}`; responses repeat the shape with the response opcode:

```
poll (getSensorStatus):
  W: 7B   80 10 00 07 00 00 00
  R: 23B  00 00 00 00 00 00 00          <- 7 echo/dummy bytes (full-duplex)
         80 20 00 10 00 00 00          <- response header (len 0x10 = 16)
         <16-byte status payload>
```

The driver issue is exactly constant `0x07001080` (bytes `80 10 00 07`,
three zero bytes) built in `AppleMesa_getSensorStatus`; the read length is
`status_len + 7` with `status_len` clamped to 16 **[V trace / S code]**.

```
capture (readSensorData), after a poll reported state 7 + size:
  W: 11B  80 13 00 0b 00 00 00 <u32_le size>     (size = 0x4cd4 = 19668)
  ... ~21 ms sensor-internal delay ...
  W: 1B   00                                      (dummy byte to clock data)
  R: 19668B  ed fc 06 ..                          (opaque encrypted frame)
```

Built in `AppleMesa_readSensorData` (`0xfffffff005efb0f8`) as constant
`0xb001380` + u32 size; the size must be ≤ 65,536 (`0x10001` bound) **[V
trace / S code]**. The two writes are separate transactions (the ~21 ms gap
is the sensor fetching the frame); the second is a tx=1/rx=size transaction —
in Linux: one full-duplex transfer of `size` bytes with a zeroed TX buffer
(MOSI content during the data phase is presumed ignored; verify at stage
S4) **[S/U]**.

### 5.2 Status payload layout (16 bytes at read offset 7) **[V for observed values, S for field names]**

| offset | meaning | observed |
|---|---|---|
| +0..+3 | `80 20 00 10` response header (`AppleMesa_isSensorStatusValid` checks byte0==0x80, byte1==0x20 (mask 0xff, or 0x3f with `mesa-full-status`), byte3==0x10) | always |
| +4..+6 | zero | always |
| +7 | **sensor state** | 0x00 idle; 0x07 data available; 0x11/0x13/0x15 transitional (finger approach); 0x19 HV-boost; 0x09 ESD recovery |
| +8 | sensor status/error | 0x00 OK; **0x29 bootROM/parameter-ok**; nonzero = error (`badSensorStatus`) |
| +9 | flags (bit 1 = patch type; used for "patch-version") | 0x01 seen while transitioning |
| +10 | provision status | 0x00 |
| +11 | event flags | **0x8e data available**; 0x50 idle-confirmed; 0x40/0x10 reset-complete; bit 1 = finger-on |
| +12..+15 | u32 context | idle → **SensorID 0x3350**; state 7 → **19668** (frame size); state 0x15 → live counter (noise) |

`AppleMesa_handleStateMachine` reads state at `this+0x160`, error at
`this+0x161`, flags at `this+0x164`, size at `this+0x165` (u32), previous
driver state at `this+0x150`, last command at `this+0x154` **[S]**. When
state == 7 and the size passes the bounds (driver min-data 11924, frame
19668 — the two defaults loaded from `0xfffffff00549b600`), it calls
`readSensorData` **[V trace / S code]**.

### 5.3 Capture loop with the SEP relay, invariant prefix and variable tail **[V, trace]**

One enrollment capture cycle (12:10:08–12:10:17, repeated):

```
userspace → SBIO EnrollMode 0x03 (68B) ──────────── SEP ack
kCMD 6  StartCapture:  SPI 80 40 00 07 00 00 00   (+ capture watchdog 500 ms)
IRQ → poll → state 0x11 → ... → 0x13 → 0x15 (finger down) → 0x07 flags 0x8e len 19668
readSensorData → 11B request → 1B+19668B read (opaque frame, first bytes ed fc 06 ..)
frame → SBIO relay, with this trace-verified common prefix:
    0x23 (0 in / 0 out)                    purpose not yet named  [U]
    0x06 (151 or 167 in / 0 out)           image-processing init (per frame)
    0x65 chunk 0 (tag 0xfc, 16356/19668) → SEP replies tag 0xfe ("continue")
    0x65 chunk 1 (tag 0xfd, 16356+3312)  → SEP replies tag 0xfc (done)
    0x07 (0 in)  → 192 B out              async result fetch
    then a state-dependent tail chosen by the SEP/AP state machine
IRQ → poll → state 0x07 again (sensor streams while finger is down) → read frame → repeat
kCMD 10 PauseCapture (deferred while imaging): SPI 80 41 00 07 00 00 00
IRQ → poll → state 0x00 flags 0x01 → idle → next StartCapture (80 40)
```

Across the 20 captured frame cycles, `0x06` is 151 bytes 14 times and 167
bytes 6 times. The tail contains `0x08` 8 times, `0x09` 12 times, `0x1f` 9
times, and `0x22` 18 times. The selection rule is not recovered, so Linux
must branch on the `0x07` result and async state; it must not blindly append
`0x08` and `0x22` to every frame. Chunk size 16,356 = `0x4000 − 0x1c`
(16 KiB OOL minus the 28-byte
GenericTransfer header) **[V]**. The pause command is *deferred* until the
in-flight frame read completes (`sendDeferredCommands: Put the sensor in
Paused mode during imaging`) **[V]**.

---

## 6. Mesa command table (`AppleMesa_handleCMD`, `0xfffffff005efb6a8`) **[S]**

kCMD numbers are the Mesa-layer commands that `AppleMesaSEPDriver` issues
through `AppleMesaShim`. Verified rows are the ones seen in the trace.

| kCMD | name (kext log) | SPI frame | notes |
|---|---|---|---|
| 0x00 | GetSerialNum | `80 07 00 07` (const 0x7080) | reply 0x1b=27B → "SerialNumber" property (16B) |
| 0x01 | Idle | `80 03 00 07` (0x3080) | success: state 0 + flags&0x70==0x50 |
| 0x02 | Sleep | `80 31 00 07` | also "force idle" in pause corner |
| 0x03 | Short Reset | `80 14 00 07` | then status poll; lastCmd=0x18 |
| 0x04 | Long Reset | 0x12e-byte frame `ff … fe` (data at 0x33..0x2f stack) | no-3V-function fallback path |
| 0x05 | Echo | 35B (data `0xfffffff00549b877`) | |
| **0x06** | **StartCapture** | **`80 40 00 07 00 00 00`** or `80 42…` HV-boost variant (`this+0x37c==1`) | + capture watchdog 500 ms (`mesa-wff-watchdog-period`) |
| 0x07 | SetupPatchEnable | `80 06 00 07` (0x6080) | then patch upload (vtable+0x6f0, "sent patch to sensor"); done via patch-load interrupt (case 0x19) |
| 0x08 | HostIntDepth | parameter frame (`17 <depth> 18 …`, via vtable+0x6f8) | depth ≤ this+0x124 (19668) |
| 0x09 | SkipFingerDetect | parameter frame | |
| **0x0a** | **PauseCapture** | **`80 41 00 07 00 00 00`** | |
| 0x0b | SetImageBitDepth | parameter frame (5 = 16-bit, 4 = 8-bit) | |
| 0x0c | GoToProductionMode | data `0xfffffff00549b640` | |
| 0x0d | ProvisionSensor | `80 71 00 <len+7>` + root key | **write path — never send from Linux** |
| 0x0e | SessionKeyExchange | `80 72 00 2f` + 40B key | **write path — never send** |
| 0x0f | InitializeSequenceCounter | `80 73 00 <len+7>` + blob | **write path — never send** |
| 0x10–0x14 | Lock/Write/Unpair keys | `80 c0/86/80 … 4b` | **write paths — never send** |
| 0x15 | PowerCycle | — | calls `AppleMesa_powerOnReset` |
| 0x16 | SetEncryptedParameters | via vtable+0x6f8 | "buffer too large" check at 0xfffffff005497559 |
| 0x1a | GetModuleSerialNum | `80 75 00 07` (0x7580) | reply 0x1d (29B) for SensorID 0x3350 (0x1f for 0x3354); CRC16 checked, table `0xfffffff00549b650` |
| 0x1b–0x1c | Encrypted key writes | `80 87/88 00 4b` + 0x42 payload + CRC16 | **never send** |
| 0x1d | CalibrateAndUpdateNVM | `80 51 00 40` + 0x36 + CRC16 | **writes sensor NVM — never send** |
| 0x1e | StartFingerDetect | `80 48 00 07` | + finger-detect watchdog 500 ms |
| 0x1f | TestCMD | `80 78 00 <test#>` | used at start as the interrupt-line test |
| 0x20 | BurnIn | 10B (data `0xfffffff00549b8b9`) | |
| 0x8a | StopSSHBHB | 11B (data `0xfffffff00549b8c3`) | 21×50 ms status poll after |
| 0x8b | DHKeyExchange | 60 s timeout | **provisioning — never send** |
| 0x8c | Reprovision | — | **never send** |

Driver state model (IOStateReporter): OFF / IDLE / FING-DET / IMAGING, plus
ESD-recovery states (9, 200=disabled) **[S]**. State 0x19 = HV-boost active
("HVBOOST enabled") **[V trace]**.

---

## 7. `AppleMesa::start` sequence (`0xfffffff005ef0ec8`) **[S]**

1. publish `MesaSelfTest = "OK"` (placeholder), metaCast provider →
   `AppleSPIBiometricSensor`;
2. platform-function lookups: `function-mesa_pwr` (`this+0x268`),
   `mesa_pwr1v8` (`this+0x270`, if `mesa-drive-1v8`), `function-bb_gpio`
   (`this+0x2e8`, absent), `function-monitor-current-*`/`function-over-current`
   (absent on j71t), `function-mesa-reset-ntfy` (`this+0x278`, absent → skip
   AOP wait);
3. delay config (§3.2), boot-args `mesa-verbose/full-status/patch-directly/
   esd-*`; a 64 KiB `IOBufferMemoryDescriptor` for frame data (`this+0x298`);
4. IOReporter setup (`AppleMesa_setupReporters`, `0xfffffff005ef2c48`);
5. power on + prepare (transport vtable+0x690);
6. **sanity loop, 3 tries**: `getSensorStatus` + validator; on failure
   `powerOnReset`; exhaustion → driver state 200, power off,
   `MesaSelfTest="SPIError"`;
7. SensorID from status u16 (0x3350) → `SensorID` property; type check against
   `mesaType` (absent in j71t ADT → expected 0x3350); mismatch → power off,
   `MesaSelfTest="SensorTypeMismatch"`;
8. **interrupt test**: TestCMD 0x78 and wait for the sensor IRQ; timeout →
   `MesaSelfTest="InterruptError"`;
9. GetSerialNum → `SerialNumber` property;
10. final status dump ("start(): SensorStatus").

Boot-time SPI traffic that was *not* in the enrollment capture (it happens at
boot): HostIntDepth/SetImageBitDepth parameter frames and the **sensor patch
upload** — see §8.

---

## 8. The sensor-patch dependency (the critical unknown)

Static analysis indicates that the Mesa sensor normally receives a **patch
after every power-up before capture** **[S]**; the current runtime trace starts
after initialization, so an unpatched capture failure is not yet observed:

- `AppleMesa_loadPatch` (`0xfffffff005f014e8`): if the client
  (`this+0x130`, the AppleMesaSEPDriver-side shim) is present, the client
  drives the patch ("calling runAction(LoadPatchGated)"); if the client is
  missing or `mesa-patch-directly` is set: "client missing - load patch
  directly" via `AppleMesaResources` (resource loader class, 1 instance) **[S]**.
- The patch bytes are written over SPI by the kCMD 7 path ("Setup Patch
  Enable" → `80 06 00 07` + upload via vtable+0x6f0, log "sent patch to
  sensor"); completion arrives as an interrupt with lastCmd 0x19 ("patch load
  done interrupt", "loaded patch vers=%d") **[S]**.
- There is an explicit consistency check "sensor with patch loaded but in
  bootROM" — status byte +8 == 0x29 appears to be the **bootROM state**
  (parameter commands accept 0x29 or 0x00 as OK) **[S]**.

Implication: the patch is a strong bring-up prerequisite, but the current
trace starts after initialization and does not prove that an unpatched sensor
cannot return a frame. Static analysis exposes both a client-driven path and a
direct `AppleMesaResources` path; it does not yet prove that the blob comes
from the SEP. If SBIO carries it, the responsible opcode is not yet identified —
candidates from the uncategorized trace commands: `0x54` (20 in), `0x27`
(60 in) **[U]**. The patch SPI framing itself is also not decoded yet — the
sender lives at `0xfffffff005efade4` ("sent patch to sensor"), which Ghidra
did not recognize as a function **[U]**.

Two non-exclusive ways to close this: (a) identify the SBIO patch opcode and
let sepOS serve the patch at Linux runtime (clean, matches iOS); (b) capture
the patch bytes once from the running iOS device (boot-time SPI trace — a
device action that may only be proposed in a guarded test plan). Both are
open work items, listed in §12.

---

## 9. Kernelcache function map (renamed this pass in the shared Ghidra project)

| address | name (now) | role |
|---|---|---|
| 0xfffffff005eefe70 | `AppleBiometricSensor_sendCommand` | builds IOMDs, calls SPI transport (vtable+0x558) or AOP transport (vtable+0x540), logs `Wrote: %5zuB` / ` Read: %5zuB` when `mesa-verbose` |
| 0xfffffff005ef02fc | (thin wrapper) | send(tx, len, rx, len, verbose) |
| 0xfffffff005ef3024 | `AppleMesa_getSensorStatus` | 7B poll + 7+16B read, dumps Status={}, forwards state msg 0x1b |
| 0xfffffff005efafc8 | `AppleMesa_getSensorStatusWithRetry` | poll + validate, retry N, else 0xe00002ca |
| 0xfffffff005ef0c00 | `AppleMesa_isSensorStatusValid` | `80 / 20 / 10` header check |
| 0xfffffff005efb0f8 | `AppleMesa_readSensorData` | 11B capture request (`0xb001380` + u32 len), 1B+`len` read |
| 0xfffffff005efdc94 | `AppleMesa_handleStateMachine` | poll dispatch, data-available path, kCMD completion switch, driver-state transitions |
| 0xfffffff005efb6a8 | `AppleMesa_handleCMD` | full kCMD → SPI frame table (§6) |
| 0xfffffff005ef3394 | `AppleMesa_powerOnReset` | power cycle / long reset |
| 0xfffffff005ef37e4 | `AppleMesa_setSensorPower` | 1V8/3V0 ordering + delays, over-current path |
| 0xfffffff005ef0ec8 | `AppleMesa_start` | bring-up + MesaSelfTest (§7) |
| 0xfffffff005ef2c48 | `AppleMesa_setupReporters` | IOReporter channels |
| 0xfffffff005f014e8 | `AppleMesa_loadPatch` | client/direct patch decision (§8) |
| 0xfffffff006bbcdd8 | `d2365_set_ldo` | D2365 LDO table walker (pre-existing rename) |
| 0xfffffff006bbe488 | (LDOFunction ctor) | pmuL blob parse: ldo# = byte0, flags = byte1 |
| 0xfffffff00606ca00 | `AppleGPIOICController::start` | anchor for the GPIO IRQ-type decode **[U]** |

Data anchors: LDO table `0xfffffff005915b12` (12B rows); CRC16 table
`0xfffffff00549b650`; frame-size defaults `0xfffffff00549b600` (11924,
19668); echo/production/burn-in/stopsshhhb blobs `0xfffffff00549b877`,
`0xfffffff00549b640`, `0xfffffff00549b8b9`, `0xfffffff00549b8c3`.

Decompiler caveat: in `AppleMesa_start` Ghidra types `this` as `OSObject*`,
so displayed `param_1[N]` indices there are element-scaled; the offsets
quoted in this document were taken from consistently-typed functions
(`handleCMD`, `handleStateMachine`, `setSensorPower`) or explicit
`(long)param_1 + off` casts.

---

## 10. Linux inventory: what exists, what must be added

### Already in the Hoolock tree / workspace patches **[V]**

- `pinctrl_ap` (`apple,s8000-pinctrl`) with the S8000 mux-semantics patch
  (`patches/touch/0003`); AIC; `ps_spi2` power domain in `s800-0-3-pmgr.dtsi`.
- Samsung SPI v1 controller driver `drivers/spi/spi-apple-s8000.c`
  (`patches/touch/0002`): bounded byte-wide PIO, mode bits CPHA/CPOL,
  internal-CS path, 64 KiB transfer cap — sufficient for 19,668-byte frames.
- D2365 PMU (`pmic@3c`, `apple,angel-pmic`) with a regmap and the
  `apple-d2365-touch.c` regulator pattern (`patches/touch/0002`).
- SIO core + DMA controller (`patches/audio/0006–0013`), with S8000 endpoint
  tagging; binding documents SPI TX/RX endpoints 0x1a/0x1b (spi3) — spi2's
  are 0x18/0x19 by the same numbering.
- `drivers/soc/apple/sep-a9.c` (workstream `sep`, `/src/wt/sep`): mailbox,
  endpoint discovery, OOL publication, single-chunk GenericTransfer
  (0xfc) framing, `/dev/sep-bio` UAPI v4; multi-chunk 0xfd/0xfe framing is
  the next planned step there (see `docs/sep/driver-notes.md`).
- The DT plumbing style (`s8003-j71t.dts` includes) and the `sep` DT node.

### Missing (the mesa work items) **[V gap analysis]**

1. **DT**: `spi2@20a088000` node + `mesa@0` child + two D2365 regulators +
   GPIO IRQ wiring (none of it exists in any tree — the sep tree has no SPI
   nodes at all; the touch/audio DTs cover spi3/spi1 respectively).
2. **D2365 LDO7/LDO15 regulator support** (extend the existing
   apple-d2365 regulator or generalize it).
3. **The Mesa SPI sensor driver** (power sequencing, IRQ → poll state
   machine, capture read, watchdog, deferred pause) — the core of §5–§7.
4. **The SEP relay glue**: per-frame SBIO common prefix `0x23 → 0x06 →
   0x65 (chunked) → 0x07`, followed by the state-dependent `0x08`/`0x09`/
   `0x1f`/`0x22` tail, plus start/stop orchestration, on top of sep-a9.
   Requires the 0xfd/0xfe multi-chunk framing in sep-a9 (already specified).
5. **The sensor patch path** (§8) — source identification and the patch-upload
   SPI framing; a strong static prerequisite hypothesis that the first bounded
   status/capture experiment must confirm or falsify.
6. Optional later: SIO DMA offload for the 19,668-byte reads (endpoints
   0x18/0x19); HV-boost variant 0x42; ESD auto-recovery.

---

## 11. Patch-ready driver architecture (design, not implemented)

### 11.1 Device tree (j71t, follows the touch/audio conventions)

```dts
/* s8003-j71t-mesa.dtsi */
/ {
	spi2_nclk: clock-spi2 {
		compatible = "fixed-clock";
		#clock-cells = <0>;
		/* live iOS arm-io clock-frequencies[0x12f - 0x100] */
		clock-frequency = <61538461>;
	};
};

&{/soc} {
	spi2: spi@20a088000 {
		compatible = "apple,s8000-spi";          /* spi-apple-s8000 */
		reg = <0x2 0x0a088000 0x0 0x4000>;
		#address-cells = <1>;
		#size-cells = <0>;
		interrupt-parent = <&aic>;
		interrupts = <AIC_IRQ 190 IRQ_TYPE_LEVEL_HIGH>;
		clocks = <&spi2_nclk>;
		power-domains = <&ps_spi2>;
		status = "okay";
		/* ADT function-spi_cs0 = "null": controller-internal CS;
		 * deliberately no cs-gpios. */

		mesa: biosensor@0 {
			compatible = "apple,mesa";
			reg = <0>;
			spi-max-frequency = <8000000>;      /* ADT spi-frequency */
			/* ADT interrupt is GPIO 44, iOS mode 3. Linux trigger
			 * translation is unresolved, so omit it in the first DT. */
			vdd-3v0-supply = <&mesa_3v0>;
			vdd-1v8-supply = <&mesa_1v8>;
			apple,power-off-delay-ms = <1000>;  /* ADT power-off-delay */
			/* DMA offload later: dmas = <&sio 0x18 0x19>; */
		};
	};
};

&{/soc/i2c@20a110000/pmic@3c} {
		/* D2365 LDO table: LDO7@0x312, LDO15@0x319, enable-only bit0 */
		mesa_3v0: regulator@312 {
			compatible = "apple,d2365-mesa-ldo";
			reg = <0x312>;
			regulator-name = "mesa-3v0";
		};
		mesa_1v8: regulator@319 {
			compatible = "apple,d2365-mesa-ldo";
			reg = <0x319>;
			regulator-name = "mesa-1v8";
		};
};
```

Only j71t/j72t should enable this personality (board-local include, like
the touch work did).

### 11.2 Driver shape

`drivers/spi/spi-apple-mesa.c` (or `drivers/biometric/`), one file, following
the repo's bounded-bring-up conventions (inert until enabled):

- **probe (gated)**: `mesa.enable=1` module param (same pattern as
  `sep_a9.enable=1`); regulator get/ordering, IRQ request, workqueue. No
  MMIO outside the SPI core; the sensor is not touched until debugfs arm.
- **power**: `regulator_enable(mesa_1v8) → msleep(1) → regulator_enable(mesa_3v0)
  → msleep(power_on_delay)`; reverse for off with the ADT 1000 ms
  `power-off-delay` last. `pm_runtime` domains for spi2/ps_spi2 come via the
  SPI core `power-domains`.
- **transport primitives** (all full-duplex, zero-padded TX):
  - `mesa_get_status()`: one 23-byte transfer, tx = poll frame + zeros,
    parse rx[7..22] (validate `80 20 ?? 10`);
  - `mesa_read_frame(len)`: message 1 = write 11-byte request; message 2 =
    full-duplex `len` bytes with zeroed tx; reject `len > 65536` or
    `len != advertised`;
  - command senders: `idle(0x03)`, `start_capture(0x40)`,
    `start_capture_hv(0x42)`, `pause(0x41)`, `short_reset(0x14)`,
    `sleep(0x31)`, `finger_detect(0x48)`, `test(0x78)`.
  - **Never implemented/never callable**: provisioning, session-key,
    lock/write/unpair key, calibrate-NVM commands (§6) — do not even add
    their constants.
- **state machine** (workqueue + threaded IRQ, mirrors §5.3):

```
OFF ──power_on──► SANITY(3× poll) ──ok──► IDLE
IDLE ──start_capture──► CAPTURING ──IRQ──► POLL ─┬─ state 7 ─► READ_FRAME
                                                ├─ state 9 ─► ESD_RECOVERY (power cycle)
                                                └─ other ──► CAPTURING (keep waiting, watchdog)
READ_FRAME ──ok──► RELAY_TO_SEP ──done──► POLL (sensor streams more frames)
           └─err──► POWER_OFF
CAPTURING ──pause(deferred until frame done)──► PAUSED
any ──watchdog(500ms × max-scan-time budget)──► RECOVERY (reset + re-enter)
```

- **watchdogs**: capture 500 ms per IRQ gap (`mesa-wff-watchdog-period`
  default), overall scan budget from DT (`max-scan-time` 78,643 ms);
  every timeout ends in power-off, never in an unbounded retry.
- **relay side** (in `sep-a9` or a thin `apple-mesa-sep.c` glue, only after
  the SBIO stack is proven): on frame completion, execute the §5.3 common
  prefix: `0x23`, `0x06` (151- or 167-byte payload — layouts and selection
  rule not yet recovered), chunked `0x65` (16,356 + 3,312), then fetch
  `0x07` (192). The subsequent `0x08`/`0x09`/`0x1f`/`0x22` traffic is
  state-dependent and must be driven by the recovered result/notification
  rule. The `0x06` payload bytes are an open item (§12); without them only
  the transport can be proven, not a match.
- **userspace**: unchanged Aurora v4 ABI (`/dev/sep-bio`); `sensor_present`
  flips on only after the patch path and one full relay cycle succeed.

### 11.3 Debugfs surface (bring-up only)

`arm` (enable), `power on|off`, `status` (one poll, hex dump), `irq-count`,
`capture once` (one frame into a kernel buffer, never persisted), `relay on`
(SBIO path), plus a one-shot `poll-trace` matching the iOS `mesa-verbose`
format. All write paths bounded and logged.

---

## 12. Staged, guarded test plan (proposals only — no device commands from this agent)

Every stage is opt-in per boot, one variable at a time, following the repo's
stop-rules style (`docs/sep/touchid-next.md`). None of these run from this
document; the orchestrator executes them on the device.

- **S0 (offline, no device)**: build the DT + driver with everything
  disabled; `dtc` round-trip; confirm zero probe actions without
  `mesa.enable=1`. Verify the LDO7/LDO15 decode once more against a second
  D2365 PMU path (e.g. dump `ioreg` LDO state from the running iPad in a
  later iOS-side capture).
- **S1 (DT + regulators, no sensor traffic)**: boot with the nodes present
  but `mesa.enable=0`; check `spi2` binds to `apple,s8000-spi`, `ps_spi2`
  powers, and read back PMU registers `0x312`/`0x319` through the angel-pmic
  regmap (reads only) to confirm the LDO decode **before** any write.
  Stop if the readback doesn't show plausible idle bits.
- **S2 (power-on + sanity poll)**: with sepOS known booted (per the existing
  tri53-sep protocol) or entirely without SEP (the sensor is a plain SPI
  device; SEP is not required for polls), `power on`, then exactly three
  status polls; require the `80 20 ?? 10` header and SensorID 0x3350 at
  idle, or `status[8] == 0x29` (bootROM candidate before the patch, see §8).
  If neither appears, stop. Any SPI-mode alternative belongs in a separately
  reviewed DT/boot, not an automatic runtime fallback.
- **S3 (interrupt test, only after the GPIO mode-3 translation is narrowed)**:
  select one board-DT trigger hypothesis, send TestCMD 0x78, and count IRQs
  (bounded, 3 attempts, then power off). A different trigger requires a fresh
  DT/boot, not a runtime change by the client driver.
- **S4 (one opaque capture, no SEP)**: `start_capture`, poll until state 7
  with size 19668, read one frame into a kernel buffer, record only the
  protocol result and length, zero the buffer immediately, `pause`, and power
  off. Do not print a prefix or biometric-derived digest. A correctly sized
  frame without a patch would falsify the strong patch-dependency hypothesis
  of §8.
- **S5 (SEP relay, single frame)**: only after the SBIO 0xfd/0xfe chunked
  framing lands in sep-a9 and the patch path is understood (§8): one common
  prefix `0x23 → 0x06 → 0x65 ×2 → 0x07`, then only the tail selected by the
  recovered SEP-result/async-state rule. The 151/167-byte `0x06` payload must
  come from a recovered source, never be guessed.
- **Device-side capture proposal (for the orchestrator, iOS side, read-only
  for factory state)**: one boot-time `idevicesyslog`/oslog capture plus a
  bounded SPI-frida or kext-log capture on the jailbroken iPad to obtain (a)
  the parameter frames (HostIntDepth, SetImageBitDepth) and (b) the patch
  upload bytes and their SBIO source opcode. This uses `scripts/ipad-ssh.sh`
  and is therefore orchestrator-only; it reads only, and must never write
  syscfg/NVRAM/boot firmware or send sensor provisioning commands.

**Hard safety rules (unchanged)**: never write syscfg, NVRAM, boot-firmware
storage; never send unbounded or guessed SEP messages; never send the
sensor's provisioning/session-key/NVM commands (§6) — they can persist keys
into the sensor's NVM, which is unrecoverable exactly like syscfg; never
persist fingerprint frames or full dumps; fingerprint image data stays in
volatile memory and is relayed to the SEP only.

---

## 13. Unresolved observations (with anchors)

1. **GPIO IRQ type for iOS specifier mode 3** (mesa `[44 3]`). Verified point:
   mode 0 → `IRQ_TYPE_EDGE_FALLING` (touch attn, works on hardware). Buttons
   use 7/5; PMU uses 1. Candidates: EDGE_BOTH or a level type. Anchor:
   `AppleGPIOICController::start` `FUN_fffffff00606ca00`, plus the empirical
   S3 test.
2. **Mesa SPI clock phase**: no ADT property; `spi-clock-phase` /
   `mesa-spi-clock-phase` exist only as kext overrides (references at
   `0xfffffff005efa0ac`/`0xfffffff005efa0c8`, inside a function Ghidra did
   not recognize — AppleSPIBiometricSensor::start **[U]**). Mode 0 assumed
   from the audio/touch precedent.
3. **The 32-byte SPI child reg blob** beyond unit-address/CS (§2.3):
   first-tuple "0x53/0x1f4/0x7c" fields and the 0x14/0x2710 second tuple are
   undecoded; bit 8 of the size word marks internal CS **[S]**.
4. **The sensor patch**: required each power-up; the SBIO opcode that serves
   it is unidentified (candidates 0x54/0x27 from
   `docs/sep/driver-notes.md` §7); the patch-upload SPI framing sender at
   `0xfffffff005efade4` is not recognized as a function by Ghidra; the
   `AppleMesaResources` blob source is not decoded. This is the gating
   unknown for any real capture.
5. **SBIO 0x23** (0 in/0 out, appears per frame before 0x06) — semantics
   unknown. Also 0x08 (3224 out) vs the table's `GetEnabledForUnlock 0x21/
   0x09` pair: 0x08 with 3224 out is adjacent-but-unnamed.
6. **Status byte +8 = 0x29** — strong candidate for "bootROM/unpatched OK"
   (parameter commands accept 0x29 or 0x00); needs a boot-time capture.
7. **HostIntDepth / SetImageBitDepth / SetEncryptedParameters parameter
   frames**: byte-exact layouts not recovered (built via CONCAT in the
   decompiler); need the boot-time SPI capture or a re-decompile of
   vtable+0x6f8 call sites.
8. **MOSI content during the 19,668-byte data phase** — presumed ignored by
   the sensor in read-data mode; verify at S4 (compare `ed fc 06` prefix).
9. **The AppleMesaShim/AppleMesaUserClientBase selector table** (the numeric
   external-method IDs used by AppleMesaSEPDriver) — method *names* are
   recovered from logs; numbers are not, and are not needed if Linux keeps
   the orchestration in-kernel/sep-bio.
10. **SIO DMA endpoints 0x18/0x19 for spi2**: numbering inferred from the
    T8010 binding's 0x1a/0x1b for spi3; confirm against the S8000 SIO
    firmware endpoint list before any DMA offload work.

---

## 14. What this document deliberately does not do

- No code changes, no DT changes, no commits (per the shared-checkout rules).
- No device commands were run and none are embedded as runnable steps; the
  S0–S5 plan above is a proposal for the orchestrator's guarded execution.
- No assumptions about the 0x06 payload, the patch bytes, or any SEP-side
  semantics beyond what the live trace pins down; guessed payloads stay out
  of the driver until their layouts are recovered (repo ground rule:
  "without guessing endpoint semantics").
