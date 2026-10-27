# AOP accelerometer → iio-sensor-proxy → Hyprland

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25. Host implementation only; **no device access, install, boot,
default promotion, workspace commit or push was performed in this session**.
The orchestrator has established streaming on `a9/aop-stage3` `3a7b3805b3`,
entry `aop-stage3-coldaccel-3a7b3805b3`. Warm shutdown `aa54566a91` still
requires the two-boot test in [aop-stage3.md](aop-stage3.md).

## Driver compatibility: already implemented

`scripts/periph/aop/kernel/a9-aop-iio.c`, identical to the kernel branch,
already exports:

- `name=apple-a9-aop-accel`, signed `in_accel_{x,y,z}_raw`, shared
  `in_accel_scale=0.000149638` m/s²/count and read-only 10 Hz sampling frequency.
  Reads become `-ENODATA` after one second without a report.
- `in_accel_mount_matrix`, via `IIO_MOUNT_MATRIX` and
  `iio_read_mount_matrix(parent, ...)`. The property belongs on
  `/soc/aop@210800000`, the **parent platform device**, in the Linux DT.
  Missing DT property currently gives identity. This is metadata: the driver
  leaves raw axes unchanged. Userspace applies the matrix once.
- A managed triggered buffer, XYZ signed native-endian 32-bit scan channels,
  mask `0x7`, timestamp scan index 3, and the own trigger
  `apple-a9-aop-accel-devN`. The worker calls `iio_trigger_poll_nested()`
  for each report while buffering is enabled. XYZ plus the aligned 64-bit
  timestamp is 24 bytes on this little-endian arm64 board.

No new trigger, buffer or raw ABI implementation is needed. No behavior
change or speculative matrix was committed to `a9/aop-stage3`.

The upstream [3.9 udev rules](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/blob/3.9/data/80-iio-sensor-proxy.rules)
recognize raw XYZ as `iio-poll-accel` and scan XYZ as `iio-buffer-accel`,
then add `TAG=systemd` and `SYSTEMD_WANTS=iio-sensor-proxy.service`.
`ID_INPUT_ACCELEROMETER` is for the input-event backend, not this IIO device.
Do not add an input accelerometer tag or grant everyone access to `/dev/iio:*`.

The [buffer utilities](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/blob/3.9/src/iio-buffer-utils.c)
find exactly the trigger name this driver supplies (or `<name>-trigger`),
enable scan channels and the buffer, and probe for data for 500 ms.
The [polling backend](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/blob/3.9/src/drv-iio-poll-accel.c)
can use the raw attributes if a usable buffer is unavailable. Both operate
at roughly 700 ms orientation updates; neither requires writable frequency
when the sensor already reports 10 Hz. A buffer is not required to expose
an accelerometer through the proxy, and a failed buffer probe must be
investigated separately from raw streaming.

The proxy's [matrix setup](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/blob/3.9/src/accel-mount-matrix.c)
prefers `ACCEL_MOUNT_MATRIX`, then `mount_matrix`,
`in_accel_mount_matrix`, `in_mount_matrix`. It multiplies rows by the XYZ
column vector. The installer uses a name-matched udev property while the
DT matrix is unmeasured. An x86 DMI-based `sensor:modalias:...` hwdb entry
will not identify this OF platform device; no hwdb entry is necessary here.

## Panel and mount-matrix evidence

The native framebuffer is **1536 × 2048 portrait, home button at the bottom**.
[hyprland.md](../omarchy/hyprland.md) records the console geometry and physical
orientation checks; `rootfs/hypr/ipad.lua` records transform 3 with the home
button on the right, verified previously on the iPad. The panel's native
orientation is independent of the owner's default landscape desktop.

The static `docs/adt/j71t.txt:1888` has
`accel-orientation: "syscfg/ARot"`, a reference, **not calibration bytes**.
The already-pulled live `build/firmware/private/IODeviceTree.txt:919..936`
contains the real accel node and:

```text
accel-orientation = 01000000 ff000000 00ff0000 0000ff00
```

