# A9 idle reset and wake investigation — round 6

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

**A9 has no M1-style architectural retention that resumes after WFI.**
Deep WFI on A7–A11 returns through RVBAR. The ordinary iOS idle reset
dispatch is now recovered, but its secure-monitor handoff, the Linux
reset exception level, and the complete cluster/cache contract are not.
Consequently this branch does **not** register a new deep state. This is
a research result and a safety fix, not a claim to have achieved returning
deep idle. The full power-down implementation gate in the brief is unmet.

The kernel change adds `cpuidle_apple_a9_idle.probe=1`, default off, which
logs per-online-CPU MIDR, CurrentEL, ID_AA64PFR0_EL1, AIDR_EL1,
CPU_CFG and CYC_OVRD. It works without `enable=1`, does not register an
idle driver in that mode, and performs no MMIO or register writes.
It reports AIDR's architectural-retention capability bit as evidence;
that bit alone never authorizes a deep state.

The existing fallback needed correction: plain WFI under a rejected
power-down configuration could trigger exactly the transition being
rejected. State0 now requires CPU_CFG.bit24 clear and CYC_OVRD[25:24]=2;
unsafe requests return `-EBUSY` before WFI. An unexpected MIDR returns
`-ENODEV` before any implementation-register access. Clock-gate entry
still preserves the complete saved CYC_OVRD, forces mode2 only, and
restores it after WFI. An already-mode2 entry accounts as state0.
There is no new CPU_CFG, IORVBAR, PMGR CPU/CPM or cluster-register write.

## Evidence identity and reproduction

All code addresses below are **unslid kernelcache virtual addresses**,
unless explicitly described as physical MMIO. A physical resume address
must be derived from the actual boot layout; copying one of these VAs
into IORVBAR would be wrong.

The iOS 16.7.10 / 20H350 kernelcache SHA256 is
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
The [compact evidence index](../release/OMISSIONS.md) contains the
instruction scan and vtable pointers, not private calibration data.
The complete queries stay in `build/idle/resume-evidence/evidence.json`.
Shared Ghidra names and comments were saved for the understood core paths.

```sh
python3 scripts/idle/fetch-sources.py
python3 scripts/idle/resume-evidence.py --extract-kexts \
  --index docs/power/evidence/idle-resume-j71t.json \
  > build/idle/resume-evidence.log

# Use the Python environment named by the ghidra-mcp launcher shebang.
# The shared project's S8000 last-core function boundaries overlap;
# create exact entries in a separate, small extracted-kext project.
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python \
  scripts/idle/ghidra-kext.py \
  --binary build/idle/resume-evidence/kexts/AppleS8000PMGR.macho \
  --output build/idle/s8000-local \
  --function 0xfffffff005e91ca0 \
  --function 0xfffffff005e92484 --function 0xfffffff005e92538 \
  --function 0xfffffff005e92c74 --function 0xfffffff005e92c78 \
  --instructions 0xfffffff005e92484:0xb4 \
  --instructions 0xfffffff005e92538:0x174 \
  --instructions 0xfffffff005e92c74:0x8
```

The source fetcher pins Hoolock m1n1 to
`d5a10ac52a6468484854419a6c5130f1d62073eb`, XNU 8796.141.3 to
`1b191cb58250d0705d8a51287127505aa4bc0789`, and the examined Asahi
driver to `94fb23346d522edf53722357c426a3e58030beea`. Its manifest records
file hashes and missing public files. Public XNU omits the S8000/S8003
board headers here; those missing definitions are not evidence of a
retention feature being enabled or disabled. Binary instructions decide
what this j71t kernel actually does.

## Ordinary iOS idle and return

1. `j71t_machine_idle`, `fffffff0072e2560`, may briefly use a timed WFE
   path for imminent events. Otherwise it calls `Idle_context` at
   `fffffff00718b6a0`. The latter saves x19–x30, SP, d8–d15 and FPCR
   to the current thread's kernel context, switches to the CPU idle
   stack, and branches to `cpu_idle` at `fffffff0072d7d10`.

