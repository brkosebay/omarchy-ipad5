# AOP warm reload: SCM state 3 and the wake8 experiment

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Offline only; no device access. **[V]** means verified in the pinned
firmware or saved evidence, **[I]** an inference, **[E]** an untested experiment,
and **[U]** an unresolved fact.

`scripts/periph/aop/re-spi0-ch3-round5.py` verifies the round-4 input hashes,
checks the wake7 log, exports the exact reset/sleep/restore instructions, and
decompiles the SPI master and MPU stop helpers in the private Ghidra project.
Run `uv run --with capstone --with pyghidra python -B scripts/periph/aop/re-spi0-ch3-round5.py`.
Evidence is under `build/periph/aop-stage3/re/spi0-ch3-round5/`; `inventory.json`
records hashes and addresses. Main SHA-256:
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8`;
extension: `2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22`.

## What the panic actually means

The decode from round 4 still holds. `AOPSPIMaster_transferIRQ` (`0102f0c8`)
handles AOP IRQ `0x10014`, clears SPI control bit 0, masks that IRQ, and reads
the 64-bit SCM state at `SPI0+0x0c/+0x10` through `0102eeb8`. If its optional
master callback is absent or does not handle the event, it panics at
`0102f1a2` with `SCMErr:0x%llx,%u`. **[V]**

`SCMErr:0x3,3` is **SCM flags/state 3, selected channel 3**, not an error-code
enum. Bits 0 and 1 choose a token and callback event kind (bit 1 selects kind
2); clearing those flags leaves position 0. The strings do not name the
physical meanings of those flags. No firmware-derived write to either SCM
status word was recovered in the SPI master paths. A nonzero SCM word alone
therefore does not establish an uncleared error. **[V]**

Wake7 logs `scm_lo=00000003 scm_hi=00000000` at the idle poll and before/after
the targeted INTC acknowledge, with `c3_interval=01507725 c3_cursor=00000000`.
It passes channel teardown in 122 us and the 31-sample/184 ms window, but
still crashes at accel STARTEP. The targeted pending-clear hypothesis failed
this device test; it must not be presented as a working fix. **[V]**

The stack again interrupts SPI lifecycle event 1 at `01074084`. That event
enables `0x10014` **before** its event-5 controller start/reset. MPU channel
construction and sensor initialization are later. This makes retained
interrupt delivery the immediate difference to test, rather than a failed
MPU register transaction. **[V]/[I]**

## Firmware reset and sleep/wake sequences

INTC event 3, using the mapped wrapper at `c1800000` (AP `210800000`): **[V]**

| PC | Operation |
| --- | --- |
| `0102c592` | `str.w r1,[r0,#0x808]`, r1 = 1 |
| `0102c598` | `ldr.w r1,[r0,#0x808]` |
| `0102c59c` | `str.w r5,[r0,#0x808]`, r5 = 0 |
| `0102c5a2` | `ldr.w r0,[r0,#0x808]` |

These ordering reads are absent from the old decompiler output; the new
script checks the exact 20 bytes spanning both stores and both loads. The
last load is `0102c5a2`, correcting round 4's `0102c5a0` address. This is a
full interrupt-controller lifecycle reset, broader than `+0x980` acknowledge.
It is not a proved SPI-SCM status clear. **[V]**

The sleep command saves `+0xa00..+0xa08`, masks every group-1 source, writes
group-7 `+0xc020/+0xc024 = 2`, then spins until `+0x81c == 0`. The restore at
`0102c9d2` writes saved masks to `+0xa00..` and their complements to `+0xa80..`,
then group-7 routing/enables. Neither path writes SCM status or resets the
MPU. This is a retained-image resume, so copying fresh firmware is a different
case; do not describe ordinary iOS sleep/wake as a cold sensor restart. **[V]**

Wake8 retains wake7's source disable, SPI reset, teardown, clean window and
targeted acknowledge, then adds the exact event-3 pulse just before RUN.
Whether it removes the retained delivery on this hardware is **[E]**. SCM
remaining `3/0` after that pulse is allowed: STARTEP and streaming determine
whether delivery is fixed. No zero write to SCM or the interval/cursor is
justified by this analysis.

## MPU6900 FIFO/reset question and cold comparison

There is a real sensor-side stop operation at `0100ef14`, called from the
common `MPU6xxx_setStreamingEnabled` disable branch (`0100e8b6`): **[V]**

```text
write MPU 0x61 = 0
write MPU 0x23 = 0
cached[0x6a] &= ~0x40; write MPU 0x6a = cached[0x6a]
write MPU 0x6a = cached[0x6a] | 4
read MPU 0x74
then axes standby (0x6c |= 0x3f), sleep (0x6b |= 0x40), async channel stop
```

The bit-2 pulse in that FIFO/data-path stop is evidence for a sensor reset
operation, not a host SPI0 register. The separate helper at `0100eee4` writes
`0x23 = 0` or a channel mask; in the streaming helper it is selected for IDs
`0x90/0x95`, so it must not be substituted for the j71t `0x10` path. **[V]**
No retained transcript proves whether the common disable branch ran after
rates-zero with all firmware clients considered. **[U]**

Fresh j71t init (`01072e3c`) writes `0x6a=0x10`, `0x6b=1`, `0x37=0x20`,
`0x6c=0x3f`, `0x1b=0x18`, `0x1d=3`, `0x1a=2`, then reads `0x3a`. It does
not issue a `0x6b` bit-7 soft reset. These commands happen after the failing
event-1 enable, so no sensor reset can be the immediate missing operation
at that stack boundary. A later warm-stream/data-quality problem could still
require the firmware's sensor stop path; wake8 does not claim otherwise.
Raw AP sensor commands are not added. **[V]/[I]**

There is still **no true-cold SPI0 MMIO capture immediately before first
STARTEP**. Cold runs deliberately skip AP access to the uninitialized block.
The exact comparison available is to firmware's first use, not invented
silicon defaults: **[V]**

| Word | Wake7 after teardown | Fresh firmware first action |
| --- | --- | --- |
| C3 | `00010406` | event `0x14` replaces it with `00000400` |
| route/rate | `00000010 / 019000c8` | recomputes/writes both |
| C3C | `00200009` | programs config and bit-5 acknowledge pulse |
| descriptor | `00000000` | installed later with a transfer |
| `8d0/8d4` | `0/0` | channel aggregate status |
| SCM | `3/0` | read-only in recovered master methods |
| interval/cursor | `01507725/0` | scanned by the early IRQ; no proved clear |

Rewriting route/rate, clearing the interval, or guessing descriptor/FIFO
stores would not prevent this earlier interrupt. Wake8's seven snapshots
will distinguish the pulse's effects on observed SPI0 state without adding
such writes. **[I]/[E]**
