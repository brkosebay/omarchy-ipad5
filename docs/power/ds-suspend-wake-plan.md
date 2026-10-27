# Wake sources, the ANS pairing rule, and what stands between us and dependable s2idle

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Offline only: no device access, no kernel edits, no commits.
iPad 5 (iPad6,11), S8003 / j71t, Linux/Omarchy.

Read with [ds-suspend-s2idle.md](ds-suspend-s2idle.md) (what iOS does at
sleep, the minimum viable s2idle), [../pcie/suspend.md](../pcie/suspend.md)
(the h9p levels and patches 0036–0043), [../pcie/logs/](../release/OMISSIONS.md)
(the 13 passing cycles of 2026-10-22), [../boot/round8d.md](../release/OMISSIONS.md)
and [../boot/ds-nvme-d-iboot-handoff-state.md](../boot/ds-nvme-d-iboot-handoff-state.md)
(the port/controller pairing rule).

Marks: **[V]** read out of a binary, the live/static ADT, the DTB, the kernel
tree or a device log; **[I]** inference; **[U]** unresolved.

This document does not restate the CPU-idle or PMGR material. It answers three
questions — where the wake sources actually are, what NVMe/PCIe must do at
suspend and resume, and which drivers are missing PM work — and ends with a
test order. It also corrects three statements in
[ds-suspend-s2idle.md](ds-suspend-s2idle.md) that the DTS work depends on
(§1.9).

---

## 0. Bottom line

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 1. Wake sources, exactly

### 1.1 The three interrupt controllers in the path

| Controller | DT node | ADT id | Cells | Notes |
|---|---|---|---|---|
| AIC | `aic@e100000`, phandle 0x16 | `IOInterruptController00000016` | 1 | always-on (`ps_aic`), arms nothing, delivers everything |
| AP GPIO | ADT `gpio@F100000` = DT `pinctrl@20f100000` (`pinctrl_ap`), phandle 0x1e | `IOInterruptController0000001E` | 2 (`<pin> <cfg>`) | 208 pins, 7 AIC lines 42–48, `ps_gpio` always-on |
| D2365 PMU | `pmu@3C` on i2c0, phandle 0x4f | `IOInterruptController0000004F` | 1 (`<event>`) | parent = AP GPIO **pin 76** |

The PMU's controller name is derived from its own `AAPL,phandle` (0x4f), the
same way the GPIO's is 0x1e and the AIC's 0x16. It is an identity, never an
interrupt number. **[V]** live ADT `build/firmware/private/IODeviceTree.txt`
lines 1753–1782, 787–800, 526–541; static ADT `docs/adt/j71t.txt` 2381–2396.

### 1.2 Power button, Home, volume: the AP GPIO path (works today)

| Key | ADT `function-button_*` | Linux DT | Linux | Wake? |
|---|---|---|---|---|
| hold = Power | `30:GPIO(0x61, 0)` | `pinctrl_ap 97`, `KEY_POWER` | `gpio-keys`, IRQ 66 or 67 | **yes**, `wakeup-source` |
| menu = Home | `30:GPIO(0x60, 0)` | `pinctrl_ap 96`, `KEY_HOMEPAGE` | `gpio-keys` | **yes**, `wakeup-source` |
| vol down / up | `30:GPIO(0x8f/0x90, 0)` | `pinctrl_ap 143/144` | `gpio-keys` | no |

Reached from `s800x-ipad5.dtsi` (the `gpio-keys` node uses `GPIO_ACTIVE_LOW`,
which is right for these lines). The E/F logs show `input input1 … parent:
gpio-keys` suspending and resuming and `PM: Triggering wakeup from IRQ 66`
([E:100,180,443]; [F:444]). Both real sleeps woke on the same IRQ; which of the
two keys it is has never been printed. One device line settles it:

```sh
grep -E 'gpio-keys|apple-gpio' /proc/interrupts   # then press Power once, Home once
```

### 1.3 The PMU path: what the second controller adds

`pmu@3C` has one AP-side line and then fans out per-event:

```
pmu@3C:  interrupt-parent = <&pinctrl_ap>  (ADT phandle 0x1e)
         interrupts        = <0x4c 0x1>    (ADT form: pin 76, config 1)
         interrupt-controller; #interrupt-cells = <1>
         AAPL,phandle 0x4f -> IOInterruptController0000004F
         function-suspend_helper = <&pmgr> 'S2RT'
         event_name-gpio{1,6,7,8,10,11,12,13,16} =
             halleffect, battery, bluetooth, wlan, codec,
             halleffect1, dock, belfield, pcie0
```

