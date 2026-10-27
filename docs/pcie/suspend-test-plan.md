# Suspend (s2idle) device test plan, round 8 (iPad 5, j71t)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


For the orchestrator, with the owner present for stages E–F (Home button).
Background and patch list: [`suspend.md`](suspend.md). Nothing here
writes to NVMe; the only NVMe access is a 64 MiB direct **read**.

## 0. Kernel and payload

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

  (add the Wi-Fi overlay / `cfg80211.ieee80211_regdom=` as in the round-7
  boot). No suspend parameter goes on the command line: both knobs are
  set at runtime by the test script, and reset afterwards.
- **Default-boot check (pass/fail before anything else):** the boot must
  look like round 7. Wi-Fi associates, nvme0n1..n7 `ro=1`, and
  `cat /sys/module/pcie_apple_h9p/parameters/suspend` = `0`,
  `/sys/module/dwc2/parameters/s2idle_keep_gadget` = `N`.

## 1. Setup (every session)

On the Mac, keep netconsole running for the whole test:
```sh
scripts/omarchy/netconsole.py build/omarchy/logs/s2idle-netconsole.log
```
Copy the helper to the iPad and check the state:
```sh
scripts/omarchy/ssh.sh 'cat > /run/s2idle-test.sh && chmod +x /run/s2idle-test.sh' < scripts/pcie/s2idle-test.sh
scripts/omarchy/ssh.sh 'cat /sys/power/state /sys/power/mem_sleep /sys/power/pm_test;
  systemd-analyze cat-config systemd/logind.conf | grep -i HandlePowerKey;
  sudo grep -E "^(pcie|gpio|usb)" /sys/kernel/debug/pm_genpd/pm_genpd_summary;
  iw dev wld0 link | head -3'
```
Expected: `freeze mem` (or `freeze`), `[s2idle]`, `[none] core processors
platform devices freezer`, and `gpio` listed `on`.

Every stage runs the helper detached, so a dropped SSH session can't
kill it, then reads the log:
```sh
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=s2idle-STAGE --collect /run/s2idle-test.sh MODE PMTEST 1'
# wait ~45 s (5 s test delay + resume + 20 s settle)
scripts/omarchy/ssh.sh 'cat /run/s2idle/last.txt' > docs/pcie/logs/2026-10-XX-s2idle-STAGE.txt
```
Save each stage's log under `docs/pcie/logs/`, plus the netconsole log.

## 2. Stages (in order; stop at the first failure)

| Stage | Command (MODE PMTEST) | What it exercises | Pass |
|---|---|---|---|
| A | `0 devices` | default refusal | `rc=1` (EBUSY), dmesg `system sleep refused: untested on this SoC`; nothing else suspended (no `nvme`/`brcmf` suspend lines), SSH still fine. |
| B | `1 devices` | every `.suspend/.resume`, no noirq phase, no genpd power-off | `rc=0`, elapsed ≈ 5 s; dmesg `system sleep (mode 1): ports 0x3 stay linked, ports 0 go to L2`, `dwc2 ...: s2idle: gadget stays connected`, `Apple ANS: enabling controller` again after resume; `nvme0 state=live`; dd read OK; `wld0` reconnects (`iw dev` shows ssid); SSH/NBD never dropped. |
| C | `1 platform` | + late/noirq: h9p `suspend_noirq`/`resume_noirq`, genpd power-offs | as B, plus `port 0: link kept over the sleep`, `port 1: link kept over the sleep`, `NVMMU: state kept over the sleep`. Run C **5 times** in a row. |
| D | `2 platform` | Wi-Fi port L2 + off + retrain, DART1 restore, brcmfmac re-probe | `port 1: asleep (PME_TO …, link …)`, then no `no link after sleep`; brcmfmac reloads firmware (`brcmf_c_process_clm_blob`/firmware version lines) or hot-resumes; no `DART … fault`; `wld0` reconnects; ping through Wi-Fi works. Note whether `no PME_TO_Ack` / `did not enter L2` are logged (informational). Run D **3 times**. |
| E | `1 none` | **real s2idle**, owner wakes with **Home** after ~20 s | screen off/frozen, back after Home; everything as C; record time to wake. SSH may stall during the sleep: that's fine as long as it recovers. |
| F | `2 none` | real s2idle with Wi-Fi port off | as D + E. |
| G (optional) | `3 platform` | NVMe port L2 + retrain (experimental) | `port 0: asleep`, `NVMMU: port 0 was retrained, programming it again`, NVMe live again, dd read OK. Only after D passed 3×. |

Extra checks after C and E (they cover the non-PCIe drivers):
```sh
scripts/omarchy/ssh.sh 'sudo dmesg | grep -iE "error|fail|timeout|fault|SError|cut off|lost" | tail -40;
  cat /sys/class/power_supply/*/capacity 2>/dev/null; ls /dev/input/'
```
Touch, battery, audio and buttons must still work after resume (owner
check for touch and audio).

## 3. Stop conditions

- The helper doesn't come back within **60 s** in a pm_test stage, or
  within 60 s of pressing Home in E/F: stop. Photograph the screen
  (`no_console_suspend` keeps printing), keep the netconsole log, reboot
  with Power + Home held, and back to round 7's payload if needed.
- Any of `SError`, `Internal error`, `DART … fault`, `Apple ANS: PCIe link
  down`, `cut off until reboot`, `nvme … timeout`, `probe after resume
  failed`: stop escalating (no higher level/stage). Keep the log; the
  system may still be usable.
- `rc` non-zero in B–G: read `suspend_stats` (`failed_*`, `last_failed_dev`)
  in the log. A device other than h9p refusing is a finding, not a hang.
  Record it and stop.
- Never press **Power** to wake (logind's default `HandlePowerKey=poweroff`
  would shut down after resume), unless the logind check in §1 showed
  `ignore`.

## 4. What to send back

`docs/pcie/logs/*-s2idle-{A..G}.txt`, `build/omarchy/logs/s2idle-netconsole.log`,
the number of passes per stage, and for D/F whether PME_TO_Ack and L2
were seen.
