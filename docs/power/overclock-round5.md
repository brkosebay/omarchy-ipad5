# A9 overclock/power round 5 — sustained envelope and all-load 1848 MHz

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Kernel branch **`a9/power`**, parent `ce7e58fd4f`, head
**`20ddcb7f1c`** (full hash in the build manifest). The two round-5 commits are
`3be8d6c6c2` (shared thermal profile) and `20ddcb7f1c` (all-load clock mapping).
Only `a9/power` was committed. Workspace edits remain uncommitted. No device
connection, boot, live MMIO access, push, or workspace commit was performed.
These commands supersede the [round-4 sequence](overclock-round4.md).

## Owner decision and device evidence

Owner decision, 2026-10-23: **“80 is too low for sustained load.”** The supplied
stock test reached TCC2 74.4 C in five minutes, plateauing near 71 C, with
SOC0/SOC1 near 60 C and a cool back. The subsequent
[round-4 trial logs](../release/OMISSIONS.md) strengthen that decision:
the old 80 C guard eventually reverted a requested boost experiment that was
still actually running at 1800 MHz. These are supplied observations, not
round-5 device results.

The [local evidence summary](../release/OMISSIONS.md), reproduced with
`python3 -B scripts/power/round5-evidence.py`, records:

- Thermal-check and manual/heartbeat guard-check passed on round 4.
- Native boost delivered **zero** 1848 MHz samples: **1102/1102** actual
  1800 MHz in boost, then **1348/1348** in the ladder's first rung.
- Every sample has command **`0x8408`**, status **`0x78`**: PS1/PS2 request
  state 8, status target 8, actual state 7. The software boost request reached
  hardware. A missing global boost toggle or failure to request state 8 cannot
  explain these captures.
- Native slot 8 has PSINFO2 max-load **2**, versus **15** in all-load slot 7.
  The hardware refusal is consistent with that restriction. Its precise
  load/idle gating has not been measured. Both JSONL workload commands specify
  **`--cpu 1`**, despite both CPUs remaining online.
- Boot slot **15** contains the same full PSINFO1/2 placeholder as slot 2,
  independently visible in [handoff-status.txt](../release/OMISSIONS.md).

## One boot-selected thermal profile

There is **no second thermal parameter or userspace trip-writing service**.
These two existing arguments select the entire sustained profile:

```text
apple_soc_cpufreq.a9_tuning=1 apple_soc_cpufreq.a9_passive_millic=90000
```

On j71t, the cpufreq driver exports a shared, probe-order-independent profile
derived from those read-only boot parameters. The thermal driver uses the same
profile when registering SOC0/SOC1/TCC2. It validates the original DT's three
80/90/100 C trips, then changes passive/hot through the locked thermal-core
setter. Trip ordering, hysteresis and existing cpufreq cooling bindings remain
managed by the thermal core. The critical trip is validated but **never written**.

The sustained profile is **90 C passive/throttle/guard revert, 95 C hot,
100 C critical**, arm/configure strictly below **85 C**, cooldown strictly below
**80 C**. Custom tuning passive values are clamped to **70000..92000 milliC**;
arm is passive minus 5000, cooldown passive minus 10000, hot stays 95000 and
critical stays 100000. Negative valid temperatures are compared as signed
integers. An out-of-range parameter reports its **effective clamped** value.

Without `a9_tuning=1`, or with tuning compiled out, the stock profile remains
**80/90/100 C**, arm 75 C and cooldown 70 C; setting the passive argument alone
does not raise trips. The DTB itself retains 80/90/100 C. This opt-in is limited
to j71t. Hardware SoCHOT/alarm configuration, factory trim/calibration,
100 C critical shutdown and its five-second emergency fallback are untouched.

Both debugfs interfaces publish `thermal_profile_format=1`, `profile`,
`profile_tuning`, `passive_millic`, `hot_millic`, `critical_millic`, `arm_millic`
and `cooldown_millic`. Thermal snapshot is now **format=3**, mask **23**, and
must report `profile_ready=1`. Guard status retains format=3. Tools require
matching profiles in both interfaces and exactly matching trips in every
trusted zone. Old kernels, partially applied profiles, missing cooling, stale
samples, duplicate/missing sensors or profile disagreement abort before load.
The quarantined SOC2/TCC0/TCC1 channels remain diagnostics.

`stock-log.py` defaults its cutoff to the selected passive limit; an explicit
`--cutoff-millic` may only lower it. Every trial and stock-load sample checks
both sysfs and raw snapshot temperatures against the selected limit. The tools
do not write thermal trips. The profile is included in JSONL telemetry.

## Getting past native boost

