# Charge9 device test plan — orchestrator only

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implementation agent ran no device commands. Boot the entry recorded in
`build/charge9/entry-name` (`charge9-16a34c8432`) using the existing warm-kexec
workflow. Verify the artifacts first:

```sh
python3 -B -I scripts/periph/verify-charge9.py
```

Entry directory: `build/charge9/entry/`. The initramfs is unchanged from charge8,
including retirement of the automatic 2.4 A request. The cmdline changes only
the entry token: one `pcie_apple_h9p.ans_keep_power=1`, no systemd debug shell,
and no logging-option change. Kernel lineage is `a9/charge8`, not a new release
integration. Keep the already-retired rootfs service/rule retired.

## Record the baseline and run a local logger

Use Wi-Fi SSH so USB swaps do not stop control or the Linux-local logger.

```sh
export OMARCHY_HOST="$(cat build/omarchy/wifi-host)"
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; cat /sys/class/power_supply/charger/charge7_state; cat /sys/bus/i2c/devices/0-001a/lightning_state; cat /sys/class/power_supply/battery/status; cat /sys/class/power_supply/battery/current_now'
scripts/omarchy/ssh.sh 'cat > /tmp/charge9-watch.sh' < scripts/periph/charge9-watch.sh
scripts/omarchy/ssh.sh 'sudo -n systemd-run --unit=charge9-watch --collect --property=RuntimeMaxSec=1810 /bin/sh -c "WATCH_SECONDS=1800 sh /tmp/charge9-watch.sh > /var/tmp/charge9.tsv 2>&1"'
```

The logger is bounded to 1800 seconds (maximum 3600), with 0.5-second read
limits. It appends both `charger_detect:` and `source_epoch:` to its detector
column. Timeout exit 124 at the end is normal. Save full snapshots after each
settled source. Mark UTC times, source label, cable and Lightning orientation.
Baseline `override_ua` must remain 0 unless explicitly testing the sysfs cap.

## Repeat the four captured sequences

1. **Mac via USB-A→Lightning plus USB-C→A adapter**, then optional Mac via
   USB-C→Lightning. Expect SDP, 2400000 ceiling, normal nominal ramp
   500000→1000000→2100000→2400000, and working USB network/ACM/root I/O.
   Once the gadget is active, no pin sampling should occur. The generation
   may rise for real data/budget events; epoch must stay stable through USB
   suspend/reset. Default-reset repairs must not leave `live_ua=100000`
   alongside a successful active 500000 step. The calibrated floor normally
   reads 475000, not exactly nominal 500000.
2. **Fast Mac→USB-C wall swap at the iPad**, using the Apple C→Lightning cable.
   A Tristar phase drop or changed ID `10 0c... → 10 09 08...` must increment
   the epoch even when sampled online stays 1. After that evidence, no sticky
   SDP identity, old override or adaptive cap may survive. The first ID after
   a phase-created epoch is refinement, not a second identity boundary.
   Default 0x02 resets within the 15-second attach window must increment
   `attach_resets`, reapply the guarded step, and leave backoffs/adaptive at 0.
   If there is no phase edge, allow the existing periodic ID query to obtain
   evidence; there is no assertion of detection before that observation.
3. **Held unplug (at least five seconds)**. Require online 0, new epoch, inactive
   charger and cleared source evidence. At full capacity with negative current,
   battery STATUS must be Discharging. If its cached current is zero or still
   positive, status must be Not charging while the source is offline. Capacity
   may remain 100%. Check the desktop icon separately; record any delay.
4. **Clean wall replug through Apple USB-C→Lightning**. Require a fresh source,
   device role, no camera-adapter power handshake and `override_ua=0`. A BC
   result before the initial ID must survive the ID and later CC failures.
   A 1500 mA DCP/CDP item should support nominal 1000000 until CC qualifies.
   With two settled CC advertisements around the captured 1624–1628 mV and
   retained BC evidence, expect USB type C, supplier allowance 3000000,
   charger ceiling 2400000 and normal ramp to 2400000. The BC and CC limits
   are not added. Require that the old steady 100 mA failure is gone.

Hold the wall source for at least a minute and two gauge refreshes. For actual
charging, use a cool, partially discharged pack and controlled screen/load:
require positive net battery current and increasing charge. At 99–100%, record
classification, termination behavior and actual current separately; a correct
ceiling alone is not evidence that the battery is charging.

## Inspect convergence and event origins

`source_epoch:` adds lifetime `usb_events`, `usb_idle_events`, `usb_generation`,
last `usb_event_source`, retained BC allowance/type and `cc_first_mv`. Origin
codes are: 1 source accounting forget, 2 base budget, 3 grant revoke, 4 suspend,
5 SETUP, 6 host grant, 7 gadget state.

During idle wall classification, repeated base/state/suspend notifications may
increase total/idle counters, but must not increase the data generation or
cause recurring `usb-event` rejection. A real USB session must still exclude
or cancel sampling. Record counters before and after attempts. The old capture
did not record origin; these observations are needed to identify the exact
live producer, rather than inferring its IRQ from the old warning.

A CC refresh uses at most three samples to obtain two matching advertisements.
A failed/unstable refresh must retain `bc_ua=1500000` from this epoch instead of
falling to Unknown solely because CC failed. A subsequent stable pair must
upgrade it. Replug/ID change must clear that old BC item. ADC poison remains
terminal for this boot; do not clear it by unbinding or guessed register writes.

## Additional regressions

- Apple USB-A brick if available, both orientations: correct divider/BC source,
  fresh epoch and no stale Type-C evidence. Record missing hardware as NOT RUN.
- Fast return wall→Mac and same-ID replug: record observed phase/VBUS/ID evidence.
  An entirely unobserved same-ID swap remains a polling limitation.
- On a settled source, explicit sysfs 500000 cap and 0 disable still work and
  are revoked by a new source epoch. Positive requests cannot exceed the
  detected/board ceiling. Keep these writes separate from baseline charging.
- Optional powered camera adapter: ID `11 f0`, guarded handshake, ACA 2100000,
  real host enumeration, then clean return to Mac. No gauge or PMIC debug writes.

## Collect

```sh
scripts/omarchy/ssh.sh 'sudo -n systemctl stop charge9-watch.service; cat /var/tmp/charge9.tsv' > build/charge9/device-watch.tsv
scripts/omarchy/ssh.sh 'sudo -n dmesg' > build/charge9/device-dmesg.txt
scripts/omarchy/ssh.sh 'cat /sys/bus/i2c/devices/0-001a/lightning_state; cat /sys/class/power_supply/charger/charge7_state; cat /sys/class/power_supply/battery/status; cat /sys/class/power_supply/battery/current_now; sudo -n cat /sys/kernel/debug/apple-usb-role/regs' > build/charge9/device-final.txt
```

Report PASS/FAIL/NOT RUN per source/orientation: epoch/reason, ID, BC/CC voltages,
source type, supplier ceiling, policy/live setting, attach resets/backoffs,
notifier counters/origins, battery measurements/status, desktop icon and tether
continuity. No verified VBUS droop protection or sub-poll detach guarantee is
claimed by this build.