The `buttons` node proves the fan-out: six specifiers with two parents —
`interrupt-parent = [30 30 30 30 79 79]`, `interrupts = [97 7 96 7 143 5 144 5
48 58]`. Four AP-GPIO pins, then **PMU event 0x30 (hold)** and **0x3a (menu)**.
The PMU also owns the two iOS wake functions:

```
function-wake_button_hold = 79:STAT(0x1a9)
function-wake_button_menu = 79:STAT(0x1a8)
```

i.e. while the AP is powered down, iOS gets the button from the PMU, not from
the AP GPIO block. **[V]** static ADT 3240–3254; live ADT 3217.

For us this path is **optional**: the AP GPIO block is always-on in s2idle, so
pins 96/97 already wake the AP. The PMU path matters for the sources that have
no AP GPIO line of their own (RTC, VBus).

`function-suspend_helper` points at phandle 24, which the static ADT identifies
as `pmgr1,s8000` (line 1705): the PMU calls the PMGR's `S2RT` to run the SoC
half of system sleep. Nothing in Linux implements it and s2idle does not need
it. **[V]**

### 1.4 Charger plug

Two independent paths exist in hardware, and only one of them is in our DT:

| Path | Evidence | Linux today |
|---|---|---|
| Tristar CBTl1610 IRQ → **AP GPIO 72** (ADT tristar `interrupt-parent = <&gpio>`, `interrupts = <0x48 3>`) | live ADT 1795–1805; static ADT `tristar` node | **not described**, driver polls |
| VBUS → PMU (`setChargerWakeAlarm`, `"late vbus detect. scheduling rtc immediate wake"`, charger node `function-power_supply_change = 133:pmuI(0x0)`, `charger-sleep-interval = 300`) | PMU kext strings; static ADT 2409, 3269 | **not described** |

The tristar path is the cheaper one to implement, because the charger driver
already has a work item and a quiesce/resume pair — the IRQ only has to schedule
it. The PMU path is what iOS uses and it also arms an RTC alarm to re-check the
charger later. §1.8 keeps the PMU as the primary fix and lists the tristar hunk
as the low-risk alternative for "wake on plug" only. **[V]** wiring, **[I]**
choice.

### 1.5 RTC

There is **no RTC node in the ADT**: the RTC lives inside the D2365 and is
exposed to iOS only through `AppleD2365PMU` (`_rtc_wake`,
`setChargerWakeAlarm`, `"upo: wake in %d"`). Linux's `rtc@500` is a mainline
invention (`apple,angel-pmic-rtc`) and its ops are `read_time`/`set_time`
only — no `read_alarm`, no `set_alarm`, no `alarm_irq_enable`, no IRQ, no
wakeirq. **[V]** `drivers/rtc/rtc-apple-pmic.c` lines 186–188.

So `rtcwake` is impossible today for two independent reasons: the RTC class has
no alarm op, and nothing is armed to wake the AP. Both are needed, in that
order. The PMU register map for the alarm (`0x500` block) is **[U]** and must
come from `_rtc_wake`/`setChargerWakeAlarm`, not from guessing I²C writes.

### 1.6 The GPIO presets are driver defaults, not ADT properties

`gpio-activate-defaults`, `gpio-suspend-defaults`, `gpio-quiesce-defaults` and
`gpio-pin-config` appear **only** in the `AppleD2365PMU` kext's `__cstring`
(plus `start: gpio-pin-config is sized incorrectly!`). They are **not** in the
live ADT iBoot publishes and **not** in the static ADT. The driver therefore
uses built-in tables and lets the ADT override them. **[V]** strings in
`build/power/s2idle/kexts/AppleD2365PMU.__TEXT.__cstring.bin`; absence checked
in both ADT dumps.

What the two functions do (prior round, unchanged):

* `fffffff006bbae60` applies one preset (per-line config bytes).
* `fffffff006bbb380` is the S2R preparation: it trims the GPIO wake mask
  (clears the shadow bit for pins whose config byte `& 0xc1 == 1`), dumps
  `EVENTS`/`IRQMASK`/`GPIOCFG` (0x1e/0x1e/0x18 bytes), logs `pmu go hib`, then
  calls the PMGR's `S2RT`. **[V]** earlier round.

Consequences for Linux, and they are all "do not":

* The wake mask cannot be read out of the DT — do not try to encode it.
* `gpio-apple-d2365` models the config byte as `bit0 value | bit3 input |
  bit4 function` at `0x900 + 2*line` (`0x8bd + 6*line` above line 17). Touching
  the mode of a wake-capable line silently changes what the PMU can wake on.
  Keep `gpio-reserved-ranges` and never clear function bits (the driver already
  refuses a line in function mode). **[V]** driver source.
