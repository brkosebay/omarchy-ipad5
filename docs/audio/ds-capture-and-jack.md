# Capture and the 3.5 mm jack on J71t (A9 iPad 5)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


What Linux needs for **(1) the built-in microphones** and **(2) the headphone
jack** (output routing, plug detection, headset mic).

Sources: the live ADT (`build/firmware/private/IODeviceTree.txt`), the live
IOService plane (`build/firmware/private/IOService.txt`, iOS idle), the iOS
16.7.10 (20H350) kernelcache in the shared Ghidra project, and this workstream's
own code (`patches/audio/`, branch `a9/audio`). **Verified [V]** = read directly
from the ADT/registry/disassembly. **Inferred [I]** = deduced, not confirmed on
hardware. Playback is already working (`playback-path.md`, `test-plan.md` round
5), so nothing here repeats the speaker bring-up.

---

## 0. Bottom line

**Built-in mics — small job, no new hardware block.**
The two built-in mics are **digital MEMS mics wired to the CS42L81**
(`dmic1a-intmic` = "cimi", `dmic1b-intmic` = "cims", `product/builtin-mics = 2`
[V]). They are digitised *inside the codec*; from the SoC's point of view this
is the **same MCA0 / SmartIO / 12 MHz MCLK path as playback, in the reverse
direction**: codec ASP-TX → MCA0 RX → SIO DMA **endpoint `0x29`** [V]. No PMU
rail, no GPIO, no AOP, no PDM endpoint is involved.

Concretely that is: a capture DAI + ~8 codec registers (ADC/DMIC power, route,
rate, volume, DMIC bias) + MCA RX DMA + a DT `dmas`/`dma-names = "rx"` entry +
a capture DAI link. Estimated **~350 lines** of driver code and one boot to
verify. This is the cheap, high-confidence half of the task.

**3.5 mm jack — output is easy, plug *events* need one experiment, headset mic
is the real work.**
There is **no jack-detect GPIO described anywhere in the ADT** and no PMU
(`Lump`) GPIO carrying an audio function [V]. The only jack-related lines are
`function-ext_sw1`/`ext_sw2` (AP GPIO **68** and **66**) and the codec's own
interrupt line (**AP GPIO 54**), and iOS puts the whole plug/headset state
machine *inside the codec driver family* (`AppleCS42L81Mikey` over
`AppleCSEmbeddedAudio`, not a separate kext) [V].
Output routing (speaker ↔ headphone) is a short, fully decoded register diff in
the same codec we already drive [V]. Plug *detection* is codec-side and
**polled**: the Mikey personality builds a timer event source and its
`IOInterruptEventSource` with a *null* provider [V], so iOS debounces the jack
state on a timer rather than on the codec IRQ. The exact status bit is not yet
identified, but the existing `cs42l81-regs` debugfs plus one headphone
plug/unplug answers it with **no new code** (test T1 below).
The headset mic additionally needs the external "hs-switch" (GPIO 66/68) and an
analog mic bias whose control we have not found in the codec register set [I].

Risk order: capture (low) < headphone output (low/medium) < jack events
(medium) < headset mic (medium/high).

---

## 1. What the device tree actually says

### 1.1 Codec node `/arm-io/spi1/audio-codec` (AppleARMSPIDevice)

