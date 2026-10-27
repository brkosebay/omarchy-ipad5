# AOP power-off state probe — orchestrator handoff

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implements the read-only discriminator in
[ds-aop-standalone.md §1](ds-aop-standalone.md#1-q1--does-a-full-power-off-stop-the-aop).
The implementation agent performs no device access, workspace commit or push.
Kernel changes belong only to `a9/aop` in the `linux-a9-src` Docker volume;
the patch is appended to `patches/aop/ring-reader/series`.

Kernel commit: `906da12f51cbd53bf8bc13df196281983f3fe5de`, patch 6. The
full arm64 LLVM=1 W=1 Image.gz/DTBs and production-function ASan/UBSan tests
pass. Build provenance is clean; logs are under
`build/periph/aop-ring-reader/*state-probe*.log`.
Linked parameter/guard checks and six-patch replay pass; every patch has
zero checkpatch errors and warnings, and SHA256SUMS verifies on the host.

`apple_a9_aop.state_probe=1` is default off, independent of `debug`,
`ring_scan` and `ring_pa`. At observer probe it uses the existing five-domain
PMGR guard. All domains must be software ON, hardware ACTUAL=TARGET=15, with
reset/disable/parent-off clear before either wrapper word is read. Provider
absence defers probe. The new mode never resumes or attaches a domain and
blocks the active driver before any active allocations or resource accesses.

2026-10-25 correction: the two wrapper reads remain `+0xb88` and `+0x4020`.
The former is now named `wrapper_b88`; it mirrors inbox in the J71t captures,
so both bit0 values report `powered execution=unknown`. Earlier running/halted
verdicts and the expected `0x20001`/`0x20000` CPU interpretation are withdrawn.
`b88` and `4020` are raw bit0 observations only. When PMGR AOP ACTUAL=0,
wrapper fields say `skipped` and classification is `unpowered`. Other guard
refusals report `unknown` with errno; errors cannot fabricate PMGR values.
The current forced-cold predicate and register evidence are in
[aop-force-cold.md](aop-force-cold.md).

The same one-line result appears in the kernel log and root-only debugfs.
Debugfs reads only the saved string, never PMGR or wrapper hardware. With
`debug=0`, no other observer files are exposed and only the two required
wrapper resources are mapped. Adding `debug=1` explicitly permits existing
live status/preflight reads; adding ring mode explicitly permits its separate
RAM capture. None is needed for this experiment.

## Host validation and export

These commands operate on the host/Docker only:

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build-ring-reader.sh
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-ring-reader.py'
scripts/build-kernel.sh --run aop 'for p in /work/patches/aop/ring-reader/*.patch; do scripts/checkpatch.pl --no-tree "$p" || exit; done'
(cd patches/aop/ring-reader && shasum -a 256 -c SHA256SUMS)
```

The sanitizer harness compiles the actual PMGR guard, state callback,
capture, formatter and debugfs show functions with a fake MMIO backend.
The backend asserts that all five PMGR reads and locks precede exactly two
wrapper reads. Tests cover both b88 bit0 values with execution unknown, outbox cases, power-off,
transitions, reset/disable/parent-off, software domain state, regmap errors,
provider deferral, lock/reference balance and repeated frozen debugfs reads.
Registers are checked unchanged. Build checks verify the immutable parameter,
active exclusion, callback addresses, saved-string debugfs and canonical
templates. Full arm64 LLVM=1 W=1 Image.gz/DTBs and checkpatch remain required.

## One controlled device run, by the orchestrator

Integrate the new `a9/aop` commit into the normal full Omarchy/NVMe/kexec
kernel, following the existing
[integration instructions](aop-ring-reader-test-plan.md#integration-host-only).
The standalone AOP build is not a complete bootable integration. Preserve the
known recovery entry, the normal initramfs/m1n1/PSCI setup, and every non-AOP
baseline cmdline token. Replace the existing AOP tokens with:

```text
apple_a9_aop.state_probe=1 apple_a9_aop.debug=0 apple_a9_aop.active=0 apple_a9_aop.attach=0 apple_a9_aop.ring_scan=0 apple_a9_aop.ring_iio=0
```

Omit `ring_pa`/`ring_pa_size` and the broad-scan modifiers. No DT changes are
needed. Complete the payload packaging before the power-off experiment.

1. From a normal iOS session, use **slide to power off**, then wait 30 seconds.
   Follow the study's procedure: do not connect USB until entering DFU.
2. Enter DFU from that off state and boot via palera1n → pongoOS → m1n1 →
   Linux. Do not boot iOS or warm-kexec in between.
3. After boot, collect the saved result and identity. The snapshot has already
   happened at driver probe; collecting it later cannot change the evidence.

On-device read:

```sh
sudo cat /sys/kernel/debug/apple-a9-aop/state_probe
sudo dmesg | grep 'aop state:'
```

Or from the orchestrator Mac:

```sh
mkdir -p build/periph/aop-state-probe/capture
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline; sudo -n dmesg' \
  > build/periph/aop-state-probe/capture/identity.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/state_probe' \
  > build/periph/aop-state-probe/capture/state.txt
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/apple-a9-aop/state_probe' \
  > build/periph/aop-state-probe/capture/state-repeat.txt
cmp build/periph/aop-state-probe/capture/state.txt \
  build/periph/aop-state-probe/capture/state-repeat.txt
```

If debugfs is not mounted, mount it before collecting. Require the expected
cmdline/build identity and identical saved/logged lines. `powered execution=unknown`
means the guard allowed the raw wrapper observation. Neither b88 bit0 value
establishes execution or PC halt. Unpowered means PMGR AOP ACTUAL was zero and
no wrapper reads occurred. Unknown is a guarded refusal, not a power-off result.

This is after the ordinary PMGR provider's initialization; its existing
always-on policy may already have powered a domain. The probe does not
establish the AON rail state before Linux and never inhibits autonomous
hardware gating. Nothing clears mailbox faults, touches a FIFO, sends a
message, changes power or releases/resets the CPU. A cold-loader experiment
is separate and must omit state_probe, which deliberately excludes active
takeover. No device outcome is claimed by this handoff.
