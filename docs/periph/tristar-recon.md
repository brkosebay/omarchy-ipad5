# J71t Tristar source-presence reconstruction

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Scope: the read-only source-presence input needed by the Round 3
charger policy. No device access and no Tristar register writes were performed.
Private IOService data and raw Apple code remain under `build/`.

## Verified hardware and transport

**Verified:** this unit's ADT calls the IC `tristar,cbtl1610`, at I2C0 address
0x1a. AP GPIO interrupt 72 is described, but a polling implementation does not
need to acknowledge it. `function-brick_id_voltage` calls PMIC ADC channel 1;
`function-power_supply_change` targets charger phandle 133 through `pmuI(0)`.
See `docs/adt/j71t.txt` around lines 2405–2413.

**Verified:** the saved live IOService dump has `AppleCBTL1610` at line 7712
and `TriStar REV = 138` (0x8a) at line 7721. A driver restricted to this unit
can require revision 0x8a and fail closed on another revision.

**Verified in Ghidra:** AppleTriStarBuiltIn's actual vtable is
`fffffff006e52150`. Its read slot +0x858 points to `fffffff005fead50`; the
write slot +0x850 points to `fffffff005fead10`. These wrappers pass an
8-bit register subaddress to their AppleARMIICDevice provider, with
subaddress length 1 and caller-selected data length. The provider operation
is 1 for read and 2 for write. The one-byte reads below therefore have a
one-byte command/subaddress and a one-byte result.

**Verified deeper transport reconstruction:** this is a STOP-separated
register-pointer write followed by a byte read. It must not be described as
an exact match to `i2c_smbus_read_byte_data`, whose Linux PA Semi implementation
uses repeated START without an intermediate STOP.

AppleARMIICDevice's vtable `fffffff006ce8198` slot +0x560 points to
`fffffff0059b9068`, which sets only an optional completion argument to zero
then dispatches slot +0x568 to `fffffff0059b8f90`. That function stores the
original operation 1 unchanged into the command record at +8. Controller
`fffffff0059b85e4` calls a trace-only routine (`fffffff0059b8630`) then hardware
slot +0x558, resolving on AppleS5L8940XI2C to `fffffff006071bd8`.
For operation 1, the hardware routine emits the register subaddress byte
with `0x200` (STOP), then emits a new read address with `0x101` (START and
read direction), followed by length 1 with `0x600` (READ and STOP). Command
bit 2 could suppress the intermediate STOP, but Tristar operation 1 has it
clear. The same Linux controller defines START as BIT(8), STOP as BIT(9),
and READ as BIT(10), corroborating these FIFO words.

**Inferred faithful Linux API:** hold the adapter segment lock across two
`__i2c_smbus_xfer` operations: `I2C_SMBUS_BYTE` WRITE with command `reg` and
no data, then `I2C_SMBUS_BYTE` READ with one-byte result. Require
`I2C_FUNC_SMBUS_BYTE`. This writes only the IC's register pointer, never a
register data byte; the lock prevents another client from interleaving the
pair. A normal pair of separately locked convenience calls is avoidable.
Linux's PA Semi byte-operation implementation emits a STOP for each operation.
Raw Ghidra records are `iic-hardware-transfer.json`,
`iiccontroller-transfer.json`, and `iic-ghidra/fffffff0059b8f90.json` under
`build/periph/tristar-re/`.

## Verified register meanings

| Register | Read size | Meaning and code evidence |
|---|---|---|
| 0x0f | 1 byte | Revision. IC superclass start at `fffffff005ff4500` reads this byte and publishes it under the exact property name `TriStar REV`. |
| 0x0c | 1 byte | Live VBUS/source-detect state. `fffffff005ff498c` returns true for its strict selector 1 only when `(value & 0x03) == 0x03`; a read error returns false. |
| 0x0c | 1 byte | Connection/orientation in a separate operation: `fffffff005ff4928` returns zero unless bit 7 is set, then returns 1 or 2 according to bit 6. These bits are not a current limit. |
| 0x0d | 1 byte | Interrupt/event status consumed by `fffffff005fee47c`. It handles accessory/reset/digital-ID events; a minimal polling supplier should leave this register alone. Its complete acknowledgement semantics were not reconstructed. |

**Verified class dispatch:** CBTL1610's vtable is `fffffff006e52be0`.
Its +0x5f8 entry points to the inherited `fffffff005ff498c`. The higher-level
`getVBUSDetect` function at `fffffff005febc58` first tries an optional dock
`function-vbus_voltage` ADC callback and requires at least 4 V if it succeeds.
Without that callback it falls back through `fffffff005fe4fc4`, which calls
the IC's +0x5f8 slot with strict selector 1. J71t's dock ADT has no
`function-vbus_voltage`, so the strict register-0x0c path is applicable.

