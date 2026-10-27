# J71t AOP motion sensors — reverse-engineering round 1

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


2026-10-23. Target: iPad 5, iPad6,11, S8003 / J71t. This round used local
artifacts, the shared Ghidra project, and public source/firmware downloads.
**No device connection or device command was executed.** No workspace or
kernel commit was made; no kernel branch was created.

## Result and implementation decision

The accelerometer is behind **AppleA7IOPV2 → RTBuddy/RTKit → AppleSPU →
AppleSPUHIDInterface → AppleSPUHIDDevice → AppleSPUHIDDriver**. The transport is
an old **64-bit IOP mailbox carrying RTKit**, followed by shared-memory SPU
queues and EPIC-like packets. “Old IOP versus RTKit” is therefore not an
either/or choice. It is not the contemporary Apple Silicon 96-bit ASC mailbox.

We have static evidence for the mailbox registers, endpoint packing, management
hello, service announcement, sensor property commands, calibration tag mapping,
and the packed motion report consumed by iOS. The exact 20H350 AOP firmware is
a separate IPSW component, obtainable without reading the device. The manifest
does **not** mark it as iBoot-loaded. Whether an AOP survives **our actual
palera1n → pongoOS → m1n1 → Linux path** remains unmeasured.

**Do not start a booting sensor driver yet.** The safe attachment/cold-boot
memory contract and a validated j71t queue/service transcript are still missing.
An enabled stub or an M1 driver with changed MMIO addresses would conceal these
gaps. The proposed `a9/aop` / build tree `aop` should begin after gates U1–U4
below; it must initially be default-off. A fixed-allowlist, read-only status
collector is supplied now as `scripts/periph/aop-status.py`.

## Evidence identity and reproduction

All kernel addresses below are **unslid virtual addresses** in:

```text
build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
SHA256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
Ghidra: kernelcache.release.iPad6,11_12; AARCH64:LE:64:AppleSilicon
```

Primary local sources:

- `docs/adt/j71t.txt`: static ADT, especially lines 642–714, 1856–1945,
  2375–2450 and 3228–3252. This file contains unresolved `syscfg/...` references.
- `build/firmware/private/IODeviceTree.txt`: saved **IODeviceTree plane**, AOP
  subtree at lines 885–1022. Contains actual resolved calibration and matched
  DT-plane nubs, but not the full IOService driver hierarchy.
- `build/periph/aop-r1/inventory.json`: prelink metadata and kext personalities.
- `build/periph/aop-r1/*.strings`: kext-scoped string VAs.
- `build/periph/aop-r1/ghidra/`: retained MCP replies and C output.
- `build/periph/aop-r1/{iop,spu,rtbuddy}-local/`: small extracted-kext Ghidra
  checks for missing function entries and compiler tail sharing.

Repeatable host-only steps:

```sh
python3 -B scripts/periph/aop-recon.py inventory
python3 -B scripts/periph/aop-recon.py ghidra decompile_function_by_address address=fffffff005ba8554
python3 -B scripts/periph/aop-recon.py words 0xfffffff006d5e7f8 0x90
bash scripts/periph/fetch-aop-reference.sh
ipsw img4 im4p extract build/periph/aop-r1/ipsw/20H350__iPad6,11/aopfw-ipad6faop.im4p \
  --output build/periph/aop-r1/aop.bin
python3 -B scripts/periph/aop-recon.py verify
python3 -B scripts/periph/aop-status.py --list
```

The recon client never opens a device connection. Its `ghidra` action can also
invoke mutation tools deliberately; the inventory/words/pointers actions are
offline reads. Queries are cached under the ignored build directory.
`verify` passed: exact kernelcache identity, kext matching, independently
encoded AArch64 64-bit FIFO instructions, all nine calibration tags, both j71t
BuildManifest identities and exact firmware hashes. Python syntax and shell
syntax checks passed; the status collector's offline allowlist and host refusal
were checked. No hardware behavior or kernel build was tested in this round.

Ghidra caution: several live-project functions are absent or include adjacent
tail-shared methods after a panic call. For example `0xfffffff005b4e578` can
decompile into the following interrupt-control method. Use the retained
instruction listings for exact boundaries; do not interpret the panic edge as
normal fallthrough. Missing entries were checked in separate local projects,
without opening the shared project a second time. Understood functions in the
shared project were renamed/commented and saved with `scripts/ghidra-mcp.sh save`.

## 1. Driver matching and hardware

### Exact kexts, not guessed class names

The prelink dictionary has these executable bundles:

| Bundle | Version | Executable load/header VA | Relevant personality |
| --- | --- | --- | --- |
| `com.apple.driver.IOSlaveProcessor` | 1 | `fffffff005303f80` | Common coprocessor abstractions |
| `com.apple.driver.AppleA7IOP` | 1.0.2 | `fffffff00530e300` | `AppleA7IOPV2`: `IONameMatch=iop,s8000`, provider `AppleARMIODevice` |
| `com.apple.driver.RTBuddy` | 1.0.0 | `fffffff005310840` | `RTBuddy`: `IONameMatch=iop-nub,rtbuddy`, provider `AppleA7IOPNub` |
| `com.apple.driver.AppleSPU` | 1 | `fffffff005319e00` | Firmware service, endpoint service, HID interface/device/event driver |
| `com.apple.driver.AppleSPUSphere` | 1 | `fffffff0056a2300` | `AOP_spherecontrol`, not required for basic gravity samples |

`AppleSPUFirmwareService-AOP` matches `RTBuddyService` with `role=AOP`.
`AppleSPU` matches `RTBuddyEndpointService` names `AOPEndpoint1` through
`AOPEndpoint32` (also SPU/SPU2 names). `AppleSPUHIDDevice` matches
`AppleSPUHIDInterface`; `AppleSPUHIDDriver` matches `IOHIDInterface` with
`Transport=SPU`. These classes live in AppleSPU.kext; a distinct AppleAOP.kext
is not needed to explain this build. Presence of other SPU personalities does
not establish that those devices exist on j71t.

The saved live tree explicitly shows `iop-aop-nub <class AppleA7IOPNub>` and
`accel/gyro/compass/pressure <class AppleSPUHIDInterface>`. For accel:
`device-usage-page=0xff00`, `device-usage=3`, `service_id=0x10000007`.
**That service ID is not an RTKit endpoint number or necessarily a queue channel.**

### ADT registers and interrupts

ADT `/arm-io` translates the low peripheral window by `+0x200000000`;
the saved live `IODeviceMemory` values independently confirm these addresses.

| `aop/reg` index | Static ADT address / size | AP physical address | Interpretation |
| --- | --- | --- | --- |
| 0 | `0x10800000 / 0x1c000` | `0x210800000` | IOP wrapper, CPU/remap controls and mailbox |
| 1 | `0x10e00000 / 0xa0000` | `0x210e00000` | AOP region / target memory aperture |
| 2 | `0x10000600 / 4` | `0x210000600` | Auto-boot control, separately mapped by AppleA7IOP |
| 3 | `0x10000508 / 8` | `0x210000508` | Additional register pair; exact use unresolved this round |

**Artifact difference:** the saved live tree has reg[1] and nub `region-size`
**`0x9b000`**, with the same `region-base=0x210e00000`; the static dump says
**`0xa0000`**. Do not silently choose the larger aperture. The provenance/build
of the static text alone does not resolve this difference; capture the actual
boot ADT before reserving or writing memory. `region-base` is already absolute;
do not translate it a second time.

Other properties: `compatible=iop,s8000`, `iop-version=1`, `device_type=aop`,
`role=AOP`, `auto-boot` present, empty clock IDs/gates and power-gates. Thus the
ADT's `iop-version=1` does **not** mean the kext class is AppleA7IOPV1.
The child nub has `firmware-name=ipad6faop`, `aop-target=8`,
`compatible=iop-nub,rtbuddy`, `watchdog-enable`, and `coredump-enable=32`.
The AppleSPU patchbay table maps `aop-target` to the four-character `AOPt`
field (table word `0x00000004414f5074` at `0xfffffff005320178`).

Interrupt-parent is phandle 22, the AIC. Interrupts in ADT order are
`[131,130,133,132]`. AppleA7IOP start (`0xfffffff005b4b0d4`, offline instruction
check) creates interrupt sources at provider indices **2** and **1**:

- index 2 / AIC 133: outbox / IOP→AP receive;
- index 1 / AIC 130: inbox / AP→IOP send-space notification.

The exact empty/full roles of unused indices 0/3 are not established here.
These are AIC hardware numbers, not Linux virtual IRQ numbers or GIC SPIs.

### Wrapper/mailbox layout proved from AppleA7IOPV2

Instance vtable: `0xfffffff006d3ff98`, selected by constructor
`0xfffffff005b4e3ac`. All offsets below are relative to `0x210800000`.

| Offset | Width | Evidence and meaning |
| --- | --- | --- |
| `0x44` | 32 | CPU control: `0xfffffff005b4e7c8` RMWs bit 4; also writes reg[2] when auto-boot is present |
| `0xb88` | 32 | Raw vendor predicate bit0; mirrors inbox, CPU execution unknown; `0xfffffff005b4e4b8` |
| `0x4000 / 0x4004` | 32 | Mailbox interrupt set/clear; mirrored writes at `0xc00 / 0xc04` |
| `0x4008` | 32 | Inbox control: bit16 full, bit17 empty; bits18/19 fault checks |
| `0x4010` | **64** | AP→IOP FIFO data; one `str x8`, `0xfffffff005b4e4a8` |
| `0x4020` | 32 | Outbox control: bit17 empty, bits18/19 fault checks; bit0 enable via `0xfffffff005b4e5d8` |
| `0x4038` | **64** | IOP→AP FIFO data; one `ldr x8`, `0xfffffff005b4e498` |
| `0x4040` | 32 | Printed as `IDLE_STATUS` by `0xfffffff005b4e624`; detailed bits not decoded |

Inbox full/empty helpers: `0xfffffff005b4e50c` / `0xfffffff005b4e4dc`.
Outbox empty: `0xfffffff005b4e578`. Interrupt control helpers
`0xfffffff005b4e530`, `...e548`, `...e5a8` use masks `0x1111`, `1`, `0x1000`.
`getMailboxBulk` at `0xfffffff005b4bda8` advances its output pointer by **8**
bytes per FIFO item. Reading `0x4038` consumes a message: it is not a passive
status probe. Do not bulk-dump this register page.

Remap routine `0xfffffff005b4e72c` (prior shared name
`AppleA7IOPV1_setFirmwareRemap`, also used by the V2 vtable) programs wrapper
`+0x18/+0x20` from the IOP start address, `+0x28` from the image length masked
to 4 KiB, `+0x30=0`, `+8/+0x10` from the backing physical address, then
`+0x38=1`. This is evidence of a programmable backing window, **not permission
to load the entire IM4P at an arbitrary address**.

### Power dependencies

PMGR records in the static ADT:

| Name | `id1` | `index` | `map` | `flag` |
| --- | --- | --- | --- | --- |
| AOP | 42 | 0 | 0 | 35 |
| AOP_GPIO | 44 | 2 | 0 | 3 |
| AOP_CPU | 45 | 8 | 0 | 35 |
| AOP_FILTER | 46 | 9 | 0 | 35 |
| AOP_BUSIF | 47 | 10 | 0 | 35 |

These decoded fields are not automatically Linux genpd IDs or safe register
offsets. Reconcile them with the existing S8003 PMGR provider. Empty
`aop/power-gates` does not establish that clocks/power/reset can be ignored.
AppleA7IOP invokes provider gating operations (`0xfffffff005b4b630`,
`...b4b698`) and an optional `setActive` service (`...b4b6f4`). Its CPU-start
path is `0xfffffff005b4b738`: enable power, map firmware when required, prepare
mailboxes/interrupts, then release the CPU/auto-boot path. It has an explicit
“CPU already running” branch. The precise AOP-specific sequencing of those
provider calls remains to be mapped, not inferred from the list order.

The ADT separately names wake sources `AOP.ExternalReq`, `AOP.CPUWakeupAP`,
`AOP.OutboxNotEmpty`, `AOP.APWatchdog`, `AOP.APWakeupTime`, and PMGR events
`S2R_AOP` and `AOP_DDR`. Preserve these dependencies for initial bring-up;
suspend/power gating is a later step.

## 2. Firmware source, boot and attachment

### Exact IPSW component found

Host-only partial extraction of iPad6,11 **16.7.10 / 20H350** produced:

```text
BuildManifest: Manifest.AOP.Info.Path = Firmware/AOP/aopfw-ipad6faop.im4p
  IsFUDFirmware = true
  IsLoadedByiBoot = false
  IsLoadedByiBootStage1 = false
  IsFTAB = false

aopfw-ipad6faop.im4p: 379797 bytes
SHA256 5a4d8378bb83e175fc589cc1cf978430639d7ffcef07b2d94e9d6a4801124efa
extracted aop.bin SHA256
1306381a3876ede15ad0c3f77f6d1edb5273b1c1e93dff01ef3f6a1478327911
```

