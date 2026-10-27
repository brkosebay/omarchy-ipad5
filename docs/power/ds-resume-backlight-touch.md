# The two riskiest wake gaps: DWI backlight and N1 touch

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Offline only: no device access, no kernel edits, no commits.
iPad 5 (iPad6,11), S8003 / j71t, iOS 16.7.10 (20H350) as reference, Linux/Omarchy
as target.

Read with [ds-suspend-wake-plan.md](ds-suspend-wake-plan.md) (the six missing PM
callbacks, §3), [../touch/runtime-notes.md](../touch/runtime-notes.md) (N1 boot
and runtime protocol), [../touch/n1-protocol.md](../touch/n1-protocol.md),
[../touch/transport-notes.md](../touch/transport-notes.md) and
[../display/ds-display-kms.md](../display/ds-display-kms.md).

Marks: **[V]** read out of a binary, the live ADT, the DTB or the kernel tree;
**[I]** inference; **[U]** unresolved.

---

## 0. Bottom line

1. **The DWI kext has no power management at all — not a single callback, not a
   register dump.** `AppleS8000DWI` is 1 999 bytes of `__text` containing
   `start`, `callPlatformFunction`, `sendWLED`, `_waitTransaction`, the
   buck-table accessor and two function objects. There is no `setPowerState`,
   no sleep/wake hook and no save/restore anywhere in it. **[V]**
   Consequence: iOS's *only* recovery mechanism for a DWI that lost power is
   **re-issuing the brightness command**; the block-level configuration is
   written exactly once, at boot, by `start()`. **[V]**
2. **The Linux backlight already re-sends brightness on resume.**
   `apple_dwi_bl.c:67` sets `BL_CORE_SUSPENDRESUME`, and
   `backlight_resume()` (`drivers/video/backlight/backlight.c:291`) calls
   `update_status()` on the way up, after `backlight_suspend()` wrote
   brightness 0 on the way down. The plan document's row "`BL_CORE_SUSPENDRESUME`
   not set → set it and call `dwi_bl_update_status()`" is **wrong for this
   tree**: that fix is already in. **[V]**
3. **What the backlight is actually missing is the block-level init and the
   window to reach it.** iOS writes five words before its first transaction —
   `0x00`, `0x04`, `0xd0` from three DT properties, then a data/control pair
   `0x84=0`, `0x80=0x41` (`AppleS8000DWI::start`, `fffffff00634f50c`–
   `fffffff00634f530`). Our `dwi_bl` node maps **only `0x20e200080..087`**
   (`s800-0-3.dtsi:229`), so those three globals are outside the mapping and no
   Linux code has ever written them, on any path. If `ps_dwi` (PMGR `0x80110`)
   drops at S2R, the brightness write lands in a block whose mode/lockout/STR
   programming is gone. **[V]** writes, **[I]** consequence.
4. **The N1 is never powered down across iOS sleep.** The touch controller's
   rails are a D2365 LDO (`function-power_ana` → PMU `0x21f`), GPIO 73 and the
   PMGR TCLK gate — none of them is a PMGR power domain. The driver never
   observes system sleep at all: `0xE0000280` occurs zero times in its
   `__text`, and its only power-down request is the **shutdown** handler
   (`fffffff005ea0cd0`, compares `0xE0000310`/`0xE0000250`, logs
   `"%s: powering off"`). **[V]**
5. **Therefore iOS does not reload N1 firmware or re-send calibration on
   wake.** Boot-load is gated on "does the device already have firmware?"
   (`"Device has no firmware - will attempt to bootload"`,
   `"ignoring.. device has no firmware"`), and its caller is the driver's
   boot/start path, not any wake path.
   The post-wake mechanism is the 16-byte runtime wake packet
   `ee 00…00 ee 00` (`fffffff005ea75e8`, 2 ms, 100 ms repeat suppression),
   which is a *keep-alive*, not a re-init. **[V]** for the code, **[I]** that
   this is the whole wake path.
6. **`MtCl` is only ever read.** iOS loads it from syscfg once, at boot, into
   N1 SRAM at `cal-dl-addr = 0x10009000`. Nothing in the wake path touches it.
   Our kernel already only *reads* `apple/j71t-n1-calibration.bin`. Keep it
   that way; never write the record back. **[V]**
7. **The only block state we lose on our side is the SPI controller's register
   file — and the driver already rebuilds it.** `spi-apple-s8000.c` re-programs
   CTRL/CLKDIV/WORD_DELAY/STATUS/CFG/RXCNT/TXCNT on **every** transfer
   (lines 104–112), so there is nothing to save and nothing to restore. The
   work is *ordering* (never touch the block while `ps_spi3` is gated) and the
   N1's IRQ, not a register dump. **[V]**
8. **Three concrete defects follow, in cost order** (§4): the touch IRQ is
   disabled **permanently** after 8 bad frames (`apple_n1.c:724`) with no
   resume path to re-arm it; nothing stops the N1's threaded IRQ from running
   against a gated SPI block during noirq; and the backlight cannot re-init a
   block it cannot address. Fixes are ~60 lines total.

---

## 1. Method and what this round could not use

The shared Ghidra MCP server accepted `read_memory` and `list_strings` but
`decompile_function_by_address` and even `get_function_by_address` stopped
returning (backend timeout, JVM pinned at 100 %). Rather than restart a server
other agents share, this round used the already-extracted kexts plus capstone:

