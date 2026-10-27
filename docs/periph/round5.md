# Round 5 peripherals report

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Kernel `41c03c2e90` on `a9/periph`, 52 patches from baseline
`6831bc701`. No device access or host-repo commits by the implementation
agent. Touch and other workstreams are unchanged.

## Round 4 device findings

**Verified by the orchestrator:** the real Bluetooth address was installed
from DT during initial setup, PMIC bad-cell-count warnings disappeared, and
USB presence/type were correct. The saved capture is
`docs/test-logs/2026-10-22-periph-r4-autocharge.txt`.

The same capture confirms the charging regression: the ordinary 500-mA
source selected calibrated input `0x20` (475000 µA), battery limit zero,
Not charging, and battery current −159 mA. The explicit 2400000 write failed
and left both limits unchanged. No Apple grant was observed by the
orchestrator. Those two behaviors came from Round 4 Linux policy, not a
new PMIC-register discovery: the ADT's 1000-mA threshold was overinterpreted
as requiring battery charging to be disabled below it, and manual writes
were restricted to detected-source caps.

## Charging corrections

**Implemented and verified offline:** ordinary nominal 500-mA USB now enables
a conservative **400-mA battery-current ceiling**, D2365 `0xa08=0x08`, while
retaining this unit's calibrated input `0xa03=0x20` (475000 µA). The PMIC
input-current control bounds total USB draw; the battery ceiling is separate.
The 400-mA selection is a conservative Linux policy using the already-verified
50-mA register encoding, not a claimed reconstruction of iOS's complete
low-current charging algorithm. Actual net battery current can still be zero
or negative when system demand consumes the available input power. It is
no longer intentionally disabled by a zero battery-current setting.

**Implemented and verified offline:** a supported positive write to
`charger/input_current_limit` again explicitly authorizes that input budget,
including **2400000 on the owner's measured Mac port when detection reports
only 500000**. The existing command works without a kernel parameter:

```sh
printf '2400000\n' > /sys/class/power_supply/charger/input_current_limit
```

Supported nominal values remain 500000, 1000000, 2100000 and 2400000 µA.
The driver logs the owner authorization alongside the detected allowance;
`usb/current_max` continues to report what detection actually established.
Only the charger setting reflects the owner's higher authorization. A root
write is the explicit source assertion; high-speed USB/VBUS does not silently
make it. Use the higher request only on the already measured source.

The driver still requires at least an ordinary 500-mA detected allowance,
strict live VBUS/revision, valid private CBAT, a present battery, 10–40 °C,
3.0–4.2 V, no termination/timeout and a sufficient read-only hardware cap.
At 500 mA the cap must permit 400 mA; at the 2400-mA owner request it must
permit the existing 2500-mA battery ceiling. Every write remains checked,
quiesced and reversible. No new PMIC register is written.

Owner authorization survives normal refreshes, but is revoked on an observed
source allowance below 500 mA, VBUS loss, guard failure, zero request or PM
quiesce. Failed enable/replacement also clears it. It is not restored on
resume/reconnect; the automatic policy can return to its detected budget.
Hardware/readback faults retain their latch and failed rollback retains its
snapshot/retries. Very short physical changes between polls may be missed,
as in the original manually authorized implementation. No claim is made to
identify a newly attached source from an old owner assertion.

Writing zero disables automatic policy and restores the original handoff
settings. A new positive guarded request re-enables it and explicitly retries
a cleared fault. Invalid requests, failed source reads, suspended/100-mA
allowances and stopped devices cannot initiate the owner override.

## Host detection and battery telemetry

The bounded Ghidra follow-up found **no verified** high-speed/Apple-host/VBUS
shortcut to 2400 mA in the inspected resolver, USBConnectShim and power-source
aggregation. USB speed scales the peripheral's own descriptor budget; it does
not identify the host vendor. Tristar's verified VBUS field reports presence,
and the dock's 2400-mA source item is a restriction ceiling, not a grant.
The existing USB grant and Apple resistor-signature paths remain available.
See [host evidence](round5-host-recon.md) for exact addresses and search limits.
No unverified Mac detection heuristic was added.

The owner also reported `charge_now` errors −5 at boot. **Verified in source:**
this cache profile can retain an optional NAC read error after other battery
fields have become valid, and returning EIO aborts the full battery uevent.
The cached charge getter now returns **ENODATA for an unavailable sample**,
which the power-supply core omits while publishing the remaining fields.
Valid cached charge values and later recovery remain available; live-read
profiles retain their existing errors. No charge value or design capacity is
fabricated. See [battery diagnosis and tests](round5-battery.md).

## Delivery and validation

**Verified offline:** clean full Image.gz and 104 Apple DTBs build; all 52
patches replay exactly; the two new patches pass strict checkpatch. Actual
charger functions pass the original 56 fault/guard cases plus source budgets,
400-mA fallback, no-grant owner override, repeated refresh, guard loss,
failed rollback and suspend/resume tests under ASan/UBSan. The battery tests
cover unavailable/valid/recovered cache samples and uevent behavior.

Firmware selection is unchanged. The one-gzip-stream initramfs retains
HoolockRD `/init`, firmware and static btmgmt. Artifact hashes and validation
logs are recorded in `build/periph/round5-final.json`. New charging behavior
and quiet/recoverable charge-now reporting remain **pending device validation**.
The already-confirmed BT identity and DT fixes are retained.

```sh
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

Follow [test-plan.md](test-plan.md): capture the 400-mA fallback first, then
make the explicit 2400-mA owner request on the measured Mac port, observe
several gauge refreshes and check restoration/telemetry.
