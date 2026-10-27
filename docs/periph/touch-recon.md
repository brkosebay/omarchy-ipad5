# J71t touch reconnaissance

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Offline only. No device, UART, SSH, telnet, serial, or boot access.

## Round 3: N1 calibration and shared SPI primitives

**Verified in Ghidra, not on hardware:** the selected N1 path shares some
Z2/HBPP primitives. It still is not the J172 HBPP14 implementation. The
following narrows the required Linux N1 work beyond Round 2's asset search.
All raw decompilation/memory responses are under
`build/periph/ghidra-round3/`; no Apple code bytes are copied into the repo.

### Method identities and calibration sequence

- `fffffff005ea8830` is `MTSPIBootloader_N1::performCalibSeq()` (unique
  diagnostic xref). It is now named/commented in the shared Ghidra session.
- `fffffff005ea8ab0` initializes N1 controller-register defaults;
  `fffffff005ea8b00` reads N1 property overrides, then invokes inherited
  property loading at `fffffff005ea6a50`.
- N1 bootloader instance-vtable base is `fffffff006e13a18`. Its calibration
  slot +0x118 points at `fffffff005ea8830`; instance constructor/destructor
  xrefs are recorded in `n1-vtable-base-xrefs.json`.
- N1 SPI instance construction sets vtable `fffffff006e12fe8`
  (`n1spi-new-instance.c`). Slots +0x928/+0x930 are register read/write;
  +0x920 obtains a 32-bit register result; +0x908 is the 16-bit ATN_ACK.

**Verified calibration order**, combining constructor defaults, property
loads, the C1F15,1 personality and the N1 routine:

1. Read touch-controller version register **0x10008ffc**.
2. Write `fll-mval` to `fll-mval-addr`: for C1F15,1,
   **0x10003060 ← 6089**.
3. Write reference-clock divider: defaults **0x1000305c ← 0x20**.
4. Write **0x10003058 ← 6**.
5. Write constant calibration selector: defaults **0x10003000 ← 3**,
   except controller version **0x434d11a0** selects value **2**.
6. If configured, enable clk32: C1F15,1 supplies
   **0x10003518 ← 1**.
7. Read **0x10003800** for the `SPI_APU_EN` diagnostic.
8. Send a two-byte transfer containing the in-memory 16-bit value
   **0x011f**, then sleep **65 ms**.
9. Perform the inherited two-byte HBPP ATN_ACK transaction. This routine
   checks its transport return status, but does **not** validate the
   returned 16-bit status word. A successful transfer is not proof of
   successful physical calibration.

Each register-write call is checked for a nonzero result before continuing.
The version read itself has weak failure reporting in Apple's wrapper.
These addresses are in the touch controller, not AP MMIO or PMIC space.
The selected personality overrides the constructor's original FLL address
0x1000300c; copying only constructor constants would be wrong.

### Verified shared packet construction

`fffffff005ea2564` (now `mtspi_read_register32`) is reached through the
actual N1 SPI vtable. Ghidra instructions independently confirm it returns
a 32-bit result in W0; the initial decompiler wrongly inferred `void`.
Its prototype was corrected in the shared session.

The eight-byte transmit buffer before the SPI call is:

```text
16-bit command value 0x731c
address[15:8], address[7:0], address[31:24], address[23:16]
16-bit big-endian additive checksum over those four address bytes
```

The initial 8-byte full-duplex transfer is followed by the virtual +0x920
result transaction. `fffffff005ea248c` implements that transaction using
an 8-byte constant at kernelcache VA `fffffff00547ec70`; on transport
success it reconstructs the register value from response bytes as
`rx[4]<<24 | rx[5]<<16 | rx[2]<<8 | rx[3]`.

`fffffff005ea6cf8` is the additive 16-bit byte sum, **not a CRC**.
`fffffff005ea23c0` implements the two-byte ATN_ACK with in-memory TX word
**0xa11a** and a byte-swapped 16-bit response. These are buffer layouts and
call lengths verified from code; AppleARMSPI word size/bit order and CS
handling still need tracing before asserting the observed physical wire
format. The register-read wrapper ignores some transport errors and can
return its zero-initialized result; Linux should preserve explicit errors.

