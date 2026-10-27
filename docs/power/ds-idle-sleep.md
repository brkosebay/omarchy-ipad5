# A9 (S8003/j71t) CPU idle, core power-down and system sleep — driver study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Offline research only: no device access, no commits.
Target: iOS 16.7.10 / 20H350 kernelcache (SHA-256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`),
Ghidra project `/kernelcache.release.iPad6,11_12`, plus pinned public sources
(XNU 8796.141.3, Hoolock m1n1 `d5a10ac52a`, Asahi `94fb23346d`) already
under `build/idle/resume-sources/`.

Read with [idle.md](idle.md) and [idle-resume.md](idle-resume.md): this
document does not repeat their register-level idle work, it answers what
iOS *does* with those registers, what a returning core needs, and how Apple
suspends the whole machine. Everything is labelled **Evidence** (read from
the binary, the live ADT, or a pinned public source) or **Inference**
(reasoning that has not been verified on this SoC).

## 1. Summary

| Question | Answer | Confidence |
|---|---|---|
| Does iOS put a core into a deeper-than-WFI state on every idle entry? | No. `cpu_idle` executes `dsb sy; wfi` and writes no power-state register; the only implementation-register write before WFI is `PMCR0` (PMU control). | Evidence (0x72d7d10..0x72d7e48) |
| What makes WFI clock-gate instead of plain-WFI? | `CYC_OVRD` WFI mode field `[25:24] = 2`. m1n1 already programs mode 2 at boot for A7–A11 (`features_a7`), and clears `CPU_CFG.bit24`. | Evidence (m1n1 `chickens.c`, `cpu_regs.h`) |
| What is the deeper state, and who uses it? | `CPU_CFG.bit24 = 1` (legacy deep sleep, Twister) plus `CYC_OVRD[25:24] = 3` (force power-down). iOS uses this only on the *system sleep* path, never in per-core idle. | Evidence (0x718bb9c, public XNU `arm64_prepare_for_sleep`) |
| Does a power-down return to the instruction after WFI? | No. A7–A11 have no architectural retention; the core restarts at IORVBAR. | Evidence (m1n1 `deep_wfi`, AIDR scan) |
| Where does it restart? | IORVBAR (`cpu-impl-reg + 0`, bits47:12, bit0 = lock) → `LowResetVectorBase` page → reset dispatch that indexes `CpuDataEntries` by physical CPU id and calls that CPU's `cpu_reset_handler`. | Evidence (0x7189000, public `start.s`) |
| Who owns IORVBAR? | iBoot computes it from the kernel entry page (`__start & ~0xfff`); the kernel additionally tells the EL3 monitor `MONITOR_SET_ENTRY` (0x800) with the physical address, and later `MONITOR_LOCKDOWN` (0x801) at `machine_lockdown`. m1n1 overwrites IORVBAR for Linux when the register is unlocked. | Evidence (0x72d859c, 0x71f0788, m1n1 `smp.c`, public `cpu.c`/`machine_routines.c`) |
| Does the AOP take part in *core* idle? | No evidence of it. AOP appears only in system wake (`ap-wake-sources`) and as a register block PMGR can read/write (`readAOPGlobalReg`/`writeAOPGlobalReg`). | Evidence for the AOP facts, inference for "not in core idle" |
| What does iOS system sleep do to the CPUs? | Parks every non-boot core with `cpu_sleep_token = 'SUSP'`, then the boot CPU (after platform quiesce actions) sets deep-sleep bits and loops `dsb/isb/wfi` forever — reset-based wake. | Evidence (0x72d83d4, 0x718bb9c) |
| Are the classic-S2R low-memory sleep tokens used? | No. The only `'SUSP'` writes are `cpu_sleep_token` in each `cpu_data`; there is no `suspend_signature` copy to a low buffer in this build. | Evidence (0x72d83f8..0x72d8478) |
| What is the last thing that runs before `_ml_arm_sleep`? | `AppleARMCPU`'s platform sleep hook: boot CPU runs `IOCPURunPlatformQuiesceActions()`, secondary CPUs invoke their `function-enable_core` object with `(0,0,0)`. | Evidence (0x59b6448) |

The short version for our port: **the only missing piece for deeper idle is
the reset-resume contract, not a magic register.** The registers are already
in the right mode after m1n1; what A9 lacks is a Linux-owned reset vector and
a verified EL1/cache/cluster restore path. Section 9 turns that into work
items.

## 2. Vocabulary: the three WFI depths on Twister

Two implementation registers decide what `wfi` does:

| Register | Sysreg | Fields |
|---|---|---|
| `CPU_CFG` (XNU: `ACC_CFG`) | `S3_5_C15_C4_0` | bit24 `deepSleep` (legacy deep/system sleep), bit30 `skipInit` (Samsung C0 only) |
| `CYC_OVRD` (XNU: `CPU_OVRD`) | `S3_5_C15_C5_0` | `[25:24]` WFI mode, `[23:22]` IRQ mode, `[21:20]` FIQ mode, bit0 `disWfiRetn` |

**Evidence.** Field layout and the A7–A11 feature set come from the pinned
m1n1 tree: `src/cpu_regs.h` (`ACC_CFG_DEEP_SLEEP BIT(24)`,
`CYC_OVRD_WFI_MODE(x) ((x) << 24)`, `CYC_OVRD_DISABLE_WFI_RET BIT(0)`),
`src/chickens.c` (`features_a7` = `sleep_mode SLEEP_LEGACY`,
`acc_cfg`, `apple_sysregs_unlocked`; A9 Twister maps to `features_a7`), and
`src/utils.c` (`deep_wfi`: A7–A11 "always ends up at rvbar after deep WFI").
The XNU side names the same bits `ARM64_REG_CYC_CFG_deepSleep`,
`ARM64_REG_CYC_OVRD_ok2pwrdn_force_up` (mode 2) and
`..._force_down` (mode 3) in
`osfmk/arm64/machine_routines_asm.s` (`arm64_prepare_for_sleep`,
`arm64_force_wfi_clock_gate`).

| WFI mode `[25:24]` | XNU name | Effect (as far as established) |
|---|---|---|
| 0/1 | — | plain WFI, core retains state, wakes in place |
| 2 | `ok2pwrdn_force_up` ("clock gating only") | core clock is gated; state retained; wakes in place |
| 3 | `ok2pwrdn_force_down` | core is powered down; **restarts at IORVBAR**, state lost |

`CPU_CFG.bit24` is an *additional* legacy "deep sleep" enable that Twister
needs in combination with mode 3 (XNU writes `CPU_CFG = deepSleep` first,
then ORs `force_down` into `CYC_OVRD`).

**Evidence (boot state on j71t).** m1n1's `init_..._chicken_regs()` clears
`ACC_CFG.bit24` for `SLEEP_LEGACY` parts and, when
`apple_sysregs_unlocked`, sets `CYC_OVRD` `FIQ/IRQ mode 0`, `WFI mode 2`,
`disWfiRetn 0`. So after our boot chain the CPUs are already forced into
clock gating and deep sleep is disabled. That is why round 5's `WFI-CG` state
was expected to measure ~0: it re-requests a mode the bootloader already set.
**Inference:** on a device where `apple_sysregs_unlocked` is false, m1n1
leaves `CYC_OVRD` untouched and the boot state is whatever iBoot left; the
round-6 `probe=1` line is the check for that.

## 3. How iOS idles one core

Entry chain (all unslid kernel virtual addresses):

```text
machine_idle                     fffffff0072e2560
  └─ Idle_context                fffffff00718b6a0   save x19-x30, SP, FPCR, d8-d15
       └─ cpu_idle               fffffff0072d7d10
            ├─ idle notifier     cpu_data+0x98(cpu_data+0x40, 1, &hint)
            ├─ idle deadline     cpu_data+0x108 / cpu_data+0x110
            ├─ PMU accounting    fffffff0072ba488
            ├─ msr PMCR0, 0x3200 fffffff0072d7e34
            ├─ dsb sy            fffffff0072d7e44
            └─ wfi               fffffff0072d7e48
       └─ cpu_idle_exit          fffffff0072d7e78  (shallow wake)
       └─ Idle_load_context      fffffff00718b720  restore FPCR; return restores the rest
```

**Evidence.**

* `Idle_context` (0x718b6a0) stores x19–x30 (LR at `+0x58`) and the caller's
  SP at `+0x68` to `[*(tpidr_el1+0x130)]`, then FPCR at `+0xb0` and d8–d15.
  This is the current thread's kernel context, not a per-CPU area. The
  matching `Idle_load_context` (0x718b720) reloads FPCR; the general
  registers are restored by the function return.
* `cpu_idle` matches public XNU `cpu_idle()` in structure: the same guards,
  the notifier call `(cpu_data+0x40, TRUE, &new_timeout)`, the
  `idle_timer_deadline` (`+0x108`) / `rtcPop` (`+0x110`) resync, and the
  `cpu_idle_exit(FALSE)` tail. The one local difference is the PMU/accounting
  helper `FUN_fffffff0072ba488`, which corresponds to XNU's `kpc_idle()`.
* The pre-WFI write is `msr S3_1_C15_C0_0, #0x3200`. m1n1 names that register
  `SYS_IMP_APL_PMCR0` and decodes `[10:8]` as `IMODE`:
  `0x3200` = IMODE `2` (AIC) + bits 12/13 set. **It is a PMU/debug-counter
  control write, not a power-state write.** The exact intent is not
  recovered (XNU's `kpc_idle()` is outside the fetched sources); treat it as
  "iOS reprograms PMU routing around idle" and nothing more.
* There is **no** `CPU_CFG`/`CYC_OVRD` write and no cache maintenance in the
  ordinary idle path. The observed pre-WFI sequence is exactly
  `dsb sy; wfi`.
* `machine_idle` (0x72e2560) can take a timed WFE shortcut
  (`FUN_fffffff0072d7c34`) when the platform requests a clustered WFE window;
  otherwise it falls into `Idle_context`.

**What is saved across an ordinary idle entry:** only the thread's callee-
saved context (`x19–x30`, SP/LR, `FPCR`, `d8–d15`), the active-thread pointer
(`cpu_data+0x30`), the idle deadline and PMU accounting state. Nothing in
the MMU, caches, timers or AIC is touched, because clock gating retains all
of it.

### 3.1 The platform idle callback (AppleARMCPU → PMGR)

`cpu_idle`'s notifier lands in `AppleARMCPU`, which forwards to the CPU
node's `function-cpu_idle` provider (property fourcc `cpuI`; idle.md decodes
the provider as the PMGR node, phandle 24).

| Function | Address | Role |
|---|---|---|
| `AppleARMCPU::start` | `fffffff0059b5460` | binds `function-cpu_idle`, fills `ml_processor_info_t`, registers `idle`/`idle_exit`/`start` |
| idle (enter) wrapper | `fffffff0059b62e8` | tail-call PMGR selector `0x10` (CPU number in the upper bits) |
| idle_exit wrapper | `fffffff0059b633c` | tail-call PMGR selector `0x02` (CPU number in the upper bits) |
| PMGR function dispatch | `fffffff005a3e5bc` | decodes the low byte: `0x10` → enter, `0x02` → exit, `0`/`1` → accounting |
| PMGR idle enter | `fffffff005a3bf78` | per-CPU enter bookkeeping |
| PMGR idle exit | `fffffff005a3bdd8` | per-CPU exit bookkeeping, returns a timeout hint |
| PMGR idle accounting | `fffffff005a3b514` | active-count/mask and status-sticky handling (see idle-resume.md) |

**Evidence.** `AppleARMCPU::start` reads `function-cpu_idle` from the
provider, stores the object at `this+0xb8`, and passes
`(cpu_number<<24)|0x10` to the function's `+0x80` call slot at start time
(the idle wrappers repack the CPU number with `BFM` and only the low byte is
consumed as the selector).
The wrappers do `bfm` to insert the CPU number into bits31:24 and `br` to
`+0x80`. The third argument is the `&new_timeout` pointer that `cpu_idle`
passes; on exit the PMGR handler may fill in a relative counter delta that
`cpu_idle` converts to an idle deadline. This is the mechanism behind the
"idle timer notify" in public XNU.

**Inference.** The per-CPU enter/exit work is bookkeeping (active counts,
status, timeout hints), not a per-core power-state MMIO sequence: the
S8000PMGR per-core slots (`vtable+0x8c0/+0x8c8`) are literally `ret`
instructions. The actual power state change is done by the core itself when
it executes WFI in mode 2/3, and the cluster-level work is done by the
last-core/first-core hooks below.

## 4. Cluster-level coordination (AppleS8000PMGR)

A9 has two cores in **one** cluster (`logical-cluster-id = 0` for both in the
live ADT), a CPM power-state register at `PMGR+0x80040`, and a shared ACC
window at `0x202240000`.

`AppleS8000PMGR` vtable `fffffff006e0fe30` (confirmed by slot reads):

| Slot | Target | Role |
|---|---|---|
| `+0x8b0` | `fffffff005e92330` (entry `...92484`) | last active core entering idle |
| `+0x8b8` | `fffffff005e92538` | first active core leaving idle |
| `+0x8c0` / `+0x8c8` | `fffffff005e92c74` / `...2c78` | per-core enter/exit — **return immediately** |
| `+0x6c8` | `fffffff005e91c24` | cluster performance/power "handoff" policy |
| `+0x768` | `fffffff005e92094` | `setPerfState` (already known) |
| `+0x8d0` | `fffffff005e926ac` | toggles bit31 at `PMGR+0x9c080` |
| `+0xa28` | `fffffff005e926fc` | PMGR restore sweep (`0x40000..0x400a4`, `0x40200..`, `0x40210`, `(2..5)<<14`) |
| `+0xa30` | `fffffff005e92884` | voltage/EMA calibration/table programming |

### 4.1 Last-core entry (0x5e92330)

```text
poll PMGR(map 9)+0x20 bit31 clear           (performance-controller BUSY)
wait  PMGR(map 9)+0x510 & 0xf800 == 0       (cluster status idle)
call  +0x6c8(this, 0, 0, 1, 0)              ("hand the cluster to CPMS/CPM")
```

### 4.2 First-core exit (0x5e92538)

```text
poll PMGR(map 9)+0x20 bit31 (twice)
if flag +0x6d3e: write saved64 -> (map 0xd)+0x50, (map 0xd)+0x48
if flag +0x6d41: write saved64 -> (map 0xd)+0x40, (map 0xd)+0x100
poll PMGR(map 9)+0x20 bit6 clear
call +0x6c8(this, 0, 1, 0, 0)               (reverse handoff)
if +0x3698c: read (map 9)+0x50 low nibble, map through table +0x369d0,
             call +0x768(this, 2, index, 0, 0)   (restore perf state)
```

**Evidence.** Decompiled bodies above; vtable slots read from
`0x6e0fe30+0x8b0` etc. The four "saved" values are 64-bit fields in the PMGR
object at `+0x369f8/+0x36a00/+0x36a10/+0x36a18`, with enable flags at
`+0x369f0/+0x36a08`. `AppleS8000PMGR::initDriver` (`fffffff005e91348`)
reads three DT values through its property helper (keys 2, 0x24, 0x26 —
key→name mapping not recovered) into `+0x6d31/+0x36989/+0x3698c` and builds
the nibble→index tables used by the perf-state restore; the live `pmgr` ADT
node contains `l2c-acc-sleep = 0xffffffff`, `cpm-power-gating = 1`,
`mcc-power-gating = 1` and `cpu-power-gate-latency-us = 0xC350
(50 000 µs)`, which are the obvious candidates for those keys.

**Inference / not established.** The four ACC-window registers
(`map 0xd` + 0x40/0x48/0x50/0x100) are almost certainly configuration that
the cluster power-down loses and that must be re-applied on the first core's
exit, but **which** registers they are and who produces the saved values is
not recovered. The producers are not in the last-core hook (it writes no
object fields), so they are either programmed once at init from the ADT or
saved elsewhere. Do not copy these writes into Linux as blind MMIO: the
`map 0xd` window is `0x202240000` (shared ACC), and its semantics on a
running Linux kernel are unknown.

**What this means for Linux.** Do not implement "cluster power-down" as
"power off CPU0 and CPU1 domains independently". The two cores and the ACC
are one cluster; iOS's own handoff requires the perf-controller BUSY bit to
be clear, a cluster-status wait, and a CPMS/CPM interlock bit
(`map 9 + 0x20` bit22) to be toggled and acknowledged by hardware before and
after. A Linux implementation must reproduce that handshake exactly, or
leave the cluster powered.

## 5. The reset/resume path — what state is saved and where the core restarts

### 5.1 Boot-time setup

`cpu_machine_idle_init(from_boot)` at `fffffff0072d84f0`
(public: `osfmk/arm64/cpu.c`):

```text
idle_enable          = (boot-arg "jtag" absent or 0)     -> DAT_fffffff007893d88
ResetHandlerData[0]  = 0
ResetHandlerData[1]  = phys(CpuDataEntries)               -> DAT_fffffff0070c0068
monitor_call(0x800 /* MONITOR_SET_ENTRY */, phys(&LowResetVectorBase), 0, 0)
production status from /chosen/effective-production-status-ap
  -> coresight_debug_enabled (DAT_fffffff0078d9db8)
start_cpu_paddr        = phys(j71t_start_cpu)
resume_idle_cpu_paddr  = phys(j71t_resume_idle_cpu)
every CPU: cpu_data+0xb8 = resume_idle_cpu_paddr
```

**Evidence.** 0x72d84f0 decompiled; the monitor stub is
`j71t_monitor_call_smc11` (0x718bc1c) = `smc #0x11` with x0=call number
(`monitor_call` in public `machine_routines_asm.s`). `0x800 = MONITOR_SET_ENTRY`
and `0x801 = MONITOR_LOCKDOWN` come from public `cpu.c` and
`machine_routines.c`; the binary calls 0x801 from the `machine_lockdown`
stage of `kernel_bootstrap` (0x71f0788), which on this kernel means
`KERNEL_INTEGRITY_WT` is enabled and the monitor takes over integrity
enforcement after that point.

### 5.2 Reset dispatch (what runs after a power-down)

`j71t_reset_vector_dispatch` at `fffffff007189000` (also mapped at
`...7189400`):

```text
OSLAR_EL1 = 0;  DAIF |= 0x3c0;  VBAR_EL1 = 0xfffffff00718a000
mpidr = MPIDR_EL1 & 0xff
scan CpuDataEntries (16-byte entries) for cpu_data->phys_id (+0x1c8) == mpidr
handler = cpu_data->cpu_reset_handler (+0xb8)
accept only resume_idle_cpu or start_cpu, else spin forever
call handler
```

**Evidence.** Decompiled 0x7189000; public `start.s` `reset_vector` is the
same algorithm. The binary compares `MPIDR_EL1 & 0xff`; the public source
uses `& 0xffff` when `HAS_CLUSTER` is defined, which does not change the
result for a one-cluster, two-core part.

### 5.3 The resume handler

`resume_idle_cpu` → `start_cpu` → `common_start_cpu` (`0x718b000/0x718b00c/`
`0x718b018`), then the common setup at `0x718b270`:

* restore `TPIDR_EL0`/`TPIDRRO_EL0`, `SPSel`
* TCR/TTBR0/TTBR1/MAIR/early VBAR/`SCTLR` from the kernel's globals, i.e.
  the MMU is rebuilt from scratch
* `tlbi vmalle1; dsb; isb`
* re-apply Twister "chicken" registers, and `CPU_CFG` bit30 only for the
  Samsung C0 part (`MIDR part 0x004 rev 0x20`) — **not** j71t's TSMC part
* `arm_init_idle_cpu` (`0x72dae28`): restore thread TPIDR_EL0 from
  `cpu_data+0x30`, `CNTKCTL_EL1`, normal `TTBR0`, `tlbi vmalle1is; dsb; isb`,
  clear DAIF.FIQ, re-enable CoreSight/debug as needed, `CNTV_CTL_EL0 = 1`,
  `CNTP_CTL_EL0 = 2`, then `cpu_idle_exit(from_reset = TRUE)`.
* `cpu_idle_exit` (0x72d7e78) reconnects accounting, the idle notifier, the
  timer and the CoreSight EDLAR unlock, then `Idle_load_context` restores
  `FPCR` and returns into the saved thread context.
* `arm_init_idle_cpu` finally translates the physical return address that
  `cpu_idle_exit` produced back to a virtual address through the kernel's
  physical map (the long PA-range chain at 0x72db08c); that value is the
  branch target.

The same file owns the cold-start twin: `j71t_cpu_start` (`fffffff0072d82e4`)
is the `ml_processor_info_t.start` callback. For a secondary core it writes
`start_cpu`'s physical address into that CPU's `cpu_data+0xb8`, links the
`cpu_data` into the active thread's per-CPU chain, and calls the platform
object's `+0x560` method, which is the PMGR CPU-start controller write
(`0x20e0d4004` / `0x20e0d4008` per m1n1). That is how a fresh core is
released; idle resume reuses the *other* handler (`resume_idle_cpu`) that the
boot CPU installs over it.

**Evidence.** Decompiled 0x72dae28/0x72d7e78; public `start.s`/`cpu.c`
(`arm_init_idle_cpu`, `cpu_idle_exit(from_reset)`).

### 5.4 What is actually saved

| State | Where | Restored by |
|---|---|---|
| `x19–x30`, SP/LR, `FPCR`, `d8–d15` | current thread kernel context (`tpidr_el1+0x130`) | `Idle_load_context` + function return |
| active thread | `cpu_data+0x30` | `arm_init_idle_cpu` |
| idle deadline / rtcPop | `cpu_data+0x108` / `+0x110` | `cpu_idle` / `cpu_idle_exit` |
| sleep token `'SUSP'` | `cpu_data+0x1dc` | only system sleep |
| reset handler (physical resume entry) | `cpu_data+0xb8` | reset dispatch |
| physical CPU id | `cpu_data+0x1c8` | reset dispatch |
| CoreSight bases | `cpu_data+0xd0/0xd8/0xdc` | `cpu_idle_exit` |
| MMU control (`TCR/TTBR/MAIR/SCTLR`) | kernel globals | `common_start_cpu` |
| cluster config shadows (4 × 64-bit) | PMGR object `+0x369f8…` → ACC `+0x50/0x48/0x40/0x100` | first-core hook |
| wake timebase | `wake_abstime`, `ml_set_reset_time(UINT64_MAX)` | `ml_arm_sleep` / `cpu_timebase_init` |

**Not** saved by this path: L1/L2 contents, TLBs, branch predictor state
(m1n1 sets `ACC_CFG` BP retention bits, which may or may not survive a
mode-3 power-down), or any PMGR power-state register of the *individual*
core. The observed code performs no L1 clean/invalidate before the
power-down — **Inference:** Apple's hardware/firmware owns the cache
transition, and that is exactly the part we cannot copy without evidence.

### 5.5 IORVBAR, the monitor and the AOP

* `IORVBAR` is MMIO at the base of each CPU's `cpu-impl-reg`:
  `0x202050000` (CPU0) and `0x202150000` (CPU1), 4 KiB windows, bit0 = lock,
  bits47:12 = base. The live ADT also gives `reg-private` `0x202010000` /
  `0x202110000` and `coresight-reg` at the same bases, and
  `acc-impl-reg = 0x202240000` on both CPU nodes.
* Public `start.s` states the ownership rule explicitly: `_start` is placed
  at a fixed offset inside `LowResetVectorBase` "so that iBoot can compute
  the reset vector position to set IORVBAR using only the kernel entry
  point. Reset vector = (`__start` & ~0xfff)".
* The kernel then calls `MONITOR_SET_ENTRY` (SMC #0x11, x0=0x800) with
  `phys(&LowResetVectorBase)`. That is an Apple monitor ABI, **not PSCI**,
  and the implementing monitor is not in the kernelcache, so its
  IORVBAR-write/lock/security behaviour cannot be recovered from this
  artifact. Later, `MONITOR_LOCKDOWN` (x0=0x801) is issued at
  `machine_lockdown`; after that the monitor enforces kernel-text integrity,
  which is strong evidence that `MONITOR_SET_ENTRY` is a boot-time,
  pre-lockdown operation.
* **AOP.** The AOP is a separate IOP (`aop@10800000`, `compatible
  iop,s8000`, firmware `ipad6faop`, `aot-power = 1`, four AIC interrupts) and
  is not referenced anywhere in the core idle or reset path. Its only
  power-management appearances are (a) `pmgr`'s `ap-wake-sources`
  (`AOP.ExternalReq`, `AOP.CPUWakeupAP`, `AOP.OutboxNotEmpty`,
  `AOP.APWatchdog`, `AOP.APWakeupTime`) and (b) `AppleS8000PMGR`'s
  `readAOPGlobalReg`/`writeAOPGlobalReg` methods with a bounds check against
  `AOP_GLOBAL_AP_CONTROLS` (+0x1000). The concrete AOP register offsets are
  not recovered. **Inference:** AOP is the wake master and the PMGR-side
  AOP registers are how the AP asks it to keep or drop power; neither is on
  the per-core idle path.

## 6. How iOS suspends the system

### 6.1 The IOKit side

`IOPMrootDomain::sleepSystem` (`fffffff0077e5a10`) first runs the sleep
gate `IOPMrootDomain_systemSleepCheck` (`fffffff0077e5e00`), which returns 0
and logs `"PMRD: System sleep prevented by %s"` when an assertion, an
idle-sleep disable, or a pending wake request blocks the transition. On
success it asks the platform object at `this+0xa8` to perform the transition
(`FUN_fffffff0077660ac(this+0xa8, 8, 3, 0x67)`); that is the standard IOKit
cascade: every `IOService` in the PM tree gets `setPowerState` for the
system-sleep state, children before parents. That cascade is where
NVMe/ANS, PCIe, USB, display and the PMGR domains are quiesced.

**Evidence:** the two functions above. **Inference (well-established IOKit
semantics, consistent with our own Linux observations):** endpoint before
port, port before platform, platform before CPU sleep.

### 6.2 The platform sleep call and `_ml_arm_sleep`

Before any of this, IOKit initialises its platform action lists
(`PE_platformActionsInit`, `fffffff007794f00`): six keyed lists behind one
`IOLock`, published as `IOPlatformSleepAction`, `IOPlatformWakeAction`,
`IOPlatformQuiesceAction`, `IOPlatformActiveAction`,
`IOPlatformHaltRestartAction` and `IOPlatformPanicAction`. Drivers attach
handlers to them as properties; the quiesce list is the one that runs last
(`IOCPURunPlatformQuiesceActions`, `fffffff007794d14`, walks the list head at
`fffffff0078e6fe0`). This is the closest iOS equivalent to our
`dev_pm_ops.suspend_noirq`/`.poweroff_noirq` tail — and, unlike Linux, it is
explicitly a *platform* list rather than a device-tree walk.

The last pre-sleep hook is `AppleARMCPU`'s platform sleep function
(`AppleARMCPU_platformSleep`, `fffffff0059b6448`), reached through the PLK
pointer at `fffffff006ce6f28`:

```text
if (!this->boot_cpu)                      // secondary CPUs
        function-enable_core->call(0,0,0)  // "eroC", PMGR object at this+0xd8
else                                       // boot CPU
        IOCPURunPlatformQuiesceActions()   // platform last-chance callbacks
_ml_arm_sleep()
```

The AppleARMPE side of that call (the `IOPlatformExpert`/`AppleARMPE` method
that ultimately invokes the CPU driver's platform sleep action) is not
separately identified here: the AppleARMPE class methods in this kernelcache
carry no symbols, and the PLK pointer table gives the callee, not the caller.
The hook above is nevertheless the last kernel code before `_ml_arm_sleep`,
so it is the contract that matters for the port.

`_ml_arm_sleep` (0x72d83d4, the real body behind the PLK stub at
0x59eb16c):

```text
boot CPU:
  for each other cpu_data: wait cpu_sleep_token(+0x1dc) == 'SUSP'
  wake_abstime = ml_get_timebase(); ml_set_reset_time(UINT64_MAX)
  own cpu_sleep_token = 'SUSP'
  CoreSight EDLAR unlock (0xfb0 = 0xc5acce55), DBGPRCR = 0
  serial_go_to_sleep()
  Twister_arm64_prepare_for_sleep_nonreturning(1)
      CPU_CFG  = 0x01000000                  (bit24 deep sleep)
      CYC_OVRD |= 0x03000000                 (WFI mode 3, force power-down)
      loop { dsb sy; isb; wfi }
secondary CPU:
  cpu_sleep_token = 'SUSP'
  DSB  (the cache hook at 0x7181554/0x718155c compiles to dsb/ret here)
  Twister_arm64_prepare_for_sleep_nonreturning(1)   (same loop)
```

The decompiler places the CPU-token wait loop after the `noreturn` helper;
the only ordering consistent with the code and with public `ml_arm_sleep()` is
the one above (secondaries park, boot CPU waits, then everyone enters the WFI
loop).

**Evidence:** 0x72d83d4 and 0x718bb9c decompiled/disassembled; public
`ml_arm_sleep()` in `cpu.c` matches (sleep-token wait, `wake_abstime`,
debug-lock clear, `arm64_prepare_for_sleep(true)`); `ARM_CPU_ON_SLEEP_PATH`
is `0x50535553` (`'SUSP'`).

Note what is *not* there: no low-memory suspend signature, no iBoot `SleepToken`
buffer copy, no PSCI `SYSTEM_SUSPEND`. The wake path is the reset vector
(below), not an in-place resume.

### 6.3 Quiesce order and per-device state

Plausible order, from the IOKit cascade plus the concrete Apple callbacks
found in this and earlier rounds:

1. **Blockers/assertions** — `IOPMrootDomain::sleepSystem` gate.
2. **Storage: NVMe/ANS.** NVMe `CC.SHN = 01` (normal shutdown), wait
   `CSTS.SHST`; the ANS coprocessor keeps running on its own power rail.
   This is the same state Linux's `nvme-pci` simple-suspend reaches.
   Evidence: [PCIe suspend round 8](../pcie/suspend.md) and
   [ANS/kexec rounds 3–5](../boot/ans-kexec.md); re-verified here only at
   the level of "the NVMe driver's sleep path owns the endpoint".
3. **PCIe root ports.** `AppleS800xPCIePort::disableGated` sequence:
   PME_Turn_Off (`port+0x8c = 0x31`), wait for ack, wait for L2, assert
   PERST, `perst-to-refclk-delay-us`, refclk off, port power off, port
   enable off. `disablePortHardware` (`fffffff005ee72c8`) is its inverse
   partner `enablePortHardware` (`fffffff005ee73dc`);
   `setLTSSMEnabled` is `fffffff005ee7a60`.
4. **USB.** DWC2/PHY quiesced by their drivers; the ADT marks
   `USB@80250 → USBCTRL@80258 → USBOTG@80290` as the DWC2 hierarchy.
5. **Display.** Panel + eDP/DP + DISP0. The `soc-tuner` node exposes a
   `function-dispidle_ctrl` callback (payload length 0x57, fourcc that
   decodes to `ADPi` with the same convention as `cpuI`) and
   the kernelcache has `AppleDisplayPipeSetSystemIdleStateFunction`;
   the concrete register writes are not recovered here. Our current Linux
   policy (keep DISP0/DP always-on) is the conservative equivalent.
6. **Clock/power domains.** PMGR domains for everything actually off;
   `always-on` domains (PMS, PMS_SRAM, AP GPIO, and our DISP0/DP) stay.
7. **CPUs.** Section 6.2. Secondary cores park first, boot CPU last.
8. **AOP stays alive** and remains the wake source; it is not quiesced.

**Evidence vs inference.** (2)(3)(5) are backed by driver code or ADT
properties; the *ordering* is IOKit generic plus our Linux noirq
observations, not a captured iOS trace. We have no iOS boot trace of the
suspend cascade, so the order of (4)/(5) and the exact display sequence
remain inference. What is certain is the *shape*: a keyed list
(`IOPlatformQuiesceAction`) runs on the boot CPU immediately before
`_ml_arm_sleep`, after the IOKit `setPowerState` cascade has already put the
devices down.

### 6.4 Wake

Wake sources in the live ADT (`pmgr/ap-wake-sources`) are all AOP channels:
`AOP.ExternalReq`, `AOP.CPUWakeupAP`, `AOP.OutboxNotEmpty`, `AOP.APWatchdog`,
`AOP.APWakeupTime`. The AP-side wake then re-enters through the reset vector
(5.2): every core, including the boot CPU, comes out of reset at
`LowResetVectorBase`, is dispatched by physical id, and runs
`resume_idle_cpu` → `arm_init_idle_cpu` → `cpu_idle_exit(TRUE)`.

**Inference:** since `_ml_arm_sleep` is `noreturn` and both cores sit in the
mode-3 WFI loop, the boot CPU's return to `cpu_idle_exit(TRUE)` after a
system sleep must be the same reset path as a deep-idle core wake, with the
additional `cpu_timebase_init`/`wake_abstime` correction. Nothing else in the
kernelcache resumes a suspended CPU in place, which is consistent with the
public comment that "the reconfig engine script brings the CPU out of reset
at the kernel's reset vector".

## 7. iOS sleep vs our s2idle

| Aspect | iOS system sleep | Our s2idle today |
|---|---|---|
| CPU state | `CPU_CFG.bit24=1`, `CYC_OVRD` mode 3, all cores parked in a `dsb/isb/wfi` loop | `WFI` in place, still mode 2 from m1n1; no deep bits |
| CPU return | reset vector → `resume_idle_cpu` → MMU rebuild → `cpu_idle_exit(TRUE)` | fall-through from WFI |
| Timebase | `wake_abstime` + `ml_set_reset_time(UINT64_MAX)`; `cpu_timebase_init` fixes the clock on wake | normal Linux timekeeping, `tick_freeze` |
| NVMe/ANS | SHN before sleep; ANS rail stays up; controller state is re-established by the reset path | `nvme-pci` simple suspend (SHN), async controller reset on resume; **round 5 saw ANS RDY timeouts after s2idle** |
| PCIe | PME_TO/L2/PERST/refclk+power off for sleeping ports | level 0 refuses; level 1 keeps D0/L0; level 2 sleeps only the GPIO-powered (Wi-Fi) port; level 3 (all ports, NVMe included) untested |
| USB | driver-quiesced; gadget may disconnect | `dwc2.s2idle_keep_gadget=1` keeps a connected gadget and holds `ps_usbotg`/`ps_usbctrl` |
| Display | display pipe idle + backlight off | backlight only; DISP0/DP kept `always-on` after 0043 |
| AOP | stays on, is the wake master | no AOP power/wake driver; `a9/aop` is a read-only status collector and whether the AOP survives our boot path is unmeasured ([aop-sensors.md](../periph/aop-sensors.md)) |
| Platform quiesce | `IOCPURunPlatformQuiesceActions()` list | none (no such list in Linux) |

The important consequence: **our s2idle is strictly shallower than iOS
sleep, and the difference is exactly the reset-resume contract.** That is why
copying iOS's deep-sleep register writes into `s2idle` cannot work today.

## 8. What broke after s2idle in round 5, in this picture

Round 5 (see [ans-kexec.md §Round 5](../boot/ans-kexec.md)) reported a panic
right after resume: console output immediately on the button press, phoenix
ran, stage2 linked both PCIe ports but ANS never reached RDY (CAP
`0x20f00103ff`, VS `0x10001`, CC/CSTS `0`, BFH `0xbadadd`, no host fault
100 ms after enable).

What this study adds:

* iOS's system sleep powers sleeping PCIe ports down (PME_TO → L2 → PERST →
  refclk/power off) and brings the endpoint up on wake as a cold device: link
  retrain, NVMMU reprogram, then ANS `CC.EN`. Our mode 2 keeps the NVMe port
  in D0/L0 while the NVMe driver has already sent SHN. The ANS firmware
  therefore sees a controller that was shut down but a link that never
  dropped. The round-5 register dump (CC=0, CSTS=0) is consistent with
  "endpoint was disabled and never re-enabled", which the NVMe reset path
  should have done; a stuck/again-failed ANS enable then looks exactly like a
  dead controller.
* The fix direction that matches iOS is: on resume, if the ANS enable does
  not reach RDY within a bounded time, **reset the endpoint the way a cold
  boot would** (assert PERST with the port powered, clear CC.EN/SHN, poll
  RDY=0, release PERST, redo link + NVMMU) instead of retrying `CC.EN` on a
  possibly wedged firmware.

**Inference:** the round-5 panic itself (before phoenix) is not explained by
this; it needs its own record. The ANS state machine explanation above is
consistent with the observed registers but not proven.

## 9. Linux implementation plan

### 9.1 cpuidle states we can add (and one we cannot)

1. **`WFI` (state 0)** — existing `cpuidle_apple_a9_idle`, guarded. Keep.
2. **`WFI-CG` (state 1)** — existing opt-in. Keep it as an A/B tool, not as
   a power feature: m1n1 already leaves `CYC_OVRD` in mode 2, so expected
   gain is zero (round 5/6 conclusion, now explained by `chickens.c`).
3. **No third state yet.** A mode-3 state is a *reset* state and must not be
   exposed until 9.2 is done. Do not add a fake `retention`/`deep_wfi` state
   for A9: m1n1's `supports_arch_retention()` gate (AIDR) excludes A7–A11 and
   its `deep_wfi()` deliberately refuses to enter that mode there; the
   round-6 probe is the on-device confirmation.
4. **Parameters for the future state**, from the platform itself:
   `pmgr/cpu-power-gate-latency-us = 0xC350` (50 ms) is Apple's own
   power-gate latency for this SoC. Any real power-down state should use at
   least that as `target_residency` and must measure `exit_latency` rather
   than assume it. It also justifies keeping a `menu`/`teo` governor
   threshold well above 50 ms.
5. **Cluster idle is not a cpuidle state.** The existing `a9/idle` PMGR
   floor relaxation is the only safe cluster-level knob; the real Apple
   handshake (§4) needs the perf-controller BUSY wait + CPMS interlock and
   the four ACC restores, none of which are understood well enough to port.

### 9.2 What the resume path must do (the gate for any deeper state)

Ordered, smallest-first:

1. **Measure IORVBAR and its lock on both CPUs, before Linux boots**, from
   m1n1: read `0x202050000` and `0x202150000`, record value + bit0. Also
   record `CPU_CFG`/`CYC_OVRD` (the round-6 `probe=1` line does the latter).
   If bit0 is set and the base is not a page we control, a reset-based idle
   state is impossible without a monitor call we do not have.
2. **Reserve a trampoline page and own IORVBAR.** Two acceptable designs:
   * m1n1 writes the trampoline's physical address into IORVBAR for both
     cores just before entering Linux (it already does this for its own
     `_vectors_start` when `apple_sysregs_unlocked`), and keeps its
     secondary-start spin table working by having the trampoline
     distinguish "cold start" from "resume"; or
   * Linux writes IORVBAR itself at boot (same MMIO, same lock check) and
     takes over secondary startup from the spin table.
   Either way the trampoline must be identity-mapped, reserved in
   `/reserved-memory`, and never freed. Note what happens today: IORVBAR
   points at m1n1's `_vectors_start` (written by `smp_start_cpu` when
   `apple_sysregs_unlocked`), and m1n1's secondary entry loop calls
   `deep_wfi()`, which on A7–A11 is a plain `wfi`. A Linux core that reset
   today would therefore land in m1n1's spin loop and never return to Linux —
   which is exactly why the reset path must be owned before mode 3 is used.
3. **Trampoline contents** (mirror of §5.3, EL1 only):
   mask DAIF, set a private VBAR, read `MPIDR_EL1 & 0xff`, look up a
   per-CPU record, validate a magic + generation counter (this is also the
   proof that the core really reset rather than fell through WFI), restore
   `TPIDR_EL0`, `SPSel`, `TCR/TTBR0/TTBR1/MAIR/SCTLR`, `tlbi vmalle1; dsb;
   isb`, re-apply the Twister chicken registers, restore debug/CoreSight if
   used, `CNTV_CTL_EL0=1`, `CNTP_CTL_EL0=2`, then branch to `cpu_resume`.
   `cpu_suspend()`/`cpu_resume` (arch/arm64/kernel/sleep.S) give us the
   context save/restore and the `mpidr` hash check; they do **not** give us
   the vector ownership, the EL1 state restore or the cache contract.
4. **Cache/coherency contract.** iOS performs no L1 clean before the
   power-down and leans on hardware; we cannot assume that. Minimum
   requirement before testing: clean+invalidate L1 D by VA for the saved
   context, `dc civac` the trampoline record, and treat L2 as
   platform-owned. Prove it with a CPU1-only round trip first (CPU0 kept
   running), with a trampoline counter that distinguishes reset from
   WFI fall-through.
5. **Cluster protocol.** Only after a single-core round trip works: last-core
   entry must poll the perf-controller BUSY bit and the cluster-status word
   and perform the CPMS/CPM handshake; first-core exit must restore the four
   ACC values. Until the producers of those values are known, keep both
   cores out of cluster power-down.
6. **Bounded refusal + watchdog.** Every entry checks `CPU_CFG.bit24 == 0`
   and `CYC_OVRD[25:24] == 2` first (round 6 already does this); a rejected
   entry must return `-EBUSY` without executing WFI. The trampoline must
   have a "no valid record → park in a known state" path, and the platform
   watchdog must be able to reset the SoC if a core never comes back.

### 9.3 What our s2idle path should do differently

1. **ANS/NVMe.** Keep `nvme-pci` simple suspend (SHN) for mode 1/2, but on
   resume replace "retry CC.EN" with the cold-boot recovery: bounded RDY
   poll, then PERST + `CC=0` + RDY=0 poll + release + link/NVMMU redo. This
   is the same shape as the round-5 phoenix `crash_quiesce`, reused on the
   normal resume path when the enable times out. Do not add an ANS PMGR
   domain or an RTKit command: the ADT has neither. Ordering is part of the
   fix: the port must retrain, the NVMMU tables must be reprogrammed and
   `apple_dart_resume` must have run **before** the NVMe reset path touches
   ANS registers, otherwise the enable writes can go to a dead link.
2. **PCIe.** Stay at mode 1/2 until the ANS recovery above is proven on
   hardware; then move to mode 3 for both ports, because that is what iOS
   does and it is where the screen-off power actually is. Mode 3 requires:
   port power-off for port 0, NVMMU reprogram after retrain, `apple_dart_resume`
   for DART1, and a bounded retrain with the existing `down_ports` fallback.
3. **Display.** Keep DISP0/DP `always-on` and backlight-only until a display
   driver exists; iOS's `function-dispidle_ctrl` is the eventual hook, not a
   register recipe.
4. **USB.** Keep `dwc2.s2idle_keep_gadget=1` only for NBD-root sessions; for
   a battery measurement, run from internal storage so the gadget and PHY
   can actually suspend.
5. **genpd/wake path.** Keep `GENPD_FLAG_ACTIVE_WAKEUP` and the
   `apple,always-on` list; that is the Linux equivalent of iOS's always-on
   domains. Do not add PSCI-ish "system suspend" calls.
6. **Do not enable deep WFI in s2idle** as a shortcut. `s2idle` keeps the
   kernel running and expects in-place resume; mode 3 would send every core
   to the reset vector with no installed trampoline. The deep-sleep bits
   belong to the future "real suspend" implementation built on 9.2.

### 9.4 Validation gates (orchestrator only)

1. **Read-only baseline.** m1n1 debug: IORVBAR value/lock on both CPUs,
   `CPU_CFG`, `CYC_OVRD`; then Linux with `cpuidle_apple_a9_idle.probe=1`
   (no enable) to confirm the same values from EL1. Stop if IORVBAR is
   locked to a page we do not own.
2. **Trampoline smoke test without power-down.** Install the trampoline,
   have CPU1 jump to it deliberately (e.g. a debug hook that stores a
   generation counter and branches), and verify it reaches `cpu_resume` and
   the scheduler with a valid context. This tests the restore path, not the
   power-down.
3. **CPU1-only reset round trip**, CPU0 running, with the trampoline
   counter proving a real reset. Repeat ≥100 times with timer, IPI, USB and
   touch traffic; any hang, lost timer or corruption rejects the design.
4. **Both-core reset**, then system-sleep-style parking. Only after 3.
5. **ANS recovery test.** One `systemctl suspend` at mode 1, then at mode 3,
   with the new resume recovery armed; require both ports at L0, NVMe root
   mounted, and no ANS RDY timeout. Keep the panic/pstore setup from round 5
   armed during these tests.
6. **Energy.** Paired discharge measurements with `scripts/idle/measure.py`
   (screen-off, backlight off, same brightness/radio/root medium) before
   claiming anything. WFI accounting alone predicts zero.

## 10. Open questions

1. Which four ACC-window registers are restored on first-core exit, who
   writes their software shadows, and what the CPMS/CPM interlock bit
   (`map 9 + 0x20` bit22) actually gates.
2. What `PMCR0 = 0x3200` before WFI is for (PMU routing vs. counter halt),
   and whether it matters for Linux's PMU driver.
3. IORVBAR lock state and value on j71t after palera1n → pongoOS → m1n1;
   and whether `MONITOR_SET_ENTRY`/`MONITOR_LOCKDOWN` leave it writable.
4. Whether the L1/L2 caches and the branch predictor survive a mode-3
   power-down, or whether firmware/hardware flushes them.
5. The display and USB quiesce order inside iOS sleep, and whether the
   AOP's `aop-retention` mode is entered during AP sleep.
6. Whether the `readAOPGlobalReg`/`writeAOPGlobalReg` window is used in the
   sleep path (the methods exist; no caller was recovered).

## Appendix A — function map used in this document

| Address | Name (shared Ghidra name where it exists) | Note |
|---|---|---|
| `fffffff0072e2560` | `j71t_machine_idle` | WFE shortcut or `Idle_context` |
| `fffffff00718b6a0` | `j71t_Idle_context` | saves thread context |
| `fffffff00718b720` | `j71t_Idle_load_context` | restores FPCR, returns |
| `fffffff0072d7d10` | `j71t_cpu_idle` | WFI at `...7e48` |
| `fffffff0072d7e78` | `j71t_cpu_idle_exit` | shallow/reset exit |
| `fffffff0072d84f0` | `j71t_cpu_machine_idle_init` | SMC 0x800 at `...859c` |
| `fffffff00718bc1c` | `j71t_monitor_call_smc11` | `smc #0x11` |
| `fffffff007189000` | `j71t_reset_vector_dispatch` | MPIDR → `CpuDataEntries` |
| `fffffff00718b000` | `j71t_resume_idle_cpu` | reset resume entry |
| `fffffff00718b00c` | `j71t_start_cpu` | cold start entry |
| `fffffff00718b018` | `j71t_common_start_cpu` | MMU rebuild |
| `fffffff0072dae28` | `j71t_arm_init_idle_cpu` | EL1/timer/debug restore |
| `fffffff0072d82e4` | `j71t_cpu_start` | platform CPU start |
| `fffffff0072d83d4` | `_ml_arm_sleep` | park + deep sleep |
| `fffffff00718bb9c` | `Twister_arm64_prepare_for_sleep_nonreturning` | `CPU_CFG`/`CYC_OVRD` mode 3 |
| `fffffff00718bbcc` | `Twister_arm64_force_wfi_clock_gate` | mode 2 |
| `fffffff0071f0310` | kernel bootstrap tail | SMC 0x801 at `...71f0788` |
| `fffffff0059b5460` | `AppleARMCPU::start` | `function-cpu_idle` bind |
| `fffffff0059b62e8` | AppleARMCPU idle enter | PMGR selector `0x10` |
| `fffffff0059b633c` | AppleARMCPU idle exit | PMGR selector `0x02` |
| `fffffff0059b6448` | AppleARMCPU platform sleep | quiesce actions + `_ml_arm_sleep` |
| `fffffff005a3e5bc` | ApplePMGR idle dispatch | op byte routing |
| `fffffff005a3bf78` | ApplePMGR idle enter | bookkeeping |
| `fffffff005a3bdd8` | ApplePMGR idle exit | bookkeeping + timeout hint |
| `fffffff005a3b514` | `ApplePMGR_cpuIdleAccounting` | counters/mask/sticky |
| `fffffff005e91348` | `AppleS8000PMGR_initDriver` | DT props → policy flags |
| `fffffff005e92330` | S8000PMGR last-core hook | map9 BUSY/status wait + handoff |
| `fffffff005e92538` | S8000PMGR first-core hook | ACC restore + perf restore |
| `fffffff005e91c24` | S8000PMGR cluster handoff | bit22 toggle + waits |
| `fffffff005e92c74/78` | per-core enter/exit | `ret` |
| `fffffff005e926ac` | `PMGR+0x9c080` bit31 | enable/disable |
| `fffffff005e926fc` | PMGR restore sweep | `0x40000..0x40210`, `(2..5)<<14` |
| `fffffff0077e5a10` | `IOPMrootDomain::sleepSystem` | sleep gate + PM cascade |
| `fffffff0077e5e00` | `IOPMrootDomain_systemSleepCheck` | `"PMRD: System sleep prevented by %s"` |
| `fffffff007794d14` | `IOCPURunPlatformQuiesceActions` | walks list `0x78e6fe0` |
| `fffffff007794f00` | `PE_platformActionsInit` | initialises sleep/wake/quiesce/active/halt/panic action lists |
| `fffffff007794660` | `_PE_cpu_machine_quiesce` | per-CPU quiesce hook |

## Appendix B — physical map used here

| Block | Address | Source |
|---|---|---|
| CPU0 / CPU1 `cpu-impl-reg` (IORVBAR+0) | `0x202050000` / `0x202150000` | live ADT |
| CPU0 / CPU1 `reg-private`, `coresight-reg` | `0x202010000` / `0x202110000` | live ADT |
| shared ACC window (`acc-impl-reg`, PMGR map 13) | `0x202240000` (64 KiB) | live ADT |
| CPU perf/ACC command/status (PMGR map 9) | `0x202220000` (+0x20 cmd, +0x50 status) | live ADT + Linux cpufreq |
| CPU-start controller | `0x20e0d4000` (+0x4 status, +0x8+4·cluster start) | live ADT / m1n1 |
| PMGR base | `0x20e000000` | live ADT |
| PMGR CPU0/CPU1 power state | `0x20e080000` / `0x20e080008` | live ADT (`ps-regs`) |
| PMGR CPM power state | `0x20e080040` | live ADT |
| PMGR `pms` / `pmp` / `pms-sram` | `0x20e080120` / `0x20e0802e8` / `0x20e0802f0` | [ans-kexec round 4](../boot/ans-kexec.md) |
| AOP | `0x10800000` (windows incl. 0x1c000, 0x9b000) | live ADT |
| PMGR `cpu-power-gate-latency-us` | `0xC350` = 50 ms | live ADT |
| PMGR `l2c-acc-sleep` | `0xffffffff` | live ADT |
| AOP wake sources | `AOP.ExternalReq`, `AOP.CPUWakeupAP`, `AOP.OutboxNotEmpty`, `AOP.APWatchdog`, `AOP.APWakeupTime` | live ADT |
| AOP `aot-power` | always-on marker on `aop@10800000` | live ADT |
| `aop-retention`, `aop-profiler-*`, `aop-target` | AOP kext property names; `aop-target` is in the ADT, the retention property is not | kernelcache strings / live ADT |

## Appendix C — reproduction

```sh
# Anchors already recorded by the previous round
python3 scripts/power/kc-idle-evidence.py
python3 scripts/idle/resume-evidence.py --index docs/power/evidence/idle-resume-j71t.json

# This round's new evidence (shared Ghidra server, no device)
scripts/ghidra.py decompile_function_by_address address=fffffff0072d83d4   # _ml_arm_sleep
scripts/ghidra.py decompile_function_by_address address=fffffff00718bb9c   # Twister sleep helper
scripts/ghidra.py decompile_function_by_address address=fffffff0072d84f0   # idle init + SMC 0x800
scripts/ghidra.py decompile_function_by_address address=fffffff0059b6448   # platform sleep hook
scripts/ghidra.py decompile_function_by_address address=fffffff0059b62e8   # AppleARMCPU idle
scripts/ghidra.py decompile_function_by_address address=fffffff005a3e5bc   # PMGR idle dispatch
scripts/ghidra.py decompile_function_by_address address=fffffff005e92330   # last-core hook
scripts/ghidra.py decompile_function_by_address address=fffffff005e92538   # first-core hook
scripts/ghidra.py decompile_function_by_address address=fffffff0077e5a10   # IOPMrootDomain::sleepSystem
scripts/ghidra.py decompile_function_by_address address=fffffff007794d14   # quiesce action runner
scripts/ghidra.py decompile_function_by_address address=fffffff007794f00   # platform action lists
scripts/ghidra.py decompile_function_by_address address=fffffff0072d82e4   # j71t_cpu_start

# vtable slot check (AppleS8000PMGR)
scripts/ghidra.py read_memory address=0xfffffff006e106e0 length=8          # +0x8b0
scripts/ghidra.py read_memory address=0xfffffff006e106e8 length=8          # +0x8b8
```

Pinned public sources used for semantics (already fetched by
`scripts/idle/fetch-sources.py`): `osfmk/arm64/cpu.c` (`cpu_idle`,
`cpu_idle_exit`, `cpu_machine_idle_init`, `ml_arm_sleep`),
`osfmk/arm64/start.s` (reset vector, IORVBAR comment),
`osfmk/arm64/machine_routines_asm.s` (`monitor_call`,
`arm64_prepare_for_sleep`, `arm64_force_wfi_clock_gate`, retention WFI),
`osfmk/arm64/machine_routines.c` (`machine_lockdown` → `MONITOR_LOCKDOWN`),
m1n1 `src/chickens.c`, `src/utils.c`, `src/smp.c`, `src/cpu_regs.h`.

**Renames.** The shared server stalled for a while on a `get_xrefs_to` query
and was not restarted; after it recovered, these symbols were saved with
`scripts/ghidra-mcp.sh save` (no analysis state was rebuilt):

```text
fffffff0059b6448 AppleARMCPU_platformSleep      fffffff0059b62e8 AppleARMCPU_idle
fffffff0059b633c AppleARMCPU_idleExit           fffffff0072ba488 cpu_idle_pmu_account
fffffff0072e7b04 cpu_idle_arm_deadline          fffffff0072d83d4 ml_arm_sleep_body
fffffff0072db34c j71t_kvtophys                  fffffff005a3bf78 ApplePMGR_cpuIdleEnter
fffffff005a3bdd8 ApplePMGR_cpuIdleExit          fffffff005e92330 AppleS8000PMGR_lastCoreIdle
fffffff005e92538 AppleS8000PMGR_firstCoreExit   fffffff005e91c24 AppleS8000PMGR_clusterHandoff
fffffff005e926fc AppleS8000PMGR_restoreSweep    fffffff0077e5e00 IOPMrootDomain_systemSleepCheck
fffffff007794f00 PE_platformActionsInit         fffffff007794d14 IOCPURunPlatformQuiesceActions
```

The previously shared names (`j71t_*`, `AppleS8000PMGR_initDriver`,
`Twister_arm64_*`, `ApplePMGR_cpuIdleAccounting`) were already present.