There is no separate numeric panel rotation in that accel node. A plausible
decode is LE version 1 followed by three padded signed-byte rows, yielding
`-1,0,0;0,-1,0;0,0,-1` (`-I`). It has determinant -1: it can encode a sign
convention change and must not be assumed to be a proper spatial rotation.
[ds-aop-accel.md §8.1](ds-aop-accel.md#81-arot) explicitly leaves this decoding
and the report-to-screen mapping unproven.

The pinned firmware **rejects property 0x18** before examining its payload;
the successful capture has `arot_sent=0`, `cal_unsupported=1`, `cal_state=2`.
The AOP did not apply this blob. See [the streaming state](../release/OMISSIONS.md)
and [dmesg](../release/OMISSIONS.md).
The supplied capture
`build/periph/aop-stage23/stage3-calstate-full-tilt-20261025T190705Z/iio.txt`
has `(132,21,-64695)`, approximately `(0.020,0.003,-9.681)` m/s², screen up.
That establishes the Z sign for this pose. Its saved ring samples do not
label portrait/landscape poses and cannot establish the X/Y mapping.

[Core Motion's device axes](https://developer.apple.com/documentation/coremotion/cmmotionmanager)
use device coordinates (portrait X right, Y toward the top, Z out of the
screen), rather than rotating the sensor axes when the UI rotates. Its
[accelerometer data](https://developer.apple.com/documentation/coremotion/getting-raw-accelerometer-events)
includes gravity and is expressed in g. A screen-up negative Z is consistent
with Core Motion-style gravity values. **It does not prove these raw AOP
reports already have the Core Motion frame**, nor establish X/Y signs.

Use the actual proxy [orientation/tilt classifier](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/blob/3.9/src/orientation.c)
as the acceptance frame: `-Y=normal`, `+Y=bottom-up`, `+X=left-up`,
`-X=right-up`, `+Z=face-up`. The previous direct-IIO fallback used the opposite
X/Y signs; this implementation fixes it to agree with the proxy.

**`-I` is a trial hypothesis, not a measured mount matrix.** Flipping only Z
also fits the supplied flat reading. If `-I` is right, six held poses predict:

| Physical pose | Dominant raw count, predicted for `-I` | Mounted result | Desktop/touch transform |
|---|---:|---|---:|
| Flat screen up | Z ≈ -65536 | +Z, face-up | retain previous |
| Flat screen down | Z ≈ +65536 | -Z, face-down | retain previous |
| Home bottom, vertical | Y ≈ +65536 | -Y, normal | 0 |
| Home right, vertical | X ≈ +65536 | -X, right-up | 3 |
| Home top, vertical | Y ≈ -65536 | +Y, bottom-up | 2 |
| Home left, vertical | X ≈ -65536 | +X, left-up | 1 |

The six-pose tool derives all signs and permutations from **labelled physical
poses**, not from this hypothesis. It requires 20+ readings/pose from one
boot, gravity within 20%, per-axis standard deviation below 0.08 g, and a
unique signed permutation fitting every pose within 0.2 g. Capture while
stationary and nearly vertical for the four edge poses. A failed fit means
repeat the labelled captures or investigate calibration; do not force `-I`.
The fitted matrix does not establish metrology-grade scale or bias accuracy.

## Install and test on the running NVMe root — orchestrator commands

Do this after the existing stage-3 cold trigger has reached streaming.
These commands do not reboot, reload AOP, write protected storage or touch
the host's NBD server. Use the usual `OMARCHY_HOST`/`OMARCHY_USER` if required.

```sh
# Raw six-pose capture; prompts on the Mac before each three-second hold.
POSES="build/periph/aop-autorotate/poses-$(date -u +%Y%m%dT%H%M%SZ)"
scripts/omarchy/capture-aop-orientation.sh "$POSES"
cat "$POSES/matrix.json"
# Install package, shared measured matrix, daemon/unit, and paired Lua config.
scripts/omarchy/install-aop-autorotate.sh --matrix-file "$POSES/matrix.json"
```

For an explicit provisional experiment before the six-pose measurement:
`scripts/omarchy/install-aop-autorotate.sh --trial-adt` installs the documented
`-I` hypothesis and saves `validated:false`. Replace it with the measured
file afterwards. Do not use that trial as evidence for a kernel DT change.

The installer runs `pacman -S --needed --noconfirm iio-sensor-proxy`
([Arch Linux ARM package](https://archlinuxarm.org/packages/aarch64/iio-sensor-proxy),
3.9 at inspection), reloads/triggers udev and restarts the **static**
`iio-sensor-proxy.service`. Do not `systemctl enable` that D-Bus/udev-activated
system service. It enables/restarts `ipad-autorotate.service` in the user
session with an explicit `--source proxy` drop-in so acceptance cannot
silently pass using only direct sysfs reads.
The same AOP-specific drop-in clears the base unit's login-time IIO path
condition: registration after graphical login is then handled by the running
monitor/reconnect loop, without requiring another login.

It installs the same matrix in a device-specific udev rule and
`/etc/linux-a9/aop-accel-matrix` for the direct fallback. It backs up replaced
user configuration, restores the standard four name mappings, removes a
user `mount_matrix` override and a fixed `touch_transform` override, and
retains timing settings and the rotation lock. `ipad-rotate` updates the
state file, then reloads the Lua configuration; **both** `hl.monitor` and
`input.touchdevice.transform` consume the same transform. No obsolete
`hyprctl keyword monitor` syntax or competing `iio-hyprland` daemon is added.

For an image/chroot build, copy the same sources and confirmed `matrix.json`
and run, as root inside the mounted rootfs:

```sh
AUTOROTATE_CHROOT=1 bash /path/to/rootfs/autorotate/install.sh \
  linux /path/to/rootfs /path/to/matrix.json
```

The existing general image builder still installs the daemon as before;
the per-device matrix is deliberately not guessed into generic rootfs files.

Discovery and D-Bus checks:

```sh
scripts/omarchy/ssh.sh 'pacman -Q iio-sensor-proxy; systemctl --no-pager status iio-sensor-proxy; for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat "$d/name")" = apple-a9-aop-accel ] || continue; echo "$d"; cat "$d/in_accel_mount_matrix"; udevadm info -q property -p "$d" | grep -E "IIO_SENSOR_PROXY_TYPE|ACCEL_MOUNT_MATRIX|SYSTEMD_WANTS|TAGS"; done'
scripts/omarchy/ssh.sh 'busctl --system get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy HasAccelerometer; ipad-autorotate --status; systemctl --user cat ipad-autorotate'
# Interactive; Ctrl-C when finished. This client holds a D-Bus claim.
scripts/omarchy/ssh.sh 'monitor-sensor --accel'
```

Require `HasAccelerometer = true`, both upstream IIO type tags, the installed
matrix property, `source: proxy` in daemon status and the expected orientation
names for the four edge poses. Sysfs may still show identity on the old
kernel: the udev property has precedence and corrects it in userspace.

The touch driver's SPI driver name is `apple-n1`, OF match is
`apple,j71t-n1` (the Apple ADT names it `multi-touch,j72`), and its **input
name** is `Apple N1 Touchscreen` (`drivers/input/touchscreen/apple_n1.c:938`).
It exports `ABS_MT_POSITION_X/Y`, `INPUT_MT_DIRECT`, with
`touchscreen-inverted-y` in `s8003-j71t-touch.dtsi`; it already supplies
portrait display coordinates. Hyprland typically normalizes the input name
to `apple-n1-touchscreen`; use `hyprctl devices -j` to check, not the SPI
driver's name as a guessed compositor identifier. The existing global
touchdevice setting handles the one internal touchscreen.

```sh
# Full end-to-end test: flat, portrait, both landscapes, upside down, lock.
# Prompts stream to the terminal; HOLD is the time to move/hold each pose.
HOLD=15 scripts/omarchy/test-autorotate.sh --hardware \
  --shots build/periph/aop-autorotate/physical-shots
# This additional mock test exercises lock/debounce/manual rotation/OSK;
# it is not a sensor or matrix validation.
scripts/omarchy/test-autorotate.sh --mock
scripts/omarchy/ssh.sh 'ipad-rotate lock off; ipad-autorotate --source iio --probe 15'
```

At each physical hold require transforms **0/3/2/1** for home
bottom/right/top/left respectively, and tap **all four corners plus the
center** in an app. A screenshot/JSON transform match cannot prove touch
geometry. Check two fingers/pinch and gesture directions in both landscapes.
Leave the tablet flat: the daemon must retain the previous orientation.
The physical test restores the prior orientation and lock afterwards;
`ipad-rotate lock off` explicitly enables following for normal use.

Optional independent buffered-IIO test (close other `monitor-sensor` clients
first; hold stationary). This tests the real trigger and scan packing:

```sh
scripts/omarchy/ssh.sh 'systemctl --user stop ipad-autorotate; sudo -n systemctl stop iio-sensor-proxy'
scripts/omarchy/ssh.sh 'sudo -n python3 -' < scripts/omarchy/test-aop-buffer.py
# Restore these even if the preceding test fails.
scripts/omarchy/ssh.sh 'sudo -n systemctl start iio-sensor-proxy; systemctl --user restart ipad-autorotate'
```

Require ten 24-byte XYZ+timestamp scans with increasing timestamps. The test
restores scan settings and leaves the buffer disabled. The proxy journal
can show the selected backend with a temporary service drop-in:
`[Service] Environment=G_MESSAGES_DEBUG=all`, restart, claim the sensor,
then `journalctl -b -u iio-sensor-proxy`. Remove the drop-in afterwards.
If buffer discovery fails but raw reads work, an explicit diagnostic rule
at `/etc/udev/rules.d/90-linux-a9-aop-poll.rules` may set
`SUBSYSTEM=="iio", ATTR{name}=="apple-a9-aop-accel", ENV{IIO_SENSOR_PROXY_TYPE}="iio-poll-accel"`.
Reload/trigger/restart as above. This is a polling isolation test; the normal
installer does not mask a buffer failure by forcing it.

## Kernel integration prepared; automatic/default mode is a separate change

New branch **`a9/aop-nextint-prep`**, from clean nextint
`db6d504a412b2c63d565a69a51fb16378b9d74c6`:

- `c2d706d221`: cherry-pick of kexecfix `6d449cccb3` (late-port deferral).
- `37f3c73ba5`: cherry-pick of kexecfix `a412bc8f04` (powered reset), resolved
  against nextint's shared sleep helper. Powered retention is gated on the
  fatal/kexec path; nonfatal suspend retains its existing full-off behavior.
- **`2318526cd7`**: stage-3 AOP sources/headers/bindings and exact firmware/AFK
  reservations from `aa54566a91`, preserving nextint's other drivers,
  SEP/Kconfig, USB/audio/periph DT changes and PMGR API. IIO and active source
  are byte-identical to stage3. All active modes remain opt-in.

Reproduce with `scripts/periph/aop/prepare-nextint.sh`. The deterministic
conflict resolution is in `resolve-nextint-kexec.py`; it accepts only that
one expected file/conflict. Exports under `build/periph/aop-autorotate/`:
`nextint-prep.patch`, `nextint-COMMITS`, `nextint.config`, `nextint.dtb`,
`nextint-prepare.log`, `nextint-w1.log`. This is an integration preparation,
**not a new booted kernel, complete Image build, or automatic startup patch**.
The shared `nextint`/`tri` and `aop-stage3` branches were not advanced.

Verified: warning-free `W=1` AOP observer/active/IIO and PCIe objects, all DTBs,
static no-map/AFK/header reservations, 23 production-C ANS handoff tests,
two powered-reset tests and the existing suspend stop/rollback helper test.
The same production sleep fixture also passes with `ans_keep_power=1`,
including its rollback cases (`test-nextint-sleep.py`).
The C replay harness emits unused-stub warnings; the kernel W=1 log has none.

Before making stage 3 the normal boot behavior:

1. **Prove the warm contract first.** Run both full boots and the retained
   shutdown check in [aop-stage3.md](aop-stage3.md#exact-owner-run-cold--full--same-entry-warm-kexec--full-test).
   The existing `aa54566a91` `.shutdown` cannot veto kexec. A missing/failed
   quiesce must not be converted into unconditional automatic cold release.
2. **Introduce a board-specific production mode**, selected by a documented
   DT compatible/property and Kconfig, with a per-device mode enum. Keep
   observer/experiment modes mutually exclusive. Do not achieve default-on
   by assigning global defaults `cold_test=5`, `cold_defer=1`, `cold_run=2`:
   this retains the experiment's semantics and sysfs trigger dependency.
3. **Move the successful invariants into that mode:** pinned firmware hashes,
   real private ECID, zero NONC, the verified D-cache-off quirk, normal IOP
   masks, exact no-map firmware and 4 MiB monotonic grant pool, all advertised
   endpoints, three accepted calibrations/optional ARot, mandatory `3d=2`
   before rates, and IIO only after a first report. Keep the finite timeouts,
   stale-data behavior and failure diagnostics. No extra retry/reset/cycle.
4. **Replace manual deferred release** with one asynchronous startup after
   the complete firmware set is available and PMGR/reservation invariants
   pass. Test the changed startup timing explicitly. The private firmware
   must be present on NVMe, and in the initramfs if startup may precede
   switch_root; do not depend on `/lib/firmware` from another root.
5. **Certify warm provenance in reserved RAM**, not NVRAM: a versioned,
   consumed-once handoff record written valid only after all quiesce ACKs,
   unchanged grant hashes, halt and FIFO drain. Allocate its space explicitly
   before ext/grants; update allocator and reservation tests. On a kexec
   boot, absent/stale/failed certification means no automatic AOP start,
   with a visible error. It does not authorize replay of old AFK mappings.
   Prove cold-boot handling and crash/kdump exclusion separately.
   Untested implementation candidate, for the sleep (no-halt) variant:
   `a9/aop-handoff` `621fe3f144`, `aop_handoff=sleep`. It uses the pool tail
   page, so the cold layout is unchanged, and treats auto-boot 0 as a cold boot
   (see [aop-stage3.md](aop-stage3.md#opt-in-sleepwake-handoff-aop_handoffsleep-shutdown-transcript-full-crashlog)).
6. **Check suspend/resume and shutdown ownership.** Pause/restore the sensor
   stream and invalidate stale IIO reads as required, retain reserved RAM,
   and handle watchdog timing. Add production-mode tests for missing files,
   first-report timeout, failed quiesce, two consecutive warm boots and
   suspend/resume with proxy claims. The present driver is not evidence of
   a complete PM lifecycle.
7. **Put the measured matrix in the board DT**, on `aop@210800000`, e.g.
   `mount-matrix = "-1", "0", "0", "0", "-1", "0", "0", "0", "-1";`
   **only if** the six poses confirm that exact result. Correct the binding's
   current "residual after ARot" description to cover unapplied ARot/full
   report mapping too. Keep raw values untransformed. Retire the temporary
   udev/system-file overrides after verifying sysfs-only parity.
8. Build a complete trial Image/DTBs with debug info off, retained kexecfix,
   ramoops and recovery. Keep **`pcie_apple_h9p.ans_keep_power=1`** and existing
   NVMe write-window/root parameters. Strip `aop_force_cold`, `aop_cold_test`,
   `aop_defer`, `aop_run`, `aop_quiesce` and `apple_a9_aop.attach_stage` from
   the production entry; require the final m1n1 cmdline **<1000 bytes**.
   Inspect the extracted payload and require automatic streaming with those
   knobs absent, cold + both warm tests + physical rotation + suspend tests.
   Stage a trial entry first using the existing boot tools; promote with
   `linux-a9-kexec set-default ENTRY` only after those tests pass and retain
   the previous recovery entry. No automatic promotion is part of this work.

The automatic-mode portion is deliberately a plan: current evidence proves
deferred cold streaming, not safe automatic startup across warm boot and
suspend. The prepared branch makes the already-tested code reviewable in
nextint without claiming those additional hardware results.

## Reproduce host checks

```sh
python3 -B scripts/omarchy/qemu/test-autorotate.py
python3 -B scripts/omarchy/qemu/test-aop-orientation.py
scripts/omarchy/qemu/test-aop-install.sh
scripts/periph/aop/prepare-nextint.sh
scripts/build-kernel.sh --run aop-nextint-prep 'python3 -B /work/scripts/boot/test-ans-keep-power-offline.py --source /src/wt/aop-nextint-prep'
scripts/build-kernel.sh --run aop-nextint-prep 'python3 -B /work/scripts/boot/test-ans-handoff-offline.py --source /src/wt/aop-nextint-prep'
scripts/build-kernel.sh --run aop-nextint-prep 'python3 -B /work/scripts/power/test-s2idle-round1.py --boot /src/wt/aop-nextint-prep SleepTests.test_port_certificate_guard_stop_and_rollback'
scripts/build-kernel.sh --run aop-nextint-prep 'python3 -B /work/scripts/periph/aop/test-nextint-sleep.py --boot /src/wt/aop-nextint-prep'
```

All workspace changes remain uncommitted. Kernel commits exist only on the
new `a9/*` branch. No protected storage or QEMU process is involved.
