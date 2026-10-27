# Status by subsystem — 2026-10-27

Only **iPad6,11 / j71t / S8003**, with iPadOS 16.7.10 as the reference OS,
has device evidence. Other A9 boards and A9X S8001 j127/j128 are untested.
The committed release2 device run passed **13/13 checks** (two warnings,
six skips). See [EVIDENCE.md](EVIDENCE.md) for the scope of each result.
No hardware tests were run during this snapshot preparation.

Public kernel source: [a9/orchard-j71t](https://github.com/brkosebay/linux/tree/a9/orchard-j71t),
the clean OrchardLinux port on base `6831bc701a`, **in preparation**.
The release2 observations below are not a device pass for that new series.
Historical patch exports have separate bases and are not an installation stack.

GPU: research ongoing privately, not included; software rendering only.

| Subsystem | State | Tested scope / remaining limit |
|---|---|---|
| Cold boot | Works | Tethered palera1n → pongoOS → m1n1 → Linux; RAM-loaded boot payload. No persistent bootloader; internal root does not remove the tether. |
| Omarchy / Hyprland | Works | Software-rendered desktop; tablet gestures, on-screen keyboard, USB networking and serial console. |
| Native display / brightness | Works with limits | Native ADBE/eDP-1 display, brightness slider and scale checked on release2. Earlier fixed-mode KMS flips passed 31/31; full modesetting and display power management are not established. |
| Auto-brightness / ambient light | Works | LP8559 backlight and CT819 ambient-light path, opt-in daemon. |
| Multitouch | Works | N1 firmware and per-device MtCl calibration; evdev multitouch. New suspend IRQ changes need separate validation. |
| Wi-Fi / Bluetooth | Works | BCM4355 association, scan/pairing and tested keyboard; firmware/identity must come from your unit. |
| Audio | Works with limits | Speakers, PipeWire and built-in stereo microphone capture tested. Headphone routing/jack validation remains incomplete. |
| PCIe / NVMe | Works | ANS root and tested installation/write-window fences. Protect factory namespaces independently on another board. |
| Warm kexec / panic recovery | Works with limits | Shared-NVMe-IRQ correction passed ten gated warm boots. Earlier link-training/post-suspend hangs are not universally resolved; panic recovery cannot recover every hang. |
| H.264 / VXD | Works | Tested High/B/weighted/scaling/multislice cases bit-exact; earlier 1080p 300/300 frames at 52.8 fps. GPL table provenance cleared; DEVA firmware remains external. No all-codec claim. |
| Battery gauge | Works | Read-only bq27540 telemetry; gauge writes disabled. |
| Charging | Partial | Computer SDP policy reaches 2400 mA; release2 and charge7/8 device runs verify it. USB-C wall charging remains broken: charge8 ended at 100 mA with battery discharging. Round 9 is in progress, with no passing device result recorded. |
| SEP / Touch ID | Partial | Eight endpoints, GenericTransfer SBIO init and GetProtectedConfig work. First valid Mesa sensor status reply on 2026-10-27. Later SBIO init again passes, but patch fetch is refused; sensor remains off. No fingerprint capture, matching or enrollment. |
| AOP / auto-rotate | Partial | Accelerometer streaming and auto-rotate after cold boot. Warm reload still fails; wake8 was built, not device-validated. |
| Rotation blanking | Known issue | Native display rotation can blank for 1–2 s. A proposed userspace change is not a verified fix. |
| CPU / buttons / RTC | Works | SMP, AIC, UART, GPIO, WDT, PMGR, I2C and stock 300–1800 MHz DVFS. Short optional 2.16 GHz trials on S8003 are not sustained-stability or other-board evidence. |
| Suspend / idle | Partial | Desktop defaults to screen-off. Earlier staged s2idle/Home-wake passes do not validate all current paths; CPU idle uses WFI. |
| Lightning camera adapter | Partial | Identification and automatic role selection work; hub enumeration and host charging are incomplete. |
| Cameras / cellular | Not yet | No validated camera driver; no cellular board tested. |

See [PORTING.md](../../PORTING.md), [docs map](../README.md) and
[omissions](OMISSIONS.md). Obtain your own firmware and calibration.
