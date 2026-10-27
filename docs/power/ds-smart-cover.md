# Smart Cover → suspend and wake on j71t

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Offline study: no device access, no kernel edits, no commits.

> **Correction (same day, [smart-cover-test-plan.md](smart-cover-test-plan.md)):**
> events are at **0x140** (clear on read), 0x180 is live status, and **0x1c0
> is the interrupt mask (set = masked)**. Do not "ack" by writing 0x1c0. In
> iOS sleep only lines with `GPIOCFG & 0xc1 == 1` are unmasked at 0x1c6, so
> §0.5, §3.1 and the 0x1c0 rows of §4B are superseded there.
iPad 5 (iPad6,11), S8003 / j71t, Linux/Omarchy. Read with
[ds-als-hall.md](../periph/ds-als-hall.md) (the ALS + hall study this
supersedes for the cover), [ds-suspend-wake-plan.md](ds-suspend-wake-plan.md)
(the PMU event line and the DTS diff), [s2idle-test-plan.md](s2idle-test-plan.md)
(what already sleeps and wakes), [tablet-usage.md](../omarchy/tablet-usage.md)
and [sensors-test-plan.md](../periph/sensors-test-plan.md) (the polled switch
that exists today).

Marks: **[V]** read out of the ADT, the DTB, a binary or a source file in the
tree; **[I]** inference from that evidence; **[U]** not decidable offline.

## 0. Bottom line

1. **The cover is a D2365 PMU input, not an AP GPIO and not an AOP input.**
   Two hall lines (PMU GPIO 0 and 10) are reported to the AP as PMU *events*
   `halleffect` and `halleffect1` through the PMU's own 1-cell interrupt
   domain, whose single parent is **AP GPIO 76**, active low. **[V]**
2. **The AOP is not involved.** No `cover`, `hall`, `lid` or `smart` string in
   the iPad6,11 AOP firmware (`ipad6faop-20H350.bin`, its `.osbin` and the
   extracted `aop.bin`), `AppleSPUSmartCoverDriver` has 0 instances on the live
   unit, and the ADT's `ap-wake-sources` lists only AOP.ExternalReq,
   CPUWakeupAP, OutboxNotEmpty, APWatchdog and APWakeupTime. **[V]** The
   `aop_smart_cover` notification name exists in `AppleSPU.kext`, but nothing
   on this board publishes it.
3. **iOS reads it as an interrupt, one filter interrupt source per line, and
   interprets the level, not an edge.** The ADT binds them exactly like the
   power/home buttons (`buttons` node, `function-button_halleffect =
   79:GPIO(0x0,0)`), so `AppleM68Buttons` handles them; no debounce override is
   set for the halls, so the kext default applies: **14000** for
   `button-delay`, `button-debounce-on` and `button-debounce-off`
   (`0x124f8` = 75000 is set for hold/voldown/volup only). **[V]** code,
   **[I]** µs (≈14 ms).
4. **Polarity and the pin mode are still guesses.** `_setGPIOFunction`
   (`0x6bbd4a8`) can put a line in plain GPIO mode (function 2: clear bit 4
   FUNCTION, set bit 3 INPUT), but the ADT property carries only two arguments
   (line, 0) and which function value the halls get is not resolved offline —
   the mode bits may be whatever the PMU defaults to. **[V]** the mechanism,
   **[U]** the state. The polled switch already in the tree answers both the
   mode and the polarity question: `ipad-lid --watch`.
5. **iOS can wake on the cover, and it is the PMU that does it.** The D2365
   has a 3-byte GPIO wake mask at **0x1c6** (24 lines = events 0x30–0x47) that
   the iOS sleep path rewrites from the per-line config; the only lines it
   masks off are outputs driven high, so **every input line — including both
   halls — stays wake-enabled**; the PMU's wake-reason dump names those lines
   `gpio1…gpio24` (1-based, so line 0 = `gpio1`, line 10 = `gpio11`). There is
   no `function-wake_button_halleffect` in the ADT, so this is the GPIO wake
   path and not the "synthetic button" path used by hold/menu. **[V]** mask
   and dump, **[I]** that cover-open wake uses it.
