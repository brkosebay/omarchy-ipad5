# Audio device test plan (J71t speakers, mics, headphones)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Run by the orchestrator. Every step is read-only or bounded, and no step
touches NVMe, NVRAM or syscfg. Background: `playback-path.md` (iOS path,
SmartIO protocol, round-3 analysis in section 8), `ds-capture-and-jack.md`
(mics and jack; section 9 = what round 6 writes).

## Round 6: built-in mics, headphones, jack (kernel `a9/audio` 5133d982ce, `patches/audio/capture-jack/`)

New in the kernel (details: `ds-capture-and-jack.md` section 9):

- **Capture:** `pcmC0D0c`, 2 channels (left = DMIC1A, right = DMIC1B),
  S16_LE/S32_LE, 8–48 kHz, same rate and format as a running playback
  (the DAI is symmetric). MCA0 RX through SmartIO endpoint 0x29. Mixer:
  `Capture Volume` (0..128, 1 dB steps, 96 = 0 dB, default 0 dB; units
  inferred) and `Capture Switch`.
- **Headphones:** `Headphone Playback Volume` (0..124, 0.5 dB steps,
  100 = 0 dB, **default 40 = −30 dB**) and `Headphone Playback Switch`.
  Playback goes to the headphones only while the `Headphone Jack` reports
  a plug; then the speakers are off. Card pin switches `Speakers Switch`
  and `Headphones Switch` can also mute a path.
- **Jack:** card control `Headphone Jack` (iface CARD). Detection is a
  poll of the codec's latched status registers, **off by default**, because
  the plug bit is not known yet (T1). Module parameters (all writable at
  runtime, `/sys/module/snd_soc_cs42l81/parameters/`): `jack_poll_ms`
  (0 = off, min 100), `jack_reg`, `jack_mask`, `jack_invert`, `dmic_bit7`
  (0x0215 bit 7, default 1). debugfs `cs42l81-jack`: read = state, the
  OR of every status value the poll saw since the last read, and the
  hs-switch GPIO levels; write `1`/`0` = force a plug/unplug report,
  `auto` = back to the poll, `poll` = restart the poll.
- GPIO 66/68 (ext_sw1/2) are requested **as is** and only read; nothing
  drives them. GPIO 54 (codec IRQ) is unused.
- debugfs `s8000-mca-regs` now prints `capture yes`, the prepared/running
  directions, and `last playback stream:` / `last capture stream:` lines
  (was `last stream:`).

### 0. Build and boot (warm kexec into Omarchy)

    # a9/audio is in integrate.sh's default branch list
    scripts/integrate.sh 2>&1 | tee build/audio/integ-r6.log
    # if another workstream is using integ:
    #   INTEGRATION_TREE=integ-audio scripts/integrate.sh  (output build/kernel-integ-audio)

    WAIT_SECONDS=240 scripts/boot/kexec-host.sh \
      build/kernel-integ/Image.gz build/kernel-integ/dtbs/s8003-j71t.dtb \
      "$(cat build/boot/integ-entry/cmdline)"

    scp scripts/audio/omarchy-alsa-test.sh linux@172.16.42.1:/tmp/
    ssh linux@172.16.42.1 'dmesg | grep -iE "cs42l81|s8000-mca|sio-dma"; cat /proc/asound/cards; ls /dev/snd'

Expect `s8000-mca 20a0a0000.mca: MCA version 4, FIFO tx 64 rx 64, SmartIO
DMA, capture`, `cs42l81 spi0.0: CS42L81 ID 42a810 rev 0xa1`, card
`iPad5 Speakers`, and **both** `pcmC0D0p` and `pcmC0D0c`. No
`hs-switch GPIOs unavailable` warning (if it appears, the jack still
works; T1b falls back to `a9audio peek` in HoolockRD).

> Private graphics-dependent excerpt omitted from this source snapshot.

Stop PipeWire's hold on the card for the direct-hw steps, as in round 5
(the script says when the PCM is held). Run every step as
`ssh linux@172.16.42.1 'sudo bash /tmp/omarchy-alsa-test.sh MODE' 2>&1 | tee docs/test-logs/r6-MODE.txt`.

**Regression first:** `check`, then `tone`. Speakers must sound as in
round 5, every step-7 line `OK`. `s8000-mca-regs` after `check` shows
`last playback stream:` and `irq_mask 0x1fff` at idle.

### T1: plug survey (closes O2), owner plugs and unplugs

Snapshots (each one reads, and so clears, the latched status registers
once):

    sudo bash /tmp/omarchy-alsa-test.sh jack snap empty
    #   owner: plug wired headphones (3-pole), wait 2 s
    sudo bash /tmp/omarchy-alsa-test.sh jack snap hp
    #   owner: unplug, plug a 4-pole headset (with mic), wait 2 s
    sudo bash /tmp/omarchy-alsa-test.sh jack snap hs
    #   owner: unplug, wait 2 s
    sudo bash /tmp/omarchy-alsa-test.sh jack snap empty2
    sudo bash /tmp/omarchy-alsa-test.sh jack diff empty hp
    sudo bash /tmp/omarchy-alsa-test.sh jack diff hp empty2
    sudo bash /tmp/omarchy-alsa-test.sh jack diff empty hs