`fffffff005ea75e8` is the N1 wake override. It constructs a 16-byte
MT_SPI_Z2_WAKE_CMD buffer, uses a 2 ms delay, and suppresses repeated wake
transactions within roughly 100 ms. Its buffer offsets must be checked
against instructions before making a Linux packet builder.

### Firmware/calibration upload remains a separate step

The N1 bootloader vtable contains the shared Z2 boot routine
`fffffff005ea47f4`. That routine calls virtual slots +0x150, +0x148,
+0x140, +0x120, +0x118, then +0xa0, gates each stage on success, disables
its temporary transport mode on both paths, and sleeps 40 ms on success.
N1 overrides +0x118 with the calibration routine above.

**Exact analysis gap:** the current Ghidra database does not define
functions at the other relevant virtual targets:

```text
+0x150  fffffff005ea5bf0
+0x148  fffffff005ea55e8
+0x140  fffffff005ea52d0
+0x120  fffffff005ea4bf4
write-register slot +0x930  fffffff005ea268c
```

The available decompile/function-disassembly tools return
FUNCTION_NOT_FOUND for these addresses. The pointer targets are identified;
their bodies need function definitions before continuing through this MCP.
Do not label those virtual stages as upload/reset/calibration-data operations
merely from their positions. Their exact packet framing, CS lifetime,
checksums, retries, firmware execution and MtCl/PxCl insertion are still open.

### Updated implementation sequence

1. Define the missing virtual-target functions in the shared analysis and
   reconstruct them in the order above. Start with write-register and the
   8-byte result ACK constant, then map the firmware and calibration-data
   stages. Verify all constructor/personality overrides and error paths.
2. Decode the 57016-byte `J71t.mtprops` constructed asset into a bounded
   host-side container/packet sequence. Keep the original asset and the
   unit's 1024-byte MtCl private. Determine how the inherited path handles
   the absent PxCl; do not fabricate one or assume it is mandatory.
3. Port A9 SIO/SPI/clock/analog power separately. Implement explicit
   register-read/write/ATN_ACK helpers with byte-order fixtures from the
   verified layouts, typed errors, bounded waits and clean reset unwinding.
4. Bring up controller identity reads first. Only after confirming the
   register interface should an owner-run test attempt a bounded RAM upload,
   real MtCl transfer, the N1 calibration order above and firmware execute.
5. Discover report descriptors and map parser type 1/options 16 before
   contact parsing. Then test bounds/orientation, contact release and reset
   recovery. No touchscreen node is enabled by Round 3.

## Round 2 asset evidence

**Verified from the orchestrator's live-device pull:**
`build/firmware/sources/usr/share/firmware/multitouch/J71t.mtprops` contains
the matching `C1F15,1` personality with:

```text
Constructed Firmware              57016 bytes
Constructed Firmware Version      0x0210.bin
PreconstructedBootloadPacketType  Z2
ResetInterval                     432000 (units not established here)
```

The raw constructed asset is now preserved for analysis, without conversion:

```sh
python3 -B scripts/periph/inspect-touch.py
```

Output: `build/periph/touch/j71t-c1f15-1-constructed.bin`, SHA256
`98f73566489fed958a04bb73b87f63d8abd6948b7ceacd89d1bc638e64b8998d`.
The adjacent private `manifest.json` records the original plist hash and
metadata. This is an Apple binary and stays under `build/`.

**Verified calibration:** live text IODeviceTree line 1592 supplies a real
1024-byte `multi-touch-calibration`, preserved as
`build/firmware/private/MtCl.bin`. No resolved PxCl was present. Neither this
asset nor MtCl is installed in the next-boot firmware archive as a Linux
touch container. The existing SmartIO blob remains preparatory.

**Inferred:** the plist's `Z2` boot-packet label may describe a framing
format shared with the selected N1 bootloader. It does not establish that
J172's HBPP14 loader or Linux `apple_z2` sequence applies to J71t. Compare
the constructed bytes with N1 download/calibration methods before writing
a converter. The old statements below that the exact rootfs asset was
unknown are historical Round 1 findings, superseded by this section.

**Verified recon limitation:** `scripts/ghidra.py tools` succeeded, but the
following two bounded queries each ended in `TimeoutError: timed out`
with a 15-second client timeout and no result body:

```sh
GHIDRA_MCP_TIMEOUT=15 scripts/ghidra.py list_strings filter=performCalibSeq limit=20
GHIDRA_MCP_TIMEOUT=15 scripts/ghidra.py search_functions_by_name query=MTSPIBootloader_N1 limit=40
```