2. `cpu_idle` rejects disabled idle/signals and checks pending timer work.
   It records the active thread in `cpu_data+0x30`, calls the idle notifier
   at `cpu_data+0x98` with entry=1, resynchronizes the idle deadline when
   needed, and saves monotonic PMU state when enabled. The final sequence
   is `dsb sy` at `fffffff0072d7e44`, then WFI at `fffffff0072d7e48`.
   **It writes neither CPU_CFG nor CYC_OVRD.**

3. A shallow wake continues after that WFI, clears idle-timer state and
   calls `cpu_idle_exit(false)` at `fffffff0072d7e78`. A context-losing
   wake takes the reset route below. Both converge on the same notifier
   exit and `Idle_load_context` at `fffffff00718b720`, restoring the
   saved registers/FPCR and returning to `machine_idle`. That routine
   unmasks IRQ/FIQ before returning to the scheduler.

There is no software L1 clean/invalidate, I-cache invalidation or TLBI
in the observed ordinary entry sequence. That is a bounded observation,
**not** proof that any arbitrary cache configuration can power down safely.
Apple hardware/firmware owns part of the cache/coherency transition.
The public XNU debug simulation can flush caches/TLBs *after* WFI; that
is not a pre-entry production requirement and is not compiled into this
idle path. The normal WFI differs from the nonreturning system-sleep
helper recovered in round 5.

An aligned scan of every file-backed executable top-level Mach-O segment,
including `__PLK_TEXT_EXEC`, finds two WFI instructions: ordinary idle at
`fffffff0072d7e48` and system sleep at `fffffff00718bbc4`. The only
CYC_OVRD writes are the sleep helper and force-clock-gate helper.
The only CPU_CFG writes are system sleep and the Samsung C0 workaround
described below. Indirect firmware execution and non-code literal-pool
hits are limitations of this scan; the selected hits were checked in Ghidra.

## Reset-vector setup and resume

`cpu_machine_idle_init(from_boot)` at `fffffff0072d84f0` initializes
`ResetHandlerData` at `fffffff0070c0060`. Its +8 field gets the physical
`CpuDataEntries` address. It translates `LowResetVectorBase` to a physical
address, then calls `fffffff00718bc1c` with x0=`0x800`, x1=that address,
x2=x3=0. The helper executes **SMC #0x11**, then returns.
XNU identifies 0x800 as `MONITOR_SET_ENTRY`.

This is a firmware/secure-monitor ABI, not PSCI. The implementing monitor
is outside this kernelcache, so its actual IORVBAR write/locking/security
behavior cannot be recovered from this artifact. Public XNU's boot ABI
also says iBoot can locate the reset page from the aligned kernel entry.
Those are two pieces of firmware involvement, not proof that Linux
inherits a valid iOS reset handler after the tethered boot chain.

The same initializer computes physical addresses for `start_cpu`
(`fffffff00718b00c`) and `resume_idle_cpu` (`fffffff00718b000`). Each
CPU's `cpu_data+0xb8` defaults to the physical idle-resume entry.
`cpu_start`, `fffffff0072d82e4`, selects the start entry for a newly
started core, initializes its active-thread links, and calls the platform
CPU-start method. Selecting that per-CPU software handler does not
reprogram the hardware reset vector on every idle entry.

The recovered reset flow is:

```text
hardware/firmware reset destination -> physical LowResetVectorBase
  kernel VA fffffff007189000: branch to reset dispatch
  fffffff007189400: mask DAIF, set early VBAR, read MPIDR low byte
  scan two 16-byte CpuDataEntries; compare cpu_data.phys_id (+0x1c8)
  load physical cpu_data.reset_handler (+0xb8)
  accept only resume_idle_cpu or start_cpu, else halt
  resume_idle_cpu -> common_start_cpu with arm_init_idle_cpu continuation
  rebuild MMU/CPU state -> virtual CPU-data trampoline
  arm_init_idle_cpu -> cpu_idle_exit(true) -> Idle_load_context
```