Then the poll, which also catches bits that latch and clear between
snapshots (every change is logged with the hs-switch levels):

    sudo bash /tmp/omarchy-alsa-test.sh jack poll 250
    sudo bash /tmp/omarchy-alsa-test.sh jack watch 30
    #   owner, during the 30 s: plug headphones at ~5 s, unplug at ~15 s,
    #   plug the headset at ~20 s, unplug at ~25 s

Expected lines: `cs42l81 spi0.0: status 002f: 00 -> 08 ext_sw1 in=0 ext_sw2 in=1`
(register, old and new value). **Ignore bits that follow the codec's
power state, not the plug:** every codec power-up latches `002e = 03` and
`002f = 80` (seen in integ10: `powered up: ... status 2e=0x3 2f=0x80`),
and `002f` bit 7 is PDN_DONE at every power-down (5 s after the last
stream). They appear as `002e: 00 -> 03` / `002f: 00 -> 80` around stream
start and stop. A plug bit changes when the owner plugs, with no stream
starting or stopping. The idle codec is powered down, so repeat the poll
test with the codec powered, a silent stream running:

    sudo sh -c 'head -c 7680000 /dev/zero | aplay -q -D hw:0,0 -t raw -f S16_LE -r 48000 -c 2 &'
    sudo bash /tmp/omarchy-alsa-test.sh jack watch 30      # same plug sequence

Reading the result:

| Seen | Meaning | Next |
|---|---|---|
| a bit set in `hp`/`hs`, clear in `empty`/`empty2`, and one log line per plug and unplug | level bit: plug state | set it (below), then T3 |
| a bit appears once per plug **and** once per unplug, never stays set | latched edge bit | report reg/bit; detection needs a level source (next row) |
| nothing changes, idle or streaming | no register-visible detect | T1b decides: ext_sw inputs, else the codec IRQ (GPIO 54) |

Turn detection on with the bit found (example values):

    echo 0x2f > /sys/module/snd_soc_cs42l81/parameters/jack_reg
    echo 0x08 > /sys/module/snd_soc_cs42l81/parameters/jack_mask
    echo 0    > /sys/module/snd_soc_cs42l81/parameters/jack_invert   # 1 if set while unplugged
    sudo bash /tmp/omarchy-alsa-test.sh jack watch 20      # expect "headphones plugged/unplugged"
    amixer -c 0 cget iface=CARD,name='Headphone Jack'      # values=on while plugged

Two equal polls are needed before a report, so with 250 ms a plug shows
within 250–500 ms. Report the values; they become DT/driver defaults.

### T1b: ext_sw1/2 direction (closes O3)

Read from the same snapshots: the `gpio-66`/`gpio-68` lines of
`/sys/kernel/debug/gpio` (`in`/`out`, `hi`/`lo`) and the `hs-switch:`
line of `cs42l81-jack`. Raw pads from HoolockRD instead:
`audio-test.sh jack snap NAME` reads GPIO 54/64/66/68 with
`a9audio peek` (mode bits 3:1, input enable bit 9, level bit 0).

| Seen | Meaning |
|---|---|
| `in`, level follows the plug | detect inputs: they can drive the jack (tell the audio agent) |
| `out`, level constant | outputs left by iBoot: headset switch control, needed for the headset mic (T5) |
| `in`, constant | unused or pulled; no conclusion |

Nothing in this round writes these pins.

### T2: capture from the built-in mics

    sudo bash /tmp/omarchy-alsa-test.sh mic 2>&1 | tee docs/test-logs/r6-mic.txt
    #   owner: talk or tap near the mics during each 3 s take
    scp linux@172.16.42.1:/tmp/mic-S16_LE-48000.wav build/audio/   # listen on the Mac

Pass: three takes with `arecord rc 0`, about 3 s each, no `SILENT` and no
`DC only`, RMS clearly higher while talking than in a quiet take, both
channels alive (tap next to each mic to tell DMIC1A from DMIC1B; report
which mic is left). In the "during capture" dump: `capture yes`,
`running 0x2`, codec `0203 = c0`, `0205 = 0c`, `0011 = 00` (bit 6 = ASP
TX now on; bit 7 is clear too, because core power-up keeps the playback
path on), `000e = 00`, `0215 = 80` at 48 kHz (`90` at 44.1 kHz),
`0014 = 00`, `0015 = 02`, `0021 = 0f` (`ff` for S32), `0115/0116 = 18/1c`.

