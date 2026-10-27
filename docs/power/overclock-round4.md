# A9 overclock/power round 4 — runnable three-zone sequence

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Kernel **`a9/power` head `49c0339866d2ec2fae2d031b58212d28d0c53b52`**,
parent `b553f1ebaf`. Only that kernel branch was committed. Workspace changes
are uncommitted. No device connection, MMIO access on the iPad, workspace commit,
push, boot or live tuning test was performed by this agent.

This replaces round 3's device sequence and exact-`1` voltage-control assertion.
The owner policy remains RAM-only, default-off experiments, +50000 uV maximum
per affected rail, thermal throttling, emergency stock revert and a hardware
watchdog. Crashes are acceptable. No tuning service or persistent boot argument
is installed. Logs may be retained across reboot; tuning state may not.

## The two failures

`stock-log.py` still had its round-1/2 six-element `ZONE_TYPES` set. Its
`guard()` compared the three requested types against all six and raised
`stress requires all six calibrated SoC zones`. This was a stale tool
prerequisite: round 3 intentionally registers only SOC0/SOC1/TCC2, required
mask **0x23**. The battery is not a substitute. The logger never launched
stress-ng on that failed check; installing stress-ng did not fix the mismatch.

`ladder.py guard-check` reaches `control('arm')`. On the supplied kernel,
`a9_control_write()` calls **`a9_arm()`**, whose first hardware precondition is:

```c
if (readl(g->voltage_base) != 1 || readl(g->voltage_base + 4) != 0x1782)
        return -EOPNOTSUPP;
```

`g->voltage_base` is `ioremap(0x20e0a0000, 0x20)`. The status prints direct
**32-bit `readl()`** results at +0/+4 with `%08x`; it is not a PSINFO byte,
shifted field, cached boolean, decimal/hex mix-up, or read from the wrong bank.
`voltage_routes=1782` agrees with LLB's exact routing write. With control=0,
the first comparison returns **-95** before stock-table checks, temperatures,
arming, QoS expansion or table stores. This is not the round-2 nonzero-voltage
rejection, a thermal failure or a missing Kconfig option. The enabled-boot log
shows `enable=1`, `armed=0`, `dirty=0` after the failed operation.

Evidence: [enabled arm refusal](../release/OMISSIONS.md),
[cold-boot status](../release/OMISSIONS.md),
[stock-log refusal](../release/OMISSIONS.md),
[thermal samples](../release/OMISSIONS.md), and the
[round-4 evidence manifest](../release/OMISSIONS.md).
All **109** supplied cold-boot thermal samples report **0 / 0x1782**, with
stock frequency transitions and a passing three-sensor check. The corrected
TCC coefficients at +0x100/+0x118/+0x130 agree after the normal cold boot.
SOC2/TCC0/TCC1 remain diagnostic; plausible new readings do not promote them.

**The precondition was wrong; there is no evidence for changing the readback.**
More precisely, zero is the real *observed Linux handoff* on this unit. The
logs do not measure the register before m1n1, so calling it the literal iBoot
value before that handoff would exceed the evidence:

* Pinned [m1n1 cpufreq.c](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/cpufreq.c)
  writes `1`; [payload.c](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/payload.c)
  does call `cpufreq_init()` before booting a kernel/DT payload. The write is
  not followed by an assertion that the register reads `1`. The earlier claim
  confused a value written with a guaranteed sticky readback.
* Hash-pinned LLB `0x180396978..984` and iBoot `0x87001b7b4..7c0` read this
  same physical control register and OR **0x1f00**. Their helpers at
  `0x18039750c` / `0x87001cebc` supply the upper physical-address bits and do
  a 32-bit load. LLB `0x18039655c..56c` writes routing=0x1782; iBoot
  `0x87001c968..988` preserves that routing register.
* Shared Ghidra's `AppleS8000PMGR_setVoltageControlMode`,
  `0xfffffff005e91b0c`, changes the **0x1f00** mask: argument zero sets those
  bits; nonzero clears them and seeds the live voltage bytes at
  PMGR+a0018/a0010/a0008. It does not establish that exact value `1` is a
  readable automatic-mode identifier. Neither a self-clearing bit0 nor the
  specific intervening writer explaining 1→0 has been proved here.

