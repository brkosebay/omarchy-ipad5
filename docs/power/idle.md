# Independent A9 idle implementation and device test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23, j71t / S8003. No device access or device commands by this agent.
Kernel branch: **`a9/idle`**, based directly on `hoolock` (`6831bc701a`).
Head: **`5609d32abbc4a9289ce70a81639e35372f8737cc`**.
The patch series and config are under [patches/idle](../release/OMISSIONS.md).
The kernel head and build hashes are recorded in `build/kernel-idle/COMMIT`
and `build/idle/resume-validation.txt` after validation.

**Returning deep idle is not implemented.** [Round 6](idle-resume.md) establishes
that A9 lacks M1-style architectural retention and recovers the ordinary iOS
reset-resume dispatch. Firmware/reset-vector ownership and the complete cluster
protocol still prevent a full power-down implementation. The returning experiment
remains `WFI-CG`. Round 6 adds an opt-in register probe and fixes unsafe fallback
to plain WFI; it does not add a deeper state.

## What changed

1. `CONFIG_ARM_APPLE_A9_IDLE`: independent cpuidle driver, default inactive.
   `cpuidle_apple_a9_idle.enable=1` registers guarded WFI accounting.
   Adding `cpuidle_apple_a9_idle.clock_gate=1` exposes `WFI-CG` on j71t.
   Each entry checks the Apple TSMC Twister MIDR, rejects a deep-sleep CPU_CFG
   or force-down CYC_OVRD configuration, saves CYC_OVRD, forces its bit25,
   executes WFI and restores the complete saved register with ISBs.
   An already clock-gated boot mode accounts as state0. Since round 6,
   state0 checks that CPU_CFG.bit24 is clear and CYC_OVRD[25:24]=2;
   unsafe entry requests return `-EBUSY` without executing WFI. Unsupported
   MIDRs are rejected without implementation-register reads. A rejected
   deep configuration must never fall back to an unguarded WFI.
   The 10 us / 1000 us state policy is conservative experimental policy,
   not measured latency. System suspend uses baseline WFI.
   GPR/FP/EL1 state is retained by clock gating; this path does not need the
   CPU_PM/context-save machinery required by a context-losing retention state.
   `cpuidle_apple_a9_idle.probe=1`, also default off, logs per-online-CPU
   MIDR, CurrentEL, PFR0, AIDR, CPU_CFG and CYC_OVRD without registering
   this driver unless `enable=1` is also given.

2. PMGR: `pmgr_pwrstate.idle_power_gate=1` plus the per-domain
   `apple,idle-power-gate` property permits clearing MIN[19:16] **only when
   genpd powers a domain off**. The saved active floor is restored on resume.
   Existing target/actual acknowledgement and hierarchy remain in use.
   A failed opt-in off request attempts bounded active-state rollback.
   The permission list covers MCA0–4, ISP, MSR, JPEG and video engines;
   no active stream, MCLK reference, child domain or consumer hold is removed.
   Already-off domains are not forced through an extra transition at probe.
   With a zero boot minimum, this feature changes no effective power policy.

3. USB PHY: `ausb.runtime_pm=1` plus `apple,enable-runtime-pm` enables parent
   runtime PM before generic PHY creation. The generic PHY core then holds
   the parent from `phy_power_on()` to `phy_power_off()`.
   Runtime resume replays the existing device tunables, reset and delays;
   runtime suspend uses the existing PHY power-off sequence before genpd
   powers down. Wake-capable/wakeup-path suspension is refused.
   Probe marks the initial runtime state suspended so the first operation
   always runs hardware setup, including after firmware-owned boot setup.
   **An active DWC2 gadget keeps its hold.** A silent network link or unplugged
   cable alone does not release DWC2's reference; controller lifecycle work
   is still required for transparent disconnected-gadget autosuspend.