Apple's original slots **0..8**, including state 8's max-load=2, remain intact.
The 1848 MHz turbo OPP now maps to experimental **slot 15**. `configure 1848000 0`
clones all-load stock slot 7, changes its PLL multiplier from 75 to 77, and
retains both voltage codes, timing and max-load=15. This is an experimental
all-load clock at stock voltage, **not a successful native-boost measurement**.
No nonzero offset is accepted at 1848 MHz. Existing 1896..2160 MHz mappings stay
in slots 9..14. Slot 15 is validated against its immutable boot placeholder
before arming and restored on every revert, like the other dormant slots.

Guard status must contain **`clock_profile=s8003-r5-all-load boost_state=15`**.
The kernel refuses the old state-8 OPP mapping before arming; the tool rejects
old clock interfaces. **Use the matching Image and DTB together.** The ladder
now configures 1848 MHz explicitly and runs **two** stress workers for every
rung. It checks actual state, both full PSINFO words, nominal DAC codes and the
guard throughout the trial.

No initial target delivery within **10 seconds**, or a continuous loss of
delivery for 10 seconds, causes a diagnostic event, revert and abort without a
voltage retry. Every rung, including 1848, needs at least 80% target samples to
pass. A failed native boost no longer blocks entry to a different all-load
clock path. Actual delivery of this new path remains a device test.

## Offline verification and artifacts

```sh
python3 -B scripts/power/round5-evidence.py
scripts/power/build-round5.sh --full
```

The suite runs **47 Python tests** and compiles the actual guard/MMIO helpers
and thermal trip application code with ASan/UBSan. It retains round-4 handoff,
voltage ceiling, heartbeat, watchdog and cleanup coverage; adds profile bounds,
90 C behavior, signed samples, trip/guard mismatch, cooldown/arm scaling, slot-15
configure/readback/restore, and early/lost delivery faults. Both tuning-enabled
and tuning-disabled cpufreq/thermal objects and DTBs are built in Docker.
Checkpatch reports zero errors/warnings/checks for the round-5 kernel delta.

Full output: **`build/kernel-power-r5-integ/`**, a detached checkout of pinned
integration **`f53bf861ca`** plus the exact power delta **`49c0339866..20ddcb7f1c`**,
including the orchestrator's `ce7e58fd4f` change. No integration commit/ref is
created. `git merge-tree --write-tree` must equal the staged build tree. `COMMIT`
names the integration base; `DIRTY` identifies the four staged power files.
The build captures the integration config input, disables debug info and
thermal emulation, and verifies the Image, stock DT trips, cooling bindings and
new OPP level. [The build manifest](../release/OMISSIONS.md) records
the complete power hash, merged tree and artifact/source hashes. Shared
`build/kernel-integ/` and `build/kernel-tri/` payloads are not replaced.

## Ordered orchestrator device commands

**Instructions for the orchestrator; none were executed here.** Start from a
healthy stock-clock Linux session descended from a normal LLB/iOS cold boot,
with corrected calibration and a working PSCI/kexec path. Do not kexec from a
dirty/unconfirmed overclock or the old coefficient-corrupting integ31 kernel.
Keep the existing, known-good root/storage arguments and boot initramfs.

Through the orchestrator's transfer workflow, copy the current **`ladder.py`,
`stock-log.py`, `a9_thermal.py`** into **`/root/a9-power-r4/`**. Copy the new
`build/kernel-power-r5-integ/Image.gz` and `dtbs/s8003-j71t.dtb` into its
`kernel-r5/` directory, along with the known-good boot initramfs under the name
`initramfs.gz`. Check their hashes against the build manifest before proceeding.
Retain an external log stream across resets. Use new log directories for
repeat runs. Every tool command below uses the required root bash/cwd wrapper.

### 0. Load and kexec the tuning image with RAM-only profile arguments

The following registers kernel artifacts as a separate entry without changing
default/previous. Its saved cmdline is **stock**, without tuning arguments; only
the command passed to kexec enables tuning. The live FDT must be reused or
merged with m1n1's runtime fixups, never replaced with a pristine DTB directly.

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
mkdir -p logs /run/a9-power-r5
python3 -B - << "PY"
from pathlib import Path
import ladder
print(ladder.ensure_stock(Path("/sys")))
PY
test -d /proc/device-tree/psci
test "$(tr -d "\000" </proc/device-tree/psci/method)" = smc
old=""
args=()
for word in $(cat /proc/cmdline); do
  case "$word" in
    linux_a9.entry=*) old=${word#linux_a9.entry=} ;;
    apple_soc_cpufreq.a9_tuning=*|apple_soc_cpufreq.a9_passive_millic=*|BOOT_IMAGE=*|initrd=*|panic=*|apple_wdt.cold_restart=*|crash_kexec_post_notifiers|crash_kexec_post_notifiers=*) ;;
    *) args+=("$word") ;;
  esac
