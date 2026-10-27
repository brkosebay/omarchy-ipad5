# Charge8: idle mux recovery and USB-C current advertisement

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Offline implementation on `a9/charge8`, based on `a9/charge`
`a8aa6fea363394f3d21f2eef7525998884b4cf3f`, reusing `/src/wt/charge` in
`linux-a9-src`. Two kernel commits; final commit
`824a8bda2cd69634884d8830224a41aa3ceecd93`. Entry `charge8-824a8bda2c`.
No iPad access, deployment, push or workspace git commit was performed.
Hardware classification/charging and warm-kexec remain **NOT RUN**.

## Root cause and limits of the old evidence

The concrete offline regression is the mux precondition in
`apple_tristar_pin_mv()`: charge7 (and the older aop-wake8 implementation)
accepts only `(DX_CONTROL & 7) == 1` or `(DX_CONTROL & 0x70) == 0x10`.
A completely disconnected mux, **0x00**, returns `-EOPNOTSUPP` before a route
write or ADC trigger. Linux does not select a USB mux nibble on every replug.
Earlier hardware captures actually show `01=00`, and the usb-host5 role path
leaves that register alone ([source comparison](ds-usb-host5.md), §3).
The new harness executes the **actual charge7 pin function** with route 0 and
reproduces that failure, then exercises the fixed implementation in both
orientations. Earlier successful sampling only proves the old precondition
was satisfied on that boot/route; it does not make it valid after every replug.

The release2 log puts the first warning at **467.923556**, only **0.823 ms**
after the attach message at 467.922733. That is before the 5 ms routing settle
and before a 20 ms idle timeout or 1 s completion timeout could elapse.
Together with the known zero mux, this strongly points to the pre-ADC route
rejection. **The historical errno/register were not logged, so the exact live
failure cannot be proved from that capture.** A route-read I/O error or an
intervening USB event can also fail early. The new diagnostics make that
remaining hardware question directly observable instead of guessing.

Several alternatives can already be separated by code:

- Role callback refusal (`-EBUSY`/`-ENODEV`) cannot print that warning: the warning
  is inside the callback's classify path. The old poller discarded the callback
  result, so it supplied no separate evidence about role refusals.
- A trigger-write or post-trigger completion failure latches `adc_poisoned`.
  Later classify calls return before printing. Thus a permanently poisoned ADC
  cannot explain a warning on **every** wall-charger tick in charge7.
- A pre-trigger idle-latch timeout does not poison and could repeat; it needs
  `adc-idle`/`adc_status` evidence. It does not fit the sub-millisecond first
  warning. D+/D− range failures happen only after successful pin conversions.
- Charge7's strict VBUS and several generation-change exits return silently;
  those exits alone do not print the warning. Pin-level cancellation can print
  it. The new code records all these stages and refuses uncertain results.

Charge7 set `classify_after = now + 1000 ms` on every failure and deliberately
left `brick_sampled=false` until success. There was no exponential delay or
terminal per-attach route failure. A printk rate-limit bucket can refill while
one warning per second continues, so `dev_warn_ratelimited` alone did not bound
that steady-state spam.

## iOS USB-C evidence

Target: iOS 16.7.10 J71t kernelcache, SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
All addresses are unslid virtual addresses. Repeat the capture with
`python3 -B scripts/periph/capture-charge8-re.py`; raw JSON and bounded Capstone
instructions are in `build/charge8/re/`. The undecorated function at 5fe52dc
has no shared Ghidra function definition, so its bytes were read with Ghidra
and decoded, rather than trusting a guessed decompiler entry.