`common_start_cpu`, `fffffff00718b018`, loads boot virtual/physical bases,
memory size, exception and interrupt stacks. The common setup starting at
`fffffff00718b270` restores TCR, TTBRs, MAIR, early VBAR and SCTLR,
invalidates the local TLB (`VMALLE1`, DSB), and reapplies Twister chicken
registers. Its CPU_CFG write at `fffffff00718b3e4` sets **bit30**, only
for MIDR part 0x004 revision 0x20: Samsung Twister C0's skip-init workaround.
It does not enable deep sleep on j71t's TSMC part 0x005.

`arm_init_idle_cpu`, `fffffff0072dae28`, restores the thread pointer,
sets CNTKCTL, reinstalls the normal TTBR0, executes `tlbi vmalle1is;
dsb ish; isb`, restores debug state as needed, enables CNTV_CTL_EL0=1
and disables/masks CNTP_CTL_EL0=2, then calls `cpu_idle_exit(true)`.
The latter reconnects accounting/debug and the idle notifier before
loading the saved context. Linux's physical/virtual timer selection
must be respected; these iOS timer values are not a Linux recipe.

## CPU_CFG, ACC and last-core behavior

CPU_CFG is system register S3_5_C15_C4_0; CYC_OVRD is
S3_5_C15_C5_0. The MMIO IORVBAR at the base of `cpu-impl-reg` is a
different register. j71t physical MMIO addresses decoded from the ADT:

| Block | Physical address / relevant offsets |
| --- | --- |
| CPU0 / CPU1 IORVBAR | `0x202050000` / `0x202150000`, offset 0 |
| CPU performance/ACC window, PMGR logical region9 | `0x202220000` |
| Shared ACC implementation window, logical region13 | `0x202240000` |
| CPU-start controller | `0x20e0d4000` |
| PMGR CPU0 / CPU1 power-state/status | `0x20e080000` / `0x20e080008` |
| PMGR CPM power-state/status | `0x20e080040` |

The system-sleep helper at `fffffff00718bb9c` conditionally writes
CPU_CFG=`0x01000000`, ORs `0x03000000` into CYC_OVRD, and loops over
DSB/ISB/WFI forever. It expects reset-based restart. `ml_arm_sleep`
has CPU sleep tokens and boot-CPU synchronization. Its apparent cache
helpers `fffffff007181554` and `fffffff00718155c` are just DSB SY/RET
in this build. This does not supply the missing hardware cache protocol.

The returning helper at `fffffff00718bbcc` ORs **bit25 only** into
CYC_OVRD. XNU calls that force-up/clock-gate mode2. Mode3 is forced
power-down; CPU_CFG.bit24 additionally enables legacy deep/system sleep.
Neither operation makes a retention mode appear on A9. The public
`HAS_RETENTION_STATE` conditional and M1's ACC_OVRD code cannot be
assumed to apply to Twister.

`AppleARMCPU` registers `function-cpu_idle` via its start method at
`fffffff0059b5460`; the entry/exit wrapper is `fffffff0059b62e8`.
The ADT provider/fourcc `cpuI` reaches ApplePMGR's dispatch at
`fffffff005a3e5bc` and accounting at `fffffff005a3b514`.
Under a shared simple lock, it tracks a per-cluster active count and mask:
last entry is the 1->0 transition; first exit is 0->1. It also reads
POWER_GATED/clock-gate status and clears a sticky bit9 for later
measurement. Those status operations do not establish a complete
power-down control sequence.

The actual AppleS8000PMGR vtable is `fffffff006e0fe30`, confirmed from
its constructor and file-backed pointers:

| Slot | Target | Observed role |
| --- | --- | --- |
| +0x8b0 | `fffffff005e92484` | Last active core entering idle |
| +0x8b8 | `fffffff005e92538` | First active core leaving idle |
| +0x8c0 / +0x8c8 | `fffffff005e92c74` / `...2c78` | Per-core entry/exit: both RET |

The last-core hook checks policy flags, polls region9+0x20 bit31,
conditionally waits for region9+0x510 & 0xf800 to clear, then calls
the +0x6c8 policy method with `(0,0,1,0)`. The first-core hook polls
region9+0x20 bit31 twice; conditional flags restore saved 64-bit values
to region13+0x50, +0x48, +0x40 and +0x100. Further flags wait on
region9+0x20 bit6 and call +0x6c8 with `(0,1,0,0)`; a performance-state
remapping can follow. +0x6c8 resolves to `fffffff005e91ca0`, which
delegates to ApplePMGR policy and polls cluster status for some requests.

The saved-value producers, all flag provenance, cache/coherency effects
and bounded rollback have **not** been established. Some iOS polls are
unbounded. This is evidence of real last-core/first-core coordination;
it is not safe to replace it with independent CPU0/CPU1 PMGR off writes,
nor to copy those waits into Linux's idle callback. CPU0/CPU1 share one
Twister cluster; CPM is not a second CPU cluster.

## Comparison with m1n1 and Asahi Linux

