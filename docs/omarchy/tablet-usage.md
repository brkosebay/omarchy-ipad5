# Omarchy on the iPad: tablet cheat sheet

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


The tablet layer on top of Omarchy: touch gestures, bar buttons, an on-screen keyboard, layouts that suit one hand and no mouse, and a demo tour. `scripts/omarchy/hot-apply.sh` installs it on a running iPad, and the image build installs it with `phase tablet`. The sources are listed at the end.

## Bar buttons (left of the bar, after the workspaces)

| Button | Tap | Same as |
|---|---|---|
| Apps (grid) | app grid; right-click opens the Omarchy menu | swipe up from the bottom edge, 4 fingers up |
| Keyboard | on-screen keyboard on/off | swipe down from the top edge |
| Layout | tiling → tablet → floating | Super + Alt + Y |
| Float | floating on/off for the window | 4-finger tap, 3-finger pinch in |
| Full screen | full screen on/off | 3-finger tap, 3-finger pinch out |
| Rotate | next orientation | Super + Alt + R |
| Rotation lock (only while auto-rotate runs) | lock on/off; red with a padlock while locked | Super + Alt + Shift + R |
| Auto-brightness (only while its daemon runs; landscape bar) | ambient-light brightness on/off; lit while on | iPad Screen menu, `ipad-als-brightness toggle` |
| Close | close the window | 4 fingers down |

## Gestures

| Gesture | Does |
|---|---|
| 3 fingers left / right | next / previous workspace |
| 3 fingers up / down | next / previous window on this workspace |
| 3-finger tap | full screen on/off |
| 3-finger pinch in / out | floating on/off / full screen |
| 4 fingers up | app grid |
| 4 fingers down | close the window |
| 4-finger tap | floating on/off |
| swipe up from the bottom edge | app grid |
| swipe down from the top edge | on-screen keyboard |
| swipe in from the left / right edge | previous / next workspace |
| long press (hold ~0.6 s, lift) | right-click there (not in browsers, which have their own) |
| 1 finger tap / drag, 2 fingers | the app's own: tap, scroll, zoom |

Directions follow the screen, whichever way it's turned. Single-finger edge swipes are the most reliable. Multi-finger gestures depend on the touch driver reporting 3–4 contacts at once, which hasn't been checked on the device yet.

No gesture has the rotation lock by default (every slot is taken). To put it on one, for example the 4-finger pinch in: `pinch4-in = ipad-rotate lock toggle` in `gestures.conf`.

Changing a gesture: edit `~/.config/linux-a9/gestures.conf` (`name = command`; an empty command turns a gesture off), then `systemctl --user restart ipad-gestures`. To watch what is recognised: `systemctl --user stop ipad-gestures; ipad-gestures --dry-run`.

## Keys (Bluetooth keyboard)

| Keys | Does |
|---|---|
| Super + Space | Omarchy menu |
| Super + Alt + Space | app grid |
| Super + Return | terminal |
| Super + W / Q | close window |
| Super + F | full screen |
| Super + T | float/tile window (Omarchy) |
| Super + 1…9 | workspace |
| Super + L | this workspace's layout (tiling/scrolling/floating) |
| Super + Alt + Y | tablet layout: tiling → tablet → floating |
| Super + Alt + U | next window on this workspace |
| Super + Alt + O | on-screen keyboard |
| Super + Alt + R | rotate by hand |
| Super + Alt + Shift + R | rotation lock on/off (auto-rotate) |
| Super + K | every key binding |
| Power | screen off and sleep; Home wakes. On a blank screen: screen on |

## Auto-rotate and the rotation lock

`ipad-autorotate` (a user unit) turns the screen to the way the iPad is held. It needs an accelerometer, so it starts only where an IIO one exists (`/sys/bus/iio/devices/iio:device*/in_accel_x_raw`, or its buffer channels). On the iPad that is the A9's always-on coprocessor, behind a kernel driver that is still being written; until then the unit shows `inactive (condition no)` and nothing else changes. Turning by hand (Super + Alt + R, the bar's Rotate button, the iPad Screen menu) always works.

