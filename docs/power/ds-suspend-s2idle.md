# A9 (S8003/j71t) suspend-to-idle: what iOS does at sleep/wake, and the minimum viable Linux s2idle

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-23. Offline research only: no device access, no kernel changes, no commits.
Target: iOS 16.7.10 / 20H350 kernelcache,
SHA-256 `c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`
(shared Ghidra project `/kernelcache.release.iPad6,11_12`), plus the live ADT
`build/firmware/private/IODeviceTree.txt` and the j71t Linux DTB
(`build/kernel-bootr8d-check/dtbs/s8003-j71t.dtb`).

Marks: **[V]** = read out of the binary, the live ADT/DTB, or a device log;
**[I]** = inference that has not been verified on this SoC. Addresses are unslid
kernelcache virtual addresses unless they are explicitly MMIO.

Read with [ds-idle-sleep.md](ds-idle-sleep.md) (CPU idle, core power-down, the
`_ml_arm_sleep` path), [idle-resume.md](idle-resume.md) (the reset vector and the
round-6 probes), [../pcie/suspend.md](../pcie/suspend.md) (the h9p suspend levels
and patches 0036–0042) and [../boot/ans-kexec.md](../boot/ans-kexec.md) (ANS
kexec history). This document does not repeat them; it answers three questions:
what iOS does on A9 around sleep, what the smallest correct Linux s2idle is, and
in what order to build and test it.

---

## 0. Bottom line

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 1. What iOS does on A9 at sleep and wake

### 1.1 The CPU side (one paragraph, details elsewhere)

iOS parks secondaries with `cpu_sleep_token='SUSP'`, then the boot CPU sets
`CPU_CFG.bit24` and `CYC_OVRD[25:24]=3` and loops `dsb/isb/wfi`; wake is a reset
through IORVBAR into `resume_idle_cpu` → `arm_init_idle_cpu` →
`cpu_idle_exit(from_reset=TRUE)`. **[V]** [ds-idle-sleep.md §6](ds-idle-sleep.md),
[idle-resume.md](idle-resume.md). The PMGR-side knobs iOS honours are the ones in
the live ADT: `cpu-power-gate-latency-us = 0xC350` (50 ms), `l2c-acc-sleep =
0xffffffff` (disabled), `cpm-power-gating = 1` and `mcc-power-gating = 1`; the
`apsc-sleep-dcs` / `apsc-sleep-soc` names exist only as defaults inside the
`ApplePMGR` binary, not as ADT properties. **[V]** Nothing in this document depends
on reproducing the CPU power-down, and §9.2 of
[ds-idle-sleep.md](ds-idle-sleep.md) remains the gate for doing so.

### 1.2 PMGR: what is gated, and the protocol iOS uses

The gates that exist on j71t (labels from the DTB, which is generated from the same
ADT `pmgr` tables). There are **two** PMGR windows: the AP one at
`0x20e000000` (0x8c000 bytes) and a second at `0x210200000` that owns the AOP
gates. **[V]**

> Private graphics-dependent excerpt omitted from this source snapshot.

**There is no central "sleep list".** iOS releases gates the way Linux genpd does:
each driver's `setPowerState(kIOSleepState)` drops its own `ApplePMGRFunctionPowerGate`
reference during the IOKit cascade, and PMGR follows. **[V]** for the gate list and
the per-driver pattern ([ds-idle-sleep.md §6.1](ds-idle-sleep.md)); **[I]** that no
extra sleep-only list exists — the only PMGR code that runs at the end is
`quiesceHW`/`quiesceACC` and the SEP preparation below, which do not gate devices.

The register protocol iOS uses for one gate (new evidence): **[V]**

* register = `group_offset + 8 * ps_index` (iOS strides 8 bytes per gate);
  `group_offset` and a per-group valid mask come from the driver's copy of the
  ADT `pmgr/devices` table.
* **current sub-state = `(read32(reg) >> 4) & 0xf`** — read at `fffffff005a2e280`.
* **requested sub-state = bits `[19:16]`** of the same word, written
  read-modify-write; bits `[9:8]` are cleared on every write — write at
  `fffffff005a256a8` (`value = old & 0xfff00000 | old & 0xfcff | (sub << 16)`).
