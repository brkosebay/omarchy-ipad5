# Omarchy polish: brightness, rotation, Smart Cover, screen off: device test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25. Userspace only: **no kernel or rootfs rebuild**. Everything goes on
the running iPad with `scripts/omarchy/hot-apply.sh` (the image build picks the
same files up through `phase_linux_a9`). Nothing here has run on the iPad yet.
Offline: `python3 scripts/omarchy/qemu/test-screen-sleep.py` (25 checks) and
`python3 scripts/omarchy/qemu/test-als-lid.py` (85 checks, 2 new) pass on the
Mac.

## State found, per item

> Private graphics-dependent excerpt omitted from this source snapshot.

Files changed: `scripts/omarchy/rootfs/files/usr/local/bin/{ipad-screen (new), ipad-sleep, ipad-als-brightness}`,
`scripts/omarchy/rootfs/files/usr/local/sbin/linux-a9-backlight-floor` (new),
`scripts/omarchy/rootfs/files/etc/systemd/system/systemd-backlight@.service.d/90-linux-a9-floor.conf` (new),
`scripts/omarchy/rootfs/hypr/{hypridle.conf, ipad.lua (comment), omarchy-menu-ipad.jsonc}`,
`scripts/omarchy/rootfs/smart-cover/{hypridle.conf, README.md}`, `scripts/omarchy/hot-apply.sh`,
`scripts/omarchy/rootfs/in-container.sh`, `scripts/omarchy/qemu/{test-screen-sleep.py (new), test-als-lid.py}`,
`docs/omarchy/tablet-usage.md`.

> Private graphics-dependent excerpt omitted from this source snapshot.

## 0. Before

```sh
scripts/omarchy/ssh.sh 'uname -r; hyprctl monitors | head -1; systemctl is-enabled suspend.target; ls /etc/linux-a9/lab-awake 2>&1; cat /sys/class/backlight/*/brightness /sys/class/backlight/*/max_brightness'
```

> Private graphics-dependent excerpt omitted from this source snapshot.

## 1. Install

```sh
scripts/omarchy/hot-apply.sh
```

PASS: the check's `sleep:` line now shows `idle-suspend off; screen on`, and a
`backlight:` line shows the device, `restore unit active` (or `inactive` with
the reason below) and the saved value. If `suspend.target` was masked, the
install log says `left masked`, and `systemctl is-enabled suspend.target` still
says `masked`. `systemctl --user --failed` is empty, and
`journalctl --user -u hypridle -b` shows no config error. The new 2 s listener
has no `on-timeout`. If this hypridle version rejects that, report the line.

## B. Brightness

**B1 keys and slider.** On `eDP-1`: the Omarchy bar's brightness slider and,
with a Bluetooth keyboard, the brightness keys move
`/sys/class/backlight/*/brightness`. On `Unknown-1`, expect them not to work
(known, documented); `brightnessctl set 30%` must work.

**B2 persistence.** Set 60 % (`brightnessctl set 60%`), warm reboot the usual
way (same default entry), then:

```sh
scripts/omarchy/ssh.sh 'cat /sys/class/backlight/*/brightness; systemctl status "systemd-backlight@*" --no-pager | head -12; ls -l /var/lib/systemd/backlight/; journalctl -b -u "systemd-backlight@*" -o cat'
```

PASS: about 60 % after the boot. If the unit doesn't exist (no udev tag on this
backlight), report it; nothing restores the level then, and that is a finding,
not a failure of this round.

**B3 floor.** `brightnessctl set 5%`, warm reboot. PASS: about 40 % after boot,
and the journal has `linux-a9: backlight ... under 15 %: set to 40 %`.

**B4 auto-brightness (decides the default).** This is
`docs/periph/sensors-test-plan.md` §2. In short: `ipad-als-brightness --probe 20`
while covering and uncovering the sensor (top bezel, next to the camera). Then
watch 60 s with no change of light. PASS = lux falls under about 20 when
covered and comes back, and the value never freezes (A2). If it passes, run
`ipad-als-brightness on`. Cover it, uncover it, then use the slider: the
daemon pauses 30 s and then follows the light again. Report whether the
owner wants it on by default. If yes, the next round sets `enabled = on` in
the installed `als-brightness.conf` template.

## R. Rotation

`ipad-autorotate --list` shows `IIO accelerometer: none` and the unit shows
`inactive (condition no)`. Super+Alt+R and the bar's Rotate button turn the
screen and the touch together. Nothing else is expected until the AOP driver
exists.