```
ipad-rotate lock [on|off|toggle]  # no argument: print locked / unlocked
ipad-tablet rotate-lock           # what the bar button does
ipad-autorotate --list            # which sensors it can see, and a reading
ipad-autorotate --status          # what the running daemon last saw
ipad-autorotate --probe 15        # 15 s dry run, logs every reading and what it would do
journalctl --user -u ipad-autorotate -b
```

How it behaves:

- It follows the sensor: **hold** the iPad another way up for 0.8 s and it runs `ipad-rotate <orientation>`, which turns the screen, the touchscreen (Hyprland's `input:touchdevice:transform`, the same number as the monitor's) and the gestures together, then hides and shows the on-screen keyboard again if it was up so squeekboard lays itself out for the new shape. Flat on a table changes nothing, and so does an orientation the sensor calls `undefined`. At least 2 s pass between two turns.
- It acts on a *change* of the sensor, not on every reading: a manual turn stays until the iPad is physically turned.
- Locked, it keeps watching but doesn't turn. **Unlocking turns to however the iPad is held at that moment**, like iPadOS. The lock is `rotation_lock=on` in `~/.local/state/linux-a9/display`, next to the orientation.
- The bar: the lock button appears while the daemon runs (there is nothing to lock before) and shows the lock state. In portrait (768 px wide) the Layout, Float and Full screen buttons step aside, since the clock and indicators need the room (they have gestures and keys). If an indicator such as Stay Awake is showing, the last buttons can still touch it.

| iPad held with | sensor says | screen | transform |
|---|---|---|---|
| home button at the bottom | `normal` | portrait | 0 |
| home button on the right | `right-up` | landscape | 3 |
| home button at the top | `bottom-up` | portrait-flipped | 2 |
| home button on the left | `left-up` | landscape-flipped | 1 |

Sources: `iio-sensor-proxy` when it reports an accelerometer (the installer adds the package, and the daemon reads its orientation through `monitor-sensor`, which holds the D-Bus claim), otherwise the IIO device straight from sysfs, polled at 4 Hz. The direct path uses the kernel's mount matrix (`in_accel_mount_matrix` or `mount_matrix`) and the IIO convention: an axis pointing up reads +1 g, so flat on a table, screen up, `in_accel_z_raw` × scale is about +9.8, and held upright (home button at the bottom) y is +9.8. With the matrix applied x is the screen's right, y its top, z out of the screen. **What the kernel driver has to provide:** `in_accel_{x,y,z}_raw` with a scale, and the mount matrix that puts the sensor onto the screen's axes. If the axes come out wrong, fix the matrix (or `ACCEL_MOUNT_MATRIX` for iio-sensor-proxy); `autorotate.conf` below is the stopgap without a rebuild.

`~/.config/linux-a9/autorotate.conf` (a commented template is installed; `systemctl --user restart ipad-autorotate` applies):

```
right-up = landscape-flipped   # swap these two if turning right turns the screen the wrong way
left-up = landscape
debounce = 0.8                 # seconds an orientation must hold
min_interval = 2.0             # seconds between two turns
mount_matrix = 1,0,0;0,1,0;0,0,1   # direct IIO only; replaces the kernel's
```

### Without hardware

`ipad-autorotate --mock /tmp/fifo` reads lines from a FIFO or a file (`right-up`, `left-up`, `bottom-up`, `normal`, `undefined`, or `accel X Y Z` for raw gravity, which runs the classifier and its hysteresis); `--mock-iio DIR` reads a fake sysfs tree; `IPAD_AUTOROTATE_MONITOR_CMD` replaces `monitor-sensor`. Three levels of test:

