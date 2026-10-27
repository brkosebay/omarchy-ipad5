Staged Smart Cover policy. Not installed by the rootfs builder or hot-apply,

> Historical design/test notes. Current support: [status](../../../../docs/release/STATUS.md); examples may need local firmware/captures.

and only meaningful with `s8003-j71t-smart-cover.dtb` (the PMIC hall event
IRQ kernel). Install after steps R1–D1 of
`docs/power/smart-cover-test-plan.md` pass; step D2 is its acceptance test.

| File | Goes to | What it does |
|---|---|---|
| `90-linux-a9-lid.conf` | `/etc/systemd/logind.conf.d/90-linux-a9-lid.conf` (replaces the `ignore` drop-in) | close → `suspend`, also on external power; inhibitors ignored as on iOS; 5 s hold-off |
| `lid.conf` | `~/.config/linux-a9/lid.conf` | `ipad-lid` keeps lock (only with a password) + display/backlight off on close and restores on open; it never suspends itself |
| `hypridle.conf` | `~/.config/hypr/hypridle.conf` | the shipped idle policy (which now also turns the screen off before every sleep and on after every wake through `ipad-screen`); kept as its own copy for this round |

Keep one suspend owner: logind. `ipad-lid suspend` must stay off (stored
state in `~/.local/state/linux-a9/display` overrides `lid.conf`, so run
`ipad-lid suspend off` and `ipad-lid on` once). Reload logind, never restart
it (a restart can end the session): `sudo systemctl reload systemd-logind`,
then `busctl get-property org.freedesktop.login1 /org/freedesktop/login1
org.freedesktop.login1.Manager HandleLidSwitch` must print `s "suspend"`; if
the reload is refused, the drop-in applies from the next boot. Restart the
user units: `systemctl --user restart hypridle ipad-lid`.

Rollback: put back the saved `ignore` drop-in, `lid.conf` and
`hypridle.conf`, send logind `HUP`, restart the two user units; boot
`s8003-j71t.dtb` for the polled halls without any PMIC event access.