| Target | Artefact | Base VA |
|---|---|---|
| `AppleS8000DWI` | `build/power/oc-r2/kexts/AppleS8000DWI.*` | `__text` `fffffff00634f000` |
| `AppleARMPlatform` (owns `AppleARMBacklight`) | `build/video/kexts/AppleARMPlatform.*` | `__text` `fffffff0059b3000` |
| `AppleMultitouchSPI` (owns `AppleMultitouchN1SPI`) | `build/periph/kexts/AppleMultitouchSPI.*` | `__text` `fffffff005e94000` |

Raw `__text` disassembly with `capstone` 5.0.7 and `__DATA_CONST` vtable
walks; the scripts are in Appendix B. Every address below is a kernelcache VA;
the kexts' section tables (`build/power/oc-r2/kexts/*.json`,
`build/*/kexts/manifest.json`) give the VA↔offset maps.

Two earlier-round results are reused and not re-derived: the N1 boot power
sequence and wire protocol ([../touch/runtime-notes.md](../touch/runtime-notes.md)),
and the N1 vtable at `fffffff006e12fe8`.

---

## 2. DWI / AppleARMBacklight: exactly what iOS saves and restores

### 2.1 Inventory: there is no PM in the DWI kext

`AppleS8000DWI.macho`, `_PrelinkExecutableSize` 1 999 bytes. Complete list of
functions:

| Address | What |
|---|---|
| `fffffff00634f1f0` | `start()` — maps the block, reads DT, programs the globals |
| `fffffff00634fac8` | `_waitTransaction(reg)` — poll bit0, panic on timeout |
| `fffffff00634fb74` | `sendWLED(level, blocking, version)` |
| `fffffff00634f8ec` | `AppleS8000DWI_setBuckTableIndex()` — PMIC voltage path |
| `fffffff006350034` | `AppleS8000DWIBacklightFunction::setLevel()` |
| `fffffff0063500c0`/`…00c8` | `AppleS8000DWIVoltageFunction` entry points |
| `fffffff00634f674` | `callPlatformFunction()` override (matches `"dwiB"` = `0x64776942`) |
| `fffffff00634f000`/`…f0d0` | metaclass constructors and destructors |

No `setPowerState`, no `willSleep`, no `didWake`, no register cache, no
`RESET`/`PWRGATE` handling. The class vtable
(`__DATA_CONST` `fffffff006ebf830`) overrides only `start` (`+0x2d0`),
`callPlatformFunction` (`+0x3c8`) and a handful of constructor/destructor
thunks. **[V]**

So "what iOS saves around sleep for the DWI" is: **nothing**. Everything needed
to make a brightness value take effect must be reconstructed from the object's
own copy of the level plus the five boot-time words.

### 2.2 Register map (block base `0x20e200000`, 16 KiB per ADT)

| Offset | Dir | Meaning | J71t value | Evidence (write site) |
|---|---|---|---|---|
| `0x00` | W | global mode: `((nclk_div-1) << 16) \| polarity` | `0x00000001` | `fffffff00634f514` |
| `0x04` | W | lockout, in units of 24 (`lockout_us * 24`) | `0x00000078` | `fffffff00634f524` |
| `0xd0` | W | STR delay `(str_delay * 24) / nclk_div` | `0x0002ee00` | `fffffff00634f51c` |
| `0x80` | RW | WLED channel: control/command, bit0 = busy | `0x41` then `0x71` | `fffffff00634f530`, `fffffff00634fc18` |
| `0x84` | W | WLED channel: data | `0x0` then `0xa0000000 \| level` | `fffffff00634f528`, `fffffff00634fc08` |
| `0xa0` | RW | buck channel: control/command, bit0 = busy | `0x71` | `fffffff00634f9c8` |
| `0xa4` | W | buck channel: data | buck word | `fffffff00634f9bc` |
| `0xb0` | W | buck channel: timing | `str*24/nclk` | `fffffff00634f9c0` |
| `0xc0` | W | WLED channel, non-blocking variant: control | `0x71` | `fffffff00634fbbc` |
| `0xc4` | W | WLED channel, non-blocking variant: data | `0xa0000000 \| level` | `fffffff00634fbac` |

The control word is `(opcode << 4) | 1`: **opcode 4** = the boot-time init
transaction (`0x41`), **opcode 7** = "send a DWI write" (`0x71`, used for both
WLED and buck). Bit 12 is `slave-select` (`(slave_select & 1) << 12`,
`fffffff00634f470`); J71t's ADT has **no** `slave-select`, so iOS's mode word is
0 and it writes `0x71`, not `0x1071`. **[V]** values, **[I]** the bit-field
reading.

Channel stride is `0x20`: `0x80`, `0xa0`(+`0xb0` timing), `0xc0`. The
`0xc0`/`0xc4` pair is written *without* the busy wait when the caller passes
`blocking == 0` — that is the path a caller in interrupt context would use.
**[V]** code, **[I]** "deferred/pending slot".

One ambiguity worth stating, because it changes what `0xd0` "is": if the
channel stride really is `0x20` all the way down, then `0x00`/`0x04` are
channel 0's command/data and `0xd0` is channel `0xc0`'s timing register, not a
global. The *values* argue the other way — `nclk<<16 | polarity` and
`lockout_us*24` are shaped like configuration, not like commands — so the table
above calls them globals **[I]**. Either reading gives the same code: replay
exactly the five words `start()` writes, to the same addresses. **[V]** values,
**[U]** semantics.

