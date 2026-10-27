# iPad 5 (j71t) speaker playback path, reconstructed from iOS 16.7.10

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Sources: the ADT (`docs/adt/j71t.txt`), the live IOService plane
(`build/firmware/private/IOService.txt`, iOS running, idle), and the iOS
16.7.10 (20H350) kernelcache in the shared Ghidra project. Ghidra addresses
are kernelcache VAs. Helpers: `tools/audio/kcmap.py` (kext ranges, strings,
repacked Mach-O for `llvm-objdump`), `tools/audio/gdump.py` (bulk decompile),
`tools/audio/vtab.py` (vtable slots). Dumps are in `build/audio/` (gitignored).

**Verified** = read directly from the ADT, the live registry or decompiled or
disassembled code. **Inferred** = deduced, not yet confirmed on hardware.

## 1. Topology

| Block | Where | Driver (iOS) | Role |
|---|---|---|---|
| CS42L81 codec (control) | `/arm-io/spi1/audio-codec`, SPI1 `0x20a084000`, CS GPIO 16, IRQ AP-GPIO 54 | `AppleCS42L81Audio` (com.apple.driver.AppleCS42L81Audio) on base `AppleCSSPIv2Audio`/AppleEmbeddedAudio | codec + **both speaker amps** (`aout1-speaker`, `aout2-speaker`) + headphone (`hpout-hp`) + mics |
| CS42L81 codec (data) | `/arm-io/mca0/audio-codec` | `AppleARMIISDevice` → `AppleEmbeddedAudioDevice "Codec"` | ASP stream, 2 ch out / 2 ch in |
| MCA0 | `0x20a0a0000` (16 KiB) + MCLK word `0x20a002000`, AIC IRQ 201, PMGR `ps_mca0` | `AppleAE2MCA` (com.apple.driver.AppleAE2Audio), `mca-version 4` | I2S/TDM serializer |
| i2s-switch | `0x20a003000` (4 KiB) | `AppleAE2I2SSwitch3` | pad router; live `ActiveRoutes = ("AUD0 - PIN0")` |
| SIO DMA | `/arm-io/sio` `0x20ae00000`, IRQs 185/184/187/186, fw `s8000-smartio.bin` | `AppleSmartIODMAController` (AppleS8000SmartIO) | MCA0 TX/RX DMA (endpoints 0x28/0x29) |
| Speaker mute | AP GPIO 64 (`function-spkr_mute = GPIO(0x40, 0x10001)`) | AppleCS42L81Audio | external amp/speaker mute line |
| MCA1 (voice, XSP) | `0x20a0a4000`, `audio-codec-voice` | — | not needed for speakers |

No separate speaker amplifier exists in the ADT (**verified**: no amp node,
the codec node carries `aout1-speaker`, `aout2-speaker`, `spkr-gain-db`).
So unlike the A5 (Cascadia: CS35L19 amps), the CS42L81's own outputs drive
the speakers. `spkr-gain-db` = 0xfffffffd_00000000 = **-3.0 dB** (32.32
fixed point, **verified** encoding by value; meaning as max speaker gain is
**inferred**).

There is **no AE2 coprocessor** on this SoC path. AppleAE2MCA here talks to
the MCA registers directly (source path string `AppleA5AE2/AppleAE2MCA.cpp`).

## 2. Clocking: the codec is clock master, MCA0 is slave (verified)

Live MCA0 property `iisConfig` (the 36-byte descriptor iOS last applied,
from AppleAE2MCA at 0xfffffff006772fbc):

    10 00 00 00 02 00 01 00 00 1b b7 00 00 00 00 00 b9 01 0a 00
    03 00 00 00 03 00 00 00 02 02 10 10 02 00 00 00

