# Rotation: fast IPC, delayed visible recovery

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Investigation: 2026-10-27. **Source verified, device experiments pending.** No
device connection, compositor restart, package rebuild, or commit was made by
the implementation agent. The owner reports a rotated/stretched picture,
then 1–2 seconds of black, on both simpledrm and apple_adbe_drm. That observation
does not by itself identify whether the panel, the complete compositor frame,
or just the wallpaper is black.

## Findings and fix order

> Private graphics-dependent excerpt omitted from this source snapshot.

The old comment in `ipad-rotate` promised no blank because no config reload
was used. That promise was too strong: a successful IPC reply is not a
presentation or client-redraw barrier.

## Reproducible source pins

Run on the Mac, without touching the device:

```sh
bash scripts/omarchy/fetch-rotation-sources.sh
```

This fetches shallow, detached, unmodified snapshots under
`build/omarchy/rotation-src/`, verifies HEAD and a clean working tree, and
does not build or patch them:

| Prefix below | Exact tag | Commit |
|---|---|---|
| `H:` | Hyprland `v0.56.2` | `efb50993780079460b0cbed1363e2166a2de1d9f` |
| `A:` | aquamarine `v0.15.1` | `f31c47a1b9d300847d8fc3108ad959448103dfc1` |

`docs/omarchy/README.md` agrees with `build/omarchy/reports/packages.txt`:
`hyprland 0.56.2-7`, `aquamarine 0.15.1-1.3`, `mesa 1:26.2.5-1`,
`quickshell 0.3.2.r7.g2b4501a-3`. The report also has Qt base/declarative
`6.12.0`. Package revisions can carry distribution changes; the argument
here is specifically about the upstream tags, not an assertion that the
installed ELFs are byte-identical to upstream builds.

For the wallpaper, the inspected local Omarchy checkout is
`5a9ae9ed821f54b155cad6defae3e45769bcd55a`; **check the live files** before
attributing its behavior to the running package. This checkout is newer than
the historical Omarchy commit mentioned in the image README.

> Private graphics section omitted from this source snapshot.

## Transform diff, commit flags, and the real retry paths

`H:src/config/lua/bindings/LuaBindingsConfigRules.cpp:1103–1164` implements
`hl.monitor`. It copies the existing rule with the **same `output` selector**
at **1124–1126**, merges supplied fields, replaces that rule, and schedules
refresh. Therefore this is a supported partial update:

```lua
hl.monitor({ output = "", transform = 0 })
```

Keep `output = ""` to match `ipad.lua`; substituting `eDP-1` creates/updates a
different rule, whose omitted scale can default to auto. If there is already
an output-specific rule, it can also override the catch-all. There is no
writable `hl.get_monitor(...).transform` or dedicated transform setter:
`H:src/config/lua/objects/LuaMonitor.cpp:129–130,184–197` exposes the value and
only workspace-changing methods. Old hyprlang `keyword monitor` examples are
not the Lua API for this image. Output-management clients still route through
monitor rules; they are not a bypass around this logic.

`H:src/config/shared/monitor/MonitorRule.cpp:7–32` compares **rule values**,
not the resolved physical mode. Resolution, refresh, scale, bit depth,
modeline and enabled state are hard fields; transform and position are soft.
`MonitorRuleManager.cpp:174–189` skips a full match, calls
`applyMonitorRuleSoft()` on a soft mismatch, and calls `applyMonitorRule()`
only for a hard mismatch. Soft apply (`H:src/output/Monitor.cpp:652–716`)
updates the transform, logical/transformed dimensions, matrices and damage;
**it does not set an Aquamarine mode/format/enabled property or commit a KMS
modeset**. Layout/layer arrangement follows in `MonitorRuleManager.cpp`.
Ordinary render commits then put rotated pixels in the same **1536×2048**
scanout buffers. Rotation is rendered by the compositor, not a KMS plane
rotation property. The transform update does not wait for QML/app buffers.

Consequences:

- The current repeated `preferred`/`auto`/2 rule should already be soft when
  these hard rule fields match. Sending only transform is conservative, but
  is **not an established fix** for the reported blank.
