# Rotation: fast IPC, delayed visible recovery

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Investigation: 2026-10-27. **Round 2: a source-proven false panel-remap trigger
matches the 90°/180° distinction; device validation of its fix is pending.** No device
connection, compositor restart, package rebuild, or commit was made by the
implementation agent. Device results below were supplied by the orchestrator.

At about 18:40, boot `charge8-824a8bda2c` on simpledrm reproduced the same
symptom previously seen with ADBE `eDP-1` on release2:

- `retainWhileLoading: true` plus a shell restart made **no difference**;
  the patch was reverted and the shell restarted back to stock.
- Suspending the wallpaper also made **no difference**. The bar disappears
  too: the owner sees the **whole screen black**, not just the wallpaper.
- Quarter-turns blank; portrait → portrait-flipped does not. This implicates
  the logical dimension swap (768×1024 ↔ 1024×768), not every transform change.
- These tests used `ipad-rotate`'s default legacy sender. Repeated optional
  Aquamarine renderer errors remain; no actual commit-failure trace or
  timestamped frame capture has yet been supplied.

**Key new finding:** Quickshell's x/y/width/height all share the same notify
signal. Omarchy interprets an x/y notification as an origin movement without
comparing the coordinates. A width/height swap therefore schedules **all
`ScreenMoveRemap` users to hide and recreate their native windows**: bar,
wallpaper and dock. This is independent of wallpaper decoding. A 180° turn
keeps dimensions, so it avoids that trigger. The nominal sequence is stretch,
200 ms settle, unmap, 50 ms remap delay, then expensive native-window/scene
recreation on the two-core software renderer. The actual 1–2 s recovery cost
and whether anything else contributes still need the device A/B.

The pinned sources also rule out two tempting explanations for a **soft** rotation:
scanout buffers do not resize, and the compositor does not wait for all layer
clients to acknowledge a resize before rendering. Existing client buffers can
be stretched immediately. A universal 1–2 s output-resize blackout timer was
not found. Client buffer replacement/remapping, damage/render correctness,
and physical scanout remain separate possibilities.

