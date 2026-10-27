# omarchy-ipad5

## Status: early and experimental

[omarchy-ipad5](https://github.com/brkosebay/omarchy-ipad5) brings Linux and
[Omarchy](https://github.com/basecamp/omarchy) to iPad 5. The 2026-10-27
release2 device run passed **13/13 checks**. This is evolving source and
research, with your own firmware/calibration required and no kernel binaries.

**Tested: only iPad6,11 j71t (S8003).** j71s/j72s/j72t are untested. Other
A9/A9X devices, including iPad Pro 9.7 (S8001, j127/j128; device trees exist
in the base kernel), are porting targets: see [PORTING.md](PORTING.md).

✅ **Works** = verified on j71t; 🧪 **Experimental** = partial, opt-in or limited;
❌ **Not yet** = unsupported.

| Feature | State | Scope / limit |
|---|---|---|
| Tethered cold boot | ✅ | Host needed each cold boot |
| Warm reboot (kexec) | ✅ | Ten gated boots; recovery limits |
| NVMe install / root | ✅ | Tested write-window fences |
| Display + brightness / auto-brightness | ✅ | Native KMS, 1536×2048; opt-in auto |
| Touch | ✅ | Multitouch |
| Wi-Fi | ✅ | ~300–400 Mbit/s headless |
| Bluetooth | ✅ | Keyboard pairing / reconnect |
| Speakers | ✅ | ALSA / PipeWire |
| Microphones | ✅ | Built-in stereo capture |
| Headphone jack | 🧪 | Unvalidated |
| Battery gauge | ✅ | Read-only telemetry |
| Charging from a computer | ✅ | 2.4 A input limit |
| Wall / USB-C charging | 🧪 | Broken; detection fix in progress |
| Ambient light sensor | ✅ | Calibrated lux |
| Accelerometer / auto-rotate | ✅ / 🧪 | Cold works; warm incomplete; 1–2 s blank |
| H.264 hardware decode | ✅ | Tested profiles; GPL source cleared |
| CPU frequency scaling | ✅ | Stock 300–1800 MHz |
| Overclock | 🧪 | Opt-in 2.16 GHz; short trials |
| Suspend | 🧪 | Opt-in; screen-off default |
| USB gadget networking / serial | ✅ | USB network + console |
| Lightning camera adapter / USB host | 🧪 | Identification works; no hub yet |
| Touch ID | 🧪 | Sensor + Secure Enclave channel up; no enrollment/matching |
| Cameras | ❌ | No validated driver |
| Cellular j72 | ❌ | No hardware tested |
| GPU acceleration | ❌ | Software rendering; work not included. |

[Detailed status](docs/release/STATUS.md) and [evidence](docs/release/EVIDENCE.md)
record the limits. The clean OrchardLinux kernel port is **in preparation**;
release2 results do not validate that new series or another board.

## Install pointers

You need an iPad 5 on iPadOS 16.x, a Mac or Linux host, a USB data cable,
and Docker for kernel builds. Individual scripts may need host-specific tools.
Cold boot is tethered: **palera1n → pongoOS → m1n1 → Linux**;
`scripts/boot.sh pongo` → `scripts/boot.sh linux <payload>` sends a RAM-loaded
payload. NVMe root does not make boot untethered.
Kernel source: [a9/orchard-j71t](https://github.com/brkosebay/linux/tree/a9/orchard-j71t);
`scripts/build-kernel.sh` drives Docker builds; release integration is being prepared.
Firmware comes from a public IPSW; calibration comes from **your own iPad's
boot device tree**. See [provenance and staging gaps](docs/release/firmware-provenance.md).
Root options: [USB-NBD development root](docs/omarchy/README.md) or
[NVMe installation](docs/nvme-install/) — destructive; back up first.
**Never touch syscfg, NVRAM or boot firmware (LLB/iBoot).** DFU cannot restore factory calibration.
The OrchardLinux installer path (`orchard install omarchy-a`) is coming; the port is in preparation.
**Point your AI agent at [AGENTS.md](AGENTS.md) and [docs/](docs/) — the repo is written to be agent-readable.**

## Device safety

Back up protected factory data before the first main-disk write. The tested
j71t fences NSIDs 2–7 and limits NSID 1 writes to an explicit window; identify
the namespace mapping independently on another board. Repartitioning destroys
data. Keep iOS bootable as a reference where possible.

Keep probes bounded and opt-in: unpowered MMIO reads can hang the SoC.
Never read an empty SEP outbox. Coordinate one device operator at a time.
Preserve a live USB-NBD root's host server: stop only processes you started,
never all QEMU processes by name. Obtain firmware and calibration from your
own device; no Apple binaries or personalized inputs are distributed.

## Credits and licensing

Original work is **GPL-2.0-or-later** ([LICENSE](LICENSE)); kernel changes
are GPL-2.0. Preserve existing SPDX expressions, authorship and upstream
terms; see [LICENSING.md](docs/release/LICENSING.md) and
[snapshot scope](docs/release/OMISSIONS.md).

Thanks to [HoolockLinux](https://github.com/HoolockLinux),
[Paul Praschl / Pauli1Go](https://github.com/Pauli1Go/HoolockLinux),
[Asahi Linux / m1n1](https://github.com/AsahiLinux/m1n1),
[palera1n](https://github.com/palera1n/palera1n),
[pongoOS](https://github.com/checkra1n/pongoOS),
[OrchardLinux](https://github.com/OrchardLinux),
[Omarchy](https://github.com/basecamp/omarchy), Arch Linux ARM, Hyprland and
[Project Cascadia](https://github.com/rzxvx/Project-Cascadia).