6. **Linux cannot wake from the cover today for one structural reason:**
   the switch is read by `gpio-keys-polled` (a delayed work, cancelled on
   suspend), `gpio-apple-d2365` has no `irq_chip`, and the PMU node in the
   sensors branch has no `interrupt-parent`/`interrupts` at all — so there is
   no interrupt to arm and nothing can end the sleep. **[V]** wiring,
   **[I]** the driver detail.
7. **The fix is the event path, not a second polled device**: PMU IRQ →
   `regmap_irq`/`irq_domain` → `gpio-keys` with `interrupts-extended`, or a
   small input driver on the same IRQ. The parent DTS node already exists in
   `patches/power/s2idle-round1/0001-0002`; what is missing is the irqchip and
   the child. **[V]** tree, **[I]** design.
8. **Keep `HandleLidSwitch=ignore` until that lands.** With a real `SW_LID`
   device and no wake path, logind's default would suspend on close and the
   user would have to press Home — and once the lid device is a wake source,
   flipping the drop-in to `suspend` is the whole of the policy work. **[I]**

## 1. Where the sensor is wired

### 1.1 ADT (live dump `build/firmware/private/IODeviceTree.txt`, static `docs/adt/j71t.txt`)

```text
/arm-io/i2c0/pmu@3C            compatible = "pmu,d2365"
  reg = <0x3c ...>, interrupt-controller, #interrupt-cells = <1>
  interrupt-parent = <0x1e>    ; AP GPIO controller (pinctrl_ap)
  interrupts       = <0x4c 0x1>; AP pin 76, config 1
  AAPL,phandle = 0x4f          ; IOInterruptController0000004F
  event_name-gpio1  = "halleffect"     ; 1-based over PMU GPIO line 0
  event_name-gpio11 = "halleffect1"    ;                 PMU GPIO line 10
  (also gpio6 battery, gpio7 bluetooth, gpio8 wlan, gpio10 codec,
         gpio12 dock, gpio13 belfield, gpio16 pcie0)

/buttons                       compatible = "buttons"
  button-names = "hold","menu","voldown","volup","halleffect","halleffect1"
  function-button_halleffect  = 0x4f:"IOGP"(line 0,  flags 0)
  function-button_halleffect1 = 0x4f:"IOGP"(line 10, flags 0)
  interrupt-parent = <0x1e,0x1e,0x1e,0x1e,0x4f,0x4f>
  interrupts       = <0x61 7>,<0x60 7>,<0x8f 5>,<0x90 5>,<0x30>,<0x3a>
  button-delay_hold/voldown/volup = 0x124f8        ; none for the halls
```

Two numbers matter and are easy to mix up:

| Thing | halleffect | halleffect1 | Status |
|---|---|---|---|
| PMU GPIO line | 0 | 10 | **[V]** `function-button_*` |
| PMU event id (child specifier) | **0x30** (48) | **0x3a** (58) | **[V]** `/buttons` `interrupts` |
| `event_name-gpioN` (1-based) | gpio1 | gpio11 | **[V]** |
| Event id rule | `event = 0x30 + line` | same | **[I]** holds for both halls, and the iOS wake dump prints `gpio{line+1}` for exactly the bytes at 0x30+ |

### 1.2 What it is not

| Candidate | Evidence | Verdict |
|---|---|---|
| AP GPIO input | No `halleffect` in `gpio@F100000`; the only AP-side line in the path is pin 76, the PMU's aggregate event output | no |
| AOP endpoint | 0 `cover`/`hall` strings in `ipad6faop-20H350.bin`; `AppleSPUSmartCoverDriver` = 0 instances; `ap-wake-sources` has no cover channel | no |
| SMC / `AppleSMCPMU` | `has_lid_open_sensor`, `MSLD`, `HID Lid=` strings exist, but they belong to the SMC PMU kext used on Macs; this unit's PMU is `AppleD2365PMU` | no |
| `AppleSPUSmartCoverDriver` | personality in `AppleSPU.kext` matches an `IOHIDInterface` with `Transport = SPU` and a vendor usage pair; no such interface exists on this board (0 instances) | no |

So there is exactly one AP-visible path: **PMU event → AP GPIO 76**. Everything
below is about making that line a Linux interrupt.

### 1.3 Current Linux state (a9/sensors DT, `build/kernel-sensors/dtbs/s8003-j71t.dtb`)

