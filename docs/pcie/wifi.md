# Wi-Fi on j71t: BCM4355 C0 on PCIe port 1

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Round 6 brought it up; round 7 fixes MAC/regulatory/P2P and adds power
options. Status (device, integ 66a186dca8 + pcie 946c7da17f, Omarchy,
`logs/2026-10-22-integ-omarchy-wifi.txt`):

- **Verified working** on the first boot:
  - link, `[14e4:43dc]` on bus 2 at 1.13 s;
  - `brcmfmac4355-pcie` firmware with TxCap and calibration (from m1n1)
    blobs; `Firmware: BCM4355/10 wl0: ... 9.44.205.0.3.50.46 FWID
    01-9787ef4d`;
  - NetworkManager scans 2.4 GHz (ch 1/5/6/11) and 5 GHz (ch 36/100).
- **Association verified** (same round-6 kernel, live, 2026-10-22):
  - `nmcli dev wifi connect <owner's WPA2 SSID>` activated;
  - `iw dev wld0 link`: 5 GHz **ch100 (5500 MHz, DFS, world regdom 00)**,
    signal -67 dBm, rx 866.6 / tx 780.0 MBit/s;
  - DHCP 192.168.2.x, default route via wld0 (172.16.42.0/24 stays on
    usb0);
  - internet: ping 1.1.1.1 13 ms, DNS and HTTPS OK;
  - curl download 55.3 Mbit/s (proof.ovh.net 100 MB, probably server or
    uplink limited), upload 201.5 Mbit/s (speed.cloudflare.com 30 MB);
  - no brcmfmac errors besides the P2P -52 trio, re-logged at 1184 s when
    NM recreated p2p-dev (fixed in round 7, 0033);
  - MAC **00:00:00:00:00:00** in use (after the NM conf in test-plan
    section 1).
- Not yet tested: iperf3 Wi-Fi vs USB, 30 min soak, firmware reset, the
  round-7 kernel (country, no P2P), ASPM.

## Round 7

| Item | Finding | Change |
|---|---|---|
| MAC | ADT `/chosen mac-address-wifi0` = **00:00:00:00:00:00** (Apple OUI; same as `uart2/wlan` local-mac-address). Verified: m1n1's `dt_set_mac_addresses()` copies it into `wifi0`'s `local-mac-address`. brcmfmac's NVRAM loader replaces the NVRAM `macaddr` with the platform MAC (`eth_platform_get_mac_address()`), so the firmware's `cur_etheraddr` and the wiphy's permanent address should be it. That is how Apple-silicon DTs do it too. The `00:00:00:00:00:00` NM showed is locally administered: NM's scan randomisation (inferred). | none; verify with the test plan |
| Regulatory DB | cfg80211 is built in and asks for `regulatory.db` ~1.0 s into boot. The Omarchy initramfs has none. | `scripts/pcie/fetch-regdb.sh` (Fedora wireless-regdb 2026.09.03, upstream-signed) → `regulatory.db{,.p7s}` in `initramfs-wifi{,-overlay}.gz` |
| Country | NVRAM default `ccode=XZ` (world). brcmfmac only passes a cfg80211 country to the firmware with a ccode map, and it parsed `brcm,ccode-map(-trivial)` only for `brcm,bcm4329-fmac` nodes. | 0032: parse it for PCIe nodes; 0035: `brcm,ccode-map-trivial` on wifi0 (country → `<CC>/0`). The CLM blob lists ~180 ISO codes plus XZ (verified with `strings`), but whether it takes rev 0 is inferred; if not, the firmware stays XZ and brcmfmac logs the iovar error. |
| P2P -52 | the firmware answers `p2p` but fails `p2p_da_override` (-52), so NM's P2P device creation fails at every start | 0033: per-FWID feature-disable table, P2P off for `01-9787ef4d`. No p2p-dev, no error. |
| ASPM | the DT keeps L1 off (aspm-no-l1) on both ports | 0034: opt-in `pcie_apple_h9p.aspm_l1_ports=0x2` (Wi-Fi link only); policy switchable at runtime |
| Firmware crash | brcmfmac's recovery (`brcmf_pcie_reset`, also triggered by `brcmf_fw_crashed()`) does a chip watchdog reset and reloads the firmware, without a PCIe-level reset. The same watchdog reset runs in every probe (`brcmf_pcie_buscore_reset`), so it already ran on this link (verified indirectly). | none; test with debugfs `reset` |
| System suspend | round 8: opt-in s2idle (`pcie_apple_h9p.suspend=1/2`), see `suspend.md`. Refused by default; untested on hardware. | keep suspend masked in Omarchy until `suspend-test-plan.md` passes |

### Power, what exists today

- **Firmware power save** (802.11 PS, the main battery lever when
  associated): brcmfmac turns it on by default, and NM's default
  `wifi.powersave` keeps it on. Check with `iw dev wld0 get power_save`.
- **Radio off:** `nmcli radio wifi off` (or rfkill) brings the interface
  down; brcmfmac then sends `wl down` to the firmware.
