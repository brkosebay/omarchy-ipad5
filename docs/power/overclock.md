# A9 CPU clock research and read-only stock characterization

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Historical overclock round 1. Current implementation and owner policy:**
[overclock-round3.md](overclock-round3.md). The current hard ceiling is stock
maximum **+50 mV**, and the orchestrator may run small steps without per-step
owner approval. The +25 mV and per-step-approval text below is superseded.
Round 2 adds thermal sensors and guarded stock-voltage clock writes; it rejects
nonzero voltage changes while the physical DAC/rail conversion remains unproved.

2026-10-23. Owner's j71t iPad 5, TSMC S8003, two Twister cores, passive cooling.
Primary goal **2.0 GHz**, stretch **2.16 GHz**; **2.26 GHz is an owner-supplied
A9X comparison point, not an established A9 limit**. This round changes no
frequency, PLL, voltage, PMIC, thermal-control or DVFS-table register. No iPad
connection or workspace commit was made. Kernel work is limited to a default-off,
read-only observer on `a9/power`, based on `d59013ae2d`.
Kernel commits: `ec18dcbecc` plus `d8ed7cdb76` (take both; the second removes
unvalidated generic PLL probes). Combined patch: `build/power/dvfs-ro/observer.patch`.

**The first proposed clock change is Apple's own 1848 MHz state, at its existing
stock voltage and with its stock restrictions retained.** It is not an
overclock. Linux currently omits it deliberately; removing that guard without
resolving its deep-sleep/load restriction would not be a free fix.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Sources and reproducibility

* Private saved live ADT: `build/firmware/private/IODeviceTree.txt`, SHA256
  `70053282197d2371ca41ce78e15056630d538f2f13c33e0789a61cea8b733844`.
  Filtered decode: [evidence/dvfs-stock.json](../release/OMISSIONS.md).
  It contains no MACs, serials, unique chip IDs or calibration payloads.
* iOS 16.7.10 / 20H350 j71t kernelcache, SHA256
  `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
  Shared Ghidra analysis; new full read-only decompilations are local under
  `build/power/overclock-re/`. See [RE evidence](../release/OMISSIONS.md).
  [Round 4](round4.md) independently establishes the CPU reporting labels and
  measured iOS residency. A table entry alone is not a sustained-load result.
* Kernel `a9/power`: `drivers/cpufreq/apple-soc-cpufreq.c`,
  `arch/arm64/boot/dts/apple/s8003.dtsi`, `s800-0-3.dtsi`.
* [Hoolock m1n1 cpufreq source, pinned d5a10ac](https://github.com/HoolockLinux/m1n1/blob/d5a10ac52a6468484854419a6c5130f1d62073eb/src/cpufreq.c):
  independently identifies the A9 PSINFO banks and boost load restriction.
  This is reference source, **not a tool to run during this read-only round**:
  it contains voltage-control and boost writes. Its current public revision
  is not proof of which revision is in the owner's boot payload.
* Public Apple A9X IPSW ADTs: [evidence/a9x-adt.json](../release/OMISSIONS.md)
  records board/build, hashes and Apple CDN URLs. Downloaded only the small
  DeviceTree members, using the existing partial-download script.

Host-only reproduction:

```sh
python3 -B scripts/power/dvfs-decode.py
python3 -B scripts/power/validate-dvfs-ro.py
bash scripts/power/build-dvfs-ro.sh
# Optional repeat of the public A9X extraction; never contacts this iPad:
scripts/get-adt.sh iPad6,3 iPad6,7
python3 -B scripts/power/a9x-compare.py
```

The build helper checks branch `a9/power`, copies the observer source, adds its
Kconfig/Makefile entry, adds the **disabled** default to `.config`, and compiles
only its object. It creates neither a boot payload nor an active write feature.
Disassembly and undefined symbols are under `build/power/dvfs-ro/`.

## Stock CPU tables: three different indexes

`pmgr/perf-domains` consists of 28-byte records. The saved records describe
SOC (type 0, domain ID 1) and CPU (type 1, domain ID 2). There is one shared
two-core CPU policy, not a separate efficiency/performance cluster pair.

For the **type-1 CPU** voltage table the first word is an inverse-frequency
descriptor. iOS uses integer `65536000 / first_word` to build its MHz array,
stops at the first zero first word, and uses identity voltage-table mapping.
The second word is voltage-related input, not a measured regulator readback.

| ADT / iOS voltage index | First word | Decoded MHz | Second word, raw | Corresponding Linux hardware level by frequency |
|---:|---:|---:|---:|---:|
| 0 | 165494 | 396 | 546 | 2 |
| 1 | 109226 | 600 | 575 | 3 |
| 2 | 71859 | 912 | 640 | 4 |
| 3 | 54613 | 1200 | 700 | 5 |
| 4 | 43343 | 1512 | 775 | 6 |
| 5 | 36408 | 1800 | 812 | 7 |
| 6 | 35463 | **1848** | **812** | 8, pending live-table correlation |
| 7 | 0 | terminator | 0 | — |

This associates rows by frequency; it is not proof that the ADT voltage index
is the hardware request index. In particular, index 6 must **not** be sent as
the request for 1848 MHz. The enabled Linux levels 1..7 are
300/396/600/912/1200/1512/1800 MHz. The 300 MHz state has no row in this saved
CPU voltage table; its voltage must be observed separately. Nominal descriptor
71859 decodes to 912 MHz; boost descriptor 109226 decodes to 600 MHz. These
policy names are not a license to reinterpret either as the maximum clock.

`voltage-states0` is `(0,0),(1,0)` for the SOC type-0 domain. Do not apply the
CPU reciprocal formula to it, or treat its zeros as CPU voltages.

### Why Linux stops at 1800 MHz

In `s8003.dtsi` the entire level-8 node is inside `#if 0`, labeled
`1844000000`, with `turbo-mode` and the explicit comment
`Not available until CPU deep sleep is implemented`. The live iOS table
decodes to **1848**, so 1844 must not become an unverified frequency label.
The existing A9 cpuidle implementation provides WFI, not the required deep
cluster sleep. Public m1n1 also calls bits 43:40 of PSINFO2 the maximum load
field, and its test-only boost override changes that field for state 8.
The override is expressly an untested configuration. **Do not copy it.**