4. [measure.py](../../scripts/idle/measure.py) records signed battery current
   in uA, voltage in uV, status, capacity, charge, temperature, external supply
   online flags, CPU state counters, proc-stat idle/iowait and frequencies.
   Before/after inventories include genpd, per-device runtime PM, IRQs and
   cpufreq time-in-state. JSONL is flushed each sample; the summary integrates
   samples by actual monotonic elapsed time. Discharge magnitude and energy
   use only intervals whose endpoints both say `Discharging`.
   It flags charging/missing data, counts distinct current/voltage pairs,
   and reports reset or unavailable cpuidle counters.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Register evidence and unresolved reset-resume work

Reproduce [selected evidence](../release/OMISSIONS.md) without accessing a device:

```sh
python3 scripts/idle/evidence.py > build/idle/evidence.json
python3 scripts/power/kc-idle-evidence.py
```

Ghidra program SHA256 is
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
`0xfffffff00718bbcc` reads `S3_5_C15_C5_0` (CYC_OVRD), ORs `0x02000000`
and returns. `0xfffffff00718bb9c`, called by `_ml_arm_sleep`, conditionally
writes `0x01000000` to `S3_5_C15_C4_0` (CPU_CFG), ORs `0x03000000` into
CYC_OVRD, then loops over barriers/WFI forever. The instruction stream agrees
with Apple's separate Twister deep-sleep and force-clock-gate code in
[XNU 8796.141.3](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/osfmk/arm64/machine_routines_asm.s).

The ADT decodes CPU implementation windows as `0x202050000`/`0x202150000`,
and the shared ACC window as `0x202240000`. **CPU_CFG is a system register,
not an offset inferred inside those MMIO windows.** The ADT idle function
references PMGR provider phandle24/fourcc `cpuI`; that is a callback reference.
PMGR device map + index*8 gives CPU0/1 `0x20e080000`/`0x20e080008`,
MCA0 `0x20e080168`, ISP `0x20e0802c8`, DISP0 `0x20e0802b0`,
USBCTRL `0x20e080258`, USBOTG `0x20e080290`. The shared CPM domain also
decodes from the ADT to PMGR+`0x80040`; that address does not establish a
last-core control protocol.
No CPU/CPM MMIO write is made.

