# Omarchy workstream: handoff (round 3, 2026-10-22 ~18:00)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Design and image packaging: [`README.md`](README.md). Device steps: [`test-plan.md`](test-plan.md). Hyprland layer: [`hyprland.md`](hyprland.md). Device results so far: [`../test-logs/integ10/README.md`](../release/OMISSIONS.md) (Omarchy boots 1–5). Rules: [`BRIEF.md`](BRIEF.md). Nothing is committed in this repo (rule).

## State

| What | Path | State |
|---|---|---|
| Live root | `build/omarchy/rootfs.img` | the iPad's root over NBD (round-2 image plus live changes); **never rebuild or edit it on the Mac while it's served** |
| Next image | `build/omarchy/rootfs-next.img` | round 3, `--fresh`, pristine; QEMU results below |
| Fallback | `build/omarchy/rootfs.pristine.img` | round 2, never booted; deletable once round 3 runs on the device (owner's call, ~8.5 GB) |
| Hot apply | `scripts/omarchy/hot-apply.sh` | brings the live iPad up to this round without a reboot |

## Round 3 changes

1. **Rotation/pointer.** `ipad-rotate` and `ipad-pointer` died silently on a fresh image: under `set -euo pipefail`, `sed` on the missing state file failed inside `$(...)` (the r3a QEMU report printed `orientation , pointer `). Both now read the state file only if it exists, and the Hyprland-socket lookup no longer aborts when `/run/user/UID/hypr` is missing (the same bug over SSH before Hyprland's first start). `ipad.lua` defaults to landscape (transform 3, home button right, touch follows).
2. **K2 reconnect.** See below. `linux-a9-bt-pagescan.service` (after every bluetooth start) + new `.timer` (every 30 s) run `ipad-bt-doctor --ensure-pagescan`, now unconditional: connectable on whenever it's off. `ipad-pair-keyboard` runs it right after pairing (the first K2 drop happened there). New diagnostics: `ipad-bt-doctor --probe`, `--trace-restart`.
3. **Audio.** `pipewire-audio` and `alsa-card-profiles` named explicitly (they were only pulled in as dependencies) and the build asserts ACP's `analog-output-speaker` path exists. No UCM profile or WirePlumber rule: on the device the sink already appeared once the packages were in (integ10 README, "Desktop audio"), and ACP's speaker path merges volume into the mono `Speaker` control (`required-any`, `volume = merge`), which is what `omarchy-audio-output-volume` (pactl) changes. Whether ACP picked that port on the device rather than a software-volume one is **unverified**: commands below.
4. **Desktop at boot.** `graphical.target` was already the round-3 default; the live iPad still runs the round-2 image (`multi-user.target`), which is why SDDM needed a manual start. **Verified** in QEMU: SDDM and Hyprland are up before anyone starts them (report line "desktop at boot"). `qemu-smoke.sh --hyprland` now only starts SDDM if it isn't running, so the check means something. Stale comments/test-plan step 7 fixed.
5. **Packaging.** README "Image packaging": build-while-live with `OMARCHY_ROOTFS=rootfs-next.img ... --fresh`, smoke test, swap, carrying the BT pairing and Wi-Fi password across, disk budget.

## Verification (QEMU/HVF, 2026-10-22 17:43)

- `OMARCHY_ROOTFS=rootfs-next.img build-rootfs.sh --fresh`: one run, ~3 min, e2fsck clean, 8.5 GB on disk (`logs/build-rootfs-r3c.driver.log`). The only `ERROR:` lines are `ufw allow` in the chroot (`|| true`, rules asserted after), as in every build.
- `qemu-smoke.sh --disk --hyprland` (`logs/qemu-r3c-verify.log`, `qemu/report-disk.txt`, `qemu/screen-disk.png`): `running`, no failed units, graphical.target at 1 min 31 s userspace; SDDM active and Hyprland running **before** the script did anything; `orientation landscape, pointer visible` (r3a: empty); pagescan service + timer enabled, timer active, `--ensure-pagescan` with no controller exits 0; audio packages present, WirePlumber sink on QEMU's dummy card; screenshot landscape with bar and notifications.
- `hot-apply.sh` against a VM of the r3a image (target forced to multi-user first): installed, `graphical.target`, `orientation landscape, pointer visible`, timer active; then live `ipad-rotate portrait` → `hyprctl monitors` `transform: 0`, back to `3`; `ipad-pointer auto` → `cursor:hide_on_touch` true. `--probe`/`--trace-restart` ran to completion without a controller and restored the unit/timer; their btmon summarizer was checked on the real pairing snoop.
- Not reachable in QEMU: a real BCM4355 (timer against bluetoothd, the trace's content), the iPad card's ACP port choice, the live round-2 image's menu file (the hot-apply merge actually runs there).

## K2 reconnect: what's known

- The kernel page-scans (so a keyboard can page the iPad) only while `HCI_CONNECTABLE` is set or a disconnected device is on its BR/EDR accept list (`net/bluetooth/hci_sync.c` `hci_update_scan_sync`, `disconnected_accept_list_entries`). BlueZ 5.87 with kernel connection control leaves connectable off and relies on the accept list (`adapter_accept_list_add` → MGMT Add Device, action 0x01, from `btd_device_set_temporary(false)` at load and after bonding).
- `scanning_paused` is **not** it: only `hci_suspend_sync`/`hci_pause_scan_sync` set it, i.e. a system suspend, and suspend is masked since boot 5, which still failed.
- The pairing trace (`build/omarchy/logs/bt-pair-k2.snoop`) shows BlueZ's Add Device for the K2 **succeeding** after bonding. So the BlueZ side works at least then; it contains no disconnect, so it doesn't show what the kernel did after the drop.
- Other candidates (none verified): `hci_powered_update_sync` returns early on an earlier failing command (SSP / LE host / adv / auth) and so never writes Scan Enable after power-on; the kernel's `HCI_PSCAN` bit out of step with the controller (it skips the write when it believes page scan is on); Add Device at load not happening on this boot path.
- `ipad-bt-doctor --trace-restart` decides it: it replays the boot sequence under btmon with the workaround held off, and prints the Add Device / Set Powered / Write Scan Enable steps, every non-success status, the raw Scan Enable and the kernel accept list.
- Note BlueZ `adapter.c`: with kernel connection control, discoverable off sends Set Connectable 0, so a hand-set `btmgmt connectable on` is undone by any discoverable toggle. Hence the timer.
- Host-initiated `bluetoothctl connect` failing (`br-connection-create-socket`) is expected for this keyboard: it reconnects itself (ReconnectMode "device") and doesn't page-scan while idle.

## Next steps

1. Orchestrator: `scripts/omarchy/hot-apply.sh`, then [`test-plan.md`](test-plan.md) "Round 3: device commands". Hot-apply covers the helpers, BT units, Hyprland layer and default target only; the foot shim, Wi-Fi NM conf and suspend masks come with `rootfs-next.img` (or were applied live by earlier rounds).
2. Next reboot: either keep the hot-applied `rootfs.img`, or swap in `rootfs-next.img` (README, "Image packaging"; save BT/Wi-Fi state first).
3. If `--trace-restart` shows the kernel at fault, a kernel fix belongs on a kernel branch (`a9/*`), and the timer can go once it lands.
