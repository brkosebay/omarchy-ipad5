# Porting Linux to another A9/A9X device

Start with a board-specific RAM boot and a read-only hardware inventory.
Treat this repository as a worked example for **j71t/S8003**, not a universal
installation recipe. Only iPad6,11 / j71t / S8003 has been tested.

The **iPad Pro 9.7 is A9X S8001, j127/j128**, not the tested A9 board. Its
SoC, graphics hardware and board wiring differ. Device trees, clocks, power
domains, interrupts, display, touch and firmware must be checked independently;
this repository does not claim a working boot on it.

[Current status](docs/release/STATUS.md) and
[evidence](docs/release/EVIDENCE.md) take precedence over dated test notes.

## Reuse and replace

| Piece | Reusable starting point | Recheck for your device |
|---|---|---|
| Kernel / boot | HoolockLinux `hoolock`, m1n1 `idevice`, pongoOS `bootm` | SoC/board DTB, memory reservations, entry level and CPU bring-up |
| A9 variants | iPad 5 has j71s/j71t Wi-Fi and j72s/j72t cellular DTBs | S8000 Samsung vs S8003 TSMC; never reuse voltage/calibration tables blindly |
| PCIe / ANS / NVMMU | A9 port based on Pauli1Go's A10 T8010 work | Compatible strings, bridge count, IRQs, gates, DMA/DART ownership, namespace map |
| BCM4355 / bq27540 / CT819 | Same part families as A10 iPad 7 | Board routing, firmware variant, GPIOs, MAC/calibration from your own unit |
| Touch | j71t N1 / `multi-touch,j72` | A10 uses `hid-transport,spi`; firmware, MtCl and panel transform are board-specific |
| Audio | CS42L81 + SPI1 + MCA/SmartIO design | A10 iPad 7 has different codec/transport; endpoint and clock topology differ |
| Display | Adopt the boot mode, then guarded flip and vblank | Panel timing, physical/bus address relationship, DART, FIFO and IRQ layout |
| AOP | RTKit / AFK / EPIC methodology and IIO plumbing | Firmware image, endpoint map, calibration state, six-pose mount matrix, retained SPI state |
| Desktop | Generic aarch64 Omarchy, llvmpipe, tablet helpers | User account, keymap, scaling, gestures and rotation; avoid Asahi Mac package detection |

