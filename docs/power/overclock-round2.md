# A9 thermal and clock implementation, overclock round 2

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Historical runbook. Current voltage decoder, guarded offsets, ladder and
full integration: [round 3](overclock-round3.md).** Nonzero-offset rejection
below describes the superseded round-2 implementation.

2026-10-23. Kernel branch `a9/power`, based on `d8ed7cdb76`; head
`39703809f0cc9bd446bed3f1a195584d7270dd09`. Only kernel-workstream commits were made. No iPad
connection, device write, or workspace commit was made by this agent.

Owner policy: the orchestrator may run small RAM-only steps autonomously,
accepting tuning crashes. The hard voltage ceiling is Apple's highest stock
CPU voltage +50 mV, with no parameter to raise it. This supersedes round 1's
+25 mV/per-step approval text. Thermal protection and emergency stock fallback
remain mandatory.

Implemented and compiled: six die-sensor thermal zones and hwmon channels,
CPU cooling maps, 1848 MHz stock boost, and a default-off guarded CPU clock
write path at unchanged stock voltage. **Voltage increases are not implemented:**
every nonzero delta is rejected. The complete physical DAC conversion and the
remaining PSINFO timing/rail fields are still unproved. No stable overclock or
live Linux temperature result is claimed. The new kernel must be tested on the
device by the orchestrator, starting with thermal bring-up.

## Sensors and thermal policy

The matched iOS class is **AppleT700XTempSensor**, not AppleT8015TempSensor or
the older ApplePMGRTempSensor. Its prelink personality explicitly matches
`tempsensor,s8000`. Evidence and source identities:
[overclock-round2.json](../release/OMISSIONS.md).

Shared Ghidra entry `fffffff005f1a424` is sensor startup; renamed
`AppleT700XTempSensor_start`. `fffffff005f1b938`, renamed
`AppleT700XTempSensor_readQ6`, proves readBK bit 31 is valid, with signed Q6
Celsius in the low bits. `sensor-accuracy=2` uses sign bit 14, and accuracy 1
uses sign bit 15. Linux converts to milliCelsius with `signed_q6 * 1000 / 64`.
Invalid data returns an error; Linux does not reproduce iOS's fallback 30 C
value. Offline Ghidra defines the additional vtable-proven helpers that the
shared analysis had not recognized:

* `5f1b714`: alarm programming (not used by Linux).
* `5f1b7a0/5f1b808`: enable/disable conversion.
* `5f1b870/5f1b8d0`: start/freeze accumulation, CTL0 set/clear bit 3.
* `5f1ba18 -> 5f1ba40`: timer command-gate callback; freeze, read BK/count,
  publish current/average/maximum, restart accumulation.
* `5f1c2d8/5f1c3a8`: control save/restore paths.

New driver `drivers/thermal/apple-s8003-thermal.c` maps the saved live ADT's
SOC page `0x20e0c0000` and TCC page `0x202230000`. SOC channels use offsets
0/0x100/0x200; TCC clear registers are at 0x408/0x438/0x468, and TCC2's count
really is 0x238. All accesses are 32-bit, matching the absent `reg-64-bit`
property and iOS's default path, despite TCC's eight-byte register spacing.

The driver enables conversion/accumulation and samples every 100 ms. It
requires valid BK, enabled conversion, nonzero non-overflowing accumulation
count and a plausible code. Cached data expires after 500 ms. It preserves
CTL1/CTL2, params/trim, alarm settings and SoCHOT registers. iOS only writes
trim params when `apply-trim-workaround` is nonzero; all six saved nodes have
zero. The hardware supplies converted temperatures using its boot-programmed
calibration; no factory trim payload, fuse layout, or invented per-chip slope
is copied into Linux. Preservation is implemented; calibration accuracy across
the palera1n/pongoOS/m1n1 handoff still needs live validation.

> Private graphics-dependent excerpt omitted from this source snapshot.

