# Running AOP fingerprint investigation — Round 5

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. No device access by the implementation agent. Evidence is the
orchestrator's saved cold capture, pinned firmware files, ADT dumps and the
shared iOS 16.7.10 kernelcache. [E] = read from those artifacts; [I] = inference;
[U] = unresolved. The Round 4 attach/refusal policy is unchanged.

## What the failed run establishes

[E] `build/periph/aop-r4/live/active-20261023T170147Z/`, integration
`b25061d6f8`, driver `9c64a50e99`: powered, running bit set, both FIFOs empty,
no faults, auto-boot 0, Phase A `refused-running-foreign-image`, RX/TX counts 0.
The old `active-patchbay.txt` already retained these windows:

* Region+0 begins `00 00 03 00 00 00 07 00`, whereas the pinned image begins
  `18 00 00 ea fe ff ff ea` (ARM branch vectors).
* Region+0x20 begins `00 00 1b 00 00 00 1b 00`, not `75 75 69 64 05 00 00 00`.
* Region+0x47440 begins `69 95 7e 1a 17 b7 97 1f`, not the printable build text.

[E] Neither complete captured window (96-byte head, 64-byte build window) nor
its first 16 bytes occurs anywhere in the pinned main file. This rules out
simply choosing another file offset for those exact windows. It does **not**
locate the runtime image in hardware. The old tag walker stopped before
printing a malformed first entry, which is why raw words must be reported.

[U] These bytes do not identify a different iOS build. A different mapped
region, a changed/runtime-used aperture, firmware-modified memory, stale
state, or a foreign image remain possible. CPU-running bit 0 alone proves
neither instruction execution nor a protocol-12 implementation. No conclusion
about a power cycle, firmware identity or safe ownership follows from it.

## Rechecked file offsets and iOS layout

[E] Main SHA-256 is
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`, size
342336 (0x53940). There is exactly one `uuid` signature in this artifact, at
0x20, version 5. Its raw UUID is `cce47592d067374e8b46151be730c1de`.

[E] The header's +0x0c field (file+0x2c) is 0x47440, the version-string
offset. The +0x28/+0x2c fields (file+0x48/+0x4c) are 0x389b8/0x1df, the TLV
offset/size. The table has 35 packed tags. The build text is
`ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465`. The 64-byte comparison
window also includes NULs and binary bytes after that string; the new dump
retains all of them. These are offsets in the extracted main image, not the
IM4P container or OSSerialize bundle. `inspect-fingerprint.py` derives them
from the file's header rather than returning the driver's constants.

Rechecked shared Ghidra functions in kernelcache SHA-256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`:

* [E] `RTBuddyFirmware_copyImageToTarget` (`fffffff005b6f340`) allocates/obtains
  a target large enough for max(image length, RTSZ), maps at offset zero and
  copies the image bytes to the mapped base. No added image-header skip or
  image relocation is present in that copy routine.
* [E] Region construction (`fffffff005b69000`, fallback path in
  `RTBuddy_regionsFromSegmentRanges`) reads `region-base`/`region-size` and
  constructs one segment with physical/device base equal to region-base,
  virtual base 0. The saved live nub supplies 0x210e00000/0x9b000; segment
  ranges/names are absent. This agrees with the existing cold-load research.
* [E] `AppleA7IOPV1_setFirmwareRemap` (`fffffff005b4e72c`, also used by V2)
  writes IOP low/high at +18/+20, descriptor size at +28, zero at +30, AP PA
  low/high at +08/+10 and enable 1 at +38. The new driver only **reads** these
  words and reports the values; it never follows them to a guessed mapping.
  [U] Readback semantics have not been measured on this device; zero readback
  alone must not be treated as proof that a remap is disabled.
* [E] `RTBuddyFirmware_publishVersionProperties` (`fffffff005b6f6dc`) uses
  UUID header +8/+0xc for RTKitVersion/FirmwareVersion offsets plus the map
  base. `RTBuddyFirmware_locatePatchBay` (`fffffff005b6fc60`) uses header
  +0x28/+0x2c for v5, then aligns the mapped table range to four bytes. Its
  aligned copy length is 0x1e0 including padding; the declared TLV size stays
  0x1df. That one padding byte cannot explain the missing UUID/build text.

Saved decompilations: `build/periph/aop-r5/ghidra/`; local verification:
`build/periph/aop-r5/identity-report.txt`.

[I] The selected offsets are correct for the image iOS initially loads by
this path. [U] That does not prove the current wrapper still maps that target
or the firmware preserves every loaded byte after starting. This pass does
not establish or exclude a later firmware-side relocation or alias. The next
remap/prefix/UUID/build/raw-TLV snapshot distinguishes those hypotheses better
than weakening a comparison. No blind aperture scan is added.

## Identifying another image safely

1. Capture the new `fingerprint` file, preserving exact bytes and remap words.
   A valid `uuid` header/build string can identify a candidate; the current
   non-text capture cannot. Do not interpret unvalidated tag lengths or
   addresses as permission to read arbitrary memory.
2. Obtain the candidate AOP main/ext artifacts from the relevant iPad IPSW
   on the host. Record their component hashes, main/ext hashes, UUID, header
   version, table layout and build text. Run the local inspector with
   `--image <candidate-main.bin>`; the existing extractor remains pinned to
   20H350 and must not silently relabel another version as that file.
3. [U] If the runtime layout differs, establish the CPU-visible/AP-visible
   mapping and any firmware transformations using that image's loader/code.
   Comparing only the build string cannot establish those mappings, the tag
   schema, protocol ABI, calibration requirements or ext-buffer ownership.
4. Only a separately reviewed identity/layout rule could support that image.
   It must preserve wrapper/fault checks, consistent loader tags, the exact
   corresponding ext bytes in reserved memory Linux cannot reuse, two ping
   ACKs and one HELLO_REPLY followed by EPMAP. An image already owned by a
   previous AP remains refused regardless of its build string.

The current driver accepts no new image or alternate base. Diagnostic output
has no effect on acceptance and cannot trigger a second attempt.

Host-only reproduction:

```sh
python3 -B scripts/periph/aop/inspect-fingerprint.py \
  --capture build/periph/aop-r4/live/active-20261023T170147Z/active-patchbay.txt
```
