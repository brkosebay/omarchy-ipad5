# A9 overclock round 6 — single-core limit scan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Kernel **`a9/power`**, parent `20ddcb7f1c`, head
**`ca72db3fca0eb162d4bbde984ba219237195a8cd`**. Only the kernel branch was
committed. Workspace changes are uncommitted. No device access or push.

## Supplied device evidence and unchanged limits

The owner reports round-5 two-core, two-minute passes at stock voltage for
1848, 1896, 1944, 1992, 2040 and 2088 MHz. The 2160 MHz trial was **still
running** when this task was assigned (TCC2 about 87 C, SOC about 65 C); a
2160 MHz pass is not assumed. Owner: “if we pass 2.16, I want to keep going —
let's test the limits”. These are supplied observations; round 6 has no device
results yet.

The same opt-in boot arguments select the **90/95/100 C** sustained profile:
90 C passive/guard revert, 95 C hot, 100 C critical; arm/configure strictly below
85 C and cooldown below 80 C. Both nominal rail offsets remain bounded to
**+50000 uV**, with exact 3125 uV DAC steps. No thermal trip, watchdog,
heartbeat, calibration, handoff or voltage-routing rule is relaxed. Every new
rung starts at delta zero. Only verified correctness failures permit a voltage
retry, after stock restoration and a passing stock control. Thermal, sensor,
readback, delivery and guard faults abort without increasing voltage. Crashes
remain acceptable; a hard reset requires an explicit orchestrator retry.

## Real encoding limit and slot reuse

