# PCIe / NVMe / Wi-Fi device test plan, round 7 (iPad 5, j71t, S8003)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


For the orchestrator. Nothing here writes to the NVMe disk, and the kernel
enforces that (`read-only.md`): every namespace is read-only, and NSIDs 2–7
(LLB/iBoot, syscfg, iPCB, NVRAM, effaceable) stay protected even if NSID 1
writes are ever enabled. Never run `mkfs`, partitioning writes,
`dd of=/dev/nvme*`, `blkdiscard` or `nvme` passthrough tools on it.

Commands are for **Omarchy** (Arch, NetworkManager, `sudo`), where udev
names the interface **`wld0`**. Under HoolockRD it is `wlan0` and there is
no `sudo`.

**Don't let it suspend.** In round 7 the PCIe host driver and the ANS
NVMe glue have no suspend/resume callbacks, so nothing would restore the
ports, NVMMU or Wi-Fi power after a suspend. (Round 8 refuses suspend by
default and has an opt-in path: `suspend-test-plan.md`.) Before any long test:
`sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target`.

## Where we are

- Round 6 on device (integ 66a186dca8 = pcie 946c7da17f + others, Omarchy,
  `logs/2026-10-22-integ-omarchy-wifi.txt`):
  - NVMe guard line OK, nvme0n1..n7 `ro=1`;
  - Wi-Fi up: firmware, TxCap, calibration; scans 2.4 + 5 GHz.
- Round 7, kernel `a9/pcie` **`868c0c56cc`** (patches `0001–0035`):
  - 0032/0035: the cfg80211 country reaches the firmware
    (`brcm,ccode-map-trivial`);
  - 0033: P2P off for this firmware, so the `-52` errors are gone;
  - 0034: ASPM L1 opt-in `pcie_apple_h9p.aspm_l1_ports=0x2`;
  - regdb in the Wi-Fi initramfs overlay (`wifi.md`, "Round 7").

## 0. Round-7 kernel

Integration (preferred):
- take patches `0032–0035`;
- no new config options;
- the m1n1 is unchanged (`build/m1n1-pcie/m1n1.bin`, round 6);
- merge the rebuilt overlay (now with `regulatory.db{,.p7s}`) into the
  Omarchy initramfs, so cfg80211 finds the regdb at boot:
  ```sh
  scripts/pcie/mkinitramfs-wifi.sh            # refreshes build/kernel-pcie/initramfs-wifi-overlay.gz
  scripts/merge-initramfs.py OUT.gz <omarchy-initramfs.gz> build/kernel-pcie/initramfs-wifi-overlay.gz
  ```
- bootargs to add:
  - `cfg80211.ieee80211_regdom=<CC>`: the owner's country, ISO 3166
    alpha-2;
  - for the ASPM run (section 6) only: `pcie_apple_h9p.aspm_l1_ports=0x2`.

Standalone payloads (HoolockRD): `build/kernel-pcie/m1n1-linux-pcie-r7.bin`,
`-r7-aspm.bin` (ASPM L1 on the Wi-Fi link), `-r7-debug.bin`.

```sh
scripts/boot.sh pongo
scripts/boot.sh linux build/kernel-pcie/m1n1-linux-pcie-r7.bin
```

## 1. Live now (current session, round-6 kernel): MAC

```sh
od -An -tx1 /proc/device-tree/soc/pcie@610000000/pci@1,0/wifi@0,0/local-mac-address
cat /sys/class/ieee80211/phy*/macaddress            # wiphy permanent address
cat /sys/class/net/wld0/addr_assign_type            # 0 = permanent, 3 = set by userspace (NM)
nmcli -f GENERAL.HWADDR,GENERAL.STATE dev show wld0
```

Expected:
- the DT bytes `f0 76 6f 76 33 62`, put there by m1n1 from the ADT;
- `phy*/macaddress` = `00:00:00:00:00:00`.

`wld0`'s current address can differ while NM randomises scans. To keep
NetworkManager on the real MAC for testing:

```sh
printf '[device]\nwifi.scan-rand-mac-address=no\n[connection]\nwifi.cloned-mac-address=permanent\n' | sudo tee /etc/NetworkManager/conf.d/90-a9-wifi-mac.conf
sudo systemctl restart NetworkManager
sleep 5; cat /sys/class/net/wld0/address           # 00:00:00:00:00:00
```

If `phy*/macaddress` isn't `00:00:00:00:00:00`: report it, the DT bytes,
and `sudo dmesg | grep -i brcmf`.

## 2. Live now: regulatory database and country

