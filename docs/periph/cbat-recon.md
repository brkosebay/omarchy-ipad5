# J71t D2365 input-current calibration

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. **Verified statically in Ghidra; device validation remains pending.**
This note covers only the CBAT calculation used by the charging workstream.
No device was accessed, and no kernel files were changed by this subtask.

## Result

For this unit's private 24-byte CBAT record, an iOS input-current request of
2400 mA produces an integer calibrated target of 2400 mA and the byte
**`0xba` for register `0xa03`**. The match with the uncalibrated encoding is
incidental: the verified path applies a nontrivial calibration first.

The requested source budget, charger-enable controls, battery-current limit,
and safe disconnect handling are separate decisions. This calculation alone
does not establish that an arbitrary connected USB port permits 2400 mA.

## Evidence

Kernelcache SHA256:
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
The source is the extracted `AppleD2365PMU.macho` described by
`build/periph/round3-kexts/manifest.json`.

The shared analysis lacked function definitions for the evaluator. A focused,
separate Ghidra project imported the exact extracted Mach-O, defined the entry
points, and decompiled them without changing the shared project. Evidence and
that project are under `build/periph/cbat-re/ghidra/`. The extracted code was
also disassembled independently; listings remain under `build/`.

| Address | Verified role |
|---|---|
| `fffffff006bbf868` | Attach obtains 24-byte `usb-input-limit-calibration`; constructs the outer calibration object with percentage guard zero and an inner fallback with guard 15. |
| `fffffff006bc0614` | Loads the inner default record, then the supplied record into the outer object. |
| `fffffff006bc09cc` | Interleaves five nominal reference values with the first five calibration words; stores the sixth word as metadata. |
| `fffffff006bc0700` | Evaluator thunk obtains the object's percentage guard and invokes the guarded evaluator. |
| `fffffff006bc0710` | Calls the curve evaluator and optionally rejects a result outside a percentage band. The outer guard is zero, so it accepts the curve result. |
| `fffffff006bc07ec` | Specialized low/middle/high range curve, with an inner fallback when its slope test fails. |
| `fffffff006bc0afc` | Generic interpolation in the inner fallback object. |
| `fffffff006bbff6c` | Shifts the calibrated Q16 result to integer mA, then encodes the input-limit byte. |

The public nominal reference vector at `fffffff005915cc0` is
`[100, 475, 1000, 2100, 2400]` in signed Q16. The public fallback record at
`fffffff005915cd4` contains `[105, 500, 1060, 2180, 2480]` in Q16 and metadata
zero. The unit's supplied record has metadata one. Its raw words and precise
derived coefficients remain private under `build/`.

## Exact high-range arithmetic

Let `Q = 65536`, `C3` and `C4` be signed little-endian words at CBAT offsets
12 and 16, and `R` be the requested mA. Values `C3` and `C4` are already Q16.
For `R >= 1500`, the specialized path computes:

```text
slope = trunc_toward_zero((C4 - C3) / 30)
accept specialized path only when 8*Q <= slope <= 12*Q

gain = trunc_toward_zero((C3 - C4 + 600*Q) / 300)
calibrated_Q16 = ((gain * ((R - 2100)*Q)) >> 16) - C3 + 4125*Q
calibrated_mA = calibrated_Q16 >> 16
code = ((8*calibrated_mA - 600) & 0xfff8) / 100
```

The slope comparisons are inclusive. The divisions truncate toward zero;
right shifts preserve the low signed fixed-point result. At `R=2400`, this
unit follows the specialized path and yields `code=186`, or `0xba`.

The common encoder additionally treats targets below 75 mA as code zero
with a separate below-minimum flag, and saturates at code 255 for integer
targets at least 3262 mA. The below-minimum flag is used by the already
identified `0xa00` bit-1 operation. A bounded 2400 mA policy should not need
either extreme behavior.

Do not substitute ordinary interpolation through the unit's five points:
that is not the selected high-range algorithm. Do not silently use the inner
fallback when a Linux safety check rejects the supplied record; failing
closed makes the supported experimental policy explicit.

## Suggested bounded implementation

Stage the private record as `apple/j71t-CBAT.bin` in the firmware overlay and
use `request_firmware_direct()`. Keep the record out of source, patches and
DTS. Missing calibration must return promptly and prevent the higher input
limit from being applied.

For a policy supporting only an explicit 2400 mA request:

1. Require exactly 24 bytes and metadata/version one. Requiring version one
   is a Linux restriction for the observed unit; Apple stores the field but
   does not use it to choose this formula.
2. Decode into signed 64-bit temporaries. Require positive, strictly
   increasing first five points, each at most `3000*Q`. These are defensive
   Linux validation bounds, not claimed Apple checks.
3. Apply the exact slope check and high-range formula above using signed
   64-bit arithmetic and truncation toward zero for division.
4. Require the resulting integer target to be between 75 and 2400 mA and
   the code to be at most `0xba`. Reject a result outside those bounds.
5. Read back the actual byte after writing. Preserve unrelated control bits,
   retain the original input-limit byte for restoration, and make ownership
   of any enable-bit/current-limit changes explicit in the charger policy.

Those input bounds keep the largest intermediate product well within signed
64 bits. Readback as `75000 + code*12500` microamps describes the programmed
hardware setting. It should not be represented as an inverse reconstruction
of an independently measured or negotiated source budget.

## Offline check

The private fixture is `build/periph/cbat-re/check-cbat.py`. It checks all
901 integer high-range requests from 1500 through 2400 against a synthetic
identity calibration, rejects malformed/out-of-range inputs, then checks this
unit's exact 2400 mA result. Its detailed result is private in
`build/periph/cbat-re/2400-result.json`.

```sh
python3 -B build/periph/cbat-re/check-cbat.py
```

Recreate the focused Ghidra evidence with the existing Ghidra MCP Python
environment; the shared server need not be restarted or opened:

```sh
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python \
  scripts/periph/ghidra-local.py \
  --binary build/periph/round3-kexts/AppleD2365PMU.macho \
  --output build/periph/cbat-re/ghidra \
  --function 0xfffffff006bc0700 \
  --function 0xfffffff006bc0710 \
  --function 0xfffffff006bc07ec \
  --function 0xfffffff006bc09cc \
  --function 0xfffffff006bc0afc
```