- On the Mac, with no Hyprland: `python3 scripts/omarchy/qemu/test-autorotate.py` (classifier, hysteresis, mount matrix, debounce, lock, manual turn, the proxy path through a fake `monitor-sensor`, the sysfs path, the config).
- In the project's QEMU VM (`OMARCHY_ROOTFS=rootfs-next.img scripts/omarchy/qemu-smoke.sh --disk --hyprland --keep`, never the image the iPad is served from): `OMARCHY_HOT_SSH=scripts/omarchy/qemu/vm-ssh.sh scripts/omarchy/hot-apply.sh`, then `OMARCHY_TEST_SSH=scripts/omarchy/qemu/vm-ssh.sh scripts/omarchy/test-autorotate.sh --shots build/omarchy/qemu/autorotate-shots`. `scripts/omarchy/qemu/vm-shot.sh OUT.png` takes an upright screenshot.
- On the iPad: `scripts/omarchy/test-autorotate.sh` (below).

### Device test plan (orchestrator)

None of this needs the accelerometer until step 4.

1. `scripts/omarchy/hot-apply.sh`. The check ends with an `auto-rotate:` line. Expect `lock unlocked, unit inactive (condition no)`, `HasAccelerometer: False`, `IIO accelerometer: none`. The bar restarts once for the new button code. `systemctl --user --failed` stays empty.
2. `scripts/omarchy/test-autorotate.sh --shots build/omarchy/ipad-autorotate-shots` (mock mode; it stops the real unit, runs its own daemon on a FIFO, and puts the orientation and lock back). All `ok`, `RESULT: all passed`. Each step also checks `hyprctl monitors` and `input:touchdevice:transform`. Look at the screenshots: the bar and the keyboard upright in all four orientations. The daemon's log (`journalctl --user -u ipad-autorotate`, or the `rotated to ... (N s)` lines) gives the real reload time (QEMU: 0.1 s); on the iPad, if the screen takes more than 3 s to settle, raise `debounce` and `min_interval`.
3. Touch alignment by hand, since the fake sensor can't judge it: with the mock daemon running (`ipad-autorotate --mock /tmp/f &`, `echo left-up > /tmp/f`), tap each bar button and swipe from the bottom edge in every orientation; the finger must land where the pointer shows. `touch_transform=N` in the state file overrides the touch rotation if one orientation is off.
4. Once the kernel driver exists: `ipad-autorotate --list` (a reading, and the matrix), then lay the iPad flat, screen up: z should read about +9.8. `ipad-autorotate --probe 15` while turning it shows each reading and what it would do. Start the unit (`systemctl --user start ipad-autorotate`; it's started at login once the sensor is there), then `scripts/omarchy/test-autorotate.sh --hardware` tells you when to turn the iPad and checks the screen, touch and lock each time (`HOLD=15` seconds per position). If a pair of orientations is swapped, swap the two lines in `autorotate.conf`; if all are off, the mount matrix is wrong. If `--list` shows the device but no reading, `cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw`: `Device or resource busy` means iio-sensor-proxy has the buffer open, and the proxy path is what runs; `sudo systemctl stop iio-sensor-proxy` exercises the direct fallback. (`--probe` and `--hardware` stop with a message when there is no sensor at all.)
5. Lock: bar button, Super + Alt + Shift + R and `ipad-rotate lock on` agree; turn the iPad while locked (nothing), unlock (it snaps to how it is held).
6. Not checked anywhere yet: the real sensor, that the `Super + Alt + Shift + R` binding parsed (`hyprctl binds`), `install.sh` re-run with an unchanged plugin (every run so far changed it), how iio-sensor-proxy reports the AOP's accelerometer, the reload time and squeekboard's re-layout on the iPad's own hardware, and whether the portrait bar is crowded by an indicator (the bar and keyboard were checked in QEMU only).

To turn auto-rotate off: `systemctl --user disable --now ipad-autorotate` (the lock button disappears with it).

## Auto-brightness (ambient light)

`ipad-als-brightness` (a user unit) follows the ambient light sensor and moves the backlight through `brightnessctl`. The sensor is the CT819, an IIO light sensor (`in_illuminance_input`, lux) on the kernel branch `a9/sensors`. The unit starts only where such a sensor exists (`/sys/bus/iio/devices/iio:device*/in_illuminance_input`); until then it shows `inactive (condition no)` and nothing else changes. **It is off even then**: the sensor's lux has not been checked against a light meter, so nothing moves the screen until you switch it on (the bar's sun button, the iPad Screen menu, or `ipad-als-brightness on`).

