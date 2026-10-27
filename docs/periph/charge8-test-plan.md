# Charge8 device test plan — orchestrator only

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


No device steps were run by the implementation agent. Warm-kexec from release2
into the entry in `build/charge8/entry-name`, using the existing boot workflow.
Verify `python3 -B -I scripts/periph/verify-charge8.py` first. Entry directory:
`build/charge8/entry/`. The command line differs from charge7 only in the entry
name: one `pcie_apple_h9p.ans_keep_power=1`, no `systemd.debug_shell`, unchanged
logging options. Charge8 descends from `a9/charge`, as requested; it is not a
new release integration branch.

## Retire the installed automatic request before booting

The workspace files are changed, but the live rootfs was **not** modified.
The packaged initramfs already makes `charger_limit()` a no-op. On the running
Linux system, install the no-op compatibility service and retire both possible
udev-rule locations. These are orchestrator steps; they do not write PMIC,
syscfg, NVRAM or boot firmware.

```sh
export OMARCHY_HOST="$(cat build/omarchy/wifi-host)"
scripts/omarchy/ssh.sh 'cat > /tmp/charge8-charger-limit.service' < scripts/omarchy/rootfs/files/etc/systemd/system/linux-a9-charger-limit.service
scripts/omarchy/ssh.sh 'sudo -n systemctl disable --now linux-a9-charger-limit.service; sudo -n install -m 0644 /tmp/charge8-charger-limit.service /etc/systemd/system/linux-a9-charger-limit.service; sudo -n rm -f /etc/udev/rules.d/90-linux-a9-charger.rules /usr/lib/udev/rules.d/90-linux-a9-charger.rules; sudo -n systemctl daemon-reload; sudo -n udevadm control --reload-rules'
```

The existing release2 sysfs request may remain latched until detach or reboot;
installing a no-op service does not revoke an existing kernel cap. Charge8 must
start with `override_ua=0`. Do not add an automatic 2400000 write to the test.
Explicit root sysfs requests remain supported per-attach caps/disable controls.

## Capture continuously over cable swaps

Use Wi-Fi SSH; logging runs locally on Linux and survives USB removal.

```sh
scripts/omarchy/ssh.sh 'cat > /tmp/charge8-watch.sh' < scripts/periph/charge8-watch.sh
scripts/omarchy/ssh.sh 'sudo -n systemd-run --unit=charge8-watch --collect --property=RuntimeMaxSec=1810 /bin/sh -c "WATCH_SECONDS=1800 sh /tmp/charge8-watch.sh > /var/tmp/charge8.tsv 2>&1"'
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; cat /sys/class/power_supply/charger/charge7_state; cat /sys/bus/i2c/devices/0-001a/lightning_state; sudo -n cat /sys/kernel/debug/apple-usb-role/regs'
```

The logger includes the existing `charge7_state` ABI and the new cached
`charger_detect:` line. Each read has a 0.5 s timeout and the run is bounded to
1800 s (maximum 3600); timeout exit 124 at the end is normal. Mark each cable
swap's UTC time, adapter label and Lightning orientation separately. Save a
full `lightning_state` snapshot at each settled source, not just the TSV.

## Cable/source sequence

1. **Mac USB-A → Lightning**, both orientations. Expected ID `10 0c 00 00 00 00`,
   then SDP and board ceiling 2400000, nominal ramp 500000 → 1000000 → 2100000 →
   2400000 with roughly one-second steps. `usb/current_max` remains USB stack
   accounting, so it need not equal the charger ceiling. Verify USB networking,
   ACM and root I/O. Once the host is seen, detector attempts and ADC routing
   must stop increasing for this epoch, including across USB suspend/reset.
2. **Mac USB-C → Lightning**, ID `10 09 08 00 00 00`. Expected eventual SDP and
   the same 2400000 ceiling, `override_ua=0`. An idle attach may start a CC
   sample before enumeration; a USB event must discard an in-progress result.
   Once enumerated, there must be no recurring monitor sampling or tether loss.
3. **USB-C wall charger through that C→Lightning cable**. After a measured
   detach/new epoch, expect the same cable ID but no computer identity. Role
   stays device; the camera-adapter power handshake is not run. Examine
   `cc_mv`, `dp_mv`, `dm_mv`, `stage`, `error`, `adc_status` and `route`.
   A stable CC advertisement above 1229 mV **plus BC1.2 charging-port evidence**
   gives USB type C, supplier allowance 3000000 and charger ceiling 2400000.
   CC 660–1229 mV gives 1500000 and the existing calibrated nominal 1000000 rung.
   CC ≤659 gives no Type-C elevation; analog/BC1.2 can still classify it.
   A 3 A CC value without BC1.2 evidence is conservatively capped at 1500000,
   matching iOS's unqualified-source restriction. If this owner's source
   remains at 500000, record FAIL and retain the exact diagnostic snapshot.
