# Charge10 device test plan — orchestrator only

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implementation agent ran no device commands. Entry: `charge10-a18b0e2ee1`,
under `build/charge10/entry/`. Verify before the existing boot workflow:

```sh
python3 -B -I scripts/periph/verify-charge10.py
```

The initramfs is unchanged from charge9, including the retired automatic 2.4 A
request. Cmdline changes only `linux_a9.entry`: one keep-power token, no systemd
debug shell and unchanged logging options. Keep `override_ua=0` for baseline
tests. This kernel descends from `a9/charge9`, not a new release integration.

## Baseline and local logging

Use Wi-Fi control so cable swaps do not stop the Linux-local logger.

```sh
export OMARCHY_HOST="$(cat build/omarchy/wifi-host)"
scripts/omarchy/ssh.sh 'uname -a; cat /proc/cmdline; cat /sys/class/power_supply/charger/charge7_state; cat /sys/bus/i2c/devices/0-001a/lightning_state'
scripts/omarchy/ssh.sh 'cat > /tmp/charge10-watch.sh' < scripts/periph/charge10-watch.sh
scripts/omarchy/ssh.sh 'sudo -n systemd-run --unit=charge10-watch --collect --property=RuntimeMaxSec=1810 /bin/sh -c "WATCH_SECONDS=1800 sh /tmp/charge10-watch.sh > /var/tmp/charge10.tsv 2>&1"'
```

The logger is bounded to 1800 seconds (maximum 3600), with 0.5-second read
limits. Timeout exit 124 at its end is normal. Record source/cable/orientation
and UTC times; save a full snapshot at each settled source. Its detector column
contains `charger_detect:` and `source_epoch:`, including the new
`computer_seen`, `bc_primary_mv` and `bc_secondary_mv` fields.

## Required source sequence

1. **Mac USB-A→Lightning**, including the previously used USB-C→A adapter.
   Require SDP, `computer_seen=1`, board ceiling 2400000, normal ramp and working
   USB network/ACM/root I/O. No automatic sysfs override should appear.
2. **USB-C wall charger via Apple's C→Lightning cable**. Require a fresh epoch,
   no computer evidence, retained BC followed by CC around the prior 1624 mV,
   type C/3000000 supplier allowance, capped charger ceiling 2400000 and normal
   ramp. Before CC, a secondary reading ≥326 mV should report DCP (Linux 2),
   not the former CDP label. Below 326 should report CDP (Linux 3). Capture the
   actual secondary value; the passive dp_mv/dm_mv pair is a separate reading.
3. **Mac USB-C power adapter**, same cable. It must stay a power-source case:
   no computer latch without real USB activity, qualified CC → board 2400000,
   no false adaptive backoff from an attach-default 0x02 reset. Record
   `attach_resets`, live setting and battery current over two gauge updates.
4. **Mac USB-C computer port**, same cable — the key regression. If BC is sampled
   before enumeration, it may initially offer 1500000 and use nominal 1000000.
   After the real gadget session/configuration, require `computer_seen=1`,
   supplier type **SDP**, charger ceiling **2400000**, and normal upward steps
   to 2400000 when battery guards permit. The source epoch must not change
   merely because a host was recognized. A historical `bc_ua=1500000` diagnostic
   must not cap the charger. USB networking/ACM/root I/O must remain functional.
   No further ADC attempts/routing should occur after observed host evidence.

USB `current_max` may still show 100000 or 500000 after enumeration: that is
USB-stack accounting. The charger's `ceiling_ua` is the board policy result.
Use a cool, partially discharged pack for an actual-current/ramp test; full
capacity or termination can legitimately reduce the live step. Report ceiling,
nominal/live setting and battery current separately.

## Keep round9 behavior

- Reset/suspend the Mac link while VBUS stays present. Keep the same epoch,
  computer identity and user cap; no new analog sampling.
- Unplug for at least five seconds, then return to a wall source. Require a new
  epoch, cleared `computer_seen`, no stale SDP/board grant, and fresh BC/CC.
- Fast Mac→wall→Mac swaps at the iPad end. A valid changed ID or observed phase
  drop must revoke the old source even when a VBUS-low sample is missed. An
  entirely unobserved same-ID swap remains a polling limitation.
- The attach-default 0x02 reset in the 15-second settling window must be repaired
  without false adaptive backoff or persistent live 100000 beside a higher step.
- A failed CC refresh must retain this epoch's verified BC allowance, with
  bounded retry/logging. No ADC-poison clearing or guessed register writes.
- At negative current, battery STATUS must be Discharging even at 100%; offline
  zero/stale-positive cached current should be Not charging. Observe the
  desktop icon separately; the current cache refresh can take 30 seconds.
- On a settled source, explicit 500000/1000000 caps and zero disable still work.
  Host promotion must not revoke an existing cap; a new epoch must revoke it.
- Apple USB-A brick and powered camera-adapter host regression if available;
  report unavailable hardware as NOT RUN. Camera ACA behavior remains separate
  from an external computer using the iPad's gadget.

## Collect

```sh
scripts/omarchy/ssh.sh 'sudo -n systemctl stop charge10-watch.service; cat /var/tmp/charge10.tsv' > build/charge10/device-watch.tsv
scripts/omarchy/ssh.sh 'sudo -n dmesg' > build/charge10/device-dmesg.txt
scripts/omarchy/ssh.sh 'cat /sys/bus/i2c/devices/0-001a/lightning_state; cat /sys/class/power_supply/charger/charge7_state; cat /sys/class/power_supply/battery/status; cat /sys/class/power_supply/battery/current_now; sudo -n cat /sys/kernel/debug/apple-usb-role/regs' > build/charge10/device-final.txt
```

Report PASS/FAIL/NOT RUN per source: ID, epoch/reason, computer latch, passive and
BC/CC voltages, type, supplier allowance, charger ceiling/step/live value,
attach resets/backoffs, battery measurements and tether continuity. Hardware
success cannot be inferred from the offline replay alone.