The iPad has no internet until Wi-Fi associates (the USB link gives no
router or DNS), so copy the db from the Mac. It's the same Fedora
wireless-regdb 2026.09.03 the round-7 initramfs carries.

```sh
# on the Mac
scp build/firmware/regdb/regulatory.db build/firmware/regdb/regulatory.db.p7s <user>@172.16.42.1:/tmp/
```

```sh
# on the iPad
sudo dmesg | grep -iE 'cfg80211|regulatory'
sudo install -m644 /tmp/regulatory.db /tmp/regulatory.db.p7s /usr/lib/firmware/
sudo iw reg reload                                  # cfg80211 loads the db from the root fs now
sudo iw reg set <CC>                                # the owner's country
iw reg get
sudo dmesg | tail -n 20
```

Expected:
- after `reload`, no `failed to load regulatory.db`;
- `iw reg get` shows `country <CC>` with that country's rules.

The round-6 kernel can't pass the country to the firmware (it stays
`XZ`); round 7 does. With the round-7 initramfs overlay, cfg80211 already
loads the db at boot: `cfg80211: Loading compiled-in X.509 certificates`
and **no** `failed to load` line.

## 3. Live now: association (needs the owner's Wi-Fi password)

```sh
nmcli dev wifi list ifname wld0
# 2.4 GHz
nmcli dev wifi connect "<SSID>" password "<password>" ifname wld0 name a9-wifi
nmcli connection modify a9-wifi 802-11-wireless.band bg && nmcli connection up a9-wifi
iw dev wld0 link                                    # freq 24xx, signal, tx bitrate
ip -4 addr show wld0; ip route
ping -c 20 -I wld0 "$(ip -4 route show dev wld0 | awk '/default/ {print $3}')"
# 5 GHz (same SSID on both bands; otherwise connect the 5 GHz SSID)
nmcli connection modify a9-wifi 802-11-wireless.band a && nmcli connection up a9-wifi
iw dev wld0 link                                    # freq 5xxx
ping -c 20 -I wld0 "$(ip -4 route show dev wld0 | awk '/default/ {print $3}')"
iw dev wld0 get power_save                          # expect: on
```

Expected:
- WPA2-PSK connects on both bands;
- `iw dev wld0 link` shows the right band and a sane bitrate;
- pings have 0% loss and stable latency (with power save on, a few tens of
  ms of jitter is normal).

Report `nmcli -f GENERAL,IP4 dev show wld0`, `iw dev wld0 link` and
`sudo dmesg | grep -iE 'brcmf|wld0'`.

## 4. Live now: throughput, Wi-Fi vs USB

The Mac runs the server (`brew install iperf3`; `iperf3 -s`). On the iPad:
`sudo pacman -S --noconfirm iperf3`. This needs the Wi-Fi association
from section 3, since USB gives no internet. Arch's `wireless-regdb`
package can be installed the same way, to keep the db. Use the Mac's LAN address for Wi-Fi
and 172.16.42.2 for USB. The NBD root also runs over USB, so keep USB runs
short.

```sh
ip route get <mac-lan-ip>                           # must say dev wld0
iperf3 -c <mac-lan-ip> -t 30                        # iPad -> Mac over Wi-Fi
iperf3 -c <mac-lan-ip> -t 30 -R                     # Mac -> iPad
iperf3 -c 172.16.42.2 -t 10                         # over USB ECM
iperf3 -c 172.16.42.2 -t 10 -R
```

Repeat the Wi-Fi pair on 2.4 and 5 GHz. Report the four sender/receiver
summary lines per band and `iw dev wld0 link` (bitrate) during the run.

## 5. Live now: soak (30 min) and firmware recovery

```sh
sudo dmesg -w | grep --line-buffered -iE 'brcmf|apple-h9p|apple-dart|translation fault|NVMMU|wld0' &
iperf3 -c <mac-lan-ip> -t 1800 -i 60 -R &
ping -i 0.5 -I wld0 "$(ip -4 route show dev wld0 | awk '/default/ {print $3}')" > /tmp/ping.log &
wait %2
kill %1 %3; tail -n 3 /tmp/ping.log
nmcli -f GENERAL.STATE dev show wld0
grep -iE 'brcmf|pcie' /proc/interrupts
```

Expected:
- iperf runs 30 min without stalls;
- ping loss well under 1%;
- still connected;
- no `brcmf` error, firmware `trap`/`halted`, or DART/NVMMU fault lines.