Same from HoolockRD (no alsa-utils): `audio-test.sh rec 3` (a9audio
prints per-channel RMS/peak/DC and the measured rate).

Failure signatures:

| Symptom | Likely cause | Next |
|---|---|---|
| `stalled: no data` / arecord timeout | no SmartIO RX completions | dmesg `ep 0x29`, `dmaengine/summary`, `s8000-mca-regs` (`running 0x2`, RX_CFG `mca+0x80 = 0x133`) |
| data flows, all channels `SILENT` | ADC or ASP TX not feeding | T2b: `0x0203`, `0x0215` bit 7, `0x0021` |
| `DC only` or slow drift | slot/width mismatch | T2b: `0x0021`, `0x011f/0x0120` |
| fine at 44.1 kHz, noise at 48 kHz | DMIC divider (O4) | T2b on `0x0215` at 48 kHz: `80 90 c0` |
| one channel silent | ADC B source | T2b: `0x0116`, `0x0120` |
| very quiet | gain | `CAPVOL=116 sudo -E bash /tmp/omarchy-alsa-test.sh mic` (+20 dB) |

### T2b/T2c: register variants in one capture

`mic-sweep REG V1 V2 ...` records once and pokes REG to each value every
2 s; window 0 is the driver's value. Talk continuously while it runs:

    sudo bash /tmp/omarchy-alsa-test.sh mic-sweep 0x0203 0xc4 0x80 0x40 0x00
    sudo bash /tmp/omarchy-alsa-test.sh mic-sweep 0x0215 0x00 0x90 0xc0 0x10
    sudo bash /tmp/omarchy-alsa-test.sh mic-sweep 0x0021 0xff 0xf0
    sudo bash /tmp/omarchy-alsa-test.sh mic-sweep 0x011f 0x00 0x1c
    sudo bash /tmp/omarchy-alsa-test.sh mic-sweep 0x0205 0x8c 0x1c

Only if every window of T2 was silent:
`mic-sweep 0x000d 0x02 0x0a 0x2a` (analog-mic bias; iOS never sets it on
J71t, so a change here contradicts the code reading) and
`echo 0 > /sys/module/snd_soc_cs42l81/parameters/dmic_bit7` before `mic`.
Report the window that has signal.

### T3: headphone output and plug events

Before T1 has a bit, force the report (owner wears the headphones; the
level starts at −30 dB):

    sudo bash /tmp/omarchy-alsa-test.sh hp 2>&1 | tee docs/test-logs/r6-hp.txt

Pass: the tone is in the headphones and the speakers stay silent, then
the same tone on the speakers and silence in the headphones. Registers
during the headphone tone: `0220 = 08` (HP driver on, line off),
`022e = 18` (speakers off), `0227 = 62` (−30 dB, 7-bit two's
complement), `0225 = 0226 = 00` (no 0.5 dB trim), `0219 = 00` (the
charge pump stays off at or below −9 dB). During the speaker tone:
`0220 = 28`, `0227 = 40`. dmesg: `headphones plugged`,
`headphones unplugged`.

With detection on (T1 done): play a long tone and plug/unplug by hand:

    sudo sh -c 'speaker-test -D hw:0,0 -c 2 -r 48000 -F S16_LE -t sine -f 440 -l 3 >/dev/null &'
    #   owner: plug headphones after ~3 s, unplug after ~8 s
    dmesg | grep -iE 'headphones|cs42l81' | tail
    amixer -c 0 cget iface=CARD,name='Headphone Jack'

Pass: sound moves to the headphones within ~0.5 s of the plug and back to
the speakers after the unplug, without a stall of the stream (`speaker-test`
keeps running). `udevadm monitor --kernel --subsystem-match=input` shows
the jack input device events; with PipeWire installed, `wpctl status`
switches the sink port.

### T4: headphone quality

With a forced plug (`echo 1 > /sys/kernel/debug/cs42l81-jack`) and a tone
running (`audio-test.sh`-style `aplay` of a 1 kHz file, or `speaker-test`):

    for v in 40 60 80 84 90 100; do
      amixer -c 0 -q cset name='Headphone Playback Volume' $v; sleep 1
      grep -E '^(0219|0225|0227|c96f) ' /sys/kernel/debug/cs42l81-regs | tr '\n' ' '; echo " vol $v"
    done

Expect `0219 = 01` and `c96f = 1e` from 83 (−8.5 dB) on, `0227` stepping
by 1 per 2 control steps, `0225` = `00` or `ff` (the 0.5 dB trim). The
pump stays on until the headphones power down. Listen for a pop when the
pump comes on (84 in the loop) and when the plug report changes;
iOS has the same 155 µs/60 ms sequence. Check balance with
`sudo bash /tmp/omarchy-alsa-test.sh lr` while forced plugged (the
script sets the speaker volume, which does not matter for headphones).
Unforce with `echo auto > /sys/kernel/debug/cs42l81-jack`.