[Asahi's driver](https://github.com/AsahiLinux/linux/blob/asahi/drivers/cpuidle/cpuidle-apple.c)
provides a useful CPU_PM/GPR-save model for M-series power-down. Its whitelist
does not include A9. [Round 6](idle-resume.md) recovers A9's ordinary idle
reset dispatch and context restoration, including boot-time SMC #0x11
vector registration and shared last-core/first-core hooks. The firmware
side of that handoff, Linux reset exception level and complete cache/coherency
and cluster restore protocol remain unresolved. A9 has no architectural
retention; full power-down needs an installed, verified reset-resume path.
Merely setting CPU_CFG.bit24 and copying the Asahi bits25:24 sequence would
not supply that missing protocol. The iOS round-4 `IDLE` bucket has no depth
split, so 77%/84% CPU idle residency cannot validate retention either.

## Build and integration

> Private graphics-dependent excerpt omitted from this source snapshot.


`a9/idle` is opt-in in integration: its fragment is added only when explicitly
listed. `scripts/idle/build.sh` can replay the exported series onto a fresh
`hoolock` idle worktree. Debug information stays disabled. Use an integration
kernel for device work: the standalone branch intentionally lacks the other
workstreams' battery, storage, audio and input drivers.

Offline validation passed at the recorded head: full `Image.gz` plus 104 DTBs,
clean kernel tree, all idle fragment options retained, debug info disabled,
and ten recorder tests (including a complete fake-sysfs recording), plus
36 mocked-hardware scenarios for the actual C entry guards.
The exported five-patch series reproduces the final Git tree. Round 6's
reverse-order merge check passes with `a9/power` at `c4a1c8f7b2`.
Round-5 merge checks passed with all five hardware branches and `a9/power`
at `39703809f0`; those merged changed objects and j71t DTB compiled with
`W=1`. The round-6 idle object also compiles with `W=1`. The DTB reports
the pre-existing USB simple-bus unit-address warning. No physical validation
or energy measurement was performed. Final artifacts:

```text
Image.gz sha256 307185393863c8dffe9b1abb60d2b1f031bf9e01089fc06eaf39b4fa838fedef
j71t DTB sha256 074ae95fca3b8cf2f54ce7c4c07ce35295dbcae57167b0285795dfc552929555
```

## Ranked device commands — orchestrator only

Use the existing boot/payload workflow to add one experiment's command-line
parameters per fresh boot. Copy `measure.py` to `/var/tmp/measure.py` by the
orchestrator's transport. No SSH commands are run by this sub-agent.

1. **Baseline and measurement validity.** Stop automatic screen/suspend
   timers for the entire comparison, restoring their previous state afterward;
   for a running Omarchy user session this includes `hypridle.service`:

   ```sh
   systemctl --user is-active hypridle.service
   systemctl --user stop hypridle.service
   cat /proc/cmdline
   findmnt -no SOURCE,FSTYPE /
   sudo cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
   sudo python3 /var/tmp/measure.py /var/tmp/idle-baseline-desktop \
     --mode desktop --duration 300 --interval 5 --settle 60 \
     > /var/tmp/idle-baseline-desktop.stdout
   sudo python3 /var/tmp/measure.py /var/tmp/idle-baseline-dark \
     --mode screen-off --backlight-off --duration 300 --interval 5 --settle 60 \
     > /var/tmp/idle-baseline-dark.stdout
   ```

   Screen-off here is **backlight off while CPUs remain running**. Firmware
   scanout is retained; simpledrm has no validated full display power restore.
   The script saves/restores brightness on normal exit, SIGINT and SIGTERM.
   SIGKILL/power loss cannot run cleanup; record the prior brightness for
   manual recovery. Without `--backlight-off`, screen state is operator-owned.
   Screen mode is a label plus recorded actual brightness, not proof of DPMS.

   Use the same brightness, radio state, workload, temperature band, governor,
   root medium and observer interval in each pair. Close terminal updates and
   leave stdout local. Five-minute intervals are a starting point: repeat
   baseline/variant/baseline at least three times. Gauge telemetry is cached;
   repeated values can occur and distinct pairs do not establish its cadence.
   USB/NBD/charging measurements are useful for regression/residency checks;
   battery-saving claims need real discharge without external power. Leave
   the Mac's existing NBD server running throughout an NBD-root session.

2. **WFI accounting only**, fresh boot with
   `cpuidle_apple_a9_idle.enable=1` and all other idle experiment flags absent:

   ```sh
   cat /sys/devices/system/cpu/cpuidle/current_driver
   cat /sys/devices/system/cpu/cpu*/cpuidle/state*/name
   # Expect apple_a9_idle and state0=WFI on both CPUs.
   ```

   Repeat both measurement commands with fresh output directories. Check
   increasing WFI time/usage, shared-policy low-frequency residency and IRQ
   deltas. This step should save no energy by itself.

3. **PMGR idle floors**, fresh boot adding only
   `pmgr_pwrstate.idle_power_gate=1` to the WFI baseline:

   ```sh
   dmesg | grep -E 'idle floor|Failed to reach|rollback'
   sudo cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
   cat /sys/bus/platform/devices/20a0a0000.mca/power/runtime_status
   cat /sys/bus/platform/devices/20a0a0000.mca/power/runtime_usage
   ```

   Expect allowed domains with no users/active children to be off and MCA0
   to suspend only after real stream/MCLK holds end. An initial already-off
   domain is not cycled just to clear a floor. A nonzero saved floor and an
   actual active→off transition are prerequisites for marginal benefit.
   Repeat the established audio stream/stop/resume test 100 times and touch,
   brightness, charger/gauge, Wi-Fi and USB checks. MCA remaining on with an
   MCLK/codec hold is an audit finding, not permission to discard the hold.
   Any timeout or rollback error rejects the change for that device boot.

4. **Forced clock gate**, fresh boot adding only
   `cpuidle_apple_a9_idle.clock_gate=1` to the WFI baseline:

   ```sh
   cat /sys/devices/system/cpu/cpu*/cpuidle/state*/name
   # state1=WFI-CG; an already-mode2 boot still accounts entries as WFI.
   stress-ng --cpu 2 --cpu-method all --verify --timeout 30s
   ```

   Use the project's existing known-good stress/memory/FP validation if
   stress-ng is unavailable. Check repeated idle/load transitions on both
   CPUs, timer progress, Home/button/touch response, USB traffic and memory/FP
   integrity; repeat at least 100 wake cycles before longer dark-screen runs.
   Zero state1 residency can mean the boot mode already forces clock gating.
   Unsafe CPU_CFG/CYC_OVRD configurations now increment rejected attempts
   without executing WFI; check the state `rejected` counters and round-6
   probe. Neither result is evidence to force additional bits.
   Hang/reset/corruption means remove the parameter and
   tether-boot the last known-good payload. No watchdog timeout can guarantee
   software recovery from a broken CPU sleep/wake path.

5. **USB runtime PM**, fresh boot with only `ausb.runtime_pm=1` added to WFI:

   ```sh
   cat /sys/bus/platform/devices/20c000030.phy/power/runtime_status
   cat /sys/bus/platform/devices/20c000030.phy/power/runtime_usage
   dmesg | grep -E 'opt-in runtime PM|Failed to reach|ausb'
   ```

   Platform names above come from DT; enumerate the matching platform device
   if a loader/kernel uses a different suffix. With DWC2 running, the PHY
   must stay active across traffic, ordinary idle and the established
   connected-gadget s2idle path. Do not force its `power/control`, gate its
   domain, or unbind DWC2 while `/` or any mounted filesystem uses NBD/USB.
   Only with verified internal root and an independent local/Wi-Fi console,
   have the orchestrator release/reacquire the controller using the existing
   gadget teardown/rebind workflow. Verify the PHY becomes suspended after
   the last power reference and re-enumerates correctly on rebind, including
   restoration of the saved configfs gadget/UDC binding. Repeat 100 cycles.
   Silent-traffic autosuspend with DWC2 still holding its reference is not
   implemented by this patch.

After individual passes, combine the successful flags and repeat the same
desktop/dark baseline pairs. Restore `hypridle.service` only if it was active
before the experiment. Keep existing PCIe/s2idle stage and wake-path settings.

## Expected gains and remaining risks

No gain has been measured on this branch; no mA, watts or battery-life number
is predicted. WFI accounting has zero expected direct gain. WFI-CG can help
only if boot firmware is not already forcing the same mode; its benefit may
be zero or negative after extra entry/exit overhead. PMGR floor relaxation
can help only for a retained nonzero floor on a domain that actually idles;
unused camera/video blocks may already be power-gated by the normal sweep.
USB gain requires the controller to release the PHY and cannot be obtained
by suspending an NBD-root link. Panel backlight reduction is measurable but
does not establish display-pipeline gating or CPU retention.

Additional CPU deep-idle gain requires a verified A9 reset-resume protocol;
A9 lacks M1-style architectural retention. Forcing power-down bits could
reset/hang a CPU or lose CPU/cache state. PMGR changes can expose missing
consumer context restore; MCA IRQs,
MCLK, DMA and first-event-after-idle need physical validation. USB has link
and wake regression risk. All new hardware policies default off and require
fresh-boot opt-in; a failed hardware wake cannot be repaired by the WFI fallback.
Display DISP0/DP and wake/core/fabric domains retain their existing guards.
