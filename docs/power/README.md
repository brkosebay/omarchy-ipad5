# A9 / S8003 power management research

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Current CPU thermal/boost/overclock implementation and device commands:
[overclock-round3.md](overclock-round3.md). Validated SOC0/SOC1/TCC2 zones,
cpufreq cooling, raw diagnostics for all six channels, opt-in stock boost and
guarded CPU core/buck7 voltage offsets are implemented. Integ31 exposed a TCC1
stride bug that damaged a volatile TCC0 coefficient; round 3 corrects the map
and quarantines unvalidated channels. Experimental clocks/offsets remain untested.
Historical read-only research: [overclock.md](overclock.md).

> Private graphics-dependent excerpt omitted from this source snapshot.

Round 2 implementation and current status: [round2.md](round2.md).
Historical round-2 iOS commands: [ios-round2.md](ios-round2.md).
The original round-1 report below remains historical evidence.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Main findings and confidence

> Private graphics-dependent excerpt omitted from this source snapshot.

Static program: kernelcache SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
New decompilations are in [evidence/](../release/OMISSIONS.md). Ghidra sometimes merges adjacent
functions across panic/tail-call boundaries: use the successful branch before
its return, not the apparent code after a noreturn panic, as evidence.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics section omitted from this source snapshot.

## Chip-wide efficiency plan

### CPU DVFS: use the existing implementation

`performance-controller@202220000`, shared by CPU0/1, has
`apple,s8000-cluster-cpufreq`, `apple,t8103-cluster-cpufreq` fallback. Linux uses
64-bit command+0x20 (`0x202220020`), BUSY31, SET25, PS1[4:0], PS2[15:12];
status+0x50 current[7:4]. It polls busy before a request (400us), writes the
OPP index to both fields and sets bit25. OPPs are300/396/600/912/1200/1512/1800
MHz at levels1..7. 1844MHz level8 is explicitly disabled until CPU deep sleep.

> Private graphics-dependent excerpt omitted from this source snapshot.

Live `voltage-states1` raw pairs:
`(165494,546),(109226,575),(71859,640),(54613,700),(43343,775),
(36408,812),(35463,812),(0,0)`; nominal-performance1=71859.
The first column's units/mapping to Linux OPPs have not been recovered.
Neither column is a Linux `opp-microvolt` table to paste into DT.

The inspected config already selects schedutil, APPLE_SOC_CPUFREQ and CPU_IDLE.
It has no Apple cpuidle source in-tree; CONFIG_CPU_IDLE alone doesn't create
deep states. It has THERMAL/THERMAL_OF, but CPU_THERMAL and DEVFREQ_THERMAL are
**disabled**. Enabling those options alone would still need sensors, DT thermal
zones/trips and cooling maps.

### Priorities, impact and acceptance

Battery impact below is a qualitative expectation, **not measured watts or a
battery-life claim**. Compare battery discharge at fixed brightness, workload,
radio connectivity and temperature; USB charging/NBD activity confounds it.

> Private graphics-dependent excerpt omitted from this source snapshot.

`apple-pmgr-pwrstate` already supports A9 through the generic driver, including
hierarchical DT domains and reset sequencing. Audit missing consumers and leaked
references before adding SoC-specific gate code. Keep GPIO, iBoot display/DP and
wake-path/USB constraints from [PCIe suspend](../pcie/suspend.md); do not remove
its always-on properties as a generic efficiency tweak. Unused ISP/MIPI/PMP
might already be off via genpd's boot sweep—measure marginal benefit first.