Both erase/update `j71tap` identities select that component. The plaintext
extracted payload contains `RTKit-2062.142.1.release`,
`ipad6faop:AppleSPUFirmwareBuilder-427.100.4~18465`, `ipad6faop_ext`,
`Accel.cpp`, `AppleImuSensor.cpp`, and `AppleAccelGyroCombo.cpp`.
The payload starts with a binary serialized container (`d3 00 00 00`), including
`firmware`/`data`; the ARM vector-like bytes inside are not a Mach-O header.
The firmware bundle/segments need parsing before a Linux loader is written.

A whole-kernelcache byte search finds no `ipad6faop`, `ipad6faop_ext`, or
`aopfw`; no complete matching firmware image was identified embedded there.
The generic `RTKit-2062.142.1` string alone does occur in the kernelcache and
does not identify an embedded AOP payload. The positive evidence is the exact
IPSW component plus the firmware-update service path, not just string absence.

### Kernel loader path

`AppleSPUFirmwareService_start` (`0xfffffff005b87340`) obtains `firmware-name`
from the nub, matches role AOP/SPU/SPU2, and checks RTBuddy's pre-loaded state.
It uses state 3 for pre-loaded versus state 2 otherwise, and accepts the
`aop-force-fw-loading` bootarg. This is **not** a recommendation to set it.

The timer/request chain is:

```text
0xfffffff005b876cc -> 0xfffffff005b8792c
  -> firmware-update service initialized by 0xfffffff005b1d3e4
     (IOUserClientClass = AppleFirmwareUpdateUserClient)
  -> callback 0xfffffff005b87ab8 -> 0xfffffff005b87b38
  -> RTBuddyFirmwareBundle parse 0xfffffff005b617c0
  -> select Target / matching firmware-name at 0xfffffff005b87c28
```

The selection also handles `_ext` images and external segment metadata.
The userspace FUD file location on the installed iOS filesystem was not pulled;
do not substitute a guessed `/System/Library/...` path for this evidence.

`RTBuddy_parseBootProperties` (`0xfffffff005b68764`) distinguishes presence of
`pre-loaded`, `running`, and `no-firmware-service`; it also reads watchdog,
hibernate and power-management properties. The saved live DT-plane nub has
none of these three attachment properties. `auto-boot` on its parent is a
separate hardware control property, **not proof of firmware already running**.

`RTBuddy::_loadFirmwareGated` path `0xfffffff005b6479c` validates the role and
calls firmware preparation `0xfffffff005b6f8a4`. That path can copy a supplied
image to target memory (`0xfffffff005b6f340`) or use an existing target, apply
patchbay configuration, and publish firmware identity. Target selection
`0xfffffff005b68cdc` can use described regions or allocate backing memory.
The exact j71t choice and AP/IOP physical-address relation need confirmation.

### What can be concluded about iBoot?

The manifest and absence of `pre-loaded`/`running` in the saved nub support
**normal kernel/FUD loading**, rather than relying on an M1-like iBoot-loaded
image. Existing decrypted j71t iBoot on the host contains `arm-io/aop` and
`iop-aop-nub` strings; those strings alone cannot prove CPU state or firmware
lifetime. They do not override the manifest.

**Unresolved:** cold DFU/checkm8 entry, a warm path after running iOS, and a
Linux kexec may leave different AOP states. A running bit alone also cannot
prove the retained firmware/shared-memory pointers are safe. U1 measures this
before any mailbox consumption or CPU/reset/remap write. No claim of an AOP
running under the current Linux session is made in this report.

## 3. RTKit, queues and sensor protocol

### 64-bit mailbox framing and management

`RTBuddy` vtable at `0xfffffff006d4c000`, slot `+0x568`, selects
`0xfffffff005b66aa8`. The exact-entry offline assembly proves:

```text
wire = (message & 0x00ffffffffffffff) | ((u64)endpoint << 56)
```

It rejects a nonzero high byte inconsistent with the endpoint. Thus endpoint
is **bits63:56**, not a separate 32-bit MMIO word. Management type is bits55:52
in this transport. `RTBuddyManagement_handleHello` (`0xfffffff005b7196c`)
requires the peer's `[min,max]` to contain **12** and sends hello-ack
`0x00200000000c000c` with optional debugger/tracing bits above bit31.
The firmware's RTKit release `2062.142.1` and mailbox protocol version 12 are
different version spaces.

`RTBuddyManagement_handleEndpointMap` (`0xfffffff005b71afc`) handles a low32
bitmap, base in bits32–37, and last flag bit51, then emits type-8 replies.
Endpoint service names are generated from the role (`%sEndpoint%u` string at
`0xfffffff005315e1b`). Discover endpoints through roll-call; neither ADT usage3
nor Asahi's M1 accel endpoint is a valid j71t endpoint constant.

### Shared queues: established and still to verify

The application protocol is queue-based, not one accelerometer sample per
mailbox word. AppleSPU start (`0xfffffff005ba7984`) attaches to an
RTBuddyEndpointService, obtains slave memory alignment, installs endpoint
callbacks and queue machinery, then enables the endpoint.

`0xfffffff005ba7d40` extracts an **8-bit** message type from bits55:48; it
recognizes `0xa2` and `0x85` as queue activity. `0xfffffff005ba7d68` schedules
the queue reader which calls `AppleSPU_routeEPICPacket`. Strings identify
`AppleSPUDataQueue.cpp`, `iop_ringbuffer.h`, TX/RX/historical queue sizes, and
an SPU protocol-version compatibility check. This is strong evidence for the
same AFK/EPIC family as Asahi's AOP work, with an older wire revision.

An additional exact-entry offline check of the endpoint-message switch at
**`0xfffffff005ba88e0`** recovered the following initialization contract:

| IOP→AP type | Static behavior |
| --- | --- |
| `0x80` | Requires low48 **8** (SPU protocol version); replies `0xa0 << 48` and enters initialization state1 |
| `0x89` | Requests a shared buffer: low16 tag, size=`((word >> 16) & 0xffff) * 64`; allocates rounded backing memory and replies `0xa1 << 48 \| (IOP-visible-address & ((1ULL<<48)-1))` |
| `0x8a` | RX queue descriptor: low16 buffer tag, size in bits31:16 ×64, offset in bits47:32 ×64 |
| `0x8b` | TX queue descriptor, same encoding |
| `0x8c` | Historical queue descriptor, same encoding |
| `0x85` | Queue/RX notification; kicks the packet reader when active |
| `0x86` | Start acknowledgement / wakes the waiting state transition |
| `0x87`, `0xc3` | Stopped/shutdown-related state notification; full lifecycle semantics still to trace |

Legacy alternatives `0x81` (size/address-style allocation) and
`0x82/0x83/0x84` (address-based queue descriptors) also exist in the switch.
Their presence does not establish which path the j71t image chooses. The
modern comparison is therefore supported by matching concrete message types
and 64-byte units, while a **complete observed j71t transcript is still absent**.
The SPU version8 here is a third version space, distinct from management12
and packet/subheader version2.

Producer/consumer ownership, ring header/block layout, wrap/alignment, cache
attributes and safe running-firmware takeover are still to verify. In
particular, an IOP-visible buffer address is not established to equal an AP
physical address. Preserve historical-queue handling as a separately negotiated
capability; basic rotation should initially use ordinary reports and a measured
small batch interval.

### Packet headers proved in the iOS consumer

`0xfffffff005ba83ec` checks outer version2. After the queue/channel envelope,
its packet header is 16 bytes: version at0, sequence16 at1, timestamp64 at8.
The first subpacket begins at16. The walker at `0xfffffff005bacde4` advances
by `24 + payload_length`, so one outer packet may contain multiple subpackets.

The **old SPU subheader**, built by `0xfffffff005bae308` and consumed by
`0xfffffff005bacf58` / `0xfffffff005bad4d8`, is packed little-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Payload length |
| 4 | 1 | Subheader version = 2 |
| 5 | 1 | Category: report <`0x10`, notify `0x10`, reply `0x20`, command `0x30` |
| 6 | 2 | Subtype |
| 8 | 2 | Sequence / transaction tag |
| **10** | **8** | **AOP timestamp, unaligned** |
| 18 | 2 | Reserved/flags; some report path metadata is in this area |
| 20 | 4 | Inline/response capacity; builder uses `0xe4` for notify |
| 24 | variable | Payload |

Do not cast a C struct over untrusted queue bytes. Use explicit lengths and
`get_unaligned_leXX`; reject overflow, short packets and inconsistent versions.
Do not copy the current M1 subheader's timestamp offsets into this parser.

Ready/announcement subtype **`0xc0`** is parsed by `0xfffffff005ba8554`:
name string occupies **32 bytes**, service ID is at+32, flags at+36, optional
interface number at+40. It finds a matching ADT child by name and publishes an
SPU HID interface or app interface. Modern Asahi AOP announcements have a
20-byte name and different metadata; those definitions are not interchangeable.

Report accounting (`0xfffffff005bace88`, labels at `0xfffffff00531f26f` onward)
orders `0xc0` ready, `0xc1` gyro, **`0xc2` accel**, `0xc3` compass, `0xc4` HID.
`0xfffffff005b8d1f0` → `0xfffffff005b8dc78` accepts `0xc1`–`0xc4`, wraps their
payloads in a memory descriptor and delivers them through HID. The actual
j71t accel subtype/descriptor pair should still be recorded: a label sequence
is not a captured transaction. Generic HID subtype `0xc4` is not itself the
Apple vendor usage3 identifier.

### Queries, enable and rate

`AppleSPUHIDDevice_getReportDescriptor` (`0xfffffff005b8d550`) sends subtype
**1** with no supplied argument and returns the firmware's report descriptor.
Get-report uses subtype6 plus a report-ID byte (`0xfffffff005b8d6f8`);
set-report uses subtype7 (`0xfffffff005b8d8e8`). The descriptor is supplied at
runtime; no j71t descriptor was recovered from the saved DT-plane dump.

Property setters enter `0xfffffff005bad638` → `0xfffffff005bad898` and use
subtype **4**. Encoder `0xfffffff005bae168` adds the leading zero word:

```text
notify subtype4 payload = le32(0) || le32(property_key) || value_bytes
reply payload starts with IOReturn; correlate the transaction tag
```

The high-level ReportInterval path (`0xfffffff005b99ef0`) and power callback
(`0xfffffff005b9e6d8`) demonstrate:

| Property | Key | Value / evidence |
| --- | --- | --- |
| Report interval | **0** | 32-bit interval; zero stops ordinary reports, nonzero enables them |
| Batch interval | `0x16` | 32-bit; high-level path defaults to 10000 when enabling without a batch interval |
| Raw interval | `0x5c` | 32-bit; separate from report interval |
| Calibration state | `0x3d` | 32-bit; sent after factory calibration |

The ReportInterval setter sends batch key `0x16`, raw key `0x5c`, then key0.
On sleep, the ordinary stream sends key0=0; on wake it restores the saved
interval. Existing historical-stream cases have exceptions. An initial
20 Hz proposal is key0=50000 **if microseconds are confirmed**, not a hardware
sampling-frequency register write. Microseconds are strongly suggested by
the 10000 default and Asahi's 200000-us ALS example, but accepted j71t accel
rates, rounding and latency require measurement. Do not confuse the raw,
report and batch intervals or advertise untested rates as IIO capabilities.

### Motion payload and scaling

The accel branch of `AppleSPUHIDDriver_dispatchMotionReport`
(`0xfffffff005b9eb5c`, enabled for page0xff00/usage3 in `...b9c318`) reads:

| Payload offset | Width | Use in iOS |
| --- | --- | --- |
| 0 | 16 | Sequence, widened into HID motion event |
| 2 | 32 | Motion type/status argument |
| **6, 10, 14** | **3 × 32** | **X, Y, Z passed directly to accelerometer event factory** |
| 18 | 32 | Temperature event value |
| 22 onward | variable | Optional auxiliary data; not needed for initial accel |

This is the **payload after the 24-byte subheader**, not a mailbox layout.
It is at least 22 bytes for the consumer path above. The axes go unchanged to
`0xfffffff005b08c84` → `0xfffffff005b08afc`, which stores them into the event.
Apple's public [IOHIDEvent declarations](https://github.com/apple-oss-distributions/IOHIDFamily/blob/main/IOHIDFamily/IOHIDEvent.h)
identify the corresponding arguments as `IOFixed`. That supports **signed
16.16**, rather than IEEE floats or three 16-bit chip counts.

**Working interpretation to validate:** 16.16 g, giving an IIO scale of
`9.80665 / 65536 = 0.000149637603759765625 m/s²/count` (e.g.
`IIO_VAL_INT_PLUS_NANO`, 0 and 149638). Signedness/fixed-point interpretation
has strong API evidence; absolute g scale and axis/gravity sign still need
the six-pose check. Temperature's scale is not established here. Do not
export it with guessed units. Report bytes, final HID events and chip-register
counts are distinct layers; the first two may already be calibrated.

