# iOS capture plan: the camera adapter's Lightning ID-bus exchange

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-26. Companion to [ds-lightning-host-idbus.md](ds-lightning-host-idbus.md).
Nothing here was run yet — this is the plan for the **orchestrator** (only the
orchestrator talks to the device, per AGENTS.md). Offline-written.

**Goal.** On the jailbroken iOS 16.7.10 (20H350) iPad, with Apple's
Lightning-to-USB Camera Adapter attached, capture:

1. the adapter's **6-byte digital ID** (`IOAccessoryDigitalID`) and the
   decoded 16 capability fields (ds-lightning-host-idbus.md §5.2);
2. the ID-bus **command/response evidence** (GetID 0x74/0x75, the power
   handshake 0x72/0x73/0x70) as iOS performs it;
3. the Tristar/dock state transitions when the adapter switches the port to
   **USB host** (vendor request 0x51 / CHRG type 5), including with a charger
   on the adapter's Lightning power input.

This is the only way to get the adapter's ID: it is not in the kernelcache
(ds-lightning-host-idbus.md §8), and noplist/stock log contains it.

## Why ioreg is the primary source (and it already worked once)

The Mac-cable dump (`build/firmware/private/IOService.txt`, taken 2026-10-22
in device mode) already shows the mechanism end-to-end: the
`Port-Lightning@101` node publishes `IOAccessoryDigitalID = <100c00000000>`
for the plain cable. The camera adapter will publish its own 6 bytes the same
way, with no new tooling and zero write risk. Everything else (syslog, BPF)
is corroboration.

## Step-by-step

### A. Baseline (nothing attached)

Over `scripts/ipad-ssh.sh` (key auth as `mobile`; use `sudo` where needed —
the device is jailbroken):

```sh
scripts/ipad-ssh.sh 'ioreg -l -w0 -p IOService -n AppleTriStarBuiltIn' \
  > build/periph/idcap/baseline-tristar.txt
scripts/ipad-ssh.sh 'ioreg -l -w0 -p IOService -n AppleCBTL1610' \
  > build/periph/idcap/baseline-ic.txt
scripts/ipad-ssh.sh 'ioreg -l -w0 -p IOService -n IOAccessoryManager' \
  > build/periph/idcap/baseline-manager.txt
```

(Or the existing `scripts/periph/pull_firmware.py --ioreg` flow if preferred;
the three targeted pulls are smaller and faster to diff.)

Record: `IOAccessoryDigitalID`, `IOAccessoryID`, `IOAccessoryOrientation`,
`IOAccessoryUSBModeType`, `TransportsActive/Provisioned`,
`IOAccessoryPowerHandshake`, `IOAccessoryAccessorySerialNumber`,
`IOAccessoryManagerSystemID` (expected **2** — this is the HOSTID the 0x74
carries), the `statistics` dictionary (idCommand* counters), and the
`IOPortTransportStateUSB2` child's properties.

### B. Syslog capture must START BEFORE the adapter is attached

The adapter **displaces the Mac USB tether** — start the log capture first,
then attach. From the Mac (before unplugging):

```sh
# while the iPad is still tethered:
idevicesyslog > build/periph/idcap/syslog-adapter.txt 2>&1 &
# (or, over Wi-Fi ssh if idevicesyslog is unavailable: run
#  /usr/bin/log stream --style compact  on-device if present, else
#  oslog/Procursus if installed — fallback: skip, ioreg is primary)
```

Then unplug the Mac cable and attach the **Camera Adapter** (nothing on its
USB-A port yet). Wait ~10 s.

### C. Capture the three adapter configurations (pull ioreg for each)

For each state below, save the three ioreg pulls of step A into
`build/periph/idcap/<state>-*.txt` (plus a `system_profiler`/`syslog` snippet
if the Mac is reachable over Wi-Fi):

1. **adapter alone** (no USB device, no charger on the adapter's Lightning
   input) — the pure identification state;
2. **adapter + keyboard (or flash drive)** — should trigger the 0x51 → host
   switch; captures `IOAccessoryUSBModeType` transitions (0 → 3), the
   `AppleEmbeddedUSBArbitrator`/EHCI-OHCI nub publication, and
   `IOPortTransportStateUSB2.DataRole` changes;
3. **adapter + charger on its Lightning power input** (charge-while-hosting)
   — the USB3 camera adapter feeds VBUS in while hosting; capture whether
   the ID/`IOAccessoryPowerHandshake`/power-source items change
   (`IOAccessoryPowerSourceItemUSBHost`, "determinePowerSupply" items at the
   manager, current limits).

Also, once, after removing everything, re-pull to see the detach transition
(bits 0x04/0x0d semantics, ID removal).

### D. What to grep in the syslog (exact strings, from Ghidra)

