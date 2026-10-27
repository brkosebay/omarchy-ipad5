# Ambient light and Smart Cover: device test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Kernel branch `a9/sensors` (based on `a9/periph`, head `df595c3247`, three commits on top of `64221b189e`), plus the tablet layer in `scripts/omarchy/` (auto-brightness `ipad-als-brightness`, cover handling `ipad-lid`). Nothing here has run on the iPad: the kernel and the daemons are checked offline (below), the hardware questions are the ones in [`ds-als-hall.md`](ds-als-hall.md) §5 (A1–A4, H1–H6). The orchestrator runs this plan; agents only write it.

## What changed, and what is still a guess

| Piece | State |
|---|---|
| ALS lux (`tsl2583.c` / `tsl2583.h`) | The LSCI computation moved into `apple_als_calc_lux()` in the header. Checked on the Mac against this unit's blob and an independent implementation (5120 cases, 0 mismatches): `scripts/sensors/test-ct819-lux.sh`. New: saturation at 1024 counts per cycle (the old check missed it for integration times under 64 cycles), clamp at 5000 lux (iOS's `ALSSensorMax`). **Not** checked against a real lux meter (A3). |
| CONTROL power bit (A1) | The driver still writes `PWR_ON \| ADC_ENABLE` (0x03), reads CONTROL back, requires the ADC bit to stick, logs once if the power bit does not (`CONTROL reads 0x.. after writing 0x.. (power bit not kept)`). `tsl2583.apple_no_pwr_bit=1` writes 0x02 like iOS. |
| Interrupt clear (A2) | Still the TSL258x special function (0xe1); a failing clear no longer discards the counts. `tsl2583.apple_int_clear=1` writes 0x40 to command 0x8c, `=2` clears nothing. Both parameters are writable at runtime under `/sys/module/tsl2583/parameters/`. The driver polls; the INT line (AP pin 71) is left disabled in the part. |
| Hall (H1, H2) | Two `gpio-keys-polled` devices on PMIC GPIO lines 0 and 10: `iPad Smart Cover` (`SW_LID`) and `iPad Smart Cover 2` (`SW_MACHINE_COVER`), 50 ms poll and debounce, active low (guess). `gpio-apple-d2365` no longer clears the FUNCTION bit of a line it is asked for as an input: it refuses with `-EBUSY` and a warning that carries the config register, and writes nothing for a line that is already an input. |
| Userspace | `ipad-als-brightness` is **off** until switched on; `ipad-lid` takes **no action** until `ipad-lid on`; nothing suspends unless `ipad-lid suspend on` (s2idle resume still crashes). logind is told to ignore a lid switch. |

Offline checks already done (rerun any time): `python3 scripts/omarchy/qemu/test-als-lid.py` (both daemons against fakes), `scripts/sensors/test-ct819-lux.sh`, `scripts/omarchy/qemu/test-als-lid-vm.sh` (QEMU VM with a uinput cover switch and real Hyprland DPMS), `scripts/build-kernel.sh --tree sensors` (kernel and dtbs build clean; the KUnit file compiles).

## 0. Build and boot

```sh
scripts/sensors/configure-kernel.sh          # once: .config from the periph tree + KEYBOARD_GPIO_POLLED=y
scripts/build-kernel.sh --tree sensors       # build/kernel-sensors/{Image.gz,dtbs/s8003-j71t.dtb,config,COMMIT}
```

> Private graphics-dependent excerpt omitted from this source snapshot.

Boot it the usual way, then install the layer (works with or without the kernel pieces; the units skip themselves):

```sh
scripts/omarchy/hot-apply.sh
```

