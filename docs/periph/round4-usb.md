# Round 4 USB host current authorization

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


This component receives the host authorization; the Tristar/charger workstream
owns source classification, physical VBUS checks, PMIC guards and programming.
No device was accessed and no USB identity or userspace startup script changed.

## Verified protocol and existing gadget

The shared kernelcache analysis identified the IOUSBDeviceFamily vendor setup
handler at `fffffff0060e9190` and current resolver at `fffffff0060e4074`.
See [round4-source-recon.md](round4-source-recon.md) for the exact evidence.
The no-data device request has request type `0x40` or `0xc0`, request `0x40`,
signed extra current in **wIndex**, and signed suspend current in **wValue**.
Those fields must not be interchanged.

For the active bus, iOS uses 500 + extra when both its standard base and extra
are nonzero, otherwise base + extra, then clamps to zero and the platform
maximum. J71t's verified platform maximum is 2400 mA. The active result is
recomputed immediately on this request. Reset clears the extra/suspend fields.

**Verified offline:** HoolockRD's outer `/init` sources its outer
`init_functions.sh`, which creates configfs gadget g1 with VID `0x05ac`, PID
`0x4142`, NCM/RNDIS and later ACM. It does not override MaxPower; configfs
defaults to the current kernel's `CONFIG_USB_GADGET_VBUS_DRAW=500`. Thus its
selected USB 2 configuration authorizes a standard 500 mA. That script
disconnects/rebinds the gadget when adding ACM, so a previous host grant must
not survive the rebind.

**Unverified on device:** whether macOS sends the Apple grant to Linux's
`05ac:4142` gadget. The handler does not manufacture a 2400 mA grant merely
because a Mac is attached. Existing iOS source evidence has the USB-host
item at 2400 mA while its analog brick signature is zero, making receipt of
this request important for the measured Mac port. No VID/PID substitution
was made to force a host driver match.

## Kernel interface

Only a UDC with the board opt-in boolean `apple,host-current-budget` enables
this path. J71t's UDC label is `usbdev`, full path
`/soc/bus@20c000000/usbdev@20c100000`. The Tristar consumer references it with
`usb-controller = <&usbdev>`. The primary workstream owns those DT additions.

The gadget header exports:

```c
struct usb_gadget_current_budget {
        const struct device_node *of_node;
        unsigned int current_ma;
        bool host_grant, configured, connected, suspended;
};
int usb_gadget_register_current_notifier(struct notifier_block *nb);
int usb_gadget_unregister_current_notifier(struct notifier_block *nb);
int usb_gadget_get_current_budget(const struct device_node *node,
                                  struct usb_gadget_current_budget *budget);
```

Callbacks run on an atomic notifier chain and receive a temporary snapshot.
They may filter by `of_node` and schedule work; they **must not sleep or call
the getter**. The getter is sleepable, looks up the controller under the UDC
mutex and snapshots under its state spinlock. Register the notifier before
the initial query. During removal, unregister first and cancel the scheduled
consumer work before releasing the node/consumer memory.

`configured` records a configuration selected in this USB bus session and
survives suspend/resume. It clears on reset/disconnect. This allows the
consumer to avoid analog probing a sleeping data host while still examining
an unconfigured dedicated charger. `connected` records actual host bus evidence:
default/address/configured notifications or an observed ep0 SETUP. It survives
suspend but clears on not-attached/attached/powered states. Suspend alone never
marks an unenumerated dedicated charger as a data host. DWC2 omits the generic
default/address notifications, so the first SETUP provides the evidence needed
to pause analog probing while enumeration is still in progress; it authorizes
only the standard 100 mA before configuration or an explicit grant.
A disconnected or suspended bus publishes zero current.
An absent or opted-out controller returns ENODEV.

The standard budget follows the existing `usb_gadget_vbus_draw()` calls,
capped at 500 mA for this USB 2 board. Authorization is cached separately
from optional legacy USB PHY power programming. That matters here: the DWC2
callback returns ENOTSUPP when no legacy USB PHY exists, although the host's
selected configuration still authorizes the stated current.

The composite setup hook reserves only the exact board-enabled request before
other vendor/function handlers. Valid signed extra values range from -2400 to
2400 mA; suspend current must be 0–2400 mA. This bounded Linux subset rejects
out-of-range values, whereas the iOS resolver also handles larger inputs.
The resulting active authorization is clamped to 0–2400 mA. Negative extra
requests reduce the budget; they never wrap to a large unsigned current.

Missing/malformed request data, invalid ranges and failed ep0 queue/status
completion revoke the extra grant. Suspend revokes it immediately and resume
requires a fresh grant. Reset restores the generic 100 mA base; DWC2's reset
path reports disconnect instead and therefore revokes to zero until the next
configuration. Disconnect, gadget rebind and UDC removal also revoke it.
Normal configured unknown USB remains at its 500 mA standard budget.

## Validation and next device evidence

**Verified offline:** gadget core and composite objects compile; strict
checkpatch has zero errors, warnings or checks. Actual kernel authorization,
state, VBUS and completion functions are compiled with ASan/UBSan and exercised
by 160 assertions: signed arithmetic/clamps, board/request filtering, malformed
requests, reset/suspend/disconnect/revoke, configured-session preservation,
notifier lock context, query absence, PHY-not-supported behavior and failed
status completion. They distinguish powered-to-suspended from
default-to-suspended bus histories and cover host detection from SETUP before
configuration. Setup ordering and failed ep0 queue revocation have source
checks. The applier also replays from immutable Round 3 file copies to exact
current USB sources and leaves them unchanged on a second application.
No PMIC behavior is claimed by these tests.

```sh
scripts/build-kernel.sh --run periph 'python3 -B /work/scripts/periph/round4-usb.py'
scripts/build-kernel.sh --run periph 'python3 -B /work/scripts/periph/test-round4-usb.py'
```

Evidence lives under `build/periph/round4-usb/`; final integrated build and
charger policy checks belong to the primary workstream. On the next device
boot, capture this before manually changing the input limit:

```sh
dmesg | grep -E 'Apple host current grant|Tristar|CBTL1610|input budget'
cat /sys/class/udc/*/state
cat /config/usb_gadget/g1/configs/c.1/MaxPower
cat /sys/class/power_supply/usb/current_max
cat /sys/class/power_supply/charger/input_current_limit
```

The grant log reports the received extra and suspend fields. The consumer's
source/limit telemetry must confirm that the authorization propagated. A
missing grant log is a host-protocol integration result to investigate; it
does not justify assuming a higher USB input budget.