* If we ever write a suspend preset ourselves we must reproduce the
  `& 0xc1` rule, and we have no way to validate it offline. Do not.

### 1.7 What can wake Linux today, and what could

| Source | Hardware path | Linux state | Needs |
|---|---|---|---|
| Power button | AP GPIO 97 → AIC (via GPIO group IRQ) | **works** (IRQ 66) | nothing in the kernel |
| Home button | AP GPIO 96 | **works** | nothing |
| Volume keys | AP GPIO 143/144 | not wake | — |
| RTC alarm | PMU RTC → PMU event → GPIO 76 → AIC | impossible | alarm ops + PMU IRQ + arming |
| Charger plug | Tristar → GPIO 72, or VBUS → PMU → GPIO 76 | impossible | DTS + IRQ handler, or PMU IRQ |
| Smart Cover | PMU GPIO line 0/10 → PMU event → GPIO 76 | impossible (polled today) | PMU irqchip (see ds-als-hall §4.2 Option B) |
| Touch | AP GPIO 65 (`attn`), no `wakeup-source` | not wake | `wakeup-source` + a resume that cannot touch a gated SPI |
| Wi-Fi OOB | AP GPIO 67 (`device-wake-gpios`), no IRQ, no WoWLAN | not wake | firmware + DT + brcmfmac work |
| BT | AP GPIO 142 (`device-wakeup-gpios`), no host-wake IRQ | not wake | DT + `hci_bcm` work |
| AOP | five `ap-wake-sources` channels | no driver | out of scope |

### 1.8 The DTS diff

Two real defects on the PMU node:

* **D1 — no interrupt at all.** `pmic@3c` in `s800x-ipad5.dtsi` has no
  `interrupt-parent`/`interrupts`, so the D2365 event line has no Linux
  consumer; `gpio-apple-d2365` polls, and nothing else can ever see a PMU event.
* **D2 — not described as an interrupt controller.** The ADT publishes
  `interrupt-controller`, `#interrupt-cells = 1` and an event domain, which is
  how the PMU fans one line out to buttons/charger/RTC/cover. Without it, no
  child can name an event and no driver can build a domain.

```diff
--- a/arch/arm64/boot/dts/apple/s800x-ipad5.dtsi
+++ b/arch/arm64/boot/dts/apple/s800x-ipad5.dtsi
@@ -70,6 +70,28 @@
 		#address-cells = <1>;
 		#size-cells = <1>;
 
+		/*
+		 * D2365 event line. Live ADT pmu@3C:
+		 *   interrupt-parent = <&gpio>   (phandle 0x1e)
+		 *   interrupts       = <0x4c 0x1>
+		 * 0x4c is AP GPIO 76, not an AIC line. "IOInterruptController
+		 * 0000004F" is this node's own AAPL,phandle, i.e. the id of the
+		 * event domain below, not an interrupt. The line is open-drain,
+		 * active low, and acked by reading the PMU event bank (no status
+		 * write), so it is level-triggered here.
+		 */
+		interrupt-parent = <&pinctrl_ap>;
+		interrupts = <76 IRQ_TYPE_LEVEL_LOW>;
+		wakeup-source;
+
+		/*
+		 * The PMU fans the single AP line out to its own event domain:
+		 * <event> in a child specifier. Only valid together with the MFD
+		 * irq_chip; see docs/power/ds-suspend-wake-plan.md section 1.8.
+		 */
+		interrupt-controller;
+		#interrupt-cells = <1>;
+
 		rtc@500 {
 			compatible = "apple,angel-pmic-rtc", "apple,antigua-pmic-rtc";
 			reg = <0x500 0x10>;
```

Both hunks were applied to a copy of the integration tree and run through
`cpp -nostdinc -undef -D__DTS__` + `dtc` (`s8003-j71t.dts` → 32 616-byte DTB,
no warnings); the compiled node reads `interrupts = <0x4c 0x08>`, i.e. pin 76,
`IRQ_TYPE_LEVEL_LOW`. Nothing was built or committed in the tree itself.

What must land with it (DTS alone changes nothing, and D2 alone can make a child
fail its probe):

