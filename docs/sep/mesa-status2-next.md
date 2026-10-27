# Mesa2: reproduce Apple's phase setting, then one status transaction

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Offline implementation and package complete; **not hardware tested**.
No device was contacted during this work. This supersedes the mode-0 and
mandatory-cold-DFU assumptions in `mesa-status-next.md`. Mesa1 run 2 is the
baseline: `logs/tri53-mesa1-status-20261027-1653/RESULT.md` (23 RX zeros,
controller success, physical regulator enables observed on/off).

## Ranked diagnosis and recovered evidence

All addresses below are unslid addresses in the j71t iOS 16.7.10 kernelcache,
SHA-256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
These are static reconstructions, not a scope trace of the sensor pins.

1. **Incorrect clock phase is the strongest actionable explanation.** The
   32-byte ADT Mesa child `reg` is an Apple SPI configuration structure, not
   a Linux address/size pair. Its public board fields decode as CS 0,
   initial period 500 ns, polarity byte `+8=0`, phase byte `+9=1`, format
   byte `+10=1`, word size byte `+11=8`, interword delay `+12=0`, common CS
   delay `+16=20 ns`, and separate setup/hold fields zero. Thus the bit
   previously described as an internal-CS flag in `mesa-driver-re.md` §2.3
   is actually **CPHA**. This agrees between `docs/adt/j71t.txt` and the
   whitelisted SPI2/Mesa properties in the saved live IODeviceTree dump.
   No calibration, serial number, MAC, or other private value is copied here.

   More decisively, Mesa init at `0xfffffff005ef9fd4` reads the provider's
   `spi-frequency`, defaults `mesa-spi-clock-phase` to **3** at
   `0xfffffff005efa0bc`, gets the inherited SPI config, then forces polarity
   zero (`0xfffffff005efa10c`) and phase three (`0xfffffff005efa108`) before
   provider vtable `+0x550` applies it (`0xfffffff005efa120`). The `+0x690`
   init call in `AppleMesa_start` is at `0xfffffff005ef147c`, with its vtable
   target at `0xfffffff006e21038`. It precedes the first sanity poll.

   Samsung's config programmer `0xfffffff005b47a44` maps polarity to CFG[2]
   (`0xfffffff005b47a5c`), phase bit 0 to CFG[1]
   (`0xfffffff005b47a74..7c`), and, on version 1, phase bit 1 to **CFG[20]**
   plus **WORD_DELAY >= 2** (`0xfffffff005b47b0c..28`). Word size 8 leaves
   the width encoding zero. The v1 constants at `0xfffffff00530d650`
   supply CFG base `0x4000`, optional format flag `0x2000`, and FIFO depth
   16. Format byte 1 omits that optional flag, matching Linux's byte path.
   Apple phase **3 is Linux mode 1 plus a Samsung extension**, not Linux
   mode 3. The electrical meaning of CFG[20] is still unnamed; Mesa2 copies
   the demonstrated programming without assigning it an invented meaning.
   Mesa1 omitted both CPHA and this extension. Misdecoded requests can leave
   a sensor silent while the host FIFO completes normally.

2. **CS/timing and dummy MOSI differ, but have weaker causal weight.**
   `function-spi_cs0` is a null platform function, not a GPIO. There is no
   `internal-cs` property in the saved SPI2 node: the iOS controller checks
   that separate property in `AppleSamsungSPI_start`,
   `0xfffffff005b471f0`. A null platform function alone does not prove the
   iOS `_spiInternalCS` flag is set. Power-on writes PIN=0 in
   `0xfffffff005b4773c`; the internal-CS setter `0xfffffff005b49a70` clears
   PIN[1] on assert and sets it on deassert. Linux uses the same active-low
   polarity, PIN=0 throughout the full-duplex transfer, and PIN=2 idle. Its
   core passes physical-level `high` to `set_cs`; this is not inverted.
   No GPIO CS or `spi-cs-high` is justified. Apple rounds the descriptor's
   common 20 ns CS delay up to 1 us in `0xfffffff005b47b2c..78`, and its
   PIO path `0xfffffff005b485d0` supplies **0xff** after the seven TX bytes
   when RX length is 23. Mesa2 matches those timing/dummy settings too.