The DT uses 80 C passive / 5 C hysteresis, 90 C hot and 100 C critical. These
are conservative lab margins below the ADT's 100 C sensor alarm, **not recovered
thermalmonitord product curves**. Apple's saved ACC SoCHOT thresholds are
120/125 C; the PMGR node skips programming its trips, so iOS's 105/115 C defaults
do not prove that page's programmed values. Linux does not raise or reprogram
those hardware protections. Hot and critical callbacks synchronously block
experimental targets, force a stock request and restore the modified table
before the normal thermal core shuts down at 100 C. A five-second emergency
poweroff fallback covers an orderly shutdown that stalls. The callback uses
RCU and the hardware spinlock, avoiding the control mutex while holding a
thermal-zone mutex. A generation check prevents an in-progress arm from
clearing a new thermal latch.

Freshness depends on hardware accumulation behaving as in iOS. Before tuning,
validate count restart, BK validity, load response, quantization, latency,
unbinding/error behavior, and agreement with iOS. Software timestamps cannot
prove an independently stuck-valid hardware sensor is healthy.

## CPU clocks, boost and write protection

The captured PSINFO words fit **all** stock clocks with:

```
frequency_kHz = 24000 * PSINFO1[11:4] / (PSINFO1[3:0] + 1)
```

For example 396 MHz uses 66/4, 600 uses 75/3, 912 uses 76/2,
1200 uses 100/2, 1512 uses 63/1, 1800 uses 75/1, and 1848 uses 77/1.
The iOS reciprocal ADT labels `65536000 / period` agree. This is a strong
stock-correlated encoding model, not independent proof of VCO limits or of
safe overclock settling times.

**code1 is voltage-related, not the frequency multiplier.** The high-byte
codes 31/40/61/80/104/116 fit all seven integer ADT voltage descriptors with
`floor((450000 + 3125*code1)/1000)` mV. The fitted stock maximum is 812500 uV,
but it is not a measured rail voltage or independently proved DAC transfer
function. A physical +50 mV limit cannot honestly be derived from that fit
alone. Therefore `delta_uv > 50000` returns `ERANGE` and **all other nonzero
deltas return `EOPNOTSUPP` before any register write**. The +50 mV macro is
compiled into the driver and has no runtime override. Actual implemented
voltage-byte changes are zero on both copied fields.

Level 8 is labeled **1848 MHz**, turbo-only and disabled by default. Its
PSINFO2 max-load field stays **2**, versus stock level 7's 15. Hardware enforces
that field; Linux reports the actual state from ACC+0x50. Nothing establishes
that the number is a count of online cores. No restriction override is used.
The current WFI-only cpuidle may still prevent useful boost delivery. A
request for level 8 is not evidence of running at 1848 MHz. The public
[Hoolock m1n1 source](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/cpufreq.c)
independently identifies the field and marks unrestricted boost experimental.

The experimental rungs are **1896, 1944, 1992, 2040, 2088, 2160 MHz** at hardware
levels 9..14. These are the 24 MHz grid, rather than promises of exactly 1.9/2.0
GHz. 2260 MHz/A9X is not enabled. At arm time every stock OPP must match the
captured clock model, boost must have matching stock voltage bytes/load=2, and
slots 9..14 must be exact copies of the observed stock level-2 placeholder.
Each configuration first returns to a stock state, then clones level 7 into
one dormant slot, changes **only PSINFO1[11:4]**, reads back both words, and
allows that OPP. The slot keeps level 7's voltage/timing/load fields; Apple's
levels 0..8 are never overwritten. The unknown timing fields are a remaining
experimental risk, not a fully decoded sequencer recipe.

`apple_soc_cpufreq.a9_tuning=1` is a read-only boot parameter, default false.
debugfs `a9_tuning/control` is mode 0200 plus CAP_SYS_RAWIO; there is no raw
address/word interface. Commands:

```
arm
heartbeat
configure <one supported kHz value> 0
revert
```

All six zones must work and be below 75 C to arm/configure. A fixed two-second
heartbeat and 100 ms guard monitor are mandatory. At 80 C, on any sensor error,
stale data, missed heartbeat, or failed configure, the guard latches, blocks
all above-1800 MHz targets under the same spinlock as hardware transitions,
requests a stock state, then restores dormant-slot backups once actual and
target states are stock. A lower cooled state is preserved. A separate
frequency-QoS cap constrains governors; fast switching is disabled for A9.
Requests validate the actual hardware level, not the frequency-table index,
and A9 transitions have pre/post busy polls. Revert failures remain visible
and prevent rearming. Root can rearm after an ordinary successful revert and
cooldown; no approval prompt is required.