- Switching from `preferred` (rule resolution `0×0`) to `1536x2048@60` is a
  **hard mismatch once**, even if it resolves to the exact same mode. Later
  rotations using that same exact rule are soft. `0x0` position is soft; do
  not confuse explicit position with a modeset-avoidance requirement.
- Hard apply sets format at `Monitor.cpp:862`, stages/tests modes at
  **878–917**, and commits at **1055**. Aquamarine property setters mark
  dirty bits even for equal values (`A:src/output/Output.cpp:168–170,193–207`);
  `needsReconfig()` at **93–95** includes enabled/format/mode/HDR/WCG, **not
  transform**. `A:src/backend/drm/DRM.cpp:2581–2583,2751–2752` then requests a
  blocking commit with `data.modeset = true` even with unchanged physical mode.
- A modeset-flagged **atomic** request has `ALLOW_MODESET`
  (`A:src/backend/drm/impl/Atomic.cpp:634–640`). This is permission, not proof
  of a panel power cycle: **257–266** compares the actual current mode and
  omits `MODE_ID` if equal; **335–348** also gates mode properties on
  `data.modeset`. It still forces connector properties at **251** and sets
  ACTIVE/plane properties at **305–329**. The legacy implementation instead
  calls DPMS/`drmModeSetCrtc` on `data.modeset` (`impl/Legacy.cpp:39–62`).

The actual fallback paths are distinct:

| Condition | Bound and behavior |
|---|---|
| Optional primary renderer creation fails | One attempt each time `updateSecondaryRendererState` runs; no fixed 5–8 loop, sleep, or deliberate black buffer. Current KMS commit remains accepted. |
| Real non-modeset KMS commit fails | `A:DRM.cpp:2793–2802`: one immediate retry as blocking modeset, unless tainted; failure taints until a success. Failed atomic requests do not install an intermediate frame. |
| Atomic request fails with emitted max_bpc | `A:impl/Atomic.cpp:644–689`: one retry omitting max_bpc, then return. |
| Modeset fails, other heads can be restated | `A:impl/Atomic.cpp:692–711`: at most one all-head retry; a single-head setup has no other head to restate. |
| Hyprland render commit fails and has an in-fence | `H:src/render/Renderer.cpp:2473–2493`: one retry without fences; otherwise roll swapchain back and damage the whole output. |
| Hyprland cannot find any valid mode | `H:src/output/Monitor.cpp:878–968`: test candidates (preferred plus up to three for preferred selection), then alternatives; only total failure schedules **up to 3 retries at 1-second intervals**, so up to 4 full selection rounds (`1084–1111`). Look for `NO FALLBACK MODES`, `REJECTED`, and `retrying in 1s`. This path is not implied by renderer-state errors. |

No path above establishes a 1–2-second blank from the supplied log excerpt.
On a genuine modeset, an old buffer can be carried forward; a missing/wrong
format buffer can be acquired from the swapchain
(`H:Monitor.cpp:2633–2653`), and new-size modesets can acquire a correctly sized
buffer (`A:DRM.cpp:2764–2777`). Such acquisitions need not contain a newly
rendered desktop. That is a possible artifact source on a **hard** change,
not the expected same-mode soft rotation. Do not present it as measured here.

