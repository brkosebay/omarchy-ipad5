# A9 display, round 3: boot-time KMS driver

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Kernel branch: `a9/display`, tip `1b83789aa87469552a06d46e70b48e9dccf2b71e`.
Base: round 2b, `9d776859c0dfb28c6a59458c2ba47186cc027199` (series in
`../round2/`). Apply after round 2. `SHA256SUMS` covers the patch.

- `CONFIG_DRM_APPLE_ADBE=y`: replaces simpledrm during boot. A
  simple-framebuffer device that simpledrm has not bound yet is retired
  too, so simpledrm cannot come back later in the boot.
- Kill switch: `apple_adbe_drm.enable=0` keeps simpledrm.
- `0x400` (packet applied at the frame boundary) is the documented
  completion. The vblank mask is VBI | `0x400`, and `0x8` is only counted.
  Flip events carry the vblank sequence and timestamp of the boundary
  where the flip was applied.
- The latched VBI bit is cleared before vblank is enabled; otherwise the
  first VBI interrupt was a stale boundary (round 2b: 64 interrupts for 63
  frames).
- Each completion is counted once (this fixes the "36 kicks, 35
  completions" DPMS reading, which was the park path's uncounted `0x400`).
  debugfs reports in-flight packets and a flip-latency histogram.
- The probe is no longer auto-loadable (no `MODULE_DEVICE_TABLE`).

No DTS change. Offline: incremental `W=1` build with no warnings, isolated
`W=1` recompile of `drivers/gpu/drm/adbe/` clean, built-in parameter
present, host test, schema checks, strict checkpatch 0/0/0, patch/tree
identity, clean merges against the stack. No device tests were run by this
round.

Build: `scripts/display/build-round3.sh`. Export:
`scripts/display/export-round3.sh`. Device procedure:
`docs/display/d1-d2-test-plan.md`, section "Round 3". Visual tear test:
`scripts/display/tear-test.html`.
