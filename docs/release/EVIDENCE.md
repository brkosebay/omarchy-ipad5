# Status evidence ledger — 2026-10-27

Evidence was read from committed workspace history through `21f419d4`.
The export's private manifest records its exact source ref. Workspace IDs
below are evidence references, not commits in the curated public history.
Raw logs, identifiers, sensor patch/frame bytes and captures are not shipped.
No new device access or hardware validation took place during publication prep.

| Claim | Committed evidence | Observation / limit |
|---|---|---|
| Release2 device checks | `5cb8d888`; `docs/release/logs/release2-20261027/20261027T164303Z/result.txt` | Cold DFU run: 13 PASS, 0 FAIL, two warnings, six skips. Covers the bring-up checks, not every subsystem or new kernel series. Later release rounds are in progress, without a new full device-pass claim here. |
| Native display and brightness | `806bd5a6`, `5cb8d888`; release2 `display-slider.log` and owner observation recorded in the commit | ADBE restored as default, eDP-1 brightness slider and scale work. The later rotation fix has separate evidence below. This is display support, not accelerated rendering. |
| H.264 decoding | `4c5ed001`; `docs/test-logs/nextint6-2026-10-24/` | Fixed-mode KMS 31/31 flips; H.264 High/B/weighted/scaling/multislice cases bit-exact. GStreamer 1080p 300/300 frames bit-exact at 52.8 fps and playback in Hyprland. Other codecs unproven. |
| VXD provenance | `4287be27`; [scoped GPL audit](vdec-gpl-provenance.md) | Pinned Imagination/TI GPL source reproduces 544/544 entries and ten compiled objects. Refreshed round-5/6/7 exports retained. Firmware is external. |
| Computer charging | `a6614503`, `26713fd8`; `docs/periph/logs/charge7-release2-20261027/`, `docs/periph/logs/charge8-20261027/RESULT.md` | Computer SDP policy ramps to 2400 mA on tested USB-A and USB-C cable paths. Charge7 recorded +834 mA battery current at 99%. A 2.4 A input limit is not a 2.4 A battery-current claim. |
| Charging round 9 | `deb49402`; `docs/periph/logs/charge9-20261027/RESULT.md` | Cold DFU: Mac USB-A, USB-C wall charger and Mac 96 W-class power adapter reach 2400 mA. The computer USB-C cable path kept an early BC1.2 result and failed; fixed in round 10. |
| Charging round 10 | `33046ced`, `6473859a`; `docs/periph/logs/charge10-20261027/RESULT.md` | Healthy second kexec boot: computer USB-A/adapter and Apple USB-C→Lightning paths reach 2400 mA; enumeration supersedes earlier BC1.2 classification. USB-C wall CC path reaches the 2400 mA board cap. First kexec boot failed the NVMe health gate and was discarded. 5 V only; no USB-PD. Battery at 100%, so input-limit readings do not prove battery current. |
| Additional charger coverage | Owner tests reported in the publication brief, 2026-10-27 | Samsung USB-A fast charger: BC1.2 DCP, 1 A. 5 V/2 A USB-C charger with Default CC: 1 A. Apple USB-A brick untested. These are owner reports, not new release2 checks or committed raw-log evidence. |
| Camera adapter / USB host | `89af4235`; `docs/periph/logs/usb-host-camera-20261027-1853/RESULT.md` | Host role and detach to gadget/USB networking pass. Handshake fails (-19); USB_CTL bit 1 reads back clear, no hub/drive or host charging. Round 6 in progress; still experimental. |
| First Touch ID sensor reply | `ac7763e4`; `docs/sep/logs/tri53-mesa2-status-20261027-1726/RESULT.md` | Single bounded status transfer yields a valid response header, valid=1/spi_error=0. Rails return off. This is sensor transport, not a fingerprint result. Patch/frame bytes were not logged or exported. |
| SEP SBIO end-to-end | `127592bd`; `docs/sep/logs/tri53-mesa4-run-20261027-1827/RESULT.md` | Fresh DFU confirms CPID 0x8003/BDID 0x10. ARTS+xARM gate and SBIO GenericTransfer init 0x73 pass (ret=0, sep_status=0). Patch request 0x5f/type 2 is refused (sep_status=1); no sensor power-on, relay or capture. |
| Earlier SEP transport | `251b3514`, `47bc91d7`, `787d7d9d`, `054bef93`, `b64cbd77` | Eight endpoints discovered, SBIO init passes and GetProtectedConfig returns 32 bytes of unset flags. No matching or enrollment. |
| Cold AOP / orientation | `4af1ef1f`, `7632e1ce`, `211b96a4`, `5cb8d888` | Cold streaming and auto-rotate pass. Earlier stream: 612 reports, zero errors. Board mount matrix retained without per-unit samples. |
| AOP warm boundary | `fa3138eb`, `cb7d3fc1`, `d18a8615` | Channel-3 teardown fixed; warm accel STARTEP still panics. wake8 reset/diagnostics built, not device-tested. Cold success does not establish warm success. |
| Rotation blank fixed | `901c04c8`; [Omarchy patch 0004](../../patches/omarchy/README.md), `docs/omarchy/rotation-blank.md` | Owner A/B on iPad 5, Hyprland 0.56.2 / Quickshell 0.3.2, four rotations each: quarter turns no longer blank for 1–2 s after ScreenMoveRemap limits remapping to actual origin moves. A brief wallpaper rescale remains. |
| Warm boot | `a58c4da3`, `a701aa46` | Shared ANS MSI service-all/rescan correction passes 10/10 gated warm boots. Link-training and post-suspend failure modes are not all closed. |
| Wi-Fi throughput | Owner report in the publication brief, 2026-10-27 | Approximately 300–400 Mbit/s headless on the tested j71t. This is an owner-reported measurement, not a new release2 throughput test. |
| Audio baseline | `a1f491cf` | Built-in 48 kHz S16_LE stereo capture, 3 s, 97% nonzero. Headphone/jack pass not established. |
| CPU baseline | `592ca7e2`, `cef1c7a6`, `893e245a` | Short stock-voltage trials through 2160 MHz; about 19.6–19.7% matrix benchmark gain over 1800 MHz. Round 6 built only; no sustained-stability claim. |
| Suspend baseline | `a1f491cf`, `d3b382f2`, `2929b8a1` | Earlier staged Home-wake/NVMe recovery observed; desktop defaults to screen-off and current suspend changes need their own device plan. |
| OrchardLinux port | `a51efc36`, `21f419d4`; published kernel tip `0dd7ae0ce2` | Only [a9/orchard-j71t](https://github.com/brkosebay/linux/tree/a9/orchard-j71t), on base `6831bc701a`, is the public kernel entry point. Charge10 is included as patch 0013; Orchard kernel/build passes, without a device pass for this exact series. Orchard installer port under review: [hardware#6](https://github.com/OrchardLinux/hardware/pull/6), [cli#2](https://github.com/OrchardLinux/cli/pull/2), [omarchy-a#4](https://github.com/OrchardLinux/omarchy-a/pull/4). Release2 observations do not validate this new series or A9X. |

The only tested unit is iPad6,11 / j71t / S8003. A9X S8001 j127/j128 differs
in silicon and board hardware; [PORTING.md](../../PORTING.md) is the starting
point for independent validation.