References: [rule comparison](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/config/shared/monitor/MonitorRule.cpp#L7),
[soft apply](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/output/Monitor.cpp#L652),
[atomic commit](https://github.com/hyprwm/aquamarine/blob/f31c47a1b9d300847d8fc3108ad959448103dfc1/src/backend/drm/impl/Atomic.cpp#L626).

## Why a stretched wallpaper can precede black

At the inspected Omarchy commit:

- `shell/plugins/background/Background.qml:369–388` derives decode size from
  the panel's width/height and DPR; **407–414** passes it to `BackgroundMedia`.
- `shell/Ui/BackgroundMedia.qml:38–46` uses an asynchronous Image with
  `sourceSize` bound to that decode size and no `retainWhileLoading` override.
- Qt's `v6.12.0` `src/quick/items/qquickimagebase.cpp:122–130` reloads on
  `setSourceSize`; **333–347** handles loading/retention. The [Qt Image
  contract](https://doc.qt.io/qt-6/qml-qtquick-image.html#retainWhileLoading-prop)
  says retention defaults false; changing sourceSize reloads the image.
  See also [Qt source](https://github.com/qt/qtdeclarative/blob/v6.12.0/src/quick/items/qquickimagebase.cpp#L122).

**Inference:** the first rotated compositor frame can contain the client's
old-size buffer, scaled while its new layer dimensions settle; then the
client clears its image during decode and presents a transparent wallpaper
surface over the compositor background; finally the decoded image is
presented. Both KMS drivers would show this. A bar or terminal surviving the
black interval would strongly support it. If every layer disappears, this
explanation alone is insufficient.

There is another possible client-side gap: `ScreenMoveRemap.qml:24–46` waits
200 ms after an **origin** change, then hides/remaps the layer for 50 ms.
Background visibility uses that guard (`Background.qml:346–352`). A single
output should keep x=y=0 when rotated; record it rather than assuming this
guard fires on a size change. Software drawing can extend the visible gap
beyond a timer's nominal interval. The existing no-op shell.json-write guard
should stay in place; restarting Quickshell on every rotation would confound
all of this.

## Env knobs: what they actually affect

> Private graphics-dependent excerpt omitted from this source snapshot.

Setting any of these on an SSH `hyprctl` process does not change the running
compositor's environment. No changes to `uwsm-env-ipad` are proposed.

## Orchestrator experiments (not executed)

All commands below run **from the Mac**, with device actions delegated through
`scripts/omarchy/ssh.sh` as **linux**. Keep the iPad stationary, unlocked,
with a still wallpaper, a visible bar, no OSK and no fullscreen app. The tests
lock autorotation temporarily. No QEMU/qemu-nbd process is touched. A fresh
`/tmp/rotation-ab` is required; save old results before repeating setup.

### Setup and common restore

Copy only temporary candidates; the installed `ipad-rotate` is not replaced:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'test ! -e /tmp/rotation-ab && mkdir -m 700 /tmp/rotation-ab'
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/ipad-rotate-rotation-ab && chmod 700 /tmp/ipad-rotate-rotation-ab' < scripts/omarchy/rootfs/files/usr/local/bin/ipad-rotate
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-ab/probe.py' < scripts/omarchy/rotation-probe.py
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-ab/retain.patch' < scripts/omarchy/experiments/rotation-retain-wallpaper.patch
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
set -euo pipefail
cat > /tmp/rotation-ab/session.sh <<'SESSION'
set -euo pipefail
test "$(id -un)" = linux
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
mapfile -t rotation_session < <(hyprctl -j instances | python3 -c '
import json, sys
a=json.load(sys.stdin)
if len(a) != 1: raise SystemExit("need exactly one live Hyprland instance")
print(a[0]["instance"]); print(a[0]["wl_socket"]); print(a[0]["pid"])
')
test "${#rotation_session[@]}" = 3
export HYPRLAND_INSTANCE_SIGNATURE=${rotation_session[0]}
export WAYLAND_DISPLAY=${rotation_session[1]}
rotation_hypr_pid=${rotation_session[2]}
test -S "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock"
OMARCHY_PATH=$(systemctl --user show-environment | sed -n 's/^OMARCHY_PATH=//p' | tail -n 1)
export OMARCHY_PATH
SESSION
source /tmp/rotation-ab/session.sh
hyprctl -j monitors > /tmp/rotation-ab/before-monitors.json
ipad-rotate > /tmp/rotation-ab/orientation
ipad-rotate lock > /tmp/rotation-ab/lock
hyprctl -j getoption debug:disable_logs > /tmp/rotation-ab/log-option.json
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
ipad-rotate lock on
pacman -Q hyprland aquamarine mesa quickshell qt6-declarative > /tmp/rotation-ab/packages.txt
tr '\0' '\n' < "/proc/$rotation_hypr_pid/environ" |
  sed -n '/^AQ_/p; /^LIBGL_ALWAYS_SOFTWARE=/p; /^GALLIUM_DRIVER=/p; /^LP_NUM_THREADS=/p; /^QT_QUICK_BACKEND=/p' > /tmp/rotation-ab/session-env.txt
hyprctl layers > /tmp/rotation-ab/before-layers.txt
SH
```

Expected: one monitor, scale 2, physical 1536×2048, x=y=0,
`dpmsStatus=true`; record output name (eDP-1 or Unknown-1) and preferred refresh
rather than assuming them. Verify live wallpaper code in experiment 3.

Common restore after each experiment (also restores the original logging
setting). It deliberately reapplies the ordinary catch-all rule:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab "$(cat /tmp/rotation-ab/orientation)"
case $(cat /tmp/rotation-ab/lock) in
  locked) ipad-rotate lock on ;;
  unlocked) ipad-rotate lock off ;;
esac
rotation_disable_logs=$(python3 -c 'import json; print("true" if json.load(open("/tmp/rotation-ab/log-option.json"))["bool"] else "false")')
hyprctl eval "hl.config({ debug = { disable_logs = $rotation_disable_logs } })"
SH
```

Before a subsequent experiment, set logging false/lock on again using the
two setup commands; keep the saved original files. A failed transform eval
leaves the requested orientation in the state file for next login; run common
restore immediately and inspect stderr, rather than counting it as a pass.

### 1. Legacy versus partial-transform rule, with timing

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
ipad-rotate lock on
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- env IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/legacy.jsonl
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- env IPAD_ROTATE_APPLY=transform /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/transform.jsonl
hyprctl -j monitors > /tmp/rotation-ab/after-monitors.json
SH
```

Expected: both rotate to transform 0; touch follows; no full reload, no
`Applying monitor rule for ...`, `drm: Modesetting ...`, mode rejection or
1-second mode-retry lines for a pure transform. Renderer-state errors may
remain in both. If only the legacy variant modesets, investigate hard fields
or competing rules; retain the partial variant. An unchanged blank is a
useful negative result, not evidence the partial syntax failed.

**Measurement:** the probe starts the action at +1 s, timestamps action start
and return, samples `hyprctl -j monitors` (including IPC latency), tails new
Hyprland logfile bytes, and records Hyprland/Quickshell CPU ticks before/after.
CPU seconds = tick delta / reported `clock_ticks`; IPC latency is **not**
physical blank time. Log `ms` is receipt time (polling delay), not a native
DRM presentation timestamp. If no logfile is present, also retrieve
`hyprctl rollinglog` after the trial; don't infer absence of errors from an
empty tail.

To objectively separate black content from a dark panel, repeat one trial
with bounded screenshots (screenshot capture itself adds CPU/render work):

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 --screenshots /tmp/rotation-ab/legacy-ppm -- env IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/legacy-ppm.jsonl
SH
```

The PPMs are only ~96×128/128×96, not a full-resolution recording. Each has a
capture-start and completion timestamp and mean RGB. Inspect the files,
particularly the bar and a terminal: low mean on dark artwork is not proof
of black. If black samples run from completion times B1 to Bn, report that
span plus the preceding/following sample intervals as uncertainty, not a
precise presentation duration. Screenshot errors/timeouts are missing
observations, not black frames. Use the uninstrumented trial for performance.

For **physical** blank time, film the display at 120/240 fps during the
uninstrumented trial, keeping a bright terminal/bar visible. Count from first
fully dark frame to first recovered frame: blank seconds = frame count /
capture fps, with ±one frame at each endpoint. This is required if grim
shows a desktop while the panel is dark. Neither grim nor frame callbacks
prove the panel is emitting light. Stop after one trial per variant; compare
three pairs only if the effect is inconsistent. **Revert:** common restore.

### 2. Exact fixed mode and position (diagnostic, not a promised optimization)

Only after confirming the live available/preferred mode is 1536x2048@60.
The first apply is a deliberate hard-rule change; exclude it from timing.

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
ipad-rotate lock on
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
hyprctl -j monitors
hyprctl eval 'hl.monitor({ output = "", mode = "1536x2048@60", position = "0x0", scale = 2, transform = 3 }) hl.config({ input = { touchdevice = { transform = 3 } } })'
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- hyprctl eval 'hl.monitor({ output = "", mode = "1536x2048@60", position = "0x0", scale = 2, transform = 0 }) hl.config({ input = { touchdevice = { transform = 0 } } })' > /tmp/rotation-ab/exact.jsonl
SH
```

Expected: initial hard apply, then soft portrait apply, same physical scanout
size and stable position. It should not outperform an already-soft preferred
rule just by spelling out the mode. If an exact rule appears to help, verify
there were no concurrent shell restarts or position shifts. It must not be
sent as 2048x1536; that is the transformed desktop, not a panel mode. The raw
test uses touch=transform; with a saved `touch_transform` override, substitute
that saved value in **both** evals. **Revert:** common restore (preferred/auto,
saved orientation and touch override); no compositor restart.

### 3. Isolate wallpaper decode, then try retention

First use the shell's background-only IPC. It leaves the bar and apps up;
the desktop is intentionally without wallpaper during this control:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
ipad-rotate lock on
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
test -d "$OMARCHY_PATH/shell"
grep -nE 'sourceSize|asynchronous|retainWhileLoading' "$OMARCHY_PATH/shell/Ui/BackgroundMedia.qml"
grep -nE 'decodeWidth|decodeHeight|decodeSize|remapGuard' "$OMARCHY_PATH/shell/plugins/background/Background.qml"
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
timeout 3s omarchy-shell background setSuspended true
trap 'timeout 3s omarchy-shell background setSuspended false || true' EXIT
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- env IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/no-wallpaper.jsonl
timeout 3s omarchy-shell background setSuspended false
trap - EXIT
SH
```

Expected: if the **bar/apps** rotate without black but the wallpaper vanished
in experiment 1, focus on the image/client. This control alone does not time
a black wallpaper (it is hidden intentionally). If all layers still disappear,
check KMS/commit evidence. Revert on interrupted SSH:
`OMARCHY_USER=linux scripts/omarchy/ssh.sh 'source /tmp/rotation-ab/session.sh; timeout 3s omarchy-shell background setSuspended false'`,
then common restore. Skip the control if the live shell lacks this IPC;
do not kill Quickshell to simulate it.

The prepared QML patch is outside the automatically installed Omarchy patch
set. Apply it explicitly, only on an unlocked desktop, after saving all work
that relies on shell UI. This **restarts the shell once**, not Hyprland;
wait for it to be ready before timing rotation. `omarchy-restart-shell` guards
an active lock and launches from the compositor's session environment.

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
rotation_qml=$OMARCHY_PATH/shell/Ui/BackgroundMedia.qml
test -f "$rotation_qml"
patch --dry-run --forward "$rotation_qml" < /tmp/rotation-ab/retain.patch
cp -p "$rotation_qml" /tmp/rotation-ab/BackgroundMedia.qml.before
# sudo writes only this installed QML file if the package owns it.
sudo -n patch --forward "$rotation_qml" < /tmp/rotation-ab/retain.patch
timeout 150s omarchy-restart-shell
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- env IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/retained.jsonl
SH
```

Expected: no cleared wallpaper while decoding; renderer errors may persist.
Remaining rescaling is expected while the previous image covers the new
aspect ratio. Compare CPU deltas and physical video from experiment 1. If
there is no improvement, do not ship the patch just because it applies.
Memory can temporarily rise by the previous decoded image's size (potentially
more than one 12 MiB panel-sized RGBA image due to aspect-cover decoding).

**Revert the patch, restart the shell, then common restore:**

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
rotation_qml=$OMARCHY_PATH/shell/Ui/BackgroundMedia.qml
patch --dry-run --reverse "$rotation_qml" < /tmp/rotation-ab/retain.patch
sudo -n patch --reverse "$rotation_qml" < /tmp/rotation-ab/retain.patch
timeout 150s omarchy-restart-shell
SH
```

If the patched shell fails to load, perform that exact reverse-and-restart
over SSH. If the source changed in the meantime, the dry-run must fail;
inspect it instead of overwriting it with the backup. Neither applying nor
reverting this patch requires restarting sddm or rebuilding a package.

### 4. Last resort: legacy KMS A/B with automatic session restore

> Private graphics-dependent excerpt omitted from this source snapshot.

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
rotation_env=$HOME/.config/uwsm/env.d/90-ipad
test -f "$rotation_env"
! grep -q AQ_NO_ATOMIC "$rotation_env"
test ! -e /tmp/rotation-ab/90-ipad.before
cp -p "$rotation_env" /tmp/rotation-ab/90-ipad.before
cat > /tmp/rotation-ab/restore-env.sh <<'RESTORE'
#!/bin/bash
set -euo pipefail
cp -p /tmp/rotation-ab/90-ipad.before /home/linux/.config/uwsm/env.d/90-ipad
runuser -u linux -- env XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus systemctl --user unset-environment AQ_NO_ATOMIC || true
systemctl restart sddm
RESTORE
chmod 700 /tmp/rotation-ab/restore-env.sh
sudo -n systemd-run --unit=rotation-blank-env-restore --on-active=120s /bin/bash /tmp/rotation-ab/restore-env.sh
printf '\n# temporary rotation A/B\nexport AQ_NO_ATOMIC=1\n' >> "$rotation_env"
sudo -n systemctl restart sddm
SH
```

After the desktop returns (new signature), record environment and one trial:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-ab/session.sh
tr '\0' '\n' < "/proc/$rotation_hypr_pid/environ" | sed -n '/^AQ_NO_ATOMIC=/p'
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
ipad-rotate lock on
IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab landscape
sleep 3
timeout 15s python3 /tmp/rotation-ab/probe.py --seconds 7 -- env IPAD_ROTATE_APPLY=legacy /tmp/ipad-rotate-rotation-ab portrait > /tmp/rotation-ab/no-atomic.jsonl
SH
```

Expected: startup log says legacy DRM, `AQ_NO_ATOMIC=1` in **Hyprland's**
environment, optional primary-renderer errors still occur. Improvement only
here points toward atomic/KMS behavior; failure is possible because the
driver's legacy property support differs. Do not retain it by default.

**Immediate manual restore**, instead of waiting for the timer:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'sudo -n systemctl stop rotation-blank-env-restore.timer; sudo -n /bin/bash /tmp/rotation-ab/restore-env.sh'
```

Then reconnect, source session.sh (new signature), confirm AQ_NO_ATOMIC is
absent from `/proc/$rotation_hypr_pid/environ`, and run common restore. The
automatic restore has the same path. Do not remove `/tmp/rotation-ab` while
its restore timer is armed. No process-name kill is used anywhere.

## If a binary patch is eventually justified

> Private graphics-dependent excerpt omitted from this source snapshot.

Alternative: try the existing GBM-allocator renderer overload when DRM-device
matching fails. That follows Hyprland's own strategy, but creates an extra GL
context and is a larger lifecycle/resource change than avoiding an unused
primary blit renderer. Neither alternative is prepared for packaging here.

**Hyprland:** no transform fast-path patch is needed for the matching-rule
case. If trace proves a hard reapply, fix the rule sender first. Only if a
repro isolates stale compositor buffers after soft rotation should a patch
invalidate transform-dependent resources/damage and keep the old scanout
until a completely rendered replacement is ready. Do not blindly disable
modesets or mark all state changes successful.

## Prepared artifacts and local validation

- `scripts/omarchy/rootfs/files/usr/local/bin/ipad-rotate`: opt-in
  `IPAD_ROTATE_APPLY=transform`; default legacy behavior kept; invalid gate
  rejected before state changes; experimental eval failure has no hidden
  reload. Corrected timing comment and used socket-checked glob discovery
  instead of parsing `ls` output.
- `scripts/omarchy/experiments/rotation-retain-wallpaper.patch`: explicit
  application/reversal only, not added to build or pacman hooks.
- `scripts/omarchy/rotation-probe.py`: bounded user-only observer; no sudo,
  device MMIO, DRM master, compositor stop, or process-name kill.
- `scripts/omarchy/fetch-rotation-sources.sh`: reproducible upstream snapshots.

Validation on the Mac: `bash -n` and ShellCheck 0.11.0 clean for both shell
scripts; Python AST/help checks; QML patch forward/reverse applicability on a
temporary copy of the inspected Omarchy file. Local mocks verified both Lua
payloads (also parsed with Lua), touch override, experimental failure without
reload, legacy fallback, invalid-gate rejection before writes, unchanged bar
mtime, and the observer's action/IPC/PPM JSONL. Device conclusions remain
pending the orchestrator's A/B results.