**Verified correction to the earlier notes:** in the BuiltIn object,
`function-brick_id_voltage` is stored at +0x1158;
`function-power_supply_change` is stored at +0x1188. The callback dispatcher
at `fffffff005fec0d4` invokes the latter object's +0x80 method with three
zero arguments. +0x1158 must not be called or labelled the supply-change
callback.

## Practical bounded supplier

A minimal J71t-only Linux supplier can perform read-only I2C transactions:

1. Probe address 0x1a and require register 0x0f == 0x8a.
2. Read register 0x0c for `ONLINE`; strict VBUS presence is `(value & 3) == 3`.
3. Poll rather than touching interrupt registers. Notify the charger when
   presence changes; on any read error invalidate source presence so the
   charger restores its saved state.
4. Keep the current entitlement separate from this boolean. VBUS presence
   does not demonstrate a 2400 mA source budget.

**Verified from iOS on the same iPad and Mac port:** the owner-provided
IOService record reports accessory current/base 2400 mA, AdapterDetails
PMUConfiguration 2400, and positive battery current about 1704 mA.
**Not yet reconstructed:** the entire accessory/USB enumeration and source
classification path which derives that 2400 mA budget dynamically. There is
no verified raw Tristar byte that can be treated as a 2400 mA entitlement.

**Inferred practical test policy:** an explicit userspace request for
2400000 microamps, authorized only for this already measured Mac source,
can supply the missing policy decision. It must be gated by strict live
VBUS presence, the exact IC revision and battery checks; it must be revoked
and restored on disconnect/read error, and must require a new explicit
request after reconnection. The hardware driver must not automatically
claim 2400 mA for every cable or infer it solely from register 0x0c.

**Verified kernel ownership check:** the existing peripherals tree has no
Tristar/CBTL1610 driver and no I2C0 child at 0x1a. I2C0 is already enabled in
`s800x-ipad5.dtsi`. A board-only node and a supplier reference do not collide
with an existing client at that address.

## Evidence and reproduction

The shared MCP server became unavailable with `ConnectionRefusedError`
during this investigation. Instead, the repository's focused Ghidra helper
imported the extracted AppleTriStar Mach-O into an independent project under
`build/periph/tristar-re/ghidra/`; it never opened the shared project. All
register meanings above were confirmed by the Ghidra decompiler there,
with a matching LLVM instruction listing kept as a cross-check.

```sh
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python \
  scripts/periph/ghidra-local.py \
  --binary build/periph/round3-kexts/AppleTriStar.macho \
  --output build/periph/tristar-re/ghidra \
  --function 0xfffffff005ff498c \
  --function 0xfffffff005ff4928 \
  --function 0xfffffff005ff4500 \
  --function 0xfffffff005fead50 \
  --function 0xfffffff005febc58 \
  --function 0xfffffff005fe4fc4 \
  --function 0xfffffff005fec0d4
```

Project input SHA256:
`bc0013e7f7adf6bc363a9e53cf73e26cd96afd3a51a16148a4a20c8eec4ce4a6`.
The parent kernelcache SHA256 is recorded in
`build/periph/round3-kexts/manifest.json` and `charging-recon.md`.

The shared service returned after the focused analysis. Five understood
functions were renamed and commented there: `tristar_read_register8`
(5fead50), `tristar_get_vbus_detect` (5febc58),
`ths7383_cbtl1610_get_vbus_detect` (5ff498c),
`ths7383_cbtl1610_get_connection_orientation` (5ff4928), and
`ths7383_start_read_revision` (5ff4500). The supply-change dispatcher
5fec0d4 received a comment; renaming failed because the shared analysis has
no defined function there. It is defined in the focused local project.
Results are recorded in `build/periph/tristar-re/annotations.log`.


## Minimal driver review

The initial `scripts/periph/kernel/apple_tristar_power.c` draft was reviewed
without changing it. Two concrete issues were reported to the implementation
agent: the intermediate STOP mismatch above, and devres teardown ordering.
`devm_delayed_work_autocancel` must be registered after the power supply so its
cleanup cancels polling before the supply is unregistered. The mutex protects
all I2C/cached state; the getter refreshes the cable state and propagates read
errors, allowing a charger consumer to fail closed. No further blocker was
identified in the initial read-only supplier's lifetime or state handling.