| Address | Verified meaning |
| --- | --- |
| `fffffff005e34408` | Digital-ID field decode: byte 2 bit 3 is **CC capability**, not an Rp current code. `10 09 08` has USB=2 and CC=1; `10 0c 00` has no CC flag. |
| `fffffff005e4d9ac` | Transport decode produces `usb0` and `usbc` for the C→Lightning cable. It does not itself change hardware mux registers. |
| `fffffff005e4d028` | Reorders the two transport fields according to Lightning orientation. |
| `fffffff005e23a90` | `IOAccessoryUSBTypeCCurrentDetect` samples CC using pin selector **2 for orientation 1**, **0 for orientation 2**; converts positive Q16 volts to integer mV; creates a Type-C source item and debounces changes. |
| `fffffff005e50e0c` | Command-gated pin-voltage wrapper; dispatches manager vtable `+0x7c8`. Built-in vtable `fffffff006e52150 + 0x7c8` points at **5fe52dc**. |
| `fffffff005fe52dc` / table `fffffff0054cbd50` | Selector 2 maps to IC selector 3, selector 0 to IC selector 2; calls IC `+0x628`. This is **D+ on the opposite Lightning lane pair**, not an ID-bus query. |
| `fffffff005fee258` | CBTL1610 IC `+0x628`: selectors 2..5 delegate to the direct DX lane monitor. Selectors 0/1 instead address ACC pins, so labelling this CC path an ACC-pin measurement would be wrong. |
| `fffffff005ff5ed0` | Direct DX monitor accepts a zero cached mux, substitutes low `0x0c` or high `0xc0` for D+, waits 5 ms, calls port `+0x830`, restores the cached mux. |
| `fffffff005febe00` | Built-in port `+0x830` calls `function-brick_id_voltage`, the existing J71t PMIC ADC input. CC is a pin voltage, **not VBUS**. |
| `fffffff005de4024` | Type-C current item: **≤659 mV → 0; 660..1229 → 1500 mA; ≥1230 → 3000 mA**. |
| `fffffff005de3f78` | Stores the advertisement and CC voltage; publishes the current limit in the Type-C source item. |
| `fffffff005e236c0`, `fffffff005de8328` | The detector initialization stores a qualification argument at `+0xd0`; charger detection supplies its charging-port type. `5e23a90` clamps a 3000 mA item to **1500** when that argument is zero. |
| `fffffff005f15504` | Generic `usb-input-limit-max` board ceiling, documented in charge7; J71t ADT supplies 2400 mA. |

The sample routine retries a changed advertisement after about 10 ms and uses
a 20 ms matching window before publishing; its normal timer is about 30 ms.
Linux deliberately uses two equal advertisements separated by **at least 20 ms**,
then refreshes every five seconds while idle. This is a slower conservative
adaptation of the debounce, not a reproduction of iOS timer cadence. The code's
20 ms comment describes the Linux guard. No byte of `10 09 08` is treated as
1.5/3 A, and no fixed 2400 mA cable allowance or USB-PD negotiation is added.

Four newly understood functions were renamed in shared Ghidra and a comment
was added to the pin-selector dispatch. Saving was deferred by the server's
60-second-idle guard (exit 75); disk persistence of the annotations is not
claimed. Raw address-based evidence is captured independently.

## Implementation and ownership

The ordinary divider/BC monitor selects the pair from a fresh strict
VBUS/present/orientation snapshot. A selected nibble may be idle (0) or the
already understood USB setting (`nibble & 7 == 1`); other accessory routing is
refused. It preserves the other nibble and restores the exact original byte,
including zero. CC on the verified six-byte `10 09 08 00 00 00` cable uses D+
on the opposite pair, without a DCP pulse on the CC lane.

Every operation remains inside `apple_s8000_usb_role_monitor()`'s role hardware
mutex, then the Tristar mutex. Host role, teardown and role transitions exclude
sampling. Connected/configured/host-granted gadgets exclude sampling; the USB
event generation is checked around conversions, and an intervening event
invalidates the result. No gadget descriptors, PHY setup or role implementation
was changed. This preserves charge7's event/snapshot guard; offline tests do
not establish physical timing guarantees against a host beginning bus activity
between observations. Tether continuity and cancellation need hardware tests.

Route repair was removed from the unguarded power-supply getter. It now runs
only in the role-serialized monitor, including quiesce. A failed restore keeps
`-EUCLEAN` and the existing host-entry inhibit. Quiesce without a pending route
repair performs no monitor call, so a normal host-role shutdown is not reported
as a spurious repair failure. DCP control is cleared before restoring DX.

CC and passive/BC detection are combined conservatively. A 3 A CC advertisement
requires a completed BC1.2 DCP/CDP classification; otherwise it is capped at
1.5 A. A detected Apple divider remains an independent source entitlement.
The charger accepts USB type C, caps all supplier allowances by the generic
board maximum, and uses only the existing calibrated ladder: 3 A CC → 2.4 A
maximum; 1.5 A → nominal 1 A (usual live value 962500 µA). Successful Type-C
samples are refreshed while idle; an uncertain refresh removes the old CC
allowance. Enumerated Mac sources retain the generic board-ceiling SDP policy.

