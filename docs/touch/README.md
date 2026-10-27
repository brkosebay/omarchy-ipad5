# J71t N1 touch — Round 3 handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Touch works.** The orchestrator verified all three stages and sustained
runtime input with `b0270d0bb3` in the integration kernel including audio.
The new Round3 head **`6ca4db775649909602763aa6bfd7bcbbe867285a`** is built
and staged with the confirmed portrait Y correction and tighter cleanup.
These new changes still need the owner-run acceptance tests.

Full report: [round3.md](round3.md). Exact next commands:
[test-plan.md](test-plan.md). Protocol: [n1-protocol.md](n1-protocol.md).
No device access, integration-tree changes, or workspace-repo commits were
performed by this workstream; only its kernel branch was committed.

## Verified device results

- HBPP controller `0x434d15c1`; N1 is now `spi1.0` with audio present.
- Firmware/MtCl upload, calibration transport, E2 and all descriptors pass:
  interface 1, maxpacket 2454, family 21, 40x30, FW 0210, surface 14745x19657.
- `Apple N1 Touchscreen` registers as event1; frame reads and evdev work.
- Supplied continuous-drag capture: **9508 events / 1151 frames** in
  19.967826 seconds, median 16.667ms, max 299.826ms, no SYN_DROPPED; IRQ+1160.
- Owner confirmed TL/TR/BL corner order with home button at the bottom:
  X agrees with the portrait framebuffer, raw Y grows upward.

Logs are `docs/test-logs/2026-10-22-touch2-*.txt`; captured events stay in
`build/touch-events*.raw`. Reproducible analysis is
`scripts/touch/analyze-events.py`, summary `build/touch/round3-events.json`.
The Round2 timing correction set works on hardware; the individual effect
of zero word gap versus restored waits/retries was not isolated.

## Round 3 changes

`0cbca54ab4` synchronizes/frees the managed IRQ before releasing contacts
and power, including after an IRQ error already masked the line. It uses
ownership state confined to probe/remove and registers the same cleanup
for shutdown. It also advertises both firmware names to initramfs tools
and diagnoses invalid firmware/missing calibration explicitly.

`6ca4db7756` adds **`touchscreen-inverted-y` unconditionally for J71t**.
The generic helper maps Y to `19538 - normalized_raw_Y`; X stays unchanged.
The corrected DTB must accompany the kernel. No userspace Y flip is needed
for the portrait output.

Normal boot already defaults to **stage 3, trace off**: no N1 parameters
are required. Both firmware requests use the no-fallback direct API before
controller power-on. Actual cold default-stage3 boot and missing-file
latency remain owner-run checks, with exact scripts in the test plan.

**Verified source properties:** DIRECT=1, POINTER=0, BTN_TOUCH, type-B
slots/tracking IDs, X 0..14620 and Y 0..19538 at 100 units/mm, with identical
legacy ABS_X/Y metadata. The original classic parser converts physical
coordinates from hundredths of a millimetre; the active area therefore
reports 146.20×195.38mm. Userspace udev/libinput classification and Hyprland
output mapping still need their own capture. The static read-only
`build/touch/device/n1-input-info` helper audits the actual evdev properties.

## Build and artifacts

**Verified:** full arm64 LLVM Image.gz and 104 DTBs build from clean
`a9/touch`; four driver probe symbols, firmware metadata, Y-only DT
orientation and every option in `patches/touch/touch.config` are checked.
Nine patches are exported with BASE/HEAD/series. Patches 0008/0009 are the
Round3 delta, and full-series application exactly reconstructs HEAD.
Merge preflight against the observed integration ref passes without
modifying its checkout or branch.

Artifacts are in `build/kernel-touch/` and `build/touch/`; exact sizes and
SHA256s are in **`build/touch/artifacts.json`**. Image SHA256:
`ad8607028fc9136ca19bc63eac67a895d494b655fcb8df7a245eeebf2d27d983`.
J71t DTB SHA256:
`1aa9a63802f14d687fbb5cb96efff20c3127e1080a1bce2973d3d09e4d1354d6`.

Firmware and same-unit MtCl are unchanged private files under
`build/firmware/lib/firmware/apple/`. The touch archive remains one gzip
stream with HoolockRD and the two firmware files; existing combined
`build/initramfs-fw.gz` may be reused. MODULE_FIRMWARE declares names and
does not embed or replace the Apple assets. Input-info is shipped separately.

Reproduce offline:

```sh
scripts/touch/build-input-info.sh
scripts/touch/configure-kernel.sh
scripts/build-kernel.sh --tree touch
python3 -B scripts/touch/test-protocol.py
python3 -B scripts/touch/test-control.py
python3 -B scripts/touch/test-lifecycle.py
python3 -B scripts/touch/verify-build.py --check
scripts/touch/export-patches.sh
```

Actual C helpers pass ASan/UBSan tests for firmware/calibration, descriptor
wire fixtures (including the captured 12-byte D3/E7 report), IRQ bounds,
all four portrait corners through the real generic transform, teardown
ordering with active/already masked IRQs, repeated cleanup, and 51,200
malformed packets. Updated N1 source passes checkpatch with 0 errors/warnings.

## Next acceptance test and scope

Merge the two commits, keep the updated config fragment, and assemble the
normal payload without N1 stage/trace arguments. Capture cold boot, all
four corners, the input-info output, one clean unbind/rebind, and the two
individual missing-file cases on HoolockRD's RAM rootfs. Commands and
expected output are at the top of [test-plan.md](test-plan.md).

**Suspend/resume remains unimplemented and unverified.** This round
implements the task's clean-unbind alternative and shutdown cleanup.
It does not claim that platform clock/power providers resume correctly.
New orientation/cleanup behavior and userspace display mapping require
the next device test; the preceding driver already supplies working evdev.
