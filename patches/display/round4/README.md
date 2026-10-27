# A9 display, round 4: buffer accounting for the soak leak

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Kernel branch: `a9/display`, tip `04ca5aa8f24f4c6cca628dd4e0619146289ec019`.
Base: round 3, `1b83789aa87469552a06d46e70b48e9dccf2b71e` (series in
`../round3/`). `SHA256SUMS` covers the patch.

No leak was found in the driver. It takes no framebuffer or GEM
references of its own; the atomic helpers own every buffer. All four flip
completion paths (interrupt, VBI fallback, 100 ms timer, CRTC disable)
only move the pending event and balance their vblank reference. The
analysis is in `docs/display/d1-d2-test-plan.md`, "Round 4".

What this patch adds (debugfs only, no behaviour change):

- Every GEM object is tracked: dumb buffer (allocated here) or PRIME
  import (another device's memory, e.g. a decoder buffer), its size, whether
  it is in the CMA area, its live CPU mappings and its creator.
- `adbe_state` gains three kinds of line:
  `gem dumb=N/K import=N/K dumb-peak=K orphans=N/K mmaps=N dumb-in-cma=K
  import-in-cma=K fbs=N cma-free=K`, `vblank refcount=… enabled=… vbi=…
  vbl-events=… flip-pending=… timer=…`, and one `client COMM[PID]
  master=… fbs=… dumb=N/K import=N/K` line per open DRM file.
- New `adbe_bos` file: one line per live object, oldest first.
- Orphans are objects with no handle left, which only a framebuffer, a
  mapping, a dma-buf export or the kernel's fbdev client keeps alive.
  `dumb-in-cma` below `dumb` means CMA was full and the buffer came from a
  high-order page block instead. The CMA accessors are not exported, so a
  modular build prints `?` there.

No DTS or config change. Offline: incremental `W=1` build with no warnings,
full Image/DTB/module build of the tree (`scripts/display/build-round3.sh`,
unchanged), strict checkpatch 0/0/0, patch/tree identity, clean `git
merge-tree` against `a9/nextint`. No device tests were run by this round.

Export: `scripts/display/export-round4.sh`. Device procedure and sampler:
`docs/display/d1-d2-test-plan.md`, "Round 4", with
`scripts/display/churn-r4.sh`.
