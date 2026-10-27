# usb-host5 device test plan (orchestrator)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Goal:** the Lightning camera adapter works on Linux the way it does on iOS. The
adapter's hub (1a40:0101) and the flash drive (SanDisk 0781:5581) enumerate at high speed,
and the iPad keeps charging from the adapter's power input while it is the host. The
reverse engineering and the reasons behind each step are in [ds-usb-host5.md](ds-usb-host5.md).

**Entry:** `usb-host5-5306b84fa3`
- kernel `a9/usb-host5` `5306b84fa39dacf0c2857f621a0988fc53cadbf5` (four commits on
  irqfix-usb4 `209a5a55e3`)
- payload `build/boot/m1n1-linux-usb-host5-5306b84fa3.bin`, sha256
  `8fa35075c7e8b5533da863e1711922496b554c53099586abe22c63e5905640e6`
- entry dir `build/usb-host5/entry/`, manifest `build/usb-host5/manifest.json`
  (`hardware_tested=false`)

> Private graphics-dependent excerpt omitted from this source snapshot.

### usb-host5b (current entry, after the 2026-10-27 11:17 run)

**Entry:** `usb-host5b-2801e838fc`
- kernel `a9/usb-host5b` `2801e838fc9a26e418b987aea173909db1d254ea`: usb-host5 plus four fix commits, same base
  `209a5a55e3`
- payload `build/boot/m1n1-linux-usb-host5b-2801e838fc.bin`, sha256 `5479f4f3497ccb658ce38915407f7630672a98eceffa3ce493c4ad7df9d056c1`
- entry dir `build/usb-host5b/entry/`, manifest `build/usb-host5b/manifest.json`

Same m1n1, initramfs, DTB, config and command-line rules as usb-host5, with only the
entry name changed. Boot it as in §1, using `build/usb-host5b/entry-name` and
`build/usb-host5b/payload-path`. Offline checks: `scripts/periph/check-usb-host5b.sh`.
Everything in §1–§7 still applies. The differences
([ds-usb-host5.md](ds-usb-host5.md) §9):

- **USB_CTL.** Each host entry logs `USB_CTL+0 trace: entry …, gadget-off …, bit1 …,
  phy-on …, host …, phy-host …, ehci …, ohci …[, reassert …]`, and debugfs `regs`
  shows the same on its `usb_ctl trace` line. **Report the trace whatever the result:**
  it shows at which step bit 1 is lost, or that it never latches.
- **Direct test of the 0x3 hypothesis** (once the adapter is in host mode, no owner
  action). Do both:
  1. `scripts/periph/usb-host5-watch.sh usbctl 0x3`: writes `USB_CTL+0 = 0x3` now. It
     prints the readback, `regs`' `usb_ctl`/`ehci:` lines, the `USB_CTL`/`usb`/`hub`
     kernel lines and `lsusb -t`. Wait about 5 s, then run `status` and `lsusb -t` again.
  2. `scripts/periph/usb-host5-watch.sh reenter usb_ctl_force=3`: a fresh host entry
     that forces `0x3` right after EHCI/OHCI come up and logs
     `USB_CTL+0 forced to 0x3: read back …, … after 1 ms, … after 100 ms`.

  How to read the result:

  | Readback | Port | Meaning |
  |---|---|---|
  | `0x1` immediately | — | bit 1 is not writable in this state: the iOS bit cannot be reproduced as a plain write |
  | `0x3`, then `0x1` after 1/100 ms | — | self-clearing or cleared by hardware; report the timing |
  | `0x3` held | hub attaches (`portsc1` connect bit, `1a40:0101`) | hypothesis confirmed |
  | `0x3` held | still `00501000` | bit 1 is not the missing piece; go to §6 step 5 |

  Then `reenter usb_ctl_force=0` returns to the normal path.
- **Charging.**
  - The battery window now ends at 4.4 V, so a near-full battery keeps charging until the
    PMIC terminates.
  - Read `battery: … voltage_now_uv=…` in `lightning_state`.
  - A change in the charger's own registers right after the attach now logs
    `charger registers changed (input 0x.., control 0x.., charge 0x..); reasserting …`
    instead of a 60 s ownership fault. **Report these lines.**
  - The input reaches `input_current_limit_ua=2075000`: the calibrated 2100 mA code `0xa0`.
  - If the battery reaches Full, `status=Full` with `current_now` near 0 at that input is a
    pass: the iPad is running from the adapter.