4. **Apple USB-A brick, if available**. Test each available brick and both
   Lightning orientations. Expect the sampled divider table (2.5/2.5 V → 2400 mA;
   2.5/2.0 V → 2100 mA; other entries as in charge7). BC1.2 DCP/CDP offers
   1500000 and uses the existing nominal 1000000 rung. Do not predict the table
   entry solely from the brick's printed wattage. Missing hardware is NOT RUN.
5. **Mac → wall → Mac, then fast replug**. Every observed interval with both
   VBUS bits clear must increment the epoch, revoke override/adaptive state,
   clear prior ID/brick/computer evidence and restart at the floor. Check
   replug in both orientations and several intervals below one second. A
   pulse missed by all polls is **missed detach**, not a passed epoch test.
   No analog or Type-C allowance may leak back to the Mac source epoch.
6. **Explicit override semantics**, on the settled wall source: write 500000
   to `charger/input_current_limit`; expect a user cap, not a changed detected
   ceiling. Replug must clear the cap. Write 0 to disable this attach, then a
   supported positive request to re-enable; a request above the source cannot
   raise its ceiling. Unsupported positive rungs must fail. Do this separately
   from the baseline automatic-current test and record the writes/times.
7. Optional camera-adapter regression: ID `11 f0`, guarded power handshake,
   ACA 2100000 after success and real USB-host enumeration; return to the Mac
   and verify a new epoch, working tether, SDP and no stale ACA/handshake.

For an adequate source, test charging below full capacity with a cool pack and
unchanged screen/load settings. Require positive net battery current and an
increasing charge over multiple gauge updates. At 99–100%, termination may
hold a lower live setting despite a correctly classified ceiling; report
classification and actual battery charging as separate results.

## If detection fails

`stage=route-select error=-95 route=0` would contradict the zero-mux fix: save
entry identity and the built commit. Other mux nibbles can still be refused.
`adc-idle` means no new trigger was issued; a persistent high completion latch
or transport failure is distinct from `adc-done` timeout after a trigger.
`poisoned=1` prohibits further triggers for this boot; `poison_stage` and
`poison_attach` identify its origin even after unplugging. Do not clear that
state by unbind/rebind, a guessed PMIC write, or repeatedly forcing GetID.

`monitor_error=-16` means role hardware refused the callback; it cannot itself
produce the old classify warning. `-117` means a mux/DCP restore is pending and
host entry is inhibited until repair. `usb-event`, `usb-active`, `orientation`
and `vbus` reject a sample when the source/link changed. `divider-range`,
`primary-range`, `secondary-range`, `cc-range` and `cc-debounce` identify invalid
or unstable measurements. Read errors remain errors, never zero voltages.

Continuous failures should attempt after 1, 2, 4, 8, 16, 32 and then 60 seconds,
with no more than one warning per minute per attach. ADC poison is terminal
for conversion attempts; no repeated trigger is permitted. A successful
Type-C classification is refreshed every five seconds only while idle, with
at least 20 ms between its two matching CC advertisements. A failed refresh
revokes the old CC allowance; the charger's existing transport/link grace can
delay the downward setting, so observe it for more than five seconds.

## Collect and report

```sh
scripts/omarchy/ssh.sh 'sudo -n systemctl stop charge8-watch.service; cat /var/tmp/charge8.tsv' > build/charge8/device-watch.tsv
scripts/omarchy/ssh.sh 'sudo -n dmesg' > build/charge8/device-dmesg.txt
scripts/omarchy/ssh.sh 'cat /sys/bus/i2c/devices/0-001a/lightning_state; cat /sys/class/power_supply/charger/charge7_state; sudo -n cat /sys/kernel/debug/apple-usb-role/regs' > build/charge8/device-final.txt
```

Report PASS/FAIL/NOT RUN per source/orientation, sampled CC and divider voltages,
USB type, supplier current, charger ceiling/nominal/live setting, battery
current/voltage/temperature/capacity, epoch transitions and tether continuity.
No voltage-triggered VBUS backoff is claimed. `vbus_mv=unavailable` remains
accurate; an observed PMIC setpoint reduction is not proof of droop detection.