Failures back off at 1/2/4/8/16/32/60 seconds, capped at 60 seconds; warnings are
limited to one per minute per source epoch, including intermittent failures.
A physical detach resets retry state, not ADC poison. The low-before-trigger
completion guard and terminal poison after uncertain completion are unchanged.
New `charger_detect:` diagnostics in `lightning_state` expose pin/stage/errno,
role callback result, attempts/failures/backoff, route, last ADC status and
sampled voltages. `poison_stage`/`poison_attach` preserve the fault origin even
after later detach. The existing `charge7_state` ABI and explicit sysfs cap /
zero-disable semantics remain.

No PMIC control write was added. Tristar still has exactly one PMIC write:
the existing guarded `0x4000=1` ADC trigger. Charger writes remain `0xa03`,
`0xa08`, and RMW of `0xa00[1]`. No syscfg/NVRAM/boot-firmware operation exists
in this work, and no QEMU process was touched.

The rootfs compatibility service now executes `/usr/bin/true`; its udev rule
has no actions and new rootfs builds no longer enable it. The initramfs helper
is also a no-op. Packaging retires the request in all four existing `/init`
archive records, preserving every other file's contents and metadata. The
**installed live rootfs still needs the orchestrator migration** in the
[test plan](charge8-test-plan.md); changing workspace files does not update it.

## Offline verification and reproduction

```sh
scripts/build-kernel.sh --run charge 'python3 -B -I /work/scripts/periph/prepare-charge8.py'
scripts/periph/check-charge8.sh
scripts/periph/build-charge8.sh
python3 -B -I scripts/periph/verify-charge8.py
```

Run checks before the full build, since the build script cleans generated
kernel objects after packaging to keep Docker disk usage bounded. `.config`
is byte-identical to charge7/USB-host5b and has `CONFIG_DEBUG_INFO_NONE=y`.

- Fresh `W=1`: both charging drivers, role switch and gadget core; zero warnings
  or errors. Full Image.gz and DTB build is recorded in `build/charge8/build.log`.
- Strict checkpatch: 0 errors, 0 warnings, 0 checks; `git diff --check` passes.
- ASan/UBSan detector harness: actual old zero-mux regression; both orientations,
  divider 3×3 table, BC pulses/threshold, CC edge/debounce/refresh/revocation,
  unqualified 3 A cap, unsafe mux refusal, strict VBUS/orientation/range rejection,
  exponential retry/log cadence, every ADC cancellation, restore/quiesce repair,
  detach/epoch/ACA reset, sticky computer and pthread role-lock exclusion.
- Actual ADC function with scripted PMIC: idle timeout/error performs no trigger;
  trigger/completion faults poison; a late completion cannot trigger another
  conversion; invalid result/bulk error and fresh sequential conversions.
- ASan/UBSan charger harness: 30 source/ceiling cases, Type-C board cap/1 A rung,
  Mac/wall/fast-replug/ACA transitions, per-attach override, 60 s setpoint backoff,
  thermal/gauge/transport guards and 17 transaction/rollback fault points.
- Existing accessory policy (73 checks) and ID-bus core regressions pass.
- Package verification: manifest/entry hashes, exact command line apart from
  entry name, one keep-power token, no debug shell, and initramfs change scope.

No voltage-triggered droop protection or guarantee for sub-poll VBUS loss is
claimed; both remain unresolved from charge7. No hardware charging outcome is
inferred from the mocked I/O tests.

## Delivered artifacts

Payload: `build/charge8/m1n1-linux-charge8-824a8bda2c.bin`. SHA256:
`3a3abccb542a6dfe43a7b8a4dd09f4aebafdf650c7dd0e19949c286147d13e29`.

Entry: `build/charge8/entry/`; complete hash list in
`build/charge8/manifest.json` and `build/charge8/entry/SHA256SUMS`.

- `Image.gz`: `6f06493d418dc343161eb401a702b930454aeface63eb1c4b7216c20949f74dc`
- `dtb`: `2a047c0b39cb1b46a94efee78198a635cefc9298b9dba2cd00f0591a7a870c6f`
- `initramfs.gz`: `4681b31dc77dd8050427c4d60073630fb480a1112d94edf68002438ba61755c3`

The kernel worktree is clean; `a9/charge` and `a9/release` are unchanged.
All four delivered board DTBs retain the 2400000 µA ceiling. Generated
objects were cleaned after packaging; Docker reported 6.6 GiB free, the same
as the initial check in this session. Only the intermediate charge8 payload
from the earlier development commit was removed.