```dts
smart-cover {                     /* input device name: "iPad Smart Cover" */
	compatible = "gpio-keys-polled";
	poll-interval = <50>;
	lid-switch {
		gpios = <&periph_pmu_gpio 0 GPIO_ACTIVE_LOW>;
		linux,input-type = <EV_SW>;
		linux,code = <SW_LID>;
		debounce-interval = <50>;
	};
};
smart-cover-aux {                 /* "iPad Smart Cover 2" */
	compatible = "gpio-keys-polled"; poll-interval = <50>;
	cover-switch { gpios = <&periph_pmu_gpio 10 GPIO_ACTIVE_LOW>;
	               linux,input-type = <EV_SW>; linux,code = <SW_MACHINE_COVER>; };
};
```

and on the PMIC side `gpio-reserved-ranges = <1 9 11 2 14 10>` — lines 0, 10
and 13 are free. `pmic@3c` has **no** `interrupt-parent`/`interrupts`, and
`gpio-apple-d2365` is a plain `gpio_chip` with no `irq_chip`: no interrupt, no
wake, by construction. `CONFIG_KEYBOARD_GPIO_POLLED=y` is added by
`scripts/sensors/kernel.config`. **[V]**

## 2. How iOS reads it

### 2.1 Interrupt, per line, level-based

`AppleM68Buttons::handleStart` (`0xfffffff006c20988`) creates one
`IOFilterInterruptEventSource` per `button-names` entry, with interrupt index
`i` (0…5). Per-button properties are read from the provider:
`function-button_%s`, `function-wake_button_%s`, `button-delay_%s`,
`button-debounce-on_%s`, `button-debounce-off_%s`, `button-priority_%s`. **[V]**

The filter callback (`0xfffffff006c23718`) **reads the current value** from the
per-button PMU GPIO function object (a vtable call with an out-parameter, i.e.
a level read, not an event code) and passes it to the state handler; it skips
the read only for `halleffect` while `CoverDetached` is set. The state handler
(`0xfffffff006c23834`) publishes an Apple-vendor HID event and keeps the masks.
So: level-sampled on an interrupt, not edge-latched. **[V]** structure,
**[I]** that the value is the pin level.

| Button | HID event published | Notes |
|---|---|---|
| hold / menu | page 0x0C usages 0x30 / 0x40 | power / home |
| volup / voldown | page 0x0C usages 0xE9 / 0xEA | |
| **halleffect** | page 0x00FF usage 0x0A **and** page 0xFF09 usage **1** | primary magnet |
| **halleffect1** | page 0xFF09 usage **2** | second magnet |
| `hesattach` | page 0xFF09 usage **0x10**, and sets `CoverDetached = !state` | **not present on j71t** |

`hesattach` ("hall effect sensor attach") exists in the kext's symbol table but
not in the J71t `button-names`; the only runtime writer of `CoverDetached` is
that branch, so on this board the property is not a cover indicator and the
state reaches userspace only as the two 0xFF09 usages. **[V]** writers,
**[I]** consumption (no consumer string in the kernelcache).

### 2.2 Mode, polarity, debounce

- **Mode**: `_setGPIOFunction` (`0xfffffff006bbd4a8`) keeps bits 1/2/5/6/7,
  clears bit 0 (VALUE), bit 3 (INPUT) and bit 4 (FUNCTION), then sets the
  requested function: 2 → bit 3 (input), 1 → bit 0 (output high), 0 → nothing,
  anything else logs `unsupported function`. So function 2 is a *plain GPIO
  input*, and FUNCTION is the same bit the Linux driver calls
  `D2365_GPIO_FUNCTION`. Whether the hall entries get function 2 is **[U]**:
  the ADT property carries only (line, 0). **[V]** mechanism, **[U]** state.