These are plain `IOLog` strings in the two kexts; they surface through the
syslog/unified-log stream (some are level-gated by the kext log mask — take
what appears; ioreg does not depend on it):

- `IOAccessoryManagerAID::handleDigitalID` — followed by
  `data=%02x:%02x:%02x:%02x:%02x:%02x` (the raw ID) and
  `USB=%x ACCx=%x DB=%x MB=%x UART=%x PWR=%x HV=%x AI=%x AU=%x PU=%x BP=%x PO=%x CC=%x BD=%x HS=%x HPM=%x`
  (the decode — the single most valuable line for us);
- `getAccessoryDigitalID: 0x%x` (AppleTriStar re-read of the retained frame);
- `prevent sending CmdID since %llu us has elapsed` (GetID cadence);
- `handleIDBusResponse:%d: RspGetState` (0x71/0x73 power-handshake responses);
- `no VBUS detected when starting powerhandshake`,
  `setPowerHandshakeState(kPowerHandshakeLimit)`,
  `accessory needs PH override`, `power handshake verification failed`,
  `handshake aborted`;
- `EVENT_IDBUS_WAKE`, `timeout waiting for digital ID`,
  `EVENT_DIGITAL_ID: _handleAidSwitchConfigChanged`,
  `AID switch config unexpectedly changed; expected:%d`,
  `changeAidSwitchConfig: requested:%d, locking:%d` (reg 0x17 path —
  should NOT appear on J71t);
- `chargerFunction: ilim=%dmA type=%x` — **type 5 = the host-adapter CHRG
  classification** (the fallback host trigger);
- `USB role switch configureUSBMode() failed %#x`,
  `Starting role switch` (vendor 0x51 path).

### E. Optional (check existence, don't build anything)

- `accctl` / `tristar` internal tools on the device (lightning-idbus.md §9
  claims `accctl` runs on jailbroken retail; `tristar -p` prints pin
  configuration). If present: `accctl` output in each state is a bonus
  register-level view. If absent, skip — nothing depends on it.
- An IDBus BPF tap interface (`tapCommand` → `bpf_tap_in`; classes
  `AIDSnifferInterface`/`IOAccessoryIDBusUserClient` exist but were not
  instantiated in our dump): `ifconfig -a` after attach; if an IDBus-ish
  interface shows up, `tcpdump -i <if>` captures the raw frames. Opportunistic
  only — internal builds gate it.
- `IOReportLegend` channels on the port (`LightningPortACCxFaultCount`,
  `Dx1FaultCount`, …) via `ioreg` — the fault counters double as evidence the
  adapter's lanes came up.

### F. Decode and file the result

1. Take `IOAccessoryDigitalID` from each state's `Port-Lightning@101` pull.
2. Decode with the 16-field table (ds-lightning-host-idbus.md §5.2); note the
   `USB`, `ACCx`, `UART`, `PWR`, `AU` values in particular.
3. Record the three states' `IOAccessoryUSBModeType` /
   `IOPortTransportStateUSB2.DataRole` and the CHRG type if logged.
4. Fill the "camera adapter" row into ds-lightning-host-idbus.md §5.2 and
   close open question 4/5 (§11). That ID becomes the driver's match value
   (§9.2 arm condition).

## Expected outcomes and what each means for the Linux driver

| Observation | Meaning for the Linux plan |
|---|---|
| ID present, `USB` field non-zero | the GetID sequence (§9.2) will see the same frame; implement the match |
| ID identical across states 1–3 | one match value suffices |
| ID differs with charger attached | the driver must decode per-state; log both |
| `EVENT_IDBUS_WAKE` appears after a USB device is attached to the adapter | the adapter renegotiates after enumerating the iPad — our driver needs the 0x0d bit-1 handler eventually |
| CHRG type 5 (not 0x51) appears | the fallback path is the adapter's mode; the driver's arm condition may not need the USB-device enumeration step at all |
| `IOAccessoryPowerHandshake = Yes` only with charger | the 0x72/0x70 handshake is only a charging prerequisite — keep the Linux power-handshake step optional (module param) |

## Safety envelope (unchanged)

- **Read-only on the device**: ioreg/log pulls only. Never write syscfg,
  NVRAM, boot firmware; never OTP/ESN/provisioning ops; never
  `ttrs.apple.com`; never modify the ADT or device tree on iOS.
- No boot-arg changes (that means no NVRAM writes) — if the kext logs are
  level-gated silent, rely on ioreg, not on NVRAM.
- The Mac-side `idevicesyslog` process: started by the orchestrator, killed
  by PID after the capture (never by name).
- Files land under `build/periph/idcap/` (gitignored, like other private
  dumps).

## After the capture

Hand the results back to the ds-lightning-host-idbus.md §5.2 table; the next
task is the §9 driver build (a new `a9/*` kernel branch with the identify
sequence + debugfs decode), then a repeat of the manual host trial with the
identify step armed.
