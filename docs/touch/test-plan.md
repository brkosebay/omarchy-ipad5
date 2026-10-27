# J71t N1 owner-run test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Round 3. **All integration/device commands below are for the orchestrator.** The touch
implementation agent ran only offline analysis, builds, and host tests.
Expected messages are acceptance criteria, not claimed device results.

## Round 3: normal boot, portrait mapping and clean teardown

**Verified, owner/orchestrator:** `b0270d0bb3` passed all three stages in
integration kernel `77fccbcaf19b`, with audio moving N1 to `spi1.0`.
The 20-second capture has 9508 events / 1151 frames, median gap 16.667ms,
max gap 299.826ms and no SYN_DROPPED records. Corner order is confirmed:
TL/TR/BL have low/high/low X and high/high/low raw Y. J71t therefore needs
only Y inversion. Full report: [round3.md](round3.md).

**Current clean head:** `6ca4db775649909602763aa6bfd7bcbbe867285a`.
Two new commits after the tested head: `0cbca54ab4` (synchronized cleanup,
shutdown and firmware metadata) and `6ca4db7756` (unconditional J71t
`touchscreen-inverted-y`). Patches 0008/0009 are the Round3 delta.
The corrected DT must accompany the kernel.

The touch agent built only its own tree. The following integration/device
commands are **for the orchestrator**, and have not been executed by this
workstream. Preserve the existing integration branches:

```sh
scripts/build-kernel.sh --run integ '
set -e
test "$(git branch --show-current)" = a9/integ
test -z "$(git status --porcelain)"
git merge --no-ff --no-edit 6ca4db775649909602763aa6bfd7bcbbe867285a
scripts/kconfig/merge_config.sh -m .config /work/patches/touch/touch.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
while IFS= read -r option; do
    case "$option" in CONFIG_*=y) grep -qxF "$option" .config ;; esac
done </work/patches/touch/touch.config
'
scripts/build-kernel.sh --tree integ
TREE=integ scripts/mkpayload.sh build/initramfs-fw.gz
```

Boot that payload using the existing tethered workflow. **Pass no
`apple_n1.boot_stage` or `apple_n1.trace` arguments for this test.** Defaults
are3 andN. The combined initramfs already contains both unchanged assets;
retain them in normal-use rootfs/initramfs builds. MODULE_FIRMWARE now
advertises both names to initramfs tools; it does not supply the files.

Once the shell appears:

```sh
scripts/linux-run.sh 'cat /proc/cmdline; cat /sys/module/apple_n1/parameters/boot_stage; cat /sys/module/apple_n1/parameters/trace; dmesg | grep -Ei "apple-n1|firmware|timeout|stall|lockup"; cat /proc/bus/input/devices'
TIMEOUT=25 scripts/linux-run.sh -f scripts/touch/capture-events.sh
```

Expected: automatic HBPP/upload/calibration/descriptors and
`touchscreen reporting enabled (inverted-y=1)`, plus the direct touchscreen
input block with `B: PROP=2`. No manual rebind should be needed to create it.
Repeat TL, TR, BL, BR taps with the home button at the bottom. Expected
normalized corners are approximately (0,0), (1,0), (0,1), (1,1), respectively.
The already captured TL/TR Y values94.7%/94.6% should become about 5.3%/5.4%;
BL1.3% should become98.7%. X stays the same. Do not add another Y inversion
in libinput/Hyprland; keep their extra transform at identity for portrait.

### Read-only evdev metadata audit

`build/touch/device/n1-input-info` is a static arm64 tool built by
`scripts/touch/build-input-info.sh`. It opens evdev read-only, performs
bounded capability ioctls, never grabs the device and consumes no events.
It checks DIRECT=1, POINTER=0, BTN_TOUCH, tracking IDs/slots, X/Y minima0,
maxima14620/19538 and resolution100 units/mm, on both MT and legacy axes.
It reports every value and exits nonzero if the contract differs.

To transfer it using the existing USB-network tools, serve the repo on
the Mac (or reuse the already running server at this address):

```sh
python3 -m http.server 8000 --bind 172.16.42.2
```

In the orchestrator's other terminal:

```sh
scripts/linux-run.sh 'timeout 10 wget -O /tmp/n1-input-info http://172.16.42.2:8000/build/touch/device/n1-input-info && chmod 755 /tmp/n1-input-info'
```

