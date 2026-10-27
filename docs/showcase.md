# Showcase cold boot (filming): auto-rotate + 2.16 GHz + no-cable boot

> Historical design/test notes. Current support: [status](release/STATUS.md); examples may need local firmware/captures.


2026-10-26. One DFU cold boot in which auto-rotate (AOP accelerometer → IIO →
iio-sensor-proxy → `ipad-autorotate`), the CPU at 2160 MHz behind the a9_tuning
guard, auto-brightness and the iPad fastfetch all work together, and the boot
does not hang without a Mac on the USB gadget. Not run on the device yet.

## Payload

| | |
|---|---|
| Entry | `showcase-aop-oc-9ab7e00531` |
| Payload | `build/showcase/m1n1-showcase-aop-oc-9ab7e00531.bin` |
| SHA-256 | `99c2cca7eee1856decba0d481e60c1c9b071c557170446a267a2168e8a692010` |
| Kernel | `a9/aop-wake4` `9ab7e00531` (`7.3.0-rc1-aop-wake4-g9ab7e00531a7`), **not rebuilt** |
| Entry dir | `build/showcase/showcase-aop-oc-9ab7e00531/` (`entries.env`: `SHOWCASE_*`) |

The AOP kernels already contain the a9/power round-5/6 cpufreq/thermal tuning code:
`apple_soc_cpufreq.a9_tuning`, `a9_passive_millic`, `clock_profile=s8003-r6-shared-slot`
(the profile the current `scripts/power/ladder.py` expects). Same config as tri49a, debug info
off. So this is a repackage of the wake4 AOP entry `aop-ch3tdq-accel-9ab7e00531`. Its cold path
is wake3's, and leg A of wake3 passed on 2026-10-26 17:35.
`scripts/showcase/package.py --base wake3` builds the same thing from `63eadc3456`.

Changes from the base entry. The Image, initramfs (with the four private AOP files), m1n1
(`psci=1`) and the other three DTBs are byte-identical:

1. **cmdline** (983 bytes): `systemd.debug_shell=ttyGS0` is dropped (the usb-host3 no-cable
   hang). `systemd.mask=maintainer@example.invalid` stays. The entry is renamed.
   `apple_soc_cpufreq.a9_tuning=1 apple_soc_cpufreq.a9_passive_millic=90000` is added: the
   round-5 sustained profile, 90 °C passive and guard revert, 95 °C hot, 100 °C critical, arm
   below 85 °C. These params are 0400, so they only take effect at boot. Still exactly one
   `pcie_apple_h9p.ans_keep_power=1`, plus the full kexecfix set and the deferred test-5 AOP
   tokens.
2. **j71t DTB, CPU OPP table only** (`s8003-j71t-showcase.dtb`): stock `opp01..opp07` plus the
   single 2160 MHz turbo OPP (`opp14`, level 14). The 217 other turbo OPPs are removed:
   1848, 1896..2088 and the round-6 `opp-r6-*` up to 12264 MHz. The reasons:
   - With boost on, `cpufreq_frequency_table_cpuinfo()` sets `cpuinfo_max_freq` to the
     largest turbo OPP and never lowers it. The full table would make it 12264000, and
     fastfetch reads that value.
   - schedutil could resolve to an unconfigured turbo rung, which
     `apple_soc_cpufreq_set_target()` refuses with -EPERM.
   - The guard does not need the removed rungs: `a9_check_stock` only maps 2160 → slot 14, and
     `set_boost` checks state 8 by number, not by table entry.
   - The cooling maps are all `THERMAL_NO_LIMIT`.
   - The package test proves that the DTS differs from the base only by those
     `/opp-table` subnodes, and that none of them has a phandle.

## Orchestrator sequence

```sh
source build/showcase/entries.env
scripts/boot.sh pongo                       # put the iPad in DFU when told
scripts/boot.sh linux "$SHOWCASE_PAYLOAD"
scripts/showcase/bring-up.sh                # waits up to 300 s for SSH
```

`bring-up.sh` prints a PASS/WARN/FAIL line per step and `RESULT:`. It exits 0 when no step
failed. Everything is logged in `build/showcase/runs/<UTC>/`. Each remote call has a hard
time bound, and nothing waits for the owner. It is safe to re-run: the AOP trigger fires
only while `cold_release` reads `triggered=0`, and a running hold is left alone.