### 2.3 The brightness protocol

`AppleS8000DWI::sendWLED(level, blocking, version)`, `fffffff00634fb74`:

```c
/* version == 1 (J71t: "dwi-version" = 1); 0xfffffff00634fbd4 */
data = 0xa0000000 | (level & 0x7ff);         /* 11-bit DAC, opcode 0xa in [31:28] */
/* version == 0 (S5L8940); 0xfffffff00634fb98 */
/* data = ((level & 3) << 9) | (level >> 2) | 0x20000000; */

if (!blocking) {                             /* 0xfffffff00634fbac */
	writel(data, base + 0xc4);
	writel(mode | 0x71, base + 0xc0);
	return;
}
spin_lock(&lock);                            /* IOSimpleLockLock, irqs off */
_waitTransaction(0x80);                      /* 0xfffffff00634fbfc, poll bit0 clear */
writel(data,        base + 0x84);            /* 0xfffffff00634fc08 */
writel(mode | 0x71, base + 0x80);            /* 0xfffffff00634fc18 */
spin_unlock(&lock);
```

`_waitTransaction` (`fffffff00634fac8`) polls `base[reg] & 1` until clear, with
a deadline derived from `clock_interval_to_absolutetime_interval(1000000, 0xc8)`
(≈1 ms) and **panics** on expiry (`"AppleS5L8940DWI::_waitTransaction timeout
for register %x"`). The wait happens *before* the write, not after.

These are the same two writes mainline performs, in the same order, modulo the
missing busy wait and the extra bit 12:

```c
cmd  = FIELD_PREP(DWI_BL_CMD_TYPE, 0xa) | FIELD_PREP(DWI_BL_CMD_DATA, brightness); /* 0xa0000000 | b */
writel(cmd,    base + DWI_BL_CMD);   /* base = 0x20e200080 → 0x84 */
writel(0x1071, base + DWI_BL_CTL);   /* → 0x80 */
```

### 2.4 What `start()` programs, and why it matters

`fffffff00634f4d0`–`fffffff00634f530`, in order, before anything else:

```c
provider_call(1, 0);            /* 0x88 -> vtable+0x568 */     /* [U] power/clock enable */
provider_call(1, 0, 0);         /* 0x88 -> vtable+0x570 */
writel(mode,                 base + 0x00);   /* 0x00000001 on J71t */
writel(str_delay,            base + 0xd0);   /* 0x0002ee00 */
writel(lockout,              base + 0x04);   /* 0x00000078 */
writel(0,                    base + 0x84);
writel(0x41,                 base + 0x80);   /* opcode 4: init transaction */
```

The three globals derive from four DT properties — `nclk-div` (1),
`polarity-config` (1), `str-delay` (8000 ns) and `lockout-us` (5) in the live
ADT. The two provider calls immediately before the first MMIO are the only hint
that the block needs its clock/power enabled first; the exact vtable slots are
**[U]** (they are shared by many consumers in `AppleARMPlatform`).

**Nothing re-runs this.** Not the backlight driver, not PM, not wake. It is a
boot-time, once-only configuration.

### 2.5 Enable/disable on the AppleARMBacklight side

`AppleARMBacklight` lives in `AppleARMPlatform.kext` (not a separate kext). Its
level path is:

| Address | Function |
|---|---|
| `fffffff0059e1314` | `setBacklightEnableGated(enable, level)` — caches at `this+0x168`, logs `"Set backlight %s"`, then pushes a level |
| `fffffff0059e19dc` | `_setBacklightLevelGated(level, type)` — policy/type selection before the write |
| `fffffff0059e1bf8` | `updateBacklightLevel()` — `"Set Backlight enabled=%ld _lastNits=%lld/1000, iDAC=0x%x"` |
| `fffffff0059e2010` | the writer; calls the provider through vtable+`0x80` (i.e. `callPlatformFunction` → `"dwiB"`) |
| `fffffff0059da92c` | `start()`; resolves `function-backlight_enable` / `function-backlight_update` |

Two consequences:

* **Disable is expressed as a level, not a register bit.** There is no "DWI
  off" bit, no `PWRGATE`, no reset path in the backlight driver: it caches an
  enable flag at `this+0x168`, logs on/off, and pushes a level. The ALS variant
  makes it explicit — `"setBacklightEnableGated %d (set level to 0x%x)"`. That
  level is 0 on disable **[I]**. The Linux backlight core does the same thing
  (`BL_CORE_SUSPENDED` → `backlight_get_brightness()` returns 0).
* **The level lives in the driver object and in NVRAM**, not in the block:
  `"nvram backlight level = 0x%lx"`, `"updated NVRAM with level 0x%x"`
  (`fffffff0052adf02`, `fffffff0052aea7b`). That is how iOS survives a full
  reboot, and it is why there is nothing to "save" at sleep.

### 2.6 Answer to the question as asked

> What must be re-sent after the DWI block loses power?

| Item | Re-sent by iOS? | Re-sent by us today? |
|---|---|---|
| `0x00` global mode | only by `start()`, i.e. never after boot | **no** — outside the mapped window |
| `0x04` lockout | idem | **no** |
| `0xd0` STR delay | idem | **no** |
| `0x84=0`, `0x80=0x41` init transaction | idem | **no** |
| last brightness (`0xa0000000 \| level` → `0x84`, `0x71` → `0x80`) | yes, on every enable/level change | **yes**, `backlight_resume()` |