* a transition is "write target, poll actual until it equals the target, then
  commit", e.g. `fffffff005a2e180` for the SEP case: it logs
  `"ApplePMGR: Preparing SEP for sleep"`, writes sub-state **0xf** (deepest),
  polls until `0xf`, then commits. **[V]**
* The iOS name `ApplePMGR::_enablePSNoWakeup` next to this code, plus the fact
  that `[9:8]` is cleared by every write, is consistent with one of those bits
  being a per-gate wake enable. **[I]** (bit not isolated).

**Do not copy those bit positions into Linux without checking the driver.**
Linux's `pmgr-pwrstate` already drives these gates successfully on A9
([README.md](README.md)), which is the behaviour that matters; whether it decodes
the same fields is **[I]** and was not verified here. For s2idle the only PMGR
contract that matters is the `apple,always-on` set, and that is already in the DT.

### 1.3 Wake sources: who arms what

**The AIC arms nothing.** The `AppleInterruptController` kext
(`0xfffffff00671f000`–`0xfffffff0067218d0`, ~30 functions) has no sleep, wake or
power-state code: its string table contains no PM vocabulary at all, and the only
power-adjacent path is a quiesce-safe register-write helper at
`fffffff006721250` that logs `"AIC writing register 0x%08x = 0x%08x after
quiesce"` when a register is written after a quiesce. **[V]** for the strings and
that helper, **[I]** for "nothing else exists" (absence of a string is not proof,
but there is also no caller of the PM APIs in the disassembled body).
`ps_aic` is `apple,always-on` in the DTB. **[V]**

This is consistent with the architecture: AIC wake arming is unnecessary because
AIC never loses power; the decision "which always-on line may wake the SoC" is made
by the PMU and by the PMGR/AOP, and the AIC only delivers the interrupt afterwards.

**PMGR/AOP side.** `ApplePMGR` reads a **36-byte (9 × u32) wake-source bitmask**
through virtual slot `+0x610`, then walks its `_apWakeSources[]` array (stride
0x24: `{u32 bit_index; char name[32]}`, count at `+0x31240`) and, for every set
bit, calls `IOPMrootDomain::claimSystemWakeEvent(rootDomain, pmgr, 0, name, NULL)`.
Bits nobody claims are published as `AOP.unclaimed%u`. **[V]**
`fffffff005a1a9d0` (dump + claim), `fffffff005a1da9c` (per-source dump at start).

The live ADT `pmgr/ap-wake-sources` gives exactly five names, all AOP channels:
`AOP.ExternalReq` (bit 1), `AOP.CPUWakeupAP` (15), `AOP.OutboxNotEmpty` (27),
`AOP.APWatchdog` (28), `AOP.APWakeupTime` (29). **[V]** So on A9 the wake *reason*
is an AOP concept; the AP reads a mask, it does not get a per-device reason.

`AppleS8000PMGR` adds `setAPWakeupSources(UInt32 *)`, which validates
`(sourceMasks[0] & ~kAPWakeupSourcesValidMask) == 0`, and an S2R timer:
`fffffff005e91844` logs `"ApplePMGR: Starting S2R timer"` after two PMGR register
writes — group `0x2a`, offset `0xc` ← 2 and offset `0x10` ← 3 (same write helper
`+0x9e0` used by the power-state code), plus a read at `0x4000 + n` in the same
group. **[V]** for the code, **[I]** for the meaning (an S2R deadline and its
enable fit the values and the log, but the register is not decoded).

**PMU side (the actual always-on button/charger/RTC owner).** `pmu@3C` is a Dialog
D2365 on i2c0, `compatible = "pmu,d2365"`, AIC IRQ **0x4c (76)**, and it is itself
an interrupt controller for the button events (`InterruptControllerName
IOInterruptController0000004F`; `event_name-gpio*` maps gpio 1/6/7/8/10/11/12/13/16
to halleffect/battery/bluetooth/wlan/codec/halleffect1/dock/belfield/pcie0). **[V]**
It provides: **[V]**

