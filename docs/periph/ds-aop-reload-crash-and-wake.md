# AOP reload crash and sleep/wake doorbell

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-26, offline only. No device access was performed. This analysis uses
the pinned `ipad6faop-20H350` image, its private Ghidra project, the shared
iOS 16.7.10 J71t kernelcache, the saved ADT/IORegistry, and the captures in
`logs/aop-handoff-20261026/` and `logs/aop-warm-20261026/`.

## Conclusions

1. **Reload crashes on a retained AOP-private SPI0 interrupt, not on retained
   firmware BSS and not while touching an accelerometer register.** SPI0
   aggregate fault/status `+0x8d0` contains `0x40` (channel 3). The new
   firmware has zeroed its per-channel software table but has not installed
   channel 3 yet. Its IRQ handler loads the null slot and faults reading
   `slot->flags` at `NULL+0x4c`. The exact exception is an LPAE level-2 **read
   translation fault**, `DFSR=0x206`, `DFAR=0x4c`, at `PC=0x0102f252`.
2. **The slept AOP did not consume `0x220` because Linux put the word in the
   FIFO without replaying AppleA7IOP's resume-time wrapper notification setup.**
   iOS does not assert CPU RUN and does not use the NMI helper on the ordinary
   status-3 resume path. It calls `setActive(true)`, masks all wrapper sources,
   unmasks bits 0 and 12 in both wrapper views, enables the two AP interrupt
   event sources, enables outbox notification, then sends `0x220`. The Linux
   capture says `ap_irqs_masked=1` / `mailbox_unmasked=0`; the word changed the
   inbox from empty to occupied (`0x21101 -> 0x1201`) but generated no usable
   wake notification to leave the firmware's WFI.
3. **SEP independently exposes the same A9 wrapper-activation gap.** Its five
   AP-to-SEP words changed `snd_sts` to `0xc101` and remained queued for 40 s.
   The saved iOS registry proves that SEP is another `AppleA7IOPV2`
   (`IONameMatch=iop,s8000`, role `SEP`), not merely a similar mailbox. Linux's
   SEP arm writes only `+0x4004=0x1000`; it omits the generic provider's bit-0
   unmask, mirrored `+0xc04` writes, outbox-enable RMW and `setActive(true)`.
   SEP endpoint-0 opcode `0x10` is not a request for a mailbox reply: iOS asks
   `AppleSEPManager` for PM state 1 and sends nothing. Thus the shared symptom
   is evidence for a missing wrapper wake/notification contract, not for a
   required protocol acknowledgement.

## 1. Accel STARTEP reload crash

### The full crashlog identifies the fault

`quiesce-reload/crashlog.bin` is complete (`CLHE` v`0x301`, used `0x650`) and
has the pinned UUID `cce47592d067374e8b46151be730c1de`. `Crtk` names the
current task `rtk_ep_work`; `power` is also present. The useful `CrgA` fields
are:

| Saved field | Value | Meaning |
|---|---:|---|
| r0 | `0xc1100280` | SPI0 channel-3 register bank (`base + 0x280`) |
| r1 | `0x010427f8` | new firmware's per-channel pointer table |
| r4 | `0x01047e78` | static `spi0` controller object |
| r5 | `0` | selected channel object is null |
| r6 | `0x280` | channel-3 register offset |
| r8 | `0x40` | value read from SPI0 `base + 0x8d0` |
| r10 | `6` | status bit index, `2 * channel` |
| r11 | `3` | channel number |
| SP | `0x01046f80` | firmware stack |
| LR | `0x0102c6fb` | Thumb return through the IRQ dispatcher |
| PC | `0x0102f252` | faulting load |
| CPSR | `0x800000b2` | Thumb, IRQ mode |
| DFSR / DFAR | `0x206` / `0x4c` | LPAE level-2 read translation fault at `0x4c` |

`Ccst` independently starts with `0x0102f252`, then `0x0102c6f8`. At the PC,
the pinned bytes decode as:

> Public snapshot: literal code/capture excerpt omitted; protocol and register analysis retained.

The function is now named `AOPSPIMaster_channelFaultIRQ` at `0x0102f20a`.
It reads `controller_base+0x8d0`, tests bit `2*n` for every channel, touches the
channel register block at `base+0x100+0x80*n`, then dereferences
`channel_table[n]`. Thus `0x40` selects channel 3 exactly. This is not an
ambiguous backtrace guess: the captured general registers, DFSR/DFAR and the
faulting opcode all agree.

### Why a new software image meets old hardware

The SPI object at `0x01047e78` is the firmware device named `spi0`. Its target
descriptor gives IOP address `0x10100000`, space 2, size `0x1000`; its runtime
mapping in this crash is `0xc1100000`. The `/arm-io` ADT range translates the
physical peripheral to AP PA **`0x210100000`**. The ADT has no AP SPI node for
it, but PMGR does have the otherwise-unconsumed **SPI0 gate 74 / index 23**.
Warm logs show that gate inherited as on (`power-controller@801b8`, raw
`0x200`). The AOP firmware device table contains no I2C controller, so this is
not retained I2C state.

`AOPSPIMaster_handleEvent` (`0x0102e660`) establishes the race:

- lifecycle event 1 allocates and explicitly zeroes the pointer table;
- it maps the controller, registers three IRQ callbacks (`0x0102f0c8`,
  `0x0102f1b4`, `0x0102f20a`) and enables their IRQs;
- only after that does it dispatch lifecycle event 5, whose
  `AOPSPIMaster_start` (`0x0102f2aa`) resets/reprograms the controller;
- channel objects are allocated later by event `0x14`.

Cold hardware has no pending channel-3 bit during that window. Halt plus SRAM
reload does not reset the dedicated SPI0 register page or its PMGR gate, so the
retained bit can invoke the handler while the replacement table is still
zero. A clean RTKit quiesce does not change that: the proved handoff reached
IOP `0x201`, stable RAM and `steps=0x7ff`, but it deliberately did not halt or
reset peripherals. The following force-stop says `reset_hook=none`.

This also explains why iOS supplies no reload recipe. `AppleSPU_stopEndpoint`
at `0xfffffff005ba9808` sends the AFK `A4/C2` stop, waits for queue-stopped and
calls `AppleSPU_drainStaleEndpointMessages` (`0xfffffff005ba84d0`). That drains
ring messages; it does not reset SPI0. Ordinary iOS sleep resumes the same AOP
image and the same populated channel table. Linux's overwrite-and-restart is a
different lifecycle.

**State classification:** the demonstrated survivor is the **AOP-domain SPI0
controller's channel-3 pending/status state** (`AP 0x2101008d0 bit 6`). The
fresh table proves that retained firmware BSS is not the direct cause. The
MPU-6x00's own configuration may also survive, but the exception happens
before a sensor-register access and provides no evidence that the chip must be
reset. No I2C peripheral is involved.

### Required reload fix

Before a fresh image can unmask its SPI IRQ, the owner must put SPI0 into the
same reset baseline as a cold boot. Clearing/reloading AOP SRAM or draining the
RTKit FIFOs is insufficient. The firmware's controller-local reset at
`0x0102f2f4` is:

```text
saved = SPI0[0x000]
base  = saved & ~0x101
SPI0[0x000] = base | 0x100
wait until bit 8 is set and bit 0 is clear
delay 100 us
SPI0[0x000] = base
delay 100 us
provider power transition 3 (when requested)
SPI0[0x000] = saved
```

The enclosing `0x0102f2aa` first performs provider transition 5, programs
`SPI0+0x8` from the object (default `0x01b71b00`), calls that reset with the
provider transition enabled, and updates control bit 0. A production fix has
two viable locations:

- **Outgoing, preferred if a firmware-owned hook can be exposed:** after the
  accel/AFK endpoints are stopped, make the still-valid old firmware execute
  this SPI teardown/reset before IOP `0x201` and before halt.
