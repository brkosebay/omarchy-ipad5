# AOP runtime mapping and cold-recovery design — Round 7

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Offline only. No device access or workspace commits. [E] denotes
an artifact/instruction observation, [I] an inference, [U] unresolved. This
updates the hypotheses in [Round 6](ds-aop-runtime-layout.md); static evidence
must not be presented as an observed execution of the last iOS boot.

## Outcome and limits

**The running code address and build identity remain unknown. No verified
alternate layout was found, so none is added.** The correct-width Round 6
capture establishes that the existing AP aperture does not contain the
pinned loader layout at offset zero. It does not establish where the image
went, that it was decompressed, or that another physical range is safe to
read. Ext identity/reservation and mailbox ownership therefore remain
unproved, and the driver still refuses before any mailbox traffic.

There is one useful new interpretation: the pinned firmware's own physical
address translator uses the remap words as a **translation delta and inclusive
page bounds**. A larger bound is not a new code-base pointer. The iOS loader
still copies raw main bytes to the fixed region at offset zero in its normal
path. Neither that path nor the late firmware paths examined establish a
pinned second AP-readable image.

The fallback below is a concrete, separate recovery design. **A clean Linux
AOP-only power cycle is not established safe or implemented.** iOS's generic
gate calls are ineffective for this board's empty gate lists; they supply no
PMGR sequence to copy. Linux also has explicit retention and GPIO dependency
constraints. A warm diagnostic boot is supported; a warm restart of the AOP
is not. DFU alone does not supply the missing AON reset/isolation proof.

## 1. What the retained capture actually contains

[E] [Round 6 debugfs capture](../release/OMISSIONS.md), integration
`46dc6de336`, AOP `228f4e3a70`: domains on, CPU status `0x00020001`, idle
`0x79`, no faults, mailbox RX/TX zero. The header is structured data rather
than vectors/UUID, and the complete 64-byte build window differs from the
pinned file. Correct aligned readl32 transactions did not change this result;
the Round 6 access-width hypothesis did not solve the device failure.

[E] The combined committed file is truncated within the fingerprint prefix
line at offset 0x60, immediately before the patchbay section. It contains
110 actual prefix bytes, a complete 96-byte patchbay header (overlapping),
and the complete build window; **no raw tag-table bytes** survive there.
The driver's printed `214/256` difference count is retained evidence, but
the file does not retain all 256 actual bytes. Do not claim to have decoded
the missing prefix/table. The host inspector now reports byte coverage and
conflicting overlapping windows explicitly. New fingerprints end with
`snapshot_end=1`; the orchestrator collector rejects an incomplete new dump.

## 2. Remap: new firmware-side evidence