The recovered [iBoot decoder](overclock-round3.md#fuller-cpu-pll-formula)
at `0x87001c35c` uses:

```text
kHz = 24000 * PSINFO1[12:4]
      / max(PSINFO1[17:13], 1) / (PSINFO1[3:0] + 1)
```

The multiplier is **9 bits, maximum 511**. Stock slot 7 has predivider **1**
and postdivider field **0** (`0x74000000002024b0`, multiplier 75). Holding
those dividers fixed gives an exact **12,264,000 kHz encoding ceiling**.
It is not a measured, achievable or physically safe silicon/VCO frequency.
The code rejects other divider encodings, off-grid requests and values above
that ceiling before any PSINFO store; it never wraps the multiplier or changes
adjacent bits to continue a scan.

New rungs are **2208, 2256, 2304, 2352, 2400 MHz**, then every **48 MHz**
through **12240 MHz**, with a final **12264 MHz** rung (24 MHz) to cover the
field endpoint. All 211 new rungs are turbo OPPs, inaccessible until armed and
explicitly configured. There are 218 experimental rungs including round 5.
The existing default `--through-khz 2160000` remains conservative; higher
rungs require an explicit ceiling.

Slots 9..13 retain 1896..2088 MHz, slot 15 retains all-load 1848 MHz with zero
offset, and slot **14 is reused for 2160 MHz and every higher rung**. A target
request must match the exact currently configured frequency, so a lower OPP
alias cannot silently select a higher clock. Actual-state reporting decodes
the live PSINFO word instead of taking the first OPP with that slot number.
Both full PSINFO words and nominal DAC codes remain checked on every sample.
The original Apple slots 0..8 remain intact, and every revert restores dormant
slots from their immutable boot snapshots. Use the matching Image and DTB;
status must show **`clock_profile=s8003-r6-shared-slot boost_state=15`**.
Frequency delivery is still state/PSINFO correlated, not independent cycle
measurement or proof of analog PLL lock.

`--cores 1` starts one stress-ng worker; both CPUs stay online, and the Python
checker/OS still produce some load. Default is two workers. `--seconds 60`
now sets ladder/boost trial duration; `--minutes` remains supported (default
five minutes), and the two explicit duration flags are mutually exclusive.
Thermal-check still defaults to 30 seconds. Duration and core count appear in
trial-start, trial-pass and summary telemetry. `--from-khz` skips earlier rungs;
setting it equal to `--through-khz` confirms exactly one clock. Stock controls
use the selected worker count and their separate `--control-seconds` duration.
The 10-second delivery timeout and 80% delivered-sample requirement are unchanged.

## Build and provenance

```sh
scripts/power/build-round6.sh
```

This performs 52 Python tests plus ASan/UBSan checks of the actual guard/MMIO
and thermal-trip helpers. The helper checks exercise every new 48 MHz rung at
all 17 allowed offsets, the exact multiplier-511 endpoint, alias rejection,
overflow, unchanged dividers, stock-slot preservation and restoration. Python
checks cover one worker, seconds/minutes parsing and propagation through voltage
retries/stock controls, all new full-word readbacks and the existing fault paths.
Checkpatch reports zero errors, warnings or checks for the kernel delta.

Output: **`build/kernel-power-r6-integ/`**, detached integration base
**`f53bf861ca`** plus the complete **`49c0339866..ca72db3fca`** power delta.
Merged tree: **`8c957946a5118f9c62b2fa15f3817d32b698020b`**.
No integration commit/ref is created. The staged source tree must equal
`git merge-tree --write-tree f53bf861ca a9/power`. COMMIT names the integration
base; DIRTY identifies five staged power files, including the cpufreq driver's
new live-frequency decoder. The exact round-5 integration config is reused,
with debug info and thermal emulation disabled. The build seeds ignored
compilation products from the round-5 worktree and preserves timestamps only
for identical Git blobs/generated config bytes to avoid a clean full build.
The [build manifest](../release/OMISSIONS.md) records source/tree,
configuration and artifact hashes and validates all 218 experimental OPPs,
stock DT trips and cooling bindings. Existing integration/round-5 outputs are
not replaced. Only this tuning-enabled Image/DTB build is required for round 6;
the round-5 tuning-disabled object build is not repeated.


Artifact SHA256 (also recorded with tool/source hashes in the manifest):

```text
Image.gz          3f68c90913b2e344d33f9414ebf0b4d261d9a1a6f8d7bd8bfaa58edf6d643b1d
s8003-j71t.dtb     ec43a4d3e2d94a709c347d101823aa0b4425334afee41400fb9db7e4aaf392ad
```

## Ordered orchestrator device commands

**Instructions only; none executed here.** Start from a confirmed stock-clock
Linux session descended from a normal cold boot, with known-good calibration,
PSCI/kexec, root/storage arguments and boot initramfs. Revert and confirm stock
before kexec. Do not kexec from a dirty/unconfirmed overclock or integ31.

Using the orchestrator's transfer workflow, copy current `ladder.py`,
`a9_thermal.py` and `stock-log.py` to `/root/a9-power-r4/`. Copy
`build/kernel-power-r6-integ/Image.gz`, `dtbs/s8003-j71t.dtb` and the known-good
boot initramfs to its `kernel-r6/` directory (initramfs named `initramfs.gz`).
Verify Image/DTB and tool hashes against the build manifest. Keep an external
JSONL/console log stream across resets. Each run needs a fresh log directory.

### 0. Kexec with the same RAM-only sustained profile

This reuses the round-5 entry/FDT workflow: only the live kexec command includes
the tuning arguments, the saved entry cmdline stays stock, and no default or
previous entry is changed. Reuse the live FDT or merge runtime fixups; never
pass a pristine DTB directly. A merge refusal is a stop condition; use the
normal cold-boot workflow instead.

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
mkdir -p logs /run/a9-power-r6
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
entry=power-r6-ca72db3fca
d="/boot/linux-a9/entries/$entry"
if test ! -e "$d"; then
  mkdir "$d"
  cp kernel-r6/Image.gz "$d/Image.gz"
  cp kernel-r6/s8003-j71t.dtb "$d/dtb"
  cp kernel-r6/initramfs.gz "$d/initramfs.gz"
else
  cmp kernel-r6/Image.gz "$d/Image.gz"
  cmp kernel-r6/s8003-j71t.dtb "$d/dtb"
  cmp kernel-r6/initramfs.gz "$d/initramfs.gz"
fi
args+=(panic=0 apple_wdt.cold_restart=0 crash_kexec_post_notifiers "linux_a9.entry=$entry")
printf "%s\n" "${args[*]}" > "$d/cmdline"
cmdline="${args[*]} apple_soc_cpufreq.a9_tuning=1 apple_soc_cpufreq.a9_passive_millic=90000"
gzip -dc "$d/Image.gz" > /run/a9-power-r6/Image
printf "N\n" > /sys/module/apple_wdt/parameters/cold_restart
cat /proc/sys/kernel/random/boot_id | tee logs/boot-before-r6.id
if cmp -s "$old_dtb" "$d/dtb"; then
  kexec -s -l /run/a9-power-r6/Image --initrd="$d/initramfs.gz" --command-line="$cmdline"
else
  python3 /usr/local/lib/linux-a9/linux-a9-fdt-merge \
    --old "$old_dtb" --running /sys/firmware/fdt --new "$d/dtb" \
    -o /run/a9-power-r6/merged.dtb
  kexec -c -l /run/a9-power-r6/Image --initrd="$d/initramfs.gz" \
    --dtb=/run/a9-power-r6/merged.dtb --command-line="$cmdline"
fi
test "$(cat /sys/kernel/kexec_loaded)" = 1
printf "%s\n" "$cmdline" | tee logs/kexec-cmdline-r6.txt
sync
systemctl kexec
'
```

### 1. Check the effective profile and guard before load

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
command -v stress-ng
cat /proc/cmdline | tee logs/cmdline-r6.txt
cat /proc/sys/kernel/random/boot_id | tee logs/boot-r6.id
! cmp -s logs/boot-before-r6.id logs/boot-r6.id
cat /sys/kernel/debug/a9_tuning/status | tee logs/status-r6.txt
python3 -B ladder.py thermal-check --seconds 30 | tee logs/thermal-r6.jsonl
python3 -B - << "PY"
from pathlib import Path
import a9_thermal, ladder
root = Path("/sys")
s = ladder.ensure_stock(root)
ladder.voltage_handoff(s)
assert s["enable"] == "1"
assert s["clock_profile"] == ladder.CLOCK_PROFILE == "s8003-r6-shared-slot"
assert s["boost_state"] == "15"
p = a9_thermal.profile(root)
assert p == dict(profile="sustained", profile_tuning=1, passive_millic=90000,
                hot_millic=95000, critical_millic=100000,
                arm_millic=85000, cooldown_millic=80000)
assert max(ladder.sensors(root)[0].values()) < p["arm_millic"]
print("round-6 profile/handoff/stock PASS", p)
PY
python3 -B ladder.py guard-check | tee logs/guard-r6.jsonl
'
```

Require thermal-check-pass, manual-revert-pass, heartbeat-revert-pass and
stock-restored. Preserve the existing proof of the exact hardware watchdog
reset path; if missing, perform the round-5 stock-only watchdog-check first.
Every reboot requires profile/stock checks again. Do not install a tuning
service, automatic retry or persistent tuning boot arguments.

### 2. Fast single-core limit scan

The scan below starts at 2160 MHz to retest the still-pending round-5 endpoint
on the new image. If the orchestrator has since confirmed the round-5 two-core
2160 MHz pass, `--from-khz 2208000` can skip that rung. The ceiling is the
encoding maximum, **not a target to jump to**: the ladder advances in order and
stops on a fault/instability that exhausts permitted voltage retries. For a
short first batch, use `--through-khz 2400000` instead.

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
python3 -B ladder.py ladder --cores 1 --seconds 60 \
  --from-khz 2160000 --through-khz 12264000 \
  --voltage-step-uv 6250 --max-delta-uv 50000 \
  --logdir logs/scan-r6-1c | tee logs/scan-r6-1c.jsonl
'
```

Keep cooldown and stock controls enabled. A healthy handled failure writes the
passed rungs to `summary.json` before returning nonzero. A hard reset may leave
only the externally streamed JSONL; recover it under the same log filename.
After a reset, use the existing explicit `--retry-failure <interrupted.jsonl>`
workflow only after confirming a new boot ID, known-good tuning boot, stock
checks and the watchdog; it resumes the failed rung one bounded voltage step
higher, without skipping to an untested clock.

### 3. Two-core confirmation at the highest single-core passing clock

Run this after the scan ends, only on a healthy tuning boot with profile/guard
checks completed and a confirmed stock state. It selects only fully completed
60-second one-core `trial-pass` events, tolerates a truncated final JSONL line,
and tests that one frequency for **120 seconds with two workers**. It starts
again at delta zero and can perform the same bounded retries; the two-core
result must be reported separately from the one-core result. A thermal revert
means the sustained two-core limit is lower; confirm the next lower passing
rung separately rather than raising the thermal threshold.

```bash
sudo bash -c '
set -euo pipefail
cd /root/a9-power-r4
highest=$(python3 -B - << "PY"
import json
from pathlib import Path
import ladder
passed = []
for line in Path("logs/scan-r6-1c.jsonl").read_text().splitlines():
    try:
        e = json.loads(line)
    except json.JSONDecodeError:
        continue
    if (e.get("event") == "trial-pass" and e.get("cores") == 1
            and e.get("seconds") == 60 and e.get("target_khz") in ladder.STEPS):
        passed.append(e["target_khz"])
assert passed, "no completed single-core pass to confirm"
print(max(passed))
PY
)
printf "Confirming %s kHz with two workers\n" "$highest"
python3 -B ladder.py ladder --cores 2 --seconds 120 \
  --from-khz "$highest" --through-khz "$highest" \
  --voltage-step-uv 6250 --max-delta-uv 50000 \
  --logdir "logs/confirm-r6-2c-$highest" | tee "logs/confirm-r6-2c-$highest.jsonl"
'
```

All paths finish by restoring stock before stopping the workload and disarming
the watchdog only after stock readback succeeds. If restoration cannot be
confirmed, the watchdog remains armed. Tuning state and modified PSINFO words
are RAM-only and disappear on reboot. To leave tuning, explicitly use the saved
stock cmdline rather than inheriting the live tuned cmdline.