| Hunk | Driver obligation | Unlocks |
|---|---|---|
| `interrupt-parent` + `interrupts` + `wakeup-source` | Request the parent IRQ **threaded** (`IRQF_ONESHOT`, no hardirq I²C), `device_init_wakeup()` from `wakeup-source`, `enable_irq_wake()` in `.suspend`, read the PMU event/status bank in the thread, log the event ids | wake **reason**; a foundation for charger/RTC/cover |
| `interrupt-controller` + `#interrupt-cells` | A `regmap_irq` (or irq_domain) on the PMU, `irq_chip_set_wake_parent()` so a child wake propagates to GPIO 76, `handle_nested_irq` for children | child events (RTC alarm, charger, PMU GPIO lines) |

No child may carry an `interrupts` property until the domain exists — otherwise
`irq_of_parse_and_map()` returns 0 and the child (today: the RTC) fails probe.
That is why the RTC's alarm event is *not* in this diff: the PMU event index for
the RTC alarm is not in either ADT (**[U]**) and must come from the `_rtc_wake`
work in §4 step 8.

Optional, lower-risk alternative for charger wake only (needs a real handler in
`apple_tristar_power`, which today polls). It goes in the board file, not the
common dtsi:

```diff
--- a/arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi
+++ b/arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi
@@ -114,6 +114,10 @@
 		usb-controller = <&usbdev>;
 		apple,pmic = <&{/soc/i2c@20a110000/pmic@3c}>;
 		reg = <0x1a>;
+		/* ADT tristar: interrupt-parent gpio, interrupts <0x48 3> */
+		interrupt-parent = <&pinctrl_ap>;
+		interrupts = <72 IRQ_TYPE_EDGE_BOTH>;
+		wakeup-source;
 	};
 };
```

**Do not touch the `pmu { }` node in `s800-0-3.dtsi`.** See §1.9.

### 1.9 Why the "two pmu DTB defects" in the previous document are not where it says

[ds-suspend-s2idle.md §2.3](ds-suspend-s2idle.md) says the `pmu` node has
`interrupts = <0 0x4c 4 0 0x4f 4>`, that the second entry is spurious, and that
the ADT says `0x4c` is edge while the DTB says level-high. Every step of that
chain crosses nodes:

1. `<0 0x4c 4>, <0 0x4f 4>` is the **`pmu { compatible = "apple,twister-pmu" }`**
   node in `s800-0-3.dtsi:338` — three cells per entry: `AIC_IRQ 76
   IRQ_TYPE_LEVEL_HIGH`, `AIC_IRQ 79 IRQ_TYPE_LEVEL_HIGH`. It is the CPU
   performance-monitor (perf) block, with `interrupt-affinity = <&cpu0 &cpu1>`.
2. Both numbers are real AIC lines. The AIC node publishes
   `target-destinations = <0x4c 1 0x4f 2>`, which reads as {line, target CPU}
   pairs — line 76 to CPU 1, line 79 to CPU 2 — exactly the shape a per-core
   perf interrupt needs **[V]** values, **[I]** pairing. `apple-m1-cpu-pmu`
   binds this node with `interrupt-affinity = <&cpu0 &cpu1>` and appears in
   every suspend log. Removing the 0x4f entry would break perf on CPU1 and the
   driver's one-IRQ-per-CPU contract. **[V]**
3. The `0x4c`/`0x4f` pair in the *PMU* is a different pair with the same
   numbers by coincidence: `0x4c` is the **GPIO pin** and `0x4f` is the
   **phandle**. Proof: `pmu@3C` has `interrupt-parent = <0x1e>` and
   `IOInterruptControllers = ("IOInterruptController0000001E")` — both name the
   AP GPIO controller, not the AIC — and the GPIO node declares
   `#interrupt-cells = <2>`.
4. There is therefore no AIC trigger flag on the D2365 line, so "the ADT says
   edge, the DTB says level" compares an Apple GPIO *config* word with a Linux
   `IRQ_TYPE_*`. They cannot be compared. The config word is a small bitfield,
   not `IRQ_TYPE_*`: in the same ADT the buttons are `7` and `5`, tristar `3`,
   the touch `0`, the PMU/ALS/codec `1` — and `7` cannot be
   `EDGE_RISING|EDGE_FALLING|LEVEL_HIGH`. The exact decode of that field is
   **[U]** offline (the `AppleARMGPIO` interrupt-config path is not in the
   shared Ghidra project). **[V]** values.

The chosen `IRQ_TYPE_LEVEL_LOW` in §1.8 is **[I]**, from the hardware: an
open-drain active-low interrupt line that stays asserted until the handler
reads the PMU event bank, with no status-write acknowledge. If the first device
run shows the line stuck (interrupt count climbing with no new events), the
fallback is `IRQ_TYPE_EDGE_FALLING`; that is a one-line change and it does not
affect any other decision in this document.

