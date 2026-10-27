# iOS camera-adapter capture, 2026-10-27

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Read-only ioreg capture on jailbroken iOS 16.7.10 (`scripts/periph/ios-adapter-capture.sh`).
The logger ran on the iPad while the owner swapped Mac cable → camera adapter
(powered, with a SanDisk flash drive) → Mac cable. Raw output is in
`build/periph/ios-adapter-capture/20261027T095244/`. The compact extract is in
`logs/ios-adapter-20261027/` (`NNN-tristar-props.txt` = AppleTriStarBuiltIn
properties at snapshot NNN; `events.txt` = snapshot times).

`dmesg` was not captured: the binary is `/var/jb/sbin/dmesg`, which wasn't on PATH. The script now adds it.

## Timeline (AppleTriStarBuiltIn / IOAccessory properties)

| time | snapshot | state |
|---|---|---|
| 09:52:44 | 001 | Mac cable. DigitalID `100c00000000`, USBModeType 0, ConnectType 2 "Host", CurrentLimit(Base) 2400, ChargingVoltage 5000 |
| 09:53:16 | 020 | unplugged. Detect No, ModeType 4, ConnectType 0, USBPower No |
| 09:53:24 | 025 | adapter in: Detect Yes, **DigitalID `11f000000000`**, InterfaceDeviceInfo `01250180`, MagicPullDown 3, ActivePowerMode 1→4, SleepPowerCurrentLimit 100, ModeType 0 |
| 09:53:25 | 026 | **~1 s later: USBModeType 2, ConnectType 4 "Device", CurrentLimitBase 2100**, AllowedFeatures 65407. The adapter's **USB2.0 hub (1a40:0101, high speed)** has enumerated |
| 09:53:29 | 028 | PowerHandshake Yes, **USBPower Yes, CurrentLimit 2100, ChargingVoltage 5000** (charging in host mode) |
| 09:53:42 | 036 | SanDisk 3.2Gen1 (0781:5581, mass storage, high speed) enumerates behind the hub |
| 09:54:34 | 063 | adapter out |
| 09:54:39 | 066 | Mac back: ModeType 0, ConnectType 2, 2400 mA |

## What it changes for Linux

1. **No device-mode/iAP phase.** iOS goes from the ID-bus DigitalID straight to
   USB host mode (`USBModeType 2`) in about 1 s, and the hub enumerates at high
   speed right away. The "adapter enumerates the iPad first, then iAP vendor
   0x51 → configureUSBMode(3)" path (ds-usb-host.md §3.1, **[I]**) is not what
   happens with this adapter. Our usb-host4 iAP FunctionFS responder waited
   for a SETUP that never comes: the adapter is a hub that waits for the
   iPad to act as host.
2. **Charging continues in host mode.** Power handshake → USBPower Yes with a
   2100 mA input limit at 5 V (the adapter had external power). In Linux, host mode stopped
   charging (owner observation, and ba3ef573 "charging works only in device
   mode"). The host-mode role switch must keep the D2365 input enabled, at a
   2100 mA limit, after the power handshake.
3. Mode 2 here, not the 3 that ds-usb-host.md predicted for a keyboard. Which
   IOAccessoryManager `configureUSBMode` value maps to which PHY/OTG setup
   needs checking against `IOAccessoryManager_configureUSBModeGated`
   (`fffffff005df2228`).