No N1 decompilation, address rename or new protocol claim is made in Round 2.
The shared server was left running. Resume those queries when its initial
analysis completes; match the five controller addresses in the personality
below, then determine upload framing, calibration insertion and runtime
report discovery. A9 SIO/SPI/clock/analog power and N1 runtime remain open.

## Result

**Verified:** J71t selects Apple's **N1** SPI touchscreen implementation,
`AppleMultitouchN1SPI` / `MTSPIBootloader_N1`, personality `C1F15,1`.
Calling this simply an older Z2 device conceals a material distinction.
**Inferred:** some runtime framing may still be shared with Z2, but this
needs packet-level verification. The J172 driver's HBPP14 bootloader and
report initialization are not a justified substitute.

No touch DT node or driver is enabled by the peripheral patch series.

## Evidence and reproducibility

```sh
python3 -B scripts/periph/inspect-kernelcache.py --stage-smartio
```

> Private graphics-dependent excerpt omitted from this source snapshot.

**Verified — kernelcache identity:** SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.

**Verified — relevant kexts in the prelink dictionary:**

- `com.apple.driver.AppleMultitouchSPI`, executable at KC file +`0xb5a500`;
  `__TEXT_EXEC,__text` at +`0x1570000`, size `0x14eac`.
- `com.apple.driver.AppleMultitouchSPIC1F15`, codeless plugin, path
  `/System/Library/Extensions/AppleMultitouchSPI.kext/PlugIns/AppleMultitouchSPIC1F15.kext`.
  It supplies the actual `multi-touch,j72` personality rather than executable
  firmware. Do not try to extract a nonexistent executable from this entry.
- `com.apple.driver.AppleMultitouchDriver`, the higher-level report/input
  stack, distinct from the SPI transport and bootloader.
- `com.apple.driver.AppleS8000SmartIO`, with A9 SmartIO firmware at KC
  +`0x27a1000`, size `0x77040`; see the firmware inventory for its hash.

**Verified — selected personality:**

```text
IONameMatch                 multi-touch,j72
IOProviderClass             AppleARMSPIDevice
IOClass                     AppleMultitouchN1SPI
mt-merge-personality        C1F15,1
reset-deassert-delay        15 (units not encoded in the personality)
parser-type                1
parser-options             16
touch-size-id              3
fll-mval                   6089
fll-mval-addr               0x10003060
clk32-clock-enable-addr     0x10003518
clk32-clock-enable-val      1
fw-execute-addr             0x10003400
cal-dl-addr                 0x10009000
prox-cal-addr               0x10009600
```

These are **touch-controller address-space values**, not AP physical MMIO.
They are not permission to write through `/dev/mem`.

**Verified — transport strings in the selected executable:**
`AppleMultitouchN1SPI`, `MTSPIBootloader_N1::performCalibSeq()`,
`Detected N1 Version: 0x%08X. Writing to registers`, `SPI_APU_EN`,
`sending REQ_WAKEUP`, `sending MT_SPI_Z2_WAKE_CMD`, and separate
`MTSPIBootloader_Z2`/`AppleMultitouchZ2SPI` implementations. The shared binary
contains both implementations; the presence of a Z2 string alone does not
prove the selected N1 wire protocol. The same applies to its HBPP strings.

## A9 versus the A10 J172 implementation

**Verified — ADT comparisons** (`docs/adt/j71t.txt:1963–1983,2233–2263`
and `j172.txt:2237–2260,2520–2555`):

- Both use SPI3 MMIO `0x20a08c000`, clock gate 77; A9 SPI IRQ 191,
  A10 SPI IRQ 217. Both ADTs encode the same SPI DMA channel pair
  `0x1a`/`0x1b` under their SIO DMA parent. The template documents TX `0x1a`;
  the A9 direction interpretation remains inferred until its driver is checked.
- Both have SIO MMIO `0x20ae00000`, gates 90/4. A9 IRQs are
  185/184/187/186; A10 IRQs 211/210/213/212. The A10 lists
  `iop,t8010` then `iop,s8000`; A9 lists `iop,s8000` only. That is evidence
  for a shared starting point, not proof of mailbox/firmware compatibility.
