# Charge7 device test plan — orchestrator only

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implementation agent ran no device steps. Boot the entry named in
`build/charge7/entry-name`, prepared by `scripts/periph/build-charge7.sh`.
The payload is under `build/charge7/`; manifest and entry SHA256SUMS must verify.
Command line differs from USB-host5b only in `linux_a9.entry`: exactly one
`pcie_apple_h9p.ans_keep_power=1`, no debug shell, unchanged loglevel.

**Open prerequisites:** voltage-triggered weak-source protection is not proved.
`last_backoff=pmic-input-reduced` is a response to a setpoint change, not evidence
of VBUS droop. Sub-poll VBUS pulses can be missed. Do not declare either
requirement passed solely because the offline model passes.

## Start a persistent, bounded logger

Use Wi-Fi SSH so the control path survives USB removal. The existing
`scripts/omarchy/ssh.sh` honors `OMARCHY_HOST`; use the known Wi-Fi host from
`build/omarchy/wifi-host`. The logger itself runs locally on Linux, not on the
Mac. It only reads sysfs and records one sample per second under normal I/O;
each read is limited to 0.5 s, the whole run to 1800 s by default, at most 3600 s,
with a two-second forced-stop grace. Timeout exit 124 at the deadline is normal.
No cable countdowns occur. USB disconnection does not stop local logging.

```sh
export OMARCHY_HOST="$(cat build/omarchy/wifi-host)"
scripts/omarchy/ssh.sh 'cat > /tmp/charge7-watch.sh' < scripts/periph/charge7-watch.sh
scripts/omarchy/ssh.sh 'sudo -n systemd-run --unit=charge7-watch --collect --property=RuntimeMaxSec=1810 /bin/sh -c "WATCH_SECONDS=1800 sh /tmp/charge7-watch.sh > /var/tmp/charge7.tsv 2>&1"'
scripts/omarchy/ssh.sh 'cat /sys/class/power_supply/charger/charge7_state; tail -3 /var/tmp/charge7.tsv'
```

Record a baseline `uname -a`, `/proc/cmdline`, `dmesg`, `lsusb -t`,
`/sys/bus/i2c/devices/0-001a/lightning_state`, and
`/sys/kernel/debug/apple-usb-role/regs` while the Mac cable is attached.
The watcher logs online, USB type/current_max, live input limit, battery
current/voltage/temp/status, and the policy reason. A read error is `err` or a
negative errno; it is never substituted with zero.

## Owner-controlled cable sequence

Stay on each source until its type and limit have settled and enough battery
samples have arrived (the gauge normally refreshes on a slower interval than
the watcher). The owner can swap whenever ready; mark wall-clock times and
the brick/cable label in a separate note.

1. Mac cable. Once enumerated, expect SDP and **2400000 µA ceiling** in
   `charge7_state`, with floor→1000→2100→2400 nominal ramp at one-second steps.
   `usb/current_max` may still say 500000: it is USB stack accounting. Confirm
   NCM/ECM, ACM, SSH, root I/O and the NBD link remain working.
2. Owner unplugs, then plugs each available Apple 12 W / 10 W / 5 W brick.
   Expect offline/epoch increment during the observed removal, then a fresh
   classification and no retained user/adaptive cap from the Mac.
3. Try each available USB-C charger with USB-C-to-Lightning cable. Expect
   classification from its actual signature/BC behavior; there is no USB-PD
   negotiation implementation. Record UNKNOWN/floor as a detection failure
   needing investigation, not a successful high-current test.
4. Back to Mac. Expect a new epoch, no old brick ceiling or override, SDP and
   2400 mA ceiling again. Test suspend/resume or bus reset without removing
   VBUS; epoch and ceiling must remain unchanged, with no repeated input-disable
   writes or brick sampling on the active gadget.
5. Fast unplug/replug below one second, both orientations if convenient.
   Check epoch increment and return to 2400 mA. Repeat Mac→brick→Mac quickly.
   If no low VBUS sample was captured, record **missed detach**, even if charging
   happens to recover. The current polling design cannot guarantee every pulse.