Step 0 therefore needs a live state-8 snapshot, frequency validation, a review
of its load/deep-idle requirement, calibrated thermals, and a boost-aware
policy. Merely exposing the slot may still leave actual clocks capped under
two-core load. Retain all Apple load/boost restrictions and measure this.

### Selection and live hardware table

ACC CPU performance-controller base = `0x202220000`. The DT fallback selects
Linux's `t8103` layout. `opp-level` is copied into `driver_data`; no regulator
or clock driver is called by `set_target`.

| Register / table | Address or rule | Established meaning |
|---|---|---|
| CPU command | `0x202220020`, 64-bit | BUSY31; SET25; Linux request1[4:0], request2[15:12]. A9 iOS uses four-bit state IDs in both fields. |
| CPU status | `0x202220050`, 32-bit | Linux actual[7:4], target[3:0]; compare request/actual, not only `scaling_cur_freq`. |
| Generic PLL status/factor | `0x2022200c0` / `0x2022200c8` | Documentation only; **not probed**. Generic driver's multiplier/divider comment is not a verified A9 PSINFO programming format or proof of safe read semantics. |
| PSINFO1 states 0..7 | `0x202220068 + 8*n` | Per-state word read by iOS; high byte is copied to live ACC+0x68 on a conditional tuning path. |
| PSINFO1 states 8..15 | `0x202220368 + 8*(n-8)` | Extended bank; iOS helper uses `0x220328 + 8*n`, the same addresses. |
| PSINFO2 states 0..15 | `0x202220868 + 8*n` | iOS copies bits39:32 to live ACC+0x868; public m1n1 identifies max-load[43:40]. Rail/DAC mapping unresolved. |
| PMGR performance-register descriptions | raw records `[0,0xb4000,55,0]`, `[1,0xb4000,11,1]` | Descriptions of register groups/counts, **not 55/11 decoded OPPs**. Group resolution and per-word semantics still needed. |

Linux waits for BUSY clear for up to 400 us, changes both request fields, sets
SET and returns without a post-write completion poll. `get` reads actual
state and looks it up in the OPP table; unlisted states cannot be reported
correctly. The observer brackets its raw table snapshot with command reads;
this detects some transitions but is **not an atomic hardware snapshot**.