[Hoolock m1n1's utils.c](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/utils.c)
tests AIDR_EL1.bit2 for architectural retention. For A7–A11 without
that capability, `deep_wfi()` executes ordinary WFI rather than its
register-saving deep helper, explicitly because deep WFI returns to RVBAR.
Its Twister feature configuration clears CPU_CFG.bit24 and sets
CYC_OVRD WFI mode2, unmasking the IRQ/FIQ override fields. This explains
why round-5 WFI-CG will usually have **zero marginal benefit** after
this bootloader: the boot mode is already forced clock gating.

[m1n1's smp.c](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/smp.c)
gets IORVBAR from `cpu-impl-reg`, or `reg-private+0x40000`. If unlocked,
it writes `_vectors_start` there. It cleans the new secondary-stack
pointer to PoC, DSBs, then writes the core bit to physical
`0x20e0d4004` (startup/interrupt state) and `0x20e0d4008`
(cluster0 CPU start), waiting up to 100ms for the secondary handshake.
IORVBAR bit0 is treated as a lock and the address mask is bits47:12.
The boot CPU's IORVBAR is also replaced if unlocked.

Its [reset trampoline](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/start.S)
has separate CurrentEL=EL3 and lower-EL paths, establishes its own stack,
and, when entered at EL3, configures SCR/ELR/SPSR/SP_EL1 and ERETs to
nonsecure EL1. Its SMP worker then waits on a spin table; `kboot.c`
provides `cpu-release-addr` for Linux. That boot-time spin table is
**not** a `cpu_suspend()` resume context or a per-idle reset protocol.
No PSCI CPU_SUSPEND service is supplied by this examined handoff.

[Asahi's examined cpuidle driver](https://github.com/AsahiLinux/linux/blob/94fb23346d522edf53722357c426a3e58030beea/drivers/cpuidle/cpuidle-apple.c)
saves x18–x30 around mode3 WFI, loops until ISR_EL1 says an interrupt
is pending, then returns by the retained PC/SP. `cpu_pm_enter/exit`
preserves FP and notifies context owners; RCU/context-tracking handling
is explicit. Its machine whitelist contains M-series SoCs, not A9.
The examined driver does not match an `apple,deep-wfi` DT compatible.
Adding that string to a DT cannot provide a missing hardware capability.
The [A7–A11 AIC patch author](https://patchew.org/linux/20240829110436.46052-1-towinchenmi%40gmail.com/)
independently warned that this downstream deep-idle approach restarts
these earlier cores at RVBAR.

For a Linux reset-based implementation, `cpu_suspend()` and `cpu_resume`
provide a relevant EL1 context/MMU restoration framework, but **not**
the Apple reset-vector ownership, secure-to-nonsecure reset transition,
Twister chicken restoration or shared-cluster protocol. FPSIMD and the
architectural timer have CPU_PM handling in this kernel; that alone
does not prove the AIC/timer wake path survives a Twister reset.

## What remains gated

The following must be established before adding a full power-down state:

1. Read the actual boot handoff's IORVBAR address/lock/security ownership
   and reset exception level for **each** CPU. Establish a reserved Linux
   trampoline and a supported way to install it without calling unknown
   iOS monitor services or writing boot-firmware storage.
2. Prove a CPU1-only reset-resume round trip with CPU0 kept running;
   distinguish WFI fall-through from a real reset using a trampoline
   counter. Preserve/reapply required EL1, implementation, debug, FP,
   AIC and timer state. A saved LR alone is insufficient.
3. Establish the cache/TLB/coherency contract for that precise transition.
   The observed absence of an entry cache clean is insufficient evidence
   to omit cleaning Linux context or to power off shared caches.
4. Resolve the shared policy flags/ACC saved values and last-core/first-core
   restore protocol. Only then allow both cores to request deep idle.
5. Provide an independent watchdog/reset path and a bounded entry refusal
   path. A software timer on the sleeping core cannot repair a bad reset
   vector; neither can a watchdog guarantee recovery if its clock/power
   domain was disabled. Establish that hardware behavior before relying
   on it for an unattended power-down test.

No runtime `deep_wfi`/`retention` switch that bypasses these gates is added.
No fake cpuidle state is exposed for a path known not to exist on A9.

## Ranked device commands — orchestrator only

> Private graphics-dependent excerpt omitted from this source snapshot.

1. **Probe only, no new idle policy.** Add only
   `cpuidle_apple_a9_idle.probe=1`; omit this driver's `enable` and
   `clock_gate`, and keep the older `cpuidle_apple_a9.enable` off.

   ```sh
   cat /proc/cmdline
   findmnt -no SOURCE,FSTYPE /
   dmesg | grep 'apple-a9-idle:'
   cat /sys/devices/system/cpu/online
   cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || true
   tr -d '\000' </sys/firmware/devicetree/base/cpus/cpu@1/enable-method
   ```

   Expect two probe lines with Apple TSMC Twister MIDR (part0x005),
   CurrentEL=4 (EL1), no EL2 in PFR0, AIDR.bit2 clear, CPU_CFG.bit24
   clear, and CYC_OVRD[25:24]=2. Record the actual EL3 capability field,
   not an assumption about it. The cpuidle driver must remain the prior
   driver or absent. A missing/mismatching line stops progression;
   cold-boot the known-good payload with `probe` omitted if necessary.

2. **Guarded WFI accounting.** Fresh boot with `probe=1 enable=1` for
   this driver only; `clock_gate` absent. In full parameter form use
   `cpuidle_apple_a9_idle.enable=1`.

   ```sh
   cat /sys/devices/system/cpu/cpuidle/current_driver
   for p in /sys/devices/system/cpu/cpu[01]/cpuidle/state0; do
     cat "$p/name" "$p/usage" "$p/time" "$p/rejected"
   done
   stress-ng --cpu 2 --cpu-method all --verify --timeout 30s
   ```

   Expect `apple_a9_idle`, state0 WFI, rising usage/time and no sustained
   rejection growth. Any unsafe inherited configuration is rejected
   **without WFI**, so it can consume CPU power; investigate rather than
   clearing guards. Do not inject unsafe CPU_CFG/CYC_OVRD values on the
   device to test the rejection path; host tests already cover that logic.
   Use the known-good memory/FP stress suite if stress-ng is unavailable.

3. **Repeated independent wake sources.** On the same guarded WFI boot,
   run at least 100 local idle/load/timer transitions before using a
   longer idle measurement. Keep a local console/recovery route.

   ```sh
   python3 - <<'PY'
   import os, time
   for cpu in (0, 1):
       os.sched_setaffinity(0, {cpu})
       for _ in range(100):
           deadline = time.monotonic() + 0.02
           while time.monotonic() < deadline:
               pass
           time.sleep(0.1)
       print('timer/load cycles complete on CPU', cpu, flush=True)
   PY
   cat /proc/interrupts
   dmesg | grep -Ei 'watchdog|lockup|stall|SError|BUG:|Failed to reach'
   ```

   Also exercise Home/button, touch, Wi-Fi, connected USB and scheduler
   cross-CPU work. This checks returning WFI behavior, not reset resume.
   A hang/reset/corruption rejects that boot; remove the experiment flags
   and tether-boot the known-good payload. Do not change watchdog settings
   or claim this userspace timeout recovers a sleeping CPU.

4. **Existing clock-gate experiment.** Only after the above passes, fresh
   boot with `probe=1 enable=1 clock_gate=1` under this module prefix.
   Repeat steps2/3 and the [round-5 recorder](../../scripts/idle/measure.py).

   ```sh
   cat /sys/devices/system/cpu/cpu[01]/cpuidle/state*/name
   sudo python3 /var/tmp/measure.py /var/tmp/a9-idle-r6-cg \
     --mode screen-off --backlight-off --duration 300 --interval 5 --settle 60
   ```

   If the probe already reports mode2, zero state1 usage and state0
   fallback are expected: no extra hardware depth was entered. Use fresh
   output directories and baseline/variant/baseline at least three times,
   with unchanged brightness, workload, root medium, radio/charger state
   and a comparable temperature band. Stop/restore automatic screen/sleep
   timers as described in [idle.md](idle.md). Real energy comparisons
   require discharge and no external supply; USB/NBD measurements alone
   cannot establish battery savings. Backlight-off is not full display
   power-down.

5. **Future reset experiments: no executable power-down command yet.**
   Have the orchestrator capture IORVBAR/lock and CurrentEL through the
   already-established m1n1 debug workflow before Linux boots. Capture
   both cores only after m1n1 confirms them started, with deep mode off.
   Do not use Linux `/dev/mem` writes or SMC #0x11 from a shell. Return
   those observations to resolve gate1. No CPU1 reset test and no both-core
   idle test is authorized by a fabricated `deep_wfi=1` parameter here.

## Validation and expected gain

The changed object compiles with `W=1`; strict checkpatch reports zero
errors/warnings/checks. Thirty-six behavioral guard scenarios execute the
actual C entry functions against mocked register access: unknown CPUs
perform no implementation-register reads; unsafe configurations execute
no WFI; the allowed clock-gate experiment restores all unrelated bits.
These tests do not emulate physical power gating or establish wake safety.

The full standalone `Image.gz` and 104 DTBs build with debug information
disabled. `scripts/idle/validate.sh` also runs ten recorder tests, checks
the built commit/config, and verifies that all five exported patches
reproduce the committed kernel tree. Build evidence is in
`build/idle/resume-validation.txt`; artifacts and head are recorded under
`build/kernel-idle/`. Non-mutating merge-tree checks also pass with
`a9/power` at `c4a1c8f7b21c21efde4c0078dc745169e463197e` and the existing
integration head `11c2bf4073b6803b6f11d7eda2632477da219d72`.
The latter is a merge check, not a new integrated-kernel build.

**Expected additional deep-idle power gain from this round: zero**, because
no new deep transition is enabled. Probe-only is diagnostic; WFI safety
checks have small unmeasured overhead. WFI-CG's marginal gain is expected
to be zero with m1n1's already-mode2 setup. Full core/cluster power-down
could remove additional idle leakage, but there is no supported mA, watt
or battery-life estimate until a working reset-resume transition and paired
battery measurements exist. No device gain was measured.