```
ipad-als-brightness on|off|toggle   # the switch (kept as auto_brightness= in ~/.local/state/linux-a9/display)
ipad-als-brightness status          # what the running daemon last saw (lux, smoothed lux, brightness, holds)
ipad-als-brightness --list          # the sensors and backlights found, one reading
ipad-als-brightness --probe 20      # 20 s dry run: logs lux, the smoothed level and what it would set
journalctl --user -u ipad-als-brightness -b
```

How it behaves:

- **Smoothing.** A median of the last five readings, then a filter in log(1 + lux) with a 3 s time constant: a hand over the sensor or a passing shadow doesn't flicker the screen.
- **Curve.** The smoothed level maps to a brightness through `0:10, 5:18, 20:30, 80:50, 250:70, 700:90, 1500:100` (lux:percent, linear in log lux), clamped to 5–100 %. It never turns the screen off.
- **Hysteresis.** It moves only when the level changed by more than 20 %, the brightness would move by at least 2 points, and 2 s have passed since the last move. A move is a short ramp (40 %/s), not a jump.
- **Manual override.** Each tick it compares the backlight with the value it last set. A different value (a brightness key, the menu) pauses it for 30 s; after that it carries on from there.
- **Holds.** It leaves the backlight alone while the screen is dimmed for idle (hypridle runs `ipad-als-brightness dim` / `undim`, which are `brightnessctl -s set 10%` / `-r` plus a flag for the daemon) and while the cover is closed (`ipad-lid`), and for a second after either ends.
- **Sources.** `iio-sensor-proxy`'s `AmbientLightLevel` (read through `monitor-sensor`) when the proxy reports a light sensor, otherwise the IIO device straight from sysfs, read once a second (the sensor powers down after 2 s without a read, and the first read after that waits one 400 ms integration).

