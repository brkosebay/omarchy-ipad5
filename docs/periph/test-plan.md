# J71t next-boot peripherals test plan (Round 6)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


All device commands are for the **orchestrator/owner**. The implementation
agent executed none. No persistent storage writes or raw PMIC operations are
needed. Touch and other device work remain with their dedicated workstreams.

**Verified on integ10 with Round 5:** the owner 2400000 write charges at
about +1.16 A and charge-now publishes valid increasing values. Automatic
fallback remained at handoff 100000 / 2600000 after an initial HDQ failure.
Round 6 keeps first-sample acquisition retryable and logs policy deferrals;
automatic 475000 / 400000 application and quiet startup reporting await this
boot. BT boot identity, ALS and PMIC DT fixes are retained.

## 1. Local host preparation

From the repo root on the Mac. All inputs are existing local artifacts:

```sh
python3 -B scripts/periph/test-charging.py
python3 -B scripts/periph/test-round6-battery.py
python3 -B scripts/periph/verify-firmware.py
scripts/periph/mkinitramfs.sh --check --require-btmgmt
cat build/kernel-periph/COMMIT
test ! -s build/kernel-periph/DIRTY
shasum -a 256 build/kernel-periph/Image.gz build/kernel-periph/dtbs/s8003-j71t.dtb build/initramfs-fw.gz
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

Expected kernel head: `64221b189e`. SHA256:

```text
bc984aa02f23540af8400fabc1b26561118eb3689f3ac20c746e4b157821edaf  build/kernel-periph/Image.gz
852decf85f7e1ca7ae2e9bc71c395428b4c3672053165f63c6e767ee85a69b5f  build/kernel-periph/dtbs/s8003-j71t.dtb
54400b6dbf0cd618d95d2d18757351a01ad44cdb3b300556bda99f1da372c1bf  build/initramfs-fw.gz
```

The archive must have one gzip stream containing both HoolockRD and the
firmware/tools cpio, preserving `/init`. Private CBAT, LSCI and BMac remain
in it; radio selection and static btmgmt are unchanged. The verifier identifies
any staged touch files separately. If another workstream has changed staged
firmware since packaging, regenerate once with
`scripts/periph/mkinitramfs.sh --require-btmgmt`, repeat the check and record
the resulting new hash before assembling the payload.

Use the existing tethered RAM-boot procedure. An integration kernel must
include patches 53–55 on top of the Round 5 peripheral series. Retain a
known-working payload; the implementation agent does not perform this boot.

## 2. Capture automatic fallback before any manual write

Run in the Linux shell on the same measured Mac USB port:

```sh
uname -a
dmesg | grep -Ei 'Bluetooth|BCM|device address|Power Level|CBTL|source budget|charging policy|owner|Apple host current grant|CBAT|cache|Temperature check|charge_now|Bad cell count|LSCI|failed'
for supply in usb charger battery; do
  printf '\n%s\n' "$supply"
  cat "/sys/class/power_supply/$supply/uevent"
done
for n in 1 2 3 4 5 6 7 8 9 10 11 12; do
  date
  for p in online type usb_type current_max; do
    printf 'usb/%s=' "$p"
    cat "/sys/class/power_supply/usb/$p"
  done
  for p in online status input_current_limit constant_charge_current; do
    printf 'charger/%s=' "$p"
    cat "/sys/class/power_supply/charger/$p"
  done
  for p in status capacity voltage_now current_now temp charge_now charge_full; do
    printf 'battery/%s=' "$p"
    cat "/sys/class/power_supply/battery/$p" || true
  done
  sleep 5
done
dmesg | grep -Ei 'charging policy|source budget|cache refresh|driver failed to report|Temperature check|restor|revok'
```

With no Apple grant, expect `usb/current_max=500000`, online=1 and
charger input `475000` (this unit's calibrated nominal 500-mA setting).
After the first complete safety sample, expect `constant_charge_current=400000`, with:

```text
source budget 500 mA: input 0x20, battery limit 400 mA
```

If the first gauge read fails, the policy should first log
`charging policy waiting for valid battery/source sample; retry in 1000 ms`.
The gauge retries after one second until its first complete safety sample.
The next one-second policy evaluation should apply the limits without any
owner write. A persistent wait/guard failure is logged again every 30 seconds;
a permanent latch explicitly says owner retry is required. Capture that
message if limits remain at handoff values; do not repair them before
saving the automatic-test evidence.

Initial unavailable cache properties return ENODATA, including unknown
presence. Their uevent fields may be absent until ready. There should be no
initial `battery cache refresh failed: -5`, battery status EIO warning or
`Temperature check failed` spam while the first sample is pending. Thermal
must retry rather than substitute a temperature or disable the zone. Genuine
mandatory errors after a first valid sample remain visible and guarded.

The battery charging path is enabled with a nonzero conservative ceiling.
Its actual net current depends on system load and the total input-current
limit; it may remain negative if the active system consumes all input power.
Record both programmed limits, status and current rather than interpreting
400000 as a measured charging current. Battery must still be present,
10–40 °C and 3.0–4.2 V, without termination/timeout, with valid CBAT and a
sufficient hardware current cap. A hot/full battery may correctly refuse.

**Battery uevent check:** when the optional charge sample is unavailable,
`cat battery/charge_now` may return ENODATA, but `cat battery/uevent` should
still print the other valid fields. There should be no optional-charge EIO
warning. On a later valid sample, CHARGE_NOW should reappear. Numeric charge-now was verified on integ10; capture it again with charge_full
and capacity to confirm recovery with the new first-sample retry path.
Gauge-wide or other mandatory-property errors remain diagnostic failures.
`charge_full_design` remains absent; no design capacity is fabricated.

## 3. Explicit owner authorization on the measured Mac port

After the fallback capture, execute the same request that charged in Round 3:

```sh
printf '2400000\n' > /sys/class/power_supply/charger/input_current_limit
printf 'override rc=%s\n' "$?"
cat /sys/class/power_supply/usb/current_max
cat /sys/class/power_supply/charger/input_current_limit
cat /sys/class/power_supply/charger/constant_charge_current
for n in 1 2 3 4 5; do
  date
  cat /sys/class/power_supply/charger/uevent
  cat /sys/class/power_supply/battery/uevent
  sleep 35
