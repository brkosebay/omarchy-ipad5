# Round 4: J71t PMIC addresses and desktop battery telemetry

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. No device was accessed by this workstream. Kernel changes are
limited to the J71t board override, the PMIC NVMEM provider and two bindings.
The battery driver needs no additional properties for UPower's charge-based
backend and was left unchanged.

## PMIC warning: cause and fix

**Verified:** `2026-10-22-integ3-nonvme-dmesg.txt` reports seven `OF: Bad cell
count` messages for `/soc/i2c@20a110000/pmic@3c`. The existing PMIC describes
one address cell plus one size cell, while its I2C parent correctly has zero
size cells.

This is not a failed raw `of_property_read_reg()` operation. Platform-device
creation and naming attempt to translate the children's PMIC register offsets
into CPU addresses. In `drivers/of/address.c::__of_translate_address()`, the
initial child counts are valid, then the next bus level has zero size cells.
The latter check prints the error before the absent PMIC `ranges` property
could reject translation. Adding `ranges` cannot fix that ordering.

**Implemented:** the J71t override now sets the PMIC's size-cell count to zero
and consistently uses address-only child register entries:

| Child | Register addresses | Separately retained information |
|---|---|---|
| RTC | `0x500` | The RTC driver already reads only the base address. |
| NVMEM | `0x5000` | New `size = <0x28>` property; existing RTC-offset cell remains offset 4, size 4. |
| Charger | `0x180`, `0xa00` | Existing `status`/`control` names remain; the J71t driver uses its verified fixed register addresses. |

The PMIC's I2C address, all functional register addresses and NVMEM-cell
offsets are unchanged. No shared board include was changed.

**Necessary prerequisite:** `simple-mfd-nvmem.c` formerly required a two-cell
`<base size>` register property. It now also accepts an address-only entry
with an explicit positive `size`. The original two-cell form is retained for
other boards. The PMIC binding allows zero or one size cells, and the NVMEM
binding documents the new size property. This is why the board DT patch has
a small provider/binding prerequisite instead of leaving malformed register
entries behind.

J71t's PMIC NVMEM node is explicitly `read-only`. The existing NVMEM core
honors that property, so the layout and RTC-offset reads remain available
without enabling writes. Persisting a newly set RTC offset is consequently
unavailable on this board; no factory calibration or storage was written.

**Inferred from the verified OF control flow:** zero size cells reject CPU
translation at its initial non-MMIO check, so the PMIC-parent error messages
should disappear. Confirming their absence requires the next device boot.

## UPower and desktop battery status

**Verified device evidence:** the saved charging test reports battery status
Charging, current about +1.17 A, voltage 3.855–3.874 V and capacity increasing
from 36 to 37 percent. Both USB and charger report online. The earlier gauge
test reported full charge 8,036,000 microamp-hours and cycle count 237.
Sources are the existing charging baseline/enable logs and the Device test 1
section in `README.md`.

**Verified kernel interface:** the cache profile already exposes Battery type,
presence, status, capacity, signed current, voltage, temperature, technology,
charge-now/full, cycle count and health. It refreshes asynchronously and
emits `power_supply_changed()` when its cache changes. No userspace read
initiates an HDQ transaction.

The reviewed primary UPower revision is
[`ecfd2e166b6213bbdaa4e7c952ebb9a6c1405337`](https://gitlab.freedesktop.org/upower/upower/-/commit/ecfd2e166b6213bbdaa4e7c952ebb9a6c1405337).
Its [battery backend](https://gitlab.freedesktop.org/upower/upower/-/blob/ecfd2e166b6213bbdaa4e7c952ebb9a6c1405337/src/linux/up-device-supply-battery.c)
accepts absent scope as system scope and treats model/serial as optional.
It selects charge units when `charge_full` exists, then consumes `charge_now`
and `current_now`; design capacity is not required for that path. It also
accepts the present voltage when no design voltage is available.

Thus no artificial design capacity, energy measurement or battery-pack model
was added. Explicit system scope would be truthful, but is unnecessary for
the reviewed UPower discovery path. `charge_full_design` remains absent
because the cached transport has no verified source for it.

The [USB supply backend](https://gitlab.freedesktop.org/upower/upower/-/blob/ecfd2e166b6213bbdaa4e7c952ebb9a6c1405337/src/linux/up-device-supply.c)
prefers `usb_type` plus `online` for line-power detection. With only `online`
it still recognizes line power, but emits a warning about missing USB type.
The charging owner was advised to expose `USB_TYPE_UNKNOWN` if no charger
classification is verified; VBUS presence does not establish SDP, DCP or an
Apple-current entitlement.

**Remaining device validation:** the saved logs do not include a numeric
`charge_now` sample or an actual UPower/desktop run. The next test should
capture the full battery uevent plus `charge_now`, `charge_full`, `capacity`,
`voltage_now` and `current_now` together. This distinguishes interface
availability from validation of the inferred gauge register map. No map
change is justified by the presently saved evidence.

On a Linux userspace with UPower installed, the following read-only commands
will establish the final integration result:

```sh
cat /sys/class/power_supply/battery/uevent
for name in usb charger; do
  cat "/sys/class/power_supply/$name/uevent"
done
upower --enumerate
for device in $(upower --enumerate); do
  upower --show-info "$device"
done
upower --dump
```

The expected result is a system battery with measured percentage and signed
charge/discharge status, plus online line power. This work did not install
or run a desktop on the device.

## Validation and reproduction

`scripts/periph/round4-power-ux.py` reapplies the exact changes, idempotently,
without committing. The focused NVMEM object build passed. The two modified
bindings and the J71t DTB passed dt-schema checks; only optional `yamllint`
was unavailable. Decoding the resulting DTB confirmed all table entries,
read-only NVMEM and the unchanged RTC-offset cell.

Artifacts are under `build/periph/round4-power-ux/`: schema log, decoded DTB,
source snapshot and a pinned UPower source manifest with SHA256 digests.
Checkpatch found no code errors or style defects; its combined-diff warning
asks for separate binding/code patches. Land the bindings/provider support
before the board DT migration.

```sh
scripts/build-kernel.sh --run periph \
  'python3 /work/scripts/periph/round4-power-ux.py; make ARCH=arm64 LLVM=1 drivers/nvmem/simple-mfd-nvmem.o'
```

The parent workstream owns the final complete kernel build, patch export,
payload and device-test plan.
