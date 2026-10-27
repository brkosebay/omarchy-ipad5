# AOP runtime layout and identity — Round 6

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Offline investigation of the committed
[`aop-r5-fingerprint-warm-1.txt`](../release/OMISSIONS.md), the pinned
main/ext files, and the iOS 16.7.10 kernelcache. No device access. [E] means
an artifact/instruction observation, [I] an inference, [U] an unresolved fact.

## Result

Two preflight defects are established: **the AP aperture access width** and
**mutable words incorrectly included in build identity**. Fix those before
moving reads to another address. The remap still names AP PA 0x210e00000;
the +0x28 readback alone does not establish a different DRAM image. The old
snapshot cannot yet be assigned to a firmware structure or another build.

Round 6 reads the same statically described AP region with aligned 32-bit
transactions, compares the exact pinned build text including NUL, and checks
its runtime guard against STKG. It does not expand the resource, follow an
IOP/DRAM pointer, or relax any ownership, reservation, fault, tag or handshake
refusal. **Safe attachment to an already-owned iOS instance is not proven.**

## 1. Vendor access contract, not ordinary memcpy

[E] In kernelcache SHA-256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`:

* `RTBuddy_memcpy_from32` at `fffffff005b63838` checks source/length alignment
  and loops over `ldr w8,[x1],#4` / `str w8,[x0],#4`.
* `RTBuddyFirmware_readRange32` at `fffffff005b6fb78` resolves the mapped
  range and invokes that primitive. UUID discovery (`b6f7e8`), patchbay
  copying (`b6fe50` → `b6fbc4`) and version-text reads (`b6f6dc` → `b6f528`
  → `b6f5c8`) all use it. The text helper reads aligned first/last words,
  then selects bytes **in local RAM**. The TLV's declared 0x1df bytes are
  fetched as an aligned 0x1e0-byte bus range, not 479 byte accesses.
* `RTBuddy_memcpy_to32` at `fffffff005b637d8` similarly checks alignment and
  writes u32s. Main-image copy (`b6f340`) and tag writeback (`b6f9b4`) use it.

[E] Our actual Linux tree's `lib/iomem_copy.c` implements `memcpy_fromio`
with raw 64-bit reads when CONFIG_64BIT is set, and byte accesses for the
tail. `memcpy_toio` similarly uses wider writes. This does not match the
vendor's explicit 32-bit aperture contract.

[I] Wrong-width access is a concrete candidate for the structured/random
bytes in the R5 snapshot. [U] Static analysis does not prove that corrected
readl transactions will recover the expected bytes on this running instance.
The next capture must measure that; do not label the R5 bytes as decoded
runtime tables or evidence of another firmware solely from their appearance.

## 2. Initial iOS target and the remap readback

[E] The actual AppleA7IOPV2 vtable is `fffffff006d3ff98`; its +0x6a0 entry
points to `fffffff005b4e72c` (pointer at `fffffff006d40638`). The source assert
also names V2. The old shared name referring to V1 was misleading and has been
corrected to `AppleA7IOPV2_mapFirmware`. It writes supplied IOP origin at
+18/+20, descriptor length masked with ~0xfff at +28, zero at +30, descriptor
AP PA at +08/+10 and enable 1 at +38. There is no software inverse-mask or
size-minus-one encoding in this writer.

[E] The capture's +08/+10 still decode to **0x210e00000**, and +18/+20 to zero.
[U] Why +28 reads 0x1ff000 is not established: hardware readback/clamping or a
later writer are possibilities, not findings. No corresponding size-readback
decoder was identified in the inspected paths. Do not interpret the value as
authority to map an additional 2 MiB aperture or another DRAM target.

[E] The ordinary serialized OSData constructor (`b6eba8` → `b6eee4`) initializes
the raw image object. `loadAndPatch` (`b6f8a4`) invokes `copyImageToTarget`
(`b6f340`) unless preloaded/existing-target state suppresses it. The copy
maps its target at offset zero and writes the unchanged raw main bytes through
`memcpy_to32`. On the fixed-region path, `b68cdc`/`b69000` use the saved j71t
nub's 0x210e00000/0x9b000 region. The service's pre-copy callback +0x550
(`b610f8`) is a single RET. No hidden header skip or relocation was found in
those examined paths.

[E] Descriptor/segment-backed and preloaded constructors also exist. Their
existence does not establish that this j71t OSData load used them. The DT
`running` flag skips patch/version work; hibernation restoration assumes a
surviving AP object graph and buffers. Neither is a stateless ownership-transfer
operation. These are bounded static findings, not a replay of the old iOS boot.

## 3. Firmware virtual, code and data layout