- **Incoming reload path:** with the AOP CPU halted and wrapper notifications
  masked, hold only the dedicated SPI0 PMGR domain active and replay the
  controller-local reset through AP PA `0x210100000` before copying/releasing
  the new image. The new firmware then programs its clock/channel registers.
  Do not cycle the whole AOP island, and do not touch the live controller
  before the AOP CPU is stopped.

Longer term, a patched firmware would reset/ack the controller before enabling
the three IRQs, eliminating the initialization race. The Linux driver should
not paper over it by retaining or reconstructing the old channel-object table.

### Minimal device experiment: SPI0 pre-reset

Add one test-only, immutable reload option. After the existing proven halt and
before SRAM load/release:

1. map only `0x210100000..0x210100fff`; record `+0`, `+0x8d0`, `+0x8d4` and
   channel-3 `+0x280/+0x28c`;
2. replay the `0x0102f2f4` register sequence above with bounded polls and the
   two 100-us delays; restore the saved control word;
3. record the same registers and require `(+0x8d0 & 0x40) == 0`; if it is not
   clear, abort without releasing the AOP;
4. run the otherwise unchanged pinned reload and accel STARTEP.

**Pass:** a pre-reset `+0x8d0` bit 6 clears, accel STARTEP returns its normal
`0x20` ACK, configuration reaches streaming and reports advance, with no
crashlog. **Fail:** the reset cannot clear bit 6, or a complete crashlog again
has `PC=0x0102f252`, `DFAR=0x4c`, `r5=0`. If bit 6 clears but reasserts before
STARTEP, the next experiment is the firmware-order fix (reset before IRQ
enable), not an IMU reset guessed from the endpoint name.

## 2. Why the sleeping AOP ignored `0x220`

### Device evidence

The outgoing transcript is a valid in-place sleep handoff: AP `0x10` ACK,
IOP `0x201` ACK, stable `0x186000`-byte RAM hash, idle FIFOs, auto-boot 1,
`steps=0x7ff`, no halt. The incoming kernel validated and consumed that record.
Before the wake write, inbox `+0x4008=0x21101`; immediately after it was
`0x1201` and stayed occupied for 20 seconds. There was no crash and no reply.
`+0x44` read zero throughout. The state file also records
`ap_irqs_masked=1`, `mailbox_unmasked=0`, polling only.

The firmware path at `rtk_power_transition` (`0x01032c18`) is equally specific:
target internal state 8 sends ACK `0x201` at `0x01032d84`, executes WFI at
`0x01032d98`, then on an interrupt restores the previous target, runs power-up
callbacks at `0x01032e2e`, and sends the restored-state ACK `0x20` at
`0x01032e64`. It does not poll the mailbox while in WFI and it does not send a
new HELLO on this path. Therefore putting a word into the FIFO is insufficient;
the wrapper must deliver its notification.

### Exact iOS status-3 resume sequence

`RTBuddy_powerStateTransition` (`0xfffffff005b65b54`) case 1 saves old
`_iopStatus`. When it is 3, the following occurs:

| Order | iOS address | Action |
|---:|---:|---|
| 1 | `0xfffffff005b65cbc` | provider vtable `+0x548`: `start(firmware, resume=1)` |
| 2 | `0xfffffff005b4b7f0` | provider `+0x6c8`, `0xfffffff005b4b6f4`: optional platform service `setActive(true)` |
| 3 | `0xfffffff005b4b854` | provider `+0x660`, helper `0xfffffff005b4e530`: write `0x1111` to wrapper `+0x4000` and `+0xc00` |
| 4 | `0xfffffff005b4b868` | provider `+0x698`, helper `0xfffffff005b4e548(true)`: write `1` to `+0x4004` and `+0xc04` |
| 5 | `0xfffffff005b4b87c` | provider `+0x670`, helper `0xfffffff005b4e5a8(true)`: write `0x1000` to `+0x4004` and `+0xc04` |
| 6 | `0xfffffff005b4b88c`, `...b89c` | enable the two IOInterruptEventSources (ADT indices 2/1, AIC 133/130) |
| 7 | `0xfffffff005b4b8b4` | provider `+0x678`, `AppleA7IOPV2_setOutboxEnable` at `0xfffffff005b4e5d8`: set wrapper `+0x4020` bit 0 |
| 8 | `0xfffffff005b4b8d0` | resume flag branches around `+0x6b0`; **does not call** `AppleA7IOPV2_setCPURun` (`0xfffffff005b4e7c8`) at `...b8e4` |
| 9 | `0xfffffff005b65ccc` | RTBuddy host status becomes 4 |
| 10 | `0xfffffff005b65cdc` | `RTBuddyManagement_sendWake220` writes `0x0060000000000220` |
| 11 | `0xfffffff005b65ce4` | `RTBuddy_waitForBootState` waits for status 8 |

The `setCPURun` helper would RMW wrapper `+0x44` bit 4 and/or the auto-boot
word, but the resume branch skips it. Consequently `cpu44=0` does not diagnose
the failed wake and Linux should not assert bit 4 for this path. The NMI helper
at `0xfffffff005b4e830` writes wrapper `+0xc004=0x11`, then `+0xc024=1`; there
is no call to it in the normal resume sequence. NMI is a recovery fallback,
not the missing ordinary action.

The relevant ADT interrupt list is `[131,130,133,132]`; the established
directions are index 2/AIC 133 for IOP-to-AP outbox and index 1/AIC 130 for the
AP-to-IOP inbox/send notification. The ADT also names wake sources
`AOP.ExternalReq`, `AOP.CPUWakeupAP` and `AOP.OutboxNotEmpty`, consistent with a
wrapper-notification wake rather than periodic mailbox polling.

With the AOP wrapper base `0x210800000`, the relative writes above are absolute
addresses `0x210804000`, `0x210800c00`, `0x210804004`, `0x210800c04`, and
`0x210804020`. CPU control is `0x210800044`; the unused normal-resume NMI pair
is `0x21080c004` / `0x21080c024`.

### SEP corroboration: the same provider, the same unconsumed FIFO

The 14:00 SEP log adds a useful independent control. sepOS had booted and had
already sent the AP an endpoint-0 opcode `0x10`. Linux then published xarm's
OOL buffers with four words and sent the xART reply as a fifth. Before those
writes `snd_sts=0x2cc01`; afterwards it was `0xc101`, whose bit 17 is clear,
and it remained exactly `0xc101` at +5 s, +10 s and +40 s (`tx=5`, `rx=0`).
The FIFO accepted and retained the words; sepOS never drained them. This is
the same hardware-level signature as AOP's `+0x4008: 0x21101 -> 0x1201`.

The saved iOS IORegistry supplies the class relationship that the raw status
cannot: `/arm-io/sep` binds to `AppleA7IOPV2`, role `SEP`, with an
`AppleA7IOPNub` child and `AppleSEPManager` below it. Both `/arm-io/aop` and
`/arm-io/sep` match the same `AppleA7IOPV2` personality
`IONameMatch=iop,s8000`. The generic `AppleA7IOP::start` sequence above is
therefore the iOS wrapper contract for both roles. At SEP base `0x20da00000`,
the corresponding register addresses are:

| Generic provider operation | SEP absolute address |
|---|---:|
| mask `0x1111` | `0x20da04000`, mirrored `0x20da00c00` |
| unmask bit 0, then bit 12 | `0x20da04004`, mirrored `0x20da00c04` |
| set outbox/control bit 0 | `0x20da04020` |
| CPU RUN, not used on resume | `0x20da00044` bit 4 |