[E] Main file SHA256
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`.
Addresses in this paragraph are linked firmware addresses, **not AP PAs**.
Function `0x010349f0` takes a firmware VA through `0x0103062c` and
`0x01027e3c`: the latter executes ARM ATS1CPR at `+0x27e4c`, ISB, then reads
PAR at `+0x27e56` and checks the translation-fault bit. Thus the next input is
an IOP CPU physical address, not simply the linked VA.

[E] The virtual callback at `0x0103028c` reads the mapped wrapper's:

* `+08/+10`: 64-bit delta;
* `+18/+20`: lower physical-address bound;
* `+28/+30`: upper physical-address bound.

It compares 32-bit page numbers `(lo >> 12) | (hi << 20)` against both
bounds in the same 44-bit address view, inclusively.
Inside the interval it returns `physical + delta` using adds/adc; outside it
returns the input unchanged. There is no subtraction of the lower bound and
no image-offset search. With the observed zero lower bound, delta
`0x210e00000` and upper `0x1ff000`, its software view covers IOP pages 0..0x1ff.
**The live physical address of the code/build is still an unknown input.**
This decoder does not authorize mapping a 2 MiB AP resource.

[E] iOS `AppleA7IOPV2_mapFirmware`, kernelcache `fffffff005b4e72c`, writes
descriptor length masked with `~0xfff` to +28. That software writer and the
firmware's inclusive bound use do not establish the hardware comparator or
readback/clamping behavior. In particular, do not silently change the cold
loader to size-minus-one or claim a verified hardware capacity of 2 MiB.

[E] Late firmware controller init `0x010302d0` changes wrapper +40/+80c and
clears spare upper halves at +0c/+14/+1c/+24/+2c/+34/+3c in the inspected
sequence. It does not rewrite the low remap fields used above. The object at
file +0x52eb8, with wrapper mapping at +0x52ec0 and another AP IO span
`0x210000000/0xe00000`, is a controller object initialized from WrAd through
`0x01034238..0x01034262` and `0x01030340`; it is not a firmware segment map.
No alternate main-image location follows from those constants.

## 3. iOS target and layout branches checked

[E] Shared Ghidra program: iOS 16.7.10/20H350 iPad6,11 kernelcache SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
The prelink personality `AppleA7IOPV2` matches `iop,s8000`; V1 matches
`iop,s5l8960x`. `iop-version=1` does not select the V1 class. The actual V2
vtable is `fffffff006d3ff98` and its +6a0 entry is b4e72c.

[E] `RTBuddy_getFirmwareTargetMemory` b68cdc chooses region-list element zero
unless `rtb_force_dram` is set. b685a4 reads that boot argument with default
zero. The fixed-region constructor b68df0/b69000 reads the nub's region-base
and region-size; b61d44/b61ff4 build a physical descriptor without a text
offset. Saved j71t values are `0x210e00000/0x9b000`.

[E] Bundle parsing b617c0, serialized entry b6eba8 and data initialization
b6eee4 retain raw OSData. `loadAndPatch` b6f8a4 calls `copyImageToTarget`
b6f340, which maps target offset zero and copies unchanged raw bytes using
aligned u32 writes, with capacity max(image length, RTSZ). The SPU pre-copy
callback b610f8 is RET. No decompression, data prefix or image displacement
was found in these bounded paths.

[E] Segment-backed/preloaded paths exist (b64510, b6ed60/b6eddc/b6ef5c).
AppleSPUFirmwareService start b87340 selects its `__ETEXT/__EDATA` state-3
path only for preloaded firmware. The saved j71t nub has no pre-loaded,
running, segment-ranges or segment-names property. Likewise SRAM access
toggle b66118 requires a nonzero `sram-index`; b6853c/b68764 default it to
zero, and the saved nub lacks that property. These alternatives do not
justify guessed segment offsets, DRAM pointers or extra gate writes.

[U] Historical iOS runtime objects/arguments and any unexamined late writer
are not reconstructed by these static branches. A different image,
post-load changes or another memory configuration remain possibilities.
The data in the current AP aperture cannot yet be assigned to a named
runtime structure or a different build.

Identification needs the original owner's **IOService** properties
`FirmwareUUID`, `RTKitVersion`, `FirmwareVersion` (published by b6f6dc), plus
its target descriptor PA/length, segment list and actual main/ext bundle.
The saved DeviceTree-plane dump lacks that object/map. A future authorized
capture through the owning iOS object could resolve it; none was attempted.
Version text alone never proves an AP resource, ext reservation or handshake
ownership. No free-roaming physical search is proposed.

## 4. Implemented boundary for running attach

Phase A now snapshots the same seven remap words **before and after** the
existing allowlisted head/build/tag reads, also when debug=0. Admission
requires both snapshots to equal the single supported software descriptor:
AP resource `0x210e00000/0x9b000`, words
`10e00000 2 0 0 9b000 0 1`. This is a conservative software whitelist, **not
device verification of the cold-loader readback**. The observed 1ff000 bound
remains unsupported, even if a recognizable build string someday appears.
Instability, another resource/base or another descriptor retains
`refused-running-foreign-image` before ping, ext mapping or HELLO_REPLY.

The frozen fingerprint reports both remap snapshots, the firmware-side
translation view and `alternate_layouts=0`. Debugfs performs no fresh MMIO.
There is no expanded resource, followed physical pointer, new message or
power/reset/control/remap/mask/image/tag write on running attach. Existing
UUID/build/tag/STKG/ext/reservation/ping/one-shot-handshake rules and precise
refusal strings remain. The stopped cold-loader path is unchanged this round.

## 5. AOP-only power-cycle/cold-load fallback design

### What the vendor stop does and does not prove

[E] V2 stop b4b8fc disables outbox/AP interrupts, calls +6d8, setCPURun(0),
+6a8, +6c8(0), +6d0. The concrete targets matter:

* +6d8 → b4c460 returns constant success (optional status ffffffff). This
  apparent idle wait is **not** a hardware idle/DMA-drain proof on V2.
* +6a8 → b4e7c4 is RET.
* +6c8 → b4b6f4 calls the **IOMapper** platform function setActive. It is
  not a PMGR operation; the mapper is obtained in probe b4b0d4.
* setCPURun b4e7c8 changes +44 bit4 and mapped auto-boot. These remain
  forbidden in the inherited-running attach path.
* powerOn b4b630 / powerOff b4b698 invoke generic provider clock/power
  gate operations at index zero and ignore their results.

[E] The saved live j71t AOP provider has `aot-power=1` and **empty**
clock-gates, power-gates and clock-ids arrays. AppleARMIODevice init b9bc178
populates counts +a0/+b0 from those arrays. Its vtable `6ce9f38` slots +568
→ b9bbdac and +570 → b9bbe58 reject index zero when those counts are zero,
before invoking PMGR. Therefore the generic loader supplies **no demonstrated
AOP PMGR off/on sequence**, dwell time or reset/isolation scope.

### Linux constraints

[E] Inspected integration PMGR source at `46dc6de33638ba2a9c6ccde0c06e2a569a0b3c72`.
Topology in s800-0-3-pmgr.dtsi, PMGR-mini base 0x210200000:

| Domain | Offset | Parents |
| --- | --- | --- |
| aop_gpio | 0x80010 | aop |
| aop | 0x80000 | aop_busif, aop_cpu, aop_filter; always-on |
| aop_cpu | 0x80040 | none described |
| aop_filter | 0x80048 | none described |
| aop_busif | 0x80050 | none described |

GPIO pinctrl at 0x2100f0000 consumes aop_gpio, so it and its IRQ/pin users
must be audited and quiesced. genpd always-on and the integration's warm
`keep_on` independently prevent normal off requests. Do not remove global
`apple_pmgr.kexec_keep_on=1`: it also protects the rest of the live system.

> Private graphics-dependent excerpt omitted from this source snapshot.

### Separate recovery state machine, pending hardware proof

1. A new immutable opt-in, distinct from attach, would exclusively select
   recovery. Pin board/chip, AOP-only domain topology and main/ext hashes
   before any mutation. Keep every unrelated domain's warm hold.
2. Acquire provider/genpd and affected consumer ownership; quiesce all AOP
   GPIO/IRQ users. Establish an AOP master isolation/drain mechanism with
   measured semantics. Stopping sensor rate, AP-power=0, AFK shutdown, a
   constant idle callback or a zero PMGR state alone cannot prove this.
3. If the surviving original owner can cleanly stop through its valid
   object graph, prefer that verified handoff. Without that owner this is
   **power-cycle recovery**, not a clean protocol shutdown. CPU-control and
   auto-boot writes require this separate, narrowly scoped recovery contract.
4. A provider-owned transaction would release only the affected AOP holds
   and request PWRGATE=0 in child-before-parent order: gpio, aop, then its
   busif/cpu/filter parents. Check target/actual zero and reset/minimum
   constraints at each bounded step. The physical dependency order among
   the three parents and off dwell need AOP-specific evidence.
5. Restore parents before children, keeping AUTO disabled until a stopped
   baseline is established. Never read AOP MMIO with dependencies off. Once
   all dependencies are active/stable, require CPU-running=0, empty FIFOs,
   no faults and understood retained auto-boot/remap state. Spontaneous CPU
   restart aborts before image writes. Never clear a fault to proceed.
6. Allocate fresh Linux-owned coherent ext and stage the pinned ext; patch
   pinned main TLVs in RAM. Check max(image,RTSZ) against an independently
   proven AP region. Copy/readback main with aligned u32 accesses. Program
   remap in the vendor order: +18/+20, +28, +30, +08/+10, +38. **Resolve the
   bound/readback semantics and verify remap before releasing the CPU.**
   Apply known IRQ masks/outbox setup, CPU +44 bit4 then auto-boot=1, and
   existing bounded HELLO/EPMAP/AP-power/STARTEP/AFK/GET_BUF/rings/calibration/
   10 Hz/IIO bring-up. These writes are part of cold recovery only.
7. One bounded attempt at every transition; no reset/power/HELLO retries.
   On partial failure retain detailed domain state and buffer reservations.
   Do not blindly repower as rollback: it could restart retained old firmware.
   Do not free memory that any surviving master could still address.

Unresolved prerequisites: actual main AP mapping; AOP-only isolation/reset
scope and CPU restart defaults; GPIO pin/IRQ retention and full consumer audit;
PMGR parent ordering/dwell; SRAM/mailbox/remap/watchdog state loss; old DMA
lifetime; failure containment. **No safe cycle command is supplied by the
current evidence.** No recovery parameter or power mutation is added.

A real AON rail power cycle may provide a new baseline, but must be measured;
the present driver still refuses if domains are off. Legitimate domain
startup and isolation need the same separate design. Re-entering DFU or
warm-kexecing does not establish that baseline.

## Evidence retained and checklist

Fresh shared Ghidra outputs: `build/periph/aop-r7/mapping/` and `power/`.
Firmware listings: `build/periph/aop-r7/firmware/`; independent bounded notes
are `re-ios-mapping.md`, `re-firmware-late.md`, `re-power.md` alongside them.
No shared program replacement or device execution was performed.

* Normal iOS copy layout, class selection and SRAM property branch: answered
  statically; not an observation of the last iOS boot.
* Firmware-side remap address conversion: answered statically; hardware
  readback/comparator semantics remain unresolved.
* Actual live code/build/tag/ext locations: **unresolved**; no alternate added.
* Power-cycle fallback: designed with explicit isolation/consumer/failure
  gates; **not safe to implement or run from the present evidence**.
* Running attach: stricter preflight, all refusals retained, no extra AP range.

The payload/test commands and validation result are recorded under Round 7
in [aop-sensors.md](aop-sensors.md).
