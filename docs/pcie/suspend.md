# System suspend (s2idle), round 8

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Status: **code built, not tried on hardware.** Kernel `a9/pcie`
**`ca94a3c6f9`**, patches `0036–0042`. Device steps:
[`suspend-test-plan.md`](suspend-test-plan.md).

The iPad has no PSCI and no SYSTEM_SUSPEND, so the only sleep state is
suspend-to-idle: devices suspend, CPUs sit in WFI, and any unmasked AIC
interrupt wakes them. `mem` maps to s2idle (`/sys/power/mem_sleep` =
`[s2idle]`, inferred from the arm64 code).

## What iOS does (RE from the iOS 16.7.10 kernelcache, previous round-8 agent)

- `AppleS800xPCIePort::disableGated`, port sleep:
  1. PME_Turn_Off: write 0x31 to port +0x8c, poll bit 0 (ack) every 1 ms
     for 10 ms, bit 5 = hardware timeout. iOS continues either way.
  2. Wait for L2 (link status bit 6, `l2-entry-timeout`).
  3. PERST#, `perst-to-refclk-delay-us` (60), refclk off, port power off,
     port enable off.
- iOS NVMe sleep: normal CC.SHN=01. SHN=11 (LPSR) is used only in
  self-refresh. Linux nvme-pci's simple-suspend path does the same.
- brcmfmac resume: a hot resume if the chip kept its state, otherwise a
  full re-probe (firmware reload). So a reset Wi-Fi chip recovers as long
  as its link retrains first.

## Patches (all default-off; with no parameter the kernel behaves as before, except that a suspend attempt is now refused)

| # | Patch | Effect |
|---|---|---|
| 0036 | `PCI: apple-h9p: Opt-in system sleep (s2idle)` | `pcie_apple_h9p.suspend=0..3` (runtime-writable, 0644). See the levels below. |
| 0037 | `nvme-pci: Don't enable a device whose host bridge cut it off` | `nvme_pci_enable()` returns -ENODEV for a disconnected device (no BAR access over a dead link). |
| 0038 | `wifi: brcmfmac: pcie: Stay down if the device was lost over suspend` | Same for brcmfmac's resume. |
| 0039 | `pinctrl: apple: gpio: Skip set_wake` | `IRQCHIP_SKIP_SET_WAKE`. Without it, `enable_irq_wake()` returns -ENXIO and gpio-keys (Home/Power are `wakeup-source`) fails its suspend, which aborts **every** suspend. |
| 0040 | `pmdomain: apple: pmgr-pwrstate: Keep wakeup-path domains on in sleep` | `GENPD_FLAG_ACTIVE_WAKEUP` on every PMGR domain. Affects system sleep only, and only for devices on the wakeup path. |
| 0041 | `arm64: dts: apple: s800-0-3: Keep the AP GPIO domain on` | `apple,always-on` on `ps_gpio`. It holds PCIe PERST# (`pinctrl_ap` 160/161) and the wake buttons (96/97), and genpd would otherwise power it off in the noirq phase. At runtime it is on anyway (pinctrl_ap is bound). |
| 0042 | `usb: dwc2: Optionally keep a connected gadget over suspend-to-idle` | `dwc2.s2idle_keep_gadget=1` (0644, default 0): in s2idle, a connected gadget is left running and not disconnected; the controller and PHY are put on the wakeup path, so `ps_usbotg`/`ps_usbctrl` stay on (needs 0040). Needed because Omarchy's root is NBD over the USB gadget. |

### `pcie_apple_h9p.suspend` levels

| Level | Ports | Endpoints |
|---|---|---|
| 0 (default) | `.prepare` returns -EBUSY with `system sleep refused: untested on this SoC`. Nothing is touched. | n/a |
| 1 | Every port stays powered and in L0. In `resume_noirq`, before any endpoint runs, each port's PERST# and link status are checked from SoC registers. A lost link is retrained; a port that doesn't retrain is cut off. The NVMMU registers are compared with the programmed values and re-programmed if lost. | NVMe: CC.SHN shutdown, re-enabled by the reset path (preinit/HMB/NVMMU slots/CC.EN as at boot). brcmfmac: D3_INFORM handshake (a missing ack aborts the suspend), hot resume. |
| 2 | Like 1, but ports with `pwren-gpios` (Wi-Fi, port 1) take the iOS port sleep in `suspend_noirq`: root-port command and bus numbers cleared, PME_Turn_Off + ack, L2, PERST#, refclk/power/enable off. On resume: the boot `setup_port` (2 tries), 100 ms, root-port config restored before the endpoint resumes. The chip keeps REG_ON (PMU GPIO, out of reach in noirq). | brcmfmac sees a reset chip: full re-probe and firmware reload. DART1 loses its state with the port. `apple_dart_resume` (resume phase, before brcmfmac through the IOMMU device link) does `hw_reset` + S5L8960X config + TTBR/TCR restore. |
| 3 | Like 2 for every port, NVMe included. **Experimental.** NVMMU re-programmed after port 0 retrains. | NVMe re-enabled through the reset path after a link retrain. |