So the risk after wake is *not* "brightness is not re-sent" — it is "brightness
is re-sent into a block that no longer knows its mode". A panel that stays dark
with a live system and a non-zero `/sys/class/backlight/*/brightness` is this
case exactly.

### 2.7 ADT facts used above

Live ADT `build/firmware/private/IODeviceTree.txt`:

```
dwi@E200000  dwi-version = 1, compatible = "dwi,s8000", reg 0x20e200000+0x4000,
             clock-gates = 0x33 (51), AAPL,phandle = 0x21, interrupts = <5>
             nclk-div = 1, polarity-config = 1, str-delay = 0x1f40 (8000),
             lockout-us = 5, no slave-select
backlight    function-backlight_update = <0x21 "dwiB" 1>, function-backlight_enable = <0x50 "bklE">,
             backlight-id = 0x3ee, use-AAB-architecture = 1
```

There is **no `always-on` property anywhere in the live ADT** (0 occurrences) —
`apple,always-on` in the Linux dtsi is a hand-written list, not an iBoot fact.
The `ps_dwi` domain at PMGR `0x80110` (`s800-0-3-pmgr.dtsi:71`) is *not* in it.

---

## 3. N1 multitouch: exactly what iOS re-initialises on wake

### 3.1 The personalities and their ship-time constants

`AppleMultitouchSPI.kext` carries `AppleMultitouchSPIC1F15` → `IOClass
AppleMultitouchN1SPI`, provider `AppleARMSPIDevice`, match `multi-touch,j72`,
`mt-merge-personality = C1F15,1`, and the N1's internal address map:

| Property | Value | Meaning |
|---|---|---|
| `fw-execute-addr` | `0x10003400` | firmware entry in N1 SRAM |
| `cal-dl-addr` | `0x10009000` | MtCl download address |
| `prox-cal-addr` | `0x10009600` | proximity calibration |
| `fll-mval` / `fll-mval-addr` | `0x17c9` / `0x10003060` | FLL multiplier |
| `clk32-clock-enable-addr` / `-val` | `0x10003518` / 0 | 32 kHz enable |
| `reset-deassert-delay` | `0xf` (15) | ms after reset release |

These are consumed once, by the boot-load path. None appears in a wake path.

### 3.2 The power model: the N1 is not in a PMGR domain

ADT `multi-touch@0` under `spi3` (`0x20a08c000`):

```
function-power_ana    = <0x4f "pmuL" 0x21f>      /* D2365 LDO, always-on part */
function-power_ldo    = <0x1e "GPIO" 73 0x01010000>
function-clock_enable = <0x18 "TCLK" 8 0x64 0x800000>   /* PMGR, phandle 0x18 */
function-reset        = <0x1e "GPIO" 75 0x01000100>      /* active low */
function-display_sync= <0x1e "GPIO" 74 0x01010003>
function-enable_cs    = <0x1e "GPIO" 102 0x01010000>
interrupts            = <65 0>                            /* ATN, parent = AP GPIO */
clock-gates           = 0x4d (77)                          /* on the SPI node, not the N1 */
```

None of those rails is a PMGR power domain: the analog rail is a PMU LDO, the
digital rail is a GPIO-switched LDO, and the clock is a PMGR gate that is not
modelled as a domain. So there is nothing for the platform's `S2RT` sleep
sequence to drop even if it wanted to. **[V]** wiring, **[I]** the S2RT
conclusion.

### 3.3 Sleep is not in this driver's vocabulary

Three independent checks, all **[V]**:

1. **`0xE0000280` (`kIOMessageSystemWillSleep`) occurs zero times in the whole
   `__text`.** The only system-power constants compared anywhere in the kext are
   `0xE0000310` and `0xE0000250`, both in one handler at `fffffff005ea0cd0`:
   it checks a flag at `+0xa5f`, logs `"%s: powering off"`, and calls
   vtable+`0x718` with `(0, 1)`. That slot is a forwarder
   (`fffffff005e98804`) which re-issues the call on the *provider*
   (`this+0x98`) as a platform function [I] "power down".
2. **The device-power message is only forwarded.** The base class's `message()`
   override at `fffffff005e989c8` takes `0xE0000210`
   (`kIOMessageDeviceWillPowerOff`) and passes it to super through
   vtable+`0x7d8`. That is a provider being told to power off, not a driver
   handling sleep.
3. **The enable function has no call sites at all.** `fffffff005e985dc` (ana
   first, then LDO; reverse on the way down) contains no `bl` target anywhere in
   the kext — it is installed as a function pointer in the class table at
   `fffffff006e114f8` and reached through it from the boot/probe paths.

Symbol names for the constants are **[I]** (`iokit_common_msg(0x310)` =
`SystemWillRestart`, `(0x250)` = `DevicePowerAck`, `(0x210)` =
`DeviceWillPowerOff`, `(0x280)` = `SystemWillSleep`); the values and the
absence of `0x280` are **[V]**.

So across S2R the N1's power is decided entirely by the platform — the PMU LDO,
GPIO 73 and the TCLK gate — and nothing in the sleep path touches any of them.
`PowerControlSupported`, `PowerOnly` and `SurrenderPowerControl` exist as
flags/strings but gate *when* `setPowerEnabled` may run, not whether sleep runs
it.

### 3.4 What is re-sent on wake: the runtime wake packet, nothing else