- **Verdict.** It now has 10 criteria. `input_2100` accepts `>= 2000000`. The new
  `input_held` fails if the input falls back to the 100 mA handoff in host mode. Re-scoring
  the 11:17 run: 6/10 (`usb_ctl`, `hub_hs`, `drive_ro` and `input_held` fail).

**Rules for this test.**
- No countdowns. The owner swaps cables whenever they are ready. The tools wait
  indefinitely and react to attach and detach.
- Wi-Fi SSH is the control path while the Mac cable is out.
- No `dmesg -n` or console loglevel change.
- Nothing here writes syscfg, NVRAM or boot storage.
- The only mount is a read-only one.

---

## 0. What the kernel now does on its own

| Event | Kernel action (`dmesg` / `journalctl -k` lines start with `lightning:` or `apple-s8000-usb-role`) |
|---|---|
| Any plug | 0.5 s later, a GetID without reorient. While a USB host uses the gadget, repeated every 10 s; otherwise every 2 s. Quiet: only ID or phase changes are logged. |
| ID `11 f0 00 00 00 00` | `power handshake ok`, then `host role on`. Role switch: `host mode for the Lightning accessory (USB_CTL 0x3/…, accessory power: pmu)`. No revert timer. |
| Adapter present | Host role kept. ID re-checked every 5 s. The supply is reported as an online `ACA`: 500 mA, then 2100 mA after the handshake. The charger ramps 500 → 1000 → 2100 mA. |
| Unplug (3 empty reads, 0.75 s) | `device role on`, the gadget rebinds, and the Mac re-enumerates it when the cable goes back in. |
| The user sets the role | The policy backs off (`inhibit`) until the next unplug. |

Readout: `/sys/bus/i2c/devices/0-001a/lightning_state` (world-readable). It shows the
phase, role, ID, handshake, supply, charger and battery. Debugfs
`apple-tristar/auto_log` keeps the last transitions, and `apple-usb-role/regs` shows the
registers.

---

## 1. Boot usb-host5 (Mac cable in)

Keep the recovery console ready ([warm-reboot.md](../boot/warm-reboot.md)). The running
entry must be staged as for the earlier USB trials.

```sh
cd .
export OMARCHY_HOST=172.16.42.1
S=scripts/omarchy/ssh.sh
"$S" 'findmnt -no SOURCE,FSTYPE /; tr " " "\n" </proc/cmdline | grep linux_a9.entry; sudo -n linux-a9-kexec status'
ENTRY=$(cat build/usb-host5/entry-name)
ENTRY="$ENTRY" scripts/boot/kexec-host.sh "$(cat build/usb-host5/payload-path)"
# ...or cold: scripts/boot.sh pongo, then scripts/boot.sh linux "$(cat build/usb-host5/payload-path)"
```

`kexec-host.sh` waits for the new boot ID and runs its NVMe health gate. Then:

```sh
"$S" 'tr " " "\n" </proc/cmdline | grep -cx pcie_apple_h9p.ans_keep_power=1; tr " " "\n" </proc/cmdline | grep -E "linux_a9.entry|debug_shell"; findmnt -no SOURCE /'
"$S" 'cat /sys/module/apple_tristar_power/parameters/{auto_host,auto_input_ua,allow_power_handshake} /sys/module/apple_s8000_usb_role/parameters/usb_ctl_phy_on /sys/module/ehci_platform/parameters/apple_port_budget_ma /sys/class/usb_role/lightning-role-switch/device/accessory_gates'
"$S" 'cat /sys/bus/i2c/devices/0-001a/lightning_state; sudo -n cat /sys/kernel/debug/apple-usb-role/regs | head -4'
```

Expect:
- `1`, `linux_a9.entry=usb-host5-5306b84fa3`, no `debug_shell`, an `/dev/nvme…` root.
- `Y 2100000 N Y 500`, `none [pmu]`.
- `phase: other`, `role: device`, `digital_id: 10 0c 00 00 00 00` (the Mac cable),
  `supply: online 1, type SDP…`, `charger: online=1`.
- `usb_ctl: +00=00000000` (device mode is unchanged).
- The Mac tether stays up. The 10 s GetID with a busy link must not disturb it.

If instead you see `phase: device` with `digital_id: none (last GetID -110)`, the ID bus
did not answer the Mac cable. Note that and continue: it does not affect the adapter
test. **Stop** only if the tether drops repeatedly. In that case collect
`"$S" 'sudo -n cat /sys/kernel/debug/apple-tristar/auto_log; journalctl -k -b | grep -E "lightning:|idbus" | tail -50'`.