The lifecycle script below discovers the event node dynamically and runs
this tool when installed at `/tmp/n1-input-info`. Alternatively, invoke it
with the actual event path printed by `capture-events.sh`; do not assume
event1 or spi1.0 remain fixed after future merges.

On Omarchy, additionally inspect userspace classification and output:

```sh
udevadm info -q property -n /dev/input/eventN
libinput list-devices
hyprctl devices
hyprctl monitors
```

Substitute the actual node for eventN. Expect ID_INPUT=1,
ID_INPUT_TOUCHSCREEN=1, libinput touch capability and approximately
146.20x195.38mm active size. The kernel marks DIRECT; udev supplies the
ID_INPUT properties. Hyprland's touch output defaults to auto-detection;
if it chooses the wrong display, set the touchdevice `output` to the exact
monitor name from `hyprctl monitors`. No connector name or compositor
configuration is fabricated or changed by this workstream. See
[round3.md](round3.md) for source links and the property audit.

### Clean unbind/rebind and missing-file tests

Keep a finger on screen during the first unbind, then lift it. Stop the
previous raw-event reader before running this test:

```sh
TIMEOUT=75 scripts/linux-run.sh -f scripts/touch/check-lifecycle.sh
```

Expected: one bounded unbind, `N1 stopped; reset asserted`, removal of the
old event node, then one stage3 rebind that recreates working touch with
`inverted-y=1`. Cleanup frees/synchronizes the IRQ before releasing all
contacts, asserting reset and disabling display-sync/clock/LDO/analog.
The same cleanup is registered for shutdown. This was audited and tested
with host stubs, including an already masked IRQ; device teardown still
needs this test. No automatic recovery/rebind loop is used.

The uploaded script is now `/tmp/check-lifecycle.sh`. On HoolockRD's RAM
rootfs, run the two negative tests individually:

```sh
TIMEOUT=75 scripts/linux-run.sh 'N1_TEST=missing-fw sh /tmp/check-lifecycle.sh'
TIMEOUT=75 scripts/linux-run.sh 'N1_TEST=missing-cal sh /tmp/check-lifecycle.sh'
```

Each command unbinds, temporarily renames **one RAM firmware file**,
tries one bind under a five-second timeout, restores the file through a
trap, then performs one normal rebind. Expected failure is ENOENT(-2),
with before/after uptime printed; normal files should be rejected within
well under a second on RAM storage. A five-second timeout or successful
missing-file probe is a test failure. There must be no 60-second firmware
fallback, upload, or controller power-on on the failed probe. The driver
requests/validates both assets before `n1_power_on()` using
`request_firmware_direct()`/NOFALLBACK_SYSFS. Factory storage is not read
or modified by these tests. Repeat corner/event sampling after recovery.

**Suspend boundary:** system suspend/resume has not been implemented or
hardware-validated. Use clean unbind before a platform suspend experiment,
and rebind after platform resume; no claim is made that other platform
clock/power providers resume correctly. This round satisfies the clean
unbind option of the task instead of introducing an untested PM reload
sequence.

## Historical Round 2 diagnostic recipe

The commands below reproduce the staged Round2 investigation; normal use
should follow the Round3 procedure above without stage parameters.

## Round 2 next test: preserve integration and capture control replies

**Verified prior device results:** `e22fc2eb3f` passed stage1, detected
controller `0x434d15c1`, uploaded both private assets, and completed
calibration transport with `0x4969`. Stage2 then failed twice with `-74`.
The previous trace parameter covered only runtime frames. These logs
cannot distinguish E2/E3/E6/E7 echo/checksum failures. See the
[Round2 audit](round2-runtime-re.md) and [transport correction](round2-transport-re.md).

**Fix to test:** `b0270d0bb30a1f017aa4d7c7233fd4bfe5701c82`, one new
commit after `e22fc2eb3f`. It removes the erroneous 5us word gap (iOS N1
sets 0), adds the missing 2ms post-boot and GET-entry delays, and retries
corrupt descriptor replies at most five times in total. CS setup/hold,
E2/E3 packet bytes, checksum byte order and descriptor order stay as
verified in iOS. Controller `0x434d15c1` uses const-cal 3; only
`0x434d11a0` uses 2. No additional execute poll or ATN_ACK is indicated
by the selected iOS path.