| Packet | Address | When |
|---|---|---|
| `19 c1` (REQ_WAKEUP), 2 bytes, +10 ms | `fffffff005ea7548` | boot sequence only, step 4 of the off→on path (`fffffff005ea1058` loads vtable+`0x910`) |
| `ee 00…00 ee 00`, 16 bytes, +2 ms, 100 ms repeat suppression | `fffffff005ea75e8` | runtime, at the head of every **control/descriptor** request: E2 `fffffff005ea28e4`, E3 `fffffff005ea2bf0`, GET `fffffff005ea3018`, E6/E7 `fffffff005ea3954` (all load vtable+`0x918`). Never on the frame-read path. |

The runtime packet is a keep-alive for the N1's own low-power state. It is
**not** followed by a descriptor re-read, a firmware reload or a calibration
resend. The "runtime init" the N1 driver runs after boot is
`fffffff005ea2854`, and it is five instructions: read the repeat-read flag
`+0x3d8`, store the resulting `EA`/`EB` selector at `+0xa59`, store
`0x0101` (sequence 1) at `+0xa5a`, then tail-call E2 device info. No wake
packet, no firmware, no calibration, no register restore. **[V]**

### 3.5 Firmware and calibration: loaded once, never on wake

Boot-load order (from [../touch/runtime-notes.md](../touch/runtime-notes.md),
re-checked here): N1 register calibration → MtCl to `cal-dl-addr`
(`fffffff005ea52d0`) → firmware blob (`fffffff005ea4ffc`) → execute at
`0x10003400` → 40 ms → discovery (E2/E3).

Two strings decide the wake question:

* `"Device has no firmware - will attempt to bootload"` (`fffffff00547f33c`)
* `"ignoring.. device has no firmware"`

i.e. boot-load is conditional on a *read-back* of device state, and its caller
is the driver's boot/start path, not any wake path. If the N1 still answers,
nothing is reloaded. **[V]**

**Calibration is read-only, in both worlds.** iOS reads the `MtCl` blob out of
syscfg at boot; Linux reads `build/firmware/lib/firmware/apple/j71t-n1-calibration.bin`
(or the DT bytes). Nothing writes it back, and nothing in this document proposes
to. Writing syscfg is forbidden by [AGENTS.md](../../AGENTS.md).

### 3.6 Answer to the question as asked

> What is re-initialised on wake for the N1 over SPI?

| Item | iOS wake behaviour |
|---|---|
| N1 firmware reload | **no** — conditional on a missing-firmware read-back, boot only |
| calibration (`MtCl`) re-send | **no** |
| N1 register calibration | **no** |
| reset GPIO pulse | **no** — reset is deasserted once at boot and stays deasserted |
| LDO / analog rail | **no** — never dropped |
| PMGR clock gate | **no** — TCLK stays enabled |
| SPI controller register restore | **none needed in our driver**; iOS's `AppleSamsungSPIController` PM was not examined (**[U]**) |
| runtime wake packet `ee…` | **yes**, per command path, 100 ms throttled |
| EB/EA sequence counter | reset to 1 by the runtime init, not by wake |

---

## 4. Mapping onto our drivers

### 4.1 `apple_dwi_bl.c` + DTS — backlight

Current state: `base` = `0x20e200080`, `reg` window 8 bytes; `update_status`
writes `0x84` then `0x80`; `BL_CORE_SUSPENDRESUME` set; no busy wait; no block
init; `power-domains = <&ps_dwi>`; `ps_dwi` not always-on.

Change list, cheapest first:

**B1 (DTS, 1 line, no code) — MVP.** Mark the DWI domain always-on:

```diff
--- a/arch/arm64/boot/dts/apple/s800-0-3-pmgr.dtsi
+++ b/arch/arm64/boot/dts/apple/s800-0-3-pmgr.dtsi
@@
 	ps_dwi: power-controller@80110 {
 		compatible = "apple,s8000-pmgr-pwrstate", "apple,pmgr-pwrstate";
 		reg = <0x80110 4>;
 		#power-domain-cells = <0>;
 		#reset-cells = <0>;
 		label = "dwi";
+		apple,always-on;	/* no driver-side re-init exists; see
+					 * docs/power/ds-resume-backlight-touch.md */
 	};
```

Proves the diagnosis in one boot: if the panel survives step 5 of the plan with
this patch and dies without it, §2.6 is confirmed and B2/B3 are the real fix.

**B2 (DTS, 1 line) — widen the window** so the block can be re-initialised at
all. `reg = <0x2 0x0e200080 0x0 0x8>` → `<0x2 0x0e200000 0x0 0xd4>`, and shift
the driver's offsets to absolute (`DWI_BL_CTL 0x80`, `DWI_BL_CMD 0x84`). Without
this, B3 cannot be written.

**B3 (code) — replay `start()`.** Add a `dwi_bl_hw_init()` and call it from a
platform-device `.resume` **only** — not from probe. At boot iBoot's values are
already in the block; rewriting them on the path that works today is a
gratuitous risk, and it also keeps the change testable against a known-good
baseline.

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

and in `dwi_bl_update_status()`:

```c
	dwi_bl_wait_idle(dwi_bl);		/* iOS waits before the write, not after */
	writel(cmd, dwi_bl->base + DWI_BL_CMD);
	writel(DWI_BL_CTL_SEND, dwi_bl->base + DWI_BL_CTL);
```

with, in the same file:

```c
static int dwi_bl_resume(struct device *dev)
{
	dwi_bl_hw_init(dev_get_drvdata(dev));
	return 0;
}

static const struct dev_pm_ops dwi_bl_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(NULL, dwi_bl_resume)
};
```

and `.pm = &dwi_bl_pm_ops` in `dwi_bl_driver`. `struct apple_dwi_bl` needs a
`struct device *dev` for the warning above.

**B4 (code, optional) — `BIT(12)`.** iOS writes `0x71` on this unit (no
`slave-select` in the ADT); we write `0x1071`. The screen works today, so the
bit is either harmless or required by the A9 block. Do not change it blind; a
one-line A/B during the B1 test settles it.

**B5 — put the numbers in DT, not in C.** iOS reads `nclk-div`,
`polarity-config`, `str-delay` and `lockout-us` from the node. If B3 is taken,
mirror that (`apple,nclk-div`, …) so a board with different values does not
need a driver edit.

Ordering note that makes B3 safe: the `backlight_device` is a child of the
platform device, so DPM resumes the platform device (→ `dwi_bl_hw_init`, after
genpd has already powered the domain in noirq) **before** the class device
(→ `backlight_resume` → `update_status`). The init therefore always precedes
the brightness re-send. **[V]** DPM ordering, **[V]** genpd noirq ordering.

### 4.2 `spi-apple-s8000.c` — nothing to save, one thing to add

The driver is stateless by construction: every `transfer_one`
(`spi-apple-s8000.c:64`) writes CTRL=0, CLKDIV, WORD_DELAY, STATUS, CFG,
CTRL=ENABLE, RXCNT, TXCNT, CFG|PIO (lines 104–112) and returns the block to
CTRL=0 (line 151). So:

* a lost register file costs nothing — **do not** add a register cache;
* there is **no** `.suspend`/`.resume`/`.runtime_suspend` today, and
  `devm_pm_runtime_enable` + one `pm_runtime_resume_and_get` at probe
  (lines 176–183) is accounting, not gating.

The one real requirement is that no MMIO happens while `ps_spi3` (PMGR
`0x801d0`, child of `ps_sio_p`) is off.

**S1 (DTS, 1 line, optional).** `apple,always-on;` on `ps_spi3` removes the
whole class of problem: the block keeps its register file and no ordering rule
is needed. Cost: one always-on domain during sleep.

**S2 — do not bother adding `dev_pm_ops` to this driver.** It is tempting to
"pair" the driver with genpd via `SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_*)`,
but it does not prevent the gate: `genpd_finish_suspend()` gates a domain when
`suspended_count == device_count`, and a device with *no* callbacks still counts
as suspended (the genpd glue runs through `dev->pm_domain`, not through the
driver). Runtime-PM bookkeeping here changes nothing about the noirq window.
The ordering problem is solved entirely in the N1 driver (§4.3 T2/T3), because
that is the only thing that ever initiates a transfer.

What *is* worth adding is a comment plus one line of belt-and-braces:

```c
	/* Every transfer re-programs the whole block (CTRL/CFG/CLKDIV/
	 * WORD_DELAY/RXCNT/TXCNT), so ps_spi3 may drop at S2R without a
	 * register restore. The only requirement is that no transfer runs
	 * while the domain is off: apple_n1.c disables the IRQ across
	 * suspend/resume for exactly that reason.
	 */
```

### 4.3 `apple_n1.c` — the real touch work

Current state: no PM callbacks; `IRQF_ONESHOT | IRQF_NO_AUTOEN` with an explicit
`enable_irq()` after the probe drain (`apple_n1.c:949`, `:972`); `n1_irq()`
disables the IRQ **permanently** after 8 bad frames (`:724`); `n1_read_frame()`
never sends a wake packet; `n1_remove()`/`.shutdown` power the device down.

**T1 — do not power the N1 down at suspend, and do not re-run the boot
sequence at resume.** iOS does neither (§3.3, §3.5). `n1_power_on()` /
`n1_power_off()` must stay on the probe/remove paths only.

**T2 — suspend: quiesce the IRQ before the controller's domain can drop.**
Hook these on the `spi_driver` (`n1_driver.driver.pm = &n1_pm_ops`, with
`DEFINE_SIMPLE_DEV_PM_OPS`); `struct apple_n1` is `spi_get_drvdata()`.

```c
static int n1_suspend(struct device *dev)
{
	struct apple_n1 *n1 = dev_get_drvdata(dev);

	/* IRQF_ONESHOT, so this waits for a running thread to finish. */
	disable_irq(n1->spi->irq);
	n1_release_contacts(n1);	/* no stuck finger across the sleep */
	return 0;
}
```

**T3 — resume: wake packet, bounded drain, re-arm.**

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

**T4 — make the IRQ error budget recoverable.** `n1_irq()` currently does
`disable_irq_nosync(irq)` and nothing re-enables it (`:724`, message
`"IRQ disabled after bounded error budget"`). Add an `unsigned int irq_errors`
counter and a `bool irq_dead`; increment on a failed frame and reset on a clean
one; on `irq_dead` schedule a work item that calls `n1_resume()`'s re-arm logic
once, with a 1 s backoff and a hard limit of, say, 3 attempts per boot. A
single glitched resume must not cost touch until the user rebinds.