iBoot's per-chip PSINFO data is left in place by Linux. The saved text ADT
does not contain those register contents. No hardware capture was obtained
this round; the observer acquires it for the orchestrator without selecting
a state. Generic PMGR dump tables must not be substituted for ACC PSINFO.

### PLL representability: arithmetic versus recovered hardware encoding

The generic driver's documented `0xc8` interpretation is
`12 MHz * multiplier[31:16] / (divider[15:0]+1)`. All of the requested
frequencies fit those integer fields mathematically. However, the complete
A9 PSINFO layout, VCO/input/output divider constraints and lock sequence have
**not** been recovered. In particular, `0xc8` is not established as the place
to install a dormant state's PLL configuration. **The following are offline
arithmetic examples, not write values, and no tool programs them.**

| Target MHz | Multiplier / denominator | Hypothetical generic factor | Integer 12-MHz-grid alternatives if denominator must stay 1 |
|---:|---:|---:|---|
| 1848, stock | 154 / 1 | `0x009a0000` | 1848 |
| 1850, superseded by stock 1848 | 925 / 6 | `0x039d0005` | 1848 / 1860 |
| 1900 | 475 / 3 | `0x01db0002` | 1896 / 1908 |
| 1950 | 325 / 2 | `0x01450001` | 1944 / 1956 |
| 2000 | 500 / 3 | `0x01f40002` | 1992 / 2004 |
| 2050 | 1025 / 6 | `0x04010005` | 2040 / 2052 |
| 2100 | 175 / 1 | `0x00af0000` | 2100 |
| 2160 | 180 / 1 | `0x00b40000` | 2160 |
| 2260, comparison only | 565 / 3 | `0x02350002` | 2256 / 2268 |

**Exact A9 per-step PLL register words cannot yet be given honestly.** Decode
stock words, correlate the active factor with independent timing, and recover
the programming masks/lock requirements before requesting any clock writes.
If the supported clock grid is 12 MHz, seek owner approval for the actual
nearby frequencies; do not advertise 1992 or 2004 as exactly 2000 MHz.

The Linux driver can expose additional OPP entries with unique levels up to
15 on this layout. Request2 is four bits, so a five-bit request1 does not
allow 32 A9 states. The source computes `pstate` before an index clamp and
checks the array index rather than validating the encoded hardware level;
future code must explicitly validate level <=15. It must also prove which
slots are free. Sixteen addressable entries do not establish any safe maximum
clock or prove slots 9..15 are unused.

> Private graphics section omitted from this source snapshot.

## Voltage path, rail steps and binning

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

**The +25 mV ceiling has no valid absolute number yet.** The future controller
must establish immutable, verified stock maxima in **microvolts for each
affected rail**, then enforce `requested_uV <= stock_max_uV + 25000`, including
rounding and coupled SRAM rails. It must derive and validate the smallest
actual DAC increment, ramp/settle constraints, range and code readback.
If one increment exceeds the remaining budget, refuse it; never round above
the ceiling. Do not assume +5, +6.25, +10 or +12.5 mV. A user-writable module
parameter must not be able to raise this bound. Unknown conversion => no writes.

### A9X comparison: what it can and cannot tell us

> Private graphics-dependent excerpt omitted from this source snapshot.

The owner's 2.16/2.26 GHz A9X examples support investigating the core design's
clock range. They do not establish this A9's cache/SRAM limits, voltage,
process/bin margin or cooling capacity. Some public frequency labels even
disagree on 9.7-inch frequency; do not use them to calibrate a rail.

Relative to Apple's 1848 MHz entry, 2.0 GHz is **+8.23%**, 2.16 is **+16.88%**,
and 2.26 is **+22.29%**. Relative to today's Linux 1800 MHz, they are
+11.11%, +20%, and +25.56%. The identical 1800/1848 voltage descriptors show
one stock frequency increment at the same descriptor, not enough margin to
extrapolate to 2 GHz. There is no evidence-backed numerical voltage estimate
or defensible probability that +25 mV will suffice. It may not. 2.16 GHz is
more speculative. Do not budget a guaranteed 2 GHz or ask for a higher ceiling
based only on A9X's nominal clock.