The other correction worth carrying forward: the PMU's **AIC IRQ 76** never
existed. `ds-suspend-s2idle.md` §1.3, §2.2 and Appendix A all say "AIC IRQ
0x4c (76)". The PMU is on AP GPIO 76. The Linux-side consequence is not
cosmetic: an AIC-76 assumption would collide with the CPU perf interrupt, which
is how the error survived review.

---

## 2. The ANS rule, and the suspend/resume sequence it requires

### 2.1 The rule

> **A port cycle and a live controller never mix.** Before PERST#/port power
> can move, the endpoint must be a *disabled* controller (`CC.EN=0`, `SHN=00`,
> `RDY=0`, `BME=0`). And a controller left in `SHST=10b` with `CC.EN=1` is a
> state no Apple component produces; the only measured outcome of pairing it
> with a port bring-up is the round-8c hang.

The two legal pairs, from iBoot and xnu (see
[ds-nvme-d](../boot/ds-nvme-d-iboot-handoff-state.md) §8D):

| Outgoing controller | Outgoing port | Incoming |
|---|---|---|
| `EN=1, SHN=00, RDY=1` (iBoot jump) | up | live-link: `CC=0` → `RDY=0` → reinit, **no port cycle** |
| `EN=0, SHN=00, RDY=0` (iBoot stop) | up | same |
| `EN=0, SHN=00, RDY=0` | **down** (`0x124`/`0x114`/`0x100` clear) | **full bring-up**, then `CC=0` → reinit |
| `EN=1, SHN=01, SHST=10b` | either | **does not exist in Apple code — ours** |

iOS system sleep picks the third row: controller stop, then the port deep-sleep
state machine, then everything again on wake, including DART and NVMMU because
the port's `force_active` goes 0→1
([ds-suspend-s2idle.md §1.4](ds-suspend-s2idle.md), [../pcie/suspend.md](../pcie/suspend.md)).

### 2.2 What the current code does

Suspend (all levels), `nvme_suspend()` → `nvme_disable_prepare_reset(ndev, true)`
→ `nvme_dev_disable(dev, true)`:

* drain, delete I/O queues, `nvme_disable_ctrl(ctrl, shutdown=true)` =
  `CC←(CC & ~SHN_MASK) | SHN_NORMAL`, then wait for `CSTS.SHST=10b`;
* `CC.EN` is **not** cleared (`nvme_disable_ctrl()` only clears `EN` when
  `shutdown=false`) — so the endpoint is left `CC=0x464001`, `CSTS=0x9`;
* the 8d stop that *would* clear `EN` exists (`nvme_pci_apple_ans_shutdown()`,
  `drivers/nvme/host/pci-apple-ans.c:868`) but is reachable only from
  `nvme_dev_disable()` under `shutdown && kexec_in_progress` (pci.c:3707–3724);
* three of its failure paths `panic()` (SHST failed, `CC` disable failed, DART
  inactive failed).

Port side, `apple_h9p_pcie_suspend_noirq()` (h9p:2812) runs after the endpoint
suspended and, for the ports in `sleep_ports`, marks `down_ports`, clears
`enabled_ports` and runs `apple_s8003_port_sleep()` (h9p:2263):
`PCI_COMMAND=0`, bus numbers 0, `PME_TO`, wait `L2`, PERST#, 60 µs, port off
(`0x124`/`0x114`/`0x100`). `sleep_ports` is: mode 1 → 0, mode 2 → ports with
`pwren-gpios` (Wi-Fi), mode 3 → every port.

Resume, `apple_h9p_pcie_resume_noirq()` (h9p:2952) → per port `setup_port()`
(full: refclk, power, ready, 100 µs, PERST release, PHY, LTSSM), `pci_restore_state(rp)`,
100 ms, then `apple_h9p_nvmmu_resume()` (check, re-program if lost or retrained).
Then `nvme_resume()` → `nvme_try_sched_reset()` → `nvme_pci_enable()` (which
refuses a disconnected device, patch 0037) → ANS `preinit`: check link,
`CC=0`, wait `RDY=0`, `dart_set_active(true)`, rebuild NVMMU, then `CC=0x460001`
and wait `RDY=1`. That is a cold-equivalent re-init and it is **already
correct**. **[V]** E log: 732 (`CC=0x464001 CSTS=0x9`), 735 (enable),
851.285→851.412 (queues up).

### 2.3 The sequence to implement

Order is the whole point: everything below happens **before** `h9p`'s
`suspend_noirq`, and only then may `sleep_ports` include port 0.