By contrast, `drivers/sep/sep-a9.c` arms only by writing zero to
`0x20da04000` and `0x1000` to `0x20da04004`; it never touches the mirrored
notification view at `0x20da00c04`, never explicitly unmasks bit 0, never
executes the `+0x4020` RMW, and has no equivalent of the provider's optional
`setActive(true)`. PongoOS uses the same reduced bit-12 setup while actively
bootstrapping SEP, where a running core polls/drains it; that success does not
show that the reduced setup can wake a sleeping sepOS.

The endpoint-0 opcode `0x10` is not the missing kick. At
`AppleSEPManager_ep0Receive` (`0xfffffff0062edac4`), iOS clears
`SEPCameraDisable` and calls the manager's IOService power request slot with
state 1; there is no mailbox send. In
`AppleSEPManager_setPowerStateGated` (`0xfffffff0062f9df8`), a nonzero-to-
nonzero transition merely updates the manager's current PM state at `+0xbc`
and completes. Only a state-0-to-nonzero transition reaches the underlying
AppleA7IOPV2 provider `+0x540` at `0xfffffff0062f9f58` and then the SEP
boot/resume paths. The live iOS registry later records the manager at power
state 2, so the ordinary opcode-`0x10` state-2-to-state-1 path is bookkeeping,
not a reply or special mailbox word Linux must synthesize.

This makes the SEP result strong corroboration, but not by itself a second
proof of which omitted write is decisive. The AOP experiment below isolates
the complete iOS register sequence first. If it succeeds, mirror the same
activation sequence at the SEP absolute addresses before sending one newly
captured response; if SEP then drains the word, both drivers can share one A9
ASC-wrapper activation helper.

### Minimal device experiment: replay only the iOS wrapper resume

From a fresh DFU cold stream, produce the same valid IOP-`0x201` handoff. In the
incoming kernel, before `0x220`, perform the exact wrapper writes in steps 3–7:

```text
wrapper+0x4000 <- 0x1111
wrapper+0x0c00 <- 0x1111
wrapper+0x4004 <- 0x0001
wrapper+0x0c04 <- 0x0001
wrapper+0x4004 <- 0x1000
wrapper+0x0c04 <- 0x1000
wrapper+0x4020 <- read(wrapper+0x4020) | 1
```

Keep `+0x44` and auto-boot unchanged and do **not** write `+0xc004/+0xc024`.
The AP may continue polling for this first proof; enabling its AIC event
sources is not required to observe the reply, but the production path should
enable them as iOS does. Then send exactly one `0x0060000000000220`.

**Pass:** `+0x4008` returns to an empty state instead of remaining `0x1201`,
and the first firmware reply within the bounded window is
`0x0070000000000020`; no HELLO is required and `iop_crashed` remains zero.
Proceed with retained endpoint/buffer adoption only after that ACK. **Fail:**
the inbox remains occupied or no `0x20` arrives. A failure isolates the next
one-variable test to the platform `setActive(true)` equivalent. Do not combine
RUN or NMI with this first test, because iOS's normal path uses neither and
mixing them would destroy the diagnosis.

## Reproduction and saved analysis

Run the crash assertions without Ghidra:

```sh
python3 -B scripts/periph/aop/re-reload-crash-wake.py
```

To regenerate the private firmware exports and annotations:

```sh
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec \
  ~/.local/share/uv/tools/ghidra-mcp/bin/python -B \
  scripts/periph/aop/re-reload-crash-wake.py --firmware
```

Evidence is emitted under `build/periph/aop-stage3/re/reload-wake/`. The private
firmware project now names/comments the SPI handler/reset path. The shared
kernelcache now names `AppleSPU_start`, `AppleSPU_stopEndpoint`,
`AppleSPU_drainStaleEndpointMessages`, `AppleA7IOPV2_outboxEmpty`,
`AppleSEPManager_start`, `AppleSEPManager_endpointsEnabled`, and
`AppleSEPManager_setPowerStateGated`. Comments cover the resume wrapper
helpers, RUN skip, NMI helper, `0x220` call, SEP opcode-`0x10`, and the SEP PM
state paths. Both Ghidra projects were saved.
