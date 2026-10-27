# A9 AOP inherited ring reader

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Base: `b0fbdf663609d05c1523a4d61f631eea43867471` (Round 7, `a9/aop`).

Commits, in series order:

1. `b8a4acf3a1` — optional early reserved-memory hint binding.
2. `286ae86efe` — bounded zero-traffic discovery, allocation diagnostics and
   opt-in reserved-ring read-only IIO, documentation and maintainer coverage.
3. `d638469c63201f26a9c2579f861680a84543a28c` — skip unreadable/protected
   pages, report coverage and skipped ranges, add opt-in postcore capture.
4. `9f0ddbb8943fc12cae5e052f0f4b9188de6e4af9` — opt into reserved linear-mapped
   DRAM discovery and report metadata-backed per-hit range classes.
5. `c225239c9069b600b2d5a3339dc6b29ff10b9bd3` — exact iOS-probed PA/size
   lists, frozen postcore byte dumps, offset signatures and one-second diffs.
6. `906da12f51cbd53bf8bc13df196281983f3fe5de` — independent probe-time
   PMGR-guarded AOP state line, frozen in debugfs; no writes or FIFO traffic.

The export script replayed all six patches into an isolated index and verified
that the resulting tree equals committed HEAD. `SHA256SUMS` covers the patches
and `series`. No push or workspace commit was made.

Validation passed: full arm64 LLVM=1 W=1 Image.gz/DTBs; final changed-source
W=1 compilation; E1 with IIO disabled/modular and no IIO dependencies;
ASan/UBSan parser and production-loop fault-injection tests; DT hint and actual kexec FDT merger tests; binding
schema; linked-symbol/gate checks; checkpatch (0 errors, 0 warnings per patch).
The full upstream build emits existing non-AOP warnings. Logs and artifacts:
`build/periph/aop-ring-reader/` and `build/kernel-aop/` in the workspace.

E1 cannot prove pages were never allocated; it reports current lockless state
and warns about candidates in Linux allocator RAM. E2 requires an explicitly
selected, early no-map DT reservation and fresh reports. Nothing writes shared
rptr: the original producer may fill the ring and stop. DMA coherency, live
streaming, physical scale and screen orientation require device validation.

On the user-reported warm-kexec scan, the previous reader stopped at
`0x8035f0000` after 56557568 bytes with EFAULT. Patch 3 quarantines that page
and continues. Tests inject this exact fault and find a later synthetic ring.
Format 2 reports readable/skipped/unvisited bytes, per-reason totals and up to
256 skipped ranges with explicit omitted-byte accounting. E2 still latches
faults. For a kernel at `c225239c90`, apply only patch 6 (or cherry-pick
`906da12f51`). A kernel at `9f0ddbb894` needs patches 5 and 6; a kernel at
`d638469c63` needs patches 4 through 6.

Reserved modifier: `apple_a9_aop.ring_scan_reserved=1` with debug=1 and
ring_scan=1 admits reserved mapped System RAM. It works with early mode,
still excludes no-map/MMIO and the reader/stack, and retains page-fault
quarantine and all bounds. Each hit reports PA and one of system-ram,
crash-kernel, cma, reserved-dram or mixed. Membership is read from kernel
resource and CMA metadata; it does not prove original contents survived.
E2 is unchanged. No AOP firmware window or new mapping is added.

The latest LLVM arm64 W=1 build, ASan/UBSan production policy/classifier
and loop tests, IIO/CMA-disabled visibility checks, linked early-initcall
and parameter/gate checks, patch replay and checkpatch all passed.
Detailed latest logs: `offline-reserved.log`, `variants-reserved.log` and
`build-w1.log` under `build/periph/aop-ring-reader/`.

Directed capture: set `apple_a9_aop.debug=1`, `apple_a9_aop.ring_pa=<PA>` and
`apple_a9_aop.ring_pa_size=<SIZE>` with values from the current iOS AFK probe.
Keep `ring_scan=0 active=0 attach=0 ring_iio=0` in the same namespace. It
automatically captures at postcore before broad scanning, without needing
ring_scan_early. Up to four matching comma-separated lists are supported;
each range is 1..65536 bytes. Read
`sudo cat /sys/kernel/debug/apple-a9-aop/ring_pa` after boot. It reports
System RAM/reserved/no-map metadata, two frozen prefix/nonzero-line dumps,
all relative 0x40-aligned header matches, and byte/line changes after 1 s.
The source must be valid linear-mapped System RAM, never MMIO or no-map;
reserved mapped RAM is admitted. No source write, mapping, reservation or
cache maintenance occurs. Read faults quarantine a page across all targets
and snapshots; partial failed bytes stay hidden. Changes do not identify
the writer. Unreserved buffers produce a critical allocator-RAM warning.

The directed path uses about 0.5 MiB of BSS to avoid capture-time buffer
allocations. Those pages are excluded from broad scans as well. Earlier
boot/kernel allocations can still overwrite the source; no historical
ownership is inferred. Latest logs: `offline-target.log`, `target-compile.log`,
`target-commit.log`, `variants-target.log`, `verify-target.log` and
`build-w1.log`. The host preparation helper supports `--mode target` and
one to four explicit `--buffer PA:SIZE` options, without a DT reservation.

Independent state probe: `apple_a9_aop.state_probe=1` works with debug=0 and
no ring flags. It takes one snapshot at observer probe, after the existing
five-domain PMGR guard. Only +0xb88 (raw cpu_status and running bit) and
+0x4020 (outbox-enable bit) are read. AOP ACTUAL=0 yields unpowered with
wrapper fields skipped; powered readings classify running/halted. Other
guard refusals report unknown, never an invented halted result. The logged
line is frozen in root-only `/sys/kernel/debug/apple-a9-aop/state_probe`.
This mode blocks active-driver resource access, issues no hardware writes
or FIFO reads, and can coexist with explicit ring diagnostics. The source
PMGR provider's ordinary always-on policy remains unchanged.

The state-probe W=1 build and production PMGR/callback ASan/UBSan tests pass.
Latest logs: `offline-state-probe.log`, `state-probe-review.log`,
`state-probe-build.log`, `state-probe-commit.log`, `verify-state-probe.log`.
Exact power-off procedure, tokens and collector commands:
[state-probe test plan](../../../docs/periph/aop-state-probe-test-plan.md).

For a postcore capture, append `apple_a9_aop.ring_scan_early=1` alongside
`apple_a9_aop.debug=1 apple_a9_aop.ring_scan=1` in the FIRST Linux payload after
iOS -> DFU. Then read `sudo cat /sys/kernel/debug/apple-a9-aop/ring_scan`;
require `phase=postcore-initcall`. Reads only format the frozen early result.
The initcall is after allocator setup, so historical allocation stays unknown.

No device access was performed by this agent. The first run must follow the last iOS boot
through DFU to Linux without cycling AON; rediscover after any subsequent iOS
boot. Warm-kexec E2 requires the reservation in both running and incoming
kernels. E1 can run after kexec but cannot recover bytes reused by the prior kernel.

Exact integration, cold-boot and warm-kexec commands:
[orchestrator test plan](../../../docs/periph/aop-ring-reader-test-plan.md).