```text
suspend, nvme .suspend  (process context, device PM order: endpoint before bridge)
 1  freeze blk-mq, drain, sync, delete I/O queues            (already)
 2  SHN=01, wait CSTS.SHST==10b, bound ctrl->shutdown_timeout (already; 5 s default)
 3  CC <- CC & ~(EN|SHN); wait RDY==0, bound 2 s              8d stop
 4  read back CC (require 0x460000), CSTS (require 0x0), reject CFS/0xffffffff
 5  NVMMU invalidate FIFO empty check (port0 +0x22c bit0), 20 ms
 6  pci_clear_master(); DART force_active(0)                  8d tail
 7  return 0, or -EBUSY to abort the whole suspend

suspend, h9p .suspend_noirq
 8  if mode >= 3: down_ports |= BIT(0); port_sleep(0)         order matters

resume, h9p .resume_noirq
 9  setup_port(0) full bring-up, then pci_restore_state(rp)
10  msleep(100)                                              PCIe 6.6.1
11  NVMMU: state kept? else re-program                         (already)

resume, nvme .resume
12  nvme_try_sched_reset -> ANS preinit: CC=0, RDY=0, DART active,
    NVMMU rebuild, CC=0x460001, RDY=1                          (already)
```

The only edits are steps 3–7: bring the existing 8d body into reach for
`suspend`, and make its failures non-fatal for this caller. In
`nvme_dev_disable()` there are three things behind `kexec_in_progress` today,
and all three need the sleep case: the queue sync + `apple_ans_nvmmu_drain()`
(step 5), the `nvme_disable_ctrl(shutdown=true)` log lines, and the
`->shutdown()` call (steps 3–7). The drain panics on failure as well, so it
needs the same `fatal` flag.

Concretely, in `drivers/nvme/host/pci.c`:

* `nvme_dev_disable(dev, shutdown=true)` currently gates the ANS stop on
  `kexec_in_progress`. Add the sleep case:
  `bool for_sleep = pm_suspend_target_state != PM_SUSPEND_ON;` and run
  `dev->dma_ops->stop_for_sleep(dev)` (a new op, or `->shutdown()` with a
  `panic_on_failure = false` argument) when `for_sleep` is set.
* Do **not** reorder inside `nvme_dev_disable()`: the stop must run before
  `pci_disable_device()` clears `PCI_COMMAND_MEMORY`, exactly where the kexec
  call already sits (pci.c:3721). After that point BAR writes are not safe.
* Keep the kexec behaviour byte-for-byte: same messages, same panics when
  `kexec_in_progress`.

In `drivers/nvme/host/pci-apple-ans.c`, split the existing function:

```c
/* 8d body, minus the panics: used by both kexec (panic) and sleep (-EBUSY). */
static int nvme_pci_apple_ans_stop(struct nvme_dev *dev, int shst_ret, bool fatal);
```

with `fatal=true` for the kexec caller and `fatal=false` for the sleep caller
(step 7 returns `-EBUSY`, `dpm_run_callback()` fails, `suspend_stats.failed_suspend`
increments and the system stays awake — the correct outcome, and it must never
turn a refused sleep into a panic).

Also required, or the sequence is not safe:

* **Do not let mode 3 exist without the stop.** Cheapest sufficient guard: the
  NVMe driver sets a flag (or a second module parameter) after a successful
  8d stop, and `apple_h9p_pcie_suspend_noirq()` refuses port 0 (and warns) if
  `sleep_ports` contains it while that flag is clear. Do not try to read the
  endpoint's `CC` from `h9p`: that is a BAR access over a link the bridge is
  about to own, and it is the same class of access patch 0037 exists to
  prevent.
* **Timeout budgets**: SHN runs under `ctrl->shutdown_timeout` (5 s by
  default), `RDY=0` under 2 s, the NVMMU FIFO check under 20 ms, plus the
  per-read link check. `panic=0` on the command line does not help inside
  `.suspend`; a timeout there must return, not spin.
* **Never read endpoint BARs after the port is down.** `h9p` already sets
  `down_ports` before `port_sleep`; `nvme` resumes only after `setup_port()`.
  Keep both.

### 2.4 Modes

| Mode | ports asleep | Controller state before `port_sleep` | ANS-safe? |
|---|---|---|---|
| 1 | none | `EN=1, SHST=10b` (today) | resume works because the link never dropped — **not** a pairing we should keep |
| 2 (shipped) | Wi-Fi only | same for NVMe; Wi-Fi does its D3_INFORM handshake | OK as deployed |
| 3 | all, NVMe included | must be `EN=0, SHN=00, RDY=0` | **only after §2.3 step 3–7** |