> Private graphics-dependent excerpt omitted from this source snapshot.

```sh
scripts/build-kernel.sh --run integ '
set -e
test "$(git branch --show-current)" = a9/integ
test -z "$(git status --porcelain)"
git merge --no-ff --no-edit b0270d0bb30a1f017aa4d7c7233fd4bfe5701c82
scripts/kconfig/merge_config.sh -m .config /work/patches/touch/touch.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
while IFS= read -r option; do
    case "$option" in CONFIG_*=y) grep -qxF "$option" .config ;; esac
done </work/patches/touch/touch.config
'
scripts/build-kernel.sh --tree integ
TREE=integ scripts/mkpayload.sh build/initramfs-fw.gz apple_n1.boot_stage=1 apple_n1.trace=1
```

Use the existing successful combined `build/initramfs-fw.gz`; touch
firmware/calibration bytes are unchanged. If reproducing a standalone
touch test instead, the independently verified single-stream archive is
`build/touch/initramfs-touch.gz`. The touch agent rebuilt/staged only
`build/kernel-touch/`, and did not rebuild or alter the integration tree.
The orchestrator performs the usual tethered boot of the newly assembled
payload. Rebinding the old running kernel cannot test this fix.

After stage1 succeeds again, run **one** stage2 rebind with tracing:

```sh
TIMEOUT=45 scripts/linux-run.sh -f scripts/touch/capture-stage2.sh
```

Capture the **whole** filtered log; `tail -100` can discard the beginning
of the new control trace. Expected success is `N1 ready: interface 1, max
packet 2454, ... 40x30, FW 0x210, surface 14745x19657`. Expected execute
TX is `1d 53 34 00 10 00 00 01 00 00 00 45`. Any failure now reports:

```text
control e2/00 echo mismatch: echo=... expected=e2 checksum=... calculated=...
control rx[0/16]: ...all sixteen bytes...
E2 discovery attempt .../4 failed: -74
```

That is an example format, not a prediction of the failed opcode.
`e3/d1` identifies D1 report-info, `e6/d1` its short data reply, and
`e7/d9` the surface's long data reply. A correct echo with a bad sum says
`checksum mismatch`. Status/length, E2 interface, D3 endianness and D9
identity/edges have distinct messages and raw wire dumps. All these
errors are printed with `dev_err` even when `trace=0`.

Trace prints at most 64 boot/control exchanges per probe (32-byte TX/RX
previews), followed by the existing eight runtime headers. Error dumps
include the entire offending control reply in 32-byte lines, up to 517
bytes. Firmware/MtCl contents are never traced. If stage2 fails, retain
the first tagged mismatch and all its raw lines and stop before stage3.
Do not infer which difference fixed the problem solely from elapsed time.
If it passes, use the stage3 rebind and bounded evdev capture below.

## Offline preparation and provenance

From the repository root:

```sh
scripts/touch/mkinitramfs.sh
scripts/touch/mkinitramfs.sh --check
python3 -B scripts/touch/test-protocol.py
python3 -B scripts/touch/test-control.py
scripts/touch/configure-kernel.sh
scripts/build-kernel.sh --tree touch
cat build/kernel-touch/COMMIT
cat build/kernel-touch/DIRTY
shasum -a 256 build/kernel-touch/Image.gz \
  build/kernel-touch/dtbs/s8003-j71t.dtb build/touch/initramfs-touch.gz
```

**Verified offline:** the firmware stage rejects a changed asset identity,
malformed DATA header/payload checksums, and missing/nonreal 1024-byte MtCl.
The private initramfs contains `apple/j71t-n1.bin` and
`apple/j71t-n1-calibration.bin` in a touch-only overlay on the original HoolockRD.
It is **one gzip member**, with two concatenated newc archives inside.
Never replace it with a base-gzip plus overlay-gzip concatenation.
`test-protocol.py` reads the actual kernel source and runs its C helpers under ASan/UBSan, including the
1008+16 calibration packets and 51,200 malformed report cases.
The control suite tests actual execute/wake/E2/E3/E6/E7 helpers, delays,
bounded corrupt-packet retries, and self-describing raw-packet errors.
Use `python3 -B scripts/touch/verify-build.py --check` to check the shipped
artifact manifest and `patches/touch/HEAD` for the exact kernel revision.

