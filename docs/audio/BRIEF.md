# Audio workstream brief (iPad 5 j71t speakers)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Goal: ALSA playback through the iPad's built-in speakers under Linux (an
ALSA card PipeWire/Omarchy can use). Microphones and headphone jack come
later.

## What's known (2026-10-22)

- Device: j71t (iPad6,11, S8003 A9), iPadOS 16.7.10 20H350. HoolockLinux
  7.3.0-rc1 boots (`PLAN.md`, `docs/STATUS.md`).
- ADT (`docs/adt/j71t.txt`, live values in
  `build/firmware/private/IODeviceTree.txt`): `/arm-io/mca0` (`mca,s8000`,
  `mca,s5l8950x`) with child `audio-codec` (`audio-data,cs42l81`);
  `/arm-io/spi1/audio-codec` (`audio-control,cs42l81`); `/arm-io/mca1`
  with `audio-codec-voice`; `/arm-io/i2s-switch` (`i2s-switch,s8000`);
  `/product/audio`; `sio` + `sio-dma` (SmartIO, Apple's I/O coprocessor
  that does DMA for SPI/UART/MCA on these SoCs). The AOP has an `mca`
  child too (probably sensors/voice, check).
- The codec is a **Cirrus CS42L81** (Apple part). Linux has cs42l83/cs42l84
  (Asahi, M1/M2 headphone path), cs42l42 and friends. Check how close
  CS42L81 is before writing a new codec driver.
- MCA: Asahi's `sound/soc/apple/mca.c` (`apple,t8103-mca`) is the
  M1-generation MCA. The A9 MCA (`s5l8950x` lineage) may differ, so verify
  it register by register.
- DMA: probably via SIO. Pauli1Go/HoolockLinux branch `touch` has an "SIO
  core" + "SIO DMA" for T8010 (A10). **The touch agent may port SIO to the A9
  in `/src/wt/touch` (branch `a9/touch`). Don't duplicate it.** Read that
  branch. If you need SIO before it lands, tell the orchestrator. The SIO
  firmware for this SoC is staged at
  `build/firmware/lib/firmware/apple/s8000-smartio.bin`.
- Prior art: Project Cascadia (A5 iPad mini) got both speakers working with
  DMA (`third_party/project-cascadia`, upstream `docs/research/p105-audio.md`).
  Different SoC, but useful method.
- **Ghidra:** the shared server holds the whole analyzed iOS kernelcache,
  saved and persistent. Use the `ghidra` MCP tools (if present in your
  session) or the CLI `scripts/ghidra.py` (works from any shell). Look at the
  audio kexts (CS42L81 codec driver, MCA/I2S, i2s-switch, SIO audio DMA,
  speaker/amp control). Rename what you understand; it's shared and
  autosaved.

## Round 1 tasks

1. Reconstruct the iOS speaker playback path: codec control over SPI1
   (register map, power-up, routing to the speakers, volume), MCA/I2S
   config (clocks, format, which MCA instance and pins), the i2s-switch,
   DMA (SIO channels), any amp/enable GPIOs or PMIC rails. Document it in
   `docs/audio/playback-path.md` with Ghidra addresses (verified/inferred).
2. Implement it in kernel tree `audio` (`scripts/build-kernel.sh --tree
   audio`, `--run audio '<cmd>'`, branch `a9/audio`): codec driver or
   extension, MCA support for s8000/s8003, the machine driver / DT
   (simple-audio-card or an Apple machine driver), j71t DT nodes. Every
   hardware wait must be bounded.
3. A way to test playback from the HoolockRD shell (no alsa-utils there):
   e.g. a static `tinyplay`/`aplay` + a short test WAV, served to the
   device like `tools/devmem`. Write the exact commands in
   `docs/audio/test-plan.md`. Export patches to `patches/audio/`, with a
   config fragment at `patches/audio/audio.config` (the integration script
   merges fragments).

## Rules

> Private graphics-dependent excerpt omitted from this source snapshot.

Final report: the playback path, driver status (builds? implemented
parts), dependencies (SIO), test plan, risks.