- **Polarity**: `_setGPIOFunction` refuses to invert an input ("tried to set
  polarity on input, not supported"), so whatever the pin reads is what iOS
  uses — the ADT gives no active-low flag and the two halls are handled
  identically. Hardware-wise an open-drain hall pulling low under a magnet is
  the obvious design, which is also what the current DT assumes
  (`GPIO_ACTIVE_LOW`). **[I]**, not measured.
- **Debounce**: no `button-*` property for either hall, so all three knobs take
  the kext default **14000** (hold/voldown/volup override `button-delay` to
  `0x124f8` = 75000). The unit is almost certainly µs (14 ms / 75 ms), which
  matches the debounce behaviour users see. **[V]** values, **[I]** unit.
- **Which line means what**: both lines are independent inputs with their own
  interrupt; iOS ANDs nothing. A two-magnet cover therefore gives 4 states
  (`11` detached, `10`/`01` folded/closed variants, `00`). Nothing in the
  kext collapses them. **[V]** independence, **[U]** the mechanical mapping.

## 3. Wake: what iOS does, and what Linux would need

### 3.1 The PMU's wake machinery (from `AppleD2365PMU`)

| Item | Value | Evidence |
|---|---|---|
| EVENTS bank | **0x1c0**, 30 bytes = 240 events | **[V]** read/write with `#0x1e` at `0x6bbb488`, `0x6bbab20`, `0x6bbb5b8` |
| GPIO wake mask | **0x1c6**, 3 bytes = 24 lines (events 0x30–0x47) | **[V]** 3-byte write at `0x6bbb560`, right after the 24-byte GPIOCFG read |
| Ack semantics | iOS writes a captured snapshot back to 0x1c0 and also writes 30 zero bytes before S2R | **[V]** both sites; W1C vs write-0-to-clear is **[U]** |
| Wake trim rule | start from `ff ff ff`, then for each of 24 lines with `GPIOCFG[i] & 0xc1 == 1` (an output driven high), clear bit `i` | **[V]** loop at `0x6bbb51c` |
| Wake-reason names | `rtc, usb, hold, menu, ringerab, button4, chg_end, tbat`, then `gpio1…gpio24` | **[V]** `__cstring` 0x5916280, decode at `0x6bbae90` |
| Per-line GPIO config | `0x900 + 2*line` (<18), `0x8bd + 6*line` (≥18) | **[V]** `gpio-apple-d2365` |
| Input level | `0x186 + line/8`, bit `line%8` | **[V]** same driver |

The trim keeps the wake bit for every line that is **not an output driven
high**: `GPIOCFG & 0xc1 == 1` selects `VALUE=1, FUNCTION=0, bits 6–7 clear`,
i.e. a plain output at 1. Input lines (bit 3 set) never match, so all 24 input
lines — both halls included — stay wake-enabled. **[V]** arithmetic, **[I]**
intent (nothing documents it; the alternative reading "value bit mirrors the
pin" only holds for outputs, since the driver reads inputs from 0x186).

`function-wake_button_hold` / `_menu` (PMU `STAT` 0x1a9/0x1a8) exist for the
two keys and have **no hall counterpart**. So the halls are not "synthetic
buttons" for the PMU; they are only in the GPIO wake mask. **[V]**

### 3.2 Linux: why it cannot wake today, and the shape of the fix

Wake in our s2idle is just "an enabled IRQ fires while the CPU idles": Home and
Power already work that way through `pinctrl_ap` 96/97 with
`IRQCHIP_SKIP_SET_WAKE`, and the 2026-10-22 sleeps woke from IRQ 66
([ds-suspend-wake-plan.md](ds-suspend-wake-plan.md) §1.2). Three things are
missing for the cover, in order:

1. **The PMU node's parent IRQ** — already written in
   `patches/power/s2idle-round1/0001-0002`
   (`interrupt-parent = <&pinctrl_ap>; interrupts = <76 IRQ_TYPE_LEVEL_LOW>;
   wakeup-source; interrupt-controller; #interrupt-cells = <1>;`).
2. **An irqchip/IRQ handler that reads and acks EVENTS at 0x1c0** — nothing in
   the tree implements it. Without the ack the level-low line 76 stays
   asserted and the IRQ storms.
3. **A switch device that consumes an event** — `gpio-keys` with
   `interrupts-extended`, or a 100-line input driver.

Minimal wake-capable DT (builds on the two series above; **not** applied):

```dts
&i2c0 {
	pmic: pmic@3c {
		/* from s2idle-round1 0002 */
		interrupt-parent = <&pinctrl_ap>;
		interrupts = <76 IRQ_TYPE_LEVEL_LOW>;
		wakeup-source;
		interrupt-controller;
		#interrupt-cells = <1>;
	};
};

smart-cover {
	compatible = "gpio-keys";          /* IRQ variant, not -polled */
	lid-switch {
		label = "Lid Switch";
		gpios = <&periph_pmu_gpio 0 GPIO_ACTIVE_LOW>;
		interrupts-extended = <&pmic 0x30>;   /* PMU event 48 = line 0 */
		linux,input-type = <EV_SW>;
		linux,code = <SW_LID>;
		debounce-interval = <14>;             /* iOS default, 14000 µs */
		wakeup-source;                        /* per-button, gpio-keys binding */
	};
};
```

Driver obligations that the DTS alone does not provide:

- a `regmap_irq`/`irq_domain` on `pmic@3c` with 240 one-cell events, a
  threaded parent handler (`IRQF_ONESHOT` — no I²C in hardirq), reading 0x1c0,
  masking with 0x1c6 for events 0x30–0x47, and **acking by writing the handled
  bits back to 0x1c0**;
- `irq_chip_set_wake_parent()` in `.irq_set_wake` so the child's
  `enable_irq_wake()` also arms IRQ 76 — otherwise `suspend_device_irqs()`
  disables the parent and no wake happens;
- `device_init_wakeup()` for the PMU (`wakeup-source`) and for the input
  device; verify `/sys/devices/.../power/wakeup` is `enabled` after probe;
- do not write 0x1c6 or 0x900 from Linux except to *set* bits 0/10 if a
  read-only test shows them clear. The wake trim is iOS policy, not ours.

**Timing hazard [I]:** the threaded handler needs I²C, and threaded IRQs can
run before `dpm_resume` has resumed `i2c@20a110000`. Keep the hard handler to
"record + `IRQ_WAKE_THREAD`", take `pm_runtime_get_sync()` in the thread, and
if the transfer fails, leave the state stale: the PMU event is latched, the
line stays low, and the level IRQ re-fires after resume, so a late `SW_LID`
update is harmless (a switch is a state, not an event).

**Cheaper interim** (no irqchip): a single driver on IRQ 76 that reads EVENTS,
and if bit 0/10 changed, reads 0x186 and calls `input_report_switch()`. It is
40 lines instead of a domain and gives the same `SW_LID` + wake; the domain is
only needed if the charger/RTC events are wanted too.

### 3.3 What logind and Omarchy do with `SW_LID`

- The kernel side that matters is: `EV_SW`/`SW_LID`, one `input` device, and
  `wakeup-source` **on the button child** (that is where the gpio-keys binding
  puts it, and `gpio_keys_suspend()` is what turns it into
  `enable_irq_wake()`; a `wakeup-source` on the parent node only sets the
  device's `power/wakeup`). A **polled** device cancels its delayed work in
  `->suspend` and has no IRQ to arm, so it can never wake; that is the whole
  difference between the two DTs above. **[V]** no IRQ, **[I]** driver detail.
- systemd-logind picks up input devices with switch capability (udev tags them
  `power-switch` from `ID_INPUT_SWITCH=1`) and acts on the `SW_LID` bit; the
  device *name* is not what selects it. Its defaults are
  `HandleLidSwitch=suspend`, `HandleLidSwitchExternalPower=suspend`,
  `LidSwitchIgnoreInhibited=yes`. **[I]** (behaviour of systemd, not verified
  from a local source tree) — the project's current drop-in
  `/etc/systemd/logind.conf.d/90-linux-a9-lid.conf` sets all three to
  `ignore`, which is correct until wake works.
- logind never wakes anything: close → `Suspend()`, open → nothing. Wake is
  the kernel/PMU's job; after wake logind emits `PrepareForSleep(false)`.
  So "close suspends, open wakes" is two independent mechanisms and both are
  needed. **[I]**
- The second hall is reported as `SW_MACHINE_COVER` so a two-magnet cover can
  be told apart; `ipad-lid` (`~/.config/linux-a9/lid.conf`, `cover2 = ignore`)
  is the only consumer today, and it deliberately does not use the name
  "Lid Switch".
- Hyprland/Omarchy do not touch the switch themselves: the session locks and
  blanks on logind's sleep signal (or `ipad-lid`), and `omarchy-hw-laptop`
  only tests for the *presence* of `SW_LID` to offer laptop menu entries.
  **[I]**

**Policy once the IRQ switch exists:** set `HandleLidSwitch=suspend` and
`HandleLidSwitchExternalPower=suspend`, keep `LidSwitchIgnoreInhibited=no` if
a lock should be able to veto, and let `hypridle`'s `before_sleep_cmd` (or a
`system-sleep` hook) lock the session. Then `ipad-lid` can drop its own
suspend path and keep only the display/brightness handling. **[I]**

**Do not enable lid-suspend on the polled device unless waking with Home is
acceptable.** It is not a brick (Home/Power still wake), but it is not the
requested behaviour.

## 4. Device test — read-only first

Run in order; stop at the first failure. Nothing here writes a PMU register.

### A. Polarity and "is the line readable as a GPIO" (no rebuild)

The polled switch already reads 0x186 over I²C every 50 ms, so this answers
H1 and H2 of `ds-als-hall.md` §5 in one pass:

```sh
ipad-lid --list                    # the two switch devices and their state now
ipad-lid --watch 60                # then open/close the cover slowly, 5 times
cat /sys/class/input/input*/name | grep -i cover
sudo evtest /dev/input/eventX      # expect EV_SW SW_LID 0/1, nothing else
```

- `SW_LID` must be **1 with the cover closed**. If it is inverted, fix
  `GPIO_ACTIVE_LOW` (`scripts/sensors/cover-polarity.sh build/…/s8003-j71t.dtb
  active-high lid`) and, if that is right, the dtsi.
- No event at all, plus a `… belongs to a PMIC function (config 0x…)` warning in
  `dmesg`: the line is in FUNCTION mode, the polled path is dead, skip to C.
- Time 20 open/close cycles from the `--watch` timestamps to size
  `debounce-interval` (iOS default is 14 ms).

### B. Does the PMU event logic run under Linux? (needs `CONFIG_I2C_CHARDEV=y`)

`CONFIG_I2C_CHARDEV` is **not set** in `configs/hoolock-16k.config`; add it to
`scripts/sensors/kernel.config` for this test only. `i2ctransfer` uses
`I2C_RDWR`, which is not blocked by the pmic driver's claim on 0x3c.

```sh
BUS=$(i2cdetect -l | awk '/20a110000/{sub("i2c-","",$1); print $1}')
# per-line config and level, cover open and closed
i2ctransfer -y $BUS w2@0x3c 0x09 0x00 r1     # line 0  config   (0x900)
i2ctransfer -y $BUS w2@0x3c 0x09 0x14 r1     # line 10 config   (0x914)
i2ctransfer -y $BUS w2@0x3c 0x01 0x86 r1     # lines 0..7  level
i2ctransfer -y $BUS w2@0x3c 0x01 0x87 r1     # lines 8..15 level
i2ctransfer -y $BUS w2@0x3c 0x01 0xc6 r3     # GPIO wake mask, 24 lines
i2ctransfer -y $BUS w2@0x3c 0x01 0xc0 r30    # EVENTS, 240 bits
```

Read 0x1c0 immediately after each cover move, then again 2 s later:

| Observation | Meaning |
|---|---|
| 0x900/0x914 `& 0x18 == 0x08` | plain input, polled path valid (**H2 = A**) |
| `… & 0x10` set | FUNCTION mode → event path only |
| 0x186/0x187 bit tracks the magnet | polarity measured (**H1** closed) |
| a new bit in 0x1c0 at 0x30 / 0x3a after a move | the PMU latches hall events under Linux → the irqchip will work and wake is plausible |
| 0x1c0 always 0 | the PMU event logic is not armed by iBoot/pongoOS; it must be programmed (0x900 FUNCTION/edge bits) before any wake work |
| mask 0x1c6 bits 0/10 set | the PMU is allowed to wake on these lines |
| mask bits clear | write-enable them **only after** the rest works (this is the one write in the plan) |

Script this loop as `scripts/sensors/cover-regs.sh` when it is worth keeping;
it is deliberately not added here.

### C. Interrupt path (after the irqchip/child lands)

```sh
grep -iE "pmic|76|gpio-keys" /proc/interrupts   # count must rise on each move
sudo evtest /dev/input/eventX                   # SW_LID flip per move
cat /sys/class/input/inputX/device/power/wakeup # expect "enabled"
```

### D. Suspend / wake

```sh
# cover CLOSED, then:
systemctl suspend
# open the cover; the device must come back by itself
journalctl -b -k | grep -iE "PM: Triggering wakeup|PM: suspend exit|irq"
cat /sys/power/suspend_stats/{success,fail,last_failed_dev}
dmesg | grep -iE "pmic|cover|lid"
```

Success = `PM: Triggering wakeup from IRQ <pmu>` and a resumed session without
touching Home. Also test the two negative cases: suspend with the cover
**open** (must not wake on its own) and a close/open while the SoC is awake
(must produce two `SW_LID` events and no IRQ storm — an unacked level line
shows up as an interrupt flood in `/proc/interrupts`).

## 5. Gates and risks

| Gate | Why it can block | Where |
|---|---|---|
| Line 0/10 in FUNCTION mode | polled path invalid; needs the event path | test B |
| PMU event logic not armed by the boot chain | EVENTS stays 0, wake impossible without programming 0x900 | test B |
| 0x1c0 ack semantics unknown (W1C vs write-0) | wrong ack = IRQ storm or lost events | test B, then driver |
| I²C in the wake window | threaded handler may run before `i2c@20a110000` resumes | §3.2 |
| Wake propagation to IRQ 76 | child `enable_irq_wake` must reach the parent, or `suspend_device_irqs()` masks it | §3.2 |
| `pinctrl_ap` has `IRQCHIP_SKIP_SET_WAKE` | fine for s2idle (GPIO block is always-on), but it means no real arm — same as Home/Power today | ds-suspend-wake-plan §1.2 |
| Writing 0x1c6/0x900 blind | changes what the PMU can wake on, and possibly a rail's behaviour | do not |
| logind flip before wake works | close suspends, open does not wake | §3.3 |

## 6. Open questions

| ID | Question | Cheapest close |
|---|---|---|
| S1 | Magnet polarity of lines 0 and 10 | test A (`ipad-lid --watch`) |
| S2 | Are the lines plain inputs under iOS/Linux? | test B, 0x900/0x914 |
| S3 | Do hall events appear in 0x1c0 under Linux? | test B |
| S4 | Are bits 0/10 set in the 0x1c6 wake mask? | test B |
| S5 | 0x1c0 ack: write-back or write-1-to-clear? | two reads + one write in test C |
| S6 | Does opening the cover wake the AP through IRQ 76? | test D |
| S7 | Does iOS itself wake from S2R on the cover? (confirms the PMU path end to end) | iOS `log`/serial: `WAKEINT`/`WAKEREA` dump should show `gpio1` |
| S8 | Does the second hall add value (folded-behind state)? | test A with `cover2 = closed` |

## 7. References

- `build/firmware/private/IODeviceTree.txt` 1753–1782 (pmu@3C), 3205–3235
  (`/buttons`); static ADT `docs/adt/j71t.txt` (same two nodes).
- Kernelcache `20H350` (iPad6,11): `AppleM68Buttons` `handleStart`
  `0x6c20988`, filter `0x6c23718`, state `0x6c23834`, dispatch `0x6c23eb4`,
  serializer `0x6c21e74`, class init `0x6c2591c`; `AppleD2365PMU`
  `_setGPIOFunction` `0x6bbd4a8`, EVENTS/wake-mask/trim `0x6bbb380`–`0x6bbb5f0`,
  wake-name decode `0x6bbae90`, event ack `0x6bbab10`.
- Tree: `patches/periph/0011` (PMU GPIO driver), `patches/periph/0035`
  (J71t peripherals), `patches/power/s2idle-round1/0001-0002` (PMU event node),
  `scripts/sensors/{kernel.config,cover-polarity.sh}`, `scripts/omarchy/rootfs/
  files/usr/local/bin/ipad-lid`, `…/etc/systemd/logind.conf.d/90-linux-a9-lid.conf`.
- Docs: [ds-als-hall.md](../periph/ds-als-hall.md) §3–§5,
  [ds-suspend-wake-plan.md](ds-suspend-wake-plan.md) §1.3/§1.7/§1.8,
  [ds-suspend-s2idle.md](ds-suspend-s2idle.md) §2.3,
  [sensors-test-plan.md](../periph/sensors-test-plan.md) §3,
  [tablet-usage.md](../omarchy/tablet-usage.md) (cover section),
  [ds-orientation-paths.md](../periph/ds-orientation-paths.md) §429–430,
  [aop-sensors.md](../periph/aop-sensors.md) "Ambient light and hall are
  separate on this board".