**Current priority:** run the [three Round 2 experiments](#round-2-experiments):
bright compositor-background control, then the optional
`rotation-origin-remap.patch` which compares actual coordinates before
remapping. If needed, use held-client/full-redraw controls afterward.
`rotation-control-ab.py` supplies those fallback diagnostic profiles with
restoration and a separate watchdog. The wallpaper retention patch is not a
current fix candidate.

## Round 1 hypotheses and fix order (historical, superseded above)

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
| `Q:` | quickshell `v0.3.2` | `4f508be500dea6e5732cc3d50382a0048b17e7b1` |

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

## Round 1 wallpaper hypothesis (rejected as the cause by Round 2)

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

**Round 2 correction:** unchanged numeric x/y do **not** mean these handlers
stay silent. Quickshell shares their notify signal with width/height; see the
source proof below. This invalidates the earlier assumption that observing
x=y=0 alone excludes the remap guard.

## Env knobs: what they actually affect

> Private graphics-dependent excerpt omitted from this source snapshot.

Setting any of these on an SSH `hyprctl` process does not change the running
compositor's environment. No changes to `uwsm-env-ipad` are proposed.

## Round 1 orchestrator experiments (historical)

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
pending the orchestrator's A/B results. See the current Round 2 evidence and
experiments below; the original wallpaper experiments were subsequently run
and were negative.

## Round 2 source trace: what changes, what keeps rendering

All `H:`/`A:`/`Q:` references use the exact pins above. The installed Quickshell
report is `0.3.2.r7.g2b4501a-3`, **seven commits beyond the requested 0.3.2
tag**, so a result requiring a Quickshell fix must also check that package's
delta. `fetch-rotation-sources.sh` now fetches/validates all three snapshots.

### The concrete false-remap chain

1. `Q:src/core/qmlscreen.hpp:36–39` declares x, y, width and height with the
   **same** `NOTIFY geometryChanged`; `qmlscreen.cpp:16` forwards
   `QScreen::geometryChanged` to that signal. These declarations were also
   verified in the image's `2b4501a` revision via its upstream raw source.
2. Qt 6.12 resolves `onXChanged`/`onYChanged` to the property's notify signal
   (`src/qml/qml/qqmlproperty.cpp:2256–2263`), even when the underlying signal
   has a different name. `Connections` binds to that resolved signal
   (`src/qmlmeta/types/qqmlconnections.cpp:412–420`). A notification is not a
   numeric-equality test.
3. Omarchy `shell/Ui/ScreenMoveRemap.qml:38–41` unconditionally restarts its
   settle timer from both handlers. **24–35** sets `remapping=true` after
   200 ms, then false 50 ms later.
4. `shell/plugins/bar/Bar.qml:1544–1549`,
   `plugins/background/Background.qml:346–352`, and
   `plugins/dock/DockWindow.qml` use the guard in window visibility.
   `Q:src/wayland/wlr_layershell/wlr_layershell.cpp:108–114` returns
   `deleteOnInvisible=true`; `Q:src/window/proxywindow.cpp:296–307` deletes
   the native window when hidden and creates it again when shown.

This is a specific causal source path, not a generic slow-resize theory.
Suppressing the background leaves the bar/dock on it. Bar.qml's own
**1534–1538** comment records why normal bar hiding parks the surface instead
of unmapping: rebuilding scene graph, glyphs and textures is expensive.
The false remap bypasses that optimization. Its comment's ~150 ms is not an
iPad measurement; do not substitute it for the owner's 1–2 seconds.

The candidate preserves real origin-movement remaps and their timing. It
initializes a screen/coordinate snapshot, ignores shared notifications whose
numeric x/y are unchanged, and restarts the timer only when either coordinate
actually changes. The patch is explicit opt-in and outside automatic patch
hooks. It does not suppress resize/configure events or redraws.

References: [Quickshell shared notification](https://github.com/quickshell-mirror/quickshell/blob/4f508be500dea6e5732cc3d50382a0048b17e7b1/src/core/qmlscreen.hpp#L36),
[the image revision's same declarations](https://github.com/quickshell-mirror/quickshell/blob/2b4501a/src/core/qmlscreen.hpp#L36),
[Qt notify resolution](https://github.com/qt/qtdeclarative/blob/v6.12.0/src/qml/qml/qqmlproperty.cpp#L2256).

| Path | Verified behavior at these pins | Implication |
|---|---|---|
| `H:src/output/Monitor.cpp:652–716` | Soft apply swaps `m_size`/`m_transformedSize`, updates the matrix, resizes the damage ring. | 90° changes logical geometry; 180° does not swap dimensions. |
| `H:src/output/Monitor.cpp:2677–2697,2787–2797`; `src/output/MonitorResources.cpp:14–25` | Scanout swapchain uses `MODE->pixelSize`; monitor GL resources use `m_pixelSize`. Both remain 1536×2048 at fixed mode. | No necessary KMS/GL buffer reallocation from logical rotation alone. Fresh work buffers can still be allocated from the pool when needed, including expiration after 5 s (`MonitorResources.cpp:42–57`); inspect `Framebuffer ... created`/`Swapchain ... Reconfigured` lines. |
| `H:src/output/DamageRing.cpp:5–12,48–68`; `A:src/allocator/Swapchain.cpp:62–71` | A dimension swap damages the whole new logical extent. Normal rendering rotates a three-buffer ring; Aquamarine reports age = swapchain length. | 90° is a full-damage event; 180° retains the ring size, although rule/layer refresh still damages the monitor. Old history is not explicitly cleared in `setSize`. A damage-control A/B is warranted, not proof this code is broken. |
| `H:src/render/Renderer.cpp:2587–2701` | Layer geometry changes immediately; configure is sent if size differs, and `setBox()` follows without waiting. Arrangement damages the output and invalidates tiled-window layout. | The old layer texture can be drawn into the new box, explaining the initial stretch. |
| `H:src/protocols/LayerShell.cpp:132–144,189–196` | Configure tracks a serial; ack updates that surface's size. There is no all-client completion barrier here. | A slow ack is not a compositor-wide “wait until every layer is ready” instruction. |
| `H:src/render/Renderer.cpp:935–990`; `src/render/pass/SurfacePassElement.cpp:20–80` | Rendering uses the current mapped surface texture, with the new layout rectangle. | Existing buffers remain eligible before the new client buffer arrives. A null buffer can unmap the individual layer (`LayerShell.cpp:67–79`), not automatically every output layer. |
| `H:src/desktop/view/Window.cpp:1514–1528`; `src/protocols/types/SurfaceStateQueue.cpp:74–85` | Window acks record pending size. Fence/FIFO/timer locks queue **one surface's** states; old current state remains. | No global resize transaction clearing the whole screen was found. Slow client rendering can still matter, especially when most visible surfaces belong to one Quickshell process. |
| `Q:src/wayland/wlr_layershell/surface.cpp:185–208` | Quickshell acks configure immediately. On an existing surface it asks Qt to `applyConfigureWhenPossible()`, later `resizeFromApplyConfigure()`. `configured` remains true. | A resize can defer the client's redraw, but this function does not intentionally unmap an already configured window for 1–2 seconds. |
| `Q:src/wayland/wlr_layershell/wlr_layershell.cpp:108–114`; `Q:src/window/proxywindow.cpp:296–307` | Making a panel invisible deletes its native window; showing it creates another. | Hiding/re-showing layers is costly and can introduce exactly the missing-buffer interval being investigated. Do not ship that as a workaround. |
| `Q:src/window/proxywindow.cpp:492–501` | Disabling updates drops expose/update requests and may leave no valid buffer until recovery. | `updatesEnabled=false` across rotation is not a reliable old-frame retention method. The experiment uses SIGSTOP with retained existing buffers instead. |

See [Hyprland layer arrangement](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/render/Renderer.cpp#L2587),
[monitor resource sizing](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/output/Monitor.cpp#L2787),
and [Quickshell configure handling](https://github.com/quickshell-mirror/quickshell/blob/4f508be500dea6e5732cc3d50382a0048b17e7b1/src/wayland/wlr_layershell/surface.cpp#L185).

Black itself is not yet proven to be a **submitted black compositor frame**:
an eyewitness dark panel cannot distinguish that from scanout failing to
display a valid frame. If clients unmap or draw transparent content, Omarchy's
normal compositor background is black (`default/hypr/looknfeel.lua:107–109`),
so a client-only gap can look like total failure when all visible UI belongs
to Quickshell. A bright native compositor background disambiguates this.
`H:Renderer.cpp:1110–1113` can suppress normal workspace drawing for a session
lock, and **2219–2224** draws a DPMS black overlay. Neither is activated by
`applyMonitorRuleSoft`. Check unexpected lock/DPMS events if the bright
background also disappears.

The retry analysis above is unchanged: the supplied `on applyCommit` renderer
errors follow accepted commits. They establish neither failed KMS submission
nor pixel contents. Real render/commit failures should be sought separately.
Several relevant failure lines are TRACE-only (`Renderer.cpp:2473–2493`),
so an ordinary log's silence does not prove they never happened.

Also correct a Round 1 instrumentation assumption: `hl.config` setting
`debug.disable_logs`/`disable_time` alone does not reconfigure the logger in
this tag. `src/debug/log/Logger.cpp:36–50` listens to `config.reloaded`, while
dynamic refresh emits `config.props_refreshed`. The experiments below use the
probe's monotonic timestamps and do not claim to enable native log timestamps
without a reload. Existing enabled logs can still be collected.

## Round 2 ranked mitigations

1. **Fix the false origin movement detection first:** explicitly A/B
   `scripts/omarchy/experiments/rotation-origin-remap.patch`. This targets the
   proven unnecessary hide/delete/recreate path and needs no binary rebuild.
   The gate is explicit forward/reverse patch application plus **one shell
   restart per variant**, never per rotation. Retain the old API semantics
   for real monitor movement. Device confirmation is still required.
2. **If full-damage control helps after that:** retain VFR and use a short full-damage
   interval around quarter-turns. `rotation-control-ab.py damage -- ...` is
   the prepared opt-in candidate. It sets `debug.damage_tracking=0` for three
   seconds and restores the exact prior value. If only `burst` helps, it also
   temporarily sets `debug.vfr=false`. The latter costs CPU and memory
   bandwidth; do not set it permanently on this iPad. Both are hypotheses
   until tested. The wrapper's completion includes the three-second hold;
   its separate `rotation_return` event records the actual IPC/script return.
3. **If holding Quickshell shifts/removes a residual blank:** keep investigating the
   one client/process and its layer lifecycle rather than adding KMS knobs.
   Reduce visible overlays and expensive widget content during resize; check
   layer disappear/reappear events and `ScreenMoveRemap` origin changes.
   The wallpaper decode is already ruled out. SIGSTOP is a diagnostic, not a
   shipping rotation policy: shell controls/notifications pause while held.
4. **Use the partial transform sender to preserve existing hard fields**, but
   it reaches the same soft path as a matching legacy rule. Explicit mode/
   position spelling does not freeze the logical layout. No stock option was
   found to preserve a complete output snapshot until all resize clients
   finish. The `makeSnapshotFB` paths (`H:Renderer.cpp:3087–3165`) serve view
   fadeouts; they are not a configurable output-resize barrier.
5. **Do not equate removing stretch with fixing black.**
   `render.expand_undersized_textures` adjusts UV/edge expansion
   (`H:src/render/ElementRenderer.cpp:53–109`); it does not retain an output
   frame or wait for a redraw. Hiding/re-showing panels and pre-sizing them
   to the other orientation add protocol/buffer churn. `AQ_NO_ATOMIC` still
   cannot turn off the optional primary-renderer probe.
6. **Only after a compositor-side reproduction:** patch the narrowed path.
   If full-damage/held-client trials isolate stale render state, start with
   transform-dependent damage history invalidation, whole damage on all
   scanout buffers, and full repaint on an unchanged physical swapchain.
   If captures are valid while the panel is dark, examine KMS/flush/sync
   instead. A separate synchronization concern is that
   `H:GLRenderer.cpp:111–120` only calls `glFinish()` for `isSoftware()` or
   selected NVIDIA cases; `Renderer.cpp:96–130` derives `m_software` from the
   **DRM driver name**, not the EGL renderer string. A simpledrm/ADBE card
   running llvmpipe need not be recognized. That is source evidence for a
   possible flush/sync A/B patch, not proof it causes this symptom and not a
   config knob we can safely enable on the running compositor.

## Round 2 experiments

These are the **three current experiments**, not executed by this agent.
R2.2 explicitly changes one installed QML file and restarts the shell once
per variant; it never restarts the compositor. The other tests require no
restart/root access. No QEMU/NBD operation is involved. Keep the device
stationary and unlocked. Begin with
the background visible (if the earlier test left it suspended, restore it
with `omarchy-shell background setSuspended false`). Use a fresh output
directory; don't overwrite old trial results. Do not change the controlled
debug options concurrently. The watchdog restores the burst/held-shell
profile after six seconds even if its wrapper is killed.

### Shared setup (Mac commands; every device command is linux over ssh.sh)

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'test ! -e /tmp/rotation-r2 && mkdir -m 700 /tmp/rotation-r2'
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-r2/probe.py' < scripts/omarchy/rotation-probe.py
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-r2/control.py' < scripts/omarchy/rotation-control-ab.py
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-r2/origin.patch' < scripts/omarchy/experiments/rotation-origin-remap.patch
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'cat > /tmp/rotation-r2/ipad-rotate && chmod 700 /tmp/rotation-r2/ipad-rotate' < scripts/omarchy/rootfs/files/usr/local/bin/ipad-rotate
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
set -euo pipefail
cat > /tmp/rotation-r2/session.sh <<'SESSION'
set -euo pipefail
test "$(id -un)" = linux
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
mapfile -t rotation_session < <(hyprctl -j instances | python3 -c '
import json,sys
a=json.load(sys.stdin)
if len(a)!=1: raise SystemExit("need one live Hyprland instance")
print(a[0]["instance"]); print(a[0]["wl_socket"])
')
test "${#rotation_session[@]}" = 2
export HYPRLAND_INSTANCE_SIGNATURE=${rotation_session[0]}
export WAYLAND_DISPLAY=${rotation_session[1]}
test -S "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock"
OMARCHY_PATH=$(systemctl --user show-environment | sed -n 's/^OMARCHY_PATH=//p' | tail -n 1)
export OMARCHY_PATH
export IPAD_ROTATE_APPLY=legacy
SESSION
source /tmp/rotation-r2/session.sh
hyprctl -j workspaces | python3 -c 'import json,sys; assert all(w["name"]!="rotation-r2" for w in json.load(sys.stdin)), "test workspace already exists"'
/tmp/rotation-r2/ipad-rotate > /tmp/rotation-r2/orientation
/tmp/rotation-r2/ipad-rotate lock > /tmp/rotation-r2/lock
cat > /tmp/rotation-r2/restore.sh <<'RESTORE'
#!/bin/bash
source /tmp/rotation-r2/session.sh
if [[ -f /tmp/rotation-r2/lease.json ]]; then
  python3 /tmp/rotation-r2/control.py --restore-file /tmp/rotation-r2/lease.json
fi
timeout 3s omarchy-shell background setSuspended false
hyprctl eval 'if rotation_r2 then hl.config({misc = {background_color = rotation_r2.background, disable_hyprland_logo = rotation_r2.logo, disable_splash_rendering = rotation_r2.splash}}); hl.dispatch(hl.focus({workspace = rotation_r2.workspace})); rotation_r2 = nil end'
/tmp/rotation-r2/ipad-rotate "$(cat /tmp/rotation-r2/orientation)"
case $(cat /tmp/rotation-r2/lock) in
  locked) /tmp/rotation-r2/ipad-rotate lock on ;;
  unlocked) /tmp/rotation-r2/ipad-rotate lock off ;;
esac
RESTORE
/tmp/rotation-r2/ipad-rotate lock on
hyprctl eval 'assert(rotation_r2 == nil); rotation_r2 = { background = hl.get_config("misc.background_color"), logo = hl.get_config("misc.disable_hyprland_logo"), splash = hl.get_config("misc.disable_splash_rendering"), workspace = hl.get_active_workspace().config_name }; hl.config({misc = {background_color = "rgb(104080)", disable_hyprland_logo = true, disable_splash_rendering = true}}); hl.dispatch(hl.focus({workspace = "name:rotation-r2"}))'
timeout 3s omarchy-shell background setSuspended true
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
hyprctl -j monitors > /tmp/rotation-r2/before-monitors.json
hyprctl -j layers > /tmp/rotation-r2/before-layers.json
SH
```

Expected before timing: empty test workspace, bar visible, **blue desktop**,
one output, scale 2, panel 1536×2048, transform 0, x=y=0, DPMS on. If the
desktop is not blue, stop and run restore; don't use a black-background trial
to conclude the compositor frame went black. The test workspace removes
tiled-app resizing as a confounder without closing any application.

Common revert after finishing the experiment group, or on interruption:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash /tmp/rotation-r2/restore.sh'
```

It restores any active profile first, then wallpaper, original color/logo/
splash settings, original workspace, orientation and lock. The restore script
is created before the setup changes any of these. Do not remove
/tmp/rotation-r2 until profile restoration is
confirmed. Temporary workspaces disappear when empty and switched away.
This common restore does not reverse an applied QML patch; use R2.2's explicit
reverse-and-shell-restart block for that variant.

### R2.1 — Is the black in compositor content, and does it require clients?

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-r2/session.sh
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers --screenshots /tmp/rotation-r2/quarter-ppm -- /tmp/rotation-r2/ipad-rotate landscape > /tmp/rotation-r2/quarter.jsonl
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers --screenshots /tmp/rotation-r2/half-ppm -- /tmp/rotation-r2/ipad-rotate portrait-flipped > /tmp/rotation-r2/half.jsonl
SH
```

Expected distinctions (film the panel too):

- **Blue remains, bar vanishes:** a client/layer gap, not total compositor
  black. Layer entries/alpha changing or PIDs restarting narrow it further.
- **Blue and bar are black in PPMs:** the compositor/capture rendering path
  actually produced black content; normal transparent/unmapped clients cannot
  explain the blue disappearing. Check unexpected lock/DPMS or an opaque
  covering layer before attributing it to KMS.
- **PPMs show blue/bar while panel is black:** presentation/scanout/sync path,
  downstream from captured content. Software-render timing can still matter.
- **No blank on the empty blue workspace:** bring an app back in a later
  trial; window resize or a layer/content combination is required.

The probe reports `center_rgb`, fraction of pixels with all channels ≤8,
PPM capture start/end, layer PID/geometry/alpha and monitor/IPC times. Inspect
images; an object can cover the center. Bound visible black duration using
first/last black sample times plus neighboring sample gaps. If grim times
out, that is missing data, not a black frame. A 120 fps camera gives physical
duration = dark frame count / 120, ±one frame per endpoint. Screencopy can
force repaint/mirror buffers and hide a bug; repeat without `--screenshots`
only if the capture changes the symptom. Native log receipt times remain
useful, but are not presentation timestamps. Revert: common restore, or
continue directly to R2.2 with the blue background left in place. Watch for
layer disappearance/new addresses about 200 ms after the 90° apply while
their owner PID remains unchanged; ordinary resize should preserve a mapped
layer's identity. The 200 ms poll can miss a short unmap, so absence of a
missing-layer sample alone is not conclusive.

The control wrapper's events are JSON lines inside `action_end.stdout`.
Align an event to the observer's clock with
`(control_event.monotonic_ns - start.start_monotonic_ns) / 1e6` milliseconds;
this locates SIGCONT/restore independently of when the outer command returns.

### R2.2 — A/B the actual-origin comparison (primary candidate)

First inspect the live file and verify it has the same unconditional handlers.
Only apply on an unlocked desktop. The file patch and shell restart are
explicitly authorized by this experiment; no package rebuild is involved.

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-r2/session.sh
rotation_qml=$OMARCHY_PATH/shell/Ui/ScreenMoveRemap.qml
test -f "$rotation_qml"
grep -nE 'onXChanged|onYChanged|checkOrigin|settleTimer' "$rotation_qml"
patch --dry-run --forward "$rotation_qml" < /tmp/rotation-r2/origin.patch
cp -p "$rotation_qml" /tmp/rotation-r2/ScreenMoveRemap.qml.before
sudo -n patch --forward "$rotation_qml" < /tmp/rotation-r2/origin.patch
timeout 150s omarchy-restart-shell
# Shell restart resets its background suspension; restore the blue control.
timeout 3s omarchy-shell background setSuspended true
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers --screenshots /tmp/rotation-r2/origin-fixed-ppm -- /tmp/rotation-r2/ipad-rotate landscape > /tmp/rotation-r2/origin-fixed.jsonl
SH
```

**Expected:** new client-sized buffers/layout can still settle and stretch,
but no guard-induced hide/recreate after the size-only change. Compare the
bar layer identity/gap and physical black interval with R2.1. Optional EGL
errors can remain. A disappearing blank while those errors remain separates
the false remap from renderer-init noise. If the fix holds on the blue control,
unsuspend the real wallpaper and repeat one quarter-turn to verify the
original desktop; the wallpaper's own decode is otherwise left unchanged.

**Revert**, including recovery if the patched shell fails to load:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-r2/session.sh
rotation_qml=$OMARCHY_PATH/shell/Ui/ScreenMoveRemap.qml
patch --dry-run --reverse "$rotation_qml" < /tmp/rotation-r2/origin.patch
sudo -n patch --reverse "$rotation_qml" < /tmp/rotation-r2/origin.patch
timeout 150s omarchy-restart-shell
bash /tmp/rotation-r2/restore.sh
SH
```

Do not overwrite a source changed by another update if the reverse dry-run
fails. No compositor restart is necessary. To confirm causality rather than
random improvement, one reverted stock quarter-turn should bring the symptom
back; use the ordinary restored desktop. Do not repeat the old wallpaper
retention patch alongside this candidate.

### R2.3 — Fallback controls only if the primary fix is negative/incomplete

After the patch is reverted, recreate the blue control if common restore
already ran. The following keeps existing original-state backups and avoids
overwriting an active Lua snapshot. First hold Omarchy's existing buffers:

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-r2/session.sh
/tmp/rotation-r2/ipad-rotate lock on
hyprctl eval 'if rotation_r2 == nil then rotation_r2 = { background = hl.get_config("misc.background_color"), logo = hl.get_config("misc.disable_hyprland_logo"), splash = hl.get_config("misc.disable_splash_rendering"), workspace = hl.get_active_workspace().config_name } end; hl.config({misc = {background_color = "rgb(104080)", disable_hyprland_logo = true, disable_splash_rendering = true}}); hl.dispatch(hl.focus({workspace = "name:rotation-r2"}))'
timeout 3s omarchy-shell background setSuspended true
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers -- /usr/bin/python3 /tmp/rotation-r2/control.py --lease /tmp/rotation-r2/lease.json hold-shell -- /tmp/rotation-r2/ipad-rotate landscape > /tmp/rotation-r2/held-shell.jsonl
SH
```

This verifies an unlocked session, selects exactly one PID from `omarchy-*`
layer ownership, verifies uid/comm/start time, sends that PID SIGSTOP, runs
the rotation, and SIGCONTs it after three seconds. Existing surfaces/buffers
are retained; no panel is hidden/recreated. A separate session watchdog
resumes it after six seconds even on wrapper SIGKILL/SSH loss. No process-name
kill or compositor signal is used. Shell input/notifications pause briefly;
do not press lock/suspend or interact with shell controls during the hold.

Expected: old bar geometry may be stretched/clipped while held. If blank
begins only at `profile_restored`/SIGCONT, client resize/commit activity is
strongly implicated. If blue/bar go black while the client is still stopped,
waiting for its ack/new buffer cannot be a necessary trigger. A performance
improvement can also reflect less CPU contention; it alone does not prove a
particular Qt bug. Watch physical video; add screenshots only for a second
run if R2.1 showed capture was non-disruptive. Revert: automatic profile
restore; immediate manual recovery is common restore. An unfamiliar layer
layout or missing unlock helper makes the script refuse the test safely.

#### Optional repaint variant within R2.3

```sh
OMARCHY_USER=linux scripts/omarchy/ssh.sh 'bash -s' <<'SH'
source /tmp/rotation-r2/session.sh
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers -- python3 /tmp/rotation-r2/control.py --lease /tmp/rotation-r2/lease.json damage -- /tmp/rotation-r2/ipad-rotate landscape > /tmp/rotation-r2/damage.jsonl
/tmp/rotation-r2/ipad-rotate portrait
sleep 3
timeout 15s python3 /tmp/rotation-r2/probe.py --seconds 7 --layers -- python3 /tmp/rotation-r2/control.py --lease /tmp/rotation-r2/lease.json burst -- /tmp/rotation-r2/ipad-rotate landscape > /tmp/rotation-r2/burst.jsonl
SH
```

Compare the physical blank to R2.1 and CPU tick deltas. **Damage-only helps:**
buffer-age/partial-damage correctness is implicated. **Only burst helps:**
frame scheduling or delayed refresh deserves attention; extra rendering can
also conceal client latency. **Neither helps, frozen client still blanks:**
the config workaround is negative; prioritize captured pixels versus panel
light and compositor/KMS synchronization. All profiles may still print the
optional Aquamarine EGL errors.

Both profiles restore original options automatically after three seconds,
with a six-second backup watchdog. The setting is not written to config
files. Revert: automatic profile restoration, then common restore. If a
hyprctl restore fails, the lease stays on disk and the independent watchdog
retries once; inspect `lease.watchdog.log` and use the explicit common restore
over SSH. Do not retain a permanent `debug.vfr=false` setting.

## Round 2 prepared changes and validation

- `experiments/rotation-origin-remap.patch`: primary opt-in candidate; not
  included in image/pacman patch hooks. Actual-origin comparison preserves
  movement remaps while suppressing the false size-only trigger.
- `rotation-control-ab.py`: fallback damage/burst/held-shell diagnostics;
  no default config changes. Uses exact owned/verified PIDs only, with a
  start-time check before resuming a held shell and an independent watchdog.
- `rotation-probe.py`: optional layer PID/geometry/alpha sampling, shared
  monotonic clock anchor, screenshot center RGB and black-pixel fraction.
- `fetch-rotation-sources.sh`: also pins Quickshell 0.3.2; upstream checkouts
  remain unmodified.

Mac-only validation: shell syntax and ShellCheck clean; all documented shell
blocks parse; setup Lua executed with API stubs; optional QML patch applies
and reverses to identical original bytes on a temporary copy. Its extracted
JS was exercised for initialization, 90° size-only notifications, 180°/no
origin changes, duplicate shared notifications, actual x/y movements and
screen replacement. Profile mocks verified original-value restore, failed
rotation cleanup, verified PID STOP/CONT selection (signals mocked), and the
independent watchdog restoring debug options after wrapper SIGKILL. Observer
mocks verified layer samples, clock anchor and blue/black pixel metrics.
No live Qt scene or device was tested here; source-level remapping is proven,
and removal of the observed blackout remains the orchestrator's A/B verdict.