`~/.config/linux-a9/als-brightness.conf` (a commented template is installed; `systemctl --user restart ipad-als-brightness` applies): `enabled`, `curve`, `min_pct`, `max_pct`, `lux_scale` (multiply the sensor's lux, to calibrate against a meter), `smooth_secs`, `hysteresis`, `min_pct_step`, `min_interval`, `ramp_pct_per_s`, `override_secs`, `backlight`, `poll_hz`.

## Smart Cover

The cover's hall sensor is an input switch from the kernel (`a9/sensors`): the device `iPad Smart Cover` (PMIC GPIO line 0, `SW_LID`) and `iPad Smart Cover 2` (line 10, `SW_MACHINE_COVER`). It is deliberately **not** called `Lid Switch`, which systemd-logind and Omarchy's Hyprland bindings look for: logind's default would suspend on a cover close, and s2idle resume is still being hardened (`docs/power/s2idle-test-plan.md`). The tablet layer also installs `/etc/systemd/logind.conf.d/90-linux-a9-lid.conf` (`HandleLidSwitch=ignore`) in case a kernel names it that. One thing does change once the switch exists: `omarchy-hw-laptop` answers true for a device with `SW_LID`, so Omarchy's menu offers its laptop entries (battery percentage, laptop display).

`ipad-lid` (a user unit, started only where the switch exists, `ExecCondition=ipad-lid --available`) is the only thing acting on it:

```
ipad-lid --list                  # the switch devices and their state now
ipad-lid --watch 60              # print every switch event: open and close the cover
ipad-lid on|off|toggle           # cover closed -> lock + display off; open -> display on
ipad-lid suspend on|off          # also sleep once the cover has been closed 3 s (opt-in, below)
ipad-lid status
journalctl --user -u ipad-lid -b
```

- **Off by default.** Until `ipad-lid on` (or "Cover Closes Screen On" in the iPad Screen menu) it only watches and logs. The polarity (which level means closed) is a guess until it has been measured with `--watch`; a wrong guess would blank the screen with the cover open.
- **Closed:** it locks the screen **only if the account has a password** (`passwd -S` says `P`): the iPad's user has none today, and a lock that can't be undone would strand the session. `lock = on` in `lid.conf` forces it. Then DPMS off through Hyprland (`hl.dsp.dpms`) and the backlight to 0 (the panel is on simpledrm, where DPMS alone may leave it lit); the old brightness is remembered. `ipad-als-brightness` holds still while the cover is closed.
- **Open:** the backlight goes back, DPMS on. The lock (if there was one) stays until it is unlocked. Opening the cover does not wake a sleeping iPad: the switch is polled (no wakeup).
- **Suspend is opt-in.** `ipad-lid suspend on` makes it run `ipad-sleep` after the cover has been closed for `suspend_delay` (3 s), once per close, after the lock. The default is off because s2idle resume is still being hardened, and the polled switch can't wake the iPad (the wake-capable variant is `docs/power/smart-cover-test-plan.md`).
- **Wrong way round?** The device tree has the polarity (`J71T_COVER0_ACTIVE` in `s8003-j71t-periph.dtsi`, `scripts/sensors/cover-polarity.sh` flips it in a built dtb). `invert = on` in `~/.config/linux-a9/lid.conf` is a stopgap that needs no new kernel. The second hall is shown by `--watch` and `status` but ignored unless `cover2 = closed`.

`~/.config/linux-a9/lid.conf`: `actions`, `suspend`, `suspend_delay`, `lock` (`auto|on|off`), `lock_cmd` (`auto`: `hyprlock` if installed, else `omarchy-system-lock`), `invert`, `cover2`, `device`, `settle`, `backlight`.

### Without hardware

`python3 scripts/omarchy/qemu/test-als-lid.py` runs both daemons on the Mac against fake sysfs trees and fake `brightnessctl`, `hyprctl`, `hyprlock` and `ipad-sleep` (everything above: off by default, hysteresis, spikes, manual override, idle dim and cover holds, the proxy path through a fake `monitor-sensor`, packed `input_event` records, lock only with a password, suspend only when asked, polarity). `scripts/sensors/test-ct819-lux.sh` runs the kernel's own lux function on the host against this unit's calibration and an independent implementation. In the project's QEMU VM (after the hot-apply above), `scripts/omarchy/qemu/test-als-lid-vm.sh [--shots DIR]` drives both daemons with a fake light sensor and backlight and `ipad-lid` with a virtual switch (uinput, `vm-switch.py`) against the real Hyprland (DPMS), the udev rule and the user units. The device test plan, with the hall polarity and light-sensor measurements, is `docs/periph/sensors-test-plan.md`.

## Screen off, sleep and brightness

Idle: the screen dims at 3 min and goes **off** at 5 min (`ipad-screen off`: Hyprland DPMS off and the backlight to 0, since DPMS alone leaves this panel lit). Touch, a key, Home or Power turn it back on at the old brightness. Idle does **not** suspend unless you ask for it: s2idle resume is still being hardened (`docs/power/s2idle-test-plan.md`). Omarchy's Stay Awake toggle stops both.

> Private graphics-dependent excerpt omitted from this source snapshot.

```
ipad-screen [off|on|toggle]        # the screen by hand; no argument: "on" or "blanked"
ipad-sleep idle-suspend [on|off]   # idle also suspends (default off; iPad Screen menu: Sleep When Idle)
journalctl --user -u hypridle -b
```

The brightness keys and the Omarchy brightness slider drive the backlight through Omarchy's own commands. They need the panel to show up as `eDP-1`, which is the case with `apple_adbe_drm` (the default kernel). On the simpledrm fallback the monitor is `Unknown-1` and Omarchy treats it as an external display (DDC), so use `brightnessctl set 50%` there. The level survives reboots and kexecs through `systemd-backlight`. A drop-in (`/etc/systemd/system/systemd-backlight@.service.d/90-linux-a9-floor.conf`, `linux-a9-backlight-floor`) starts at 40 % when the saved level is under 15 %, which would be the idle dim, a blank screen or a closed cover saved at shutdown.

## Layouts and the browser

```
ipad-tablet layout tablet     # one window at a time, full size (like iPadOS)
ipad-tablet layout floating   # Omarchy floating workspaces: title bars, the dock
ipad-tablet layout tiling     # Omarchy's tiling [default]
ipad-tablet browser float     # new browser windows float, centred (tile: back)
```

The choice persists across logins, in `~/.local/state/linux-a9/display`. It applies at once to the workspace on screen and to every workspace without a layout of its own (Super + L gives one a layout of its own, which it keeps). On a floating workspace, Omarchy's dock appears at the bottom and windows get title bars.

**If the on-screen keyboard shows but doesn't type:** Omarchy autostarts fcitx5, which also takes Hyprland's one input-method slot. For the video, turn fcitx5's autostart off and log in again:

```
mkdir -p ~/.config/autostart && printf '[Desktop Entry]\nHidden=true\n' > ~/.config/autostart/org.fcitx.Fcitx5.desktop
```

To undo it, delete that file. `journalctl --user -u linux-a9-osk -b` shows squeekboard's side. On a floating workspace, Omarchy's dock sits at the bottom edge, so open the apps there with four fingers up or the bar button rather than a bottom-edge swipe.

The on-screen keyboard is squeekboard from Arch Linux ARM's repo. `hot-apply.sh` installs it with pacman, so the iPad needs Wi-Fi then. Without it, the keyboard button shows how to install it.

## Text size

The tablet layer sets 14 px, up from Omarchy's 12, once. This covers the bar, menus, GTK and terminals, and the bar's height grows with it. To change it: `omarchy display text size 16` (9–20), or `… reset`.

## Demo tour (proof-of-concept video)

From the Mac: `scripts/omarchy/demo.sh` (all steps, 6 s apart; `DEMO_PAUSE=10` to slow it). Or one step at a time: `status`, `fetch`, `sound`, `browser [URL]`, `close`. On the iPad itself, run `ipad-demo` with the same arguments.

> Private graphics-dependent excerpt omitted from this source snapshot.

Tips for filming: `ipad-demo close` gives a clean desktop between takes. Turn on Stay Awake in the Omarchy menu (Toggle) so the 3-minute dim doesn't hit mid-take.

## Quiet boot

What the image does: `ShowStatus=error` in `/etc/systemd/system.conf.d/90-linux-a9-quiet.conf` (systemd prints failures only, from the next boot), and kernel console level 3 once userspace runs (as before).

The kernel's own boot text comes from the payload's bootargs (`loglevel=7` today). For a quiet take, build the payload with these extra bootargs. They're appended, so they win over the earlier `loglevel=7`:

```
quiet loglevel=3 vt.global_cursor_default=0 systemd.show_status=error rd.udev.log_level=3 udev.log_level=3
```

For example `scripts/boot/m1n1-dev.sh linux quiet loglevel=3 …` or `scripts/mkpayload.sh <initramfs> quiet loglevel=3 …`. Some text is left even then: m1n1's own output, and the initramfs's own lines (USB gadget, NVMe root, charger). The initramfs writes those straight to the console, so loglevel doesn't hide them. Hiding them, or a Plymouth splash, would need an initramfs rebuild. That was deliberately not done the day before the video. With autologin, the screen goes from those few lines to the desktop.

## Where it lives

| What | Source | On the iPad |
|---|---|---|
| actions (buttons + gestures) | `scripts/omarchy/rootfs/files/usr/local/bin/ipad-tablet` | `/usr/local/bin/ipad-tablet` |
| gesture daemon | `…/bin/ipad-gestures`, `etc/systemd/user/ipad-gestures.service` | user unit `ipad-gestures` |
| auto-rotate + lock | `…/bin/ipad-autorotate`, `…/bin/ipad-rotate`, `etc/systemd/user/ipad-autorotate.service` | user unit `ipad-autorotate` (condition: an IIO accelerometer) |
| on-screen keyboard | `…/bin/ipad-osk`, `etc/systemd/user/linux-a9-osk.service` | squeekboard, hidden until asked |
| touch/uinput access | `etc/udev/rules.d/70-linux-a9-tablet.rules` | uaccess + `input` group |
| Hyprland layer | `scripts/omarchy/rootfs/hypr/ipad-tablet.lua` | `~/.config/hypr/ipad-tablet.lua` |
| bar buttons | `scripts/omarchy/rootfs/tablet/plugin/` | `~/.config/omarchy/plugins/linuxa9.tablet` |
| fastfetch | `scripts/omarchy/rootfs/tablet/fastfetch.jsonc` | `~/.config/fastfetch/config.jsonc` |
| demo | `…/bin/ipad-demo`, `usr/local/share/linux-a9/demo/`, `scripts/omarchy/demo.sh` | `ipad-demo` |
| installer (user side) | `scripts/omarchy/rootfs/tablet/install.sh` | run by hot-apply / phase tablet |
| recognizer test (Mac) | `scripts/omarchy/qemu/test-gestures.py`, `scripts/omarchy/qemu/fake-touch.py` | n/a |
| auto-rotate tests | `scripts/omarchy/qemu/test-autorotate.py` (Mac), `scripts/omarchy/test-autorotate.sh` (iPad or VM), `scripts/omarchy/qemu/vm-ssh.sh`, `vm-shot.sh` | n/a |
| screen off / sleep | `…/bin/ipad-screen`, `…/bin/ipad-sleep`, `hypr/hypridle.conf`, `usr/local/sbin/linux-a9-backlight-floor`, `etc/systemd/system/systemd-backlight@.service.d/` | `ipad-screen`, `ipad-sleep`, hypridle; tests `scripts/omarchy/qemu/test-screen-sleep.py` (Mac) |
| auto-brightness | `…/bin/ipad-als-brightness`, `etc/systemd/user/ipad-als-brightness.service`, the button in `tablet/plugin/`, the menu entries in `hypr/omarchy-menu-ipad.jsonc`, `hypr/hypridle.conf` (dim/undim) | user unit `ipad-als-brightness` (condition: an IIO light sensor); off until switched on |
| Smart Cover | `…/bin/ipad-lid`, `etc/systemd/user/ipad-lid.service`, `etc/systemd/logind.conf.d/90-linux-a9-lid.conf`, the switch rule in `70-linux-a9-tablet.rules` | user unit `ipad-lid` (condition: a `SW_LID` input device); no action until `ipad-lid on` |
| auto-brightness and cover tests | `scripts/omarchy/qemu/test-als-lid.py` (Mac), `scripts/omarchy/qemu/test-als-lid-vm.sh` (VM), `scripts/sensors/test-ct819-lux.sh`, `docs/periph/sensors-test-plan.md` | n/a |

To turn it off: remove `require("hypr.ipad-tablet")` from `~/.config/hypr/hyprland.lua`, remove the `linuxa9.tablet` entry from `~/.config/omarchy/shell.json`, and run `systemctl --user disable --now ipad-gestures ipad-autorotate ipad-als-brightness ipad-lid linux-a9-osk`.