### T5: headset mic (not implemented)

Nothing in round 6 routes `mic2-extmic`. After T1/T1b, record whether the
headset changes any status bit differently from plain headphones (the
`hs` snapshot) and which way GPIO 66/68 point; that decides the next round
(ADC path `"cime"`: `0x0204` bits 4/5, `0x0205` `0x10/0x40`, plus the
external switch).

### Round 6 safety

- Capture and headphones only touch codec registers over SPI and MCA0
  while a stream holds it. No new MMIO from userspace except the existing
  read-only `a9audio peek` of GPIO pads.
- Headphone level starts at −30 dB; the charge pump only runs above −9 dB.
- The poll is off unless `jack_poll_ms` is set; it reads 7 registers per
  period and takes the same lock as the codec's PDN_DONE wait.
- All waits are bounded: MCA RX reset 10 ms (prepare) / 1 ms (stop),
  arecord under `timeout`, a9audio poll timeouts.

## Round 5: Omarchy (kernel `a9/audio` 1a1a5dc2f5)

**Audio works** (integ 26de914e61 = `fbe859a481`, owner heard it):
`speaker-test -D hw:0,0 -c 2 -r 48000 -F S16_LE -t sine -f 1000 -l 2`
returned rc 0, and the codec came up with `clk_ctl 0x8c`, so the round-3
CLK_CTL fix is confirmed. No IRQ storm.

What the run shows (verified by arithmetic from the reported numbers):

- **The clock is exactly 48 kHz.** speaker-test writes `rate*3/period` = 17
  periods of 8192 frames per channel, so one 2-channel loop is 278528
  frames = **5.802667 s** at 48 kHz. Measured: 5.802668 s. The first loop
  is shorter by the 32768-frame buffer it fills before the clock starts:
  5.802667 − 0.682667 = **5.120 s**. Measured: 5.1186 s. No rate or clock
  mismatch, so pitch should be right.
- **The single error IRQ** (`status 0x80000005` = TX underflow + TX
  low-water, bit 31 = cause pending) is most likely one underflow at stream
  start (inferred). Trigger start enables MCA TX while SmartIO only starts
  moving data once its configure/submit commands reach the firmware.
  Continuous underflows would make SmartIO fall behind real time, and
  the loop time matches real time to about 1 µs in 5.8 s. From 1a1a5dc2f5,
  `s8000-mca-regs` prints `last stream: first error N us after start
  (status ..), status at stop ..`. A small N and no bit 0 in the status
  at stop confirms a one-off start-up underflow.