A software heartbeat cannot execute during a hard kernel lockup. The ladder
therefore requires its own stoppable Apple hardware-watchdog FD at a verified
3..15-second timeout (requests 10 s). It feeds that FD only after sensor,
kernel-guard, workload and checksum checks pass. If stock restoration cannot
be confirmed, it leaves the watchdog running for a reset. A hard lockup resets
the device; that resets these RAM-only clock-table changes. An already-owned
watchdog is not taken away from another service. Thermal emulation is excluded
by Kconfig from the tuning build so an emulated cool temperature cannot bypass
the guard.

## Remaining iOS decoding

`AppleS8000PMGR_setPerfState`, `fffffff005e92094`, supplies the request masks,
extended PSINFO bank and optional live-voltage-byte propagation; it selects
boot-built states rather than constructing a new arbitrary PLL state here.
`5e91508` initializes tuning; `5e91b0c` copies the two bytes into PMGR+a0018 and
+a0010 when changing voltage-control mode, and uses +a0008 for SOC. This does
not independently identify the two physical rails or calibrate their DACs.

`AppleS8000DWI`, `634f1f0/634f8ec/634fac8`, uses optional buck0..2 arrays,
encodes a selector/code word and submits at +0x80/+0x84 or +0xa0/+0xa4 with
settling handling. This saved ADT has none of those arrays. The D2365 PMIC
kext has also been extracted; its existing gauge/ADC machinery is not evidence
that the PSINFO byte has a specific CPU buck transfer function. No direct DWI,
I2C, PMIC, fuse, syscfg, NVRAM or storage write path was added.

The pulled thermalmonitord was inspected in a separate Ghidra project:
`createNewProduct:` at `10000aea8`, `updateDieTempTarget` at `100046b20`,
`dieTempTarget/defaultDieTempTarget` at `100046b74/100046b7c`, and voltage-array
import at `1000485e4`. The die target is derived and clamped using per-instance
parameters; the getter reads a field rather than a universal literal trip.
The binary also names `/System/Library/ThermalMonitor/` and `-Info.plist`.
The actual j71t policy resources have not been pulled, so no board-specific
thermalmonitord trip is claimed. A later orchestrator-only iOS capture should
include those resources and matched T700X/SoCHOT services. Those missing files
do not prevent conservative lab trips, but may justify lowering them.

## Host reproduction and verification

```
python3 -B scripts/power/round2-evidence.py --extract
python3 -B scripts/power/validate-round2.py
scripts/power/build-round2.sh
# Complete build; isolated from other workstreams' integ builds:
scripts/power/build-round2.sh --full
```

The build script uses `INTEGRATION_TREE=power-r2-integ` by default. The first
shared-integ attempt failed with a missing object and a partially written .cmd
file during concurrent build activity, consistent with a shared-tree collision. The isolated
attempt has its own source/build output and does not kill other processes.

> Private graphics-dependent excerpt omitted from this source snapshot.

Outputs are `build/kernel-power-r2-integ/{Image.gz,dtbs/s8003-j71t.dtb,config}`.
Use `TREE=power-r2-integ` when the orchestrator prepares its normal payload;
this agent did not replace the shared `build/kernel-integ/` payload or boot
the device. The seven new kernel patches are ordered by
`patches/power/overclock-r2.series` and require the round-1 head `d8ed7cdb76`.

Validation compiles the **actual kernel helper functions** with fake MMIO and
AddressSanitizer/UBSan, exercising stock correlation, all six rung encodes,
voltage rejection before stores, thermal decisions, heartbeat gates, restored
backups, a timed-out revert, preservation of a lower cooled state, synchronous
thermal fallback and a thermal trip racing with arm/configure. Ten
Python fault tests cover missing zones/cooling, raised trips, stale/hot data,
checksum failure, cleanup after failed revert, retention of the hardware
watchdog on an unconfirmed revert, and an undelivered target. Object builds
with tuning both enabled and disabled and DT compilation pass. The source
diff has zero checkpatch errors (metadata/binding/help warnings remain).
This is host verification; the driver,
hardware-watchdog reset, real passive cooling, sensor calibration and clocks
above stock have not been tested on the iPad.