## 2. Quiesce the old userspace

The kernel policy replaces the iAP listener and the manual `arm` trials. udiskie would
mount read-write, so mounting is done by the logger, read-only.

```sh
"$S" 'sudo -n linux-a9-usb-host device; systemctl --user stop linux-a9-udiskie.service; systemctl --user is-active linux-a9-udiskie.service; systemctl is-active linux-a9-usb-host-listen.service'
```

Expect `inactive` twice. (`linux-a9-usb-host device` stops a leftover listener and
rebinds configuration 1. With the Mac in, that is harmless.) **Do not run
`linux-a9-usb-host device` while the adapter is attached.** That is a user role change,
and the policy stays in `inhibit` until the adapter is unplugged.

## 3. Wi-Fi control path, iPad logger, Mac watcher

```sh
scripts/periph/usb-host-wifi.sh
export OMARCHY_HOST=$(cat build/omarchy/wifi-host)
scripts/periph/usb-host5-watch.sh install     # prints matching sha256 for iPad and Mac copies
scripts/periph/usb-host5-watch.sh start       # systemd unit linux-a9-usb-host5-log, no deadline
scripts/periph/usb-host5-watch.sh status
```

Start the watcher in the background (`run_in_background`). It follows the iPad timeline
over Wi-Fi and the Mac's own view of the gadget (`/dev/cu.usbmodem*`, ping
172.16.42.1). It keeps running across Wi-Fi drops and has no deadline:

```sh
scripts/periph/usb-host5-watch.sh watch      # log: build/usb-host5/watch-latest.log
```

The iPad logger:
- reads `lightning_state` every second;
- on every phase, role, ID, handshake or charger change, writes a numbered snapshot
  (state, `regs`, `auto_log`, knobs, `lsusb`/`lsusb -t`, `lsblk`, power supplies, kernel
  log) and a timeline line;
- for each new USB partition, mounts it with `-o ro,nosuid,nodev,noexec` on a private
  directory, lists it, and unmounts it.

## 4. Tell the owner (no timing)

> Whenever you're ready: unplug the Mac cable and plug in the camera adapter with its
> charger connected and the flash drive in it. Leave it as long as you like. When you're
> done, unplug the adapter and plug the Mac cable back in. There's no hurry at any step.

Then just read the watcher output. The expected sequence follows. Times are from the
adapter plug; the iOS reference values are in parentheses.

| ~t | Timeline / kernel | Pass signal |
|---|---|---|
| Mac out | `phase=empty`, `MAC: gadget acm=no ecm_ping=no` | — |
| +0.5 s | `device -> handshake: digital ID 11 f0 00 00 00 00 (camera adapter)` | ID read |
| +0.6 s | `power handshake ok (0), try 1`, `handshake -> enter-host`, `host role on` (iOS: host in ~1 s) | `role=host (accessory)` |
| +1–2 s | snapshot `host`: `regs` shows `usb_ctl: +00=00000003`, `tristar … 05=00 0c=a3`, `pmu ldo5: 0310=01 … 2603=01` | the iOS bit 1 landed |
| +1–3 s | `lsusb`: `1a40:0101` hub, `lsusb -t`: `Class=Hub … 480M` (iOS: HS hub) | **hub at HS** |
| +3–15 s | `0781:5581` behind the hub at 480M, `sdX`, `mount check /dev/sdX1: mounted read-only` (iOS: ~18 s) | **drive mounts read-only** |
| +1–4 s | `charger=[input_current_limit_ua=500000 → 1000000 → 2100000]`, `battery=[status=Charging]` (iOS: 2100 mA, USBPower Yes) | **charging in host mode** |
| unplug | `host -> empty: device role on`; `role=device` | port back to device |
| Mac in | `MAC: gadget acm=yes ecm_ping=yes`, then `MAC: GADGET OK - tether SSH answers, same boot …` | **gadget re-enumerates on the Mac** |

Spot checks over Wi-Fi at any time:

```sh
scripts/periph/usb-host5-watch.sh status
"$S" 'cat /sys/class/power_supply/battery/{status,current_now}; lsusb -t; lsblk -o NAME,TRAN,SIZE,FSTYPE,RO,MOUNTPOINTS'
```

## 5. Collect and judge

After the Mac cable is back (or over Wi-Fi at any time):