## Real sensors and mandatory thermal integration

> Private graphics-dependent excerpt omitted from this source snapshot.

| Nodes | Physical window | readBK / readBK1 | readSum / readCnt | Status | Alarm0..3 offsets |
|---|---|---|---|---|---|
| tempsensor0 | `0x20e0c0000` | 0x18 / 0x1c | 0x20 / 0x24 | 0x10 | 0x34/38/3c/40 |
| tempsensor1 | same | 0x118 / 0x11c | 0x120 / 0x124 | 0x110 | 0x134/138/13c/140 |
| tempsensor2 | same | 0x218 / 0x21c | 0x220 / 0x224 | 0x210 | 0x234/238/23c/240 |
| tempsensor3 / TCC0 | `0x202230000` | 0x20 / 0x28 | 0x30 / 0x38 | 0x400 | 0x410/418/420/428 |
| tempsensor4 / TCC1 | same | 0x60 / 0x68 | 0x70 / 0x78 | 0x430 | 0x440/448/450/458 |
| tempsensor5 / TCC2 | same | 0x220 / 0x228 | 0x230 / **0x238** | 0x460 | 0x470/478/480/488 |

The unusual TCC2 offsets are copied exactly from the live ADT, not made
regular by guessing. CTL/set/clear and trim-workaround properties also exist;
the decoder retains their offsets. **No new code initializes, acknowledges
or reads these raw sensors this round.** The calibrated conversion, valid/stale
bits, width, update cadence and enable requirements must be recovered and
compared against iOS before raw words can be called Celsius.

`AppleSocHot::start` (`0xfffffff005f24c80`) defaults to 105°C/115°C if no node
override exists. The live PMGR `sochot0` has `skip-trip-temp-config=1`, so
those defaults are **not evidence of the programmed PMGR thresholds**.
The live ACC `sochot1` explicitly supplies **120°C/125°C**, at offsets 0x90/0x98
of `0x202230000`; `use_int=0`. Its setup helper at `5f258d4` programs the
provided integer temperatures directly for this layout. These are high
hardware protection thresholds, not acceptable experimental operating targets.
Sensor `alarm-temp=100` is a separate property; do not equate all three layers.

The saved `thermalmonitord` binary and IOReport subscription analysis establish
telemetry use, **not j71t's complete skin/battery/die throttling curves**. Those
device-specific policy inputs have not been captured. Do not invent a CPU
passive trip and label it an Apple thermalmonitord value. Further orchestrator
read-only iOS captures should obtain matched thermal sensor and AppleSocHot
services, their properties, and the actual j71t thermal policy/config resources.

Before **step 0 or any overclock**:

1. Implement a separate calibrated sensor driver and six Linux thermal zones,
   types `apple-a9-soc0..2`, `apple-a9-tcc0..2`, reporting **milliCelsius**.
   Validate low/medium/high stock load, monotonicity, quantization, update
   cadence, and iOS agreement. Preserve factory trim and existing hardware
   protection. Sensor initialization is a later write-bearing task, outside
   this round. Battery/gauge temperature is not a replacement for die sensors.
2. Enable `CONFIG_CPU_THERMAL` and register the shared cpufreq cooling device
   with `#cooling-cells=<2>` and all six zones' cooling maps. Current branch has
   THERMAL/THERMAL_OF/step_wise but **CPU_THERMAL is disabled** and these zones
   do not exist. Use the hottest valid sensor for the safety decision.
3. Proposed conservative lab trips, **derived margins, not claimed Apple
   policy**: passive **80,000 m°C**, 5,000 hysteresis; hot **90,000** latches
   emergency fallback; critical **100,000** shuts down. Passive is 20°C below
   the observed sensor alarm, hot is 10°C below it. If recovered Apple policy
   throttles earlier, use the lower thresholds. Preserve PMGR's boot-programmed
   limits and ACC's failsafe; never raise them to make an experiment pass.
4. Poll at <=100 ms initially, measure sensor latency under load, and retain
   hardware alarm/SoCHOT/interrupt protection. Thermal `hot` notification alone
   does not implement the required fallback: add a kernel safety latch,
   frequency-QoS cap and watchdog handling explicitly. A sensor read error,
   invalid/stale sample, missing zone, timeout or workload verification error
   must disable experimental OPPs and clamp to a verified stock state.
