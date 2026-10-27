# Peripherals Round 2 handoff — ready for kernel probes

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. **Verified offline:** firmware is staged and
`build/initramfs-fw.gz` is built and checked. The implementation agent did
not contact or boot the iPad, and made no host-repo or kernel commits.
Kernel branch/head remain `a9/periph` / `744636325f11`; no kernel changes
were needed. Writes are confined to the authorized peripheral/build paths.

## Next boot

From the repo root, the orchestrator can assemble the payload with:

```sh
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

The implementation agent did not execute that command because its output
`build/m1n1-linux.bin` is outside this workstream's write allowlist. All its
inputs were checked present, and the kernel/DTB provenance is unchanged.
Then use the normal orchestrator-owned tethered boot workflow and follow
[test-plan.md](test-plan.md). Its preparation block is fully offline and
uses actual local paths; its main Linux probe blocks need no BlueZ tools.

## Staging result

See [firmware-inventory.md](firmware-inventory.md) for every file, source,
byte count, evidence line and private-record inventory. **11 files**:

- **Verified Wi-Fi:** the four files actually requested by this unit's iOS
  driver for C0/Feara/YSBC/u/4.1, staged with exact OTP board names and `-XX`
  aliases. Source proof: private `IOService.txt` lines 10350 and 11217.
  C0 uses `brcmfmac4355-pcie`, without `c0` in the filename.
- **Inferred Bluetooth selection:** `BCM4355C0 FearA USI MCC` →
  `brcm/BCM.apple,j71t.hcd`. Vendor inferred from the same combo module's
  verified `V=u`. Extraction is verified: two identical complete BlueTool
  images, 92913 bytes each. Device logs must confirm the patch revision.
- **Verified ALS source/format:** this unit's resolved 72-byte LSCI from
  private `IODeviceTree.txt:1856` → `apple/j71t-LSCI.bin`. Header, checksum,
  CT819 record/leakage fields, gain, integration time and coefficient
  bounds pass offline checks. Sensor behavior remains unverified.
- **Verified preparatory SmartIO:** existing A9 kernelcache section →
  `apple/s8000-smartio.bin`, 487488 bytes. Not consumed by an enabled touch
  driver.

The full unit calibration/MAC records remain only under `build/`.
Only LSCI is integrated as a file input in this kernel. Preserving WCAL,
BCAL, BMac, WMac or MtCl does not implement their loader/driver consumers.

## Packaging and verification

`scripts/periph/mkinitramfs.sh` preserves the original HoolockRD gzip bytes
and appends a deterministic gzip/newc archive of the staged firmware.
The combined output is **3746095 bytes**, SHA256:

```text
fccf05de46501c94a405da8997bd7dc1104ba94a4cb5629a4be100845fbc8bcd
```

**Verified checks:**

- 13 offline synthetic firmware tests pass, including Latin-1/escaped byte
  parsing, conflicting records, live selector/request disagreement,
  manufacturer mismatch, duplicate/truncated HCD and LSCI checksum cases.
- Real staging was repeated with unchanged results. `verify-firmware.py`
  compares all 11 packaged files to their exact sources/evidence.
- Both gzip/newc archives parse; the original base is an exact prefix,
  paths/modes/owners/content match the staged tree, and a repeat build has
  the same hash. GNU cpio independently accepts the overlay.
- Both stages of HoolockRD were inspected. Its nested extraction does not
  overwrite any packaged firmware path. **No BlueZ tools are included.**
- Existing `verify-kernel.py` passes against the unchanged worktree/build:
  required drivers are linked and J71t resources/per-board isolation match.
- Payload input files exist; kernel/DTB hashes match Round 1.

Private manifests: `build/firmware/manifest.json`, `selection.json`, and
`build/periph/initramfs-manifest.json`. Reproduce via the test plan's first
block; no device connection is involved.

## Remaining gates

**No remaining offline blocker to the next battery/ALS/kernel-HCI probe.**
Hardware remains untested, including UART rates, power/reset sequencing,
gauge/PMIC register semantics, ALS response and the inferred BT variant.

Bluetooth scanning/pairing requires BlueZ userspace and a valid controller
identity. HoolockRD lacks BlueZ; BMac loader injection is still unverified.
Kernel `hci_register_dev()` queues initial HCI setup without BlueZ, so
firmware/UART setup can still be observed on this boot. BT RF calibration
is not consumed. Wi-Fi needs the separate PCIe/OTP/NVMEM integration and
the eventual distribution's regulatory data. Touch remains unimplemented.

## Touch progress

**Verified:** J71t.mtprops supplies the matching `C1F15,1` constructed
firmware, 57016 bytes, version `0x0210.bin`, packet-type label `Z2`.
`inspect-touch.py` preserves it with metadata under `build/periph/touch/`;
the actual MtCl record is saved privately. This resolves the Round 1 rootfs
asset search, not the N1 protocol or Linux container.

The shared Ghidra tool catalog responded; two N1 queries timed out after
15 seconds each. No decompilation result was obtained, the server was left
running, and no new protocol claims are made. See [touch-recon.md](touch-recon.md).