Kernel source lives in the public [brkosebay/linux](https://github.com/brkosebay/linux)
`a9/orchard-j71t` branch, the clean OrchardLinux series on base
`6831bc701a`. This port is in preparation and is the sole public kernel
entry point. Its exact series needs device validation. Historical patch
exports carry their own BASE/HEAD/series and are not a combined stack.
No kernel binaries or kernel source tree are bundled here.

## Build an evidence set before a driver

1. Identify ProductType, board, SoC and OS build. Use
   `scripts/identify-device.sh`; keep ECID, UDID and serial output private.
2. Dump an IPSW Apple DeviceTree with `scripts/get-adt.sh <ProductType>`.
   Compare register windows, interrupts, clocks, power domains and compatible
   strings against the closest working board. Dumps go into `docs/adt/`,
   which is ignored.
3. On your own jailbroken iOS installation, obtain the live ADT as a **text**
   ioreg dump. The tested iOS build's `ioreg -a` XML was broken. The live tree
   includes real factory calibration where the IPSW has placeholders. Keep
   it in `build/firmware/private/`, with namespace backups and hashes.
4. Extract your firmware locally (`scripts/periph/pull-firmware.sh` and
   subsystem extractors). Never substitute this unit's calibration or include
   proprietary blobs in a public patch.
5. Analyze the matching kernelcache in Ghidra. `scripts/ghidra-mcp.sh`
   manages a local headless server; `scripts/ghidra.py` provides a CLI client.
   Configure your own tool installation and kernelcache path. Save names,
   structure types and comments so the next session can reuse the analysis.
   Match the extractor to the kernelcache layout; older prelinked caches
   are not supported by every extractor.

**A7–A11 have no EL2.** The m1n1 hypervisor MMIO-tracing workflow used on M1
cannot trace iOS drivers here. Use ADT comparisons, static kernelcache RE,
read-only observation on jailbroken iOS, and controlled Linux experiments.
Even reads on iOS can panic the device. Keep probes bounded and coordinate
one device operator. A function name is a hypothesis until
its callers, arguments and behavior agree.

For each finding record: source OS build, function/address, register offsets,
structure layout, evidence, competing interpretations, bounded experiment,
expected output and stop conditions. Publish analysis and protocol facts,
not bulk decompiled Apple implementations or probe captures.

## Establish the boot loop

Establish a board-specific tethered RAM boot with the boot scripts and
your own firmware/calibration. Then add one subsystem in a separate kernel worktree on a case-sensitive
filesystem; the public build uses the separate `omarchy-ipad5-src` Docker volume to avoid
APFS filename collisions and the private lab source volume.

- HoolockLinux's pongoOS supplies `bootm`; stock `bootr` is not equivalent.
- `mkpayload.sh` sends all four iPad 5 DTBs and m1n1 chooses the compatible
  one. Extend the list for another product; adding a DTB is not validation.
- m1n1 treats every gzip member as a separate payload. Each cpio replaces
  the previous initrd, so repack concatenated initramfs members as one gzip
  stream or the boot can lose `/init`.
- Lock firmware fallback paths. Missing firmware once delayed boot by 60 s.
- Keep logs outside the device across resets. Read-only storage first;
  namespace backups and a kernel-enforced write window precede installation.

## Debugging and recovery

Warm kexec avoids the full DFU loop when the platform remains healthy.
Read [warm reboot](docs/boot/warm-reboot.md) and
[ANS failure analysis](docs/boot/kexec-hang-2026-10-25.md).
The A9 path needs m1n1's opt-in EL3 PSCI shim (`psci=1`) and verified PSCI CPU
lifecycle support. There is no native A9 EL2 shortcut.

For an unchanged DTB, reuse the running FDT. A pristine build lacks m1n1's
memory, framebuffer, calibration/MAC, reservations and CPU fixups. For a
changed DTB, the path-aware FDT merger compares the staged running pristine
DTB, actual running FDT and new pristine DTB; refusal is a stop condition.
Stage the original cold entry so the merge has provenance. Never force a new
pristine DTB past a failed merge.

A separately armed crash kernel can recover panics and preserve ramoops;
it cannot recover every hang. If a secondary core cannot stop or ANS never
trains, the crash path may fail too. Preserve the last serial/USB/pstore
milestones, use button reset and DFU, then return to a known-good cold boot.
The shared-ANS-MSI fix passed 10/10 gated warm boots; use the health gate.
Earlier ANS link-training/post-suspend recovery and AOP retained-state failures
still need separate validation.

A useful experiment changes one factor, has an exact pass/fail witness,
checks the control case, bounds every poll, and refuses on stale identity or
unexpected state. A compiled driver or a completed mock test is not a device
pass. Keep old failed hypotheses labelled as history.

## Specific lessons from this port

- **SEP A9 mailbox width matters:** TX must be one 64-bit store (`writeq`)
  and RX one 64-bit pop (`readq`), with ordering as documented in the driver.
  Do not split a word into 32-bit operations. Check availability via the safe
  status path first; **never read an empty outbox**. Linux now discovers
  eight services, initializes SBIO and reads GetProtectedConfig through
  GenericTransfer framing. A valid Mesa sensor status reply was received;
  later SEP patch fetch was refused before sensor power-on. Fingerprint
  capture/matching/enrollment remain open.
- AOP cold streaming required calibration-state property `0x3d=2` before
  rate keys. The measured mount matrix belongs to this board. Warm reload
  needs outgoing quiesce and safe retained SPI0 channel teardown. wake7
  fixes teardown but accel STARTEP still crashes firmware; wake8 adds an
  event-3 reset and is untested. Cold success does not establish warm success.
- Lightning CBTL1610 TX uses the corrected `0x5d/0x60/0x61` window; writes
  copied from another Tristar generation clobbered autonomous replies.
  Camera-adapter iOS capture disproves the assumed iAP device-first flow:
  ID 11f0 plus power handshake selects host directly. usb-host5 auto-role
  works, but the missing PHY bit and host charging limit block completion.
- Keep the desktop on software rendering. Display scanout and H.264
  decoding are separately tested hardware paths.
- Requested CPU frequency is not delivered frequency. Check hardware state,
  workload correctness and restore guards. Short S8003 trials through 2.16
  GHz do not authorize the same clock or voltage on another A9.
- A surviving USB disconnect can look like a failed boot. Correlate serial,
  pstore and a new boot identity before scoring a result.

## Give an AI agent a bounded job

Have it read AGENTS.md, this guide, the current status, the subsystem design
and its test plan. Supply your board inventory privately. Ask for one
patchable hypothesis with source addresses, an offline build/check, and a
precise hardware test with stop conditions. Coordinate one device operator
at a time; device access requires the owner's explicit scope. Never let
parallel sessions boot or write the same device concurrently.

Protect syscfg, NVRAM and boot firmware unconditionally. Keep private dumps,
keys and firmware out of commits. Stop on a namespace mismatch, unexpected
power state or failed guard. See [AGENTS.md](AGENTS.md) for the repository
contract and [omissions](docs/release/OMISSIONS.md) for this snapshot's limits.