### Calibration and ARot

`AppleSPUHIDDriver_sendFactoryCalibration` (`0xfffffff005b9d9e4`) constructs
a calibration table from data at **`0xfffffff006d5e7f8`**. The entries are
16 bytes each, with a property-name pointer, tag and control flags:

| ADT property | Syscfg source | AOP property key |
| --- | --- | --- |
| `low-temp-accel-offset` | LTAO | `0x1a` |
| `accel-sensitivity-calibration` | ASCl | `0x0b` |
| `accel-range-sensitivity-cal` | ARSC | `0x27` |
| **`accel-orientation`** | **ARot** | **`0x18`** |
| `accel-interrupt-calibration` | AICl | `0x19` |
| `accel-range-intr-cal` | ARNC | `0x4b` |
| `accel-nominal-extr-cal` | ARXN | `0x4c` |
| `accel-range-extr-cal` | ARXC | `0x4d` |
| `accel-offset-cal` | AROC | `0x1a` |

LTAO and AROC deliberately share a tag; table flags/order and calibration
version matter. `0xfffffff005b96d70` walks the table. Entry handler
`0xfffffff005b96b60` normally sends existing OSData bytes; certain flagged
entries have location-ID/version selection. `0xfffffff005b96aec` is the plain
blob-send helper. Missing entries are tolerated distinctly from transport
errors. `NoFactoryCalibration` is an explicit alternative; it must not become
the default just because a static ADT contains unresolved syscfg strings.

The saved live accel nub has ASCl (36 bytes), AICl (16), LTAO (20), and:

```text
accel-orientation = 01000000 ff000000 00ff0000 0000ff00  (16 bytes)
```

It does not contain every optional range-calibration property named by the
static ADT. Preserve presence/absence and the exact per-device blobs. The
ARot data is **not nine little-endian 32-bit matrix coefficients**. A plausible
interpretation is version1 followed by three padded signed-byte rows
`(-1,0,0), (0,-1,0), (0,0,-1)`; the host kernel does not decode that layout,
so its exact encoding/sign convention remains a hypothesis.

The proven host behavior is: send ARot to firmware as property0x18, then
consume axes without multiplying ARot on the AP. Consequently the Linux path
should either reproduce firmware calibration/orientation and expose a
**residual screen mount matrix**, or deliberately retain a verified sensor
frame and expose the corresponding mount matrix. Do not apply ARot in both
firmware and IIO. Never turn the version word into a matrix element. A
calibrated, already oriented report can still be exported as IIO `_raw`
counts with a fixed scale; document that “raw” means AOP-report counts.

## 4. Comparison with Asahi and current Hoolock

Primary comparison sources inspected:

- [Asahi m1n1 AOP client](https://github.com/AsahiLinux/m1n1/blob/7cde7d1a784d851ebfee5579fa363c38b14150c0/proxyclient/m1n1/fw/aop/client.py),
  [AFK rings](https://github.com/AsahiLinux/m1n1/blob/7cde7d1a784d851ebfee5579fa363c38b14150c0/proxyclient/m1n1/fw/afk/rbep.py),
  [EPIC](https://github.com/AsahiLinux/m1n1/blob/7cde7d1a784d851ebfee5579fa363c38b14150c0/proxyclient/m1n1/fw/afk/epic.py),
  and [AOP calls](https://github.com/AsahiLinux/m1n1/blob/7cde7d1a784d851ebfee5579fa363c38b14150c0/proxyclient/m1n1/fw/aop/ipc.py).
- [Asahi Linux AOP](https://github.com/AsahiLinux/linux/blob/f19ef6bf2bf607ffb39dc3250e0986b1fc8aa956/drivers/soc/apple/aop.rs)
  and [aop_als](https://github.com/AsahiLinux/linux/blob/f19ef6bf2bf607ffb39dc3250e0986b1fc8aa956/drivers/iio/common/aop_sensors/aop_als.rs).
- Local Docker-volume Hoolock kernel at `6831bc701a`,
  `drivers/soc/apple/{mailbox.c,mailbox.h,rtkit.c}`. Read without changing a
  kernel worktree. Source copies/references are in `build/periph/aop-r1/`.

Reusable concepts: RTKit hello/endpoint discovery, DMA-aware shared buffers,
queue-driven EPIC transactions, a dynamic service/channel registry, subtype1
descriptor query, subtype4 property updates, and separate IIO sensor clients.
Asahi ALS uses calibration key0x0b and interval key0; it selects sensor-specific
lux offsets rather than assuming one format. These validate the architecture,
not the j71t accel payload or firmware loader.

Critical differences:

| Layer | J71t evidence | Contemporary Asahi/Hoolock example |
| --- | --- | --- |
| Mailbox | 64 bits, endpoint in high8 | Hoolock mailbox struct is 64+32 bits, endpoint in `msg1` |
| MMIO | Inbox `0x4008`, data `0x4010`; outbox `0x4020`, data `0x4038` | ASC/M3 register layouts differ |
| Application doorbell | Type bits55:48 | m1n1 AFK type field is bits63:48 on its transport |
| EPIC subheader | Version2, tag@8, timestamp@10 | Current AOP definitions differ by version and packing |
| Announcement | Name32, service ID@32 | Modern AOP announcement name20 |
| Firmware lifecycle | FUD loader, no manifest iBoot-loaded flag | Apple Silicon bootargs/image handoff cannot be assumed |
| Hardware sensors | Accel/gyro/compass/pressure under AOP | Modern Mac ALS/audio service sets differ |

Hoolock's RTKit library already negotiates versions10–12 and computes the
application-endpoint base by negotiated protocol. Its send/RX paths currently
use `msg1` for endpoint; a register-offset-only mailbox variant would lose the
old high-byte packing. Introduce a transport codec/capability for 56-bit payload
plus endpoint, without changing other ASC clients. Reject unrepresentable
messages explicitly. Audit shared-buffer address/size fields and upper-bit
usage before claiming the generic library is reusable unchanged.

### Ambient light and hall are separate on this board

`/arm-io/i2c2/als1`: `compatible=als,ct819`, I²C address0x39, LSCI calibration,
AP GPIO71 interrupt. Existing [peripheral round3](round3.md) and later reports
cover the CT819 driver/calibration. It is not an AOP ALS endpoint on j71t.

`/arm-io/i2c0/pmu`: `compatible=pmu,d2365`, events `halleffect`/`halleffect1`.
`/buttons/function-button_halleffect` and `...halleffect1` call PMU phandle79
GPIO functions for lines0 and10. Event-name numbering and GPIO-function line
numbers differ; preserve that distinction. Later hall support belongs in the
PMU/button input path (likely an input switch after polarity validation), not
in this accelerometer IIO driver. A generic AppleSPUSmartCover personality
with usage0xff00/41 is not evidence that j71t uses it.

## 5. Minimal Linux design

Proposed split, once the gates below are satisfied:

1. **A9 IOP transport / lifecycle.** A default-off `APPLE_A9_AOP` option;
   `apple,s8003-aop` with a compatible fallback only where measured. Own the
   wrapper resource, AIC133 RX and AIC130 TX-space IRQs, CPU/auto-boot controls,
   reserved firmware region and PMGR dependencies. Add the 64-bit transport
   codec described above. Keep generic RTKit management helpers if the codec
   and version negotiation can be separated cleanly.
2. **SPU queue/service core.** Negotiate the observed queue revision, establish
   AP/IOP address translation, perform cache/DMA ordering, discover announced
   services and route transaction tags. Expose the accel service to a child
   driver only after a successful handshake. Support bounded timeouts and
   crash reporting; do not register usable sensor channels before samples.
3. **AOP IIO accelerometer.** Bind the discovered `accel` service plus
   page0xff00/usage3. Send this unit's valid calibration properties in the
   observed order, obtain its descriptor, set a measured report rate, and
   validate the subtype/22-byte motion payload. Decode all fields unaligned.
   Initially retain latest samples for direct reads; a timestamped IIO buffer
   can follow once timestamp conversion is validated.

IIO ABI should provide `in_accel_x_raw`, `in_accel_y_raw`, `in_accel_z_raw`,
`in_accel_scale` (or per-axis equivalent), `in_accel_mount_matrix`, and measured
`sampling_frequency` capabilities. Return `-ENODATA` before the first sample
and after stale/failed transport; never return a fabricated zero-gravity sample.
Expose the matrix through the IIO mount-matrix helper and a board property;
derive it in screen coordinates from six poses after firmware orientation.
Use a lock/sequence counter for consistent cached vector reads. IRQ work
must not sleep while issuing synchronous EPIC commands.

Lifecycle states should distinguish powered-but-stopped, firmware present,
firmware running with unknown host ownership, negotiated and streaming.
**Warm attach** must validate the retained memory and establish ownership; do
not reuse iOS AP pointers or replay a cold hello into an unknown running queue.
**Cold boot** must reserve memory before allocation, parse/select the firmware
bundle, configure remap/patchbay, then start CPU/mailboxes in the verified
order. Keep watchdog policy explicit. Hold the necessary domains on initially;
add suspend/resume only after stream stop/restart is repeatable.

DT nodes should remain `status="disabled"` until tested. Carry calibration
through a dedicated RAM-only firmware blob or a bootloader-generated property,
with per-device provenance; do not put another unit's syscfg data into a
public board DTS or teach Linux to write syscfg. Distribution/firmware staging
is separate from the driver and must not replace LLB/iBoot/NVRAM.

`iio-sensor-proxy` needs a valid accelerometer plus correct mount matrix and
units, not an Apple HID emulation layer. The workspace now also contains
`ipad-autorotate`, which can use `monitor-sensor` or direct IIO sysfs and invoke
`ipad-rotate` for display/touch rotation. This round does not modify that work.
First verify physical orientation in its `--probe` mode; only then enable
rotation policy. Accelerometer support alone does not rotate Hyprland.

## 6. Orchestrator test plan — no device commands run in this round

### Pass A: passive inventory and handoff state

**iOS, when the orchestrator has deliberately booted iOS:** save full text
IOService and IODeviceTree planes; archive privately on the Mac. XML mode is
known broken on this iOS, so use text with a large width.

```sh
mkdir -p build/periph/aop-live
scripts/ipad-ssh.sh 'ioreg -p IOService -l -w 0' \
  > build/periph/aop-live/ios-service.txt
scripts/ipad-ssh.sh 'ioreg -p IODeviceTree -l -w 0' \
  > build/periph/aop-live/ios-devicetree.txt
scripts/ipad-ssh.sh 'uname -a; sw_vers; command -v hidutil; command -v ioreg' \
  > build/periph/aop-live/ios-tools.txt
```

Inspect driver instance counts/provider chains, RTKit/FirmwareVersion/UUID,
`pre-loaded`/`running`, region base/size, `slaveAlignment`, endpoint names,
service IDs, report descriptors, interval/calibration properties and PM state.
Do not dump NVRAM/syscfg again to obtain values already resolved in the ADT;
the existing private calibration dump is enough for the first comparison.
Do not use `ioreg`/HID property setters as a passive probe.

**Linux, command text for the orchestrator, not executed here:**

```sh
uname -a
cat /proc/interrupts
cat /proc/iomem
cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
dtc -I fs -O dts /sys/firmware/devicetree/base
```

Capture missing-file/tool errors rather than changing the system just to make
the command succeed. Record the boot path and whether iOS ran since the last
power-on. Compare at least (a) cold DFU/checkm8 boot into Linux, (b) the
orchestrator's actual warm path, (c) kexec if used. DT properties can be
modified or dropped by m1n1, so record the original boot ADT as well as the
Linux DT. Linux DT alone is not proof of Apple's original nub state.

**Then**, only after the wrapper is known powered/accessibly mapped and no
other driver owns it, the orchestrator can copy/run:

```sh
python3 aop-status.py
```

The file is `scripts/periph/aop-status.py` on the host. It refuses non-Linux,
non-root or a DT not identifying j71t. It reads only 32-bit wrapper offsets
`0x44`, `0xb88`, `0x4008`, `0x4020`, `0x4040` using an `O_RDONLY` descriptor.
`--list` is safe on the host. Stop on the first fault/short read or denied
`/dev/mem` access; do not relax kernel protections, turn on domains, clear
interrupts, reset the AOP or read the FIFO to “see what happens.” Register
reads cannot by themselves identify retained firmware or establish attachment.

This pass intentionally excludes FIFO `0x4038`, mailbox set/clear registers,
bulk MMIO page dumps, firmware/remap writes and all syscfg/NVRAM/boot storage.
It must not kill or restart the qemu-nbd server serving the running rootfs.

### Pass B: iOS HID observation, then transport observation

If `hidutil` exists, first capture its `help` and the relevant subcommand's
help through `scripts/ipad-ssh.sh`; do not assume macOS CLI syntax on iOS.
Use a **listing/event-monitor** facility only after confirming it is present.
Otherwise build a small iOS IOHIDEventSystemClient observer on the Mac with
the existing signing/deployment workflow in a later round. It should:

- Match page0xff00/usage3 and log service registry ID, full property dictionary
  and ReportDescriptor without creating/updating persistent properties.
- Passively observe timestamp/type/sequence and signed accel values; retain
  raw input-report bytes separately when available. A normal HID event stream
  alone cannot establish raw mailbox or EPIC framing.
- Record 5–10 seconds each: screen up/down, portrait top up/down, left/right
  edge up. Log physical pose labels and the current rotation lock/display
  orientation. Check magnitude, each axis sign and cross-axis leakage.
- Record a stationary interval to measure timestamp units, cadence and batch
  delivery. An event subscription can cause iOS to enable a sensor stream;
  record that operational effect rather than calling it fully passive.

Only if the observer cannot recover needed fields: prepare a bounded
read-only kernel-memory capture/tracing plan for orchestrator review. At live
KASLR-adjusted addresses, copy existing ring descriptors/indices and data
without modifying their pointers, or instrument the already-running driver's
message/EPIC dispatch with an explicitly reviewed mechanism. **A host Ghidra
address is not an iOS live address.** A7–A11 have no EL2; the m1n1 hypervisor
cannot supply iOS driver traces. No tracing patch, kernel write, arbitrary
physical-memory scan or userclient command is authorized by this report's
read-only pass.

The desired transcript correlates TX/RX mailbox words, negotiated protocol,
endpoint map, shared-region allocation, queue headers/indices, ready service
name/channel, subtype1 descriptor query, factory property updates and subtype4
interval changes with the resulting accel report. Use existing iOS/CoreMotion
activity first; deliberate volatile rate changes are a later active test.

### Exact unknowns and exit criteria

| ID | Needs live evidence | Specific capture / decision |
| --- | --- | --- |
| U1 | AOP state at our handoff | Cold/warm/kexec path labels, original ADT and fixed status snapshots. Decide attach versus load; `running=1` alone is insufficient. |
| U2 | Actual reserved/backing memory | Resolve `0x9b000` vs `0xa0000`, backing AP/IOP addresses, firmware UUID/version and retained-region lifetime. Establish a non-overlapping reservation before any load. |
| U3 | Queue revision/ownership | Actual hello/EPMAP/version8 queue-init transcript, endpoint IDs, shared-buffer addresses/sizes and choice of tagged versus legacy descriptors; ring alignment/cache policy, watchdog and takeover/reset semantics. Verify the recovered 64-byte units. |
| U4 | Accel service and report identity | IOService chain, descriptor bytes, ready announcement channel, actual report subtype/length and relationship to service_id0x10000007. |
| U5 | Physical scale/sign/orientation | Six-pose raw payload and HID event pairs. Confirm signed16.16 g, residual screen matrix and whether firmware has already applied ARot. |
| U6 | Rate and batching | Stationary timestamps at existing rates, then a reviewed volatile enable/disable/rate transaction. Confirm key0 units, minimum/rounding and relation to keys0x16/0x5c. |
| U7 | Calibration behavior | Which optional blobs are present, firmware accepts each tag, calibration-state result, report before/after only if a controlled RAM-only test is needed. Never change factory storage. |
| U8 | Timestamp domain | Correlate subheader AOP timestamp, outer timestamp, HID/Mach timestamp and AP boottime. Decide latest-sample-only first versus buffered IIO timestamp conversion. |
| U9 | Power/IRQ behavior | Correct AIC source mapping, PMGR dependency state, latency and watchdog/error counters across idle and stream start/stop. Suspend only after ordinary sampling works. |

Some remaining questions **can be answered offline**, and should not be
mislabelled as requiring device access: the firmware container/patchbay parser,
ring implementation and queue lifecycle transitions, reg[3]'s use,
provider-specific power call chain and firmware-side ARot/calibration arithmetic.
Continue those from the retained
function/firmware anchors. Live captures provide concrete branch selection and
validate the interpretation; they do not replace static protocol checks.

After U1–U4 plus an agreed sample frame are resolved, create `a9/aop` with
`scripts/build-kernel.sh --run aop ...`; do not reset another workstream.
Build with the new Kconfig option **off**, then compile the enabled driver
without booting it. A subsequent orchestrator run can exercise: negotiate,
descriptor/calibration, start at a conservative measured rate, stop, restart,
six poses, `monitor-sensor`, and `ipad-autorotate --probe`. Pass criteria are
correct physical orientation and stale-data handling, bounded error recovery,
no uncontrolled IRQ flood, and repeatability across a cold boot. No permanent
storage writes are needed for any of these tests.

## Round 2 — passive kernel observer and evidence-gated driver groundwork

2026-10-23. Follow-up to workspace commit `647585f`. **No device access was
performed in this round.** Kernel work is isolated in Docker tree `aop`, branch
`a9/aop`, based on Hoolock `6831bc701a`; the kernel revision is recorded below.
Workspace scripts and this document are deliberately left uncommitted.

Kernel HEAD: **`6b181e7a202eb50659ccbdc988491a552e5e92db`**.
Its binding-only parent is `f0b0d5e80e0f651f397f038a33d832ef4d20d548`.
Both patches are exported in `patches/aop/round2/`, with `series` and SHA256SUMS.

### What Pass A established

The four supplied `logs/2026-10-23-aop-pass-a-*.txt` files were captured on a
**warm kexec boot** of integration `f53bf861ca` with
`apple_pmgr.kexec_keep_on=1`. All five AOP domains are genpd ON. The GPIO domain
has the existing `2100f0000.pinctrl` consumer; the other four have no direct
Linux consumers. This does not establish AOP CPU state or a cold baseline.

The first `/dev/mem` read, CPU control `0x210800044`, returned `EFAULT`.
The collector stopped correctly. No wrapper values were recovered, and no
permission bypass or relaxation of strict devmem is appropriate. Missing `dtc`
is handled by exporting the raw Linux DTB and a filesystem DT archive.

### Implemented kernel interface and its limits

`CONFIG_APPLE_A9_AOP` is **bool, default n**. With it built in, the immutable
boot parameter `apple_a9_aop.debug=1` enables root-only debugfs:

```text
/sys/kernel/debug/apple-a9-aop/status       mode 0400
/sys/kernel/debug/apple-a9-aop/preflight    mode 0400
```

`apple_a9_aop.attach=1` separately enables the **preflight only**. Neither
parameter has a runtime write interface. With debug off, probe returns before
requesting or mapping any resource. There is no hardware attach in this round.

The J71t-only observer DT node describes **five four-byte resources**, exactly
CPU control, CPU running, inbox control, outbox control and idle status from
round 1. No FIFO, interrupt set/clear, reset, remap, autoboot, SRAM or firmware
region is a resource. Page-sized MMU mappings are unavoidable; the code only
uses the five fixed `readl()` pointers, and exposes no arbitrary offset access.
There are no writes, IRQ requests, DMA allocations, timers or FIFO reads.

The node has **no `power-domains` property**, and never attaches/resumes a
power consumer. Its `apple,observe-domains` phandles identify existing PMGR
providers in child-before-parent lock order:
GPIO → AOP → BUSIF → CPU → FILTER. A small provider API records genpd status
and each raw PMGR word under all five existing genpd spinlocks. Only when
every domain has genpd ON, actual=target=15 and no reset/disable/parent-off
does it call the five-register reader. Otherwise it reports a skip, without
trying to make the block accessible. Locks exclude software power/reset
transitions; hardware auto-PM is unchanged and its bit is recorded. They
cannot exclude autonomous gating or turn a wedged bus transaction into a
bounded read. Opt in only on the identified board, with a recovery path ready.

**Timing qualification:** these are Linux-time snapshots after normal PMGR
probe. The pre-existing DTS already marks AOP `apple,always-on`; PMGR may
therefore have changed the cold inherited domain state before the snapshot.
The observer changes none of that policy. Integration's warm hold also changes
domain policy. Record boot path and original boot ADT separately; neither an
ON summary nor `cpu_running_bit=1` proves iBoot loaded the image or establishes
firmware identity/ownership. An exact pre-PMGR handoff trace remains separate
work if the two Linux-time snapshots are ambiguous.

Expected successful status shape (hex values must be measured, not copied):

```text
format=1 boottime_ns=<measured> hardware_writes=none
domain=aop_gpio genpd=0 raw=<measured> actual=15 target=15 auto=<0-or-1>
... four more domains ...
snapshot_errno=0
cpu_control address=0x210800044 value=<measured>
cpu_running address=0x210800b88 value=<measured>
inbox_control address=0x210804008 value=<measured>
outbox_control address=0x210804020 value=<measured>
idle_status address=0x210804040 value=<measured>
cpu_running_bit=<0-or-1>
inbox_faults=<measured> outbox_faults=<measured>
```

Genpd 0 means ON, 1 means OFF. `snapshot_errno=-112` / `wrapper=skipped`
means a power/reset/transition predicate failed; `-517` means a provider is
not ready. Preserve the raw state and **stop that pass**, rather than forcing
power, binding a fake consumer or relaxing `/dev/mem`. A missing debugfs
directory means wrong build/DT/board, debug parameter off or probe failure;
inspect config/cmdline/dmesg before proceeding.

### Firmware extraction and Linux lookup

`scripts/periph/aop/extract-firmware.py` is host-only, uses the already-downloaded
IM4P by default, and also accepts `--ipsw /absolute/path/to/20H350.ipsw`.
It does not download anything. It verifies the round-1 component and bundle
SHA256s, parses DER and a bounded unindexed OSSerialize subset, rejects
truncation, forward/cyclic references, duplicate keys and inconsistent
collection endings, and preserves both firmware entries plus metadata.
The token layout follows Apple's
[xnu-8796.141.3 OSSerializeBinary.cpp](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/libkern/c%2B%2B/OSSerializeBinary.cpp).

```sh
python3 -B scripts/periph/aop/extract-firmware.py
# Output staging root: build/periph/aop-r2/firmware/
```

| Relative to `/lib/firmware/` | Bytes | SHA256 |
| --- | ---: | --- |
| `apple/aop/ipad6faop-20H350.bin` | 342336 | `af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8` |
| `apple/aop/ipad6faop_ext-20H350.bin` | 37024 | `2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22` |

The staging directory also contains the original verified `.osbin` and a JSON
inventory. Bundle keys are `firmware[]/{data,header}` and top-level `header`,
whose identifier is `AOPfw`, version `1`. Main header has coprocessor `AOP`,
target `ipad6faop`, build Release, version
`AppleSPUFirmwareBuilder-427.100.4~18465`. External header has coprocessor
`AOP_ext`, target `ipad6faop_ext`, segment `__ETEXT`, tag **`0x454330`**, address
**`0x0106e000`**. This address is an IOP-side mapping field, **not an AP physical
copy destination**. The main data starts with ARM32 vectors; neither extracted
file is the whole serialized container or a Mach-O image.

Additional static anchors retained under `build/periph/aop-r2/ghidra/`:
`RTBuddyFirmware_fromSerializedEntry` at `fffffff005b6eba8`,
`RTBuddyFirmware_initDataHeader` at `fffffff005b6eee4`,
`RTBuddyFirmware_getCoprocessor` at `fffffff005b6f130`, and
`AppleSPUFirmwareService_stageExternalSegment` at `fffffff005b88090`.
The latter allocates backing, copies the external image, flushes processor
cache, and retains tag/length/IOP address in a bounded table. These understood
functions were renamed/commented in the shared Ghidra project and saved.
They strengthen the case for preserving external metadata; they do not resolve
the main remap, patchbay, AP/IOP translation or reservation contract. The
round-1 bounded kernelcache search found no complete embedded matching payload;
the extractor uses the actual IPSW component rather than inventing one there.

### RTKit/SPU and IIO progress

The source contains tested helpers for endpoint63:56/payload55:0 packing
(rejecting overflow), management hello/version12, bounds-checked tagged
queue descriptors in 64-byte units, old outer/subheader v2 parsing including
the unaligned timestamp at10, ready-name32/service ID parsing, subtype4
property notifications and correlated IOReturn replies, and the signed
22-byte accel report. Stream plans encode **batch key0x16, raw key0x5c,
report key0**, in order; report=0 builds a stop plan. Enable plans require
verified units and still require a measured supported rate and a reply before
each next transaction. No arbitrary 20 Hz default is sent.

This code is **not connected to an MMIO mailbox**. Existing 96-bit ASC/RTKit
clients are unchanged. A future transport must remove the high-byte endpoint,
handle management's four-bit type separately from SPU's eight-bit type, and
audit every generic RTKit management/shared-buffer field before reuse. It
must establish queue headers, ownership, cache ordering and IOP translation;
descriptor bounds alone are not a ring implementation. Discover endpoint and
channel through hello/EPMAP and ready announcements. Never treat the saved
service ID `0x10000007` as an endpoint.

`CONFIG_APPLE_A9_AOP_IIO` is separately default n. Its **dormant consumer**
implements direct accel axes, `0.000149638` m/s²/count scale, read-only current
sampling frequency, and the IIO mount-matrix helper. It registers only when
called by a future negotiated SPU service with a real first report and explicit
verified scale, calibration, interval and ARot evidence. **No caller exists
yet, so enabling both options creates no IIO accelerometer.** Malformed data
invalidates cached samples; transport failure/stream stop must invoke its
invalidate function. Stale reads return `-ENODATA` after max(1 second, five
report intervals), using AP receipt time only for freshness.

ARot v1 candidate decoding checks 16 bytes, version1, zero row padding and
a signed permutation matrix. The saved blob decodes to `-I` **under this still
unverified layout**. A caller must independently verify it before registration.
If firmware applies ARot as iOS does, the consumer requires a measured residual
screen matrix instead of exposing ARot a second time. No per-device calibration,
unverified `-I` mount matrix or arbitrary verification override is placed in
public DTS or exposed as a boot parameter. A working sensor still needs U2–U8.

First-attach decision states are deliberately bounded:

1. Unknown/off/transitioning domains → report and skip wrapper access.
2. Running CPU, regardless of cold/warm provenance → preserve firmware/remap/
   queues; refuse unknown retained-memory ownership. **No hello replay, FIFO
   drain, interrupt acknowledge, reset, watchdog or power-state message.**
3. Stopped CPU → `request_firmware_direct()` requests both pinned filenames,
   reports sizes, releases the files, then refuses missing reservation/remap/
   patchbay. File-size checks are inventory checks; host SHA validation supplies
   provenance. **No copy to AOP memory and no CPU release.**
4. Later active driver: only after the corresponding memory/ownership gate is
   resolved, use bounded IRQ/workqueue and transaction timeouts, negotiate
   management12/SPU8, establish queues, identify accel, query descriptor,
   apply present calibration in verified order, measure intervals, enable and
   decode reports, then admit IIO after six-pose scale/matrix validation.

### Ordered orchestrator plan — commands below have NOT been run

**0. Integrate and package on the host.** Integrate the `a9/aop` commit through
the normal integration workflow, preserving all of `integ`'s NVMe write-window,
PSCI/kexec, PMGR warm hold and recovery changes. The standalone `aop` image is
an offline build artifact, **not a replacement for the protected integration
image on this device**. After integrating (stop on a merge conflict):

```sh
scripts/build-kernel.sh --run integ '
scripts/config -e DEBUG_FS -e IIO -e APPLE_A9_AOP -e APPLE_A9_AOP_IIO
make LLVM=1 ARCH=arm64 olddefconfig
'
scripts/build-kernel.sh --tree integ
python3 -B scripts/periph/aop/prepare-tests.py --tree integ \
  --base-cmdline build/boot/integ-entry/cmdline
source build/periph/aop-r2/entries.env
```

The packaging script requires the integrated config and protective baseline
arguments, preserves the other known-good arguments, and produces unique
entry names, `passive.cmdline` (debug1/attach0), `attach.cmdline` (debug1/attach1)
and **`build/periph/aop-r2/m1n1-aop-passive.bin`**. It does not contact the iPad,
alter the default payload/entry or enable a sensor. Keep the exact integration
COMMIT and DTB paired with these files throughout the test.

**1. Cold-path capture first.** Deliberately enter DFU/checkm8 through the
owner's normal cold-boot procedure, with no iOS run since that entry, and record
the precise path. Distinguish a hard restart/DFU entry from proven removal of
AOP power; “DFU” by itself does not prove the latter. With the autoboot helper
available and not paused, send this specific payload:

```sh
LINUX_A9_PAYLOAD="$PWD/build/periph/aop-r2/m1n1-aop-passive.bin" \
  scripts/boot/autoboot.sh --once
# After the normal Linux USB/SSH readiness indication:
bash scripts/periph/aop/capture.sh cold
TREE="$AOP_TREE" scripts/boot/stage-kernel.sh "$AOP_PASSIVE" \
  --cmdline "$(cat build/periph/aop-r2/passive.cmdline)"
```

Capture writes under ignored `build/periph/aop-r2/live/cold-<UTC>/`: board,
boot ID/cmdline/kexec marker, root/CPU state, genpd before/after, five-register
status, interrupts/iomem, dmesg, raw Linux DTB and filesystem DT tar. No device
`dtc` is needed. Expect the cold capture's kexec marker absent and attach0;
record contradictory provenance instead of relabelling it. Keep the original
boot ADT separately. Stop for missing status/debugfs, a skip/error, reset/fault
bits, changed power domains, kernel warning/oops, unresponsive SSH or an IRQ
flood. Do not continue by forcing a domain on.

**2. Warm capture of the same kernel/DT, still passive.** After reviewing a
clean cold capture and staging its exact entry above:

```sh
ENTRY="${AOP_PASSIVE}-warm" scripts/boot/kexec-host.sh \
  "build/kernel-$AOP_TREE/Image.gz" \
  "build/kernel-$AOP_TREE/dtbs/s8003-j71t.dtb" \
  "$(cat build/periph/aop-r2/passive.cmdline)"
bash scripts/periph/aop/capture.sh warm
```

The existing helper verifies a **new boot ID** and expected entry, and preserves
the default recovery entry. Expect kexec marker present, debug1/attach0, both
CPUs, expected root, warm PMGR hold and the five measured values. The distinct
`-warm` entry avoids an immutable-entry conflict with the cold staged entry's
manifest. Compare running bit, control flags, idle and raw PMGR states; a warm
CPU-running bit does not validate retained queues or justify attachment.

**3. First attach preflight, only after both passive passes are clean.**
Stage the already-verified firmware files in the ordinary Linux firmware
directory (not syscfg/NVRAM/LLB/iBoot). This may write the Linux root filesystem
within its existing write window; it does not install boot firmware.

```sh
python3 -B scripts/periph/aop/extract-firmware.py
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata \
  -C build/periph/aop-r2/firmware/apple/aop -cf - \
  ipad6faop-20H350.bin ipad6faop_ext-20H350.bin | \
  scripts/omarchy/ssh.sh 'sudo -n mkdir -p /lib/firmware/apple/aop; sudo -n tar -xf - -C /lib/firmware/apple/aop'
scripts/omarchy/ssh.sh 'sha256sum /lib/firmware/apple/aop/ipad6faop-20H350.bin /lib/firmware/apple/aop/ipad6faop_ext-20H350.bin'
```

Verify both printed hashes against the table before proceeding:

```sh
ENTRY="$AOP_ATTACH" scripts/boot/kexec-host.sh \
  "build/kernel-$AOP_TREE/Image.gz" \
  "build/kernel-$AOP_TREE/dtbs/s8003-j71t.dtb" \
  "$(cat build/periph/aop-r2/attach.cmdline)"
bash scripts/periph/aop/capture.sh preflight
```

Reading `preflight` is the explicit first attempt; its parameter merely enables the
read-only check. Expected output begins
`hardware_writes=none fifo_reads=none irqs=none dma=none`, followed by either:

```text
case=running result=blocked-retained-memory-and-host-ownership
action=retain-firmware-no-hello-no-fifo-no-reset
```

or, for a stopped CPU, both expected sizes and
`result=blocked-memory-reservation-remap-and-patchbay` /
`action=no-load-no-cpu-release-no-mailbox`. Missing firmware gives
`result=blocked-firmware` with the real errno; wrong size gives
`blocked-firmware-size`. These are intentional stop results, **not successful
RTKit attachment**. Retain logs and resolve the indicated gate offline; do not
add a force-attach override or change CPU/reset/FIFO state to continue.
Mailbox fault bits18/19 stop preflight earlier with `blocked-mailbox-fault`;
they are never acknowledged or cleared.

> Private graphics-dependent excerpt omitted from this source snapshot.

```sh
scripts/omarchy/ssh.sh 'sudo -n linux-a9-kexec reboot default'
```

Never reset/power-cycle the AOP as an error handler in these passes. Stop
immediately on AP faults/hangs, IRQ storms, disappearing root/SSH, unexpected
mailbox consumption or any report of hardware writes. Never kill the live
qemu-nbd/QEMU server by process name. No command here writes syscfg, NVRAM or
boot-firmware storage.

### Offline verification and handoff

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build.sh off
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py off'
bash scripts/periph/aop/build.sh on
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --integration-base f53bf861ca'
```

Both full Docker `Image.gz` + **104 DTB** builds passed, debug info disabled.
Checks verify the exact five resources, all other iPad5 variants excluded,
default-off symbol absence, enabled observer/IIO linkage, immutable runtime
gates and source absence of write/FIFO/IRQ/DMA/PM-consumer operations in the
observer API. Host C tests compile the actual protocol header with endian
compatibility shims and ASan/UBSan: high-byte codec, hello12 rejection, bounded
queue descriptors, unaligned/truncated/oversized packets, property ordering and
reply tags/errors, signed motion decoding and candidate ARot validation, plus
100,000 deterministic parser trials. Firmware tests pin both images and reject
malformed DER/OSSerialize. These tests do not establish hardware behavior,
firmware loading, rate units, physical scale/orientation or working IIO.

A read-only `git merge-tree` simulation against Pass A `f53bf861ca` is clean;
no integration branch/ref was changed. A separate J71t DT fragment and stable
Kconfig/Makefile insertion points avoid the original append-location conflicts.
The installer was checked for idempotence. Driver-source strict checkpatch has
no findings; the schema YAML parses and DTBs compile, but dt-schema itself was
not installed, so schema validation was not run.

Build/check logs, configs, firmware inventory and Ghidra replies are retained
under `build/periph/aop-r2/`. Final committed-source logs are
`build-{off,on}-commit.log` and `check-{off,on}-commit.log`; the exported
`build/kernel-aop/COMMIT` identifies HEAD and its `DIRTY` file is empty.
No workspace commit or push was made.

## Round 3 — active driver

2026-10-23. Implements the active driver specified in
[`ds-aop-accel.md`](ds-aop-accel.md), corrected by a further static RE pass
(below). **No device access was performed in this round.** Kernel work is on
branch `a9/aop` (Docker tree `aop`), on top of round 2 `6b181e7a20`:

| Commit | Subject |
| --- | --- |
| `1a0464202ba185bcfcef1c89eb8a2ea39eb0861d` | dt-bindings: soc: apple: describe J71t AOP for the active driver |
| **`217bed70c3276a47effd5a80d70a4b646bfb62e5`** | soc: apple: add opt-in A9 AOP active driver and IIO accelerometer |

Both are exported in `patches/aop/round3/` with `series` and `SHA256SUMS`.
Templates live in `scripts/periph/aop/kernel/` and are installed by
`install-kernel.py`. Workspace files are left uncommitted.

### Static RE corrections to ds-aop-accel.md

A second offline pass over AppleSPU/RTBuddy/AppleA7IOP (shared Ghidra
project, functions renamed/commented and saved; working note
`build/periph/aop-r4/re-cold-load.md`) changed several spec statements.
The driver follows the corrected column. [E] = read from code.

| Topic | ds-aop-accel.md said | Code says | Evidence |
| --- | --- | --- | --- |
| Ring header owner | AP initialises bufsize/marker/rptr/wptr | **AOP** writes it; AppleSPU builds the queue with `init=0` and only asserts `hdr[0] == size - 0xc0`; v7 entries iff `(hdr[4] & 0xffff0000) == 0x70000`. Linux never writes ring headers. | `baaf2c` → `b8465c` → `bab1c8` [E] |
| Ring entry | magic, size, first 8 EPIC bytes at +8, rest at +0x10 | `+0` magic, `+4` payload length (excludes the 16-byte entry header), **`+8` channel**, `+0xc` word (0 on host TX), **whole EPIC packet at `+0x10`**; stride `align(16 + len, 0x40)` | reader `baba00`, writer `bab540`, TX `ba8398`/`ba8368`/`bae344` [E] |
| Wrap | split copy | never split: writer puts the entry at 0 and a header copy at the old wptr; reader takes the length from the tail header and reads at 0 | `baba00`, `bab540` [E] |
| rptr / doorbell | — | reader writes rptr back per entry, no message; writer publishes wptr, re-reads rptr, sends bare `0xa2` **only if old wptr == rptr** | `babd00`, `bab130` [E] |
| Reader magic | reject non-`AOP `/`IOP ` | iOS does not check the magic (driver counts surprises only) | `baba00` [E] |
| EPIC outer +4 | channel [I] | zero on TX (`baf0fc`); the channel travels in the ring entry | [E] |
| Ext segment | at IOP `0x0106e000` | separate page-aligned **DRAM** buffer; tag `EC0p` = its physical address (`EC0v`/`EC0s` absent on j71t) | `b88090`, `b88238` [E] |
| Tag table writes | AOPt | AOPt=8, nCal=0, EC0p, WrAd=wrapper PA, McRA=0x4000, SOC_=chip-id, SOCR=chip-revision, DEVf=0, STKG/PRNG random, RTTO; NONC/ECID only when `/chosen unique-chip-id` exists (not on Linux) | `b88598`, `b88930`, `b70290`, `b70044`, `b700f4`, write-back `b6f9b4` [E] |
| Region | 0x9b000 vs 0xa0000 open | target = `region-base`/`region-size` = 0x210e00000/0x9b000; image copied to +0, **no BSS clear**; remap IOP 0 → region, length 0x9b000 | `b68cdc`, `b6f340`, `b4e72c` [E] |
| STARTEP | `…0002 \| ep<<32` [I] | confirmed | builder `b717a4` [E] |
| App endpoints | not 0..4,6,7,8,10,11 | `>= 0x20` (RTBuddyEndpointService); system endpoints are STARTed by iOS on EPMAP | `b661e4` [E] |
| Boot complete | — | firmware sends mgmt type 7 with low byte `0x20`; AP sends no type 6 on a cold boot | type-7 handler, `b71fc4` [E] |
| Mask registers | — | `+0x4000` set / `+0x4004` clear [I]; handlers never touch them; bit0 ↔ AIC 130, bit12 ↔ AIC 133 [I] | `b4e530`/`b4e548`/`b4e5a8` [E] |
| After CPU release | — | iOS polls nothing; no timebase sync on j71t (`force-timebase-sync` absent) | `b4b738`, `b4e850` [E] |

### What the driver does

Config: `CONFIG_APPLE_A9_AOP_ACTIVE` (bool, default n, depends on
`APPLE_A9_AOP_IIO`, which now selects `IIO_TRIGGERED_BUFFER`). Runtime gate:
**immutable** `apple_a9_aop.active=1` (same prefix as the observer's
`debug`/`attach`); without it probe returns `-ENODEV` before mapping anything.
With `apple_a9_aop.debug=1` as well, root-only debugfs:

```text
/sys/kernel/debug/apple-a9-aop-active/state       phase, result, counters, endpoints, buffers, rings, services, last sample
/sys/kernel/debug/apple-a9-aop-active/transcript  last 512 mailbox words: index, boottime, R/T, ep, 56-bit payload, status
/sys/kernel/debug/apple-a9-aop-active/rings       ring headers (+0/+0x40/+0x80) and the last 256 entry bytes
/sys/kernel/debug/apple-a9-aop-active/patchbay    the patched firmware tag table as written to the region
```

The observer keeps `/sys/kernel/debug/apple-a9-aop/{status,preflight}`
(**hyphens**; `apple_a9_aop` with underscores is only the parameter prefix and
never a debugfs path). The observer's only change is an accessor for `debug`.

DT: new `s8003-j71t-aop.dtsi`, node `/soc/aop@210800000`,
`compatible = "apple,j71t-aop"`: wrapper `0x210800000/0x1c000` (mapped, not
claimed, because the observer claims 4-byte sub-ranges), region
`0x210e00000/0x9b000`, auto-boot `0x210000600/4`, AIC 133 `rx` / 130 `tx`
(only `rx` is requested), the five PMGR domains as `apple,observe-domains`
(no `power-domains`: `ps_aop` is always-on and holds busif/cpu/filter;
pinctrl holds gpio), `apple,aop-target = <8>`, `apple,chip-id = <0x8003>`,
`apple,chip-revision = <1>` (live IOService value). No `mount-matrix` yet.

Bring-up is a work item that starts at probe and waits up to 180 s for
`/lib/firmware` (built in; the root filesystem arrives after probe):

1. Snapshot the five wrapper registers under the observer's all-domains-ON
   predicate. Result strings, each a stop:
   `refused-domain-state`, `refused-mailbox-fault`,
   `refused-running-no-pending-hello` (running CPU, empty outbox: a previous
   AP owns it), `refused-stopped-outbox-not-empty`, and, for a running CPU
   with pending words, `refused-running-transcript-not-hello` if the first
   word is not the device HELLO (the outbox is not read after that).
2. Cold (CPU stopped): wait for the files, allocate the ext buffer, patch the
   tags, re-check that the CPU is still stopped, copy and read back the image
   at region+0, program the remap, mask all mailbox interrupts then unmask
   bits 0 and 12 (as iOS), enable the outbox, enable AIC 133, set CPU control
   bit 4 and write 1 to auto-boot. No reset, no polling, no type-6.
3. HELLO → `0x00200000000c000c`; each EPMAP chunk → reply as iOS
   (`low32 = base == 0`, last bit mirrored); after the last chunk AP power
   `0x00a0000000000001` [I] and a 1 s ping (`5 s` without ack → stream
   invalidated, AOP left alone).
4. Wait up to 2 s for IOP power state `0x20` (logged if absent), then
   STARTEP each endpoint `>= 0x20`. **System endpoints are deliberately not
   started** (iOS does; their buffer-request formats are unresolved).
5. Per endpoint: wait for `0x80`/version 8 (everything before it is dropped
   and counted), reply `0xa0`; `0x89` → `dma_alloc_coherent`, reply
   `0xa1 | pa`; `0x8a`/`0x8b`/`0x8c` → validate against the buffer and check
   the AOP-written header (`failed-ring-header` otherwise); with TX and RX
   present send `0xa3`; `0x86` → running; `0x85` → read the RX ring.
6. Ready report `0xc0` named `accel` with service id `0x10000007` binds the
   stream (channel = entry +8). Send the optional per-device calibration file
   (keys in iOS order), then batch `0x16`=100000, raw `0x5c`=0, report `0`=100000,
   each waiting up to 2 s for its correlated reply (IOReturn 0).
7. The first `0xc2` report registers IIO `apple-a9-aop-accel`.

IIO ABI: `in_accel_{x,y,z}_raw` (AOP report counts), `in_accel_scale`
`0.000149638` (16.16 g → m/s²), `in_accel_sampling_frequency` `10` (read-only),
`in_accel_mount_matrix` (DT `mount-matrix`, identity until measured; ARot is
applied in firmware and never again on the AP), `-ENODATA` before the first
report, after a transport failure and when older than 1 s. Triggered buffer
with the device's own trigger `apple-a9-aop-accel-dev<N>`, fired once per
report, timestamps = AP receipt time; iio-sensor-proxy can use either its
buffer or poll path.

Shutdown/kexec: rate 0/0/0, then AFK `0xa4` to every started endpoint, waiting
200 ms each for `0x87`/`0xc3`. The rings are ordinary DRAM the next kernel
reuses; a missing ack is logged ("AOP may still own its rings") and means:
**do not kexec, cold boot.**

### Firmware files (Linux root filesystem only)

| `/lib/firmware/` path | Source | Notes |
| --- | --- | --- |
| `apple/aop/ipad6faop-20H350.bin` | `extract-firmware.py` | 342336 B, `af5d51a3…` |
| `apple/aop/ipad6faop_ext-20H350.bin` | `extract-firmware.py` | 37024 B, `2528d268…` |
| `apple/aop/accel-calibration.bin` | `make-calibration.py` | **per-device**, optional, 132 B on this unit |

`scripts/periph/aop/make-calibration.py` reads the accel node of
`build/firmware/private/IODeviceTree.txt` and writes
`build/periph/aop-r4/firmware/apple/aop/accel-calibration.bin` (mode 0600):
`"A9AC"`, version 1, then LTAO `0x1a` (20 B), ASCl `0x0b` (36 B), ARot `0x18`
(16 B), AICl `0x19` (16 B); `accel-range-sensitivity-cal` is absent on this
unit and is skipped. It is factory calibration of this iPad: never commit it,
never put it in a public DTS, never write syscfg. Without the file the
driver streams uncalibrated and warns that axes are in the sensor frame.

### Offline verification

```sh
python3 -B scripts/periph/aop/test-offline.py
bash scripts/periph/aop/build.sh off
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py off'
bash scripts/periph/aop/build.sh on
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --integration-base f53bf861ca'
```

`tests/afk.c` (ASan/UBSan, compiled from the kernel header unchanged) covers
EPMAP/STARTEP/ping/AP-power/IOP-power words, `0x89`/`0xa1` encode and the
48-bit limit, AOP-owned v7 header checks, writer/reader loopback, wrap with
the tail header copy, every writer space rule, malformed entries, 50,000 ring
fuzz trials, the property notify layout, ready reports with interface
numbers, the real image's tag table (every patched tag present with the size
the driver writes) and the calibration file. `check-build.py` additionally
checks the j71t-only active node (exact reg/interrupts/properties, no
power-domains/status/mount-matrix), the immutable `active` gate before any
`devm_` call, the installed templates and the linked symbols in both configs.

Build results at the committed HEAD `217bed70c3` (debug info off):
`test-offline.py` PASS (protocol, afk and extractor suites); full Docker
`Image.gz` + **104 DTBs** for both `off` and `on`, no compiler warnings
in the `on` log; `check-build.py off`, `on` and `on --integration-base
f53bf861ca` (read-only merge simulation, clean) PASS;
`build/kernel-aop/COMMIT` = HEAD with an empty `DIRTY`. W=1 compile of the
AOP objects is clean; strict checkpatch reports only MACRO_ARG_REUSE checks
for the wait macro and the IIO channel macro. Logs:
`build/periph/aop-r4/{build,check}-{off,on}-commit.log`, `check-merge-commit.log`.

### Device test plan — commands below have NOT been run

Prerequisites: both round-2 passive passes are still pending (the round-2
captures read `/sys/kernel/debug/apple_a9_aop/`, a path that never exists, so
they prove nothing). This plan includes them as step 2.

**0. Integrate and package (host).** Integrate `a9/aop` up to `217bed70c3`
through the normal workflow, preserving `integ`'s protections (stop on a
conflict), then:

```sh
scripts/build-kernel.sh --run integ '
scripts/config -e DEBUG_FS -e IIO -e APPLE_A9_AOP -e APPLE_A9_AOP_IIO -e APPLE_A9_AOP_ACTIVE
make LLVM=1 ARCH=arm64 olddefconfig
'
scripts/build-kernel.sh --tree integ
fdtget build/kernel-integ/dtbs/s8003-j71t.dtb /soc/aop@210800000 compatible   # apple,j71t-aop
python3 -B scripts/periph/aop/prepare-tests.py --tree integ --active \
  --base-cmdline build/boot/integ-entry/cmdline
source build/periph/aop-r2/entries.env; source build/periph/aop-r4/entries.env
cat build/periph/aop-r4/active.cmdline   # ... apple_a9_aop.debug=1 apple_a9_aop.attach=0 apple_a9_aop.active=1
```

The cold-boot payload `build/periph/aop-r4/m1n1-aop-active.bin` carries the
integrated DTBs, so it includes the `aop@210800000` node. Keep the integration
COMMIT and DTB paired with every capture.

**1. Stage firmware (Linux root filesystem only, existing write window).**

```sh
python3 -B scripts/periph/aop/extract-firmware.py
python3 -B scripts/periph/aop/make-calibration.py
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -cf - \
  -C build/periph/aop-r2/firmware/apple/aop ipad6faop-20H350.bin ipad6faop_ext-20H350.bin \
  -C "$PWD/build/periph/aop-r4/firmware/apple/aop" accel-calibration.bin | \
  scripts/omarchy/ssh.sh 'sudo -n mkdir -p /lib/firmware/apple/aop; sudo -n tar -xf - -C /lib/firmware/apple/aop; sudo -n chmod 0600 /lib/firmware/apple/aop/accel-calibration.bin'
scripts/omarchy/ssh.sh 'sha256sum /lib/firmware/apple/aop/*'
shasum -a 256 build/periph/aop-r4/firmware/apple/aop/accel-calibration.bin
```

Compare all three hashes (the two images against the table above) before
continuing.

**2. Passive passes with the integrated kernel (no active parameter).** Run
round 2 steps 1–2 (`capture.sh cold`, then `warm`). `capture.sh` now stops
with an explicit message if `/sys/kernel/debug/apple-a9-aop` is missing.
Record `cpu_running_bit` and outbox empty (bit 17 of `outbox_control`) for
the cold path. Expected for the cold path: `cpu_running_bit=0`, outbox empty
(the AOP firmware is not loaded by iBoot). If cold shows `cpu_running_bit=1`,
**stop**: the active driver will refuse anyway; report the values.

**3. First active boot: cold path, debug on.** Enter DFU/checkm8 through the
normal cold procedure with no iOS run since, then:

```sh
LINUX_A9_PAYLOAD="$PWD/build/periph/aop-r4/m1n1-aop-active.bin" scripts/boot/autoboot.sh --once
# after SSH is up (the driver waits for /lib/firmware after the root switch):
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -i aop'
bash scripts/periph/aop/capture.sh active
```

Bring-up starts only after the root switch, then waits for the firmware
files. Poll line 1 of the state file until `phase` is `streaming`, `refused`
or `failed` (normally a few seconds after SSH is up):

```sh
scripts/omarchy/ssh.sh 'for i in $(seq 60); do sudo -n head -n1 /sys/kernel/debug/apple-a9-aop-active/state; sleep 2; done' | uniq
```

`phase=wait-firmware` for more than ~30 s after SSH means the files are not
on the root filesystem (step 1); `phase=probe` means the PMGR snapshot has
not run. Then capture.

`capture.sh active` writes `build/periph/aop-r4/live/active-<UTC>/` with
`active-{state,transcript,rings,patchbay}.txt`, `iio.txt`, dmesg, genpd,
interrupts and the DT. Read `active-state.txt` first; line 1 is
`format=1 phase=<phase> result=<result> errno=<n> path=cold`.

Expected transcript order (`active-transcript.txt`, `R` = from AOP):

```text
R ep=0x00 0010000000MMmm      HELLO, min <= 12 <= max
T ep=0x00 200000000c000c      HELLO reply
R ep=0x00 80...               EPMAP chunk(s); last one has bit 51
T ep=0x00 80...               EPMAP replies
T ep=0x00 a0000000000001      AP power
T ep=0x00 30...               pings every second (R 40... acks)
R ep=0x00 70...20             IOP power state 0x20 (if absent: warning, continue)
T ep=0x00 5000EE00000002      STARTEP per endpoint EE >= 0x20
R ep=0xEE 80000000000008      AFK init v8
T ep=0xEE a0000000000000
R ep=0xEE 89...  T ep=0xEE a1<pa>   buffer request / reply (pa matches state "buf")
R ep=0xEE 8a... 8b... (8c...)       ring descriptors
T ep=0xEE a3000000000000      AFK start
R ep=0xEE 86000000000000      start ack
R ep=0xEE 85...               ring notifications: ready reports, replies, 0xc2
T ep=0xEE a2000000000000      doorbell per command (only when the AOP had drained)
```

Pass criteria, in order, each with the debugfs line to check:

| Check | Where | Pass |
| --- | --- | --- |
| HELLO answered | transcript | first R word on ep 0 type 1, reply `200000000c000c` |
| Endpoint map | `state` `endpoints=` | at least one endpoint `>= 0x20` |
| AFK running | `state` `ep N state=running` | per started endpoint |
| Ring headers | `state` `ring rx ... valid=1 hdr_version=0x0007....` | bufsize = size - 0xc0 |
| Accel service | `state` `service name=accel ... service_id=0x10000007 bound=accel` | channel recorded |
| Calibration | `state` `accel cal_sent=4 arot_sent=1` | all four replies 0 |
| Stream | `state` `phase=streaming`, `reports=` grows ~10/s across two reads 10 s apart | |
| Rate | `active-state.txt` read twice, 10 s apart: Δreports ≈ 100 | report interval in µs confirmed |
| IIO | `iio.txt` | name `apple-a9-aop-accel`, raw values change when the iPad is tilted |

Stop and keep the capture on: any `refused-*`/`failed-*` result,
`fault_control != 0`, `irq_storm=1`, `rx_errors > 0` (then look at `rings`),
`magic_unexpected > 0`, kernel warnings/oops, SSH loss or an IRQ count that
grows without RX words. Failure meanings: `failed-no-hello` (firmware did not
start: region/remap/tags; keep `patchbay.txt`), `failed-ring-header` (header
not written before the descriptor; keep `rings.txt`),
`failed-no-accel-service` (keep `state` services list), `failed-configure`
(reply missing or non-zero IOReturn), `failed-no-report`.

**Never** reset the AOP, clear faults, force a domain or retry with a new
parameter on the same boot. A failed cold boot leaves the AOP running: the next
boot of the same kernel will report `refused-running-no-pending-hello`, which
is the designed outcome. Recover with a cold DFU entry (kexec does not reset
the AOP).

**4. IIO and userspace.**

```sh
scripts/omarchy/ssh.sh 'd=$(grep -l apple-a9-aop-accel /sys/bus/iio/devices/iio:device*/name | xargs dirname); \
  for i in 1 2 3; do cat $d/in_accel_x_raw $d/in_accel_y_raw $d/in_accel_z_raw; sleep 1; done; \
  cat $d/in_accel_scale $d/in_accel_sampling_frequency $d/in_accel_mount_matrix; ls $d/scan_elements $d/trigger'
scripts/omarchy/ssh.sh 'timeout 20 monitor-sensor --accel'
scripts/omarchy/ssh.sh 'ipad-autorotate --probe'
```

Flat on a table, screen up: one axis ≈ ±65536 counts (≈ 9.8 m/s² after
scale), the others ≈ 0. Magnitude far from 65536 means the 16.16 g scale is
wrong; record raw values.

**5. Six poses → residual mount matrix.** With `monitor-sensor` and
`state` (`accel last ... x= y= z=`) logged, hold 10 s each: screen up, screen
down, portrait top up, portrait top down, left edge up, right edge up. Derive
the residual matrix (screen frame from AOP frame, after firmware ARot) and add
it as `mount-matrix` to the j71t node in a later commit; until then IIO
exposes identity. Then `scripts/omarchy/test-autorotate.sh --hardware`.

**6. Stop/kexec behaviour.** Kexec to the default entry with netconsole or
the console attached and confirm `stopped (rate stop 0)` without "no
shutdown ack". The next kernel (active=1) must report
`refused-running-no-pending-hello`; that is correct. If any shutdown ack is
missing, cold boot instead of kexec.

**7. Warm-after-iOS (optional).** Boot iOS, then Linux with active=1 through
the warm path: expected `refused-running-no-pending-hello` (iOS owns the
firmware). Do not try to force attach.

No step writes syscfg, NVRAM or boot firmware storage. Never kill the
qemu-nbd/QEMU serving the root by name.

### Known gaps and inferences carried into the first run

- AP power `0xa` after EPMAP is [I] (iOS sends it "optionally").
- System endpoints are not started; if the firmware stalls before IOP power
  0x20 or before AFK init, the next change is to start them and answer their
  buffer requests (format still [U]).
- `RTTO = 0` [I]; NONC/ECID left at defaults (iOS does the same without
  `unique-chip-id`); NClk left at 24 MHz.
- Mask register direction (`+0x4000` set / `+0x4004` clear) is [I]; the
  driver writes the same values as iOS once, never at runtime.
- Rate units (µs), 16.16 g scale and the mount matrix are [I] until steps 3–5.
- The historical ring (`0x8c`) is attached when offered but never read.
- EPMAP reply low32 is `base == 0` (iOS); if the transcript shows the
  firmware re-sending chunks, try the mainline convention (bit 0 = "more" on
  non-last chunks).
- If `failed-configure` appears, the `state` line `command ... last_reply`
  shows what (if anything) answered the property tag.

### Round 3 device run 1 (2026-10-23, warm path)

> Private graphics-dependent excerpt omitted from this source snapshot.

### Round 3 device run 2 (2026-10-23, true cold boot)

DFU -> checkm8 -> pongoOS -> m1n1 -> Linux straight into m1n1-aop-active.bin
(kernel 50ad73f524), no warm reboot. Same refusal at 2.28 s:
`refused-running-no-pending-hello (-16)`. The AOP is running and idle before
Linux on a cold boot too, so the pre-Linux boot chain starts it, not our warm
reboots. Cold vs warm difference: AOP domains raw 0x1f0000ff/0x1f0002ff with
auto=1 on cold, 0x0f.. with auto=0 after kexec. CPU/mailbox state identical
(cpu_running 0x00020001, mailboxes empty, idle 0x79, no faults). Capture:
build/periph/aop-r4/capture-active-cold-1.log. The driver needs an "attach to a
running AOP" path (docs/periph/ds-aop-takeover.md, in progress).

## Round 4 — attach

2026-10-23. Implements [`ds-aop-takeover.md` §3](ds-aop-takeover.md#3-q3--a-safe-attach-for-our-driver)
on kernel branch `a9/aop`, above `217bed70c3`. No device access and no
workspace commits. Kernel patches are in `patches/aop/round4/` with `series`
and `SHA256SUMS`; mirrored sources/tests remain under `scripts/periph/aop/`.
Kernel HEAD: **`9c64a50e995553085830bbf98a761032c5dfc788`**.

Verification at that HEAD: offline suites PASS, AOP objects compile cleanly
with `W=1`, full Docker `Image.gz` + 104 DTBs PASS with no warnings, and
`check-build.py on --tree aop --integration-base a9/aopint` PASS (existing
integration `50ad73f524`, read-only merge simulation, no refs moved).
`build/kernel-aop/COMMIT` matches HEAD and `DIRTY` is empty. Logs:
`build/periph/aop-r4/round4-{offline,w1,build,check-build,export}.log`.

**Verified offline:** the running/empty path has a read-only region/head/UUID,
build and exact tag-layout preflight, SHA-256-pinned reference files, ext
comparison and a boot-time reservation check before any TX. A pristine table
is `refused-running-no-loader-tags`; identity/tag/ext failures are distinct.
An unreserved ext range is `refused-running-unreserved-ext` **before mapping
it**, even if its bytes might still be intact. Only the AOP region or a plain,
non-reusable `/reserved-memory` carveout without a compatible driver or pool
is accepted; mapped RAM additionally requires every covering page reserved.
No late reservation, inherited-image/tag write, reset, power-on or fault clear.

Phase B requires two consecutive ACKs with `ack == seq || ack == seq+1`, at
most three 300 ms probes (sequences 1/3/5 prevent delayed duplicate matches).
The ordinary cold-path ping now uses the same corrected ACK rule. Phase C
sends one `0x00200000000c000c` only with **both** immutable gates:

```text
apple_a9_aop.debug=1 apple_a9_aop.active=1 apple_a9_aop.attach=1
```

The observer still owns the sole `attach` parameter and remains read-only.
`attach=1 active=0` only selects its old observer preflight; `active=1 attach=0`
runs A/B and refuses before Phase C. Missing/foreign responses stop the attempt
without retry. Phase C silence or any non-EPMAP response is
`refused-running-handshake-owned`, with a loud zombie/power-cycle warning.
The shutdown callback preserves that refusal and never reports the AOP stopped;
as a `void` platform shutdown callback, it **cannot veto kexec**.

An accepted EPMAP enters the existing EPMAP replies → AP power → STARTEP
`>=0x20` → AFK/GET_BUF/rings → accel ready → calibration → 10 Hz → first
report → IIO flow. Attach requires a nonempty factory calibration file;
the old cold path retains its optional-file behavior. Debugfs `state` reports
proofs, phases, attempts/ACKs and adoption; `transcript` labels every word's
phase; `patchbay` exposes the read-only head/build/tag snapshot.

**Inferred, not device-verified:** the firmware accepts a speculative reply
when it is still waiting for its first AP, and EPMAP then establishes ownership.
The historical cold-run note above attributed startup to the current boot
chain; the later takeover research instead infers a surviving iOS instance
from the identical cold/warm state. No running image, EC0p reservation, live
response, calibration outcome or 10 Hz cadence has been verified by this round.

### Test plan (orchestrator only; no device commands run here)

Offline reproduction:

```sh
python3 -B scripts/periph/aop/test-offline.py
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/install-kernel.py && make LLVM=1 ARCH=arm64 W=1 drivers/soc/apple/apple-a9-aop.o drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o'
scripts/build-kernel.sh --tree aop
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-round4.py'
(cd patches/aop/round4 && shasum -a 256 -c SHA256SUMS)
```

`tests/takeover.c` compiles the actual policy helpers under ASan/UBSan:
real image/tag layout, changed identity/loader tags, malformed tables (20,000
trials), ext mutations/range bounds, ACK echo/+1/wrap, duplicate/foreign
messages, timeout budget, disabled gate and one-shot EPMAP adoption/refusal.
`tests/prepare.py` verifies `--tree aopint --attach` command lines and baseline
protections with payload generation mocked. Device behavior remains untested.

Integrate and package on the host (preserve the known-good recovery entry):

```sh
INTEGRATION_TREE=aopint scripts/integrate.sh a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install a9/power a9/idle a9/boot-pin a9/aop
python3 -B scripts/periph/aop/prepare-tests.py --tree aopint --attach \
  --base-cmdline build/boot/integ-entry/cmdline
```

Stage/hash-check the two firmware files and this iPad's calibration using the
Round 3 procedure. First boot `build/periph/aop-r4/m1n1-aop-active.bin` with
`scripts/boot/kexec-host.sh` while `attach=0`. Capture
`/sys/kernel/debug/apple-a9-aop-active/{state,transcript,patchbay,rings}` plus
dmesg. Any Phase A failure must have `tx_words=0`. An unreserved EC0p refusal
requires an explicit boot-time reservation hand-off in a later boot; do not
reserve already allocated Linux pages or bypass the check.

Only after A/B pass and the orchestrator has assessed that first capture,
boot `build/periph/aop-r4/m1n1-aop-attach.bin` once with
`scripts/boot/kexec-host.sh`. Expect exactly two successful ping exchanges,
one HELLO_REPLY, then EPMAP or a refusal within 2 s. On zombie refusal, stop
and use a **real power cycle**; another kexec/DFU attempt does not clear the
inherited AOP. On adoption, verify the full bring-up transcript, successful
calibration replies, IIO raw/scale/buffer and measured 10 Hz cadence. Keep the
build COMMIT, exact cmdline, boot ID and capture together; do not enable
unattended attach or infer success from `cpu_running=1` alone.

## Round 5 — explain a refused fingerprint

Kernel HEAD: **`ee698666756bc9f21555068b9476d6fcea83081b`** on `a9/aop`,
above `9c64a50e99`; exported in `patches/aop/round5/`. Export replay reconstructs
the committed tree exactly and `SHA256SUMS` passes. Offline ASan/UBSan suites,
Round 5 payload-argument tests, and `W=1` AOP object compilation pass. No
device access or workspace commits were performed by this agent.

Full Docker build at that HEAD: `Image.gz` + 104 DTBs PASS, no compiler
warnings. `check-build.py on --tree aop --integration-base a9/aopint` PASS
(integration `b25061d6f8`, read-only merge simulation). Build COMMIT matches
HEAD and DIRTY is empty. Logs are in
`build/periph/aop-r5/{offline,w1,build,check-build,export}.log`. The first full
build was interrupted when another process removed the worktree; it was
recreated from the unchanged branch and the full build rerun successfully.

The orchestrator's first attach cold run (integration `b25061d6f8`, driver
`9c64a50e99`, capture `active-20261023T170147Z`) refused in Phase A with
`refused-running-foreign-image`, with **zero mailbox RX/TX**. The saved head
and build windows already differ: `+0x20` contains no `uuid` signature and
the build window is non-text. This does not identify another firmware build.
See [`ds-aop-fingerprint.md`](ds-aop-fingerprint.md) for the rechecked file
offsets, iOS loader evidence, remaining base/relocation questions and the
requirements for identifying another image without weakening attach.

Round 5 adds root-only read-only debugfs
`/sys/kernel/debug/apple-a9-aop-active/fingerprint`: the actual and pinned
expected first 0x100 bytes, UUID header, full build window (hex + printable),
raw tag words even if malformed, comparison counts/first mismatches and the
saved remap configuration. Expected tag bytes are explicitly the unpatched
file template; required loader values are reported separately. The file
formats frozen Phase A memory copies and never reads hardware, sends a word
or retries. Extra prefix/remap reads require debug=1 and the existing safe
snapshot gates. The Round 4 decision helpers/refusal rules are unchanged.

Host-only commands:

```sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/inspect-fingerprint.py \
  --capture build/periph/aop-r4/live/active-20261023T170147Z/active-patchbay.txt
scripts/build-kernel.sh --tree aop
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop --integration-base a9/aopint'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-round5.py'
(cd patches/aop/round5 && shasum -a 256 -c SHA256SUMS)
```

After the orchestrator rebuilds `aopint` with this new `a9/aop` HEAD using
the Round 4 integration command, package the Round 5 payload:

```sh
python3 -B scripts/periph/aop/prepare-tests.py --tree aopint --attach --round 5 \
  --base-cmdline build/boot/integ-entry/cmdline
```

Output: `build/periph/aop-r5/m1n1-aop-attach.bin`, `attach.cmdline` and
`entries.env`. Final parameters remain `apple_a9_aop.debug=1
apple_a9_aop.active=1 apple_a9_aop.attach=1`. The default Round 4 output
paths remain available without `--round 5`. Payload tests mock mkpayload;
actual integration/packaging is the orchestrator's next step.

Device commands below are **for the orchestrator; not run by this agent**:
boot that payload through the established cold-boot workflow, then
`AOP_CAPTURE_ROUND=aop-r5 scripts/periph/aop/capture.sh active`. The capture
now includes `active-fingerprint.txt` (and tolerates older kernels without
that file). Inspect the raw actual/expected/remap bytes before proposing a
different base or image. A matching build substring alone is insufficient;
all identity/tag/ext/reservation/ping/handshake rules still apply.

## Round 6 — runtime identity and 32-bit aperture access

Kernel HEAD: **`228f4e3a703192df9f78b0d5305164a110eb9d43`** on `a9/aop`,
above `ee69866675`. `patches/aop/round6/` contains the patch, series and
SHA256SUMS; isolated-index replay reconstructs the exact HEAD tree.
Offline ASan/UBSan suites, Round 6 payload-argument tests, W=1 AOP compilation
and strict patch style pass. Full Docker Image.gz + 104 DTBs and
`check-build.py on --tree aop --integration-base a9/aopint` pass with no
compiler warnings; build COMMIT matches HEAD, DIRTY is empty. Logs:
`build/periph/aop-r6/{offline,w1,build,check-build,export}.log`.
No device access or workspace commits were performed.

The saved Round 5 dump is now committed as
[`aop-r5-fingerprint-warm-1.txt`](../release/OMISSIONS.md) (integration
`460279abb9`, driver `ee69866675`). The differing prefix and remap readback
do not yet prove another AP code address: +08/+10 still name 0x210e00000,
and iOS's explicit firmware aperture access width differs from our old reads.
Detailed code/RO/RW/ext layout, evidence and stop-path limits are in
[`ds-aop-runtime-layout.md`](ds-aop-runtime-layout.md).

**Proven from artifacts:** iOS UUID, patchbay and version reads use aligned
32-bit loads (`b63838`/`b6fb78`), while the kernel's old `memcpy_fromio` uses
64-bit loads and byte tails. Firmware also mutates two words inside the old
64-byte build window: STKG is copied to +0x47474, and +0x47478 is a cursor.
The build text itself is exactly 49 bytes plus its NUL.

Round 6 uses readl32 in the existing allowlisted region, with a bounded padded
word for the 479-byte tag table. It compares exact build+NUL and separately
requires the runtime guard to equal the validated STKG tag. The full raw
64-byte window remains in diagnostics alongside a distinct identity comparison.
The existing cold loader uses writel32/readl32 instead of wider copies; it
still requires stopped CPU, empty outbox, no faults and domains already ON.
Running attach gains no image/tag/remap/CPU/power writes, alternate mappings
or retries. All unknown-image, loader/tag/ext/reservation/ping/ownership
refusals remain in force.

**Not device-verified:** that corrected reads identify this running image.
The prefix structures and +28=0x1ff000 readback remain unresolved. No new
build or DRAM target is accepted. A completed iOS handshake is still refused;
no permitted stateless CPU stop/reload was found. The supported iOS stop
writes forbidden CPU control/auto-boot and power state. A real AON cycle or
separately validated stop by the original owner is the fallback; domains OFF
still need separately designed legitimate startup, never a forced domain.

Offline reproduction after installing the templates in tree `aop`:

```sh
python3 -B scripts/periph/aop/test-offline.py
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/install-kernel.py && make -j4 LLVM=1 ARCH=arm64 W=1 drivers/soc/apple/apple-a9-aop.o drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o'
scripts/build-kernel.sh --tree aop
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop --integration-base a9/aopint'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-round6.py'
(cd patches/aop/round6 && shasum -a 256 -c SHA256SUMS)
```

After the orchestrator rebuilds `aopint` with the new `a9/aop` HEAD:

```sh
python3 -B scripts/periph/aop/prepare-tests.py --tree aopint --attach --round 6 \
  --base-cmdline build/boot/integ-entry/cmdline
```

Output: `build/periph/aop-r6/m1n1-aop-attach.bin` and `attach.cmdline`, with
`apple_a9_aop.debug=1 apple_a9_aop.active=1 apple_a9_aop.attach=1`. Use the
established boot workflow and capture with
`AOP_CAPTURE_ROUND=aop-r6 scripts/periph/aop/capture.sh active` (orchestrator
only; no device commands were run by this agent). Verify
`snapshot_access=aligned-readl32`, exact 50-byte identity, guard/tag consistency
and the existing ext reservation result before interpreting the remap value.
Any Phase A failure must retain RX/TX=0. Never bypass an unreserved-ext or
handshake-owned refusal to get to IIO.

## Round 7 — unresolved runtime layout and cold-recovery design

Kernel HEAD: **`b0fbdf663609d05c1523a4d61f631eea43867471`** on `a9/aop`,
above `228f4e3a70`; exported in `patches/aop/round7/`. No device access,
workspace commits, integration branch changes or pushes by this agent.

**Verified from the supplied device evidence:** integration `46dc6de336`
with Round 6 still safely refuses foreign-image, with RX/TX=0. Correct-width
reads did not recover the pinned image. The committed
[`aop-r6-device-debugfs.txt`](../release/OMISSIONS.md) cuts off partway
through its prefix; complete header/build windows remain, but there are no
raw tag bytes in that combined file. The inspector reports actual coverage.

**Verified statically:** the pinned firmware translates IOP physical addresses
using an additive remap delta and inclusive 32-bit page bounds. The upper
word 1ff000 does not locate the code or authorize a larger AP read. Normal
iOS loading still copies raw main bytes to fixed-region offset zero; no
verified relocated/decompressed/data-first alternate was found. The running
build, code PA and tag/ext addresses remain **unresolved**. Details and
source addresses: [`ds-aop-recovery.md`](ds-aop-recovery.md).

Phase A now requires the single supported AP resource/remap descriptor to
match both before and after its existing allowlisted identity reads, also
with debug=0. Any unknown/changed descriptor retains foreign-image before
ext mapping, ping or HELLO_REPLY. Debugfs reports both remap samples,
`alternate_layouts=0`, and `snapshot_end=1`. All existing refusal rules stay.
There is no new AP resource, followed pointer or power/reset operation.

**Fallback design, not device-verified or enabled:** a separate AOP-only
recovery would need consumer/genpd ownership, GPIO/IRQ quiescence, proven
master isolation, bounded child-before-parent off and reverse on, a measured
stopped/empty/fault-free baseline, pinned cold copy/remap verification, then
the existing bounded CPU/AFK/IIO bring-up. iOS has empty AOP gate lists and a
constant-success idle callback, so it does not supply that PMGR/isolation
contract. Linux always-on/warm retention also prevents an ordinary cycle.
The design lists explicit missing facts and partial-failure handling. There
is **no safe runnable AOP-cycle command established**. Do not disable global
kexec_keep_on or substitute an AP/DFU reset for an AON cycle.

**Verified offline/build:** ASan/UBSan policy and MMIO-helper tests, stable
remap/each-bit/unknown-bound/resource rejection, all existing protocol/ring/
tag/ext/handshake suites, partial/conflicting capture parsing, real collector
with locally faked SSH (including truncation before its layout line), and
Round 7 `aopint` payload packaging with mkpayload mocked. AOP objects compile
with W=1; strict checkpatch is clean. Full Docker Image.gz + 104 DTBs,
check-build/merge simulation, export replay and SHA256 verification pass.
Logs: `build/periph/aop-r7/{offline,w1,checkpatch,build,check-build,export}.log`.

Reproduce offline validation:

```sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/inspect-fingerprint.py \
  --capture docs/periph/aop-r6-device-debugfs.txt
scripts/build-kernel.sh --run aop 'make -j4 LLVM=1 ARCH=arm64 W=1 drivers/soc/apple/apple-a9-aop.o drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o'
scripts/build-kernel.sh --tree aop
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aop --integration-base a9/aopint'
scripts/build-kernel.sh --run aop 'python3 -B /work/scripts/periph/aop/export-round7.py'
(cd patches/aop/round7 && shasum -a 256 -c SHA256SUMS)
```

Orchestrator commands using the supplied integration set (**not run here**):

```sh
INTEGRATION_TREE=aopint scripts/integrate.sh a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install a9/power a9/idle a9/boot-pin a9/aop
scripts/build-kernel.sh --run aopint 'python3 -B /work/scripts/periph/aop/check-build.py on --tree aopint'
python3 -B scripts/periph/aop/prepare-tests.py --tree aopint --attach --round 7 \
  --base-cmdline build/boot/integ-entry/cmdline
scripts/boot/kexec-host.sh build/periph/aop-r7/m1n1-aop-attach.bin
AOP_CAPTURE_ROUND=aop-r7 scripts/periph/aop/capture.sh active
```

This builds/packages a new integration, preserves the protective baseline,
and warm-kexecs a diagnostic attach payload; **no DFU or fresh iOS boot is
needed for this regression capture**. It does not enable power-cycle recovery.
Final opt-ins remain `apple_a9_aop.debug=1 apple_a9_aop.active=1
apple_a9_aop.attach=1`. On the currently observed descriptor the expected
result is still foreign-image, zero RX/TX, `remap_matches_cold=0`, and a
complete frozen dump ending in `snapshot_end=1`. No IIO success is predicted.
Retain each raw file from the collector, not only combined terminal output.
