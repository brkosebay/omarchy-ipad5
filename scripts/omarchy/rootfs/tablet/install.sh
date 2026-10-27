#!/bin/bash
# linux-a9 tablet layer: the per-user half, run as root on the target. The
# image build runs it in the chroot (phase tablet, rootfs/in-container.sh,
# TABLET_CHROOT=1); hot-apply.sh runs it on the live iPad. The system files
# it relies on (ipad-tablet, ipad-gestures, ipad-osk, ipad-demo, the udev rule,
# the user units, the quiet-boot drop-in) come from rootfs/files/, installed
# by the caller first.
#
#   install.sh USER SRC     SRC = scripts/omarchy/rootfs (hypr/, tablet/) or a copy
#
# Idempotent. Every optional step (a package, the running session) is soft:
# it says what it skipped and goes on.
set -euo pipefail

U=$1
S=$2
CHROOT=${TABLET_CHROOT:-0}
TEXT_SIZE=${TABLET_TEXT_SIZE:-14}
home=$(getent passwd "$U" | cut -d: -f6)
uid=$(id -u "$U")
OMARCHY_PATH=${OMARCHY_PATH:-/usr/share/omarchy}

as_user() { runuser -u "$U" -- "$@"; }
skip() { echo "tablet: skipped: $*"; }

# --- system --------------------------------------------------------------------
# The on-screen keyboard. wvkbd isn't packaged for Arch Linux ARM; ipad-osk
# prefers it if one is ever installed by hand.
if ! pacman -Q squeekboard >/dev/null 2>&1; then
  if pacman -S --needed --noconfirm squeekboard >/dev/null 2>&1; then
    echo "tablet: installed squeekboard"
  else
    skip "squeekboard install (no network?); the keyboard button says how"
  fi
fi
# The accelerometer's D-Bus front end for auto-rotate. Optional: without it
# ipad-autorotate reads the IIO device directly.
if ! pacman -Q iio-sensor-proxy >/dev/null 2>&1; then
  if pacman -S --needed --noconfirm iio-sensor-proxy >/dev/null 2>&1; then
    echo "tablet: installed iio-sensor-proxy"
  else
    skip "iio-sensor-proxy install (no network?); auto-rotate reads the IIO device directly"
  fi
fi
# Fallback access to the touchscreen and uinput (the udev rule's uaccess is
# the main one); takes effect at the next login.
id -nG "$U" | grep -qw input || { usermod -aG input "$U" && echo "tablet: $U added to the input group (next login)"; }
# ipad-als-brightness and ipad-lid start only where the kernel has an IIO light
# sensor / the Smart Cover switch, and then do nothing until switched on.
systemctl --global enable ipad-gestures.service ipad-autorotate.service ipad-als-brightness.service ipad-lid.service \
  linux-a9-osk.service >/dev/null 2>&1 ||
  skip "enabling the user units"

# --- the user's files ------------------------------------------------------------
as_user install -d "$home/.config/hypr" "$home/.config/omarchy/plugins" "$home/.config/fastfetch" "$home/.config/linux-a9"
as_user install -m 0644 "$S/hypr/ipad-tablet.lua" "$home/.config/hypr/ipad-tablet.lua"
grep -q 'require("hypr.ipad-tablet")' "$home/.config/hypr/hyprland.lua" ||
  as_user sh -c "printf '\n-- linux-a9: tablet layer (gestures, layouts, keyboard). Remove to turn it off.\nrequire(\"hypr.ipad-tablet\")\n' >>'$home/.config/hypr/hyprland.lua'"

# The bar buttons: a shell plugin, enabled by being on the bar.
plugin=$home/.config/omarchy/plugins/linuxa9.tablet
as_user install -d "$plugin"
# A changed widget needs the shell restarted: rescanPlugins only picks up new ones.
plugin_changed=0
for f in manifest.json TabletButtons.qml; do
  cmp -s "$S/tablet/plugin/$f" "$plugin/$f" || plugin_changed=1
done
as_user install -m 0644 "$S/tablet/plugin/manifest.json" "$S/tablet/plugin/TabletButtons.qml" "$plugin/"

# shell.json is the whole bar once it exists (no merge with the defaults), so
# a missing one is seeded from Omarchy's defaults before the widget goes in.
shellcfg=$home/.config/omarchy/shell.json
if [[ ! -s $shellcfg ]]; then
  if [[ -s $OMARCHY_PATH/config/omarchy/shell.json ]]; then
    as_user install -m 0644 "$OMARCHY_PATH/config/omarchy/shell.json" "$shellcfg"
    echo "tablet: seeded shell.json from Omarchy's defaults"
  else
    skip "bar buttons: no shell.json and no Omarchy default to seed it from"
  fi
fi
if [[ -s $shellcfg ]]; then
  as_user python3 - "$shellcfg" <<'PY'
import json, sys
p = sys.argv[1]
c = json.load(open(p))
layout = c.setdefault("bar", {}).setdefault("layout", {})
present = any(isinstance(e, dict) and e.get("id") == "linuxa9.tablet"
              for section in layout.values() if isinstance(section, list) for e in section)
if not present:
    left = layout.setdefault("left", [])
    at = next((i + 1 for i, e in enumerate(left) if isinstance(e, dict) and e.get("id") == "omarchy.workspaces"), len(left))
    left.insert(at, {"id": "linuxa9.tablet"})
    open(p, "w").write(json.dumps(c, indent=2) + "\n")
    print("tablet: bar buttons added to shell.json")
PY
fi