> Private graphics-dependent excerpt omitted from this source snapshot.

Keep the preceding working RAM payload. No NVMe, SysCfg, NVRAM, or boot
firmware storage operation is needed by this test.

## Stage 1: transport and read-only touch identity

The orchestrator assembles and boots through the existing tethered workflow:

```sh
TREE=touch scripts/mkpayload.sh build/touch/initramfs-touch.gz apple_n1.boot_stage=1
```

Once HoolockRD's USB ACM shell is ready:

```sh
scripts/linux-run.sh 'uname -a; cat /proc/cmdline; dmesg | grep -Ei "spi|n1|touch|d2365|defer|firmware|timeout"'
scripts/linux-run.sh 'ls -l /sys/bus/spi/devices; cat /sys/module/apple_n1/parameters/boot_stage; cat /proc/bus/input/devices'
scripts/linux-run.sh 'mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true; cat /sys/kernel/debug/devices_deferred 2>/dev/null; cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | grep -Ei "touch|spi3"'
```

Expected: legacy SPI3 probes, reference-clock and analog suppliers bind,
and `apple-n1 ... HBPP detected; N1 controller 0x... (stage 1)`. The
controller value must be plausible and repeatable, not zero/all-ones.
Stage1 does no RAM firmware/calibration upload and intentionally registers
no touchscreen input device. It reasserts reset and releases its clock/LDO
after the identity check. The boot and shell must remain responsive on a
bad signature or transfer timeout.

If the SPI/touch node is absent or deferred, capture the DT compatible,
supplier error and clock logs before attempting any firmware upload:

```sh
scripts/linux-run.sh 'for d in /sys/bus/spi/devices/*; do echo "$d"; cat "$d/modalias" 2>/dev/null; readlink "$d/driver" 2>/dev/null; done; dmesg | tail -100'
```

**Gate:** advance only after HBPP detection and the actual controller ID
are captured. If the clock/pinmux settings remain unverified, follow the
transport notes; do not substitute J172's clock or GPIO mapping.

## Stage 2: bounded calibration, RAM firmware, descriptors

Use the bound device ID printed by the previous commands (for example
`spi0.0`; Linux bus numbering is not fixed). On the Mac, send this one
bounded shell block through the existing runner:

```sh
TIMEOUT=45 scripts/linux-run.sh 'D=""; for p in /sys/bus/spi/drivers/apple-n1/spi*; do test -e "$p" && D=${p##*/}; done; test -n "$D" || exit 1; echo 2 >/sys/module/apple_n1/parameters/boot_stage; timeout 30 sh -c "echo $D >/sys/bus/spi/drivers/apple-n1/unbind; echo $D >/sys/bus/spi/drivers/apple-n1/bind"; dmesg | grep -Ei "apple-n1|apple-s8000-spi|timeout|stall|lockup"'
```

Expected sequence:

```text
HBPP detected; N1 controller 0x... (stage 2)
uploaded 1024-byte MtCl and 57016-byte C1F15,1 firmware
calibrating controller 0x434d15c1, const-cal=3
calibration transport complete, status 0x...
N1 ready: interface 1, max packet 2454, family ..., 40x30, FW ..., surface 14745x19657
```

The final calibration status is deliberately logged without an invented
success code: Apple's N1 routine checks the transport only. Stage2 checks
the descriptor echoes, checksums, lengths, endianness and surface identity,
then resets/powers down. It still creates no input device.

If probe failed and the device is no longer bound, retain `D` from its
`/sys/bus/spi/devices/` directory and retry only after identifying the failed
stage. Set `boot_stage` as above, then write that explicit ID once to
`/sys/bus/spi/drivers/apple-n1/bind` under the same 30-second timeout.
Do not run an automatic rebind loop. A missing firmware file should return
immediately; no 60-second firmware-class fallback is expected.

**Gate:** firmware ACKs, N1 calibration transport, and the live matching
descriptors must all succeed before stage3. A read timeout is not evidence
that a different packet/byte-order assumption is correct.

## Stage 3: evdev and contact behavior

After stage2 passed, rebind with stage3:

```sh
TIMEOUT=45 scripts/linux-run.sh 'D=""; for p in /sys/bus/spi/drivers/apple-n1/spi*; do test -e "$p" && D=${p##*/}; done; test -n "$D" || exit 1; echo 3 >/sys/module/apple_n1/parameters/boot_stage; timeout 30 sh -c "echo $D >/sys/bus/spi/drivers/apple-n1/unbind; echo $D >/sys/bus/spi/drivers/apple-n1/bind"; dmesg | tail -100; cat /proc/bus/input/devices'
```

Expected: `touchscreen reporting enabled`, then an input block containing:

```text
N: Name="Apple N1 Touchscreen"
H: Handlers=... eventN ...
```

The evdev capabilities include `BTN_TOUCH`, `ABS_MT_SLOT`,
`ABS_MT_TRACKING_ID`, X/Y, touch major/minor, orientation and pressure.
Thirty-two slots are an upper parser allocation, not a claim that this
iPad supports 32 simultaneous fingers. Force at record+26 is used when the
record is at least 28 bytes; shorter records report zero pressure.

Run the bounded BusyBox-only sample and touch/lift fingers during it:

```sh
TIMEOUT=25 scripts/linux-run.sh -f scripts/touch/capture-events.sh
```

For the first capture, bounded tracing is available: before the
stage3 rebind, set `echo 1 >/sys/module/apple_n1/parameters/trace` through
`linux-run.sh`. At most eight checksummed payload headers (32 bytes each)
are logged per probe, in addition to the first 64 boot/control exchanges.
This helps identify an unexpected firmware report
without printing firmware or calibration.

The sample remains in RAM at `/tmp/n1-events.bin`. On arm64 each
`struct input_event` is 24 bytes: 16-byte timeval, LE16 type, LE16 code,
LE32 value. The script prints TYPE, CODE, VALUE_LO, VALUE_HI. Useful hex
event fields:

| Type | Code | Meaning |
|---|---|---|
| 0003 | 002f | ABS_MT_SLOT |
| 0003 | 0039 | ABS_MT_TRACKING_ID; `ffff ffff` is release |
| 0003 | 0035 / 0036 | X / Y |
| 0003 | 0030 / 0031 | touch major / minor |
| 0003 | 0034 | orientation |
| 0003 | 003a | pressure, when force is available |
| 0001 | 014a | BTN_TOUCH |
| 0000 | 0000 | SYN_REPORT |

Expected coordinates before any DT rotation/inversion are X 0..14620,
Y 0..19538, at 100 units/mm. Round3 J71t DT applies Y inversion so
the portrait origin is top-left; earlier captures used raw upward-growing Y. These come from active descriptor edges, not
2048x1536 display pixels; libinput maps the range to the output. Record the
actual direction for top/left/right/bottom before choosing Hyprland's
touchscreen/output transform.

Test one finger, drag to all four corners, lift, two crossing fingers,
staggered lifts, and several fingers up to the controller's observed
capacity. IDs should remain stable across movement; liftoff should emit
tracking ID=-1 and eventually BTN_TOUCH=0. Empty frames and omitted contacts
must release old slots. Inspect `dmesg` for rejected/truncated/duplicate
frames or IRQ shutdown after every sample.

## Error behavior and recovery

```sh
scripts/linux-run.sh 'dmesg | grep -Ei "N1|frame/ATN|IRQ disabled|timeout|stall|lockup"; cat /proc/interrupts | grep -Ei "spi|n1|touch"'
```

**Implemented bounds:** PIO transfer deadline; five upload attempts;
finite clock polls and protocol sleeps; four E2 attempts; one shared
five-attempt descriptor budget; eight-frame IRQ drain; three
consecutive read/ATN errors before disabling IRQ and releasing contacts.
There is no background infinite restart loop. If ATN stays asserted after
eight frames, IRQ is disabled immediately.

For recovery, first capture the failure and stop event readers, then do a
single bounded unbind/rebind as above. Observe reset/probe behavior and
confirm released slots. To test missing firmware handling without another
boot, the orchestrator may temporarily rename a touch firmware file in
the RAM rootfs, perform one bounded rebind and restore the filename before
retrying. Never alter the source copies, same-unit calibration, or any
factory-storage record for a negative test.

Suspend/resume, display-sync coordination, and Hyprland rotation are not
hardware-verified. Preserve the raw descriptors, first successful
HBPP identity, complete dmesg, `/proc/bus/input/devices`, event sample, and
kernel/DTB/initramfs hashes in the orchestrator's test log before promoting
the build into the combined integration kernel.