Mode 2 is what `/etc/tmpfiles.d/linux-a9-suspend.conf` writes today, and it
passed 3× plus one real sleep. Changing NVMe's suspend to the 8d stop is
*compatible* with mode 2 (the controller is disabled, the link stays up, resume
re-inits), so it can be tested at mode 2 before mode 3 ever runs. That is the
point of §4 steps 3–4.

---

## 3. Per-driver table

"Domain" is the PMGR power domain the block sits in; "gated?" is whether s2idle
may drop it (all domains except `apple,always-on` are candidates — see
§3.1). Verdicts are what the driver must do before the feature is dependable.

> Private graphics-dependent excerpt omitted from this source snapshot.

### 3.1 Which domains actually drop — measure, do not assume

`genpd_finish_suspend()` powers a domain off in the noirq phase only if it is
not always-on, **every** device in it has suspended
(`suspended_count == device_count`, `sd_count == 0`), and its children are in
their deepest state. So the candidate list is "not always-on" minus "has a
still-active consumer".

Measured so far, from the 2026-10-22 logs: `disp0`/`dp` **did** drop (the panel
went black; patch 0043), `ps_gpio` would have (0041), and **`i2c0` did not** —
`apple-j71t-charger` wrote a source budget over I²C at 851.790 s, 0.17 s after
resume, with no PM callback in `i2c-pasemi-platform.c` at all. **[V]**

That last data point matters: it means the "everything gets gated" reading is
wrong, and the per-driver verdicts in §3 cannot be settled by reading genpd
alone. §4 step 2 is the measurement that settles all of them in one cycle.

---

## 4. Ordered device test plan

Prerequisites: the payload from
[../pcie/suspend-test-plan.md](../pcie/suspend-test-plan.md) §0
(`no_console_suspend`, netconsole, `panic=0`), netconsole running, and the
`pcie_apple_h9p.suspend` / `dwc2.s2idle_keep_gadget` values set by hand rather
than by the tmpfiles entry, so a failed step cannot survive a reboot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Notes on ordering:

* 1 must pass before 3, because 1 exercises the new suspend callback with the
  link up; if the stop is wrong, 1 fails in the cheapest possible configuration.
* 2 is free and should run before any driver work is written: it converts six
  **[I]** verdicts in §3 into **[V]** facts.
* 10 is blocked on 1, 3, 4 and the guard in §2.3; without the guard, mode 3
  reproduces the round-8c state with a power cycle on top.
* Do not run 10 from the NBD root: losing port 0 loses the root. Use the NVMe
  root and a spare payload.
* `rtcwake` (8) and charger wake (9) are the only steps that need the PMU IRQ;
  the product goal in the title does not.

---

## 5. Open questions

1. The Apple GPIO interrupt-config word (second cell of an `interrupts`
   property whose parent is a GPIO controller). Needed to read iOS's intent for
   the PMU/RTC/cover lines instead of inferring it. The decode lives in
   `AppleARMGPIO`/`AppleGPIOICController` (`com.apple.driver.
   AppleGPIOICController` at `0xfffffff0054eabc0` is the small base class;
   the shared Ghidra project has no function boundaries there yet).
2. Which PMGR domains lose power in s2idle on this SoC (§3.1) — the one
   measurement that turns §3 from inference into fact.
3. The D2365 RTC alarm register map and its event index (`_rtc_wake`,
   `setChargerWakeAlarm`), needed for `rtcwake` and for iOS's "late vbus detect"
   behaviour.
4. Whether the PMU event bank is read-to-clear or level-held. It decides whether
   the PMU line is level or edge (§1.9) and whether the handler needs
   `IRQF_ONESHOT` masking.
5. Whether `ps_dwi` gating actually leaves the panel dark on our unit, or the
   DWI register survives (step 5 answers it).
6. Whether the display pipe can be idled on A9 without a display driver
   (`ADPi`/`dispidle_ctrl`, 0x57 bytes, not decoded). Until then, "screen off"
   is the backlight, and the pipe keeps scanning.

---

## Appendix A — evidence index