3. **Inherited pad routing remains possible, but no supported pinmux fix
   was found.** Neither saved ADT exposes SCK/MOSI/MISO/internal-CS pad
   numbers or an active SPI2 pinctrl group. The iOS Samsung start/config,
   power, and CS paths above program controller registers/clock functions;
   no per-transfer SCK/MOSI/MISO GPIO mux change was recovered. Both Linux
   SPI2 DTs have no `pinctrl-0`; they inherit boot-firmware routing. Linux's
   S8000 pinctrl changes only requested GPIO/IRQ/display-sync pins, rather
   than claiming a SPI2 group. The exact active iOS mux register values and
   pad identities remain **unknown without a saved GPIO register snapshot
   or schematic**. Do not invent a pin list or drive GPIO44 to test this.

4. **Missing reset/boot/wake GPIO or longer settle is not supported.**
   GPIO44 `[44 3]` is the sensor interrupt, not a proven reset/enable pin.
   Mesa's ADT has only the two power functions and HID dispatch; no
   `function-mesa-reset-ntfy`, `function-bb_gpio`, extra boot/enable GPIO,
   over-current, or current-monitor function. The optional reset function
   path in `AppleMesa_setSensorPower` (`0xfffffff005ef37e4`) is therefore
   absent here. The rail cycle is 1V8 on, 1 ms, 3V0 on, 100 ms; reverse
   order on shutdown, board off delay 1000 ms. No longer startup delay is
   shown. The no-3V-function long-reset fallback in
   `AppleMesa_powerOnReset` (`0xfffffff005ef3394`) does not apply to j71t.
   GPIO mode 3's precise Linux IRQ translation remains unresolved and is
   unnecessary for this experiment.

   Apple disables sensor interrupts at `0xfffffff005ef14bc`, then sanity
   polls at `0xfffffff005ef14f4`, before its later interrupt-line self-test.
   It does **not wait for a sensor IRQ or send a wake/reset SPI command
   before this first status poll**. `AppleMesa_getSensorStatus`,
   `0xfffffff005ef3024`, sends exactly `80 10 00 07 00 00 00`, TX length 7,
   RX length 23, then copies status at RX+7. The startup loop permits three
   sanity polls and rail power-cycles between failed checks; the separate
   `getSensorStatusWithRetry` at `0xfffffff005efafc8` is not that initial
   loop. A longer polling loop is not a demonstrated prerequisite.

5. **Clock/PMGR or a generic PIO failure ranks below phase/routing.** The
   ADT maps SPI2 to MMIO `0x20a088000`, gate 76, clock ID 303 (61,538,461
   Hz), AIC 190. Linux matches those and `ps_spi2` at PMGR `0x801c8`, parent
   `ps_sio_p`. The controller pins a runtime-PM reference by default and
   divides by eight (7,692,307 Hz). The local driver is
   `drivers/spi/spi-apple-s8000.c`, not the M1 `spi-apple.c`: both counters
   are set to 23, byte TXDATA/RXDATA and v1 FIFO counts match Apple's PIO
   path, and every transfer is limited to one second. Receiving 23 FIFO
   bytes strongly suggests the digital block runs; it does not prove MISO
   pin routing or prove that a sensor drove the zeros. SPI1 audio reads work
   and SPI3 N1 discovery works with this driver; neither proves hardware
   SPI2. Linux bus names differ: mesa is `spi0.0`, codec `spi1.0`, and
   `spi2.0` is **hardware SPI3 N1**, not Mesa. No other working hardware
   SPI2 client was found.

## What Mesa2 changes

- J71t only: `spi-cpha`, `apple,s8000-spi-clock-phase=<3>`, and 1 us CS
  setup/hold. The controller reads that optional per-child property, checks
  CPHA consistency, sets CFG[20], and floors WORD_DELAY at two input clocks.
  Audio/N1 clients without the property retain their transfer settings.
- TX is the same seven-byte request plus sixteen `ff` dummy bytes. Reply
  parsing stays strict `80 20 ?? 10` at RX+7. No new sensor opcode exists.
- Hard maximum **one status attempt per driver instance/boot**, one-way
  arm, no retry or mode fallback. One is below Apple's three sanity checks
  and isolates the proven timing correction; the iOS code does not require
  multiple initial polls. Do not unbind/rebind to bypass the budget.