5. Test cooling and the emergency path at stock, using mocked temperature/error
   input in an isolated test build as well as ordinary warming. Prove actual
   hardware frequency decreases; neither a governor label nor a cooling-device
   state alone proves this. Do not heat the chip to 120/125°C to test it.

> Private graphics-dependent excerpt omitted from this source snapshot.

## Read-only tools and orchestrator commands

> Private graphics-dependent excerpt omitted from this source snapshot.

The orchestrator can integrate `CONFIG_APPLE_A9_DVFS_RO=y` into a matching
kernel and opt in through `apple_a9_dvfs_ro.enable=1` on its existing RAM-only
boot command line. Alternatively build a matching `.ko` through the normal
kernel module build, then load with `modprobe apple-a9-dvfs-ro enable=1`.
Only the object has been compiled here; no ready-to-load module or boot image
is claimed. Debugfs must already be mounted by the orchestrator's usual boot.
Loading the observer makes no hardware-control writes, but booting the whole
normal system still performs its existing initialization.

On **Linux**, run by orchestrator only:

```sh
# Hardware command/status, both PSINFO banks, opaque voltage bytes/load fields:
cat /sys/kernel/debug/a9_dvfs_ro/snapshot
# Existing policy, gauge, all exposed thermal/hwmon/regulator readings:
python3 stock-log.py --duration 120 --interval 1 > stock-idle.jsonl
# Already-existing, broader read-only status script:
sh linux-observe.sh
```

Copy the scripts through the existing orchestrator workflow. Log files are
ordinary files or host SSH redirections; no tool writes to sysfs/debugfs.
`stock-log.py` logs every exposed zone, cooling state, hwmon input, regulator
voltage, CPU policy and gauge current/voltage/status/temp; unavailable readings
remain explicit errors. A regulator voltage setting is not an independent
measured rail voltage, and a hwmon input needs a verified label. The observer's
bytes are opaque codes, not rail millivolts. Raw table correlation, PMIC rail
readback and calibrated die temperatures are **outstanding acquisition/decoder
work**, not features silently represented by the gauge's battery voltage/temp.

The stress harness is opt-in, Linux-only, uses `stress-ng --cpu 2 --cpu-method
matrixprod --verify`, and terminates only its own process group. It refuses
to start without all six calibrated zone types and working actual/requested
frequency telemetry. It also refuses `scaling_max_freq` or any observed CPU
frequency above **1800000 kHz**, deliberately limiting this version to today's
baseline. The existing schedutil governor can respond normally to workload;
the harness itself never requests a frequency. It does not install packages.

After the six zones exist and their names have been validated, for example
if they enumerate consecutively as 0..5:

```sh
python3 stock-log.py --stress --duration 1800 --interval 0.25 \
  --soc-zone thermal_zone0 --soc-zone thermal_zone1 \
  --soc-zone thermal_zone2 --soc-zone thermal_zone3 \
  --soc-zone thermal_zone4 --soc-zone thermal_zone5 \
  --cutoff-millic 85000 > stock-load.jsonl 2> stock-stress.txt
```

It aborts its workload on missing/invalid temperatures, threshold crossing,
missing/out-of-baseline frequency or early stress exit. The 85°C workload
cutoff is a conservative laboratory stop, **not a recovered iOS trip**. This
sampling process does not replace kernel cooling or provide a hard guarantee
during a kernel hang. It performs **no revert-to-stock writes** this round.
After later approved states exist, a separately reviewed harness revision
must explicitly allow their range; do not bypass today's baseline guard.

Further read-only iOS inventory, if the orchestrator returns to iOS:

```sh
scripts/ipad-ssh.sh 'ioreg -r -c AppleSocHot -l -w0'
scripts/ipad-ssh.sh 'ioreg -r -c ApplePMGRTempSensor -l -w0'
scripts/ipad-ssh.sh 'ioreg -r -c AppleARMPMU -l -w0'
```

Class inventories can be empty; enumerate actual matched class names from
existing IOService output before drawing conclusions. These commands do not
request power/frequency/voltage changes. Do not substitute devmem, i2cdetect,
PMIC regmap dumps, or functions with read-looking names that submit requests.