| Claim | Source |
|---|---|
| PMU on AP GPIO 76, parent 0x1e, 1-cell event domain, phandle 0x4f | live ADT 1753–1782; static ADT 2381–2396; also [../periph/ds-als-hall.md §3.1](../periph/ds-als-hall.md) |
| Buttons: 4 AP GPIO specs + 2 PMU events, `STAT` wake functions | static ADT 3240–3254; live ADT 3217 |
| AIC `target-destinations` 0x4c→CPU1, 0x4f→CPU2 | live ADT 526–541 |
| twister-pmu node = AIC 76/79, `interrupt-affinity` | `s800-0-3.dtsi:338`; `apple-m1-cpu-pmu` in every s2idle log |
| `pmic@3c` has no interrupt | `s800x-ipad5.dtsi:67` |
| power/home = pins 97/96, wakeup-source | `s800x-ipad5.dtsi:14–27`; static ADT `function-button_*` |
| wake from IRQ 66 | `../pcie/logs/2026-10-22-s2idle-E.txt:443`, `-F.txt:444` |
| ANS left `CC=0x464001`, `CSTS=0x9` | E log 732; `nvme_disable_ctrl()` `core.c:2746` |
| 8d stop exists, gated on kexec | `pci-apple-ans.c:868–925`; `pci.c:3707–3724` |
| port sleep order (PME_TO, L2, PERST, off) | `pcie-apple-h9p.c:2263–2295` |
| NIC resume = full re-init | `pci-apple-ans.c:547–576` (CC=0, DART active, NVMMU rebuild) |
| genpd gating rule | `drivers/pmdomain/core.c` `genpd_finish_suspend` |
| `i2c0` survived the sleep (no i2c PM) | E log 802; `i2c-pasemi-platform.c` (113 lines, no PM) |
| PMU preset names are kext defaults | `AppleD2365PMU.__TEXT.__cstring.bin`; absent from both ADTs |
| deployed policy (`suspend=2`, hypridle, `ipad-sleep`) | `scripts/omarchy/rootfs/files/etc/tmpfiles.d/linux-a9-suspend.conf`, `.../bin/ipad-sleep`, `.../hypr/hypridle.conf`, `.../hypr/ipad.lua` |
| "Power sleeps, Home wakes" on device | `docs/test-logs/integ10/README.md` |

## Appendix B — reproduce (all offline)

```sh
# ADT facts
rg -n 'pmu@3C|IOInterruptController0000001E|IOInterruptController0000004F|target-destinations' \
   build/firmware/private/IODeviceTree.txt
sed -n '2381,2396p;3240,3256p' docs/adt/j71t.txt

# kernel-side facts
scripts/build-kernel.sh --run boot 'cd /src/wt/boot;
  sed -n "2263,2300p;2812,2830p;2952,3000p" drivers/pci/controller/pcie-apple-h9p.c;
  sed -n "3675,3750p;4471,4500p" drivers/nvme/host/pci.c;
  sed -n "868,925p" drivers/nvme/host/pci-apple-ans.c'

scripts/build-kernel.sh --run integ 'cd /src/wt/integ;
  grep -n "pm_runtime\|dev_pm_ops" drivers/input/touchscreen/apple_n1.c \
    sound/soc/apple/s8000-mca.c sound/soc/codecs/cs42l81.c \
    drivers/video/backlight/apple_dwi_bl.c drivers/spi/spi-apple-s8000.c \
    drivers/rtc/rtc-apple-pmic.c drivers/power/supply/bq27xxx_battery_hdq.c'

# PMU kext strings (presets are defaults, not ADT)
strings -a build/power/s2idle/kexts/AppleD2365PMU.__TEXT.__cstring.bin |
  grep -E 'gpio-(activate|suspend|quiesce)-defaults|gpio-pin-config|_rtc_wake|setChargerWakeAlarm'
```

## Appendix C — corrections to [ds-suspend-s2idle.md](ds-suspend-s2idle.md)

* §1.3, §2.2, §2.3 and Appendix A: the D2365 is **not** "AIC IRQ 0x4c (76)".
  It is AP GPIO 76; 0x4c is the pin and 0x4f is the PMU's own phandle/domain id.
  The AIC line 76 belongs to `apple,twister-pmu`.
* §2.3: the "two DTB details on the `pmu` node" are two details of the perf
  node, and both are correct as written in the DTB. §1.9 above.
* §1.3 table: `gpio-activate-defaults`/`gpio-suspend-defaults`/
  `gpio-quiesce-defaults` are `AppleD2365PMU` built-in defaults, not ADT
  properties (they are absent from both dumps).
* §2.3 "exactly two IRQs are marked `wakeup-source`": still true, and still the
  two buttons — but the reason the power button can be the product wake source
  is `ps_gpio` being always-on, not the PMU.
* Appendix C's own correction of [../pcie/suspend.md](../pcie/suspend.md)
  ("code built, not tried") stands; §0.1 above adds that it is deployed.
