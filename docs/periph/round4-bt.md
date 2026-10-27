# Round 4 Bluetooth

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


## Device evidence and address interpretation

**Verified in the orchestrator's Round 3 integration logs:** the selected
`BCM4355C0 FearA USI MCC` image loaded, revision build 1312. Power on and LE
discovery worked. The capture contains roughly twenty discovery records,
including the named headset and desktop reported by the orchestrator.

**Verified:** the boot log reported the ROM default address during
`btbcm_finalize`, while the later `btmgmt info` reported this unit's real
address. The latter is the public address the controller/core exposes at
capture time. The capture does not include all preceding setup commands, so
it does not prove that the real address was installed at initial boot. The
Round 3 plan had an explicit MGMT address-config step. The core also supports
applying a bootloader `local-bd-address` property after driver setup; that
ordering can produce the same pair of messages. These are alternative
explanations of the transition; neither is established from the scan log.

**Verified in source:** the TX-power warning was printed unconditionally
because the correct `HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER` workaround was set.
The corresponding LE initialization function checks that quirk and skips the
unsupported command. A warning did not mean the command had been attempted.

## Implementation

The J71t UART controller now installs its identity after the final Patchram
reset and before the default-address check:

1. Prefer an exact six-byte, nonzero unicast `local-bd-address` property,
   using the core's HCI byte-order convention and its existing broken-order
   quirk when applicable.
2. Otherwise load `apple/j71t-BMac.bin` directly. The firmware stager copies
   this unit's resolved IODeviceTree Bluetooth address without changing its
   original display/network byte order. The kernel reverses those bytes into
   HCI order. The file and its source remain private under build/.
3. Send the existing Broadcom RAM address command, then read the address back
   and validate response size, status and exact identity. Only a successful
   readback updates `public_addr` and clears the invalid/property flags.
   Missing or malformed identity and failed programming/readback fail setup
   promptly. There is no sysfs firmware fallback and no persistent write.

This path matches only `apple,j71t-bcm4355-bt`; no unit address is embedded in
source or DTS. Other controllers retain their existing setup path.

The core's broken-command table has a per-entry diagnostic level. Only its
established TX-power workaround entry now logs at debug level. The quirk and
the command-skipping condition remain unchanged; all other entries retain
their warning level. Dynamic debugging can still show the workaround message.

The selected HCD did not change. Device build 1312 and successful discovery
now validate the earlier combo-module-based firmware selection on this unit.

## Offline validation

Run in the host repo, with no hardware connected or accessed:

```sh
scripts/build-kernel.sh --run periph 'python3 -B /work/scripts/periph/round4-bt.py'
scripts/build-kernel.sh --run periph 'python3 -B /work/scripts/periph/test-round4-bt.py'
python3 -B scripts/periph/test-firmware.py
python3 -B scripts/periph/verify-firmware.py
```

**Verified:** the modified `btbcm.o`, `hci_bcm.o` and `hci_sync.o` compile.
Strict checkpatch reports zero errors, warnings or checks. The test compiles
the actual kernel identity helper and actual TX-power command guard with
ASan/UBSan: 114 assertions pass across DT preference/byte order, direct
fallback, malformed input, programming/readback faults and the unsupported
command guard. Fifteen firmware parser/selection tests pass, including new
synthetic identity staging/invalid-address cases. The real offline verifier
matches thirteen peripherals firmware files, including the new private BMac.
Additional touch firmware is preserved and outside this verification claim.

Evidence: `build/periph/round4-bt/` and
`build/periph/round4-bt-compile.log`. Primary workstream owns the final full
kernel build, commits, patch export and combined initramfs regeneration.

## Next device test, run by the orchestrator

Use the rebuilt kernel and regenerated single-stream firmware initramfs.
Before any MGMT address-changing command, capture:

```sh
dmesg | grep -E 'Bluetooth:.*(BCM|address|Power Level)'
btmgmt --timeout 10 --index 0 info
btmgmt --timeout 10 --index 0 config
```

Expected: build 1312; a `Using device-tree device address` or
`Using private BMac device address` message with this unit's real address;
the same real public address in `info`; no `Using default device address`
message; no missing `public-address` option; no TX-power warning at normal
log level. Do not repair the address manually during this check: failure must
be captured so the boot-time path can be diagnosed.

LE discovery requires LE to be enabled first with this minimal userspace:

```sh
btmgmt --timeout 10 --index 0 le on
btmgmt --timeout 10 --index 0 power on
btmgmt --timeout 15 --index 0 find -l
btmgmt --timeout 10 --index 0 stop-find -l
btmgmt --timeout 10 --index 0 info
```

The explicit stop is retained because a client timeout need not stop the
controller's scan. The kernel identity change remains **pending device
verification**; no agent contacted the iPad.