```sh
scripts/periph/usb-host5-watch.sh pull       # -> build/usb-host5/device-logs/<run>/
scripts/periph/usb-host5-watch.sh verdict
scripts/periph/usb-host5-watch.sh stop
```

Pass criteria (`verdict` prints each one; the overall result is PASS only if all pass):

| Key | Criterion |
|---|---|
| `host_role` | The policy reached `role: host (accessory)` with `digital_id 11 f0 …` |
| `usb_ctl` | `usb_ctl: +00=00000003` while host |
| `hub_hs` | `1a40:0101` enumerated and `Class=Hub … 480M` |
| `drive_ro` | A USB partition mounted read-only, listed and unmounted |
| `charging_host` | Battery `status=Charging` and `current_now_ua > 0` while host (accessory). bq27xxx passes the gauge's signed current through, so positive means charging and negative means discharging (power-supply ABI). |
| `input_2100` | Charger `input_current_limit_ua=2100000` while host |
| `no_suspend` | Charger never `online=0` while VBUS is present, through handshake, enter-host and host |
| `detach_device` | After the unplug: `role: device`, phase `empty` or `other` |
| `gadget_mac` | The watch log has `GADGET OK` (tether SSH, same boot) after the unplug |

Also confirm by hand from `kernel.txt` in the last snapshot: no `insufficient available bus power`,
no `dwc2` errors on the gadget rebind, no NVMe timeouts.

## 6. If something fails: event-driven A/B ladder (adapter stays plugged in)

`reenter` turns the policy off, sets the device role, applies the knobs and turns the
policy back on. The kernel then identifies the still-attached adapter and makes a fresh
host entry. The owner does nothing. Run each step, then read the watcher and `status`:

| Observation in host mode | Next step |
|---|---|
| `usb_ctl +00` is not `00000003` | The fix did not apply. Stop and report the `regs` and `knobs.txt` from the snapshot. |
| `+00=00000003` but `ehci portsc1=00501000` (no hub, `sig=se0`) | `usb-host5-watch.sh reenter accessory_gates=none`, then `reenter accessory_gates=pmu usb_ctl_phy_on=0`, then back to `reenter usb_ctl_phy_on=1`. Report all three `regs` and `lsusb -t`. The next entry would keep the DWC2 device side powered and create EHCI before the PHY host switch (ds-usb-host5.md §6). |
| Hub present, drive `rejected … insufficient available bus power` | `reenter port_budget_ma=900`. Accounting only: the adapter powers the port. |
| Hub at 12M (FS) | Report `lsusb -t` and `regs`. The PHY host trims are suspect. |
| `power handshake failed` twice | Host still comes up at 500 mA (by design). Keep `apple-tristar/power_handshake_log`. |
| `charger online=0` with `vbus 1` in host | Report `power_supply.txt` and `lightning_state`. This is a supply/charger interaction bug. |
| `inhibit` | Someone set the role: unplug and replug the adapter, or `reenter`. |
| `lightning_state` and the timeline stop changing with the adapter attached, `role` stuck mid-switch | The role switch (gadget unbind, EHCI creation, `wait_for_device_probe`) now runs from the supplier's poll work; earlier trials ran it from a sysfs write. A hang there looks like this and is not an electrical failure. Over Wi-Fi: `"$S" 'echo device \| sudo -n tee /sys/class/usb_role/lightning-role-switch/role'`, then `"$S" 'sudo -n sh -c "echo w > /proc/sysrq-trigger"; journalctl -k -b \| tail -150'` (blocked-task dump) and report it. |
| No `GADGET OK` after the Mac is back | `"$S" 'cat /sys/class/usb_role/lightning-role-switch/role; cat /sys/kernel/config/usb_gadget/g1/UDC; journalctl -k -b | grep -E "gadget|dwc2" | tail -40'`. Then `linux-a9-usb-host device` (adapter out). |

## 7. Stop / roll back

- Disable the policy: `"$S" 'echo 0 | sudo -n tee /sys/module/apple_tristar_power/parameters/auto_host'`.
  The device role: `"$S" 'echo device | sudo -n tee /sys/class/usb_role/lightning-role-switch/role'`.
- The kernel refuses system suspend while the accessory holds the host role (`-EBUSY`
  from the role switch). Unplug the adapter first.
- Return to the previous entry: `ENTRY=$(cat build/kexec-irqfix/entry-name) scripts/boot/kexec-host.sh "$(cat build/kexec-irqfix/payload-path)"`.
- Do not enable `allow_power_handshake` from the command line. The policy does not need it.