## Proposed approved-write sequence: stock first, then a measured ladder

> Private graphics-dependent excerpt omitted from this source snapshot.

**Prerequisite P:** recover live PSINFO and calibrated rail/sensor decoding;
identify slot occupancy and PLL constraints; implement/validate the thermal
zone, shared CPU cooling device, watchdog and fallback design above. Refuse
all experimental states if any prerequisite or immutable stock backup is
missing. Establish battery/charging conditions and repeatable stock data.

**Step 0:** correct the proposed boost label to the validated **1848 MHz**,
make Apple's hardware level 8 available through the boost-aware OPP path,
and retain its Apple PSINFO/voltage/load settings. This changes DT/policy only;
selecting it uses the existing CPU command. Do not silently remove the
deep-sleep prerequisite or clear max-load restrictions. If it is not usable
safely on current WFI-only Linux, stop and resolve that dependency first.

**Step A onward:** first try the next frequency at the previous approved
voltage (initially the verified Apple stock maximum). If it fails without
exceeding thermal/current/error limits, consider one verified DAC increment,
then repeat the entire test. Hard bound remains stock maximum +25 mV per
rail; no automatic increase or parameter can raise it. Returning to the
stock maximum at a later step is an optional lower-voltage characterization,
not a reason to skip the previous passing voltage test. Avoid large jumps:
**1848 → 1900 → 1950 → 2000 → 2050 → 2100 → 2160 MHz**, subject to the proven
clock grid. 1850 adds little to the stock 1848 state and is superseded.
**2260 MHz is not in the executable ladder.** It requires a fresh research
and owner decision after 2160 has demonstrated value and margin.

### Exact known future register destinations, incomplete PLL/rail payloads

Provisional assignment below uses one additional dormant slot per step.
It is valid only if those slots are proved unused and the A9 sequencer can
use them. Do not overwrite Apple's states 0..8. Source/bound checks and saved
PSINFO snapshots must tie every value to this chip and this boot.

| Step, requested MHz | Provisional state | PSINFO1 address | PSINFO2 address | CPU command insertion bits only |
|---|---:|---|---|---|
| Stock restore, 1848 | 8 | `0x202220368`, **unchanged** | `0x2022208a8`, **unchanged** | `0x02008008` |
| A, 1900 | 9 | `0x202220370` | `0x2022208b0` | `0x02009009` |
| 1950 | 10 | `0x202220378` | `0x2022208b8` | `0x0200a00a` |
| Primary, 2000 | 11 | `0x202220380` | `0x2022208c0` | `0x0200b00b` |
| 2050 | 12 | `0x202220388` | `0x2022208c8` | `0x0200c00c` |
| 2100 | 13 | `0x202220390` | `0x2022208d0` | `0x0200d00d` |
| Stretch, 2160 | 14 | `0x202220398` | `0x2022208d8` | `0x0200e00e` |
| Comparison only, 2260 | 15 | `0x2022203a0` | `0x2022208e0` | `0x0200f00f` |

These command bits are **not whole register values**: preserve unrelated bits
in the 64-bit command at `0x202220020`, following the known request protocol,
then poll BUSY and actual state with a bounded timeout. Future A9 handling must
review the difference between iOS's four-bit request1 mask and the generic
Linux five-bit mask. Never write a literal from this column over the register.
PLL/voltage payloads and masks for the PSINFO words remain unresolved. Conditional
iOS tuning touches live ACC+0x68 and +0x868; whether new states require the same
updates must be recovered before implementation. DWI/PMIC destinations must
be identified precisely rather than adding guessed direct rail transactions.
This table therefore identifies the **known destination set**, not a complete
approved register manifest. It must be replaced by a fully decoded, reviewed
manifest before an overclock step is proposed for execution.

### Required controller behavior in a later patch

* Volatile in RAM only, default off, no initramfs/persistent DT/profile install,
  no syscfg/NVRAM/APFS/LLB/iBoot writes. A cold stock boot must restore defaults;
  test this rather than assuming a warm reboot always reprograms every word.
* Capture and hash immutable stock PSINFO/range/rail data before any mutation.
  Cap per-rail uV with overflow-safe integer arithmetic. Never recompute the
  stock ceiling from a table that the experiment already modified. Reject
  unknown sensor/rail/PLL IDs and unsupported chip revisions.
