# Audio workstream (J71t speakers, mics, headphones)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Round 6 (2026-10-24, not yet run on the device): **capture from the two
built-in DMICs** (MCA0 RX over SmartIO endpoint 0x29), **headphone
output** (iOS's speaker-to-headphone register sequence) and a **headphone
jack** (`snd_soc_jack` with HPOUT/SPKOUT pins; detection is a status
poll, off until test T1 names the plug bit; debugfs can force a plug).
Patches: `patches/audio/capture-jack/`. Device steps: "Round 6" in
`test-plan.md`. Re-reading iOS for this round corrected several rows of
the capture/jack study (`ds-capture-and-jack.md` section 9): `0x0203` is
0xc0 with the DMICs on, ASP TX power is `0x0011` bit 6, `0x000d` is
analog-mic bias and stays 0 on J71t, and there is a TX byte-lane mask at
`0x001e..0x0021`.

Status after round 5 (2026-10-22): **audio works** on the device (owner
heard `speaker-test` on `hw:0,0` under Omarchy; codec `clk_ctl 0x8c`, an
exact 48 kHz clock, no IRQ storm). The round-4 IRQ-storm fix (`fbe859a481`)
and the round-3 CLK_CTL fix are confirmed. Round 5 (`1a1a5dc2f5`) marks
the SmartIO PCM as BATCH for PipeWire and logs when the stream's first
error happens. PipeWire shows no devices because the rootfs lacks
`pipewire-audio` (no SPA ALSA plugin), which is a userspace issue. Omarchy
test: `scripts/audio/omarchy-alsa-test.sh` (see "Round 5" in
`test-plan.md`).

| Doc | Content |
|---|---|
| `BRIEF.md` | task, facts, rules |
| `playback-path.md` | iOS speaker path and SmartIO protocol from Ghidra (verified/inferred) |
| `test-plan.md` | exact device steps, expected output and failure strings |
| `ds-capture-and-jack.md` | built-in mics (DMIC1A/1B → MCA0 RX / SIO 0x29) and the 3.5 mm jack: register tables, driver change list, test plan |

## Code

Kernel tree `audio` (branch `a9/audio`, base hoolock `6831bc701a`), exported
to `patches/audio/` (`series`, `BASE`, `HEAD`), config fragment
`patches/audio/audio.config`. 17 commits (14–15 = round 3: codec clock fix + debugfs, MCA overrides; 16 = round 4: IRQ storm fix, runtime PM, MMIO-safe debugfs; 17 = round 5: BATCH flag, stream error timing):

| # | Commit | What |
|---|---|---|
| 1–2 | CS42L81 binding + driver | SPI regmap (5-byte paged protocol), ID check, iOS power-up, speaker routing and volume, bounded power-down, mute GPIO |
| 3–4 | S8000 MCA binding + driver | v4 registers per iOS, MCLK word as a clock, i2s-switch route, dmaengine PCM over SmartIO (or `nodma=1` timer PCM), debugfs `s8000-mca-regs` (reads registers only while runtime active) |
| 5 | j71t audio DT | spi1 + codec, mca0, i2s-switch, simple-audio-card (`s8003-j71t-audio.dtsi`) |
| 6–8 | Pauli1Go SIO binding, core, DMA | cherry-picked verbatim with `-x` (MAINTAINERS hunks dropped) |
| 9 | SIO binding: S8000 | compatible, resets, `apple,dma-endpoints` |
| 10 | SIO core: S8000 | per-endpoint tags 1..31, 0x66/0x68 handling, notify/cancel hooks, direct firmware load, async probe, reset-if-running |
| 11 | SIO DMA | one channel per endpoint, shim record via `peripheral_config`, cyclic (one command per period, ≤16), op 8 stop with a bounded drain |
| 12 | MCA → SmartIO | shim record from the ADT words, 1 ms FIFO depth, `nodma` parameter |
| 13 | j71t DT | `ps_sio` (0x80160), SIO node, `mca0 dmas = <&sio 0x28>` |

Round 6 (capture, headphones, jack): 5 more commits on `a9/audio`
(`1a1a5dc2f5..5133d982ce`), exported separately to
`patches/audio/capture-jack/` (`series`, `BASE` = `1a1a5dc2f5`, `HEAD`).
`patches/audio/` itself still ends at round 5. No new config options.

| # | Commit | What |
|---|---|---|
| 1 | `e9fce91b89` MCA binding | `dmas`/`dma-names` = `tx`, `rx` |
| 2 | `9b03d9f8fb` CS42L81 binding | `hs-switch-gpios` (read only), pin names |
| 3 | `58b8042da0` CS42L81 | DMIC capture (iOS updateRouting input order), `Capture Volume/Switch`; `HP Amp` with iOS's route change and charge pump, `Headphone Playback Volume/Switch`; `Headphone Jack` (pinless kcontrol, HPOUT/SPKOUT switched by the driver), status poll (`jack_poll_ms`, `jack_reg`, `jack_mask`, `jack_invert`), debugfs `cs42l81-jack` |
| 4 | `fccffe1539` MCA | capture over SmartIO 0x29, per-direction prepare/trigger/hw_free (first direction programs the block, last one stops it), per-direction IRQ causes and first-error timing |
| 5 | `5133d982ce` j71t DT | `mca0 dmas = <&sio 0x28>, <&sio 0x29>`, mic/headphone widgets and routes, pin switches, codec `hs-switch-gpios` |

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Dependencies

- SPI1 controller: `spi-apple-s8000` from `a9/touch`, so the device test
  uses the integration kernel.
- SmartIO firmware `apple/s8000-smartio.bin`, already in
  `build/initramfs-fw.gz` and in the audio overlay.

## Tools

- `tools/audio/a9audio.c`: static ALSA player/mixer/spidev reader (no
  alsa-lib).
- `a9audio rec FILE.wav` (round 6): stereo capture with per-channel
  RMS/peak/DC and the measured rate.
- `tools/audio/mkwav.py`: test WAVs. `tools/audio/audio-test.sh`: on-device
  steps (`probe`, `play`, `wav`, `diag`, `variant`, `mclk`, `route`,
  `fifo`, and from round 6 `rec`, `jack`, `hp`). MMIO only on powered blocks: MCA0 while runtime active, the
  MCLK word and i2s-switch only during a stream, and pads/peek only on
  GPIO and PMGR ps.
- `scripts/audio/build-tools.sh` → `build/audio/device/`.
- `scripts/audio/omarchy-alsa-test.sh`: pure-ALSA test under Omarchy
  (`check`/`tone`/`lr`/`pw`; round 6: `mic`, `mic-sweep`, `hp`, `jack`).
- `scripts/audio/mkinitramfs.sh` → `build/audio/initramfs-audio.gz` (single
  gzip stream).
- `scripts/audio/sync-tree.sh`: copies `build/audio/src/*` (staging) into
  the tree. The branch is canonical.
- RE helpers: `tools/audio/kcmap.py`, `tools/audio/gdump.py`,
  `tools/audio/vtab.py`.