## C. Smart Cover (polled halls, default DTB)

This is `docs/periph/sensors-test-plan.md` §3–4. Use a real Smart Cover.

1. `ipad-lid --list`, then `ipad-lid --watch 30` while you close and open the
   cover twice. Record which value means closed for `iPad Smart Cover` (and
   for `... 2`). If closed reads as open, set `invert = on` in
   `~/.config/linux-a9/lid.conf`.
2. `ipad-lid on`. Close: the screen goes dark (backlight 0). Open: back at the
   same level. PASS both twice.
3. New interplay: let the screen blank (`ipad-screen off`), close the cover,
   open it. The screen comes on at about 50 %, not the old level. That is
   expected: `ipad-lid` saw 0 at the close. One extra blink on the next touch
   is expected and harmless too. Then close the cover and press Power: the screen stays dark
   while the cover is closed.
4. Acceptance for turning the cover on by default (next round: `actions = on`
   in the installed `lid.conf` template): polarity right on 10 of 10 closes, no
   spurious event over 10 min with the cover open.

Waking from s2idle by opening the cover needs the IRQ DTB:
`docs/power/smart-cover-test-plan.md`, a separate round. Keep
`ipad-lid suspend` off.

## S. Screen off and sleep

**S1 by hand.**

```sh
scripts/omarchy/ssh.sh 'ipad-screen off; sleep 2; ipad-screen; cat /sys/class/backlight/*/brightness; hyprctl monitors -j | jq ".[].dpmsStatus"'
```

PASS: `blanked`, `0`, `false`, and the panel is black and unlit. Touch the
screen: the desktop comes back at the old level, and `ipad-screen` says `on`.
Repeat with a key on the Bluetooth keyboard and with Home.

**S2 idle.** Leave it alone (Stay Awake off). At 3 min it dims; at 5 min it goes
dark and unlit, and the iPad is **not** suspended:
`cat /sys/power/suspend_stats/success` is unchanged, and SSH still answers.
Touch: back at the pre-dim level.

**S3 Power with `suspend.target` masked** (`sudo systemctl mask suspend.target`
if it isn't already). Press Power: dark, and
`journalctl --user -b | grep ipad-sleep` says `masked`. Press Power again: the
screen comes on. Press Power, wait 5 s, touch: on. PASS: still `masked`
afterwards, and `suspend_stats/success` unchanged. Unmask only if it was
unmasked in step 0.

**S4 lab mode** (`/etc/linux-a9/lab-awake` present, unit active): no idle dim or
blank. Power: dark (the suspend is refused); Power again: on. In lab mode
touch does **not** turn it back on (hypridle is inhibited too). Power does.

**S5 auto-brightness holds** (only if B4 turned it on): `ipad-screen off`, wait
15 s, and the backlight is still `0`. Turn it back on: the daemon follows the
light again within a few seconds.

**S6 Power with sleep available** (only where the orchestrator considers s2idle
safe on the running kernel, see `docs/power/s2idle-test-plan.md`). Power: dark,
then s2idle. Home: wakes, desktop back at the old level. The wake press of
Power does not put it straight back to sleep.

**S7 idle suspend switch** (only with S6 safe): `ipad-sleep idle-suspend on`. S2
then ends in s2idle (`success` +1). Home wakes it. Set it back with
`ipad-sleep idle-suspend off`.

**S8 cost.** `ps -C hypridle -o rss=` is about the same as before (no new
daemon). The 2 s listener spawns `ipad-screen wake` once per pause in input,
which exits at once.

## Rollback and recovery

A dark screen over SSH: `ipad-screen on`. If that does nothing:
`brightnessctl set 50%; hyprctl dispatch 'hl.dsp.dpms({ action = "enable" })'`.
Old idle policy:
`git show HEAD:scripts/omarchy/rootfs/hypr/hypridle.conf > /tmp/h && scripts/omarchy/ssh.sh 'cat > ~/.config/hypr/hypridle.conf; systemctl --user restart hypridle' < /tmp/h`.
Floor off: `sudo rm -r /etc/systemd/system/systemd-backlight@.service.d && sudo systemctl daemon-reload`.

## Results to send back

The monitor name, the `sleep:`/`backlight:` check lines, PASS/FAIL per step
with the journal lines on a failure, the measured cover polarity, the B4 lux
readings, and the owner's call on auto-brightness and cover defaults.
