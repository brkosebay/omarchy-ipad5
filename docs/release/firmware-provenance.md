# Firmware and calibration provenance — j71t, 2026-10-27

This source snapshot distributes no Apple firmware or per-device inputs.
The private host-only audit used iPad6,11 / j71t / S8003, iPadOS 16.7.10
(build 20H350). Its findings do not establish another device's firmware
layout, calibration, supported build or signing status.

## Sources and remaining implementation gaps

| Input | Source on your own setup | State |
|---|---|---|
| BCM4355 Wi-Fi firmware, CLM, txcap and board NVRAM text | Public IPSW OS filesystem, AppleBCMWLAN firmware directory | Files identified; select the correct module/board variant |
| Bluetooth patchram | BlueTool in the IPSW OS filesystem | FearA USI record choice is inferred; validate on your board |
| N1 multitouch firmware | IPSW `J71t.mtprops` | Board-specific firmware extraction |
| SmartIO audio firmware | Matching IPSW kernelcache | Local extraction; no blob shipped |
| AOP firmware | IPSW AOP IM4P | Build-specific unwrap/bundle split; pins must follow the chosen OS build |
| VXD DEVA firmware | Matching IPSW kernelcache | Local extraction; no blob shipped |
| Wi-Fi/BT MACs, touch/ALS/charging/AOP calibration and boot identity | Your iPad's resolved boot Apple DeviceTree | Device-specific; never copy another unit's values |
| VXD CAVLC source table | Pinned Imagination/TI GPL source | Not Apple firmware; [provenance resolved](vdec-gpl-provenance.md) |

The boot ADT exposed by m1n1 contains resolved factory properties on the
tested boot chain. The IPSW DeviceTree template alone contains placeholders
and is insufficient for calibration. The private audit compared the retained
calibration inputs against the live boot ADT; they matched on this unit.

**This is a provenance result, not a completed extraction pipeline.** Current
staging scripts still derive several calibration files from a locally
obtained iOS text `ioreg` dump. Wi-Fi calibration/MAC fixups already pass
through m1n1, but a general first-boot ADT-to-firmware generator is not wired
up. Public IPSW plus your boot ADT is the intended installer path; do not
assume every extraction/staging step is automated today. The OrchardLinux
installer port is under review.

Use the matching IPSW/build and subsystem staging tools under
[scripts/periph](../../scripts/periph/), [scripts/touch](../../scripts/touch/)
and [scripts/sep](../../scripts/sep/), together with their prerequisites.
Some historical extractors depend on omitted private tools; the public tree
is a research input rather than a complete installer. Keep all generated
firmware/calibration in ignored `build/` and the device's local firmware
storage, outside public source/images. Never read/write factory namespaces
as an incidental extraction step; protect syscfg, NVRAM and boot firmware.

See [PORTING.md](../../PORTING.md) and [omissions](OMISSIONS.md). There was no
device access during this publication preparation, and no new signing or
remote-download availability check is claimed here.
