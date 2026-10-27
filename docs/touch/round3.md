# Round 3: working touch, portrait orientation and lifecycle

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Implementation head
`6ca4db775649909602763aa6bfd7bcbbe867285a`. No device access was performed by
the touch implementation agent. Device results below are verified in the
orchestrator's saved logs/captures or owner confirmation; new kernel
behavior is labeled separately from those results.

## Device result and captured events

**Verified, orchestrator:** tested touch head `b0270d0bb3` in the integration
kernel including audio (`77fccbcaf19b`) passes HBPP, firmware/calibration,
all descriptors, initial frame drain, input registration and runtime touch.
N1 is `spi1.0`, since audio occupies another SPI bus. Device/event numbering
must be discovered by name, not hardcoded into userspace.

Evidence:

- `docs/test-logs/2026-10-22-touch2-audio-boot.txt`: controller
  `0x434d15c1`; raw register reply `4c 39 15 c1 43 4d 01 66`.
- `docs/test-logs/2026-10-22-touch2-stage2.txt`: upload ACKs, calibration
  status 4969, E2/E3/E6/E7 replies, interface 1, maxpacket 2454, family 21,
  rows40/columns30, FW 0210, D9 surface 14745x19657 and successful exit0.
- `docs/test-logs/2026-10-22-touch2-stage3.txt`: input
  `Apple N1 Touchscreen`, event1, IRQ GPIO65 and reporting enabled.
- `build/touch-events.raw` / `build/touch-events2.raw`: supplied 24-byte
  little-endian arm64 input_event hex records. These stay under build/.

Reproduce the host-only analysis:

```sh
python3 -B scripts/touch/analyze-events.py build/touch-events.raw build/touch-events2.raw
```

Saved summary: `build/touch/round3-events.json`.

| Capture | Events | SYN_REPORT frames | Median frame gap | Maximum gap | X range | Y range |
|---|---:|---:|---:|---:|---|---|
| Corner taps | 157 | 21 | 16.654ms | 983.102ms, between taps | 535–14069 | 250–18645 |
| Continuous drag | 9508 | 1151 | 16.667ms | 299.826ms | 0–13076 | 401–18773 |

**Verified:** neither capture contains SYN_DROPPED. The drag spans
19.967826 seconds between first/last SYN_REPORT. Its metadata records
IRQ count 1039→2199, +1160. This verifies sustained input in the supplied
window; the maximum gap is retained rather than presenting median 60Hz as
an uninterrupted guaranteed rate.

## Confirmed orientation and implementation

**Verified, owner confirmation:** tap order was top-left, top-right,
bottom-left, bottom-right, portrait with the home button at the bottom.
BR was outside the capture window. First positions decoded with the active
ranges are:

| Tap | Raw X/Y | Raw percent | After Y inversion |
|---|---|---|---|
| TL, tracking 0 | 600 / 18509 | 4.10% / 94.73% | 600 / 1029, about 4.10% / 5.27% |
| TR, tracking 1 | 14058 / 18475 | 96.16% / 94.56% | 14058 / 1063, about 96.16% / 5.44% |
| BL, tracking 2 | 870 / 250 | 5.95% / 1.28% | 870 / 19288, about 5.95% / 98.72% |

**Implemented:** commit `6ca4db7756` adds `touchscreen-inverted-y` to the
J71t-only touchscreen node. No X inversion or axis swap is applied. The
existing driver first clamps raw signed coordinates to the active D9 edges
and subtracts the minima; the pinned generic helper then maps
`Y = 19538 - normalized_raw_Y`. Bounds/resolution are unchanged. This
aligns top-left with portrait framebuffer origin without a compositor flip.

**Verified offline:** the actual generic touchscreen transform and N1
parser are exercised together at all four exact corners and an interior
point by `scripts/touch/test-protocol.py` under ASan/UBSan. The staged DTB
contains the Y property and neither X inversion nor swap. **Pending device
verification:** corrected corner output with this new DTB; all previous
captures used the old non-inverted DT.

## Normal boot and missing files

**Verified in source:** `boot_stage` defaults to 3 and trace defaults to
false. Stage3 follows the same power/identity/upload/calibration/discovery/
input path already tested by staged rebind. No stage parameter is needed
for normal use. The cold default-stage3 boot remains an owner-run test.

Both files are loaded/validated before `n1_power_on()`:

```text
apple/j71t-n1.bin                57016 bytes, unchanged C1F15,1 image
apple/j71t-n1-calibration.bin    1024 bytes, unchanged same-unit MtCl
```

**Verified in the pinned kernel:** `request_firmware_direct()` uses
`FW_OPT_NOFALLBACK_SYSFS`. Both driver requests use this API, so missing
RAM files return an error without waiting for the firmware userspace
fallback. Reset is requested asserted and LDO off before the requests.
The missing-file path does no firmware upload or controller power-on.
Actual missing-file latency is not claimed without the owner-run test.

**Implemented:** `0cbca54ab4` adds both MODULE_FIRMWARE declarations so
initramfs tools can discover their names, and explicit invalid-image /
missing-calibration messages. MODULE_FIRMWARE does not embed the Apple
assets; the rootfs/initramfs must still install the private staged copies.
The integration config fragment retains parent INPUT/INPUT_TOUCHSCREEN,
REGULATOR and the suppliers, avoiding the previous Kconfig drop.

The [test plan](test-plan.md) boots without either N1 parameter and supplies
individual five-second missing-firmware/missing-calibration tests on the
RAM rootfs. The script restores hidden files through a trap and does one
normal rebind afterward. It rejects both successful negative probes and
timeout/termination results, including BusyBox timeout variants.