- After the status operation, the kernel reverses the rails and disarms
  before copying the saved result to userspace, including SPI/setup/header
  failure. The runner's EXIT/signal trap also disarms and verifies the two
  regulator-class states on failures. A regulator I/O failure is reported,
  not claimed to have restored hardware. Raw bytes survive teardown.
- No sensor IRQ, reset command, SEP traffic, firmware/patch upload, NVM
  write, frame request, capture, or persistent-storage operation is added.

## Artifact and offline verification

Kernel branch `a9/tri53-mesa2`, commit
`9186f845ea994fd69fe9ad1e6d447820716b33c5`, parent
`cc0b92c36c7917d3b11ee7ad845e27b266f9fc2c`.
Only this kernel branch was committed; nothing was pushed or committed in
the workspace. The reused Docker worktree remains `/src/wt/tri53-mesa1`
but now checks out `a9/tri53-mesa2`; do not infer its branch from its name.

- Entry: `tri53-mesa2-9186f845ea`.
- Payload: `build/boot/m1n1-linux-tri53-mesa2-9186f845ea.bin` (14,715,011 B).
- Payload SHA-256:
  `828d139a2059dc1a37b6f4539d1ac9232358342b7c08b6c81625f1ac260d0555`.
- Entry directory: `build/sep/tri53-mesa2/tri53-mesa2-9186f845ea-entry/`.
- Manifest: `build/sep/tri53-mesa2/payload-manifest.json`; includes every
  entry component, kernel/config/commit, runner, m1n1, and kernel patch hash.
- Clean cmdline: `build/sep/tri53-mesa2/cmdline` (835 bytes without newline),
  exactly one `pcie_apple_h9p.ans_keep_power=1`, one `mesa_a9.enable=1`, no
  SEP option, debug shell, loglevel or ignore_loglevel token. All other tokens
  match the mesa1 run-2 clean baseline; only the entry name changes.

`scripts/sep/build-tri53-mesa2.sh` builds in the reused tree with debug info
off. Both touched C objects were forced to compile with `KCFLAGS=-Werror`;
the final Image.gz/DTB build has **zero warning/error diagnostics** in
`build/sep/mesa2-re/build.log`. An initial format-type error in the new PIO
diagnostic was corrected before this final build. Docker free space is
8.8 GiB immediately after build (8.6 GiB at final audit); no new kernel
worktree or source clone was created.

Offline commands (all no-device):

```sh
python3 -B -I tools/sep/mesa_test.py
python3 -B -I tools/sep/test-mesa2.py
python3 -B -I tools/sep/test-mesa2-runner.py
sh -n scripts/sep/remote-mesa-status2.sh
bash -n scripts/sep/build-tri53-mesa2.sh
```

The parser/safety suite and lifted C tests pass: actual status functions,
phase values 0..3 and mismatch rejection, max attempt refusal, partial reads,
SPI/setup/header/copy faults, shutdown order, and visible rail-disable
failure. The shell fixture suite covers success, all-zero and SPI-error
replies, rail mismatch, wrong entry, used arm, and duplicate regulator names.
Packaging checks the **actual** DTB, extracted image/initramfs, command line,
and all entry hashes. Host tests cannot validate electrical timing or pins.
RE is reproducible with `scripts/sep/mesa2-re.py` using the ghidra-mcp venv
and `GHIDRA_INSTALL_DIR`; Apple decompilations stay under `build/sep/mesa2-re/`.
Shared Ghidra received SPI start/CS/interrupt-helper renames and a phase
comment. Its save command deferred because another request was active;
those live-analysis edits are not claimed persisted. The offline outputs
and address citations above do not depend on that save.

## Orchestrator procedure: warm kexec is sufficient

A cold DFU is **not required by this experiment**. No SEP OOL registration or
sepOS lifetime is used, and warm kexec already ran mesa1 successfully through
the controller. Require a healthy source boot, both Mesa rail enables off,
and normal SPI shutdown. The runner reads physical regulator enables and
waits another full second with them off before powering up. A cold boot is
an independent comparison only if corrected timing still produces zeros,
or recovery is needed after failed shutdown/rail restoration. A cold DFU
would not by itself prove a pinmux hypothesis.

The following commands are for the orchestrator; the implementation agent
has not run them:

```sh
set -euo pipefail  # run this procedure in bash
PKG=build/sep/tri53-mesa2
ENTRY=tri53-mesa2-9186f845ea
P=build/boot/m1n1-linux-$ENTRY.bin
RUN="$PKG/status-$(date +%Y%m%d-%H%M)"
mkdir -p "$RUN"
(cd "$PKG/$ENTRY-entry" && shasum -a 256 -c SHA256SUMS)
test "$(shasum -a 256 "$P" | awk '{print $1}')" = \
  828d139a2059dc1a37b6f4539d1ac9232358342b7c08b6c81625f1ac260d0555

# Read only: the source boot must already have both enables off.
scripts/omarchy/ssh.sh 'set -eu
for r in /sys/class/regulator/regulator.*; do
  n=$(cat "$r/name" 2>/dev/null || true)
  case "$n" in mesa-3v0|mesa-1v8) echo "$n $(cat "$r/state")" ;; esac
done' >"$RUN/0-source-rails.txt"
grep -qx 'mesa-3v0 disabled' "$RUN/0-source-rails.txt"
grep -qx 'mesa-1v8 disabled' "$RUN/0-source-rails.txt"
test "$(wc -l <"$RUN/0-source-rails.txt")" -eq 2

# Explicit clean cmdline; never append to the source boot's /proc/cmdline.
ENTRY="$ENTRY" scripts/boot/kexec-host.sh "$P" \
  "$PKG/$ENTRY-entry/dtb" "$(cat "$PKG/cmdline")" \
  | tee "$RUN/0-kexec.txt"
# Require the script's NEW boot_id, intended entry, and NVMe health PASS.
# pipefail stops here if kexec failed, even when tee succeeds.

scripts/omarchy/ssh.sh 'cat /proc/cmdline; uname -a; uptime; findmnt /; lsblk' \
  >"$RUN/1-preflight.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' >"$RUN/dmesg-before.txt"
# Require inert Mesa probe, mode=1, both D2365 supplies, no probe failure,
# NVMe timeout or I/O error. No pongo/SEP setup, no finger, no other Mesa read.

rc=0
scripts/omarchy/ssh.sh "sudo -n env ENTRY=$ENTRY sh -s" \
  <scripts/sep/remote-mesa-status2.sh >"$RUN/2-mesa-status.txt" 2>&1 || rc=$?
echo "runner_rc=$rc" >"$RUN/runner-exit.txt"
# Collect evidence on failure too; never rerun the sensor operation.
scripts/omarchy/ssh.sh 'sudo -n dmesg' >"$RUN/dmesg-after.txt"
cat "$RUN/2-mesa-status.txt"
```

Pass requires **all** of:

- Initial and settled regulator-class states disabled; powered readback
  enabled for both; final/cleanup readback disabled for both.
- Exactly one PIO begin/end on Mesa, length 23, speed 7,692,307,
  `cfg=0x10403a word_delay=2 pin=0`, controller return zero, tx/rx counts 23.
- Saved status text `spi_error=0 valid=1 attempts=1`, request plus sixteen
  `ff` dummy bytes, and RX header `80 20 ?? 10` beginning at byte seven.
- Final state `armed=0 arm_used=1 powered=0 rail_1v8=0 rail_3v0=0`,
  `status_attempts=1 last_status_err=0 last_status_valid=0`, and
  `RESULT mesa-status2=pass`. The saved transfer text has valid=1; the live
  state clears last_status_valid when power is removed, intentionally.

Any raw sensor state/error/context is acceptable with a valid packet;
neither error byte `0x29` nor a header pass means Touch ID is ready.

Stop before arming on wrong entry/cmdline, unhealthy boot, missing/duplicate
regulator, enabled baseline rail, wrong mode, used arm, or nonzero attempts.
On configuration mismatch, timeout, invalid/all-zero/all-ff packet, or rail
readback mismatch, save raw output and dmesg, verify cleanup, and **do not
retry, rebind, change modes, add an IRQ, or issue another command in that
boot**. If either rail cannot be restored off or the controller/NVMe fails
shutdown, stop and recover with a cold reset/DFU. Never write syscfg, NVRAM,
LLB/iBoot, or terminate unrelated QEMU/NBD processes. Even a pass ends this
experiment; firmware upload/frame/SEP work needs a separate reviewed rung.