* Serialize with cpufreq fast-switch and the shared two-core policy; block
  experimental selections while installing/restoring dormant entries. Apply
  voltage/frequency in the hardware's proven safe order, preserve SRAM tracking,
  wait for ramp/lock/transition acknowledgements, and validate readback.
* Thermal trips, invalid/stale sensors, transition timeout, watchdog, voltage
  disagreement and workload verification failure latch off all experimental
  states. Clamp/request a verified stock fallback (initially <=912 MHz while
  cooling), wait for actual acknowledgement, then restore dormant table bytes
  and lower voltage in safe order. The retained stock OPP envelope is <=1800
  for emergency recovery. Never restore a table while executing from that slot.
* Do not rely on a userspace logger or shell trap for emergency recovery.
  If the kernel or power sequencer hangs, software may be unable to revert;
  keep verified hardware protection and watchdog/reset recovery. Do not claim
  automatic restoration can survive every hardware failure.

## Stock and per-step sustained-load acceptance

Keep ambient, case/orientation, brightness, radio and USB/NBD load fixed. Record
charging state, battery SOC and software build. Gauge `current_now` under USB
charging is net battery current, not CPU rail current; a different charging
profile invalidates a power comparison. Discharge comparisons need an approved
boot/network arrangement that does not interrupt the live NBD root server.

For stock 1800, stock-boost 1848, and each later approved step:

1. Two minutes idle, then short verified single-core and two-core runs from a
   similar starting temperature. Test transitions and idle/wake repeatedly;
   burst success alone is insufficient. Preserve exact workload/version/options.
2. At least **30 minutes** verified two-core matrix/FPU work plus an independent
   integer/checksummed workload, and a fixed-work latency/throughput benchmark.
   Include memory/cache-heavy work to expose non-core limits. Repeat at least
   three times and after a warm soak. Stop on first error; a single correct
   benchmark is not a stability certificate.
3. Log actual/requested frequency, PSINFO identity, cooling state/trips, all six
   die temperatures, sensor freshness, gauge current/voltage/status, verification
   errors and workload progress. Use short fixed-duration benchmark windows
   to compare first 30 seconds with the **last 10 minutes**, and establish
   thermal steady state (for example <0.2°C/min for five minutes). If still
   warming at 30 minutes, report no steady state and extend only while safely
   below the abort threshold. The present harness supplies sampling/stress
   diagnostics, not a completed throughput/residency analysis.
4. Report p50/p95 job latency, work/sec, effective active-clock distribution,
   time capped by cooling, hottest sensor, thermal rise/time constant, current
   distribution, and energy per completed work where the measurement boundary
   permits it. Sampled frequency histograms approximate residency; prefer
   hardware/driver time-in-state counters where available. Do not confuse
   frequency requests with actual clocks or bogo-ops with cross-method work.
5. Require sustained improvement exceeding run-to-run variation over stock,
   no verified errors, no hangs/recoveries and a working fallback. A nominal
   2 GHz burst that spends most of the test below stock is not a useful result.
   At equal voltage, ideal dynamic switching power rises with frequency:
   2 GHz versus 1.8 is +11.1%, 2.16 is +20%; increased voltage and leakage add
   more. These are scaling relationships, not measured whole-device watts.

**Evidence before any request to raise the +25 mV ceiling:** the last passing
and first failing frequencies, actual per-rail code/uV at each tested increment,
reproducible verification failures below thermal cutoff, stock-control passes,
all-sensor histories, transition/lock status, comparable current/energy and
sustained throughput benefit. A temperature/current limit, rail overshoot,
sensor fault, PLL lock failure or throttling-induced slowdown argues for
stopping/reducing clock, not more voltage. Recovery tests must pass. Even this
evidence only justifies asking for a new explicitly bounded experiment; it
does not establish a safe higher voltage or permit the code to exceed +25 mV.

Risks include transient timing faults and silent data corruption, cache/SRAM
instability, ramp/PLL transition failure, hangs before software can revert,
increased leakage/aging and permanent silicon or battery damage. Passive
cooling can eliminate the sustained benefit. Factory calibration, PMIC
ownership, firmware handshakes and hardware trips must remain intact.