done
[[ "$old" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
old_dtb="/boot/linux-a9/entries/$old/dtb"
test -s "$old_dtb"
entry=power-r5-20ddcb7f1c
d="/boot/linux-a9/entries/$entry"
if test ! -e "$d"; then
  mkdir "$d"
  cp kernel-r5/Image.gz "$d/Image.gz"
  cp kernel-r5/s8003-j71t.dtb "$d/dtb"
  cp kernel-r5/initramfs.gz "$d/initramfs.gz"
else
  cmp kernel-r5/Image.gz "$d/Image.gz"
  cmp kernel-r5/s8003-j71t.dtb "$d/dtb"
  cmp kernel-r5/initramfs.gz "$d/initramfs.gz"
fi
args+=(panic=0 apple_wdt.cold_restart=0 crash_kexec_post_notifiers "linux_a9.entry=$entry")
printf "%s\n" "${args[*]}" > "$d/cmdline"
cmdline="${args[*]} apple_soc_cpufreq.a9_tuning=1 apple_soc_cpufreq.a9_passive_millic=90000"
gzip -dc "$d/Image.gz" > /run/a9-power-r5/Image
printf "N\n" > /sys/module/apple_wdt/parameters/cold_restart
cat /proc/sys/kernel/random/boot_id | tee logs/boot-before-r5.id
if cmp -s "$old_dtb" "$d/dtb"; then
  kexec -s -l /run/a9-power-r5/Image --initrd="$d/initramfs.gz" --command-line="$cmdline"
else
  python3 /usr/local/lib/linux-a9/linux-a9-fdt-merge \
    --old "$old_dtb" --running /sys/firmware/fdt --new "$d/dtb" \
    -o /run/a9-power-r5/merged.dtb
  kexec -c -l /run/a9-power-r5/Image --initrd="$d/initramfs.gz" \
    --dtb=/run/a9-power-r5/merged.dtb --command-line="$cmdline"
fi
test "$(cat /sys/kernel/kexec_loaded)" = 1
printf "%s\n" "$cmdline" | tee logs/kexec-cmdline-r5.txt
sync
systemctl kexec
'
```

A merge refusal is a stop condition: use the normal cold-boot workflow with
this Image/DTB and those same two arguments. Do not force a pristine DTB or
alter trim to get past a check. Generic `--reuse-cmdline`/entry-less warm boots
may inherit tuning arguments: explicitly use the saved stock cmdline when
leaving this tuning boot. Do not install tuning arguments in a default entry,
service, crash-recovery policy or other persistent boot configuration.

### 1. Thermal-check and identify the effective profile

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
command -v stress-ng
cat /proc/cmdline | tee logs/cmdline-r5.txt
cat /proc/sys/kernel/random/boot_id | tee logs/boot-r5.id
! cmp -s logs/boot-before-r5.id logs/boot-r5.id
cat /sys/kernel/debug/a9_tuning/status | tee logs/status-r5.txt
cat /sys/kernel/debug/a9_thermal/snapshot | tee logs/thermal-raw-r5.txt
python3 -B ladder.py thermal-check --seconds 30 | tee logs/thermal-r5.jsonl
python3 -B - << "PY"
from pathlib import Path
import a9_thermal, ladder
root = Path("/sys")
state = ladder.ensure_stock(root)
ladder.voltage_handoff(state)
assert state["enable"] == "1"
assert state["clock_profile"] == ladder.CLOCK_PROFILE
assert state["boost_state"] == "15"
p = a9_thermal.profile(root)
assert p == dict(profile="sustained", profile_tuning=1, passive_millic=90000,
                hot_millic=95000, critical_millic=100000,
                arm_millic=85000, cooldown_millic=80000)
assert max(ladder.sensors(root)[0].values()) < p["arm_millic"]
print("round-5 profile/handoff/stock PASS", p)
PY
'
```

Require `thermal-check-pass`, profile `sustained` in both interfaces, thermal
format3/mask23/ready1, live/boot control **0**, routing **0x1782**, handoff_ok1,
no dirty/configured slots and matching three-zone trips/cooling. Check the
coefficient groups still agree. Unexpected handoff values are a stop condition;
never write voltage-control/routing registers to make them match.

### 2. Guard-check at confirmed stock clocks

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py guard-check | tee logs/guard-r5.jsonl
cat /sys/kernel/debug/a9_tuning/status | tee logs/post-guard-r5.txt
'
```

Require `manual-revert-pass`, `heartbeat-revert-pass` with the captured
heartbeat-timeout reason, and `stock-restored`. The final cleanup reason is
user-revert. A previous proof of the exact Apple hardware-watchdog reset path
can be retained. If no proof exists, run the following stock-only reset test
with an external log stream **before relying on the watchdog for tuning**:

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py watchdog-check --watchdog /dev/watchdog0 | tee logs/watchdog-reset-r5.jsonl
'
```

Require an externally observed reset/new boot ID within the reported timeout;
a lost connection is insufficient. The tool never reports timeout as a pass.
After reset, restore the same healthy boot/profile and repeat steps 0–2.
Do not displace another watchdog owner.

### 3. All-load 1848 MHz at unchanged voltage

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py boost --minutes 5 --logdir boost-r5 | tee logs/boost-r5.jsonl
'
```

Require `configure 1848000 0` to succeed, readback state15 with unchanged stock
rail codes, actual 1848000 kHz, `trial-pass` and `stock-restored`. The native
state8 words must remain unchanged. A delivery-timeout is an abort; return its
guard/policy diagnostics instead of adding voltage. Never count requested
frequency as actual delivery.

### 4. Stock-voltage approach to 2 GHz

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py ladder --minutes 10 --through-khz 1992000 \
  --max-delta-uv 0 --logdir stock-2g-r5 | tee logs/stock-2g-r5.jsonl
'
```

The ladder runs 1848, 1896, 1944 and **1992 MHz**, with a stock restore and
cooldown below 80 C after each. Its first rung uses the same configured
all-load slot15 as step 3. 1992 MHz is the existing near-2-GHz rung, not exactly
2000 MHz. Every completed trial needs actual target delivery and verified work.

### 5. Failure-dependent bounded offsets

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py ladder --minutes 10 --through-khz 1992000 \
  --voltage-step-uv 6250 --max-delta-uv 50000 \
  --logdir offset-2g-r5 | tee logs/offset-2g-r5.jsonl
'
```

Each clock starts at delta0. Only verified correctness failure, confirmed
restore, cooldown and passing stock correctness control permit the next
6250 uV step. Sensor/thermal/heartbeat/handoff/delivery/resource/restore faults
abort without authorizing an offset. Stock and 1848 MHz never receive offsets.
Both rails retain the exact 3125 uV encoding and immutable **+50000 uV** ceiling.

### 6. Optional stretch after lower rungs pass

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py ladder --minutes 10 --through-khz 2160000 \
  --voltage-step-uv 6250 --max-delta-uv 50000 \
  --logdir stretch-r5 | tee logs/stretch-r5.jsonl
'
```

Retain independent cycle timing and longer warm-soak validation for successful
points. No round-5 clock stability or physical throttling result is established
by the offline tests.

## Stock logging, reset retry and emergency restore

To refresh the stock load baseline on this same profile, run at confirmed
unarmed stock clocks, before another tuning stage. Discovery remains by type;
the battery/quarantined channels cannot substitute:

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
zones=()
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat "$z/type")" in
    apple-a9-soc0|apple-a9-soc1|apple-a9-tcc2) zones+=(--soc-zone "${z##*/}");;
  esac