No mode/routing "repair" write is added. No claim of measured rail voltages,
verified analog settling, or a complete voltage-controller mode decoder is
made. The experiment preserves the exact observed stock handoff and modifies
only bounded dormant PSINFO inputs, as in round 3.

## Changes and safety contract

The kernel accepts **only boot control=0, boot routing=0x1782, and live values
equal to that same pair**. This is a fixed allowlist for the observed S8003
j71t handoff; it does not accept arbitrary boot values or silently accept
both 0 and 1. Arm/configure recheck under the transition lock, each above-stock
target checks it, and the 100 ms monitor reverts on drift. The original
`voltage-mode` fault reason is retained. Failed configure commands also revert
an armed/dirty experiment. Stock transition/revert remains possible after a
handoff fault.

Status is now **format=3**, with `voltage_ctl_boot`, `voltage_routes_boot`,
`voltage_handoff_ok`, `handoff_policy=s8003-r4`, and the last `control_error`.
The ladder requires that interface before tuning, and verifies it alongside
both full PSINFO words throughout a trial. Control-write failures emit the
command, errno and kernel guard state. Old kernels fail the format check with
an actionable message. Read-only thermal-check still works on the supplied
round-3 thermal format2 kernel.

Both tools use **`a9_thermal.py`**. Copy it beside the other two scripts.
They require every member of SOC0/SOC1/TCC2, thermal snapshot format2/mask23,
enabled zones, cpufreq cooling, trips no higher than **80/90/100 C**, fresh
samples (age **0..500 ms**), valid BK and nonzero non-overflowing counts.
Missing/duplicate required zones, ENODATA, stale/malformed snapshots or lost
cooling abort. Quarantined-channel errors cannot replace a required sensor or
block an otherwise healthy three-channel set. The stock logger's cutoff is
now **80 C maximum**, lowerable, and still checks known stock actual/requested
frequencies on every sample. It stops only its own workload and writes no
hardware/policy control files.

The +50 mV compiled ceiling, equal exact 3125 uV steps on core/buck7, immutable
stock reference, full-word readback, original Apple slots/timing/load fields,
75 C arm threshold, 80 C tuning revert, 90/100 C callbacks, five-second emergency
poweroff fallback, two-second heartbeat, governor QoS and thermal-emulation
exclusion remain. The ladder still needs a stoppable **Apple SoC Watchdog** at
3..15 seconds and retains it if stock restore is unconfirmed. New
`watchdog-check` deliberately stops feeding that owned watchdog at confirmed
stock clocks; success must be an externally observed reset/new boot ID. It
cannot report a timeout as a pass. Guard-check now samples thermals during
its deliberate heartbeat miss and always attempts cleanup.

## Build and offline verification

The full output is **`build/kernel-power-r4-integ/`**. It is exactly the tested
integration **`b7e52e2624` plus the delta `b553f1ebaf..49c0339866`**, built in a
separate **detached** worktree. `git merge-tree --write-tree` verifies the real
branch merge and matches the staged build tree. No integration commit or
branch is created; the build's `COMMIT` deliberately names b7e52e2624 and
`DIRTY` lists the one staged cpufreq header. The manifest records both parent
and power hash, merged tree ID and patch/artifact hashes so this cannot be
mistaken for the unmodified b7e52e2624 image. Shared `build/kernel-integ/` and
`build/kernel-tri/` payloads are not replaced.

Host-only reproduction (the Ghidra call reads the existing kernelcache project):

```sh
python3 -B scripts/power/oc-re.py fffffff005e91b0c \
  --tool disassemble_function --out build/power/oc-r4
uv run --no-project --with capstone python -B scripts/power/round4-evidence.py --disassemble
scripts/power/build-round4.sh --full
```