Ghidra reproduction for missing helper definitions (private, small projects):

```
python3 scripts/power/oc-re.py fffffff005f1a424 fffffff005f1b938 fffffff005e92094 fffffff005e91b0c fffffff00634f8ec
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec \
  ~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/power/ghidra-local.py \
  --binary build/power/oc-r2/kexts/AppleEmbeddedTempSensor.macho \
  --output build/power/oc-r2/thermal-local \
  --function 0xfffffff005f1b714 --function 0xfffffff005f1b7a0 \
  --function 0xfffffff005f1b808 --function 0xfffffff005f1b870 \
  --function 0xfffffff005f1b8d0 --function 0xfffffff005f1ba40
```

## Orchestrator device sequence

Boot the new integrated Image/DTB through the existing RAM-only workflow.
Add `apple_soc_cpufreq.a9_tuning=1` to that boot's command line; retain the
observer's `apple_a9_dvfs_ro.enable=1` if wanted. Copy `ladder.py` and the
existing `stock-log.py` to the Linux device through the orchestrator's normal
workflow. `stress-ng` must already be installed. debugfs must be mounted.

Run as root on **Linux**, in order:

```sh
cat /sys/kernel/debug/a9_thermal/snapshot
python3 ladder.py thermal-check --seconds 30 > thermal-check.jsonl
cat /sys/kernel/debug/a9_tuning/status
```

Then characterize stock load first, using the actual six zone IDs discovered
by their `apple-a9-*` types (battery/NVMe temperatures are insufficient):

```sh
zones=()
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat "$z/type")" in apple-a9-*) zones+=(--soc-zone "${z##*/}");; esac
done
python3 stock-log.py --stress --duration 300 --interval 0.25 \
  --cutoff-millic 80000 "${zones[@]}" > stock-thermal-load.jsonl 2>stock-stress.log
```

Check sensible warming/cooling and fresh count/BK data, match against saved
iOS readings, and prove actual clocks fall when stock cooling is commanded.
The kernel's decision/restore paths have host fault coverage; hardware critical
poweroff should be tested through an isolated fault test, not by heating to
100/120/125 C. If readings are invalid or unchanged under appropriate changing
load, stop and return the raw thermal snapshot/log; do not substitute battery
temperature or fabricate a trim value.

After thermal validation and cooldown, run the actual heartbeat/revert test,
then boost, then the clock ladder:

```sh
python3 ladder.py guard-check > guard-check.jsonl
python3 ladder.py boost --minutes 5 --logdir boost-r2 > boost-r2.jsonl
python3 ladder.py ladder --minutes 10 --through-khz 1992000 \
  --logdir ladder-2g-r2 > ladder-2g-r2.jsonl
# Once the lower rungs pass, this repeats them and extends to 2160 MHz:
python3 ladder.py ladder --minutes 10 --through-khz 2160000 \
  --logdir ladder-2160-r2 > ladder-2160-r2.jsonl
```

Each rung runs `stress-ng --verify` plus a fixed known SHA256 check, logs
temperatures, actual state-correlated clock and battery current/voltage, and
reverts before moving on. Boost uses one stress worker; higher clocks use two.
No target observation means no pass; experimental rungs require delivery in
at least 80% of samples. These state labels still need independent CPU cycle
timing to confirm the PLL frequency. Max passing clock is recorded per voltage
delta in each log directory's `summary.json`; this version can characterize
only delta 0. A failed rung stops the run. It does not try a higher voltage.

If boost is never delivered, retain Apple's max-load field and resolve the
idle/load dependency rather than claiming success or removing the restriction.
Do not infer sustained 1848 MHz operation from a configured/requested state.

Emergency manual fallback, always allowed even when tuning was not enabled:

```sh
printf 'revert\n' > /sys/kernel/debug/a9_tuning/control
cat /sys/kernel/debug/a9_tuning/status
```

Require `armed=0`, `dirty=0`, `revert_error=0` and actual frequency <=1800000.
Nothing is installed into persistent boot configuration or written to storage
firmware/calibration. A failing hardware revert is a reset condition, not a
successful return to stock.