6. Optional camera adapter with its external power source. Expect DigitalID
   `11 f0`, normal power handshake, host role and ACA 2100 mA ceiling. Check hub
   enumeration and a USB device as in the USB-host5b plan. Return to Mac and
   verify device role, new epoch, cleared handshake/ACA state, SDP 2400 mA and
   working tether. An unpowered/manual host role must not enable self-charging.

## Expected limits

These are source signatures, not a promise that every product label selects
one fixed signature. Record the actual `usb_type` and detection log.

| Detected source | Ceiling µA | Highest nominal rung µA | This unit's usual live `input_current_limit` µA |
| --- | ---: | ---: | ---: |
| Ordinary computer / SDP | 2400000 | 2400000 | 2400000 (`0xba`) |
| Apple signature 2.5/2.5 V, typically 12 W | 2400000 | 2400000 | 2400000 |
| Apple signature 2.5/2.0 V, typically 10 W | 2100000 | 2100000 | 2075000 (`0xa0`) |
| Apple 1000 mA signature | 1000000 | 1000000 | 962500 (`0x47`) |
| Apple 2.0/2.0 V signature | 500000 (iOS table entry 1) | 500000 | 475000 (`0x20`) |
| BC1.2 DCP or CDP | 1500000 | 1000000 | 962500 |
| Unknown / unproved signature | 500000 | 500000 | 475000 |
| Powered camera adapter after handshake | 2100000 | 2100000 | 2075000 |
| Camera adapter before handshake | 500000 | 500000 | 475000 |

At a cool, partially discharged pack, positive battery current and increasing
charge are required on adequate sources. At full charge, termination can hold
the floor and battery current can be zero; that is not proof of a charger
detection failure. At 500 mA input, system load can exceed the available power,
so negative net battery current alone does not disprove the programmed limit.

## Weak-source experiment and interpretation

If a known 500 mA port/hub and a USB power meter are available, capture its
behavior separately from root-filesystem-critical traffic. An independent
record of actual VBUS voltage/current is needed: the kernel has no verified
VBUS measurement. A port that disconnects overcurrent can interrupt the tether
and NBD session. Only the orchestrator chooses whether to run this experiment.

First test that a per-attach 500000 µA user cap is honored. Then, if appropriate
for the available current-limited source, remove that cap only by a new attach
and observe the automatic ramp. If the PMU reduces its own `0xa03`, expect
`last_backoff=pmic-input-reduced`, a lower rung, no immediate increase of that
code, a 60 s hold, and at most one upward retry each minute. Any retry must stay
under the detected ceiling. Replug must clear that adaptive cap/cause.

**Voltage-back-off pass criteria are intentionally still unmet:** a measured sag
with unchanged `0xa03` cannot trigger the present implementation. Record it as
FAIL/unsupported, not PASS. A full VBUS collapse followed by a fresh ramp is
also a failure of the requested weak-source protection. Do not simulate it by
writing PMIC registers. Safe implementation needs verified VBUS/collapse
feedback or proof of the hardware regulation setting from iOS.

## Collect results

```sh
scripts/omarchy/ssh.sh 'sudo -n systemctl stop charge7-watch.service; cat /var/tmp/charge7.tsv' > build/charge7/device-watch.tsv
scripts/omarchy/ssh.sh 'sudo -n dmesg' > build/charge7/device-dmesg.txt
scripts/omarchy/ssh.sh 'cat /sys/bus/i2c/devices/0-001a/lightning_state; sudo -n cat /sys/kernel/debug/apple-usb-role/regs' > build/charge7/device-final.txt
```

Record each source, sampled type, ceiling/live limits, battery measurements,
epoch before/after, tether continuity, and PASS/FAIL/NOT RUN. Detection/replug
passes require the correct **new** source after every observed detach, zero
stale grant/ACA/override, and no link-PM epoch change. Camera behavior requires
the handshake and real USB-host test, not ACA text alone. Leave voltage-triggered
adaptation and unobserved fast detach guarantees marked unresolved.
