# Charge10: computer precedence after early BC1.2 detection

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Kernel branch `a9/charge10`, based on `a9/charge9`
`16a34c8432c61937366964d493b82f387e7d51cc`. Commit
`a18b0e2ee18def3a2d6090e9dc8edd73beb3abd8`; entry `charge10-a18b0e2ee1`.
Reuses `/src/wt/charge` in `linux-a9-src`, with debug info off. No device
access, workspace git commit or push. **Hardware validation of charge10 is
NOT RUN.** Round9's device successes are evidence for that earlier entry.

## Captured failure and cause

Evidence: [round9 result](../release/OMISSIONS.md),
[TSV](../release/OMISSIONS.md), and
[dmesg/state](../release/OMISSIONS.md), evidence commit
`deb49402`. The USB-C wall source and Mac USB-C power adapter correctly reached
qualified CC 3 A / board 2.4 A on round9. Mac USB-A and the battery-status and
attach-default repairs also passed there.

The remaining Mac USB-C computer sequence was:

- At **237.549669**, BC1.2 was published as numeric type 2, 1500 mA, passive
  D+/D− 39/39 mV; the cable ID appeared at **237.783138**, about 233 ms later.
- A real USB session then existed: `stage=usb-active`, successful networking,
  and nine data-generation events. Yet the supplier remained DCP/1500000 and
  the charger stayed at ceiling 1500000, nominal 1000000, live 962500.
- The TSV contains **44 rows** with this combination. It does not give an exact
  timestamp for the first enumeration event or the secondary ADC voltage.

`apple_tristar_update()` explicitly excluded an existing DCP/CDP result from
computer promotion. Its later BC override restored that lower allowance even
when gadget accounting showed a host. The new harness executes the actual
round9 classifier/update functions to reproduce BC → initial ID → configured
USB still reporting DCP/1500, then runs the fixed sequence.

## Corrected iOS BC port mapping

The earlier charge7 interpretation and Linux secondary branch were reversed.
The voltage comparison itself was read correctly; its internal enum names
were assigned incorrectly. Kernelcache SHA256:
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.

- **`fffffff005de35dc`**: ordinary USB secondary D+ **below 326 mV returns
  internal type 2**; otherwise it returns internal type 1.
- **`fffffff005de377c`**: the BC item serializer uses the port-type field at
  `+0x2c` as an index into the string-pointer table at **`fffffff006de2df0`**.
- That table points index 1 to **`fffffff00545e09d` = `dcp`**, and index 2 to
  **`fffffff00545e0a1` = `cdp`**. The strings begin at `fffffff00545e099`.
- Therefore **D+ <326 → CDP; D+ ≥326 → DCP**. Linux's numeric values are
  DCP=2 and CDP=3; they are different from the iOS internal values. The Linux
  enum itself was already correct and is unchanged.

This matches the physical results: a wall source's former numeric 3 label
becomes DCP/2, while the Mac port's former numeric 2 label becomes CDP/3 before
USB enumeration. Both analog allowances remain 1500 mA. The passive 39/39 mV
log values are not the secondary result. New `bc_primary_mv` and
`bc_secondary_mv` readouts expose those distinct measurements directly.
The old [charge7](charge7.md) rule is corrected and marked as superseded.

Reproduce the primary evidence capture with:

```sh
python3 -B -I scripts/periph/capture-charge10-re.py
```

Raw decompilation, pointer bytes and strings are in `build/charge10/re/`.
The understood serializer and table were renamed in shared Ghidra; no save or
disk persistence claim is made. The address-based evidence is independently
saved in these raw extracts.

## Current-epoch computer policy

Any positive gadget `connected`, `configured` or `host_grant` snapshot establishes
an external USB computer, regardless of the earlier divider/BC1.2/CC result.
`connected` here is observed bus activity, not merely a bound gadget or VBUS.
A positive notification also latches `computer_seen`, so a short real session
cannot be lost if it ends before the next supplier query. Idle base-budget,
suspend and state notifications alone do not set that latch.

The source then reports **SDP** and clears the active analog allowance. The
existing charger SDP rule uses the generic `usb-input-limit-max` board ceiling,
2400000 for this board, rather than the gadget's separate 100/500 mA accounting.
A sampled CDP does not permanently cap a subsequently enumerated computer.
BC evidence remains available as diagnostics but does not override computer
identity. Explicit per-attach user caps still apply.

The computer result survives USB reset/suspend and transient gadget-query
failures in the same epoch. VBUS removal, a changed ID, or the existing
accessory-phase boundary clears the latch along with all previous source
state. Sampling checks the latch as well as active gadget flags/generation,
so it cannot start a new analog attempt after observed host evidence while
waiting for the next supplier update. Existing role serialization and ADC
completion/poison safeguards remain.