**Why PipeWire shows no devices** (verified on `build/omarchy/rootfs.pristine.img`
with read-only `debugfs`; the live image may differ): the image has no
**`pipewire-audio`**. `/usr/lib/spa-0.2/` has no `alsa/` plugin, there is
no `/usr/share/alsa-card-profile/` (ACP profile sets, package
`alsa-card-profiles`), and there is no `pw-play`/`pw-cat`. Without the SPA
ALSA plugin, WirePlumber's ALSA monitor cannot create any device, so
`wpctl status` is empty whatever the kernel does. `pipewire-alsa` and
`pipewire-pulse` (needed by pamixer and Omarchy's volume controls) are
missing too. This is for the Omarchy workstream:
`pacman -S pipewire-audio pipewire-alsa pipewire-pulse`, then restart the
user's pipewire, wireplumber and pipewire-pulse.

**Kernel side, checked against what ACP needs:**

| ACP needs | This card | Status |
|---|---|---|
| a playback PCM | `pcmC0D0p` (`s8000-mca-cs42l81-asp cs42l81-asp-0`), no capture | OK |
| S16_LE / S32_LE, 2 ch, 44.1 / 48 kHz | S16_LE, S32_LE; 2 ch only; 8–48 kHz | OK (mono and multichannel mappings fail as expected; `analog-stereo` / `hw:%f` fallback fits) |
| period/buffer sizes ACP and PipeWire ask for | 512 B–32 KB periods, 2–16 periods, ≤128 KB buffer; `near` requests always resolve (ACP 25 ms: 1200/4800; tsched: 8192/32768; PipeWire, period first: 1024/16384. The probe's `pw-1024` line sets the buffer first, so it shows 2048/32768) | OK (inferred from the limits; `check` verifies) |
| mmap | `MMAP`/`MMAP_VALID`; the buffer is `dma_alloc_coherent` on the SmartIO DMA device, mapped with `dma_mmap_coherent` | **untested until now**: a9audio and speaker-test use RW. `check` plays through mmap |
| BATCH flag for period-granular pointers | was missing: the custom `pcm_hardware` bypasses the dmaengine core's BATCH logic | **fixed in 1a1a5dc2f5**: PipeWire then keeps a period of headroom |
| mixer element for hardware volume | `Speaker` (playback volume with a dB TLV, 0.5 dB steps, max −3 dB, plus a switch) matches ACP's `analog-output-speaker` path | OK |
| UCM or profile set | driver `simple-card`: `conf.d/simple-card/` only has board files (Librem 5, PinePhone, …), nothing for "iPad5 Speakers", so UCM fails and ACP uses `default.conf` | OK |
| `SOUND_INITIALIZED` udev property, seat ACL | systemd `78-sound-card.rules` and `70-uaccess.rules` are in the image | `check` prints both |

### Omarchy test: `scripts/audio/omarchy-alsa-test.sh`

Pure ALSA (alsa-utils, plus python3 ctypes on libasound for an ACP-style
probe). No a9audio or devmem. Every step is under `timeout`. Run as root
over SSH:

    scp scripts/audio/omarchy-alsa-test.sh linux@172.16.42.1:/tmp/
    ssh linux@172.16.42.1 'sudo bash /tmp/omarchy-alsa-test.sh check' 2>&1 | tee docs/test-logs/omarchy-alsa-check.txt

| Mode | Audible | What |
|---|---|---|
| `check` | no (speaker switch off, restored at exit) | card/PCM/mixer, PipeWire packages + units + `wpctl status` + WirePlumber log, udev `SOUND_INITIALIZED`, ACLs, sessions, UCM, holder check, `--dump-hw-params`, ACP-style probe (S16/S32 × 48k/44.1k × mmap/RW × ACP/PipeWire sizes, prints `batch`), 6 silent 4 s streams with the **sample rate measured from hw_ptr**, debugfs during and after, new dmesg lines |
| `tone` | yes | 2 s 1 kHz −12 dBFS with `aplay -D hw:0,0`, Speaker volume `VOL` (default 164 = −20 dB) |
| `lr` | yes | left-only 440 Hz, then right-only 660 Hz: which speaker is which |
| `pw` | yes | the same tone through PipeWire (`pw-play` as `linux`). Says what to install if `pw-play` is missing |
| `all` | yes | check, tone, lr |

Expected `check` results:

- With 1a1a5dc2f5: `batch 1` in step 6 (`batch 0` means the old kernel).
- Every step-7 line `OK` within ±2 % of 48000/44100 Hz.
- Step 3 shows the missing `pipewire-audio` files until it is installed.

If step 4 says the PCM is held, the hw steps are skipped; use `pw`.

Reading a step-7 failure:

- **44.1 kHz lines measure about 48000 Hz** while the 48 kHz lines are right:
  the codec's 44.1 kHz rate code is wrong (with codec as master, the codec
  sets the clock).
- **mmap lines fail while RW lines pass:** the mmap/coherency path of the
  SmartIO buffer, not the clock.
- **Everything fails with `rc 124`:** a stall (no SmartIO completions). Send
  the "during the stream" debugfs block and the new dmesg lines.
- **`batch 0` in step 6** only means the kernel predates 1a1a5dc2f5. The
  `last stream:` debugfs line is also new in 1a1a5dc2f5.

Once `pipewire-audio` is installed (it pulls in `alsa-card-profiles`),
ACP tries `front:0` first. alsa-lib logs an "Unknown PCM cards.…front"
style error because there is no `cards/simple-card.conf`. That is
expected: the sink then comes up through the `hw:0` mapping.

## Round 4: next boot (kernel `a9/audio` fbe859a481 via integration)

**What locked up in integ10.** `audio-test.sh probe` hard-locked CPU0 in
`cat` → `mca_regs_show` (`docs/test-logs/integ10/audio-probe-hardlockup.png`).
The photo shows an **interrupt storm**, not a hung bus read (verified from the
photo plus disassembly of the same source):

- The return address `mca_regs_show+0x6c` is the `seq_printf` in the MCA
  register loop. The dump had already done `pm_runtime_resume_and_get` (at
  `+0x38`), so MCA0 was powered when its registers were read.
- The PC is `format_decode+0x84` = `and x13, x12, #0xff`, a register-only
  instruction. PC and x0–x29 are identical in the 372/377/384 s backtraces,
  and pstate has `daif` clear.
- CPU0 answered the backtrace IPI three times. A CPU stuck on a bus read
  cannot take an IPI. So CPU0 kept being re-interrupted before it could
  retire one instruction, and its timer never ran: the watchdog fired and
  RCU reported "0 ticks".

**Cause.** MCA `IRQ_STATUS` bit 31 reads as set while a cause is pending
(**verified**: round-2 log `last status 0x80000008`; the iOS handler tests
`status & 0x1fff` and logs "spurious intr" otherwise). Our handler used
`status & ~irq_mask`, and `irq_mask` only covers bits 12:0. So bit 31 alone
counted as handled: `IRQ_HANDLED` forever, and the kernel's spurious-IRQ
guard never trips. The trigger (**inferred**) is that the debugfs read
powered MCA0 up from a real power-off at idle. The block came up at reset,
with the hardware IRQ mask open and no BCLK/FSYNC, so no-BCLK/no-FSYNC were
pending. In round 2, `prepare` had programmed the hardware mask first.

**Fixes in fbe859a481** (the round-3 CLK_CTL 0x80 fix is unchanged and still
untested):

| Where | Change |
|---|---|
| MCA IRQ handler | only bits 12:0 count. A spurious entry re-syncs the hardware mask and returns `IRQ_NONE`, so a leftover storm is bounded: the kernel disables the MCA IRQ after 100 k unhandled (`irq N: nobody cared`, `Disabling IRQ #N`; N is the Linux number of `20a0a0000.mca` in `/proc/interrupts`, not AIC 201) |
| MCA runtime PM | resume masks and clears every cause before the MCA IRQ is enabled; suspend disables it (synchronously) before power-off, so the handler never runs on a gated block; `hw_free` re-masks everything |
| debugfs `s8000-mca-regs` | never powers anything up and never reads a suspended block: it prints `runtime-suspended: registers not read` plus the software state (`irqs/errors/spurious`, `irq_mask`, last MCLK word, route). The MCLK word and i2s-switch are read back only while the driver has them programmed (clock on or forced, route applied) |
| debugfs `s8000-mca-mclk` | read: cached word, no MMIO. Write: resumes MCA0 first (runtime resume masks the IRQ), then writes |
| module params `mclk_ctl`, `route` | no MMIO when written; used at the next prepare/clock enable (device active) |
| `audio-test.sh` | `probe` does no MMIO at all. MCA0 `devmem` only with `power/control = on` and `runtime_status = active`. MCLK word / i2s-switch `devmem` only while a stream runs. The NCO peek and the idle i2s-switch writes are removed |
| `a9audio pads/peek` | pads: AP GPIO block only (no base argument). peek: refuses anything outside the GPIO pads and PMGR ps registers (`0x20e080000..0x20e088020`) |

CS42L81 debugfs (`cs42l81-regs`, `cs42l81-reg`) goes over SPI, so no change.
SmartIO has no debugfs. Its driver holds its power domain for as long as it
is bound (verified in `apple-sio-core.c`).

### Run order

Build as in "Build" below, then on the device:

    export PATH=/usr/local/bin:$PATH
    audio-test.sh probe 2>&1 | tee /tmp/r4-probe.txt
    audio-test.sh play  2>&1 | tee /tmp/r4-play.txt
    audio-test.sh wav   2>&1 | tee /tmp/r4-wav.txt
    audio-test.sh diag  2>&1 | tee /tmp/r4-diag.txt

`audio-test.sh` with no argument now only prints usage.

| Step | Expect |
|---|---|
| `probe` (no MMIO) | the round-2 probe lines (SmartIO ready, 36 endpoints, `MCA version 4`, `CS42L81 ID 42a810 rev 0xa1`, card `iPad5 Speakers`). Then `mca device: /sys/.../20a0a0000.mca, runtime suspended`, `--- regs: idle (mca suspended)` and `runtime-suspended: registers not read`. No `nobody cared` line |
| `play` | **this is the first MCA0 power-up at idle** (`power/control = on`), the transition that locked up. dmesg at stream start: `cs42l81 spi0.0: powered up: rev 0xa1 clk_ctl 0x8c status 2e=.. 2f=.. 33=..` (`clk_ctl` bit 7 set, low nibble 0xc). The regs line shows `irqs/errors/spurious`: **`spurious` should be 0, report it if not** (resume masks every cause before the IRQ is enabled, so a spurious entry means something outside the 13 cause bits raises the line). MCA status bits 7 (0x80) and 9 (0x200) after the clear mean BCLK/FSYNC from the codec. **Ears: 1 kHz tone from both speakers.** No `timed out waiting for codec shutdown` |
| `wav` | see Step 3 below. Run it even if `play` was silent: no MMIO, harmless |
| `diag` | A: PMGR ps words. B/C: pads idle and with MCLK forced. D: `cs42l81-regs` (`000f = 8c`, `0012 = 8c`), pads during the stream (MCLK, BCLK, LRCK pads `TOGGLING`), MCA status, and the switch reg15 sweep, now only during the stream |

**If it hangs again**, photograph the screen and note:

- whether the backtraces show the **same PC** each time, and whether CPU0
  answers at all;
- the step that hung.

A storm now ends in `irq N: nobody cared` / `Disabling IRQ #N` within
about 1 s and the step continues, so a hang means something else. N is the
Linux IRQ number on the `20a0a0000.mca` line of `/proc/interrupts`, which
`probe` prints as `mca irq:`. A real
bus hang looks different: there is no CPU0 backtrace (`NMI backtrace` times
out), or the PC sits on an `ldr` in `s8000-mca`/`devmem`. Then send
`/tmp/r4-*.txt` as far as they got.

**If it is still silent** (no hang), the round-3 analysis applies. `diag`
answers the open questions on its own:

- **A:** PMGR power-state words for mca0/sio_busif/sio_p/sio/spi1 (actual
  state in bits 7:4).
- The `CS42L81 ID` probe line proves SPI only, not clocking. The ID is
  read before CLK_CTL is ever written.
- **Pad scan caveat:** a pad only shows its level in bit 0 when input
  enable (bit 9) is set. Pads marked `(ie 0: level not visible)` can drive
  a clock without ever showing `TOGGLING`, so a null scan is not proof that
  MCLK stays inside. Positive control for the decode: the i2c0 pads 46/45
  should show `periph 1`.
- **B/C:** pad scan idle and with MCLK forced on (debugfs
  `s8000-mca-mclk`) for each MCLK word 0x58/0x50/0x60/0x40/0x18. A pad that
  toggles only with MCLK on is the MCLK pad, which also shows its pinmux
  (`periph`, `b4`). **No toggling pad at all means MCLK does not leave the
  SoC**: either the pad mux or the MCLK source.
- **D:** during a stream: codec registers (`cs42l81-regs`: `0x0f`, `0x12`,
  status `0x2e/0x2f/0x33`), the pad scan, the MCA status, and i2s-switch
  reg15 = 0x11/0x01/0x10 with a pad scan each.

Further variants, each set **before** the stream so they apply from
power-up. Arguments are the MCLK word (0 = DT) and the route
(`"off=val,..."` byte offsets; empty = DT):

    audio-test.sh variant 0x50 ""
    audio-test.sh variant 0x58 "0x00=0x18,0x14=0x00080808,0x3c=0x11"
    audio-test.sh variant 0x58 "0x00=0x00181800,0x14=0x00080818,0x3c=0x10"

Codec register pokes (SPI, safe), while a stream runs or within 5 s after it
(DAPM keeps the codec powered):

    echo 0x0f > /sys/kernel/debug/cs42l81-reg; cat /sys/kernel/debug/cs42l81-reg
    echo "0x0f 0x8c" > /sys/kernel/debug/cs42l81-reg

Pad dump only (no MCLK change): `a9audio pads | less`. There is no
arbitrary MMIO peek any more. `a9audio peek` only reads GPIO pads and PMGR
ps registers.

## Round 2 reference: what a SmartIO boot shows

With round 2, MCA0 is fed by **SmartIO DMA** (endpoint 0x28), so a tone
should be **audible**. The bring-up order matches the dependency chain, so
each step's output tells where a failure is:

    SIO firmware boot -> SIO DMA endpoints -> MCA0 + MCLK -> CS42L81 ID -> card
    -> stream: SIO configure/submit -> MCA BCLK/FSYNC from codec -> sound

## Build (Mac)

The integration already carries `a9/audio` and `patches/audio/audio.config`.
The SPI1 driver comes from `a9/touch`.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

   On the device, `grep -c mca_hold /usr/local/bin/audio-test.sh` must not
   print 0. If it does, the old script is in the initramfs.

Without an initramfs rebuild, serve the tools instead:
`cd build/audio/device && python3 -m http.server --bind 172.16.42.2 8000`,
then on the device `wget` a9audio, audio-test.sh, the WAVs and
`build/device-bin/devmem`. The SmartIO firmware must already be in the
initramfs: `initramfs-fw.gz` has it.

## On the device (HoolockRD shell, via `scripts/linux-run.sh`)

    export PATH=/usr/local/bin:$PATH

### Step 1: probe (safe, no MMIO)

    audio-test.sh probe

Expected lines, in order (✓ = what each line verifies):

| dmesg | Meaning |
|---|---|
| `apple-sio 20ae00000.dma-controller: SmartIO firmware ready (491520 bytes)` | ✓ `ps_sio` at 0x80160, A7IOP V1 boot, firmware remap, RX IRQ 187, ready message (0x6d, protocol 3) |
| `... SmartIO shared regions configured` | ✓ the 10 setParam acks (coredump, panic, perf, DMA segments/responses) |
| `apple-sio-dma apple-sio-dma.N.auto: 36 SmartIO DMA endpoints` | DMA provider up |
| `s8000-mca 20a0a0000.mca: MCA version 4, FIFO tx 64 rx 64, SmartIO DMA` | ✓ MCA0 v4, DMA path selected |
| `cs42l81 spiN.0: CS42L81 ID 42a81x rev 0x..` | ✓ SPI1 + codec |
| `/proc/asound/cards`: `iPad5 Speakers` | card bound |

SmartIO failures. Every one is bounded: the probe is asynchronous, the
ready wait is 5 s and setup takes 1 s. Boot is never held up, but the
audio card stays absent because MCA0 waits for its DMA channel.

| dmesg | Meaning / next step |
|---|---|
| `failed to load firmware apple/s8000-smartio.bin` | file missing from the initramfs |
| `SmartIO CPU is already running (reset N)` | left running by the boot chain and the PMGR reset failed; report N |
| `SmartIO firmware did not become ready` | no ready message within 5 s. Wrong power domain (`ps_sio` is inferred), wrong RX IRQ, or firmware not started |
| `SmartIO setup timed out: index=I irqs=Q unmatched=U irq=.../... rx_status=... tx_status=... rx=... tx=...` | report the full line |

**If SmartIO fails, still test the rest**: boot with the extra bootarg
`snd_soc_apple_s8000_mca.nodma=1`
(`TREE=integ scripts/mkpayload.sh build/audio/initramfs-audio.gz snd_soc_apple_s8000_mca.nodma=1`).
That restores the round-1 timer PCM (samples dropped, clocks and codec
live). Then run steps 1–2 and the variants below.

If the codec ID check fails, run this (codec driver unbound, spidev):

    for d in /sys/bus/spi/devices/*; do grep -q cs42l81 $d/modalias && D=${d##*/}; done; echo $D
    echo $D > /sys/bus/spi/drivers/cs42l81/unbind 2>/dev/null
    echo spidev > /sys/bus/spi/devices/$D/driver_override
    echo $D > /sys/bus/spi/drivers/spidev/bind
    S=/dev/spidev${D#spi}; ls -l $S
    for m in 0 3 1 2; do a9audio spi $S -m $m -r 500000 0x1 0x2 0x3 0x5; done

Report which mode returns `42 a8 1x`. If every mode reads all `00` or all
`ff`, dump the pad configuration and send `/tmp/pins.txt` back. SPI1 has
no `pinctrl-0` and relies on iBoot, and the ADT does not list its pins:

    for p in $(seq 0 207); do printf 'pin %3d: ' $p; devmem $(printf '0x%x' $((0x20f100000 + 4*p))) 32; done > /tmp/pins.txt

### Step 2: audible tone (volume 150 = −27 dB, 4 s)

    audio-test.sh play

- **Listen:** a 1 kHz tone from both speakers.
- `a9audio` prints `played N frames in T s (expected E s), xruns X`.
  T ≈ E and X = 0 means SmartIO completions arrive on time.
- `stalled: no space for ... ms` means the PCM pointer never moved, so no
  0x68 completions are arriving. Check dmesg for `ep 0x28: configure
  failed`, `transfer failed` or `async error`.
- During the tone it holds MCA0 on (`power/control`) and prints the MCA
  status `+0x1c`. Bits 7 and 9 (BCLK and FSYNC activity) mean the codec
  clocks and the route work. Bit 0 (TX
  underflow) at a steady rate means data is not arriving fast enough.
- GPIO 64 should be high (unmuted) while it plays.
- It also prints `/sys/kernel/debug/dmaengine/summary`. One SmartIO
  channel should be in use by `20a0a0000.mca` (endpoint 0x28).

### Step 3: test files

    audio-test.sh wav

Plays, in order: 1 kHz 48 kHz, left-only 440 Hz then right-only 660 Hz,
1 kHz 44.1 kHz, a 200 Hz→4 kHz sweep, and a 32-bit-slot 440 Hz tone.
Report which physical speaker carries "left", whether anything is
distorted (slot/format mismatch), and the a9audio lines.

### Variants: only if SmartIO is fine but nothing is heard

    audio-test.sh mclk 0x50     # MCLK = nclk (instead of nclk/2)
    audio-test.sh mclk 0x60     # nclk/4
    audio-test.sh mclk 0x40     # output on, source 0
    audio-test.sh route 2       # swapped data-field directions
    audio-test.sh route 3       # pin0 also drives clocks

Each one plays 4 s with MCA0 held on and prints the IRQ status. The MCLK
word and the i2s-switch are only written while the stream runs. If the speakers stay silent
with clocks present, try the mute polarity:
`mute-gpios = <&pinctrl_ap 64 GPIO_ACTIVE_HIGH>` (needs a DT change).

`audio-test.sh fifo` (round-1 CPU FIFO probe) is no longer needed while
DMA works. Run it only on request.

## Safety

- The default volume is −24 dB with a −3 dB cap (ADT `spkr-gain-db`). The
  test signals are at −12/−18/−20 dBFS with fades.
- All waits are bounded:
  - SmartIO: async probe, 5 s ready wait, 1 s setup, configure 1 s, stop
    200 ms plus a 500 ms drain, then cancel.
  - MCA: reset/flush 10 ms, TX stop 1 ms in atomic context.
  - Codec: PDN_DONE poll 1 s.
  - a9audio: poll timeouts.
- If a stop times out, the driver logs `ep 0x28: N commands left after
  stop, cancelling`. Report it: the firmware may still have been reading
  freed descriptors.
