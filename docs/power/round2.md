# Power implementation round 2 — 2026-10-22

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

## Deliverables and verification

- `f12b3d7157`: opt-in WFI-only CPUidle driver. `CONFIG_ARM_APPLE_A9_CPUIDLE=y`
  and boot parameter **`cpuidle_apple_a9.enable=1`** register `apple_a9_wfi`
  on S8000/S8003 with Twister CPU nodes. Default off. Existing `cpu_do_idle()`
  instruction path, governor accounting and per-CPU usage/time; no PSCI,
  system-register changes, CPU powerdown, extra OPP or CPU_PM notification.
  The nominal 1us latency/residency is a WFI baseline convention, not measured
  hardware latency. This adds observability, not a promised power saving.
- `dc33fef48a6b01d14aba06fe87b8c000d93585f6`: S8000 SPI opt-in domain
  autosuspend, **`spi_apple_s8000.autosuspend=1`**. Default retains the probe
  PM reference. Opt-in gives the SPI core a reference across each whole message,
  releases the probe reference after child registration, and autosuspends after
  1000ms. Resume resets CTRL, inactive internal CS, CFG and STATUS; every PIO
  transfer already programs clock divisor, word delay, mode and counts. No DMA
  or controller interrupts are enabled. Both j71t SPI1 (codec control) and SPI3
  (touch) use this driver. Runtime suspend is assumed to permit PMGR gating;
  actual domain loss/recovery and external slave behavior need device tests.
  The reference clock remains enabled: it is a fixed clock on this board.
- `d59013ae2d`: shorten WFI description to fit the cpuidle ABI; fixes the
  warning from the first integrated build.
- Exported three patches plus `power.config` under `patches/power/`.
- `scripts/integrate.sh` includes `power.config` when **`a9/power` is explicitly
  in its branch arguments**. Existing default integration branches are unchanged.
- Read-only DTB consumer audit, runtime inventory and five-second telemetry
  scripts under `scripts/power/`; hardened iOS output directories and status logs.

Reproduced build:

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

Image SHA256: `33e7d894e03823ec3e631dc1b7cdfbb8c896d503548af4e3a06510ac5da32af3`.
J71t DTB SHA256: `15250791173f8a8453ef039338c9f6797c9f5eef904eb4c44e33ca82f001dc68`.

Also passed kernel `git diff --check`, shell syntax, Python compilation, mocked iOS shell
success/failure propagation and existing-output refusal, and
arm64 iOS helper compile with `-Wall -Wextra -Werror` plus ad-hoc signing.
An image built here is evidence of compilation, not permission to replace a
newer orchestrator image; check COMMIT before deployment.

## Why deep WFI is not enabled

Hash-checked evidence: [twister-idle-raw.txt](../release/OMISSIONS.md),
[clock-gate helper](../release/OMISSIONS.md),
[powerdown fragment](../release/OMISSIONS.md).
Reproduce offline with `python3 scripts/power/kc-idle-evidence.py`.

The **actual j71t kernelcache** uses `S3_5_C15_C5_0`:

- `0xfffffff00718bb9c`: nonreturning sleep helper. If x0 != 0, write
  `S3_5_C15_C4_0=0x01000000`; then RMW CYC_OVRD OR `0x03000000`;
  repeat `dsb sy; isb; wfi; b` forever. This is not a returning idle primitive.
- `0xfffffff00718bbcc`: returning force-clock-gate helper, RMW OR
  `0x02000000`, then return. No corresponding returning clear/restore or
  CPU-retention helper was established by this scan.

The saved live ADT CPU0/CPU1 entries both publish `function-cpu_idle` as
`<1800000049757063>` (provider phandle 0x18, function fourcc), and
`apple,twister`. They do **not** describe Linux `arm,idle-state` nodes,
entry/exit latencies or a resume protocol. The PMGR accounting callback
`5a3b514` maintains core/cluster counts; it is not a complete retention entry.