**T5 — mirror iOS's wake placement: once per resume, not per frame.**
`n1_read_frame()` (`:610`) never sends a wake packet, and **neither does iOS's
frame path** — vtable+`0x918` is only loaded from the control/descriptor
requests (§3.4). Sending one per frame would add 2 ms to every 16 ms frame, so
do not put it in `n1_irq_frame()`. T3 sends exactly one at resume; if the drain
that follows fails, escalate to the same thing iOS does after a reset — the
runtime init (`EB`/`EA` selector + sequence 1 + E2, i.e. `n1_discover()` without
the boot-load) — and only then give up.

**T6 — keep `wake_valid` across suspend honest.** `n1->last_wake` is a
`jiffies` value; after a long sleep `time_before()` will be false, so the first
post-resume wake would be sent anyway. Setting `wake_valid = false` in T3 makes
it explicit rather than accidental.

**T7 — DTS: `wakeup-source` on the touch node is *not* part of this.** The ATN
line (GPIO 65) is not a wake source in our DT and this document does not add
one; §2 of the plan covers why (a resume that cannot touch a gated SPI is a
prerequisite).

### 4.4 Corrections to [ds-suspend-wake-plan.md](ds-suspend-wake-plan.md)

* §3, row **DWI backlight**: "`BL_CORE_SUSPENDRESUME` not set" is wrong —
  `apple_dwi_bl.c:67` sets it and `backlight.c:291` re-sends on resume. Replace
  the row's "fix" with B1/B2/B3.
* §3, row **touch**: "a `.resume` that re-inits the SPI controller" is not
  needed — `spi-apple-s8000.c` re-programs every register per transfer. The
  requirement is ordering (S1 or the N1 IRQ quiesce) plus T2/T3.
* §3, row **touch**, "the N1 itself keeps firmware": correct, and now **[V]**
  rather than **[I]** (§3.5). Strengthen it: iOS *cannot* power the N1 down,
  because the block's only rails are a PMU LDO, a GPIO LDO and a PMGR clock
  gate, and its only off path is the shutdown message.
* §5, open question 5 ("whether `ps_dwi` gating leaves the panel dark") is now
  a *prediction*, not an open question: it does, unless B1/B3 lands. **[I]**

---

## 5. Test order

Prerequisites as in [ds-suspend-wake-plan.md §4](ds-suspend-wake-plan.md):
`no_console_suspend`, netconsole, `panic=0`, values set by hand.

| # | Step | Passes if | Kills |
|---|---|---|---|
| 1 | `pm_test=platform`, `suspend=2`, 1×, read `0x20e200000`/`0x04`/`0xd0` and `0x20a08c000`/`0x00` before and after | both blocks still answer; no CPU stall | the "read while gated hangs" fear, per block |
| 2 | B1 only (`ps_dwi` always-on), real sleep ×5, `brightnessctl set 40%` first | panel comes back at 40 % | isolates domain gating from protocol |
| 3 | B1 reverted, B2+B3 in | panel comes back at 40 % | proves §2.6 and the replay values |
| 4 | S1 only (`ps_spi3` always-on) **plus** T2/T3/T4, real sleep ×5, tap after wake | events flow, no `frame/ATN error` | isolates the SPI side |
| 5 | S1 reverted (domain gated again), same T2/T3/T4 | events flow, no permanent IRQ death after a forced error | proves the ordering fix is the IRQ quiesce, not the always-on |
| 6 | `echo 1 > /sys/class/backlight/*/bl_power`-style blank + sleep | panel dark on sleep, restores on wake | the blank/bl_power semantics |
| 7 | Touch during sleep (press and hold through the sleep transition) | no stuck contact, no lost slot after wake | T2's `n1_release_contacts` |
| 8 | 10 consecutive sleeps with a tap in between | zero IRQ deaths, zero rebinds | the error budget (T4) |

Read-only diagnostics worth keeping in the payload: `cat
/sys/class/backlight/*/brightness`, `cat /sys/kernel/debug/pm_genpd/pm_genpd_summary`
before/after, `grep -E 'spi3|apple_n1|dwi' /proc/interrupts`.

---

## 6. Open questions

1. **What `0x00`/`0x04`/`0xd0` really are.** The reading "global mode / lockout /
   STR delay" is from the property names and the one-shot write pattern. A
   read-back after a cold boot (step 1) settles it: if the values are
   `0x00000001 / 0x00000078 / 0x0002ee00`, the decode is right. **[U]**
2. **Whether `ps_dwi` gating loses them.** Step 1/2/3 answer it; nothing offline
   can. **[U]**
3. **`BIT(12)` in the control word** (§4.1 B4). iOS says 0, we write 1, the
   screen works. **[U]**
4. **The two provider calls at the top of `start()`** (`vtable+0x568`,
   `+0x570`, both with argument 1). Power? Clock? Something else? They are the
   only hint that the DWI needs an enable before MMIO. **[U]**
5. **Whether `AppleSamsungSPIController` saves registers at sleep.** Not
   examined this round; it does not change the Linux plan, because our
   controller driver is stateless. **[U]**
6. **Whether the N1 needs the runtime wake packet after *our* s2idle at all**,
   given that neither its rails nor its clock are dropped. T3 sends it because
   iOS sends it; step 4 with T3's `n1_wake()` removed would answer whether it is
   load-bearing. **[U]**

---

## Appendix A — evidence index