A source that never establishes a USB data session retains the proven BC/CC
policy: 1500 mA BC baseline; qualified CC 3 A bounded to 2400; failed CC refresh
falls back to retained BC. No computer identity is inferred from cable ID,
CC alone, a nonzero idle budget, or suspend. The iOS board-ceiling evidence
remains `fffffff005f15504` and the USBHost item described in
[the charging study](ds-charging-policy.md).

The round9 charger, gauge, gadget-core and public gadget header are inherited
**byte for byte**; the preparation script asserts this against the parent.
Only Tristar and the new ABI documentation change in the kernel commit. No
PMIC write was added: only the existing guarded ADC trigger is written by
Tristar; charger writes remain the established 0xa03/0xa08/0xa00 bit-1 path.
No syscfg/NVRAM/boot-firmware operation or QEMU process control was performed.

## Offline verification

```sh
scripts/build-kernel.sh --run charge 'python3 -B -I /work/scripts/periph/prepare-charge10.py'
scripts/periph/check-charge10.sh
scripts/periph/build-charge10.sh
python3 -B -I scripts/periph/verify-charge10.py
```

Checks run before the build, which cleans generated objects only after packaging.
The config is unchanged from round9, with `CONFIG_DEBUG_INFO_NONE=y`.

- Fresh W=1 builds of Tristar, charger, bq27xxx, role switch and gadget core:
  no warnings/errors. Strict checkpatch: no errors, warnings or checks.
- Replays use all 255 charge9 capture rows plus the retained 226-row charge8
  fixture. The exact BC → ID → enumeration failure is reproduced in old code;
  fixed code reports sticky SDP, and the actual charger policy reaches 2400
  with its calibrated setting. An explicit user cap continues to win.
- Host-precedence matrix covers each earlier analog type and each positive
  evidence flag. Short-session latching, no ADC after host evidence, link PM,
  query failure, source-epoch revocation, and no promotion from idle events
  are exercised. Secondary boundary cases 325/326 mV assert CDP/DCP directly.
- Round9 replays retain the 26 ms attach-default repair, fast-swap epoch change,
  held unplug, first-ID BC retention, CC convergence/fallback, gauge status,
  source/thermal/transport guards, 17 rollback faults, ADC poison, role-lock
  concurrency, accessory/ID-bus regressions and bounded logger output.

All are host-side models with scripted I/O, not hardware charging tests.
The [orchestrator test plan](charge10-test-plan.md) includes the Mac USB-C port,
wall sources, Mac USB-A and source/override regressions. No voltage-triggered
VBUS droop protection or otherwise unobservable sub-poll swap guarantee is
claimed. The old success of round9 on wall sources is not proof of charge10's
hardware behavior.

## Delivered entry

The full two-job kernel build and final package verification passed. Entry
`charge10-a18b0e2ee1` is in `build/charge10/entry/`; kernel commit
`a18b0e2ee18def3a2d6090e9dc8edd73beb3abd8` is on `a9/charge10`, based on
`16a34c8432c61937366964d493b82f387e7d51cc`. The exported patch is in
`patches/charge10/`. No workspace commit or push was made.

| Artifact | SHA-256 |
| --- | --- |
| `build/charge10/m1n1-linux-charge10-a18b0e2ee1.bin` | `9aae761b9991a631a183b79e5499380e7e9a48a678760fb0a93e408e71f64b36` |
| `build/charge10/entry/Image.gz` | `48b527016a2ccca9bb9aec9d82d43291cd8e32f9b48eaf1673256c3dc393046c` |
| `build/charge10/entry/dtb` | `2a047c0b39cb1b46a94efee78198a635cefc9298b9dba2cd00f0591a7a870c6f` |
| `build/charge10/entry/initramfs.gz` | `4681b31dc77dd8050427c4d60073630fb480a1112d94edf68002438ba61755c3` |

`build/charge10/manifest.json` records all 14 checked hashes and
`build/charge10/entry/SHA256SUMS` covers the entry files. The initrd is identical
to charge9; the command line differs only in `linux_a9.entry`, retains exactly
one `pcie_apple_h9p.ans_keep_power=1`, and has no debug shell or logging changes.
Build and verification output is in `build/charge10/build.log`; offline results
are in `check.log`, `w1.log` and `checkpatch.log` in the same directory.
Generated kernel objects were cleaned after packaging; Docker `/src` had
7.2 GiB available at the final check. Hardware charging and boot tests are
**not run**.