At every level, everything below the bridge is held in **D0**
(`PCI_DEV_FLAGS_NO_D3` from prepare to complete). D3hot would take the
links to L1, which these ports don't do (`aspm-no-l1`). In s2idle the PCI
core then skips its noirq restore for D0 devices and restores config in
the resume phase (verified in the 7.x `pci-driver.c`); h9p restores the
root port itself in `resume_noirq`.

A **dead port** gets PERST#, port off and its bit in `down_ports`.
Config access below it returns DEVICE_NOT_FOUND, and every device under
it is `pci_dev_set_disconnected()`, so NVMe/brcmfmac fail cleanly
(0037/0038) instead of reading a BAR over a dead link, which can stall
the CPU.

The PCIe genpd virtual devices (`pd_dev[]`) are `dev_pm_syscore_device`,
so genpd never powers `ps_pcie*` off in sleep. The DARTs share
`ps_pcie_link0/1`, so they stay powered too. The previous agent checked
that this dpm ordering with the `DL_FLAG_PM_RUNTIME` link can't deadlock.

## Blockers outside PCIe

| Blocker | Status |
|---|---|
| AIC / pinctrl-apple-gpio lack `irq_set_wake` → gpio-keys suspend fails | **fixed** for GPIO (0039). AIC isn't needed: the GPIO parent is a chained interrupt, which suspend never disables. |
| `ps_gpio` powered off in noirq (PERST#, buttons) | **fixed** (0041) |
| Panel black after any genpd stage (C/D/E), backlight on | **fixed** (0043). genpd powered off simple-framebuffer's `disp0` + `dp` in noirq, which reset the display pipe and eDP link that iBoot set up. They are now `apple,always-on` in `s800x-ipad5.dtsi`. The ADT pmgr table gives the path RTMUX → DISP0 → {DP, DISP0_DART}. The panel is `lcd,edp`, so MIPI_DSI isn't used on iPad 5. `mipi_dsi`/`isp`/`isp_sens*`/`pmp` have no consumer and no driver, so genpd's unused-domain sweep at boot should turn them off (no `pd_ignore_unused`). Their `off-0` after sleep is probably that, not the sleep, but no boot-time summary of them was ever captured. |
| PMGR domains lack active-wakeup | **fixed** (0040), used by 0042 |
| dwc2 soft-disconnects the gadget on suspend → NBD root and SSH over `usb0` drop | **gated** (0042, `dwc2.s2idle_keep_gadget=1`). Without it, a sleep over ~20 s also makes `serve-root.sh` restart qemu-nbd (it restarts when 172.16.42.2 has been gone for 20 s), so the root is lost. |
| `rtc-apple-pmic` has no alarm → no `rtcwake` | **open**. Unattended tests use `/sys/power/pm_test` (`devices`, `platform`: the kernel resumes itself after 5 s). A real sleep needs the Home button. |
| logind `HandlePowerKey` defaults to poweroff (no override in the Omarchy rootfs) | **open, avoid**: wake with **Home**, not Power. |
| BT has no host-wake IRQ in the DT | open; BT isn't a wake source. |
| Wi-Fi: no WoWLAN; cfg80211 disconnects on suspend | expected; NetworkManager reconnects. |
| Drivers whose power domain genpd turns off in noirq and that have no resume (e.g. i2c, ausb PHY when not kept) | **unverified**. Watch for i2c/touch/battery/audio errors after resume in stage C. |

## Unverified (everything on hardware), main risks

- Whether the links really stay in L0 over s2idle at level 1. `ps_pcie*`
  are held on, but the refclk source or another PMGR domain could still
  drop.
- Whether PME_TO_Ack / L2 work with the endpoint in D0 (iOS puts it in
  D3hot first). The sequence continues either way, and PERST# ends the
  link.
- Whether the BCM4355 accepts D3_INFORM while it stays in PCI D0
  (level 1), and whether it re-probes cleanly after PERST# with REG_ON on
  (level 2).
- Whether DART1 comes back fully with `apple_dart_resume` after a port
  power cycle (round-7 open point 2). The attach-time re-apply is the
  verified path; resume uses the same `apple_dart_s5l8960x_hw_config`.
- Whether a USB host (the Mac) tolerates a gadget that NAKs for the whole
  sleep (0042).
- Whether the NVMMU keeps its registers when port 0 is powered off
  (level 3). The code checks and re-programs.
