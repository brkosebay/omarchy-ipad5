# AOP SPI0 channel 3 — warm-reload round 4

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Scope: offline analysis of the pinned iOS 16.7.10 j71t
AOP firmware, its extension, and the controlled round-3 reload evidence. No
device access was used.

Evidence labels: **[V]** verified in pinned firmware or retained device data,
**[I]** inferred from verified facts, and **[E]** a proposed device experiment
or its expected result.

## Result

1. `SCMErr:0x3,3` is raised by the SPI0 master's global interrupt
   `0x00010014`, not the already-fixed per-channel fault interrupt
   `0x00010012`. The first `3` is the complete 64-bit SCM state read from
   `SPI0+0x0c/+0x10`; it is a bitfield/state value, not error number 3. The
   second `3` is the hardware channel selected by the master's interval scan.
   **[V]**
2. The interrupt preempts `AOPSPIMaster_handleEvent(..., 1, ...)` while the
   accelerometer STARTEP is starting j71t private peripherals. Event 1
   registers and enables all three SPI0 IRQs before it invokes event 5, which
   would run the firmware's own SPI0 start/reset. No MPU channel configuration,
   GPIO14 programming, or MPU6900 register transaction has happened yet.
   **[V]**
3. The immediate warm/cold difference is therefore a deliverable, retained
   interrupt-controller source for `0x10014`. The old SPI channel state explains
   why the retained SCM position resolves to channel 3, but route/rate, C3 bits
   16/10, the descriptor, GPIO14, and MPU register state cannot be the immediate
   cause of this stack. **[I]**
4. The smallest firmware-derived bridge is two targeted AOP interrupt-controller
   writes: disable source `0x10014` while the old CPU is halted, perform the
   existing SPI0 reset/channel-3 teardown, then apply the firmware's group-1
   pre-handler acknowledge/clear action for `0x10014` immediately before RUN.
   Fresh firmware re-enables the source in
   event 1. **[V]** for the register derivation and write semantics; **[I]** that
   the retained pending bit is the remaining cause; **[E]** until the reload
   experiment passes.

## Pinned evidence and saved analysis

The repeatable analysis is
`scripts/periph/aop/re-spi0-ch3-round4.py`. It pins these inputs:

- main AOP firmware SHA256
  `af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`;
- extension SHA256
  `2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22`;
- decoded crashlog SHA256
  `663e8cf5bf1093b3bf9e9ca92e4b9afd3854fc2231ddd13e09d7e9999eeaae30`;
- reload dmesg SHA256
  `e6f121d9c9377ac2963ef6c1cf111749d459e5c454b59fc2ce0514e19aef3a44`;
- retained cold SRAM SHA256
  `1653ba275a97103c4efcb45f591e0bf2f658617bf250b20330bf68d4222c6e43`.

The script verifies the exact Cstr, all 13 Ccst addresses, the round-3
post-teardown values, and the live-cold interrupt-controller object at
`0x01052da8`. It exports disassembly/decompilation plus `inventory.json` below
the ignored directory `build/periph/aop-stage3/re/spi0-ch3-round4/`. It also
renames and saves the understood functions and comments in the private main
firmware and combined main+extension Ghidra projects. **[V]**

## 1. What raises `SCMErr:0x3,3`

### 1.1 The exact panic path

The SPI0 object at `0x01047e78` assigns:

| Object field | AOP IRQ | Handler | Role |
| --- | --- | --- | --- |
| `+0x3c` | `0x00010010` | `0x0102f1b4` | per-channel class 1 |
| `+0x40` | `0x00010012` | `0x0102f20a` | per-channel class 0/fault |
| `+0x44` | `0x00010014` | `0x0102f0c8` | global SCM/transfer state |

The controlled crash enters `AOPSPIMaster_transferIRQ` at `0x0102f0c8` through
source `0x10014`. That handler does the following: **[V]**

1. `0x0102f0dc..0x0102f0e2`: clear SPI0 controller control bit 0.
2. `0x0102f0e4..0x0102f0ec`: disable IRQ `0x10014` through the AOP interrupt
   controller.
3. `0x0102f0ee..0x0102f0f6`: call vtable method `+0x40`, now named
   `AOPSPIMaster_readSCMState` at `0x0102eeb8`. It reads the 64-bit value from
   `SPI0+0x0c` (low) and `SPI0+0x10` (high).
4. `0x0102f112..0x0102f156`: scan channel interval/cursor pairs beginning at
   `SPI0+0x110/+0x114`, with a stride of `0x80`, and select a channel.
