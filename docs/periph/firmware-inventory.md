# J71t peripherals firmware inventory

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Round 4:** 13 peripherals firmware files are verified offline. In addition
to the historical 11-file radio/ALS/SmartIO set, `apple/j71t-CBAT.bin` is this
unit's 24-byte resolved `usb-input-limit-calibration` (IODeviceTree line 3262),
and `apple/j71t-BMac.bin` is its six-byte resolved Bluetooth `local-mac-address`
(line 1301, network byte order). Both come from the private local dump, stay
under build/, and are requested directly by the kernel. No identity bytes are
committed. CBAT calibrates source-dependent input limits; BMac installs and
verifies controller RAM identity before the boot-time default-address check.

**Verified on device in Round 3:** the selected FearA USI HCD loaded build
1312 and LE discovery worked. The original selection was inferred from the
verified combo-module vendor; this hardware result validates the selected
image on this unit. ALS also loaded its staged LSCI successfully.

The initramfs has one gzip member containing HoolockRD and the firmware/tools
cpio overlay. Static btmgmt and its license remain included; two touch-agent
firmware files are preserved without extending this workstream's verification
claim. See [round4.md](round4.md) and [test-plan.md](test-plan.md) for current
commands and hashes. The following sections retain original pull provenance.

2026-10-22. All source bytes came from existing local pulls supplied by the
orchestrator. The implementation agent did not contact the iPad. Apple
binaries, device identities, calibration, OTP and their manifests stay under
`build/`; none belong in Git.

## Exact radio selection and evidence

**Verified — this unit:** `build/firmware/private/IOService.txt`, Latin-1 text:

- Line 10350, `HWIdentifiers`: C=4355, s=C0, P=feara, M=YSBC, m=4.1, V=u.
- Lines 3780 and 11191, `ModuleInfo`: chip C0, module YSBC/4.1/u.
- Line 11217, `RequestedFiles`: `C-4355__s-C0/feara.trx`, `feara.txcb`,
  `feara.clmb`, and `P-feara_M-YSBC_V-u__m-4.1.txt` in that directory.
- Line 11210, firmware version 9.44.205.0.3.50.46, FWID 01-9787ef4d.
- Line 11206, platform config `feara-PlatformConfig.plist`. That file was
  not supplied and is not requested by this Linux brcmfmac path.

`pull_firmware.py --ioservice` cross-checks HWIdentifiers against all four
RequestedFiles before selecting anything. Explicit command-line overrides
that disagree are rejected. `build/firmware/selection.json` records evidence
line numbers and the source dump hash.

**Verified — IODeviceTree evidence is narrower:**
`build/firmware/private/IODeviceTree.txt` line 1228 identifies `feara`;
PCI node lines 2757–2795 identify 14e4:43dc, revision 0x0a. This plane alone
has no module/vendor/profile selector. Apple USB `vendor-id=0x05ac` on the
Bluetooth node is not the module manufacturer. The IOService dump resolves
what Round 1 could not establish.

## Staged Linux files

All destinations below are relative to `build/firmware/lib/firmware/`.
The common Wi-Fi source directory is:

```text
build/firmware/sources/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4355__s-C0/
```

| Destination | Source | Size | Selection claim |
|---|---|---:|---|
| `brcm/BCM.apple,j71t.hcd` | `sources/usr/sbin/BlueTool`, named image `BCM4355C0 FearA USI MCC` | 92913 | **Verified on device:** build 1312 and LE discovery; originally selected from combo vendor V=u |
| `brcm/brcmfmac4355-pcie.apple,feara-YSBC-u-4.1.bin` | common dir: `feara.trx` | 733346 | **Verified:** live RequestedFiles and HWIdentifiers |
| `brcm/brcmfmac4355-pcie.apple,feara-YSBC-u-4.1.txt` | common dir: `P-feara_M-YSBC_V-u__m-4.1.txt` | 5346 | **Verified:** exact file requested by iOS on this unit |
| `brcm/brcmfmac4355-pcie.apple,feara-YSBC-u-4.1.clm_blob` | common dir: `feara.clmb` | 8794 | **Verified:** live RequestedFiles |
| `brcm/brcmfmac4355-pcie.apple,feara-YSBC-u-4.1.txcap_blob` | common dir: `feara.txcb` | 602 | **Verified:** live RequestedFiles |
| `apple/j71t-LSCI.bin` | live IODeviceTree `als1@39/alsCalibration`, line 1856 | 72 | **Verified:** this unit's resolved LSCI v2, valid header/checksum and CT819 record fields |
| `apple/j71t-CBAT.bin` | live IODeviceTree `usb-input-limit-calibration`, line 3262 | 24 | **Verified:** resolved unit calibration; driver validates version, points and bounded codes |
| `apple/j71t-BMac.bin` | live IODeviceTree Bluetooth `local-mac-address`, line 1301 | 6 | **Verified:** exact resolved unit identity, unchanged network byte order; RAM programming/readback awaits next boot |
| `apple/s8000-smartio.bin` | kernelcache `com.apple.driver.AppleS8000SmartIO`, `__DATA,__firmware`, offset 0x27a1000 | 487488 | **Verified:** extracted A9 section, already staged in Round 1 |

The four Wi-Fi files each also have an identical `-XX` board alias, for
example `brcmfmac4355-pcie.apple,feara-YSBC-u-4.1-XX.txt`. **13 current peripherals files**, including CBAT and BMac added after Round 2.
No other NVRAM profile or unqualified NVRAM fallback is installed.