| ADT property / string | Meaning |
|---|---|
| `function-suspend_helper` = `S2RT` | platform function the AP calls for an S2R transition (the PMU's half of system sleep) |
| `gpio-activate-defaults`, `gpio-suspend-defaults`, `gpio-quiesce-defaults`, `gpio-pin-config` | three GPIO presets (awake / suspend / quiesce) applied on the way down |
| `_rtc_wake`, `setChargerWakeAlarm`, `"late vbus detect. scheduling rtc immediate wake"` | RTC alarm and charger-attach wake |
| `"cannot turn off GPIO wakes"`, ` WAKEINT: `, ` WAKEREA: `, ` EVENTS : `, ` IRQMASK: `, ` GPIOCFG: ` | wake-interrupt bookkeeping and the debug dump of the wake registers |
| `"pmu go hib (0x%x)"` | the PMU drives the hibernate entry |

`fffffff006bbb380` is the PMU's S2R preparation: it trims the GPIO wake mask
(clears the shadow bit for pins whose config byte `& 0xc1 == 1`), dumps
EVENTS/IRQMASK/GPIOCFG (0x1e/0x1e/0x18 bytes), logs `pmu go hib`, and calls its
child object. `fffffff006bbae60` applies one of the GPIO presets. **[V]**

**Per-device wake lines in the live ADT** (all on the AP GPIO controller unless
noted): **[V]**

| Device | Evidence | Notes |
|---|---|---|
| power button (`hold`) | `buttons` node: AP GPIO 0x61, plus `function-wake_button_hold` = `STAT`,0x1a9 from the PMU | two paths: AP GPIO while powered, PMU always |
| home button (`menu`) | AP GPIO 0x60, `function-wake_button_menu` = `STAT`,0x1a8 | same |
| volume down/up | AP GPIO 0x8f / 0x90 | not wake sources |
| touch | AP GPIO 0x41 (65) + `sending REQ_WAKEUP`, `sending MT_SPI_Z2_WAKE_CMD`, `delaying %d milliseconds after MT_SPI_Z2_WAKE_CMD` | touch may request wake; needs its rail + GPIO domain alive |
| Wi-Fi | `pci-bridge1` node: `function-device_wake` GPIO 0x43 (67), `function-clkreq` GPIO 0xa5 (165); driver strings `wlan.pcie.enableDeepSleepSupport`, `deassertDeviceWake`, `"Forcing out-of-band device wake"`, `"device wake GPIO not available, and inband device wake not supported"` | OOB GPIO or in-band deep-sleep-exit packet |
| Bluetooth | `function-bt_wake` GPIO 0x8e (142) | |
| NFC / Stockholm | `function-device_wake` GPIO 0x76 (118), `sePoweredOffDuringSleep=1` | |
| charger | PMU VBus (`function-get_vbus_detect`) | charger attach can wake |
| RTC | PMU RTC (`_rtc_wake`) | alarm wake |
| AOP | the five `ap-wake-sources` channels | the AP's wake *reason* source |

### 1.4 Per-subsystem behaviour around sleep

**NVMe / ANS.** iOS does not keep the controller in D0 across a system sleep: the
NVMe driver shuts the controller down (`CC.SHN=01`, wait `CSTS.SHST`), the port
goes through the PCIe deep-sleep sequence, and on wake the port is brought back and
the controller is initialised again — the DART and the whole NVMMU are re-run
because the port power-up moves their `force_active` 0→1
([../boot/ds-nvme-b-ios-ans-restart.md §1 items 3–6](../boot/ds-nvme-b-ios-ans-restart.md)).
New evidence for the start-path shape: **[V]**

* The controller start path at `fffffff006779600`–`fffffff006779854` is written
  for "first start" vs "start again": it clears `[this+0x416]`, and only when
  `[this+0x414]` is clear does it call `[+0xa10]` — which is **not** the port
  enable but `SendTimeToDevice` (verified: the callee at `fffffff00678023c`
  builds the `CORE_DEBUG_SET_TIME` tunnel command). It then allocates the queue
  structures (`[+0x7b0]`) and calls `[+0x918]`. **[V]** The port enable is a
  separate method: `enablePCIPort`/`disablePCIPort`, whose failure paths log
  `"Link enable failed! (0x%x)"` and `"enablePCIPort failed !"` (thunks at
  `fffffff0067bbf58`, `fffffff0067bbfc4`, `fffffff0067bc030`). **[V]**
* The NVMe node (`s3e`) *consumes* `function-pcie_port_control` (`PrtC`,
  argument `0x6e`) — the controller asks its port provider to enable/disable the
  link instead of poking the port itself. **[V]** for the ADT property, **[I]** for
  which node is the provider (the ADT text does not resolve that phandle).
* iOS distinguishes sleep types (`IONVMeController::DetermineSystemSleepType`,
  DT property `nvme-s2r-timeout` read at `fffffff00677f0a0`) and has an
  S2R-specific path; it also has `IsFullInit()`/`GetRestoreEnvironment()` for the
  case where the environment was preserved. **[V]** for the symbols/strings;
  **[I]** for which sleep type selects which. A timeout selector at
  `fffffff00677f000` returns **1 800 000 µs** by default and **35 000 µs** when
  one flag byte is clear (0x1b7740 / 0x88b8) — **[V]** code, **[I]** which timeout.

**PCIe / apcie.** Explicit deep-sleep state machine in `AppleEmbeddedPCIE`
(`0xfffffff005ecd000`–`0xfffffff005ee4e24`), with state-transition logs
`Port %u Suspend Request / Suspend Complete / Suspend Hardware Off / Suspend LTSSM
Off / Deep Sleep Entry / Deep Sleep Exit`, plus `_quiescePorts`, `_resumePorts`,
`setDeviceInDeepSleepGated`, `exitDeepSleep`, `manual-enable-s2r`, and two
enforcement strings: `"PCIe config space access to %02x:%02x:%x while port %d
device is in deep sleep"` and `"apcie[…]: Attempt to %s port while in deep sleep"`.
**[V]** Together with the previously recovered `disableGated` order
(PME_Turn_Off → L2 → PERST → refclk off → port power off, [../pcie/suspend.md](../pcie/suspend.md))
this is the iOS port sleep; nothing in it is an optional power optimisation.

**Wi-Fi.** The BCM4355 port is `manual-enable`, has the OOB wake GPIO above, and
the bus driver distinguishes OOB device wake from in-band deep-sleep exit
(`"Received control completion with wake packet bit set"`, `"Deep Sleep Exit
Notification"`, `"Chip seems to have recovered after a low power sleep"`). **[V]**
It is a real wake source in iOS; in our DT it is not wired.

> Private graphics-dependent excerpt omitted from this source snapshot.

**Display.** `AppleDPDisplayPowerController::setPowerStateGated` with
`total_power_on_delay_ms` / `total_power_off_delay_ms` and `setLCDPower`; the
SoC side has `function-dispidle_ctrl` (`ADPi`) on `soc-tuner` and the backlight
node provides `function-disp_aap_ctrl` (`ADPa`) and `function-backlight_enable`.
**[V]** The concrete pipe/eDP register sequence is not recovered here; our
conservative equivalent (keep `disp0`/`dp` on, backlight only) is unchanged.

**USB / touch / audio.** `AppleSynopsysOTGDevice` handles bus suspend and remote
wake; touch has the wake-request protocol above; neither is a wake source on our
board in the current DT. **[V]** strings, **[I]** board wiring.

### 1.5 Why iOS sleep is not a template for s2idle

iOS's sleep is a **power-down with reset-based resume**; every wake necessarily
goes through always-on logic (PMU GPIO/RTC/VBus → PMGR → AOP → CPU reset). Linux
s2idle keeps the kernel running and the cores clock-gated, so an enabled AIC
interrupt wakes a core directly, and the entire "arm the wake source" problem
disappears. That is why the Linux list in §2.3 is short: it is a *policy* list
(what may wake the system out of suspend), not a hardware mask.

The price is that s2idle cannot reach iOS's idle power: the AIC, PMGR, PMU and the
display pipe stay clocked, DRAM stays in self-refresh, and the PCIe links stay in
L0 in the current modes.

---

## 2. Minimum viable Linux s2idle

### 2.1 What is already proven on hardware (do not redo)

`docs/pcie/logs/2026-10-22-s2idle-*.txt`, kernel `7.3.0-rc1-g4ccbde28e24e`
(12 runs) / `g4ac8d6a654a7` (2 runs), h9p `suspend` levels 1/2,
`dwc2.s2idle_keep_gadget=1`: **[V]**

| Stage | Attempts | Result |
|---|---|---|
| A (`mode=0`) | 1 | `rc=1`, "system sleep refused: untested on this SoC" — refusal works |
| B (`mode=1 pm_test=devices`) | 1 | `rc=0`, 7 s, ANS re-enabled, `dd` read OK |
| C (`mode=1 pm_test=platform`) | 5 + 1 display variant | `rc=0` 6–7 s each, `port 0/1: link kept over the sleep`, `NVMMU: state kept over the sleep` |
| D (`mode=2 pm_test=platform`) | 3 | `rc=0`, `port 1: asleep (PME_TO 0x10, link 0xc5)`, brcmfmac re-probes, Wi-Fi reconnects |
| E (`mode=1 pm_test=none`, real sleep) | 1 (+1 display variant) | `rc=0`, 43 s, wake `IRQ 66`, Wi-Fi reconnects, `dd` read OK |
| F (`mode=2 pm_test=none`, real sleep) | 1 | `rc=0`, 49 s, both links back, `nvme0 live` |

On every resume the NVMe controller was found in `CC=0x464001` (EN=1, SHN=01) and
`CSTS=0x9` (RDY=1, SHST=10), i.e. **ANS had completed the shutdown and kept EN
set**; `nvme-pci` then disabled, re-initialised and re-enabled it, and the queues
were up ~0.13 s later (E: enable at 851.285, queues at 851.412). The "ANS not
ready" state (CC=0, CSTS=0, BFH=0xbadadd) is *not*
part of a clean s2idle cycle — it is what a panic or a hang during the resume
leaves behind ([../boot/ans-kexec.md round 5](../boot/ans-kexec.md),
`docs/STATUS-2026-10-23.md`). **[V]** logs, **[I]** attribution to the panic.

Also already in place (patches 0036–0043, [../pcie/suspend.md](../pcie/suspend.md)):
h9p levels 0–3, `PCI_DEV_FLAGS_NO_D3` for everything below the bridge, NVMMU
compare/re-program, `apple_dart_resume`, brcmfmac stay-down-on-lost-device,
`IRQCHIP_SKIP_SET_WAKE` for `pinctrl-apple-gpio`, `GENPD_FLAG_ACTIVE_WAKEUP`,
`apple,always-on` on `ps_gpio`, `dwc2.s2idle_keep_gadget`, and `disp0`/`dp`
always-on. **[V]**

### 2.2 Per-driver requirements

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

### 2.3 Wake IRQs

**Hardware view.** In s2idle any *enabled* AIC interrupt wakes a core, because
the AIC and the PMGR stay powered. There is no wake mask to program and no way to
"arm a subset" — the mask is the interrupt enable state at suspend time. **[V]**

**Policy view (what is allowed to abort a suspend).** Exactly two IRQs are marked
`wakeup-source` on this board: **[V]** DTB

| Source | DT | Linux IRQ |
|---|---|---|
| Home (`menu`) | `button-home`, `gpios = <&pinctrl_ap 0x60 GPIO_ACTIVE_LOW>` | one of the two gpio-keys IRQs. `IRQ 66` in the E log must be one of them: `check_wakeup_irqs()` only names IRQs that carry `IRQD_WAKEUP_STATE`, and these are the only two such IRQs on the board **[V]** (the log does not dump `/proc/interrupts`, so which of the two is **[I]**) |
| Power (`hold`) | `button-power`, `gpios = <&pinctrl_ap 0x61 …>` | the other gpio-keys IRQ |

Not wake sources in the current DT, although iOS has them: touch (GPIO 65, no
`wakeup-source`), PMU (AIC 76), Wi-Fi (no OOB GPIO), BT (GPIO 142 not described),
RTC (no alarm). Adding any of them is a separate, evidence-backed step (§3, steps 6 and 9).

Two DTB details on the `pmu` node are worth checking while here. It has
`interrupts = <0 0x4c 4 0 0x4f 4>`: the live ADT gives the PMU **one** AIC
interrupt, `0x4c`, and `0x4f` in the ADT is the PMU's *interrupt-controller id*
(`InterruptControllerName IOInterruptController0000004F`), not an AIC line — so the
second entry looks spurious. And the ADT configures `0x4c` as type 1 (edge) while
the DTB says level-high (flag 4). **[V]** for both mismatches; whether the driver or
the PMU tolerates them is unmeasured. **[I]**

### 2.4 What is unsafe

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 3. Step list (incremental, each with its risk)

Each step is a gate: pass → next, fail → stop and keep the log. Commands are the
existing ones (`scripts/pcie/s2idle-test.sh`, `scripts/omarchy/ssh.sh`,
`scripts/omarchy/netconsole.py`, [../pcie/suspend-test-plan.md](../pcie/suspend-test-plan.md));
`no_console_suspend` on the command line.

| # | Step | What it proves | Risk / stop condition |
|---|---|---|---|
| 0 | **Freeze only** (`/sys/power/state=freeze`) 5× | freezer and the wake path with no device PM | none; if it fails, the problem is not suspend |
| 1 | `mode=1 pm_test=devices` | every `.suspend/.resume`, no noirq | none (1 pass today) |
| 2 | `mode=1 pm_test=platform` 5× | noirq + genpd power-offs + NVMMU keep | low; a device that cannot resume shows up here (already passed 6×) |
| 3 | `mode=1 pm_test=none`, wake with **Home** 5× | real s2idle round trip, timing, Wi-Fi/NBD recovery | medium: a hang needs Power+Home; keep netconsole. (1 pass today) |
| 4 | **Screen off before suspending**: backlight 0, console blank/fb blank, then step 3 | that the display being off does not change resume | low; but `dwi` losing power would leave the panel dark — check the backlight after resume (1 pass today) |
| 5 | `mode=2 pm_test=none` 3× | Wi-Fi port power-cycle + DART1 restore + brcmfmac re-probe in a real sleep | medium: DART1 fault or a dead port ⇒ stop (1 pass today) |
| 6 | **RTC wake**: implement an alarm on the PMU RTC (`angel-pmic-rtc`) so tests need no finger | unattended repeat runs, and the first real "sleep for N minutes then wake" | medium: wrong PMU register writes can kill the PMU rail. Do it read-only first (dump the RTC block, compare with iOS `_rtc_wake`/`setChargerWakeAlarm` at `fffffff006bbae60`/`fffffff006bbb380`), then write. **Do not** guess I2C registers. |
| 7 | **ANS recovery**: on resume, if RDY is not reached in a bounded time, force a port power cycle (PERST with the port powered, `CC=0`, `RDY=0`, release, retrain, NVMMU/DART, full init) instead of retrying `CC.EN` | closes "crash right after resume leaves ANS not ready" without needing a reboot | high: this is the cold path; gate it behind a module parameter, and require the mode-1/2 path to stay the default |
| 8 | `mode=3 pm_test=platform`, then `pm_test=none` | NVMe port power-off and cold-equivalent re-init — where the real screen-off power is | **highest**: a failed retrain loses the root disk. Only after 7, with the `down_ports` fallback and a spare boot medium |
| 9 | **Policy**: screen-off (DPMS/idle) → `systemctl suspend` after N s; `HandlePowerKey` set to `ignore` or `suspend`; wake on Home | the actual product behaviour: "sleep when the screen is off" | low kernel risk; wrong logind policy means the power button shuts the machine down (already a known trap) |
| 10 | **Measurement**: paired discharge runs (`scripts/idle/measure.py`), same brightness/radio/root medium, screen off, s2idle on vs off | whether any of this saves energy | none; without this the whole exercise is unproven |

Ordering notes: 6 and 7 are independent and can be developed in parallel with 8
blocked on 7. Step 10 must not be run from the NBD root with the gadget kept
(`s2idle_keep_gadget`) if the point is to measure the USB block.

**What is deliberately not in this list:** a deeper CPU state (needs the
IORVBAR/trampoline work of [ds-idle-sleep.md §9.2](ds-idle-sleep.md)), display
pipe power-down (needs a display driver), Wi-Fi wake (needs the OOB GPIO wiring
and a WoWLAN-capable firmware path), and AOP wake reasons (needs an AOP driver).

---

## 4. Open questions

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## Appendix A — function / node map

| Address / node | What | Mark |
|---|---|---|
| `fffffff005a1a9d0` | `ApplePMGR` AP wake-source claim: reads 9×u32 via vtable `+0x610`, maps bits to `ap-wake-sources` names, claims via `IOPMrootDomain::claimSystemWakeEvent`, unclaimed → `AOP.unclaimed%u` | V |
| `fffffff005a1da9c` | `ApplePMGR` per-source dump (`"AP wake source: %2u: %s"`) | V |
| `fffffff005a256a8` | PMGR power-state write: target in `[19:16]`, clears `[9:8]` | V |
| `fffffff005a2e280` | PMGR power-state read: current sub-state `(v>>4)&0xf` | V |
| `fffffff005a2e180` | PMGR transition + `"Preparing SEP for sleep"` (target `0xf`) | V |
| `fffffff005e91844` | `AppleS8000PMGR` S2R timer start (PMGR group `0x2a`: `0xc`←2, `0x10`←3) | V |
| `fffffff006721250` | AIC quiesce-safe register write (`"… after quiesce"`) | V |
| `fffffff006779600` | NVMe controller start path: one-time gate `[+0x414]` → `SendTimeToDevice` (vtable `+0xa10`) | V |
| `fffffff00677f000` | NVMe timeout selector, 1 800 000 µs / 35 000 µs | V (code) / I (meaning) |
| `fffffff00677f0a0` | read of DT `nvme-s2r-timeout` | V |
| `fffffff0067bbf58` / `fffffff0067bbfc4` / `fffffff0067bc030` | `enablePCIPort` failure paths (`"Link enable failed!"`, `"enablePCIPort failed !"`) | V |
| `fffffff006bbb380` | PMU S2R preparation: GPIO wake-mask trim, EVENTS/IRQMASK/GPIOCFG dump, `pmu go hib` | V |
| `fffffff006bbae60` | PMU GPIO preset apply | V |
| ADT `pmgr` `ap-wake-sources` | AOP.ExternalReq 1, AOP.CPUWakeupAP 15, AOP.OutboxNotEmpty 27, AOP.APWatchdog 28, AOP.APWakeupTime 29 | V |
| ADT `pmu@3C` | `pmu,d2365`, AIC IRQ 0x4c, `S2RT`, gpio presets, `event_name-gpio*` | V |
| ADT `buttons` | AP GPIO 0x60/0x61 + PMU `STAT` wake-button functions | V |
| ADT `pci-bridge1` | Wi-Fi OOB wake GPIO 0x43, clkreq 0xa5, `manual-enable` | V |
| ADT `soc-tuner` | `function-dispidle_ctrl` (`ADPi`) | V |
| ADT `backlight` | `function-disp_aap_ctrl` (`ADPa`), `function-backlight_enable` | V |
| DTB `pmgr` | `aic`/`gpio`/`pms`/`sbr`/`disp0`/`dp` `apple,always-on` | V |

> Private graphics-dependent excerpt omitted from this source snapshot.

## Appendix B — reproduction (all offline)

> Private graphics-dependent excerpt omitted from this source snapshot.


## Appendix C — corrections to older documents

* [../pcie/suspend.md](../pcie/suspend.md) says "code built, not tried on
  hardware". The 2026-10-22 logs show 13 passing cycles through stage F; the doc's
  status line should be updated (its blocker table is still accurate except
  "rtc-apple-pmic has no alarm", which is the open item for step 6).
* [ds-idle-sleep.md §8](ds-idle-sleep.md) reads "round 5 saw ANS RDY timeouts
  after s2idle". The device record (`docs/STATUS-2026-10-23.md`,
  [../boot/ans-kexec.md](../boot/ans-kexec.md)) says the ANS-not-ready state was
  found **after a panic that followed s2idle**; clean s2idle resumes the ANS fine.
  The mechanism described there (controller stopped, link never dropped) is still
  the right explanation for the wedged state.
* [../pcie/suspend.md](../pcie/suspend.md) lists the AIC as a missing
  `irq_set_wake` provider. It needs nothing: `ps_aic` is always-on and the AIC has
  no wake programming (§1.3). The GPIO fix (0039) is what mattered.
