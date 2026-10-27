# Power research round 3 — iOS IOReport and on-demand observation

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23 (Amsterdam); inputs captured 2026-10-22 UTC, j71t / iOS 16.7.10.
No device connection, kernel edit, workspace commit or push by this agent.
The existing `a9/power` work remains at `d59013ae2d`, integrated default-off
as reported by the orchestrator. Exact recording commands: [ios-round3.md](ios-round3.md).

## Entitlement result and what is still unproved

The same-device binaries pulled by the orchestrator establish these exact
IOReport access declarations:

| Executable | Entitlement containing `IOReportUserClient` | Sandbox selection in its entitlements |
|---|---|---|
| `/System/Library/CoreServices/powerd.bundle/powerd` | `com.apple.security.iokit-user-client-class` (array) | No explicit sandbox/seatbelt selector |
| `powerdatad` | `com.apple.security.exception.iokit-user-client-class` (array) | `seatbelt-profiles = [temporary-sandbox]`; platform-application=true |
| `thermalmonitord` | `com.apple.security.exception.iokit-user-client-class` (array) | `com.apple.private.sandbox.profile:embedded = temporary-sandbox`; platform-application=true |

Full entitlement sets and hashes, including PowerUIAgent and
perfdiagsselfenabled, are in [daemon-entitlements-round3.json](../release/OMISSIONS.md).
Neither of the latter two declares IOReportUserClient. None of the five
declares a separate IOReport-named entitlement. Powerd/thermalmonitord's
`com.apple.private.ppm.superclient` and SMC privileges are **absent from
powerdatad**, which directly subscribes to IOReport; these are not established
universal subscription requirements. Files, Mach services and power-assertion
exceptions differ by daemon and are not evidence that our report-only helper
needs them. An entitlement selector is not the complete compiled seatbelt
profile, nor proof of the helper's effective/inherited sandbox state.

> Private graphics-dependent excerpt omitted from this source snapshot.

The orchestrator reports that `/usr/libexec/powerd`, PerfPowerServices*,
perfpowerservicesd and the proposed registration-service paths do not exist
on this device. The saved cache's prebuilt-loader string contains
`PowerLog.framework/XPCServices/PerfPowerTelemetryClientRegistrationService.xpc/…`;
that is a **cache string, not evidence the executable is installed**. No IPSW
or filesystem image was downloaded. The bounded optional path list, actual
powerd alternative, powerdatad and diagnostic tools, readable seatbelt sources
and launchd plists are consolidated in
[pull-ios-daemons.sh](../../scripts/power/pull-ios-daemons.sh).
It records absent/unreadable files, pulls originals, extracts signed XML on the
Mac, hashes them and writes a filtered summary. `--list` makes no connection.

## ABI, flags and channel ownership checked against Apple's own code

[Daemon call-site evidence](../release/OMISSIONS.md) is reproduced by:

```sh
python3 -B scripts/power/daemon-report.py
python3 -B scripts/power/daemon-report.py --calls
python3 -B scripts/power/kc-ioreport-evidence.py
python3 -B scripts/power/dyld-ioreport-evidence.py
```

* Powerdatad at `0x100009c3c` and thermalmonitord at `0x1000100fc`,
  `0x100038cf0`, `0x100043018`, `0x100048510` all pass:
  `IOReportCreateSubscription(NULL, channels, &subscribed, 0, error-or-NULL)`.
  There is no evidence for a required nonzero flag/channel-ID argument.
* Thermalmonitord releases the source channel aggregate immediately after
  subscribing, just as our helper does. Sampling uses **the returned subscribed
  dictionary**, not the source channels. Our existing lifetime/ownership
  pattern matches this sequence; no array-vs-dictionary conversion is needed.
* Saved iOS libIOReport 93.100.1 at `0x1d33a098c` opens `IOReportHub`, type0,
  calls its open selector0, configures interests with selector2, then maps the
  telemetry buffer. Its **fifth argument is `CFErrorRef *`**: the code calls
  `_createCFErr` and stores through x4's saved pointer. The helper formerly
  passed NULL and replaced every cause with “Subscription denied/unavailable.”
  It now prints the exact stage/domain/code/reason and saves `error.plist`.
  Initial sample, later samples and deltas also preserve CFError output.
* The kernel personality has no IOReport-specific `IOUserClientEntitlements`
  requirement. IOReportUserClient `initWithTask` delegates to the generic base;
  `open` at `fffffff0059b1c44` stores its input and returns success. Generic
  IOServiceOpen still runs MACF/sandbox checks. This is not proof that every
  app or SSH child can open it, and does not justify inventing an additional
  `com.apple.private.ioreport*` entitlement.

Kernel and library evidence with hashes:
[ioreport-kernel-round3.json](../release/OMISSIONS.md),
[ioreport-library-round3.txt](../release/OMISSIONS.md).
Understood IOReportUserClient functions were renamed and the shared Ghidra
analysis saved. Disassembly uses `--quiet` and bounded address ranges; the
719MB ipsw symbol cache generated during the first inspection was removed.

> Private graphics-dependent excerpt omitted from this source snapshot.

## What observe-1 actually establishes

Reproducible, identifier/calibration-free extraction and input hashes:
[observe-round3.json](../release/OMISSIONS.md), generated with
`python3 -B scripts/power/observe-report.py`. Original private data stays under
`build/power/ios-r2/observe-1/`. Registry queries were sequential snapshots,
not a synchronized idle/load experiment.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

**Policy inputs:** sample period16, max-power model3348, target utilization95,
minimum duty40, power filter313, performance filters5/50, integral clamps0;
float inputs Ki=0.0202128999, Kp=5.28318548, Ki2=0.197392002,
Kp2=6.85398102, power-performance scale0/1. These agree with the earlier
saved live ADT policy inputs. Ioreg's printable `<"!!J>">` is decoded as
four bytes, not discarded: it is the Ki2 float. These inputs do not expose
the unresolved host+0x450 table or runtime firmware policy image.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

**Battery:** AppleSmartBattery and AppleSMCSensorDispatcher queries are
empty. AppleARMPMUCharger is populated: externally connected and charging,
CurrentCapacity92, Amperage/InstantAmperage1410, Voltage4178,
Temperature2780 (raw property values). The recorder now snapshots this class
and AppleHDQGasGaugeControl. This USB-charging observation cannot establish
Linux/iOS discharge power or battery-life savings.

> Private graphics section omitted from this source snapshot.

## Implemented workflow and host validation

`ios-sample.sh preflight OUT HELPER` runs one100ms sample per group.
`ios-sample.sh record N LABEL OUT HELPER` runs N one-second samples per group,
1–300 nominal seconds, immediately after starting that command. The owner
starts the desired activity first; the orchestrator chooses when to record.
There are no automatic idle/render phases, countdowns or fixed wall-clock
render windows. Each process logs its initial sample bounds and subsequent
sample bounds using CLOCK_MONOTONIC plus UTC wall seconds; group start times
are not assumed simultaneous, and file/IOReport overhead is observable.

Build requires ldid, signs with the tracked plist (or an explicit plist argument),
extracts the embedded XML and verifies it equals the requested signature.
INT/TERM/HUP cleanup stops only the wrapper's outstanding sampler children.
New-output refusal and per-group status/logs remain. An error in battery
queries is recorded independently. `ios-observe.sh` now also captures
IOReportHub/IOReportUserClient properties for future access diagnostics.

> Private graphics-dependent excerpt omitted from this source snapshot.

Recorded checks and signed helper hash:
[round3-validation.txt](../release/OMISSIONS.md).