All **34 Python tests pass**. The suite also compiles actual kernel helpers
with ASan/UBSan, including the control-write handler and actual monitor/revert
code. It exercises all 102 clock/offset pairs, every nonzero
control bit rejected before arming, boot/current routing checks, a handoff
change during arm, immediate target blocking, monitor restoration, temperature,
heartbeat, failed revert and thermal races. Python coverage includes the
three-zone stock workload and a mid-run sensor failure, stricter cutoffs,
cooling/trips, selective quarantine, both full voltage words, format/handoff
rejection, manual/timeout guard-check, watchdog fallback and bounded retries.
Tuning-enabled/disabled object builds, DT compilation and the complete Image/DTB
build are checked by the scripts. Final evidence and artifact hashes are in
[overclock-round4-build.json](../release/OMISSIONS.md).
Checkpatch reports zero code errors and one commit-message wrapping warning.

## Exact ordered orchestrator device sequence

The following commands are **for the orchestrator to run**, not a record of
device execution. Start from a normal LLB/iOS cold boot, then the existing
RAM-only palera1n→pongoOS→m1n1 workflow. Use the round-4 **Image and matching
DTB together**. A kexec from a damaged/dirty earlier experiment is not this
starting condition. Pass `apple_soc_cpufreq.a9_tuning=1` for this boot only.
Keep normal hardware thermal protections and debugfs enabled; never add that
argument to a persistent boot entry/service.

Copy **`ladder.py`, `stock-log.py`, `a9_thermal.py`** from `scripts/power/`
into `/root/a9-power-r4/` using the orchestrator's normal transfer workflow.
Use a separate external host log/stream for commands that can reset the iPad.
The commands below run in a root **bash** on Linux. Stop at any nonzero exit,
missing pass event, unknown actual frequency or failed restoration.

1. **Identify the new handoff and validate thermals.**

   ```bash
   set -euo pipefail
   cd /root/a9-power-r4
   mkdir -p logs
   command -v stress-ng
   test -r /sys/kernel/debug/a9_thermal/snapshot
   cat /proc/sys/kernel/random/boot_id | tee logs/boot-r4.id
   cat /sys/kernel/debug/a9_tuning/status | tee logs/status-r4.txt
   cat /sys/kernel/debug/a9_thermal/snapshot | tee logs/thermal-raw-r4.txt
   python3 -B ladder.py thermal-check --seconds 30 | tee logs/thermal-r4.jsonl
   python3 -B - <<'PY'
   from pathlib import Path
   import ladder
   root = Path('/sys')
   state = ladder.ensure_stock(root)
   ladder.voltage_handoff(state)
   assert state['enable'] == '1'
   print('round-4 handoff/stock PASS')
   PY
   ```

   Require format3, policy s8003-r4, live/boot control both **00000000**, live/boot
   routing both **00001782**, handoff_ok=1, no dirty/configured slots, and thermal
   format2/mask23. Check the three coefficient groups still agree. An unexpected
   value is a stop-and-return-the-logs condition; do not poke PMGR to match it.

2. **Run stock load and cooldown.** Discover by type, not numeric zone ID:

   ```bash
   zones=()
   for z in /sys/class/thermal/thermal_zone*; do
     case "$(cat "$z/type")" in
       apple-a9-soc0|apple-a9-soc1|apple-a9-tcc2)
         zones+=(--soc-zone "${z##*/}");;
     esac
   done
   test "${#zones[@]}" -eq 6
   python3 -B stock-log.py --stress --duration 300 --interval 0.25 \
     --cutoff-millic 80000 "${zones[@]}" \
     > logs/stock-load-r4.jsonl 2> logs/stock-stress-r4.log
   python3 -B ladder.py thermal-check --seconds 30 | tee logs/cooldown-r4.jsonl
   ```

   On the reported kernel these are zone0 SOC0, zone1 SOC1, zone2 TCC2;
   zone3 battery is excluded. Require `complete`, no abort and healthy stress-ng
   diagnostics. Inspect warming/cooling, fresh raw count/BK data and actual
   clocks/cooling states together. Establish real stock cpufreq cooling response
   before proceeding; these offline tests do not establish physical throttling.
   Do not heat to 90/100 C to test the emergency shutdown.