| Byte | Value | Meaning (from `_setIISConfig_v4` and helper 0xfffffff005c6260c) |
|---|---|---|
| [0] | 0x10 | bit 0 = MCA is clock master → **0, slave**; bit 4 = MCA outputs MCLK → **1** |
| [1] | 0x00 | format flags (bit 1 off → frame reg bit 3 set; bits 5:4 = 0) |
| [2] | 0x00 | MCLK mode 0: MCLK derived from `nclk` by 1/2/4 |
| [4] | 2 | slots per frame |
| [5] | 0 | slot width 0 = round_up(max(sample bits), 8) = 16 |
| [6] | 1 | TX/RX config bits 15:8 (data delay) |
| [8..b] | 0x00b71b00 | MCLK = **12.000 MHz** |
| [0x10..13] | 441 / 10 | rate = 44.1 kHz (numerator/denominator in kHz) |
| [0x14], [0x18] | 3, 3 | TX / RX slot masks |
| [0x1c..1f] | 2, 2, 16, 16 | TX/RX channels, TX/RX sample bits |

The ADT template (`/arm-io/mca0/audio-codec reg`) is the same with rate 0.

Consequences (**verified** in code, **inferred** for hardware behavior):
- MCA0 only programs the **NCO in master mode** (`_setIISConfig_v4`
  0xfffffff0067735f4, `if (cfg[0] & 1)`), so the speaker path needs no NCO.
- MCA0 **outputs a 12 MHz MCLK** to the codec. The codec generates BCLK and
  LRCK from it (CS42L81 ASP master, reg `0x0012` bit 7, see section 4).

### MCLK control word (`mca0` second `reg`, 0x20a002000)

`_getMCLKClkCon` 0xfffffff0067747f4: with `cfg[2] & 3 == 0` it compares
`nclk` with MCLK: equal → 0x10, `nclk = 2×MCLK` → 0x18, `4×` → 0x20, else 0;
`| 0x40` when `cfg[0]` bit 4 (MCLK output) and enabled. Written by
`setIISControllerActive_Forced` 0xfffffff0067729e0 (`str w9,[x8]` at
0xfffffff006772b4c) and by `_setIISConfig_*`.