done
dmesg | grep -Ei 'owner|source budget|charging policy|revok|restor|CBAT|timeout|failed'
```

Expected rc=0, input `2400000`, battery-current ceiling `2500000`, and logs:

```text
owner authorizes 2400 mA input; detected allowance 500 mA
owner override budget 2400 mA: input 0xba, battery limit 2500 mA
```

`usb/current_max` should remain 500000 when there was no host grant. The
owner's write explicitly authorizes the known source; it does not falsify
USB negotiation. Higher requests are not automatic merely because a cable
is attached or the link is high-speed. Use this request only on the source
already measured by the owner under iOS/Linux.

Allow a gauge refresh before assessing current. Expected target: Charging
and positive battery current comparable to the earlier +1.18 A at similar
SOC/load. The iOS +1.7 A reference is not a fixed Linux requirement. If the
write fails or current stays negative, retain full dmesg, limits, source
uevent and battery temperature/voltage. Do not write raw PMIC registers.

The request still rejects absent/failed VBUS, less than an ordinary 500-mA
allowance (including USB suspend/100-mA enumeration), invalid calibration,
battery safety violations, termination/timeout or insufficient hardware cap.
A supported positive write also explicitly retries a latched fault under
fresh guards. Failed activation/replacement clears owner authorization.

## 4. Restoration and bounded low-current request

Zero disables automatic charging policy and restores its original handoff
snapshot, rather than changing the factory state:

```sh
printf '0\n' > /sys/class/power_supply/charger/input_current_limit
cat /sys/class/power_supply/charger/input_current_limit
cat /sys/class/power_supply/charger/constant_charge_current
```

Earlier handoff settings on this unit were input 100000 and charge 2600000;
record actual values rather than assuming every handoff is identical.
To request the ordinary fallback explicitly:

```sh
printf '500000\n' > /sys/class/power_supply/charger/input_current_limit
cat /sys/class/power_supply/charger/input_current_limit
cat /sys/class/power_supply/charger/constant_charge_current
```

Expected 475000 / 400000. To leave the known Mac source charging after the
check, issue the 2400000 request again. Supported nominal requests are
500000, 1000000, 2100000 and 2400000; other positive values are rejected.

Owner authorization survives stable refresh, but is revoked by observed
VBUS/source loss, a failed guard, zero request or suspend/shutdown. Reconnect
or resume does not restore it automatically; automatic policy can use the
detected allowance and the owner may re-authorize. Polling can miss very
short physical changes; this is not host identity tracking. Cable changes
also remove the USB console. Exercise reconnect only through an existing
owner recovery workflow and capture the next source/limit state.

## 5. Retained Bluetooth, ALS and desktop checks

Before any address-changing command:

```sh
dmesg | grep -Ei 'Bluetooth|BCM|device address|Power Level|Bad cell count'
btmgmt --timeout 10 --index 0 info
btmgmt --timeout 10 --index 0 config
btmgmt --timeout 10 --index 0 le on
btmgmt --timeout 10 --index 0 power on
btmgmt --timeout 15 --index 0 find -l
btmgmt --timeout 10 --index 0 stop-find -l
```

Expect the real boot-time identity, build 1312, no ROM-default warning and no
TX-power warning at normal log level. Keep identity/discovery captures
private. Still run stop-find after a client timeout. Static btmgmt does not
include bluetoothctl or full Bluetooth profile services.

```sh
dmesg | grep -Ei 'LSCI|tsl258|ct819|calibration'
for dev in /sys/bus/iio/devices/iio:device*; do
  test -d "$dev" || continue
  printf '%s: ' "$dev"
  cat "$dev/name"
  if test -r "$dev/in_illuminance_input"; then
    cat "$dev/in_illuminance_input"
  fi
done
if command -v upower >/dev/null 2>&1; then
  upower --enumerate
  upower --dump
fi
```

ALS should load LSCI v2 promptly and respond to covering/uncovering. UPower
should see the system battery and online USB power; unavailable optional
charge data must not suppress its other battery fields. Retain complete
artifact provenance/dmesg and distinguish programmed current ceilings from
measured battery current. Touch is tested by its own workstream.
