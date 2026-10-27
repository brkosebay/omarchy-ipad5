# Round 4: D2365 ADC completion and freshness

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. **Verified statically in Ghidra; no device access or kernel edits.**
This note supplies the missing completion path for PMIC ADC channel 1.
Raw decompilation, exact extracted-code evidence and the focused Ghidra
project are under `build/periph/round4-adc-re/`.

## Verified completion path

| Address | Verified behavior |
|---|---|
| `fffffff006bb91f0` | D2365 startup registers the common ADC-complete callback for event `0x1601`, then unmasks its IRQ index `0xb0`. |
| `fffffff006bb9b70` | IRQ event registration converts high-byte bank and low-byte one-hot mask into `bank*8 + bit`. Thus `0x1601` is bank `0x16`, bit 0. |
| `fffffff006bba334` | D2365 virtual slot `0x618` reads 30 IRQ-latch bytes starting at **`0x140`**. ADC completion is therefore **`0x156`, bit 0**. |
| `fffffff006bb9a10` | IRQ dispatch reads that bank, iterates set bits, and calls registered callbacks. The normal path has no explicit IRQ-status acknowledge write. |
| `fffffff006bb73bc` | ADC callback changes software state at `self+0xb8` from running (1) to completed (2), then wakes its command-gate waiter. |
| `fffffff006bb73fc` | Common gated ADC read serializes conversions, starts conversion, normally waits up to one second for completion, then reads the result. |
| `fffffff006bc0e58` | Trigger writes exactly one channel byte to **`0x4000`**. |
| `fffffff006bc0e8c` | Result reads two bytes at **`0x4001`**. Raw ADC value is `(low & 0x0f) \| (high << 4)`; low-byte bit 7 rejects the result. |
| `fffffff006bbf3e8` | End-conversion virtual method is a no-op. No hardware abort is established. |
| `fffffff006bba5bc` / `fffffff006bba658` | Mask/unmask respectively set/clear the selected bit at `0x1c0 + index/8`. The ADC mask is **`0x1d6`, bit 0**, with one meaning masked. |

The D2365 object vtable is at `fffffff006fd37f8`. The slot mappings and
constructor pointer establish that these are the selected D2365 methods.
Some shared Ghidra function definitions are absent or overlap adjacent code;
the focused project defined exact entry points and the disassembly was used
to exclude spurious decompiler fallthrough after stack-check failure stubs.

**Distinction:** `0x180` is the separate live/status bank. Reading
`0x196` does not substitute for the completion latch at `0x156`.

## What the code establishes about freshness

iOS waits for completion before reading the result. The trigger method has
no software clear of the completion latch or result register. Consequently
the kernelcache does **not** prove that triggering makes an old result
invalid. A fixed delay followed by a valid-bit check is insufficient to
establish freshness.

**Inferred:** the IRQ-latch bank is consumed or deasserted by reading it.
The normal dispatch path only reads the bank and calls callbacks; it has no
explicit status-ACK write. This inference does not authorize a write to an
assumed W1C register. A safe polling experiment can instead require observing
a zero completion bit before triggering. If a stale bit never clears when
read, it must fail closed.

The normal one-second command-gate deadline is verified. A separate nonzero
software timeout field has a path that forces software state 2 after timeout;
that path should not be copied into a fresh-sample policy.

## Bounded polling candidate

The charging owner is implementing a conservative polling path. Its necessary
conditions are exclusive ownership of ADC conversions and no outstanding
older conversion:

1. Read only completion byte `0x156` until bit 0 is actually observed zero,
   with a bounded drain deadline. Do not write IRQ status or change masks.
2. After the independently verified Tristar mux settling sequence, write
   channel byte 1 to `0x4000` exactly once.
3. Poll `0x156` for a new bit-0 assertion, with a maximum one-second deadline.
4. Only after that event, read both result bytes at `0x4001`, require low-byte
   bit 7 clear, and decode the 12-bit result.
5. Reject classification on every timeout, invalid result or I/O error;
   restore the Tristar routing state through its owner's verified path.

**Critical late-completion rule:** if trigger delivery is uncertain, the
completion wait times out, or polling fails after triggering, the hardware
conversion may still finish later. A later zero-bit precheck alone cannot
exclude that in-flight conversion. Disable further ADC conversions until a
verified abort/reset/idle mechanism is available, or until reboot. Otherwise
the previous conversion's late IRQ could be mistaken for the next mux
sample. Successfully completed conversions can be serialized and repeated.

Do not restore `0x4000` by writing its previously read value: this is a trigger,
so that write would initiate another conversion. There is no verified abort
in the no-op end-conversion method.

**Remaining hardware assumptions:** initial ownership after the boot handoff
must exclude an older in-flight conversion; the available code has only
software serialization, not a verified hardware busy flag. Successful
completion-latch polling while preserving the inherited mask is also a
device-test result still to establish. If masking prevents a latch assertion,
the candidate times out and rejects the higher-current classification.
No masked-latch behavior, trigger invalidation or fixed conversion latency
is claimed as verified.

The channel-1 voltage conversion and USB routing/classification math remain
with the charging and Tristar workstreams. This note does not establish the
source's current entitlement from VBUS or from a stale ADC value.

## Shared analysis annotations

Six existing functions were given descriptive names covering ADC completion,
gated read, event registration, mask/unmask and result decoding. Missing or
overlapping function entries received disassembly comments instead of
renaming an incorrectly containing function. Details are recorded in
`build/periph/round4-adc-re/annotations.json`.

Kernelcache identity is the same SHA256 recorded in `charging-recon.md` and
the extracted-kext manifests. The primary workstream owns integration,
testing and the final kernel build.