| Property | Value | Meaning |
|---|---|---|
| `compatible` | `audio-control,cs42l81` | CS42L81 control port on SPI1 CS0 [V] |
| `interrupts` / `interrupt-parent` | `<0x36 0x1>`, parent phandle `0x1e` (AP GPIO) | codec IRQ on **AP GPIO 54** [V] |
| `aout1-speaker`, `aout2-speaker` | `<>` | speaker outputs (playback, done) |
| `hpout-hp` | `<>` | **headphone output** [V] |
| `lineout1-lineout`, `linein-linein` | — (only in the kext's port list, not in this ADT) | line out / line in, unused here [V] |
| `dmic1a-intmic` | `"cimi"` | built-in mic 1 = DMIC1A [V] |
| `dmic1b-intmic` | `"cims"` | built-in mic 2 = DMIC1B [V] |
| `mic2-extmic` | `<>` | **analog external (headset) mic** [V] |
| `function-spkr_mute` | `GPIO(0x40, 0x10001)` | speaker mute, AP GPIO 64 (done) |
| `function-ext_sw1` | `GPIO(0x44, 0x101)` | hs-switch line, **AP GPIO 68** [V] |
| `function-ext_sw2` | `GPIO(0x42, 0x101)` | hs-switch line, **AP GPIO 66** [V] |
| `hs-switch-workaround` | `1` | external headset-switch workaround enabled [V] |
| `function-mbus_dock_parent` | phandle `0x84` (Pcca) | Mikey/MBUS routed to the dock port [V] |
| `spkr-gain-db` | −3.0 dB | done |

Not present on this node (or anywhere in the live ADT) [V]:
`function-headset_switch`, `HPDetectInvert`, `hp-detect-invert`,
`function-micbias-enable`, `function-diff_mic_sel`, any `jack*`/`hpdet*`
property, any PMU/`Lump` GPIO for audio, any `pin-groups`.

> `function-micbias-enable` **does** exist in the kernelcache but belongs to the
> ambient-light/proximity sensor driver (`0xfffffff00676cd5c`, "ica60",
> `mic-bias-index`), not to audio [V]. Do not chase it.

### 1.2 Data node `/arm-io/mca0/audio-codec@10` (AppleARMIISDevice)

`compatible = "audio-data,cs42l81"`, `reg` = the 36-byte `iisConfig` template
with **rate 0**; the live MCA0 copy (`playback-path.md` §2) is the same with
44.1 kHz filled in. TX slot mask 3, **RX slot mask 3**, 2 slots, 16-bit [V] —
i.e. iOS keeps both directions mapped even when only the speakers are running.

### 1.3 MCA0 (`/arm-io/mca0`, `mca,s8000`, `mca-version 4`)

`dma-channels` (four 32-byte entries) [V]:

| # | endpoint | XFER | TIMEOUT | FIFO | TRIGGER | use |
|---|---|---|---|---|---|---|
| 0 | `0x28` | `0x21` | 0 | `0x300` | `0x180` | MCA0 TX (playback) 16-bit |
| 1 | **`0x29`** | `0x21` | 0 | `0x300` | `0x180` | **MCA0 RX (capture) 16-bit** |
| 2 | `0x28` | `0x22` | 0 | `0x300` | `0x180` | MCA0 TX 32-bit |
| 3 | `0x29` | `0x22` | 0 | `0x300` | `0x180` | MCA0 RX 32-bit |

RX shim limit differs from TX: `validFIFODepth` gives TX
`clamp(req, min(trig,fifo−trig)+4, fifo)` but **RX `min(req, trigger)`**, so the
RX record is `{0x21, 0, 0x300, 0x180, 0x180, 0, 0, 0}` (TX used `0x184`) [V
formula, I for the number].

Also on mca0: `function-mclk_frequency` = phandle `0x1b` + `"fOCN"` — the MCLK
frequency can be changed at runtime by a callback [V]; `function-i2s_route` =
`i2sR(aud0,pin0,0x330303)` (done).

### 1.4 The other audio nodes are *not* our path

* `/arm-io/mca1` + `audio-codec-voice` (`audio-data,voice`, route
  `i2sR(aud1,pin2,0x030303)`) — voice/XSP codec, separate [V].
* `/arm-io/aop/mca` (`AppleSPUAppInterface`, `mca-master = 1`,
  `channel-bits = 4`, `frame-channels = 0xb`, `mclk-source = 1`) — the AOP's own
  mic array, behind the AOP firmware, unreachable from Linux [V].
* SmartIO PDM endpoints `0x5a/0x5b` exist in the endpoint table but no PDM node
  is present; the built-in mics are **not** PDM-to-SmartIO [V].

---

## 2. The built-in mic path

```
DMIC1A ─┐                                    ┌─ ASP TX ─┐
        ├─ CS42L81 ADC/DMIC block ───────────┤          ├─ MCA0 RX ─ SIO DMA ep 0x29 ─ RAM
DMIC1B ─┘   (0x000d, 0x0203/04/05, 0x215/16)  └──────────┘   (regs 0x80..0x8c)
                                     ▲
                            12 MHz MCLK (MCA0 → codec)
                            BCLK/LRCK ◄── codec (ASP master, 0x0012 bit 7)
```

Identical clocking to playback: the codec is ASP master, MCA0 slave, MCLK word
`0x58` (nclk/2 = 12 MHz) [V]. The only difference is direction: the codec
transmits on ASP TX, MCA0 receives.

### 2.1 Which codec ADC path

From `AppleCS42L81Audio::updateRouting` (`0xfffffff005e6dda0`) [V]:

* The **input** half of that function runs when
  `(vtable+0x740)(this, 1) & 1` ("input stream active") and writes:
  `0x000d`, `0x0203`, `0x020f`, `0x0204`, `0x0205`, `0x0206`, `0x0207`,
  `0x0208`, `0x0210`, `0x0212`, `0x0215`, `0x0216`, the ASP-TX slot registers
  `0x0014..0x001d` / `0x0115..0x011e` / `0x0124..0x0128`, and `0x011f/0x0120`.
* The **output** half (`(vtable+0x740)(this, 2)`) is the playback path we
  already implement.
* Both halves finish with the shared power/mute tail (`0x0131`, `0x000d`,
  `0x0220`, `0x022e`, volumes) [V].

The route names in that function are 4CCs read as little-endian u32 from the
DT strings, so the immediate `0x736d6963` is the string **"cims"** and
`0x656d6963` is **"cime"** [V]; `0x6c696e4c`/`0x6c696e52` are compile-time
constants `'linL'`/`'linR'` [V].

Three "input devices" (`this+0x20c` .. `+0x214`) are matched against the active
route flags and set bits 1, 3, 5 of `0x000d` [V]; the natural reading is one bit
per input path (internal mic / analog mic / line-in), **not one per DMIC channel**
[I] — this is O7 in §7.

For the `0x0203` value the function starts at **`0x04`** and XORs `0x40`
(ADC A) and `0x80` (ADC B) for each ADC whose `dmic1a-intmic` property exists
and is not `"cims"` [V code]. On this device (`"cimi"`) that gives
`0x04 ^ 0x40 ^ 0x80 = 0xc4` — **the same value the playback-only state writes**
[V]. So `0x0203` does *not* distinguish "mic on" from "mic off" here; the
registers that actually gate capture are `0x000d` (path power), `0x0215/0x0216`
(DMIC bias/clock) and `0x0205` (ADC rate/enable). If capture stays silent,
sweep `0x0203` through `0xc4/0x84/0x44/0x04/0x00` (T2c).

### 2.2 Codec capture register sequence

Apply on top of the existing core power-up (`cs42l81_core_power_up()`, which
already does `0x9901` unlock, ID/status clear, `0x0f`, `0x12`, trims,
`0x0201=0xe0`, `setSampleRate()`, `set_asp()`).

| Reg | Value / expression | Meaning | Source |
|---|---|---|---|
| `0x0203` | `0xc4` (starts at `0x04`, XOR `0x40`/`0x80` per ADC) | ADC input select — **not** the mic on/off switch here | [V] |
| `0x020f` | `0x04` (default; `0x00` for routes 5/6) | ADC_CTL_B | [V] |
| `0x0204` | `update_bits(0x204, 0x73, 0x20)` for DMIC (bit 5 set); `0x10`/`0` for the analog routes | ADC A control (input/PGA select) | [V] mask, [I] bits |
| `0x0210` | unchanged for DMIC (mask `0x43` does not cover the written bit) | ADC C control | [V] |
| `0x0205` | `rate_code \| 0x10` (bit 4 = ADC A enable, bits 7:6 = ADC B) | ADC rate + enable | [V] from loop, [I] bits |
| `0x0211` | `rate_code` (bits 3:0) | ADC B rate | [V] |
| `0x0206`,`0x0207` | `(v + 0x34) & 0x3f \| range` | ADC A/B digital volume, 0.5 dB steps, −102 dB at 0x00→code 0x34, range bits 6/7 | [V] formula |
| `0x0208`,`0x0212` | raw 0..0x3f, `0x80` = max | mic boost / PGA (from control `0x16f`) | [I] |
| `0x0215` | `(bias ? 0x80 : 0) \| div` | **DMIC1A bias/clock** | [V] bits, [I] semantics |
| `0x0216` | same | **DMIC1B bias/clock** | [V] bits, [I] semantics |
| `0x000d` | `(old & 0xc0) \| (in0<<1) \| (in1<<3) \| (in2<<5)`, then **20 ms** if any bit was set. Internal-mic path = bit 1 (set `0x02`); analog mic likely bit 3, line-in bit 5 [I] | ADC/mic path power | [V] formula, [I] which bit |
| `0x000e` | clear bits 7 and 5 (`mask 0xa0`) | power the capture path; bit 6 left as playback set it | [V] mask, [I] bits |
| `0x0011` | bit 7 = capture/ASP-TX enable (bit 6 = playback/ASP-RX) | ASP power | [V] from the `~swap(active)` computation |
| `0x0014`,`0x0015` | `0x00`, `0x02` (byte offset \| `(bytes−1)<<5`; **bit 7 = slot empty**) | ASP TX slot 0/1 placement | [V] encoding |
| `0x0115`,`0x0116` | `0x18`, `0x1c` | ASP TX slot source = ADC A / ADC B | [V] codes |
| `0x0124` | `rate \| rate<<4` | ASP TX pair-0 rate | [V] |
| `0x011f`,`0x0120` | iOS `0x18`,`0x1c`; **we propose `0x00`,`0x02`** | ADC A/B slot offset (see open item O1) | [V] iOS values, [I] encoding |
| `0x0131` | set bit 0 (`0x01`) | ASP control | [V] |

`div` in `0x0215/0x0216` follows the *sample-rate code* [V values]:
`0x70` for codes 1..8 (8 k–24 k), `0x40` for 9 (32 k), `0x10` for 0xa/0xb
(44.1 k), and `0` for 0xc (48 k) — the 48 k case looks like a driver bug or an
unreachable path for the intended usage [I]; start with `0x10` for both 44.1 and
48 k and confirm by capture (T2).

### 2.3 MCA0 RX

The existing driver already writes the RX block in `prepare()` but leaves it
disabled (`RX_FIFO_CFG = 0x00030004`, `RX_SLOTS = tx_slots`, `RX_SLOTS_HI = 0`,
`RX_CFG = 0x133 & ~EN`) [V `patches/audio/0004`, `0012`]. Capture is therefore
mostly "turn on what is already programmed":

```
RESET = 3 (TX|RX), poll !(&3)
FLUSH = 1, poll !(bit0)
IRQ_MASK = 0x1fff ; IRQ_STATUS = 0x1fff
SAMPLE = (32-bits)<<8 | (32-bits)          # 0x1010 for 16-bit
RX_FIFO_CFG = 0x00030004
RX_SLOTS = 0x3 ; RX_SLOTS_HI = 0
RX_CFG = 0x132                             # same value iOS uses
FRAME = half_frame<<24 | width<<20 | (slots-1)<<12 | mclk_div<<8 | 0x40 | 8 | fmt
start:  RX_CFG |= 1 ; FRAME |= 2 (run)
stop :  RX_CFG = 0 ; RESET = 2, poll !(bit1)   # TX stops with RESET = 1, RX with RESET = 2
```

RX interrupt causes are the mirror of TX: bit 3 underflow, 4 overflow,
5 high-water (`playback-path.md` §3) [V].

### 2.4 What the DMICs are *not*

* Not PDM into SmartIO (`0x5a/0x5b` unused here) [V].
* Not the AOP MCA (`/arm-io/aop/mca`) [V].
* No `micbias` GPIO/regulator exists on the audio nodes; the DMIC "bias" is
  register `0x0215/0x0216` [V, per the `%s-bias` node names in the kext].

---

## 3. The 3.5 mm jack

### 3.1 How iOS detects a plug

The state machine is in **`AppleCSEmbeddedAudio`** (`0xfffffff005dc2000`), in
`AppleCSHTDCodecMikey` (instance size `0x1b0`) which `AppleCS42L81Mikey`
(`0x1b8`, in the CS42L81 kext) extends with its provider pointer at `+0x1b0`
[V sizes and call chain, I class naming]:

| Address | Role |
|---|---|
| `0xfffffff005dc2f4c` | switch object start: creates an `IOTimerEventSource` (+0xf8) and an `IOInterruptEventSource` (+0x100) **with a null provider**, reads boot-arg `hp-detect-invert` into `this[0x128]`, then enables detection |
| `0xfffffff005dc3308` | enable/disable the interrupt source (vtable+0xb0/+0xb8) |
| `0xfffffff005dc3384` | **read plug state** (vtable+0x6a8): reads the object at `this+0x120` through a function-style read (that object is not assigned in this kext, so it comes from the parent [I]) |
| `0xfffffff005dc3420` | **detect handler** (timer/IRQ): if plugged → log "Headphones detected. Begin mic detection" → vtable+0x680; else notify unplugged. Applies `this[0x128]` as invert. There is a second source `this+0x130` (+0x88 = `isPluggedIn`) used when present |
| `0xfffffff005dc456c` / `0xfffffff005dc461c` | `startMicDetected` / `continueMicDetect`: calls vtable+0x7a8 (Mikey type detect) and caches the result in `this+0x16c` (type & 3: 1 = button-only, 2 = headset-with-mic) |
| `0xfffffff005dc8138` | reads `function-ext_sw1`, `function-ext_sw2`, `hs-switch-workaround` into `this+0x198`, `+0x1a0`, `+400` (called from `AppleCS42L81Audio`'s start) |
| `0xfffffff005dc8228` | drives/reads the two ext_sw lines through `AppleARMFunction`, caches the last value at `this+0x1a8`, then **`IOSleep(50)`** to settle |

State names published to the registry [V strings + live registry]:
`Detached`, `HeadphonesOnly`, `HeadphonesWaiting`, `SingleButton`, `MicOpen`,
`WaitingForMic`; plus properties **`HPDetect`**, **`MicDetect`**,
**`ButtonDetect`**, **`DetectConfidence`**.

Live values on this iPad with nothing plugged (iOS idle) [V]:

```
AppleCS42L81Audio: DetectConfidence = "CompleteFinal"
                   HPDetect = No, MicDetect = No, ButtonDetect = No
AppleCS42L81Mikey: Product = "Headset", Manufacturer = "Apple",
                   Transport = "Audio", HIDDefaultBehavior = Yes
```

So the perception path is: codec plug state (Mikey/HS detect, read through the
`this+0x120` function object) → timer-debounced state machine in
`AppleCS42L81Mikey` → registry properties → userspace. **No separate jack GPIO
and no PMU involvement** [V].

Note the codec IRQ itself: the ADT declares `interrupts = <0x36 0x1>` on the
codec node [V], but the class that would consume it registers its event source
with a **null provider** [V disassembly], and the Mikey bus controller does the
same [V]. So in practice iOS polls/debounces; treat the codec IRQ as an optional
accelerator, not the primary mechanism (this is why §5.4 starts with a poll).

Two things remain open and are cheap to settle on hardware:

* **O2:** which register/bit the plug state comes from. The codec's latched
  status registers `0x002e, 0x002f, 0x0033, 0x007b, 0x007c, 0x0106, 0x0528` are
  exactly what `enableAudio` clears at power-up and what our debugfs already
  dumps [V] — one plug/unplug comparison finds the bit (test **T1**).
* **O3:** whether `ext_sw1/2` are outputs (switch control) or inputs (detect
  switches). The 50 ms settle and the cached-value skip in `0x5dc8228` look like
  outputs [I]; the DT flag word `0x101` differs from the known-output lines
  (`0x10001` on `spkr_mute` and `spi_cs0`) [V]. Test **T1b** reads both pins as
  GPIO inputs in Linux and checks them across a plug.

### 3.2 Speaker ↔ headphone switching (codec side)

All from `updateRouting` (`0xfffffff005e6dda0`, output half) and
`setHeadphoneVolume` (`0xfffffff005e70798`) [V]:

**Plug in / headphone active**

| Reg | Write | Effect |
|---|---|---|
| `0x0c96f` | `0x0e` | HP trim / charge-pump pre-charge |
| `0x0219` | `0x01` | HP charge pump on |
| — | delay ≈155 µs (`IODelay(0x9b)`) | |
| `0x0c96f` | `0x1e` | HP trim, running |
| `0x0220` | `update_bits(0x220, 0x28, 0x08)` → bit 5 = 0 (HP driver on), bit 3 = 1 (line driver off) | output driver power |
| `0x0227`,`0x0228` | `vol` (`0x40` = mute) | HP L/R volume |
| `0x0225`,`0x0226` | `0x19` | speaker volumes → mute |
| `0x0232` | `vol + 0x34` | main volume now carries the level (HP/line active) |
| `0x022e` | set bits 4:3 (`0x18`) | speaker A/B powered down |
| `0x0231` | bits 5:4 = `(spk_mode != 2) << 4`, bit 5 set while the speakers are off | speaker amp config |
| `0x0201` | `0xe0` (transitionally `0xc0`) | output control |
| — | sleep 50 ms (switch/ramp), 60 ms after enabling HP | |

**Plug out / speaker active** — the mirror image: `0x0c96f = 0x0e; 0x0219 = 0x00`
(charge pump off), `0x0227/0x0228 = 0x40` (mute), `0x0220` bit 5 = 1 (HP driver
off), `0x022e &= ~0x18` (speakers on), `0x0231` bit 5 cleared.

Linux already has every one of these registers in the driver's header; the
speaker state it currently writes is the "plug out" case, so the change is a
DAPM-controlled headphone path plus a jack-driven route switch.

### 3.3 Headset mic (analog, `mic2-extmic`)

* Selected by route name **"cime"** in `updateRouting`: sets bit 4 of `0x0204`
  and the `0x0211`/`0x0212` ADC-B side [V code, I bits]; the ADC B path then
  behaves like the built-in mic path, but analogue [I].
* The ADC config is *conditional on the detected headset type*:
  `FUN_...5e30` returns the cached type (`this+0x16c`), and
  `if ((type & 3) != 2) uVar17 = 0` — i.e. only "headset with mic" gets the
  extra `0x0204` bit [V].
* **Mic bias:** no codec register write in the whole kext looks like an analog
  mic-bias control; the only "bias" knobs are the DMIC `0x0215/0x0216` and the
  external switch driven by `ext_sw1/2` [V negative result, I conclusion]. Most
  likely the bias is switched/composed by that external hs-switch, which is why
  `hs-switch-workaround = 1` exists [I].
* Button/remote and CTIA-vs-OMTP identification run over the **Mikey bus**
  (`AppleCS42L81MikeyBus`, `AppleCSCodecMikeyBus`, "transmitter tone id",
  "earpiece design", "mic design", "Mikey headset type detect timeout") [V].
  Not needed for basic capture.

Practical stance for Linux: implement **headphone output + plug events + the
built-in mics** first, and treat the headset mic as a second phase that starts
by copying iOS's `ext_sw` states (park both lines, then try the four
combinations) while watching `arecord` levels.

---

## 4. What our drivers have today

| Piece | File (tree `audio`, `a9/audio`) | Capture/jack status |
|---|---|---|
| Codec | `sound/soc/codecs/cs42l81.c` | playback only: `cs42l81_dai` has `.playback`, DAPM `Core`/`ASPRX`/`Speaker Amp`/`SPKOUT`, `set_asp()` only writes ASP RX slots `0x22..0x2b`, `0x0203=0xc4` and `ADC_PWR(0x000d) &= 0x3f` hard-coded to "no mic" |
| MCA | `sound/soc/apple/s8000-mca.c` | playback only: `snd_soc_dai_init_dma_data(dai, &dma_tx, NULL)`, one DMA channel (`dmas = <&sio 0x28>`), RX registers programmed but disabled, IRQ handler ignores RX causes |
| SIO DMA | `drivers/dma/apple-sio-dma.c` | **already supports `DMA_DEV_TO_MEM`** and cyclic (patch 0011) |
| Card/DT | `arch/arm64/boot/dts/apple/s8003-j71t-audio.dtsi` | one bidirectional-capable link but only `mca0` TX wired; no capture DAI link, no mic widgets |
| Jack | — | nothing: no `interrupts` on the codec node, no `hp-det-gpios`, no jack kcontrol, no `snd_soc_jack` |
| Tools | `tools/audio/a9audio.c`, `tools/audio/audio-test.sh` | play/spi/mixer/pads/peek only, no record mode |
| Debugfs | `cs42l81-regs`, `cs42l81-reg` | dumps `0x2e/0x2f/0x33/0x7b/0x7c/0x106/0x528` and `0x000d/0x000e/0x0203/0x0204/0x0205/0x020f/0x0210/0x0211/0x0219/0x0c96f` — already enough for T1; missing `0x0014..0x001d`, `0x0115..`, `0x011f/0x0120`, `0x0206..0x0208`, `0x0212`, `0x0215/0x0216` |

---

## 5. Driver change list

### 5.1 Codec — `sound/soc/codecs/cs42l81.c` (~250 lines)

1. **Header**: add `CS42L81_ADC_SLOT_A 0x011f`, `ADC_SLOT_B 0x0120`,
   `ASP_TX_SLOT(n) (0x0014 + (n))`, `ASP_TX_SRC(n) (0x0115 + (n))`,
   `ASP_TX_PAIR_RATE(p) (0x0124 + (p))`, `ADC_VOL_A 0x0206`, `ADC_VOL_B 0x0207`,
   `MIC_BOOST_A 0x0208`, `MIC_BOOST_B 0x0212`, `DMIC_CTL_A 0x0215`,
   `DMIC_CTL_B 0x0216`, and bit macros for `0x000d`/`0x000e`/`0x0011`.
2. **`cs42l81_capture_power_up(cs, on)`**: the table in §2.2, in this order —
   `0x0203` (`0xc4`), `0x020f` (`0x04`), `0x0204` (clear `0x73`, set `0x20`),
   `0x0205`/`0x0211` (rate, `|= 0x10` on A), `0x0206..0x0208`/`0x0212`
   (volume), `0x0215/0x0216` (bias + divider), `0x000d |= 0x02`
   (then `msleep(20)`), `0x000e` clear `0xa0`, `0x0011 |= 0x80`,
   `0x0014/0x0015`, `0x0115/0x0116`, `0x0124`, `0x011f/0x0120`,
   `0x0131 |= 1`. Reverse (and `0x0215/0x0216 = 0`) on power down. Every wait
   bounded (20 ms sleep only).
3. **DAPM**: `SND_SOC_DAPM_AIF_OUT("ASPTX", "ASP Capture", 0, ...)`,
   `SND_SOC_DAPM_INPUT("MIC1")`, `SND_SOC_DAPM_INPUT("MIC2")`,
   `SND_SOC_DAPM_SUPPLY("DMIC bias", ...)` with a `PRE_PMU/POST_PMD` event, and
   routes `ASPTX → Core`, `MIC1 → ASPTX`, `MIC2 → ASPTX`.
4. **DAI**: add `.capture = { .stream_name = "ASP Capture", 2ch, 8 k–48 k, S16/S32 }`;
   extend `cs42l81_hw_params()` to program the TX slots
   (`0x0014/0x0015 = offset | (bytes−1)<<5`, `0x0115/0x0116 = 0x18/0x1c`,
   `0x0124 = rate|rate<<4`) the same way `set_asp()` does for RX.
5. **Controls**: `"Capture Volume"` (0x0206/0x0207, TLV 0.5 dB, −102..0 dB,
   reuse the `+0x34`/range-bit encoder already used for speakers),
   `"Mic Boost"` (0x0208/0x0212, 0..0x3f, 0x80 = max) [I range].
6. **Jack** (phase 2, §5.4): `snd_soc_jack` + `snd_soc_jack_gpio` on the codec
   IRQ, `hp`/`mic`/`btn` jack pins, and a `cs42l81_set_output(cs, hp)` that
   writes the §3.2 table.
7. **debugfs**: extend `cs42l81_dbg_regs[]` with the registers in §4.

### 5.2 MCA — `sound/soc/apple/s8000-mca.c` (~80 lines)

1. Parse a second `dmas`/`dma-names = "rx"` entry; request the channel with the
   same shim words (`apple,sio-dma-config`, RX limit `0x180`), store
   `struct snd_dmaengine_dai_dma_data dma_rx`.
2. `snd_soc_dai_init_dma_data(dai, &dma_tx, &dma_rx)`; add `.capture` to
   `mca_dai`; sibling `mca_capture_*` substream state (`mca->substream_rx`).
3. `prepare()`: keep the shared init; for capture set `RX_CFG = 0x132`,
   `RX_SLOTS =` the capture slot mask (default `0x3`), and turn on the RX
   causes in `irq_mask`.
4. `trigger()`: start `RX_CFG |= 1` before `FRAME |= 2`; stop with
   `RX_CFG = 0`, `RESET = 2`, poll `!(val & 2)` (1 ms atomic).
5. IRQ handler: treat bits 3/4/5 as RX causes; count them in the existing
   `irqs/errors/spurious` debugfs line and never touch bit 31.
6. debugfs: one extra line `rx last stream` with the same first-error timing.

### 5.3 DT — `arch/arm64/boot/dts/apple/s8003-j71t-audio.dtsi` (~30 lines)

```dts
mca0 {
    dmas = <&sio 0x28>, <&sio 0x29>;
    dma-names = "tx", "rx";
    /* existing property covers TX; either reuse it for both directions or add
       a separate one for RX, whose FIFO limit is 0x180 instead of 0x184 */
    apple,sio-dma-config = <0x21 0x22 0 0x300 0x180>;
};

sound {
    simple-audio-card,widgets = "Speaker", "Speakers", "Microphone", "Mic";
    simple-audio-card,routing =
        "Speakers", "SPKOUT",
        "Mic", "MIC1", "Mic", "MIC2";
    /* the existing dai-link becomes bidirectional as soon as the codec DAI
       advertises .capture; no second link is needed */
};

&cs42l81 {
    /* optional: ADT declares interrupts = <0x36 0x1> with the AP GPIO as
       parent, but iOS drives detection from a timer, so add this only after
       T1 shows a usable status bit (see §5.4) */
    interrupt-parent = <&pinctrl_ap>;
    interrupts = <54 IRQ_TYPE_EDGE_BOTH>;
};
```

Pads: the live ADT has no `pin-groups`, and iOS only restores pad configs after
sleep (`playback-path.md` §8), so GPIO 54 comes up configured by iBoot — no
`pinctrl-0` needed until proven otherwise.

### 5.4 Jack detection (phase 2)

1. **Poll first** (this is what iOS effectively does): a workqueue/`delayed_work`
   that reads the seven status registers every 250 ms while the card is open,
   diffs them against the previous values, logs the changed bit once, and calls
   `snd_soc_jack_report()`. Publish the raw status as debugfs so T1 keeps working.
2. Only if that poll is too slow or noisy, attach the codec IRQ (GPIO 54) as an
   *additional* trigger that just kicks the same work — never do register I/O in
   the hard handler, and never trust it to be edge-accurate (O2).
3. `snd_soc_jack_new()` + `snd_soc_jack_add_pins()` with `SND_JACK_HEADPHONE`,
   `SND_JACK_MICROPHONE`, `SND_JACK_BTN_0`, so `jack/headphone` udev events fire
   and PipeWire/ACP can switch the profile.
4. `cs42l81_set_output(cs, hp)`: implement §3.2, called from a DAPM route
   switch (`"Headphone"` / `"Speaker"`), never from the poll worker directly —
   the worker only updates the jack state and a control.
5. Optional: read GPIO 66/68 as inputs (`gpiod_get_optional`) and log their
   level with each jack event (T1b) — this either confirms they are inputs or
   proves them to be outputs, and gives a second, register-free detect source.

### 5.5 Tools

* `tools/audio/a9audio.c`: add `rec <file> <seconds>` (S16_LE/S32_LE, 44.1/48 k,
  1–2 ch) so capture works in the HoolockRD shell without alsa-utils.
* `tools/audio/audio-test.sh`: add `rec`, `jack`, `regs` subcommands.
* `scripts/audio/omarchy-alsa-test.sh`: add a `mic` mode
  (`arecord | aplay` loopback + level report).

---

## 6. Device test plan

All steps are read-only or bounded; nothing touches NVMe/NVRAM/syscfg.
`sound/soc` keeps the codec powered for `pmdown_time` after a stream, so
register reads right after a stream are valid.

### T1 — jack status survey (no code change, do this first)

```sh
# nothing plugged
cat /sys/kernel/debug/cs42l81-regs > /tmp/regs-empty.txt

# plug headphones, wait 2 s, then
cat /sys/kernel/debug/cs42l81-regs > /tmp/regs-hp.txt

# plug a headset (4-pole) and repeat
cat /sys/kernel/debug/cs42l81-regs > /tmp/regs-hs.txt

diff /tmp/regs-empty.txt /tmp/regs-hp.txt
diff /tmp/regs-empty.txt /tmp/regs-hs.txt
```

Expected: at least one bit in `002e/002f/0033/007b/007c/0106/0528` flips.
That bit is the plug detect (closes **O2**). If nothing flips, repeat with the
stream running (`audio-test.sh play` in another shell) so the core is powered.

### T1b — are ext_sw1/2 inputs or outputs?

```sh
for p in 64 66 68; do printf 'gpio %d: ' $p; \
  a9audio peek $((0x20f100000 + 4*p)) 32; done
# with and without a plug inserted
```

If 66/68 change with the plug, they are detect inputs (drive the jack kcontrol
from them). If they are static, they are switch outputs (drive them from the
output-routing code).

### T2 — capture bring-up

```sh
# Omarchy (alsa-utils present)
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 3 /tmp/mic.wav
aplay -D hw:0,0 /tmp/mic.wav          # or copy the file off the device
arecord -D hw:0,0 -f S16_LE -r 44100 -c 2 -d 3 /tmp/mic44.wav

# HoolockRD shell (no alsa-utils)
a9audio rec /tmp/mic.raw 3
```

Pass criteria: non-zero RMS, no DC-only signal, `xruns 0`, and the rate check
from `omarchy-alsa-test.sh check` style measurement (compare the measured
frames/second to the nominal rate).

Failure signatures:

| Symptom | Likely cause | Look at |
|---|---|---|
| `arecord` starts, buffer stays silent | ADC/DMIC not enabled | `0x000d`, `0x0215/0x0216`, `0x0203` via `cs42l81-reg` |
| all-zero samples but MCA RX counts up | ASP TX slots not mapped | `0x0014/0x0015`, `0x0115/0x0116`, `0x011f/0x0120` |
| constant DC / slow drift | wrong slot width or offset | try slot offset variants (T2b) |
| `stalled: no space for ... ms` | no RX completions from SmartIO | dmesg `ep 0x29` lines, `dmaengine/summary` |
| works at 44.1 k, garbage at 48 k | DMIC divider code | `0x0215/0x0216` = `0x90` vs `0x80` |

### T2b — slot/offset variants (only if T2 is silent or DC)

Set before the stream, using the existing debugfs poke mechanism:

```sh
echo "0x0014 0x00" > /sys/kernel/debug/cs42l81-reg
echo "0x0015 0x02" > /sys/kernel/debug/cs42l81-reg
echo "0x011f 0x00" > /sys/kernel/debug/cs42l81-reg
echo "0x0120 0x02" > /sys/kernel/debug/cs42l81-reg
```

then the iOS values `0x011f = 0x18`, `0x0120 = 0x1c`, and then
`0x0014 = 0x18|0x20`, `0x0015 = 0x1c|0x20` (offset 24/28 with 2-byte width).
Whichever variant gives clean audio is the real frame layout (closes **O1**).

### T2c — input-select sweep (only if T2b did not help)

`0x0203` (ADC input select) is the one register whose meaning is least certain.
With a stream running, poke it and listen/inspect RMS:

```sh
for v in c4 84 44 04 00; do
  echo "0x0203 0x$v" > /sys/kernel/debug/cs42l81-reg
  arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 1 /tmp/mic-$v.wav
done
```

Then, if still silent, sweep `0x0204` bits 4/5 (`0x10`, `0x20`, `0x30`) and
`0x000d` (`0x02`, `0x0a`, `0x2a`). Report which combination gives signal: that
closes O7 and pins the internal-mic path bit for good.

### T3 — headphone output and plug events

```sh
# 1. speaker tone, then plug headphones mid-tone
audio-test.sh play &
# 2. after plugging:
amixer -c 0 cget name='Headphone Playback Switch'   # or the jack kcontrol
cat /proc/interrupts | grep -i -e gpio -e cs42l81
dmesg | tail -20
# 3. unplug and repeat
```

Pass criteria: sound moves from the speakers to the headphones within ~250 ms
(the poll period); the jack kcontrol changes and udev emits
`jack/headphone HEADPHONE plug` / `unplug`. `/proc/interrupts` only counts up if
the codec IRQ was attached (optional, §5.4 #2).

If no status bit moves at all (T1 was flat) then the codec has no register-
visible detect and the only sources left are GPIO 66/68 (T1b) or the ADC DC
level; in that case move to the T1b-based jack and drop the IRQ idea.

### T4 — headphone quality checks

* volume sweep with the HP control: no pop at unmute (iOS inserts the
  155 µs `0x0219` pre-charge and a 60 ms wait — copy both);
* speaker muted while plugged (check `0x0225/0x0226 == 0x19`,
  `0x022e` bits 4:3 set);
* unplug while playing: no click, speaker comes back.

### T5 — headset mic (phase 2, after T1–T4)

```sh
# with a 4-pole headset plugged, after the jack code sets the hs-switch:
arecord -D hw:0,0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/hs.wav
```

Start by copying iOS's parked `ext_sw` state (both lines as the driver leaves
them at power-up), then sweep the four combinations of GPIO 66/68 while
recording, and keep the one with the largest signal. If all four are silent,
the analog mic bias is not being applied and we need the schematic or an
iPhone-class CS42L42-style "HSBIAS" register hunt (`0x0219`/`0x0221`/`0x0230`
are the only candidates left).

---

## 7. Open items

| # | Question | How to close it |
|---|---|---|
| O1 | Real ASP frame layout for capture (iOS `0x011f/0x0120 = 0x18/0x1c` does not fit the 2-slot/4-byte playback frame) | T2b |
| O2 | Which codec status bit reports the plug | T1 |
| O3 | `ext_sw1/2` direction | T1b |
| O4 | DMIC divider code for 48 k (`0x90` vs `0x80`) | T2 at both rates |
| O5 | Analog mic bias for `mic2-extmic` (no register found) | T5 + schematic |
| O6 | `0x000e` bit semantics (ADC/DAC PDN polarity) | T2 (ADC silent while `0x40` set) |
| O7 | Which `0x000d` bit is the internal-mic path (bit 1 vs 1+3) | T2c |

## 8. Risks

* **No capture until the ADC power bits are right.** `0x000d` has a 20 ms
  settle in iOS; a wrong bit order gives silence with no error.
* **`0x9901` unlock must not be written while a stream runs** — power-cycling
  the core kills both directions (same rule as playback).
* **Jack events are a poll, not an interrupt.** iOS builds its detect event
  source with a null provider and debounces on a timer [V]; plan for a 250 ms
  poll and treat the codec IRQ as a bonus. Do not spin faster (the MCA IRQ
  storm of round 4 is the cautionary tale).
* **Headset mic may be blocked on hardware knowledge** (external switch +
  bias). Plan for it to be the last milestone, and do not let it block the
  built-in mics or headphone output.

---

## 9. Round 6: what the driver writes, and corrections to sections 2 and 3

Round 6 (branch `a9/audio`, `patches/audio/capture-jack/`) re-read
`updateRouting` (`0xfffffff005e6dda0`, dump in
`build/audio/r6/updateRouting.c`), `setHeadphoneVolume`,
`setSpeakerVolume`, `AppleAE2MCA_getFIFODepth`/`_setIISConfig_v4` and the
CS42L81 start code (`0xfffffff005e6d3c0..d560`) before writing the
driver. Several rows of sections 2.2 and 3.2 were wrong. The driver
follows the corrected reading.

### 9.1 Input route table (closes O7)

The start code loops over the name table at `0xfffffff006e08320` = seven
pointers to `mic1, mic3, mic4, dmic1a, dmic1b, dmic2a, dmic2b` (cstrings
`0xfffffff005479218..0xfffffff00547923c`, dumped) [V]:

* `this+0x1f0 + 4k` = 4CC of `"<name>-intmic"` (an empty property gives
  `"cimi"`). On J71t only k = 3 (`dmic1a` = `"cimi"`) and k = 4 (`dmic1b` =
  `"cims"`) are set [V ADT].
* For k ≤ 2 only, `this+0x20c + 4k` = `"<name>-bias"` if present, else the
  `-intmic` value. These three are the "input devices" that set
  `0x000d` bits 1/3/5. J71t has no `mic1/3/4` properties, so **iOS never
  sets any `0x000d` bit for the DMICs** [V]. `0x000d` is the analog-mic
  bias power (mic1/mic3/mic4), not the DMIC path. The driver leaves it at 0
  (core power-up already clears bits 5:0, as before).

`updateRouting`'s `switch` index is that table index: 1 mic3, 2 mic4,
3 dmic1a, 4 dmic1b, 5 dmic2a, 6 dmic2b; `"cime"` (`mic2-extmic`) and
`linL/linR` are separate branches [V].

### 9.2 Capture sequence (replaces the 2.2 table)

For the routes dmic1a (ADC channel 0) + dmic1b (channel 1), in iOS order
[V code; register meanings I]:

| Reg | Written | Study said | Note |
|---|---|---|---|
| `0x0014..0x001d` | `n·bytes`, `| 0x80` for n ≥ 2 | `0x00, 0x02` | **no width bits** in TX slots (RX slots have them) |
| `0x0124..0x0128` | `rate | rate<<4` | same | one per slot pair |
| `0x0115, 0x0116` | `0x18, 0x1c` | same | slot source = ADC A/B |
| `0x001e..0x0021` | `bswap32(byte mask)`: `00 00 00 0f` (16-bit), `00 00 00 ff` (32-bit) | **missing** | 4-byte burst, vtable+0xac0: header `6C page 0x80|reg len-1` (bit 7 = auto-increment); the driver writes four single bytes [I equivalent] |
| `0x0205` | bits 7:4 **cleared** (`0x30` then `0xc0`) | `|= 0x10` | `0x10/0x40` belong to `"cime"` (headset mic), `0x20/0x30/0x80/0xc0` to mic3/mic4; reset value is `0x8c`, so capture leaves `0x0c` |
| `0x011f, 0x0120` | `0x18, 0x1c` | "propose 0x00/0x02" | same code space as the slot sources: ADC channel source, not a byte offset. O1 is about these only if T2 fails |
| `0x0204` | `update_bits(0x73, boost)` | `0x20` | `0x10/0x20` are the `"cime"` headset-mic bits |
| `0x0206, 0x0207` | `0` | volume | analog PGA, unused for DMICs |
| `0x0208, 0x0212` | `gain − 0x60` (or `− 0x14 − 0x60` with boost); `0x80` muted | "mic boost" | DMIC digital volume; control `0x170`, `0x60` = 0 dB |
| `0x0210` | `update_bits(0x43, boost)` | — | boost = 3 when `gain ≥ 0x6c` (+12 dB) |
| `0x000d` | unchanged (0 on J71t) | `|= 0x02`, 20 ms | see 9.1 |
| `0x0203` | `0xc0` | `0xc4` | cases 3/4 do `& 0xfb ^ 0x40/0x80`; `0xc4` is the no-route value. Bit 2 is the capture gate the study ruled out |
| `0x020f` | `0x04` | same | `0` only for dmic2a/2b |
| `0x0011` | clear **bit 6** | `|= 0x80` | `~swap2(active) << 6`: bit 7 = ASP RX (playback) PDN, bit 6 = ASP TX PDN |
| `0x0131` | bit 0 set | same | |
| `0x0215` | `(ctl 0x188 ? 0x80 : 0) | div` | same | routes 0..4 use `0x0215`, only dmic2a/2b use `0x0216` |
| `0x000e` | clear `0x60` | "clear 0xa0, leave 6" | bit 7 DAC, bit 6 ADC, bit 5 shared PDN; input inactive sets bit 6 again |

The control getter (vtable+0x710, `0xfffffff005da603c`) returns its second
argument when the control does not exist [V], so `0x188` defaults to 1:
`0x0215` = `0x80 | div` (driver default, module parameter `dmic_bit7`).
`div` = `0x70` (8–24 kHz), `0x40` (32 kHz), `0x10` (44.1 kHz), `0` (48 kHz).

Capture off (input inactive): `0x0203 = 0xc4`, `0x020f = 0x04`, `0x0011`
bit 6 set, `0x000e` bit 6 set. iOS does not touch `0x0215` then; neither
does the driver.

### 9.3 Headphone volume (corrects 3.2)

`setHeadphoneVolume` without a line out (J71t, `this+0x1d5 = 0`) [V]:
`g` = volume − 324 in 0.5 dB; gain `a = trunc(g/2)` (capped +12 dB, floor
−50 dB with extra steps moved into the trim), trim `r = g − 2a`;
`0x0225 = r` (digital, 0.5 dB, `0x00` = 0 dB), `0x0227 = a & 0x7f`
(`0x40` = mute). Until 60 ms after the HP driver came up (`this+0x1d1`)
and whenever `a ≤ −9`, the gain is clamped to −9 dB and the charge pump
stays off; otherwise the pump comes on once: `0xc96f = 0x0e`,
`0x0219 = 1`, 155 µs, `0xc96f = 0x1e`. iOS writes neither `0x0228` nor
`0x0232` here (`setSpeakerVolume` returns early when both speakers are
powered down). The study's "`0x0227/0x0228 = vol`, `0x0232 = vol + 0x34`"
is the speaker+headphone case of `setSpeakerVolume`, which the driver
does not support. The driver also writes the trim to `0x0226` so both
channels match [I, harmless if `0x0226` is speaker-only].

`0x0220` is only rewritten when "any output" toggles, so iOS switches
speaker↔headphone through a state with no output. DAPM does the same:
power-down events run before power-up events in one sync.

### 9.4 RX DMA limit

`_setIISConfig_v4` requests the RX SmartIO depth with the same formula as
TX: `(bits ≤ 16 ? 2 : 4) · round4(rate · channels / 1000)` [V]. After
validFIFODepth's `min(req, trigger)` that is 192 bytes at 48 kHz and 176
at 44.1 kHz, not `0x180`; `0x180` is only the cap. RX_FIFO_CFG
`0x00030004` (depth 4) matches `getFIFODepth` with `this+300 != 0` [V].

### 9.5 What is still open

| # | Status after round 6 |
|---|---|
| O1 | likely moot: `0x011f/0x0120` are sources (9.2). T2 confirms |
| O2 | open: T1 with the driver's survey poll |
| O3 | open: T1b, now also visible in `/sys/kernel/debug/gpio` (`hs-switch`) |
| O4 | iOS answer is divider 0 at 48 kHz (9.2); T2 checks both rates |
| O5 | open (headset mic, not implemented) |
| O6 | closed by code: bit 7 DAC, 6 ADC, 5 shared |
| O7 | closed by code: `0x000d` is analog-mic bias, unused on J71t (9.1) |

---

## Appendix A — Ghidra names used here

The shared analysis (`scripts/ghidra-mcp.sh`) now carries these names; the
pre-existing `AppleCS42L81Audio_*` names are from round 1.

| Address | Name |
|---|---|
| `0xfffffff005e6da58` | `AppleCS42L81Audio_enableAudio` |
| `0xfffffff005e6dda0` | `AppleCS42L81Audio_updateRouting` |
| `0xfffffff005e6f50c` | `AppleCS42L81Audio_disableAudio` |
| `0xfffffff005e6f9f4` | `AppleCS42L81Audio_setSampleRate` |
| `0xfffffff005e70798` | `AppleCS42L81Audio_setHeadphoneVolume` |
| `0xfffffff005e70e40` | `AppleCS42L81Mikey_start` |
| `0xfffffff005dc2f4c` | `CSEmbeddedAudio_startHeadsetDetect` |
| `0xfffffff005dc3308` | `CSEmbeddedAudio_setHeadsetDetectEnable` |
| `0xfffffff005dc3384` | `CSEmbeddedAudio_getHeadsetPlugState` |
| `0xfffffff005dc3420` | `CSEmbeddedAudio_headsetDetectAction` |
| `0xfffffff005dc4228` | `CSEmbeddedAudio_headsetSwitchStart` |
| `0xfffffff005dc461c` | `CSEmbeddedAudio_continueMicDetect` |
| `0xfffffff005dc5e30` | `CSEmbeddedAudio_getHeadsetType` |
| `0xfffffff005dc666c` | `CSEmbeddedAudio_initHeadsetSwitchBase` |
| `0xfffffff005dc8138` | `CSEmbeddedAudio_readExtSwitchConfig` |
| `0xfffffff005dc8228` | `CSEmbeddedAudio_setExtSwitches` |

Offline dumps used for this doc (gitignored):
`build/audio/decomp/cs42l81.c`, `build/audio/decomp/ds/csembedded.c`,
`build/audio/decomp/ds/mikeybus.c`, helper `build/audio/ghrange.py`.