- A9: `multi-touch,j72`, GPIO IRQ 65; LDO GPIO73, active-low reset GPIO75,
  CS GPIO102, display-sync GPIO74, analog power via **D2365 `pmuL(0x21f)`**.
  A10: `hid-transport,spi`, IRQ GPIO21; LDO GPIO29, reset GPIO31,
  CS GPIO85, analog power GPIO145, display-sync GPIO176/177.
- A9 touch clock function: `TCLK(0x8, 0x64, 0x8000)`.
  A10: `TCLK(0x8, 0x64, 0x16e3600)`. The latter argument is 24 MHz,
  the former 32768. **Inferred:** this is a real clock-generation distinction;
  do not copy the A10 fixed 24 MHz clock without decoding the A9 PMGR function.
- A9 calibration: `MtCl` at 0x400 bytes and `PxCl`; selected firmware
  personality `C1F15,1`. A10: HBPP bootloader property, `K1F19,6`, image tag
  `wftm`, and extra MtDO/ETCL/MtPc/FPCL/FfCl records.

**Verified — template source:** the [J172 bring-up notes](https://github.com/Pauli1Go/HoolockLinux-bringup-docs/blob/main/docs/touch.md)
and `apple_z2.c` at Pauli1Go commit `ae101dd0dee8` describe an ordered
HBPP14 upload, SIO DMA, two firmware-ready waits, report discovery/setup,
and J172-specific power/coordinate handling. The source is saved locally
at `build/periph/apple_z2-template.c`. It has no N1 board variant. The supplied
SIO/SPI template commits and current dev versions differ; pin the versions
used by the next implementation rather than combining them indiscriminately.

## Concrete next implementation

1. **Firmware and private inputs first.** On the jailbroken iOS reference,
   read the selected N1 service's merge/firmware properties and locate its
   `C1F15,1` firmware assets. Preserve original bytes and hashes. Read resolved
   MtCl/PxCl from this iPad; never write SysCfg or use a different iPad's
   calibration. Inventory the actual record sizes, header and checksums.
   The Round 1 pull tool searches plausible rootfs roots and preserves its
   findings; it deliberately does not fabricate an N1 Linux container.
2. **Offline N1 disassembly.** Locate the N1 class vtable in extracted
   `AppleMultitouchSPI.macho`, then follow its init, wake, register-write,
   firmware-upload, calibration and `performCalibSeq` methods. Track X0–X7,
   SPI transfer flags, CS holds, endianness, packet lengths, checksums, retry
   limits, and DMA/PIO thresholds. Cross-reference the selected personality's
   five addresses rather than borrowing Z2/HBPP opcodes by name. Separately
   map `AppleMultitouchDriver` parser type 1/options 16 contact records.
3. **A9 transport foundations.** Port SIO core/DMA and SPI support into a
   separate A9 series, using this kernelcache's `s8000-smartio.bin`, the A9
   IRQs, power domains and DMA channel descriptions. Confirm RTKit/SIO boot
   protocol and descriptor layout offline before enabling transfers. Decode
   A9 touch-clock control and the D2365 analog regulator operation. Do not
   enable A10 GPIO145 or its clock provider on J71t.
4. **Separate N1 variant/state machine.** Use `apple_z2` only for code whose
   wire behavior is verified to match. Add an explicit N1 match/ops or an
   `apple_n1` driver when framing differs. Keep boot/download/calibration
   operations separate from runtime report parsing. Require the actual N1
   firmware format and same-device calibration, bounded validation and error
   unwinding, active-low reset, correct LDO/analog sequencing, and timed
   IRQ/CS waits. Never add `apple,j172-z2` as a fallback compatible.
5. **Staged device validation, by the orchestrator.** First validate SIO
   firmware handshake without touching the touchscreen; next SPI/CS/clock and
   N1 identity read; then a single bounded RAM firmware upload; then calibration
   and report discovery; finally raw contact packets and Linux type-B events.
   Test single touch, ten-finger tracking if the reported descriptor supports
   it, lift/release, orientation and screen bounds, with reset/error recovery.
   Each stage must capture its actual trace before advancing.

**Verified — platform constraint:** A9 has no EL2; the m1n1 hypervisor cannot
trace iOS. Dynamic evidence must come from jailbroken iOS instrumentation or
external SPI observation, authorized and run by the orchestrator. Hardware
bring-up, N1 packet format, actual rootfs firmware names and suspend/resume
remain unverified in Round 1.