**Verified correction to Round 1:** this kernel's `pcie.c` maps the C0
chip revision to `brcmfmac4355-pcie`, without `c0`. C1 has a separate
`brcmfmac4355c1-pcie` mapping. The old staging script produced nonexistent
C0 request names; both the script and regression expectations are corrected.
`brcmf_pcie_prepare_fw_request()` supplies OTP-derived board names when
board type, antenna SKU and valid OTP are present. The PCIe workstream must
supply that integration; these files alone do not enable Wi-Fi.

**Verified HCD extraction:** two identical copies of the selected USI image
occur in BlueTool at file offsets 4827322 and 5013150. Complete HCD command
bounds and terminal command are validated; duplicate candidates are accepted
only if their bytes match. SHA256 of the selected HCD is
`5513d25b9ebd3103263d27a14e709a884508feab5723521d18780cf26f0c24b2`.
The exact `FearA` spelling and the lack of a frequency token in the real
candidate name are handled. No MUR, FearB, RudderB or Olaf image is staged.
The board fallback filename matches `btbcm_initialize()` for `apple,j71t`.
The original shared-module choice was inferred without a BlueTool log; Round 3 Linux build 1312 and discovery now validate this image.

## Private records preserved

The text parser accepts hex NSData and quoted Latin-1 NSData (including this
unit's six-byte printable MAC records). It retains node paths/line numbers,
rejects conflicting records and unresolved `syscfg/`/`macaddr/` references,
and preserves zero-valued resolved records without claiming they are useful
calibration. XML/binary plist inputs remain supported. `--read-ioreg` now uses
`ioreg -l -p IODeviceTree -w0`; it no longer uses the broken iOS 16 XML mode.
Only the orchestrator may use that remote option.

| Private output in `build/firmware/private/` | IODeviceTree property / line | Bytes |
|---|---|---:|
| `LSCI.bin` | `alsCalibration` / 1856 | 72 |
| `MtCl.bin` | `multi-touch-calibration` / 1592 | 1024 |
| `WCAL.bin` | `wifi-calibration-msf` / 1233 | 1780 |
| `BCAL.bin` | `bluetooth-taurus-calibration` / 1319 | 1064 |
| `WMac.bin` | wlan `local-mac-address` / 1234 | 6 |
| `BMac.bin` | bluetooth `local-mac-address` / 1301 | 6 |
| `W24R.bin` | `rx-calibration-2.4` / 1257 | 80 |
| `W50R.bin` | `rx-calibration-5.0` / 1246 | 96 |
| `WRxT.bin` | `rx-calibration-temp` / 1266 | 1 |
| `FG2G.bin` | `freq-group-2g-calibration` / 1230 | 8 |
| `CBAT.bin` | `usb-input-limit-calibration` / 3262 | 24 |

W24R/W50R/WRxT/FG2G are all zero in the supplied dump. PxCl/BTTx/BTRx are
not resolved in this dump and were not fabricated. The raw ADT, IOService
(including OTP/ChipOTP) and NVRAM captures remain private under `build/`.

**Current integration:** ALS consumes LSCI directly; the charger consumes
CBAT; Round 4 Bluetooth consumes BMac directly when DT identity is absent.
Saving WMac/WCAL/BCAL/MtCl is not itself integration. BT RF calibration is
still unimplemented. The charger uses the dedicated source-aware J71t policy,
with no J172 policy selected. See [round4.md](round4.md).

## Reproduce and package offline

From the repo root:

```sh
python3 -B scripts/periph/test-firmware.py
scripts/periph/pull-firmware.sh \
  --source build/firmware/sources \
  --ioreg build/firmware/private/IODeviceTree.txt \
  --ioservice build/firmware/private/IOService.txt
python3 -B scripts/periph/verify-firmware.py
scripts/periph/mkinitramfs.sh --require-btmgmt
scripts/periph/mkinitramfs.sh --check --require-btmgmt
```

Existing differing files are never overwritten by the pull/stager. These
commands are idempotent for the current inputs. `manifest.json` records
source, size and SHA256 for all files copied by that run; the separate
initramfs manifest includes all packaged firmware and userspace files.
All manifests and outputs stay in `build/`.

**Verified current archive:** `build/initramfs-fw.gz` contains the original
HoolockRD cpio as an exact uncompressed prefix, followed by a deterministic
root-owned newc overlay, compressed together into **one gzip member**. The
old Round 2 multi-member construction caused m1n1 to retain only the firmware
archive and was corrected in Round 3. The checker rejects that construction.

Firmware/data modes are 0644, directories/tools 0755, and the optional RAM
identity helper is 0600. Host output is 0600. Current size is 4034852 bytes;
SHA256:

```text
54400b6dbf0cd618d95d2d18757351a01ad44cdb3b300556bda99f1da372c1bf
```

The 18 overlay files are 13 peripherals firmware files, two unchanged
firmware files supplied by the touch workstream, static btmgmt, its license,
and the private RAM identity helper. Both cpio archives and the nested
HoolockRD archive are checked for integrity/path collisions. Current tests
and expected device behavior are in [test-plan.md](test-plan.md).

## Historical touch assets and handoff

**Verified:** `sources/usr/share/firmware/multitouch/J71t.mtprops` has
personality `C1F15,1`, a 57016-byte `Constructed Firmware` value, version
`0x0210.bin`, and `PreconstructedBootloadPacketType=Z2`. That boot-packet
label does not override the kernelcache's selected N1 implementation.
See [touch-recon.md](touch-recon.md) for extraction/recon progress. Touch now belongs to its dedicated workstream, which owns the two additional
staged touch files. This report makes no current touch-driver readiness claim
and leaves `touch-recon.md` unchanged.