- **PCIe link:** the link stays in L0 by default. With `aspm_l1_ports=0x2`:
  - the default policy gives L1;
  - `powersupersave` adds L1.1/L1.2: Linux programs the substates from
    the capabilities (iOS: wlan CTL1 0x4044000f, bridge1 0x4044190f/0x10);
  - `performance` turns ASPM off again.

  The RC's refclk gating (iOS clears rc+0x114+port*0x80 bit 8) stays off,
  so L1.2 saves less than on iOS. That and CLKREQ# (pin 165, muxed in
  round 6) are the next steps if L1 is stable.
- **No PCIe runtime PM** in brcmfmac. Powering the chip off (REG_ON) when
  Wi-Fi is off would need h9p support for unbinding and re-powering a late
  port. Not done.

## Hardware facts (verified, live ADT `build/firmware/private/IODeviceTree.txt`)

| What | Value | ADT source |
|---|---|---|
| Chip | BCM4355 C0, PCI 14e4:43dc rev 0x0a, class 0x028000, subsystem 106b:1103 | `apcie/pci-bridge1/wlan@0` (IOPCIDevice) |
| BARs | BAR0 32 KB, BAR2 4 MB | `wlan@0` reg / IODeviceMemory |
| MSI | vector 0x108 = 256 + 8 (port 1's MSI base 8) | `wlan@0` IOInterruptSpecifiers |
| Root port | 106b:1004, Gen1 (`maximum-link-speed 1`), `manual-enable`, `built-in` | `pci-bridge1@1` |
| PERST# | AP GPIO 161 (0xa1) | `pci-bridge1` function-perst |
| CLKREQ# | AP GPIO 165 (0xa5), peripheral function | `pci-bridge1` function-clkreq |
| DEVICE_WAKE | AP GPIO 67 (0x43) | `pci-bridge1` function-device_wake |
| REG_ON | **D2365 PMU GPIO 14** (phandle 0x4f = `pmu@3C`) | `uart2/wlan` function-reg_on |
| Board | `module-instance = "feara"` | `uart2/wlan` |
| Calibration | `wifi-calibration-msf` (BLOB) | `uart2/wlan` |
| MAC | `/chosen mac-address-wifi0` (also `uart2/wlan` local-mac-address) | |
| WLAN UART | uart2 @0x20a0c8000, IRQ 194, TX = GPIO 192, RTS = GPIO 194 (function-tx/-rts) | `uart2@A0C8000` |
| Host wake | AP GPIO 113 (wlan interrupts <0x71 2>) | not used by brcmfmac PCIe |
| Firmware | `C-4355__s-C0`, NVRAM `P-feara_M-YSBC_V-u__m-4.1` | `build/firmware/selection.json` ("HWIdentifiers agree with RequestedFiles on this unit") |
| Bluetooth power | PMU GPIO 13 (periph's driver) | `bluetooth` function-power_enable |

## Template: Pauli1Go A10 J172 (iPad 7, BCM4355 C1)

Saved in `/tmp/pcie-tpl/dev/` (scratch, may be gone):
- **Power:** `pci-pwrctrl-apple-wifi`, with
  - VDD = PMU GPIO 14 through `regulator-fixed`, `off-on-delay-us = 100000`;
  - J172 data: no device-wake pulse, 100 ms stabilization after VDD on;
  - device-wake requested low and left low.
- **WLAN UART:** `apple,t8010-wlan-uart-provider` keeps UART2 powered,
  resets/configures it, and muxes its TX/RTS pins to the UART function.
- **DT:** `brcm,board-type = "apple,rudderb"`, `apple,antenna-sku = "XX"`,
  `bus-range = <2 2>`.

## What this tree does

**Kernel** (`a9/pcie`):
- `gpio: add Apple D2365 PMIC driver` and its binding, cherry-picked
  unchanged from a9/periph (identical commits; the integ trial merge is
  clean).
- `PCI: apple-h9p: Bring up powered endpoints after probe`. A port with
  `pwren-gpios` is a *late port*:
  - At probe: registers mapped, PERST# asserted, DEVICE_WAKE requested
    low. No link training, so NVMe on port 0 never waits.
  - After the host bridge is registered, a delayed work gets the power
    GPIO. While it's `-EPROBE_DEFER` it retries every 500 ms, for at most
    30 s.
  - Power cycle with PERST# asserted: GPIO low, 100 ms, high, 100 ms (the
    J172 numbers). Then the normal S8003 port setup: refclk, port power,
    tunables, MSI, PERST# release, phy-ip, LTSSM. Link wait ≤ 500 ms.
  - No link: one more power cycle, then power off and give up.
  - Link up: 100 ms (PCIe 6.6.1), then `pci_rescan_bus()`. Port 1's root
    port appears as 00:01.0, the chip on bus 2.
- `arm64: dts: apple: s8003-j71t: Wi-Fi on PCIe port 1`:
  - PMU GPIO node (frees line 14; 13 stays Bluetooth's);
  - port 1 gets `pwren-gpios`, `device-wake-gpios`, `bus-range = <2 2>`;
  - `wifi0: wifi@0,0` with `brcm,board-type = "apple,feara"`,
    `apple,antenna-sku = "XX"` and a `local-mac-address` placeholder;
  - uart2 enabled with its TX/RTS pins in the UART function (the serial
    driver plays the template's "provider": power domain held, UART
    configured);
  - port 1's CLKREQ# (GPIO 165) muxed to its peripheral function by the
    PCIe node's default pinctrl, as the template does with its pin 19;
  - `pcie0_dart1` on.
- Config (`patches/pcie/pcie.config`): `GPIO_APPLE_D2365`, `WLAN`,
  `WLAN_VENDOR_BROADCOM`, `CFG80211`, `BRCMFMAC`, `BRCMFMAC_PCIE`,
  `BRCMDBG`, all built in.

**m1n1** (`patches/pcie/m1n1/0002`):
- `dt_set_wifi()` falls back to `/arm-io/uart2/wlan` (A9/A10 have no
  `/arm-io/wlan`) and copies `wifi-calibration-msf` into `brcm,cal-blob`.
- It no longer requires `wifi-antenna-sku-info`, which A9 lacks; the DT's
  "XX" stays.
- The MAC already comes from stock m1n1 (`/chosen mac-address-wifi0` →
  alias `wifi0` → `local-mac-address`).

**Firmware** (`scripts/pcie/mkinitramfs-wifi.sh`):
- Puts the 8 staged files into the initramfs under `/lib/firmware/brcm/`.
- brcmfmac asks for `brcmfmac4355-pcie.apple,feara-<module>-<vendor>-<version>-XX.*`
  first. With OTP module YSBC, vendor u, version 4.1 that is the staged
  `-XX` set.
- It also adds `...apple,feara.*` and plain `brcmfmac4355-pcie.*` copies of
  the `-XX` set, for the case where the OTP read fails.
- It writes `build/kernel-pcie/initramfs-wifi-overlay.gz` too, for merging
  into the integration initramfs with `scripts/merge-initramfs.py`.

brcmfmac support (verified in this tree):
- device 0x43dc is in the PCIe ID table (WCC_SEED vendor);
- chip revisions 0–10 map to `brcmfmac4355-pcie`;
- OTP read is implemented for 4355;
- `brcm,cal-blob` is read from any DT node and sent after firmware boot.

The DT MAC (`local-mac-address`) is only read for `brcm,bcm4329-fmac`
nodes, so expect the MAC from the chip's OTP or NVRAM, or a random one.
It can be fixed later.

## Expected boot log (inferred)

```
pcie-apple-h9p 610000000.pcie: ports 0x1, lane-cfg 1
pcie-apple-h9p 610000000.pcie: ports 0x2: bringing up after their endpoint power
pcie-apple-h9p 610000000.pcie: port 1: waiting for its power GPIO          (if the PMIC GPIO isn't up yet)
pcie-apple-h9p 610000000.pcie: port 1: endpoint powered (attempt 1)
pcie-apple-h9p 610000000.pcie: port 1: bringing up link (Gen1, lanes 0x4)
pcie-apple-h9p 610000000.pcie: root port tunables: N of 2 registers changed
pcie-apple-h9p 610000000.pcie: port tunables: N of 6 registers changed
pcie-apple-h9p 610000000.pcie: port 1: link up (LTSSM 0x11)
pci 0000:00:01.0: [106b:1004] type 01 class 0x060400 PCIe Root Port
pci 0000:02:00.0: [14e4:43dc] type 00 class 0x028000 PCIe Endpoint
brcmfmac 0000:02:00.0: enabling device (0000 -> 0002)
brcmfmac: brcmf_fw_alloc_request: using brcm/brcmfmac4355-pcie for chip BCM4355/10
brcmfmac: brcmf_c_process_txcap_blob: ...
brcmfmac: brcmf_c_preinit_dcmds: Firmware: BCM4355/10 wl0: ... FWID ...
```

and `wlan0` in `/sys/class/net`.

## Risks / open points

- **Power sequence:** A9 timing is taken from A10 J172. If the link
  doesn't train, try a longer settle (`H9P_PWREN_SETTLE_MS`), or the
  template's "endpoint power after link setup" order.
- **DEVICE_WAKE polarity** (active-high) is inferred from the ADT flags. It
  stays low (deasserted) as on J172.
- **CLKREQ# pin 165, function 1**: by analogy with A10 (pin 19). iBoot
  doesn't set it up for Wi-Fi on a tethered boot. Refclk gating stays off,
  so a wrong mux probably doesn't block link training (inferred).
- **uart2 pins as UART** (192/194, function 1) by analogy with A10's
  148/150. If something else owns them, the serial probe fails, but the
  Wi-Fi path doesn't depend on it.
- **C0 firmware:** iOS's `feara.trx` is used as is (`.bin`). brcmfmac's
  4355 support was tested upstream with Mac C1 firmware, not iOS C0.
- **OTP read:** if it fails, the fallback names are used. `brcmfmac.debug`
  in the r6-debug payload shows which names it tried.
- **j72t** (cellular iPad 5) isn't wired; only j71t's ADT was read.