`nclk` comes from the provider's clock table (0xfffffff0059bb724:
`fclk aclk hclk pclk nclk` = `gPEClockFrequencyInfo` cpu/mem/bus/prf/**fix**).
mca0 has no `clock-ids` (**verified**, live ADT), so `nclk = fix_frequency_hz`.
That is 24 MHz (**inferred**: the A9 fixed clock, and the live `arm-io
clock-frequencies[0]` is 24 MHz). Hence **MCLK word = 0x58** (nclk/2, output
on), 0 when off. Fallback candidates for the device test: 0x50, 0x60, 0x40.

### NCO (for completeness, verified algorithm)

`AppleS5L8960XNCO` (0xfffffff0064e553c) = the Asahi `clk-apple-nco`
algorithm exactly: 0x4000 stride at PMGR+0x44000 (5 channels), LFSR poly
0xa01 / init 0x7ff for the coarse divider, `INC1 = 2·Fref − div·f`,
`INC2 = INC1 − f`, `ACCINIT = (INC1 ^ 1<<31) − f`, `CTRL = 0x80001000`
(bit 12 extra vs Asahi; bit 29 selects an alternate parent). Effective
Fref = 2.4 GHz / 7 = 342.857 MHz (special-cased in code), close to the A5's
measured 342 MHz. Every write waits for CTRL bit 30 to clear.

## 3. MCA v4 register map (verified from code; names are ours)

`readReg(i)` 0xfffffff0067756c0 / `writeReg(i,v)` 0xfffffff006775740 index
a 32-bit array, so offset = 4·i. Per-version index tables at
0xfffffff0057e794c (TX cfg), 0x…7964 (RX cfg), 0x…797c (global).

| Off | Idx | Name | Use |
|---|---|---|---|
| 0x00 | 0 | VERSION | must read 4 (`_initVersion` 0xfffffff0067723d8) |
| 0x04 | 1 | FIFO_SIZE | [9:0] TX, [25:16] RX; v4 expects 0x40 each (`_initFifoSize` 0xfffffff006772510) |
| 0x08 | 2 | SAMPLE | `(32−rx_bits)<<8 | (32−tx_bits)` → 0x1010 for 16 bit |
| 0x0c | 3 | RESET | write 3 (TX|RX), poll `&3 == 0`; stop writes 1 (TX) / 2 (RX) |
| 0x10 | 4 | FRAME / global | see below; bit 1 = run, set last at start, 0 when deactivated |
| 0x14 | 5 | PAD | frame padding bits (0 here) |
| 0x1c | 7 | IRQ_STATUS | W1C, 13 bits: 0 tx-underflow, 1 tx-overflow, 2 tx-low-water, 3 rx-underflow, 4 rx-overflow, 5 rx-high-water, 6 frame-error, 7 bclk-activity, 8 no-bclk, 9 fsync-activity, 10 no-fsync, 11 short-fsync, 12 rate-capture |
| 0x20 | 8 | IRQ_MASK | 1 = masked; config writes 0x1fff then 0x1fe4 |
| 0x24 | 9 | FLUSH? | write 1, poll bit 0 clear |
| 0x60 | 0x18 | TX_CFG | bit 0 enable |
| 0x64 | 0x19 | TX_FIFO_CFG | `depth | (depth−1)<<16` |
| 0x68 | 0x1a | TX_SLOTS | slot mask |
| 0x6c | 0x1b | — | written 0 |
| 0x80 | 0x20 | RX_CFG | bit 0 enable |
| 0x84 | 0x21 | RX_FIFO_CFG | `0x00030004` |
| 0x88 | 0x22 | RX_SLOTS | slot mask |
| 0x8c | 0x23 | — | written 0 |

The bit names come from the IRQ reporter strings at 0xfffffff0057e850e,
listed in bit order. The handler 0xfffffff006775740 reports `status & 0x5b`.

FRAME (0x10) = `(half_frame−1)<<24 | width_code<<20 | (slots−1)<<12 |
(mclk_div−1)<<8 | 0x40 | 8 | 4·(fmt bit0 ≠ master) | master`, with
width_code 0x1/0x2/0x3/0x4 for 16/20/24/32 bits (table 0xfffffff0057e7938).

**Speaker config (44.1 or 48 kHz, 2×16, slave), computed by hand from
0xfffffff0067735f4:**

    RESET=3, poll; reg 0x24=1, poll; MASK=0x1fff; STATUS=0x1fff
    SAMPLE=0x1010; TX_FIFO_CFG=0x002b002c (depth 44, for 44.1 k; 48 k → 48: 0x002f0030)
    RX_FIFO_CFG=0x00030004; PAD=0
    TX_SLOTS=3, 0x6c=0; RX_SLOTS=3, 0x8c=0
    TX_CFG=0x136 (enable later: 0x137); RX_CFG=0x132 (0x133)
    MASK=0x1fe4; FRAME=0x0f101048
    MCLK word (0x20a002000)=0x58
    start: TX_CFG=0x137, then FRAME |= 2
    stop:  TX_CFG=0, RESET=1, poll bit 0, then DMA stop (order verified,
           0xfffffff0067746fc..0x…4754); (deactivate) FRAME=0, MCLK word=0

**Data path: not a CPU FIFO register (inferred).** No code in AppleAE2MCA
touches a FIFO data register. Data flows through the SmartIO DMA "shim".
AppleSmartIODMAController logs `Shim Configured: EP XFER TIMEOUT FIFO
TRIGGER LIMIT` (0xfffffff006c1b3d0), and mca0's `dma-channels` decode as:

| entry | endpoint | XFER | TIMEOUT | FIFO | TRIGGER | used for |
|---|---|---|---|---|---|---|
| 0 | 0x28 | 0x21 | 0 | 0x300 | 0x180 | TX, 16-bit samples |
| 1 | 0x29 | 0x21 | 0 | 0x300 | 0x180 | RX, 16-bit |
| 2 | 0x28 | 0x22 | 0 | 0x300 | 0x180 | TX, 32-bit |
| 3 | 0x29 | 0x22 | 0 | 0x300 | 0x180 | RX, 32-bit |

The entry is selected by the MCA's DMA setup: index 0/2 for TX (`(bits > 16)
<< 1`), 1/3 for RX. The SIO "LIMIT" is the MCA's `setFIFODepth` value
(bytes per ~1 ms: `width·round4(rate_kHz·channels)` = 176 at 44.1 k). Pauli's
T8010 SIO DMA sends exactly this record as op 5 (configure), so its
configure call carries these words.

So speaker playback needs SIO DMA (endpoint 0x28, MEM_TO_DEV, continuous).
SmartIO validFIFODepth turns the requested 176/192 B into **limit 0x184**,
so the MCA0 TX shim record is `{0x21, 0, 0x300, 0x180, 0x184, 0, 0, 0}`.
Section 7 has the protocol; round 2 implements it.

## 4. CS42L81 control (verified unless marked)

### SPI framing

Base class AppleCSSPIv2Audio (AppleEmbeddedAudio kext): write
0xfffffff005dad82c, read 0xfffffff005dad980:

    write: 6C  page  reg&0x7f  00  value          (5 bytes)
    read:  6D  page  reg&0x7f  00  --  → value = rx[4] (full duplex 5 bytes)

Registers are 16 bit: page = reg >> 8. ADT SPI child `reg` byte 1 = 0 →
SPI mode 0 (**inferred**). No `spi-frequency` property; Linux uses 1 MHz.

### Identity

`0xfffffff005e6d940`: registers 0x01..0x03 read as `42 A8 1x` = "42A81".
0x05 = revision (rev 0xa0 sets 0x06 bit 7).

### Power-up (`enableAudio`, vtable +0x600, 0xfffffff005e6da58)

    MCLK on (12 MHz)
    [cold] 0x9901=0xa5, sleep 10 ms        (unlock/soft reset)
    0x9901=0x00
    read 0x2e 0x2f 0x33 0x7b 0x7c 0x106 0x528   (clear latched status)
    rev = 0x05; if rev == 0xa0: 0x06 |= 0x80
    0x07 &= ~0x80
    0x0f = (0x0f & ~0xcf) | (codec_is_clock_master ? 0x80 : 0x0c)
                                  # J71t: 0x80 (round 3 fix, see section 8)
    0x12 &= ~0xc0;  0x11 &= ~0x10
    (headset/Mikey setup, skipped)
    0x06 = (0x06 & ~0x05) | 0x04
    0x201=0xe0  0x219=0x00  0xc81f=0xff  0xc85f=0x0f  0xc96f=0x0e
    0x221 |= 0x20  0x223=0x08  0x224=0x09  0x204 |= 0x04  0x210 |= 0x04  0x230=0x0c
    setSampleRate(rate); updateRouting()

### Sample rate (`setSampleRate`, +0x650, 0xfffffff005e6f9f4)

Code table (0xfffffff005e6f908): 8 k=1, 11.025 k=2, 12 k=4, 16 k=5,
22.05 k=6, 24 k=8, 32 k=9, 44.1 k=0xa, 48 k=0xc.

    0x0f[3:0]=c; 0x12e=0x12f=c|c<<4; 0x123=c; 0x222=c; 0x205[3:0]=c; 0x211[3:0]=c; 0x22f=c

### Routing for "speaker playback only" (`updateRouting` 0xfffffff005e6dda0)

These are the writes for this state, hand-evaluated (**inferred**: the state
values below are our choice, not observed). The state: output active, input
idle, HP/line muted, speakers unmuted, codec ASP master, 2×16-bit slots.

    0x0012 = 0x80 | c              ASP clock master + rate (bit 7 when MCA is slave)
    0x0022..0x002b: ch n → (2n) | 0x20   ASP RX slot n: byte offset 2n, 2 bytes ((bytes−1)<<5)
    0x0129..0x012d = c|c<<4        per channel-pair rate
    0x000d = old & 0xc0            no mic paths
    0x0203=0xc4  0x020f=0x04
    0x0231[5:4] = (spk_mode != 2) << 4      (stereo speakers: 0)
    spkr_mute GPIO = muted while powering
    0x0223=0x08  0x0224=0x09  0x0006 |= 0x04
    0x0011[7:6] = 01               ASP playback on, capture off
    0x0131 |= 0x01
    0x000e &= ~0xa0                DAC power on
    0x0201=0xe0  0x0220[5,3]=1,1   HP and line drivers off
    0x022e[4:3]=00                 speaker A/B power on; sleep 50 ms
    0x000e |= 0x40                 ADC off
    speaker volume (below); spkr_mute GPIO = unmuted

### Volumes

- Speaker (`0xfffffff005e70a08`, controls 0x139 vol / 0x13a mute): with HP
  and line muted, `0x225 = 0x226 = v + 0x34` and `0x232 = 0x19`. With
  HP/line active, `0x225 = 0x226 = 0`, `0x232 = v + 0x34`. Mute writes 0x19.
  After an unmute it waits `(v+27)/2` ms (ramp). The encoding is the CS35L33
  one Cascadia found (0.5 dB steps, 0x00 = 0 dB, 0x34 = −102 dB, 0x19 = mute,
  **inferred** from the identical `+0x34`/0x19 pattern).
- Headphone (`0xfffffff005e70798`, 0x227), line (`0xfffffff005e7095c`,
  0x229, `+0x49`), muted value 0x40.

### Power-down (`disableAudio`, +0x608, 0xfffffff005e6f50c)

    0x227..0x22a = 0x40; 0x225 = 0x226 = 0x232 = 0x19
    0x220 = 0x28; 0x22e = 0x18
    0x06 = (0x06 & ~0x05) | 0x05      power down
    poll 0x2f bit 7 (PDN_DONE): 100 × 10 ms
    0x06 |= 0x40; sleep 10 ms; 0x9901 = 0xa5; MCLK off

### Speaker mute GPIO

`function-spkr_mute = GPIO(0x40, 0x10001)`. The flag 0x10001 also appears on
`spi_cs0` (active-low chip select), and 0x101 on power-enable lines. So
**inferred: active low**, GPIO 64 low = muted. Linux:
`mute-gpios = <&pinctrl_ap 64 GPIO_ACTIVE_LOW>`.

## 5. i2s-switch v3 (verified encoding, inferred route)

`AppleAE2I2SSwitch3`: 20 registers (+0x578 returns 0x14), version at
`+0xffc`. Defaults (+0x580, 0xfffffff006775f08): regs 0..13 = 0x00181818,
reg 14 = 0x18, regs 15..19 = 0. Apply (+0x590, 0xfffffff006775f90) writes
all 20 words. Route compiler (+0x588, 0xfffffff006776138):

- Nodes: `pinN` → reg N (0..4), `audN` (= mcaN) → reg 5+N, `dspN` → reg
  10+N, `dspc` → reg 14.
- Node codes: aud m → m, pin m → 8+m, dsp m → 0x10+m, dspc → 0x17,
  0x18 = none.
- reg[node][4:0] = the node's clock source; [12:8] / [20:16] = its data
  source for route direction 0 / 1.
- reg[15+pin] bit 0 = pin drives its clock lines, bit 4 = pin drives data.

The generic router (AppleARMIISSwitch in AppleIISController, around
0xfffffff005c64e4c) expands `i2sR('aud0','pin0',0x330303)` with the MCA's
master/slave flag. We did not evaluate the router. For codec-master the
physically required state is (**inferred**):

    reg[0]  (pin0) = 0x00000018   pin0 data (both directions) from aud0, no clock source
    reg[5]  (aud0) = 0x00080808   aud0 clocks and data from pin0
    reg[15]        = 0x00000010   pin0 drives data only (clocks come from the codec)

Variants for the device test are in `test-plan.md`.

## 6. Linux mapping

| iOS | Linux (tree `audio`) |
|---|---|
| AppleSamsungSPI on spi1 | `spi-apple-s8000` from **a9/touch** (`apple,s8000-spi`, PIO). nclk = 61 538 461 Hz (**verified**: live ADT `arm-io/clock-frequencies[0x12e−0x100]`, spi1 `clock-ids` = 0x12e) |
| AppleCS42L81Audio | new `sound/soc/codecs/cs42l81.c` (regmap over the 5-byte SPI protocol) |
| AppleAE2MCA + AppleAE2I2SSwitch3 + MCLK word | new `sound/soc/apple/s8000-mca.c` |
| AppleSmartIO + AppleSmartIODMAController | `apple-sio-core` + `apple-sio-dma` (Pauli1Go T8010 base + S8000, per-endpoint tags, multi-endpoint, cyclic, op 8 stop) |
| AppleEmbeddedAudio routing | `simple-audio-card`, codec = bit/frame clock master |

## 7. SmartIO (SIO) protocol on A9 (round 2)

iOS 16.7.10 AppleS8000SmartIO + AppleA7IOP, checked against Pauli1Go's
T8010 driver. **Everything here matches T8010 except where marked**, so the
Linux port is his driver plus generalisation. Ghidra names in parentheses
(renamed in the shared project).

### Coprocessor (verified, AppleA7IOPV1, iop-version 1 on both SoCs)

| Offset | Use | iOS function |
|---|---|---|
| 0x08/0x10 | AP physical address lo/hi of the firmware | `AppleA7IOPV1_setFirmwareRemap` 0xfffffff005b4e72c |
| 0x18/0x20 | IOP address lo/hi (0) | 〃 |
| 0x28/0x30 | size (iOS page-truncates; Linux rounds up) / 0 | 〃 |
| 0x38 | remap enable = 1 | 〃 |
| 0x44 | CPU control, bit 4 run | `AppleA7IOPV1_setCPURun` 0xfffffff005b4e7c8 |
| 0x80c, 0xc030/0xc038 | timebase sync: clear bit 1, write counter, set bit 1 | `AppleA7IOPV1_syncTimebase` 0xfffffff005b4e850 |
| 0xc00/0xc04, 0x4000/0x4004 | queue init 0x1111, then 1, then 0x1000 | disassembly 0xfffffff005b4e538 |
| 0x4008 / 0x4010 | inbox control / message (AP→SIO, 64-bit) | `AppleA7IOPV1_getInboxEmpty` 0xfffffff005b4f6d4 |
| 0x4020 / 0x4038 | outbox control / message (SIO→AP); bit 0 enable, 17 empty, 18-19 error | `AppleA7IOPV1_getOutboxEmpty` 0xfffffff005b4f734 |

The firmware (`build/firmware/lib/firmware/apple/s8000-smartio.bin`, the
AppleS8000SmartIO `__firmware` section) is a raw ARM image with its vector
table at IOP address 0 (**verified**: the first word is `0xea00040e`).

### Messages (verified)

64-bit: `endpoint[7:0] tag[15:8] opcode[23:16] param[31:24] data[63:32]`
(`AppleSmartIOEndpoint::synchronousMessage`, 0xfffffff006c1b92c area).

- Control ep 0, tag 0, op **0x6d, param 3** (`SMARTIO_PROTOCOL_VERSION`) =
  "OS active" → host setup. Op 0x6a = SIO panic
  (`AppleSmartIOControl_asyncMessage` 0xfffffff006c17d08).
- setParam = op 3 (param id, data), ack 0x65
  (`AppleSmartIOEndpoint_setParam` 0xfffffff006c178e8). Setup order (iOS =
  Pauli): ep0 0x0f/0x10 coredump 16K, ep0 0x1a/0x1b panic 8K, ep3 0x0d/0x0e
  perf 16K, ep0 1/2 DMA segments (0x7ff8 B, 0xaaa slots), ep0 0xb/0xc DMA
  responses (0x10400 B, 0x1040 entries)
  (`AppleSmartIODMAController_start_setupSharedMemory` 0xfffffff006c198a4).
- Endpoint numbers (name table 0xfffffff007ac40c8, `AppleSmartIO_endpointName`
  0xfffffff006c15c20): SPI0–3 TX/RX 0x14–0x1b, **MCA0 TX 0x28 / RX 0x29**,
  MCA1–4 0x2a–0x31, UART0–8 0x3c–0x4d, MCA_SLAVE0–4 0x50–0x59, PDM0
  0x5a/0x5b, DPA0/1 0x64–0x67.

### DMA endpoint (verified)

- Segment memory: 12-byte slots; a transfer = 6 header slots (word 14 =
  kIOMemoryRemote flag, word 15 = segment count) + one `{addr lo, addr hi,
  len}` slot per page-split segment (`AppleSmartIODMAController_getBuffer` /
  `segmentForHandle`, 0xfffffff006c1a498 / 0xfffffff006c1a608).
- Op 5 configure: data = slot of the shim record `[xfer, timeout, fifo,
  trigger, limit, w5, frame, w6]` (`AppleSmartIODMA_configureShim`
  0xfffffff006c1b3d0); ack 0x65. `frame` (setParam 10) stays 0: AppleAE2MCA
  only calls setFIFODepth and setDMAConfig on its DMA event source.
- Op 6 submit: data = first header slot; ack 0x65; later an async **0x68
  with param bit 7**, the same tag, data = bytes moved. 0x68 without bit 7
  = idle notification; 0x66 = async error (merged function
  0xfffffff006c1a8f8).
- Several submits per endpoint may be in flight (`_commands_in_flight`).
  `_startDMACommand` only waits while the channel is stopping, and
  `_completeCommand` asserts completions in submit order. Tags are per
  endpoint (`responseForEndpointAndTag` = ep·32 + tag − 1), so Linux
  allocates 1..31 per endpoint (Pauli: 1..0xfe global).
- Op 7 query (reply 0x69, position), unused in Linux. Op 8 stop: ack 0x65,
  then queued commands complete. iOS waits up to the caller's timeout for
  idle.
- validFIFODepth (`AppleSmartIODMA_validFIFODepth` 0xfffffff006c1b5f0): TX
  limit = clamp(req, min(trigger, fifo−trigger)+4, fifo); RX = min(req,
  trigger).

### Inferred for A9 (not in the ADT)

- `ps_sio` = PMGR 0x80160 (SIO is device index 12, right after SIO_BUSIF 10
  at 0x80150 and SIO_P 11 at 0x80158). Parent set to `ps_sio_p`.
- The RX mailbox IRQ is index 2 of the ADT list `[185 184 187 186]` = 187,
  by analogy to T8010's `[211 210 213 212]` → 213.

## 8. Round 3: device result and the codec clock fix

**Device (2026-10-22, `docs/test-logs/2026-10-22-audio-play.txt`):**
SmartIO, DMA, MCA0, codec ID and the card all came up, but there was no
sound and no clocks. MCA status showed no BCLK/FSYNC activity, every
MCLK/route variant also gave 0, and the codec never reported PDN_DONE.
MCA `+0x18` read `0x04000830`: if its low byte is the TX FIFO level, 0x30
equals the configured depth (**inferred**: SmartIO filled MCA0's FIFO and
nothing drained it).

**Cause found (verified in code): CS42L81 CLK_CTL bit 7.**
- `AppleCS42L81Audio::start` (0xfffffff005e6d2c8..d2e8) sets flag `+0x1ec`
  bit 0 = !(vtable +0x850)().
- +0x850 (AppleEmbeddedAudio 0xfffffff005da7f98) calls the IIS nub's
  +0x580, which is `AppleARMIISDevice::isMaster()` (0xfffffff005c63e10:
  `ldrb w8,[x0,#0x98]; and w0,w8,#1`, i.e. iisConfig[0] bit 0).
- `enableAudio` (0xfffffff005e6da58) then writes `0x0f[7:6,3:0] = flag ?
  0x80 : 0x0c`.
- On J71t, iisConfig[0] = 0x10, so isMaster = 0, the flag is 1, and 0x0f
  becomes **0x80 | rate**.
- Round 2 wrote 0x0c, the SoC-master value, while also making the ASP
  clock master (`0x12` bit 7). Bit 7 selects the codec's clock/master
  configuration, and iOS sets it when the codec provides BCLK/LRCK. The
  missing BCLK/FSYNC and the PDN_DONE timeouts are the expected symptoms of
  leaving it clear. The internal mechanism (e.g. clocking from an undriven
  SCLK) is **inferred**.

**Other questions from the brief:**
1. **Pads:** the live ADT has no `pin-groups` on any node (verified;
   AppleARMIODevice 0xfffffff0059bc178 would apply them). iOS's GPIO driver
   only restores pad configs after sleep (`no-restore-gpio-pins`). So audio
   pad functions come from iBoot, which still runs in the palera1n chain
   before pongo (inferred). `audio-test.sh diag` verifies it with a pad
   scan.
2. **MCLK source:** `AppleARMIISDevice::setMCLKFrequency` (0xfffffff005c63ef8)
   only re-applies the iisConfig with byte 0 |= 0x10, clock mode 0 and the
   new MCLK (verified). The MCA stays slave, so iOS never programs the NCO
   for this path. MCLK = nclk/2, where nclk is the fixed clock (live ADT
   `arm-io clock-frequencies[0]` = 24 MHz). The PMGR has no separate audio
   clock device (verified: device list).
3. **Codec clock/PLL:** the iOS power-up has no PLL step. The codec runs
   from MCLK with CLK_CTL bit 7 (above), `0x12` = 0x80 | rate.
4. **Waits:** iOS only waits 10 ms after the 0x9901 unlock before the
   register sequence, and polls PDN_DONE only at power-down. Nothing waits
   for MCLK lock.
5. `function-xsp_active` = `AppleARMFunctionIISActive`, a query of which
   MCA serves a pin group. It drives no pads.

## 9. Round 4: the idle hard lockup was an MCA IRQ storm

integ10: `audio-test.sh probe` → `cat s8000-mca-regs` hard-locked CPU0
(`docs/test-logs/integ10/audio-probe-hardlockup.png`).

- **Not a hung bus read (verified, photo plus disassembly of the same
  source):**
  - The return address `mca_regs_show+0x6c` is the `seq_printf` in the MCA
    register loop, after `pm_runtime_resume_and_get` (`+0x38`) succeeded.
  - The PC is `format_decode+0x84` (`and x13, x12, #0xff`), identical with
    the same x0–x29 in three backtraces, pstate `daif` clear.
  - CPU0 answered every backtrace IPI. A CPU waiting on a device read
    cannot take an IPI.
  - CPU0 was re-interrupted without retiring an instruction, so the
    timer starved: watchdog, RCU "0 ticks".
- **IRQ_STATUS bit 31 (verified):** it reads as set together with a
  pending cause (round-2 log `last status 0x80000008`). The iOS handler
  (Ghidra, merged into `AppleAE2MCA_writeReg` at 0xfffffff006775740) tests
  `status & 0x1fff` and logs "spurious intr" otherwise. Our handler counted
  bit 31 as handled, so no clear could ever stop it, and `IRQ_HANDLED`
  bypasses the kernel's spurious-IRQ guard.
- **Why at idle (inferred):** the dump powered MCA0 up from a real
  power-off. At reset the hardware IRQ mask is open (not verified), and
  with no BCLK/FSYNC the no-BCLK/no-FSYNC causes latch. Everything was
  masked in software only. At probe the domain was probably still on from
  iBoot, and in streams `prepare` programs the hardware mask first.
- **Fix (`fbe859a481`):**
  - The handler takes bits 12:0 only. A spurious entry re-syncs the
    hardware mask and returns `IRQ_NONE`.
  - Runtime resume masks and clears all causes before the IRQ is enabled.
    Runtime suspend disables the IRQ before power-off.
  - debugfs never powers up or reads a suspended block.