[E] The pinned main file is 0x53940 bytes, SHA-256
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`.
The reset stub at file+0x68 derives its execution backing P from the PC and
constructs page tables. No text-copy relocation was found in +0x68..+0x4a4.
Linked VA **0x01000000 is not another AP physical address**.

* [E] Text-side linked VA 0x01001000..0x01039dec maps onto P+0x1000. The boot
  pages are mapped separately, and VBAR later selects linked +0x4e0. The
  patchbay at linked 0x010389b8 lies in this text-side span.
* [E] Writable VA 0x0103a000..0x0106d368 uses optional backing D from the u32
  at image+0x58. With the pinned field zero, D defaults to P+0x3a000. The
  build text at linked 0x01047440 is in this writable span. No writer to
  header+0x58 was found in the bounded iOS constructors/load callbacks examined;
  that is not a whole-kernel absence proof.
* [E] Early data relocation exists (`0x01027644`, DSRL/DSVS/DSCL), but the pinned
  DSRL/DSVS/DSCL values are zero; DSRL=0 skips it. Optional BSS-zeroing tags
  also default to zero, selecting the ordinary 0x01053900..0x0106d368 interval.
  None of these observed default writes explains replacing the UUID/tag bytes.
* [E] Ext data is staged separately in AP DRAM by `b88090`; `b88238` records
  its PA in EC0p. Firmware initialization copies the EC0p/ED0p values into a
  runtime object near linked 0x01047510. The ext's linked 0x0106e000 address
  must not be treated as an AP physical pointer.

[U] P/D in the CPU's address space are not independently measured AP physical
addresses. A read32 capture is needed to confirm this instance's header and
tags. A non-default split-data header or relocation tag remains refused; this
driver does not follow that metadata to an unvalidated AP address.

## 4. Build text versus runtime globals

[E] The build identity is 49 bytes at file+0x47440 followed by NUL at +0x47471:
`ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465`. The old 64-byte comparison
also included unrelated writable state:

* +0x47474: stack guard, file default 0x00000aff. C entry +0x27412 calls
  0x01000b5c, which copies the four-byte patched STKG tag into that global.
* +0x47478: cursor/state, file default 0x80. Function near 0x010081dc reads it,
  clears it at +0x822c and writes an updated cursor at +0x8254. This is not
  immutable version text.

Thus a legitimate initialized image could fail the old 64-byte memcmp even
after a correct aperture read. Round 6 compares the exact 50-byte string+NUL,
and separately checks runtime guard == STKG **after** validating the pinned
tag layout/length. A changed build byte or NUL still yields foreign-image;
inconsistent STKG/guard yields tag-mismatch. The remaining adjacent globals
are retained in the diagnostic dump and are not used as version text.

Unknown image/header, pristine or inconsistent loader tags, stale/unreserved
ext, foreign/no ping ACK, and an owned handshake all retain their refusals.
There is no substring-based acceptance, unknown-build fallback or physical scan.

## 5. What can safely stop an inherited instance?

[E] The supported iOS provider stop disables outbox/interrupt handling, waits
for idle, invokes setCPURun(0), and changes active/power state. Its +0x6b0
target (`b4e7c8`) clears +0x44 bit 4 and auto-boot when configured. Those writes
are prohibited in this Linux attach path. The known management vocabulary has
no demonstrated stateless CPU-stop/ownership-release/reload request.

AP-power=0, rate=0 and AFK shutdown concern an AP/endpoint's existing objects;
they do not prove an unknown-owned core is stopped or its old DMA is gone.
They cannot justify overwriting the running image or adopting old rings.

**No safe force-attach or stop-and-cold-load route is established under the
current prohibitions.** Correct-width identity is the next non-destructive
step; even if it matches, the reserved-ext and one-shot handshake proofs are
still mandatory. A completed iOS handshake remains refused. On that result,
stop; do not retry management words, reset, gate power or rewrite remap/tags.

[I/U] A real AON rail power cycle, or a separately verified clean stop by the
original iOS owner, could produce a stopped CPU. DFU/AP reset/kexec is not that
proof. The existing cold loader still requires running=0, empty outbox, no
faults and all domains already ON. If a real power cycle leaves domains OFF,
legitimate provider/genpd startup needs a separate design; this change never
forces them on. It does not promise a usable post-power-cycle boot today.

## Evidence and verification

Detailed bounded investigations: `build/periph/aop-r6/re-ios.md` and
`re-firmware.md`; fresh Ghidra decompilations in `build/periph/aop-r6/ghidra/`;
ARM/Thumb listings `firmware-startup.asm`, `firmware-c-entry.asm`,
`firmware-memcpy.asm`, `firmware-build-tail.asm` alongside them. Ghidra names
for the 32-bit primitives, range reader and actual V2 mapper were saved.
These are static observations, not device execution traces.

Host tests exercise the actual copy helpers with a bus recorder (only aligned
u32 accesses, 479→480-byte tail coverage, no out-of-range access or partial
write), exact build+NUL checks, randomized STKG/global consistency, mutable
cursor acceptance, and all existing foreign-image/tag/ext/handshake cases.
The next device run must still verify the corrected reads and cannot be
predicted from a host test.