# fastfetch (and Omarchy's About): ours unless the user has their own.
ff=$home/.config/fastfetch/config.jsonc
if [[ ! -e $ff ]] || head -1 "$ff" | grep -q '^// linux-a9'; then
  as_user install -m 0644 "$S/tablet/fastfetch.jsonc" "$ff"
else
  skip "fastfetch: $ff is the user's own"
fi

# Gesture overrides: a commented template, never overwritten.
gconf=$home/.config/linux-a9/gestures.conf
[[ -e $gconf ]] || as_user sh -c "cat >'$gconf'" <<'EOF'
# linux-a9 touch gestures (ipad-gestures --help): "name = command" overrides
# one gesture, an empty command turns it off. Restart to apply:
#   systemctl --user restart ipad-gestures
# swipe3-up = ipad-tablet apps
# edge-top =
# longpress =
EOF

# Auto-rotate overrides: a commented template, never overwritten.
aconf=$home/.config/linux-a9/autorotate.conf
[[ -e $aconf ]] || as_user sh -c "cat >'$aconf'" <<'EOF'
# linux-a9 auto-rotate (ipad-autorotate --help): the sensor's orientation ->
# ipad-rotate name, and timing. Restart to apply:
#   systemctl --user restart ipad-autorotate
# If the screen turns the wrong way, swap two of these:
# normal = portrait
# bottom-up = portrait-flipped
# right-up = landscape
# left-up = landscape-flipped
# debounce = 0.8
# min_interval = 2.0
# mount_matrix = 1,0,0;0,1,0;0,0,1
EOF

# Ambient-light auto-brightness: a commented template, never overwritten.
lconf=$home/.config/linux-a9/als-brightness.conf
[[ -e $lconf ]] || as_user sh -c "cat >'$lconf'" <<'EOF'
# linux-a9 ambient-light auto-brightness (ipad-als-brightness --help). It is off
# until switched on (the bar button, the iPad Screen menu, `ipad-als-brightness on`).
# SIGHUP or a restart applies:
#   systemctl --user restart ipad-als-brightness
# enabled = off
# curve = 0:10, 5:18, 20:30, 80:50, 250:70, 700:90, 1500:100   # lux:percent
# min_pct = 5
# max_pct = 100
# lux_scale = 1.0            # multiply the sensor's lux (calibrate against a meter)
# smooth_secs = 3.0
# hysteresis = 0.20
# min_pct_step = 2
# min_interval = 2.0
# ramp_pct_per_s = 40
# override_secs = 30
EOF

# Smart Cover: a commented template, never overwritten.
cconf=$home/.config/linux-a9/lid.conf
[[ -e $cconf ]] || as_user sh -c "cat >'$cconf'" <<'EOF'
# linux-a9 Smart Cover (ipad-lid --help). Nothing happens until switched on
# (the iPad Screen menu, `ipad-lid on`); it never suspends unless
# `ipad-lid suspend on`. Restart to apply:
#   systemctl --user restart ipad-lid
# actions = off
# suspend = off
# suspend_delay = 3.0
# lock = auto               # auto: only when the user has a password
# lock_cmd = auto           # hyprlock, else omarchy-system-lock
# invert = off              # stopgap if the switch reads the wrong way round
# cover2 = ignore           # closed: the second hall also counts
EOF

[[ $CHROOT == 1 ]] && { echo "tablet: installed (the session parts run at first login)"; exit 0; }

# --- the running session ------------------------------------------------------------
udevadm control --reload 2>/dev/null || true
udevadm trigger --subsystem-match=input --subsystem-match=misc 2>/dev/null || true
udevadm settle --timeout=5 2>/dev/null || true

export XDG_RUNTIME_DIR=/run/user/$uid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
session() { as_user env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" OMARCHY_PATH="$OMARCHY_PATH" "$@"; }
if [[ ! -S $XDG_RUNTIME_DIR/bus ]]; then
  skip "session steps: $U isn't logged in (the next login starts everything)"
  exit 0
fi
session systemctl --user daemon-reload || true
session systemctl --user restart ipad-gestures.service || skip "starting ipad-gestures"
# Skipped by its own condition until an accelerometer exists.
session systemctl --user restart ipad-autorotate.service || skip "starting ipad-autorotate"
# Skipped by their own conditions until the kernel has the light sensor / cover switch.
session systemctl --user restart ipad-als-brightness.service || skip "starting ipad-als-brightness"
session systemctl --user restart ipad-lid.service || skip "starting ipad-lid"
if pacman -Q squeekboard >/dev/null 2>&1; then
  session systemctl --user restart linux-a9-osk.service || skip "starting squeekboard"
fi

# Hyprland's and the shell's instances, for hyprctl and omarchy-shell.
sig=$(ls -t "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
if [[ -z $sig ]]; then
  skip "Hyprland isn't running"
  exit 0
fi
hsession() { session env HYPRLAND_INSTANCE_SIGNATURE="$sig" "$@"; }

# Text: 14 px instead of Omarchy's 12 (shell, GTK and terminals together; the
# bar grows with it). Only from the default, so an owner's choice stays.
if hsession omarchy-display-text-size 2>/dev/null | grep -q '^text size: 12[ (]'; then
  hsession omarchy-display-text-size "$TEXT_SIZE" >/dev/null 2>&1 &&
    echo "tablet: text size $TEXT_SIZE px" || skip "text size"
fi

hsession omarchy-shell -q shell rescanPlugins
hsession omarchy-shell -q shell reloadConfig
if [[ $plugin_changed == 1 ]]; then
  hsession omarchy-restart-shell >/dev/null 2>&1 && echo "tablet: bar restarted for the new buttons" || skip "restarting the shell"
fi
hsession hyprctl reload >/dev/null || skip "hyprctl reload"
echo "tablet: installed and live"