**Firmware recovery (run last; it may need a hard reset).** It forces a
firmware reload: the same chip watchdog reset brcmfmac does in every probe,
then a new firmware download.

```sh
sudo ls /sys/kernel/debug/ieee80211/
echo 1 | sudo tee /sys/kernel/debug/ieee80211/phy0/reset   # use the phyN listed above
sleep 10; sudo dmesg | tail -n 30
nmcli dev; nmcli dev wifi list
```

Expected:
- firmware reloads (a new `Firmware: BCM4355/10 ...` line, a new phyN, the
  interface renamed to `wld0` again);
- NM reconnects to `a9-wifi`.

If the iPad freezes instead, report the last lines on screen: the link did
not survive the reset.

**Don't suspend.** The PCIe host and the ANS NVMe glue have no
suspend/resume support yet (`wifi.md`). For long tests:
`sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target`.

## 6. After rebooting into round 7

```sh
sudo dmesg | grep -iE 'regulatory|cfg80211|brcmf|p2p'
nmcli dev | grep -i p2p                            # expect nothing
iw reg get
sudo iw reg set <CC>; sudo dmesg | tail -n 5             # no "Country code iovar returned err"
```

Expected:
- regdb loaded at boot (no `failed to load regulatory.db`);
- `firmware 01-9787ef4d: disabling feature P2P`;
- no `p2p-dev-wld0` and no `-52` lines.

If the firmware rejects the country (a `Country code iovar returned err`
line), report it. The firmware then stays at `XZ`, which still works.

**ASPM L1 run** (boot with `pcie_apple_h9p.aspm_l1_ports=0x2`):

```sh
sudo dmesg | grep -iE 'ASPM|aspm_l1_ports'               # "port 1: ASPM L1 allowed", "ASPM: default states L1"
cat /sys/bus/pci/devices/0000:02:00.0/link/l1_aspm  # 1
cat /sys/module/pcie_aspm/parameters/policy
```

The ASPM policy is global, but NVMe's port 0 keeps L1 off whatever it says:
`aspm-no-l1` removes L1 from that link's supported states. `echo
performance` (below) is the live off switch.

Then measure the battery current, Wi-Fi associated and idle (5 min, every
10 s). Do it with and without L1, ideally with charging not armed:

```sh
for d in /sys/class/power_supply/*; do [ -f $d/current_now ] && echo $d; done   # pick the battery
B=/sys/class/power_supply/<battery>; cat $B/status
for i in $(seq 30); do cat $B/current_now; sleep 10; done
```

Repeat:
- with L1 substates:
  `echo powersupersave | sudo tee /sys/module/pcie_aspm/parameters/policy`;
- with ASPM off again:
  `echo performance | sudo tee /sys/module/pcie_aspm/parameters/policy`;
- section 5's soak with L1 on.

If anything hangs with L1, go back to the plain round-7 boot (L1 off) and
report.

## 7. NVMe (pending from round 6; works in Omarchy too, as root)

```sh
sudo dmesg | grep -c 'refused opcode'                   # 0
for b in /sys/block/nvme0n*; do echo "$b ro=$(cat $b/ro)"; done
sudo sh -c 'time dd if=/dev/nvme0n1 of=/dev/null bs=1M count=512'
sudo sh -c 'i=0; while [ $i -lt 16 ]; do dd if=/dev/nvme0n1 of=/dev/null bs=1M count=1024 skip=$((i * 1900)) 2>&1 | tail -n 1; i=$((i + 1)); done'
sudo sh -c 'dd if=/dev/nvme0n1 bs=1M count=64 skip=5000 2>/dev/null | md5sum; dd if=/dev/nvme0n1 bs=1M count=64 skip=5000 2>/dev/null | md5sum'
sudo dmesg | grep -iE 'translation fault|NVMMU|timeout|reset|abort'; echo "faults: $?"   # faults: 1 = none
```

Expected:
- 0 refused opcodes, every `ro=1`;
- the MB/s figure reported;
- equal md5sums and `faults: 1`.

## 8. Report template

```
MAC (section 1):       DT bytes / phy macaddress / wld0 address after NM conf
regdb+country (2, 6):  dmesg lines, iw reg get
association (3):       2.4 + 5 GHz: link lines, ping summary, power_save
throughput (4):        Wi-Fi 2.4 / 5 GHz up/down, USB up/down
soak + reset (5):      ping loss, iperf, fault grep, reset result
round 7 (6):           P2P/regdb lines; ASPM: l1_aspm, currents (L0 / L1 / L1SS)
NVMe (7):              MB/s, md5sums, fault grep
```