The downstream [Asahi cpuidle driver](https://github.com/AsahiLinux/linux/blob/asahi/drivers/cpuidle/cpuidle-apple.c)
is an architectural reference: it wraps deep WFI in CPU_PM notifications and
has an M-series compatibility whitelist. It is **not S8003 support**. The A9
uses spin-table boot and has no PSCI/EL2; adding its compatible to that whitelist
without recovering Twister's system-register, cache and wake protocol is not a
port. New iOS PMGR idle evidence (`5a3b514`) shows per-core and cluster active
counts, perf-state accounting and dynamic calls; it does not itself contain a
complete CPU powerdown/resume sequence.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Safe iOS observation: orchestrator only

No kernel-memory reads, Mach task-port acquisition, MMIO, registry setters,
IOConnect external methods, tracing hooks, NVRAM or firmware-storage writes.
The helper uses only userspace IOReport channel discovery/subscription/sampling.
It may fail due to private ABI, entitlements or unavailable channels; record the
failure rather than bypass access controls. IORegistry text output may contain
private identifiers: retain raw captures under gitignored `build/power/`.

### 1. Text IOService snapshots and published tables

The script does targeted class/name queries; **no `ioreg -a` XML** on iOS.
Commands below are plans for the orchestrator, not commands run by this agent:

```sh
scripts/power/build-ios-ioreport.sh
IPAD_SCP=1 scripts/ipad-ssh.sh scripts/power/ios-observe.sh scripts/power/ios-sample.sh build/power/ios-ioreport mobile@127.0.0.1:/tmp/
scripts/ipad-ssh.sh 'sh /tmp/ios-observe.sh /tmp/a9-power-idle'
# Owner renders/scrolls an ordinary animated app, then:
scripts/ipad-ssh.sh 'sh /tmp/ios-observe.sh /tmp/a9-power-render'
IPAD_SCP=1 scripts/ipad-ssh.sh -r mobile@127.0.0.1:/tmp/a9-power-idle build/power/
IPAD_SCP=1 scripts/ipad-ssh.sh -r mobile@127.0.0.1:/tmp/a9-power-render build/power/
```

Exact individual queries, if needed:

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

### 2. Userspace IOReport while idle / rendering / returning idle

`ios-ioreport` loads `/usr/lib/libIOReport.dylib`, discovers one named group,
records its legend, subscribes, takes a baseline and emits successive **deltas**
as plist files. No channel ABI layout is guessed. It is bounded (max300 samples),
100ms minimum interval; use1s to limit measurement overhead. The binary is
cross-compiled and ad-hoc signed on the Mac when `ldid` is installed. No special
kernel-memory entitlement is requested. Built successfully against the local
iPhoneOS SDK; **not yet executed or ABI-validated on the iPad**.

Run locally on iOS (ordinary mobile first; orchestrator may use its existing
privileged shell if subscription explicitly denies permission):

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

```sh
scripts/ipad-ssh.sh 'sh /tmp/ios-sample.sh /tmp/a9-report /tmp/ios-ioreport'
```

Manually follow0–10s idle,10–20s rendering,20–30s idle. Repeat once at fixed
brightness and thermal condition. Energy Model is a **discovery candidate**,
not a confirmed A9 group; PMGR may publish energy under SoC Stats instead.
For legend-only discovery, use sample count0. Inspect channel names/units and
state IDs, not an assumed M1 channel list or clock/voltage interpretation.

Copy captures to the Mac and inspect with:

> Private graphics-dependent excerpt omitted from this source snapshot.


> Private graphics-dependent excerpt omitted from this source snapshot.

### 3. powermetrics-style sampling, only if installed

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.


If a named sampler is absent, use only the names reported by that binary;
if the binary is absent, use the IOReport helper. Do not install a random
macOS executable or use a kernel-memory fallback. `ioreport -h`, when installed,
is discovery only; there is no asserted universal iOS CLI syntax.

## Offline verification and next handoff

```sh
python3 scripts/power/adt-report.py > /tmp/a9-power-adt.json
sh -n scripts/power/ios-observe.sh scripts/power/ios-sample.sh scripts/power/linux-observe.sh scripts/power/build-ios-ioreport.sh
scripts/power/build-ios-ioreport.sh
```

All passed locally. IOReport symbol/ABI availability and live ioreg property
visibility remain untested. Run `scripts/power/linux-observe.sh` on Linux twice,
idle and after fixed work, to collect existing sysfs/debugfs without modifying
policy or mounting debugfs. Store raw device output under `build/power/`.
No speculative kernel patches were created in `patches/`: all newly identified
mechanisms either already exist or lack enough semantics for a correct patch.

Upstream source references: [cpufreq driver](https://github.com/torvalds/linux/blob/master/drivers/cpufreq/apple-soc-cpufreq.c),
[A9 binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/cpufreq/apple%2Ccluster-cpufreq.yaml).
The checked local kernel snapshot, rather than a moving web branch, is the basis
for the exact Linux register masks above. Four understood PMGR/performance
functions were renamed in the shared Ghidra project for subsequent researchers.