| Step | What it does / requires |
|---|---|
| entry | `/proc/cmdline` has the entry, both tuning args, no `debug_shell`, one `ans_keep_power=1`. `uname -r` matches. The live `/proc/device-tree/opp-table` is stock + 2160 only. **Stops here on failure.** |
| firmware | The 4 private files in `/lib/firmware/apple/aop` match `root-firmware/SHA256SUMS`. If they don't, it installs them as the stage-3 section does. |
| aop | On the first run, leg A exactly: `AOP_CAPTURE_HOLD_SECONDS=60 trigger-cold.sh showcase-cold`. Then `phase=streaming`, `iop_crashed=0`, `cal_sent=3 cal_unsupported=1 cal_state=2 iio=1`, reports increasing over 3 s, one `SPI0 reset decision=skip`, no `SPI0 CH3TD`, no crashlog, IIO `apple-a9-aop-accel` readable. |
| rotate | iio-sensor-proxy `HasAccelerometer` = `b true`. `ipad-autorotate` active on `source=proxy` with a sensor orientation; the unit is restarted once if it isn't. WARN if the rotation lock is on. |
| cpu | Copies `ladder.py`, `a9_thermal.py` and `a9-oc-hold.py` to `/root/a9-showcase`. Runs `preflight`, then starts `systemd-run --unit=linux-a9-oc-hold -p Nice=-15 … hold --khz 2160000`. Requires the guard to be held: `armed=1 latched=0 limit_khz=2160000 configured=4000 delta_uv=0`, plus `cpuinfo_max_freq=2160000`, `scaling_max_freq=2160000` and `boost=1`. An 8 s two-worker load must run at 2160 MHz in at least 80 % of samples, using the guard's hardware-decoded `actual_khz`. |
| als | `ipad-als-brightness enabled`, its user unit active, and an IIO light sensor present. If it was off, it is switched on and reported as WARN. |
| fastfetch | Installs `~/.config/fastfetch/config.jsonc` when it is ours or missing. Prints the CPU line (`@ 2160 MHz`) and the Wi-Fi line. |

Stop the overclock (back to stock): `scripts/showcase/bring-up.sh --stop-cpu`. If the guard
reverted, for example at 90 °C, the hold daemon exits and does **not** re-arm. Let the SoC cool
below 85 °C and run `bring-up.sh` again. `--governor performance` pins 2160 MHz while held;
the default leaves schedutil.

**Sleep also ends the hold.** s2idle freezes the daemon, so the guard reverts with
`heartbeat-timeout` within 2 s of resume, just as it does after a 90 °C trip. Re-run
`bring-up.sh` after waking, with the Mac attached; the re-run takes about a minute because
the AOP and hold steps are skipped. Between takes the fastfetch CPU line shows the truth:
it reads `scaling_max_freq`, the effective cap after QoS, which is 2160 MHz only while held
and 1800 MHz after any revert. `cpuinfo_max_freq` stays 2160000 for the rest of the boot
once boost was on. A possible follow-up, not built: an opt-in `--rearm`, capped, that re-arms
after a heartbeat timeout across a gap the daemon itself saw, or after `thermal-passive` once
below 80 °C. It would never re-arm after hot, critical, sensor-error or voltage-mode.

Leaving this boot: power off, or DFU. Do **not** `linux-a9-kexec` with `--reuse-cmdline` or
into an entry-less warm boot, because the tuning args would carry over. Nothing here sets a
default entry or uploads this entry to `/boot/linux-a9/entries`. The wake4 warm reload is
still device-untested.

## The 2160 MHz hold (`scripts/showcase/device/a9-oc-hold.py`)

The hold runs the same guard sequence as `ladder.py`'s trial and `bench-ab.py`, the 2160 MHz
A/B from round 5:

1. Stock is confirmed (`ensure_stock`, voltage handoff), the sustained profile is checked in
   both debugfs interfaces, and every trusted sensor is below 85 °C.
2. `arm`, then `configure 2160000 0` (**delta 0, stock voltage**), then the PSINFO/DAC readback.
3. boost=1, `scaling_max_freq=2160000`, heartbeat.

Every 0.5 s it checks that the guard still holds the target, that every sensor is valid and
below passive, that the readback still shows delta 0, and that the clock is in range. Only
then does it send a heartbeat.