[Asahi's driver](https://github.com/AsahiLinux/linux/blob/asahi/drivers/cpuidle/cpuidle-apple.c)
saves callee-saved GPRs, ORs bits24/25, loops until ISR_EL1 reports an
interrupt, clears bit24 on exit, and brackets entry with CPU_PM. Its
machine whitelist excludes A9. The matching register encoding is verified;
A9 preserving PC/SP and required EL1/cache/coherency state is **not**.
The [iOS16 XNU sleep code](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/osfmk/arm64/machine_routines_asm.s)
has a separate Twister deep-sleep configuration and a conditional retention
path with context restoration elsewhere. The shipped binary's nonreturning
helper agrees with the full-sleep path, not a proven M1-style idle return.

Next RE prerequisite: recover Twister idle context save/reset entry and its
PMGR last-core/wake protocol, including all state that ordinary CPU_PM
notifiers do not restore. Do not append `apple,s8003` to Asahi's whitelist,
set the CPU_CFG deep-sleep bit, or enable 1844MHz based on this build.
A deep state needs its own reviewed patch and fresh-boot test, not a writable
parameter that can enter an unproved reset path.

## Every enabled DT genpd consumer: runtime-PM audit

Generated from the built j71t DTB by:

```sh
python3 scripts/power/audit-dtb.py build/kernel-integ/dtbs/s8003-j71t.dtb
```

The complete 21-consumer listing is
[j71t-genpd-consumers.tsv](../release/OMISSIONS.md).
This is a **static driver/DT audit**, not a new live domain measurement.
A bound device with runtime PM disabled can hold its domain without an explicit
`pm_runtime_get()`. Driver absence, probe failures, usage count and runtime
status must be resolved with the live inventory before saying it is powered.

> Private graphics-dependent excerpt omitted from this source snapshot.

The framebuffer has two inherited domains disp0/dp and boot-only scanout;
those are explicitly always-on even though it is outside the static SoC
consumer enumeration. Provider parent domains add further holds; the TSV
lists direct consumers, not an invented separate driver per parent. Unused
ISP/MIPI/PMP/media domains with no bound consumer should be handled by
genpd's unused sweep; a live summary is required to confirm they are off.
`GENPD_FLAG_ACTIVE_WAKEUP` is already installed. Removing always-on from
GPIO/disp0/dp or changing IRQ_SAFE is not a cheap runtime-PM fix.

## Exact Linux device test plan (orchestrator only)

Transfer `linux-observe.sh`, `linux-runtime-audit.sh`, `linux-sample.sh` to
the same directory on Linux. Scripts write only their newly-created output
directories. Use writable local storage; keep USB network available for the
first experiment. Debugfs must already be mounted to obtain genpd counters.

1. Fresh boot with neither parameter. Record `uname -a`, `/proc/cmdline`,
   `dmesg`, `sh linux-runtime-audit.sh > baseline-runtime.tsv` and
   `sh linux-sample.sh baseline-idle 60` (300s). Record fixed brightness,
   governor, display state, Wi-Fi/BT state, CPU load, battery status/SOC and
   charging source. Wait for temperatures/current to settle before each run.
2. Fresh boot adding only `cpuidle_apple_a9.enable=1`. Check
   `/sys/devices/system/cpu/cpuidle/current_driver` = `apple_a9_wfi`,
   CPU0/CPU1 `state0/name=WFI`, and increasing `usage` and `time` (us).
   Run `sh linux-sample.sh wfi-idle 60`. Compare counter deltas, IRQ deltas,
   cpufreq time_in_state and temperatures. WFI time is software residency;
   it does **not** prove hardware retention, nor should this patch alone
   materially lower power compared with the prior arch idle loop.
3. Exercise both cores with a fixed workload, memory checksum and SIMD/FP
   correctness workload; verify frequency rises, repeated idle/load cycles,
   timer wake, touch/keyboard and USB responsiveness. Keep CPU hotplug and
   system suspend out of this first variable. If hung, reboot without the
   parameter. Driver registration is boot-only; no writable deep-mode switch.
4. Fresh boot adding only `spi_apple_s8000.autosuspend=1`. Check startup log,
   touch firmware load and audio codec probe. After >1s inactivity read:

   ```sh
   for d in /sys/bus/platform/devices/20a084000.spi /sys/bus/platform/devices/20a08c000.spi; do
       cat "$d/power/runtime_status" "$d/power/runtime_usage"
   done
   sh linux-runtime-audit.sh > spi-runtime.tsv
   sh linux-sample.sh spi-idle 60
   ```

   Use the inventory's exact paths if platform naming differs. Expected:
   suspended status at idle, usage0 and increasing suspended time; genpd
   spi1/spi3 off when no other reference exists. Gating savings are inferred
   until those observations and current readings agree.
5. Repeat 100 idle (>1s) → touch press/drag/release cycles, checking the first
   event after each idle, no stuck contacts, PIO errors or missing releases.
   Repeat 20 playback-start/stop cycles with >1s gaps, volume changes and
   simultaneous touch. Inspect dmesg for genpd timeouts/PIO failures and
   resumed first-transfer correctness. Test driver unbind/rebind only with
   audio stopped and input access retained, as a separate orchestrator step.
   On failure reboot without autosuspend; do not add blanket domain flags.
6. Only after each separate test passes, combine both parameters and repeat.
   System sleep and PCIe stage tests remain a separate campaign.

Battery `current_now` is in **microamps**, `voltage_now` in microvolts;
compare discharge magnitude with `status` recorded, never assume a sign from
a filename. Battery temp is normally tenths Celsius, thermal-zone temp is
millidegrees Celsius; retain source names/units. Reject charging/unknown,
cache-warmup/unavailable, changing SOC/voltage, workload or thermal transients.
Use baseline → experiment → baseline runs, 300s each, same settled conditions;
report medians and spread, plus signed current/status. Optional battery power
proxy is `abs(current_uA)*voltage_uV/1e12` W **only for discharge samples**.
Do not call a USB-charging current difference battery savings. Fuel-gauge
reads themselves add traffic: identical sampling cadence in all runs. UART5,
MCA/MCLK and graphics work may dominate differences; no savings measured here.

> Private graphics section omitted from this source snapshot.