| Claim | Source |
|---|---|
| DWI kext is 1 999 B, no PM, `start`/`sendWLED`/`_waitTransaction` only | `build/power/oc-r2/kexts/AppleS8000DWI.json` (`_PrelinkExecutableSize`); `__text` disassembly `fffffff00634f000`–`fffffff006350200`; vtable `fffffff006ebf830` |
| `start()` writes `0x00/0xd0/0x04/0x84/0x80` | `fffffff00634f50c`–`fffffff00634f530` |
| `sendWLED` encoding, both paths | `fffffff00634fb74`; blocking path writes `0x84`/`0x80`, non-blocking `0xc4`/`0xc0` |
| busy bit + 1 ms deadline + panic | `fffffff00634fac8`; `clock_interval_to_absolutetime_interval(1000000, 0xc8)` at `fffffff00634f4cc` |
| buck path uses `0xa0/0xa4/0xb0` | `fffffff00634f9b8`–`fffffff00634f9cc` |
| `slave-select` → bit 12, absent on j71t | `fffffff00634f470`; live ADT `dwi@E200000` |
| DWI DT values (version 1, nclk-div 1, polarity 1, str-delay 8000, lockout 5) | live ADT `IODeviceTree.txt` 831–850 |
| `BL_CORE_SUSPENDRESUME` is set | `drivers/video/backlight/apple_dwi_bl.c:67` |
| core re-sends brightness on resume, 0 on suspend | `drivers/video/backlight/backlight.c:277`, `:291`; `include/linux/backlight.h:369` |
| `dwi_bl` maps only `0x20e200080+8` | `s800-0-3.dtsi:229` |
| `ps_dwi` = `0x80110`, not always-on; `ps_spi3` = `0x801d0` under `ps_sio_p` | `s800-0-3-pmgr.dtsi:71`, `:226` |
| ADT has no `always-on` property | `grep -c always-on build/firmware/private/IODeviceTree.txt` = 0 |
| AppleARMBacklight level path | `fffffff0059e1314`, `fffffff0059e19dc`, `fffffff0059e1bf8`, `fffffff0059e2010` |
| backlight level persisted in NVRAM | strings `fffffff0052adf02`, `fffffff0052aea7b` |
| N1 personality constants | `AppleMultitouchSPI` info plist, `AppleMultitouchSPIC1F15` |
| N1 node wiring (PMU `0x21f`, GPIO 73/74/75/102, ATN 65) | live ADT 1578–1598 |
| N1 power function (ana then LDO) | `fffffff005e985dc` |
| N1 only-off path is the shutdown message | `fffffff005ea0cd0`, matches `0xE0000310`/`0xE0000250`, logs `"%s: powering off"` |
| N1 runtime wake packet | `fffffff005ea75e8`; boot REQ_WAKEUP `fffffff005ea7548` |
| boot-load is conditional on missing firmware | strings `fffffff00547f33c`, `fffffff00547f355` |
| no PM in the N1 or SPI driver | `apple_n1.c` (no `dev_pm_ops`), `spi-apple-s8000.c` (same) |
| SPI driver re-programs every register per transfer | `spi-apple-s8000.c:104`–`112`, `:151` |
| touch IRQ disabled permanently on error | `apple_n1.c:724` |
| DPM resumes parent before class child | `drivers/base/power/main.c`, `dpm_list` order |

## Appendix B — reproduce (all offline)

```sh
# --- DWI kext: full text, 1152 instructions
python3 - <<'EOF'
from capstone import *
d=open('build/power/oc-r2/kexts/AppleS8000DWI.__TEXT_EXEC.__text.bin','rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
for i in md.disasm(d,0xfffffff00634f000): print(f"{i.address:x}: {i.mnemonic}\t{i.op_str}")
EOF

# --- DWI class table (find start at +0x2d0 and callPlatformFunction at +0x3c8)
python3 - <<'EOF'
import struct
b=open('build/power/oc-r2/kexts/AppleS8000DWI.__DATA_CONST.__const.bin','rb').read()
for k in range(0,len(b),8):
    v=struct.unpack_from('<Q',b,k)[0]
    if 0xfffffff00634f000 <= v < 0xfffffff006350200: print(f"+0x{k:03x} {v:#x}")
EOF

# --- AppleARMPlatform / AppleMultitouchSPI: same recipe with
#     build/video/kexts/AppleARMPlatform.__TEXT_EXEC.__text.bin  @ fffffff0059b3000
#     build/periph/kexts/AppleMultitouchSPI.__TEXT_EXEC.__text.bin @ fffffff005e94000
#     and their __DATA_CONST.__const.bin for the class tables
#     (DWI fffffff006ebf830; N1 fffffff006e12fe8, +0x910/+0x918 are the two wake
#     packets, +0x608 the runtime init, +0x8a8/+0x8b0/+0x8b8 the N1 GPIOs).

# --- ADT facts
sed -n '831,850p;1578,1598p;3284,3320p' build/firmware/private/IODeviceTree.txt
grep -c always-on build/firmware/private/IODeviceTree.txt

# --- Linux side
scripts/build-kernel.sh --run integ 'cd /src/wt/integ;
  sed -n "60,120p" drivers/video/backlight/apple_dwi_bl.c;
  sed -n "64,160p" drivers/spi/spi-apple-s8000.c;
  sed -n "700,745p;940,1000p" drivers/input/touchscreen/apple_n1.c;
  sed -n "60,80p;220,240p" arch/arm64/boot/dts/apple/s800-0-3-pmgr.dtsi;
  sed -n "225,235p" arch/arm64/boot/dts/apple/s800-0-3.dtsi'
```