5. `0x0102f158..0x0102f186`: offer the event to the optional master callback at
   object `+0x78`. The pinned live-cold object has callback/context `0/0`.
6. `0x0102f190..0x0102f1a2`: reread the state and panic with
   `SCMErr:0x%llx,%u` when no callback handled it.

The format string is at `0x010398e3`; its literal reference is at
`0x0102f1b0`. AAPCS places the aligned 64-bit value in `r2:r3` and the channel
argument on the stack, so the decoded Cstr is unambiguously:

```text
SCM state = 0x0000000000000003
selected hardware channel = 3
```

Within state `0x3`, low bit 0 selects one of two opaque transfer-event tokens,
low bit 1 selects callback event kind 2 rather than kind 1, and the position
passed to a callback is `state & ~3`, here zero. The firmware does not treat
the numeric value `3` as an error enum. **[V]** The human-readable meanings of
the two low flags are not present in this image. **[I]** only if given names
such as direction/completion; this note intentionally does not assign them.

### 1.2 What “SCM” means here

The image contains `SpuScm.cpp`, `BMP284_scm.cpp`, and the diagnostic
`Unable to select phosphorus scm channel`, adjacent to `spi0`. The code reads a
global state/cursor and maps it through per-channel intervals. Thus SCM is the
firmware's name for the SPU-side, channelized SPI transfer engine/state, not the
MPU sensor service and not a Linux-side manager. **[V]** The exact expansion of
the acronym is not embedded in the pinned image, so “SPI controller manager”
or “sensor channel manager” would be invented. **[V]**

The nearby `SCMto:%u` string at `0x010398da` is used by the SPI channel-disable
timeout path, further tying SCM to this hardware transfer engine. **[V]**

### 1.3 Every Ccst frame

The exception boundary is the ARM IRQ wrapper at `0x010005a0`; its
`0x010005bc` `BL` enters the Thumb IRQ front end through veneer `0x010352cc`.
Frames above the boundary are interrupt/panic processing. Frames below it are
the interrupted `rtk_ep_work` callback. **[V]**

| Saved PC | Resolved function | Meaning at the saved PC |
| --- | --- | --- |
| `0x01033834` | `aop_crashlog_write_callstack_section` `0x010337bc+0x78` | unwind/copy the fatal call stack |
| `0x010333c6` | `aop_crashlog_serialize_sections` `0x01033354+0x72` | call the selected crash-section writer |
| `0x01033236` | `aop_record_fatal` `0x0103310c+0x12a` | build the fatal record |
| `0x01034a64` | `aop_panic_printf` `0x01034a44+0x20` | format and record the panic |
| `0x0102f1a2` | `AOPSPIMaster_transferIRQ` `0x0102f0c8+0xda` | call `aop_panic_printf` with SCM state/channel |
| `0x0102c6f8` | `aop_interrupt_dispatch` `0x0102c66c+0x8c` | call the registered `0x10014` handler |
| `0x010272f8` | `aop_irq_frontend` `0x010272ec+0x0c` | dispatch through the AOP interrupt-controller object |
| `0x010005bc` | `aop_irq_exception_wrapper` `0x010005a0+0x1c` | ARM exception wrapper enters the Thumb IRQ path |
| `0x01074084` | `j71t_start_private_peripherals` `0x0107402c+0x58` | interrupted `BLX` of SPI master lifecycle event 1 |
| `0x01072382` | `j71t_platform_start` `0x01072348+0x3a` | call private-peripheral startup |
| `0x01073c96` | `ext_work_callback_invoke_and_free` `0x01073c84+0x12` | invoke the queued extension callback |
| `0x01033b70` | `rtk_ep_work_task` `0x01033a8c+0xe4` | invoke one RTKit endpoint work item |
| `0x01029ee0` | `aop_task_trampoline` `0x01029edc+0x04` | invoke the task entry function |

This ordering explains why the firmware can complete HELLO, EPMAP, grants, and
`SYSTEM ready` before dying: j71t private-peripheral startup is lazy work
reached by the accelerometer STARTEP. **[I]** from the verified stack and
device message order.

## 2. The remaining warm/cold state difference

### 2.1 Event 1 exposes the stale source before firmware reset

`j71t_start_private_peripherals` calls the SPI0 object at `0x01047e78` with
lifecycle event 1 at `0x01074084`. In
`AOPSPIMaster_handleEvent` event 1: **[V]**