## Input/libinput/Hyprland properties

**Verified source audit:** `n1_probe()` configures the following before
registration; `drivers/input/input-mt.c::input_mt_init_slots()` supplies
DIRECT and copies MT axis metadata to the legacy axes.

| Property | Value / provenance |
|---|---|
| INPUT_PROP_DIRECT | Set by INPUT_MT_DIRECT; no INPUT_MT_POINTER flag |
| BTN_TOUCH | Enabled; managed by MT pointer emulation on frame sync |
| ABS_MT_POSITION_X / ABS_X | min 0, max 14620, resolution100 units/mm |
| ABS_MT_POSITION_Y / ABS_Y | min 0, max 19538, resolution100 units/mm |
| ABS_MT_SLOT | 0–31; allocation limit, not claimed32-finger hardware capacity |
| ABS_MT_TRACKING_ID | Core type-B IDs; liftoff reports-1 |
| Active size from range/resolution | 146.20mm ×195.38mm |
| Orientation | Kernel/DT Y inversion only; ranges and scale preserved |

**Verified prior binary evidence:** classic contact conversion at
MultitouchSupport `1f48f3b1c` divides signed X/Y and ellipse axes by100 for
physical-coordinate fields, while separately normalizing positions by D9
active edges. Physical-mm accessors are named in the extracted framework
symbol table. D9 reports physical 14745×19657 and signed edges
-112..14508 / -112..19426. A bounded scalar-key search of J71t.mtprops
found no explicit resolution/size/mm override; resolution is derived from
the matched classic protocol, not fabricated from display pixels. See
[runtime notes](runtime-notes.md), D9 bounds and contact conversion.

**Implemented, not yet run on the iPad:** the static read-only
`build/touch/device/n1-input-info` tool queries EVIOCGPROP, EVIOCGBIT and
EVIOCGABS to verify this contract on both legacy and MT axes. It performs
no event reads, device grabs or configuration writes. Build it with
`scripts/touch/build-input-info.sh`; the owner-run lifecycle script invokes
it if installed at `/tmp/n1-input-info`. Plain sysfs/proc checks remain
available without this tool; expected properties bitmap is2.

**Userspace boundary:** libinput also requires udev ID_INPUT and
ID_INPUT_TOUCHSCREEN properties; the kernel DIRECT flag is not a substitute
for running the normal udev input classifier. This is documented by
[libinput's static device configuration](https://wayland.freedesktop.org/libinput/doc/latest/device-configuration-via-udev.html).
No custom classification rule has been added without an actual failure.
Hyprland documents touchdevice output auto-detection and a separate input
transform in its [touchdevice options](https://wiki.hypr.land/configuring/core/config-options/#touchdevice).
For this portrait DT, keep the extra transform at identity. If output
selection fails, map to the actual connector name from `hyprctl monitors`.
The Omarchy workstream owns its userspace configuration. Udev/libinput/
Hyprland recognition and final display mapping still need the documented
userspace capture; they were not tested in HoolockRD's raw evdev session.

## Clean unbind and shutdown; suspend boundary

**Implemented:** `0cbca54ab4` explicitly frees/synchronizes the managed IRQ
before releasing contacts and shutting off the controller, and registers
the same cleanup for SPI shutdown. IRQ ownership is written only by probe/
remove; it no longer depends on an IRQ-thread-written enabled flag. This
removes the possibility of racing that flag during unbind or adding an
extra disable-depth count when the error path already masked the IRQ.

Order: free/synchronize IRQ → release all slots and BTN_TOUCH → assert
reset → display-sync off → reference clock off → LDO off → analog off.
The managed IRQ action is removed before its buffers/input/power suppliers
can be released. Diagnostic stage1/2 removes and repeated cleanup are safe.
New binding allocates fresh protocol/IRQ state and uploads RAM firmware.

**Verified offline:** actual C remove/contact-release/power-off functions
pass ordering and repeated-stop tests under ASan/UBSan, with both active
and already masked IRQ models. This validates the driver's order; real
concurrent IRQ behavior and physical power transitions require the
owner-run unbind/rebind test. Existing stage1→stage2→stage3 rebinds passed
before this cleanup change.

**Not implemented/verified:** full system suspend/resume. The task's clean
unbind alternative is implemented here. Unbind before a platform suspend
experiment; rebind after platform resume, contingent on the platform
providers returning correctly. No untested PM callback claims to reload
a controller through unverified parent clock/power transitions.

## Build, staging and next test

**Verified:** clean head `6ca4db7756`, full arm64 LLVM Image.gz plus 104 DTBs,
four linked probe symbols, Y-only DT property, firmware metadata and all
integration-fragment options. Nine exported patches exactly reconstruct
HEAD from BASE. Round3 delta is patches 0008/0009. Merge preflight against
the observed integration ref is clean without modifying that tree.

Artifacts and hashes: `build/touch/artifacts.json`. Firmware and the
single-stream `build/touch/initramfs-touch.gz` are unchanged. The input-info
helper is shipped separately; the existing combined initramfs may be reused.
No repo commit, integration-tree mutation or device access was performed.

Run the Round3 block at the top of [test-plan.md](test-plan.md), then capture
normal cold boot, four corners, read-only input metadata, one clean cycle,
and the two missing-file cases. Those are the remaining hardware acceptance
checks for this revision; suspend/resume remains outside the claimed scope.