3. **Exercise manual revert and heartbeat expiry at stock.**

   ```bash
   python3 -B ladder.py guard-check | tee logs/guard-r4.jsonl
   cat /sys/kernel/debug/a9_tuning/status | tee logs/post-guard-r4.txt
   ```

   Require `manual-revert-pass`, `heartbeat-revert-pass` (reason heartbeat-timeout)
   and `stock-restored`. The final cleanup sets reason=user-revert; the timeout
   reason is captured in its preceding event. Neither part requests a new clock.

4. **Prove hardware reset once before relying on it.** If the orchestrator has
   already proved this exact hardware-watchdog path, retain that evidence;
   otherwise run the following while streaming to an external host. This
   deliberately resets the device at stock clocks:

   ```bash
   cat /sys/class/watchdog/watchdog0/identity
   cat /sys/class/watchdog/watchdog0/nowayout
   python3 -B ladder.py watchdog-check --watchdog /dev/watchdog0 \
     | tee logs/watchdog-reset-r4.jsonl
   ```

   Require Apple SoC Watchdog, nowayout=0 and `watchdog-reset-start`, then an
   observed reset in the reported timeout. EBUSY means another owner exists;
   do not take away its FD or proceed. After the reset, cold-boot the same RAM-only
   round-4 image again, rerun steps **1–3**, and confirm the new boot ID differs
   from the external reset-start record. A lost connection alone is not proof
   of reset. A timeout/failure event is not a pass.

5. **Unchanged-voltage stock boost, then stock-voltage approach to 2 GHz.**

   ```bash
   python3 -B ladder.py boost --minutes 5 --logdir boost-r4 \
     | tee logs/boost-r4.jsonl
   python3 -B ladder.py ladder --minutes 10 --through-khz 1992000 \
     --max-delta-uv 0 --logdir stock-2g-r4 | tee logs/stock-2g-r4.jsonl
   ```

   Use new log directory names for repeat runs. Boost retains Apple's load=2
   restriction; never delivering the target is an abort, not a successful boost
   or a reason to add voltage. This ladder also checks boost before overclocked
   rungs. Each trial must finish with confirmed stock restoration.

6. **Failure-dependent offsets, then optional stretch after lower rungs pass.**

   ```bash
   python3 -B ladder.py ladder --minutes 10 --through-khz 1992000 \
     --voltage-step-uv 6250 --max-delta-uv 50000 \
     --logdir offset-2g-r4 | tee logs/offset-2g-r4.jsonl
   python3 -B ladder.py ladder --minutes 10 --through-khz 2160000 \
     --voltage-step-uv 6250 --max-delta-uv 50000 \
     --logdir stretch-r4 | tee logs/stretch-r4.jsonl
   ```

   The tool always starts each clock at delta0 and adds one bounded step only
   after a verified correctness failure, successful revert, cooldown and stock
   correctness control. Thermal, sensor, heartbeat, handoff, non-delivery,
   setup/resource or restore faults abort without authorizing an offset.
   After a hard reset during a logged started trial, restore/validate the cold
   boot and steps 1–3 again, then explicitly retry the externally preserved log:

   ```bash
   python3 -B ladder.py ladder --retry-failure logs/offset-2g-r4.jsonl \
     --minutes 10 --through-khz 1992000 --voltage-step-uv 6250 \
     --max-delta-uv 50000 --logdir retry-r4 | tee logs/retry-r4.jsonl
   ```

   The retry requires a different boot ID and an unfinished trial record;
   a handled failure/completion cannot authorize it. Retain the external copy
   before reusing device log names. A short pass does not establish long-term
   stability; repeat/warm-soak successful points and independently time cycles.

Emergency command at any step (from a second root console if necessary):

```bash
printf 'revert\n' > /sys/kernel/debug/a9_tuning/control
cat /sys/kernel/debug/a9_tuning/status
```

Require armed=0, dirty=0, configured=0, delta_uv=0, revert_error=0 and a known
actual frequency **>0 and <=1800000 kHz**. If restore is unconfirmed, retain the
hardware watchdog reset; do not magic-close it or start another trial.