The check ends with an `auto-brightness:` line (unit and sensor state) and a `cover:` line (unit, the switch devices, logind's `HandleLidSwitch` = `"ignore"`).

## 1. Kernel messages

```sh
dmesg | grep -i -E 'tsl2583|ct819|light sensor|d2365|smart cover|gpio-keys|LSCI'
```

| Expect | Meaning if missing |
|---|---|
| `loaded LSCI v2 ambient-light calibration` and `Light sensor found.` (as in round 3) | the blob must be in `/usr/lib/firmware/apple/j71t-LSCI.bin` |
| `d2365-pmic-gpio: line 0 config 0x.. (input)` and `line 10 config 0x.. (input)` | **H2.** Copy the two values. `input` and no further message: Option A works, go on. |
| `line 0 belongs to a PMIC function (config 0x..), not using it as a GPIO input` (and the same for line 10) | **H2 answered "function mode"**: the hall needs the PMU event path (Option B in `ds-als-hall.md` §4.2). Skip §3; send the config values. The ALS part is unaffected, and the two devices fail separately. |
| `input: iPad Smart Cover as /devices/platform/smart-cover/input/inputN` (and `... Cover 2`) | the cover devices registered |
| `CONTROL reads 0x.. after writing 0x.. (power bit not kept)` (only if it happens) | **A1:** the part does not keep POWER. Harmless; note the value. |

## 2. Ambient light

```sh
for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name)" = ct819 ] && break; done
cat $d/name $d/in_illuminance_input $d/in_illuminance_calibscale $d/in_illuminance_integration_time
```

Expect `ct819`, a number, `8`, `0.400000`.

**2.1 Does it keep sampling (A2).** The failure to look for is one good sample and then a frozen value:

```sh
for i in $(seq 40); do echo "$(date +%T) $(cat $d/in_illuminance_input)"; sleep 1; done
```

While it runs, cover the sensor with a finger for 5 s, then uncover it (it is a pinhole in the bezel, normally next to the front camera; shine a torch along the top bezel if you can't find it). The value must follow within about 1 s (one 400 ms integration). If it freezes after the first read, in this order, re-running the loop each time:

```sh
echo 1 | sudo tee /sys/module/tsl2583/parameters/apple_int_clear     # 0x8c = 0x40 instead of the special function
echo 2 | sudo tee /sys/module/tsl2583/parameters/apple_int_clear     # no clear at all
echo 0 | sudo tee /sys/module/tsl2583/parameters/apple_int_clear     # back to the default
```

Record which setting works (A2). If none does, send the dmesg and `sudo i2cget -f -y <bus> 0x39 0x93` (the status register; the bus number is the one `i2cdetect -l` lists for the i2c2 controller at `20a112000`).

**2.2 Power bit (A1).** Read CONTROL while the sensor is awake, i.e. within 2 s of a read: `sudo i2cget -f -y <bus> 0x39 0x80`. `0x03` means POWER is kept, `0x02` means the part ignores bit 0. Then `echo 1 | sudo tee /sys/module/tsl2583/parameters/apple_no_pwr_bit`, wait 3 s (autosuspend), read `in_illuminance_input`, and check that the readings are still sane: the iOS value (`0x02`) works if they are. Put it back with `echo 0`.

**2.3 Autosuspend.** Read the value, wait 4 s, read again: the second read takes about 0.45 s (power-up and one integration), then a read every second stays quick. `dmesg` must be quiet.

**2.4 Dark and bright, against a phone light meter** (any lux app, phone next to the sensor, screen away from it). The iPad's own backlight leaks into the sensor, so turn auto-brightness off (the default) and note the backlight setting:

| Condition | Phone lux | `in_illuminance_input` | Notes |
|---|---|---|---|
| sensor covered with a finger, then with black tape | 0 | 0–2 | the dark counts of this unit's record are 0; a floor above ~5 means light leaks past the bezel |
| dim room, lamp off, screen at 50 % | ~5–30 | | |
| room lighting | ~100–400 | | |
| next to a window, daytime | ~1000–3000 | | |
| phone torch 5 cm away | thousands | **5000** | the clamp; a saturated channel also reports 5000 |
| direct sun through glass | | **5000** | no overflow, no negative numbers |

Fill the third column; the ratio to the phone is what `lux_scale` in `~/.config/linux-a9/als-brightness.conf` should correct (A3). A constant factor is expected (cover glass, spectral response); a ratio that changes with the source (lamp vs. daylight) is the infrared channel, not a scale error: send the raw counts `in_illuminance_both_raw` and `in_illuminance_ir_raw` for the two conditions.

**2.5 Other IIO attributes.** `echo 0.100000 | sudo tee $d/in_illuminance_integration_time` (valid: 0.05–0.65 s in 0.05 steps) and read again: the value is the same light, within a few percent, which tests the normalization to the calibration's 400 ms; `echo 1 | sudo tee $d/in_illuminance_calibscale` (valid: `1 8 16 128`) likewise. Put them back (`0.400000`, `8`).

**2.6 iio-sensor-proxy.** `busctl --system get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy HasAmbientLight` should be `b true`, and `monitor-sensor --light` (or plain `monitor-sensor`) should print `Light changed: N (lux)`; `unit: lux`, not `vendor`. If the proxy does not see the sensor the daemon reads sysfs itself (`ipad-als-brightness --list` says which).

## 3. Smart Cover hall

Needs a Smart Cover or any magnet (bring it to the left edge of the bezel; the sensor position is not documented, so sweep it along the edge).

**3.1 Measure the polarity (H1) and the two lines (H4).**

```sh
ipad-lid --list                     # the devices and SW_LID=? SW_MACHINE_COVER=? now
ipad-lid --watch 90                 # every event with a time stamp; (evtest /dev/input/eventN also works)
```

Do, in this order, about 3 s apart, and write down which events appear (`SW_LID` is line 0, `SW_MACHINE_COVER` line 10):

| Step | `SW_LID` | `SW_MACHINE_COVER` |
|---|---|---|
| cover absent / open (state at boot, from `--list`) | | |
| cover closed over the screen | | |
| cover opened | | |
| cover folded back behind the iPad (Smart Cover stand position) | | |
| cover removed | | |
| bare magnet at the edge, then away | | |

Reading it: with the cover **closed** `SW_LID` must be **1**. If it is 0 with the cover closed (and 1 open), the guess is wrong: set `J71T_COVER0_ACTIVE` to `GPIO_ACTIVE_HIGH` in `s8003-j71t-periph.dtsi` and rebuild, or flip a built dtb without a build: `scripts/sensors/cover-polarity.sh build/kernel-sensors/dtbs/s8003-j71t.dtb active-high lid`. Same for line 10 (`J71T_COVER1_ACTIVE`, `aux`, `both`). As a stopgap needing no new kernel, `invert = on` in `~/.config/linux-a9/lid.conf`. No events at all with the cover moving: check §1 for the `PMIC function` warning; otherwise read the PMU registers (`ds-als-hall.md` §4.4 step 3) with the cover closed and open and send both.

If line 10 never changes, or tracks line 0 exactly, it is the same magnet or a dead line: H4 is "one cover state", `cover2 = ignore` (the default) is right. If it differs (for example only in the folded-back position), tell me: it may become a separate switch.

**3.2 Debounce (H3).** `ipad-lid --watch 90` while opening and closing the cover 20 times at a natural speed. A clean transition is one event; two or more events within 100 ms are bounce. The kernel already debounces 50 ms and the daemon waits another `settle` (0.4 s) before acting, so only bounce longer than that matters: raise `debounce-interval` in the dtsi, or `settle`.

**3.3 Latency and boot state.** Close the cover and reboot with it closed: `ipad-lid --list` after boot must say `SW_LID=1` (a polled switch reports its state at once, an edge-only IRQ would not).

## 4. Userspace

Run `scripts/omarchy/hot-apply.sh` first. Everything below is on the iPad (SSH or the on-screen terminal).

**4.1 Auto-brightness.**

```sh
systemctl --user status ipad-als-brightness     # active (running) with a sensor; "inactive (condition no)" without
ipad-als-brightness --list                      # sensor, a lux reading, the backlight, brightnessctl
ipad-als-brightness --probe 20                  # dry run: lux, smoothed lux, what it would set (move your hand over the sensor)
```

Then the real thing, with the iPad flat on a table:

1. Switch it on with the bar's sun button (landscape bar, appears only while the daemon runs) or `ipad-als-brightness on`. The button lights up. Brightness should move to the curve's value for the room, as a short ramp, not a jump.
2. Cover the sensor, then uncover it. Brightness must go down and come back within a few seconds, without flicker. A shadow passing for under a second must change nothing.
3. Press a brightness key (or `brightnessctl set 80%`). Auto-brightness pauses for 30 s (`journalctl --user -u ipad-als-brightness` says `brightness changed by hand`), then carries on from there.
4. Leave the iPad alone for 3 minutes: hypridle dims it (`ipad-als-brightness dim`). It must stay dim, and `status` shows `hold: ["dimmed"]`. Touch it: it comes back to the brightness before the dim (or the ALS level one second later).
5. `ipad-als-brightness off` (or the button): it stops at once and leaves the brightness where it is.
6. If the screen's own light feeds back (brightness hunting up and down in a dark room), raise `hysteresis` (0.35) or `smooth_secs` (6) in `als-brightness.conf`; if the curve is too bright or too dark, edit `curve` or `lux_scale`.

**4.2 Cover.** Only after §3.1 shows `SW_LID` = 1 for closed.

```sh
ipad-lid status                                  # actions off, suspend off, the cover state
ipad-lid on                                      # or the iPad Screen menu: "Cover Closes Screen On"
```

1. Close the cover: the display goes dark (backlight 0 and DPMS off), `journalctl --user -u ipad-lid` says `cover closed`. The user has **no password**, so it logs `not locking`: that is intended. To see the lock anyway: `lock = on` in `lid.conf` and a restart (then be ready to unlock it with a keyboard).
2. Open it: the backlight comes back to what it was, DPMS on. With a password set (`passwd`) and `lock = auto`, closing also starts `hyprlock`; opening leaves it for you to unlock.
3. With auto-brightness on: it does not fight the dark screen (`hold: ["lid"]`) and resumes about a second after opening, without the `changed by hand` message.
4. Close and open five times quickly: the screen must end up on and bright; `systemctl --user status ipad-lid` stays active.
5. `reboot` with the cover open, then with it closed: with actions on, the screen must be off after login when closed and on when open.
6. **Do not test `ipad-lid suspend on` until s2idle resume is fixed.** When it is: `ipad-lid suspend on`, close the cover, the iPad suspends after 3 s (`journalctl -b` shows one `ipad-sleep`); waking needs Home or Power (the polled switch is not a wake source: H6).
7. `ipad-lid off` with the cover closed brings the screen back.

**4.3 Things that must not have changed.** `systemctl --user --failed` empty; `ipad-autorotate`, the gestures and the on-screen keyboard behave as before; the Bluetooth and charger work that shares `periph_pmu_gpio` (the dtsi un-reserves lines 0 and 10 only): `bluetoothctl show` powered, `cat /sys/class/power_supply/*/status`.

## 5. Back out

- Userspace: `ipad-als-brightness off; ipad-lid off`, or `systemctl --user disable --now ipad-als-brightness ipad-lid`. Hypridle's dim then still works (`ipad-als-brightness dim` is `brightnessctl -s set 10%` without the daemon).
- Kernel: boot `a9/periph` (or integ) instead; the dtsi change is the only one that alters what the PMIC GPIO driver exposes, and it reads lines 0 and 10 without writing them (`direction_input` is a no-op for an input line).
- A line that the `gpio-apple-d2365` driver found in output mode would be switched to input by the old and the new driver alike; this plan never sets a line as output.

## 6. Results to send back

| ID | Answer |
|---|---|
| A1 | CONTROL read-back value; does `apple_no_pwr_bit=1` work |
| A2 | which `apple_int_clear` value keeps sampling (0, 1 or 2) |
| A3 | the phone-vs-sensor table of §2.4 |
| A4 | not measurable from Linux; the driver polls and keeps the part's INT disabled |
| H1 | closed = `SW_LID` 1 or 0 (and the fix applied) |
| H2 | the config bytes of lines 0 and 10 from dmesg; `input` or the `PMIC function` warning |
| H3 | bounce seen or not |
| H4 | what line 10 does in each position |
| H6 | not tested (polled, no wake) |