done
test "${#zones[@]}" -eq 6
python3 -B stock-log.py --stress --duration 300 --interval 0.25 "${zones[@]}" \
  > logs/stock-load-r5.jsonl 2> logs/stock-stress-r5.log
'
```

The default cutoff here is 90000, read from the kernel profile. Require
`complete`, no abort and healthy workload diagnostics. Let all trusted channels
cool below the profile arm limit before guard-check or another trial. Never
heat to 95/100 C to test emergency shutdown.

After a hard reset during an externally recorded, unfinished offset trial,
restore the healthy cold boot and repeat steps 0–2, then explicitly retry:

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py ladder --retry-failure logs/offset-2g-r5.jsonl \
  --minutes 10 --through-khz 1992000 --voltage-step-uv 6250 \
  --max-delta-uv 50000 --logdir retry-r5 | tee logs/retry-r5.jsonl
'
```

The retry needs a new boot ID and unfinished trial record. Handled/completed
trials and 1848 MHz failures cannot authorize a voltage retry. Preserve logs
externally before reusing their names.

Emergency restore from a second root console:

```bash
sudo bash -c '
cd /root/a9-power-r4
printf "revert\n" > /sys/kernel/debug/a9_tuning/control
cat /sys/kernel/debug/a9_tuning/status
'
```

Require armed=0, dirty=0, configured=0, delta_uv=0, revert_error=0 and known
actual frequency **>0 and <=1800000 kHz**. If restoration is unconfirmed, keep
the owned watchdog reset fallback; do not magic-close it or start another trial.