1. allocate/clear the software channel-pointer table;
2. map SPI0;
3. register handlers for `0x10014`, `0x10010`, and `0x10012` at
   `0x0102e71e..0x0102e748`;
4. enable those sources at `0x0102e74a..0x0102e76c`;
5. only then recursively dispatch event 5 at `0x0102e79c..0x0102e7ae`.

Event 5 calls `AOPSPIMaster_start` and then the controller reset at
`0x0102f2f4`. The retained `0x10014` becomes deliverable at step 4 and preempts
before step 5. The firmware therefore never gets a chance to sanitize its own
SPI hardware on this path. **[V]**

Round 3 proved that the SPI0 reset plus channel teardown suppresses
`SPI0+0x8d0 bit 6` for 31 samples/182 ms. It did not capture the following
state: **[V]**

```text
SPI0+0x00c/+0x010       SCM state low/high
SPI0+0x290/+0x294       channel-3 interval/cursor pair
AOP INTC source 0x10014 retained-pending state
```

The crash supplies part of the missing observation: SCM is still `0x3`, the
interval scan still resolves that position to channel 3, and IRQ `0x10014` is
delivered immediately after event 1 enables it. **[V]** The clean peripheral
window plus delivery only on re-enable is most consistent with a retained AOP
interrupt-controller pending latch left from the old session, rather than a
continuously reasserting channel-fault source. **[I]**

### 2.2 Post-teardown words versus fresh firmware initialization

There is no true-cold dump of these SPI0 MMIO words in the retained evidence.
Consequently, this note does not invent silicon reset values. What can be
compared exactly is the warm value against the fresh firmware's first write or
first use: **[V]**

| Field | Warm post-teardown | Fresh firmware action | Consequence |
| --- | --- | --- | --- |
| `C3` `SPI0+0x280` | `0x00010406` | event `0x14` writes `0x00000400` wholesale at `0x0102e992` | retained bit 16 is cleared; bit 10 is deliberately reintroduced; neither is a prerequisite |
| `C3_ROUTE` `+0x284` | `0x00000010` | route loop writes the selector mask at `0x0102ea9c`; selector 4 produces `0x10` | old value already matches and is overwritten |
| `C3_RATE` `+0x288` | `0x019000c8` | recomputed from master/config clocks and written at `0x0102eace` | old value already matches and is overwritten |
| `C3C` `+0x28c` | `0x00200009` | read/modify/write at `0x0102ea18..0x0102ea68`, including the self-clearing bit-5 pulse | valid retained base bits; no cold-zero assumption |
| `C3_DESC` `+0x298` | `0x00000000` | populated only when a transfer descriptor is installed | zero is the teardown baseline |
| `SPI0+0x8d0/+0x8d4` | `0/0` | per-channel aggregate status | round-3 fault source is clean |
| SCM `+0x00c/+0x010` | not logged | read by the early `0x10014` handler as `0x3/0` | remaining directly observed stale SPI state |
| C3 interval `+0x290/+0x294` | not logged | early handler reads it before event `0x14`; fresh init does not explicitly clear it | explains selection of channel 3, but clearing it alone would still panic with channel `-1` |

In particular, rewriting route/rate or zeroing the descriptor cannot prevent
an interrupt that occurs before those fields are consumed. Clearing C3 bit 16
or bit 10 is also unnecessary: the first channel-init write replaces the whole
word. **[V]**

GPIO14 is configured later by the SPI client at `0x01005f70`. MPU register
initialization at `0x01072e3c` is later still; the normal j71t start path writes
`0x6a=0x10`, `0x6b=1`, `0x37=0x20`, `0x6c=0x3f`, `0x1b=0x18`,
`0x1d=3`, and `0x1a=2`, then reads `0x3a`. It does not issue an MPU soft reset
through `PWR_MGMT_1` bit 7. **[V]** An MPU reset might become relevant to a
later data-quality failure, but neither it nor GPIO14 can explain this pre-MPU
interrupt stack. **[I]**

## 3. Smallest firmware-derived fix

### 3.1 Recommended targeted sequence

The AOP interrupt-controller runtime object at `0x01052da8` contains mapped
AOP base `0xc1800000`; the corresponding AP wrapper base is `0x210800000`.
The IRQ encoding is group 1, source `0x14`. The firmware computes bank 0 and
bit `1 << 20 = 0x00100000`. **[V]**

Add these two writes around the existing round-3 sequence:

```c
/* Old AOP is sequentially quiesced; CPU halted; AP notifications masked. */
write32(0x210800a00, 0x00100000); /* disable AOP IRQ 0x10014 */

/* Existing firmware-derived SPI0 reset, C3 teardown, and clean window. */

write32(0x210800980, 0x00100000); /* group-1 ack/clear for IRQ 0x10014 */
/* Then load/release RUN as before. Fresh event 1 re-enables 0x10014. */
```

Provenance and ordering: **[V]**

| Write | Firmware provenance | Why it is placed here |
| --- | --- | --- |
| wrapper `+0xa00 = 0x00100000` | `aop_interrupt_write_enabled` `0x0102c972`, group-1 disabled branch; store at `0x0102c9ce` | prevent a new latch while the AP resets/tears down SPI0 |
| wrapper `+0x980 = 0x00100000` | group-1 pre-handler acknowledge/clear action in `aop_interrupt_dispatch`, calculation/store `0x0102c6de..0x0102c6ee` | discard the old-session `0x10014` after its peripheral cause is quiet **[I]** |

Both are write-one command banks. The firmware does not compare their
readback; an AP `readl()` may be used only as a posted-write ordering barrier
and its returned value should be logged, not used as a pass/fail value.

> **Erratum (wake7, 2026-10-26):** the firmware does read `+0xa00`/`+0xa80`
> back, as the group-1 mask (1 = disabled). See the sleep save at
> `0x0102c5ce`, the restore at `0x0102c9e6/0x0102c9f4` and the previous-state
> read at `0x0102c9c0`. Wake6's `ffffffff` meant the quiesce had already masked
> 0x10014. `+0x980` is still never read. See `aop-stage3.md`, `a9/aop-wake7`.

The expected state readbacks remain the existing round-3 ones:

```text
C3       = 0x00010406   (run/class-0/class-1 enables clear, idle set)
C3C      = 0x00200009   (bit-5 pulse self-cleared)
C3_DESC  = 0x00000000
SPI0+8d0 = 0x00000000
SPI0+8d4 = 0x00000000
```

The decisive expected behavior is that enabling `0x10014` in fresh event 1
does not vector before event 5. **[E]**

Do not write `SPI0+0x0c/+0x10`: the firmware only reads those words and this
analysis found no firmware provenance for treating them as writable. Do not
clear the C3 interval words as the primary fix; that changes only the printed
channel and not the fatal global interrupt. **[V]/[I]**

### 3.2 Broader fallback, not the first experiment

The firmware has a full interrupt-controller lifecycle-event-3 reset in
`aop_interrupt_controller_command` at `0x0102c524`: **[V]**

```text
0x0102c592  write wrapper+0x808 = 1
0x0102c598  read  wrapper+0x808
0x0102c59c  write wrapper+0x808 = 0
0x0102c5a0  read  wrapper+0x808
```

At AP addresses this is `0x210800808`; final expected readback is zero. The
firmware uses both reads for ordering and does not require the first read to be
one. This pulse is a valid firmware-derived fallback if the targeted pending
clear returns the same stack, but it resets all AOP-private interrupt state and
is therefore broader than the two-source commands above. **[I]**

### 3.3 One discriminating device experiment

On top of `a9/aop-wake5`, make exactly the two targeted writes and no sensor,
route/rate, or C3 changes. Keep the existing reset, teardown, and 182-ms clean
window. Add read-only logging immediately before the final pending clear for:

```text
SPI0+0x00c/+0x010       0x21010000c / 0x210100010
SPI0+0x290/+0x294       0x210100290 / 0x210100294
AOP INTC current source 0x21080081c
```

Log the same three pairs/words after the posted-write barrier, without making
their undocumented values a gate. Then RUN the fresh firmware and perform the
same endpoint sequence through `0x28`. **[E]**

Pass requires all of the following in one warm reload: **[E]**

- STARTEP `0x28` returns status 0 rather than `-110`;
- no `SCMErr` crashlog appears;
- the accelerometer service reaches ready and supplies changing samples after
  the warm reload.

Failure is any `0x28` timeout, AOP crash, or absent/static accelerometer stream.
If the exact `SCMErr:0x3,3` stack repeats, the targeted clear did not remove a
level/reasserted source; only then should a separate experiment substitute the
firmware event-3 `+0x808` reset. This keeps the first test single-variable and
distinguishes a retained interrupt-controller latch from a still-active SPI
source. **[E]**