The kernel guard does all the safety work. It reverts at 90 °C passive, at 95 °C hot, on a
sensor or voltage-mode fault, and 2 s after a missed heartbeat. A dead or starved daemon
therefore means stock clocks.

The hold never writes thermal trips, never sets a voltage offset, and doesn't open
`/dev/watchdog0`; bench-ab didn't either, and a crashed daemon must not reset the iPad
mid-filming.

When the kernel reverts first, the daemon keeps the kernel's reason (exit 3). On SIGTERM it
reverts, restores `scaling_max_freq`, boost and the governor, and confirms stock.
`/run/linux-a9/oc-hold.json` holds its state. The journal is `journalctl -u linux-a9-oc-hold`.

## fastfetch Wi-Fi line

`timeout 2 nmcli -t -f active,ssid dev wifi` rescans a stale list. It overran the 2 s timeout
and printed "Wi-Fi off" while connected. The new line works like this:

- **Connected** comes from sysfs: the `phy80211` interface with IFF_UP, and
  `operstate=up` or `iw dev <if> link` → `Connected`.
- **The band** comes from iw's `freq`. If iw is missing, it uses `nmcli … dev wifi list
  ifname <if> --rescan no` with 5 s.
- It prints `Wi-Fi 5 GHz connected`, `Wi-Fi 2.4 GHz connected`, `Wi-Fi connected`,
  `Wi-Fi not connected` or `Wi-Fi off`.
- **It never prints the SSID.** The Bluetooth part is unchanged.

The CPU line now reads `scaling_max_freq` instead of `cpuinfo_max_freq` (see the sleep
note above). It shows `@ 2160 MHz` while held and `@ 1800 MHz` otherwise.

`bring-up.sh` installs the file. To install it by hand:

```sh
scripts/omarchy/ssh.sh 'install -d ~/.config/fastfetch && cat > ~/.config/fastfetch/config.jsonc' \
    < scripts/omarchy/rootfs/tablet/fastfetch.jsonc
```

`scripts/omarchy/hot-apply.sh` (tablet layer) and new images install it too, through
`rootfs/tablet/install.sh`.

## Offline checks

```sh
python3 -B scripts/showcase/package.py      # rebuilds build/showcase (no Docker)
scripts/showcase/test-offline.sh            # bash -n, shellcheck if installed, 53 tests
```

- **`tests/test-package.py`** (8 tests):
  - the payload extracts to the entry;
  - checksums are valid;
  - m1n1 and `psci=1` are in place;
  - the cmdline equals the base minus `debug_shell`, with the entry renamed and the two
    tuning args added;
  - 8 rejected cmdlines;
  - the trimmed DTB passes and 4 rejected DTBs fail;
  - the AOP memory check passes;
  - the Image, initramfs and firmware are identical to the base.
- **`tests/test-oc-hold.py`** (20): a guard model covering a clean hold, a kernel trip (no
  re-arm), a hot sensor with a kernel revert, a sensor/readback/clock failure (our revert),
  7 refusals and the load test.
- **`tests/test-fastfetch.py`** (13): the real JSONC command under sh and bash, with a fake
  sysfs and stub tools. Covers the CPU line (2160, 1800 after a revert, absent), 5/2.4/6 GHz, the nmcli fallback with `--rescan no`, no tools,
  `dormant`, down, no interface and Bluetooth, and checks that the SSID never appears.
- **`tests/test-bring-up.py`** (12): `bring-up.sh` with a fake ssh. Covers the PASS path,
  a re-run with the hold already running, a wrong entry, a full turbo table, CH3TD or
  frozen reports, firmware install, a hot preflight, a slow load, WARNs, user config and
  `--stop-cpu`. No command contains kexec, `upload-entry`, `/boot/linux-a9`, reboot or
  nvram.

## Not yet shown on hardware

- A cold boot with the tuning args: round 5 used kexec. Its docs name a cold boot with those
  args as the fallback.
- schedutil with the trimmed table reaching 2160 under load: round 5 ran with `performance`.
  If the load step fails, use `--governor performance`.
- Whether `scaling_boost_frequencies` exists on this kernel: preflight checks it only when it
  exists, and the entry step checks the live DT.
- `iw` on the image: the line falls back to nmcli without a rescan.
- Sleep/wake and a 90 °C trip during the hold: the kernel guard reverts, the daemon exits
  with code 3, and the CPU line drops to 1800 MHz. Each of these is modelled offline only.
