# Display D1/D2 — orchestrator test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Implementation agent: **no device access, no workspace commits, no pushes**.
Kernel branch `a9/display`, rooted at `hoolock` (`6831bc701a6c`). Sources and
DT in `patches/display/round1/`; configuration `patches/display/display.config`.
Study: [D0 resolved](ds-display-kms.md#12-d0-resolved-the-latch-the-vbi-irq-and-the-flip-completion-register).
These experiments retain simpledrm and the iBoot mode throughout.

Offline result: branch tip `95149d28070371295295cf3fd2da0739c479a41a`
(binding `42eb27f786`, driver `24b83cd330`, DT `95149d2807`). The full
arm64 LLVM `W=1` Image/DTB/module build passed; the new driver had no compiler
warnings. The initial full build reported existing warnings elsewhere.
ASan/UBSan scratch-boundary tests, binding/example/DTB schema validation,
strict checkpatch for all three patches, patch-to-tree reproduction and
SHA256 verification passed. Checkpatch ignores only `FILE_PATH_CHANGES`,
as the existing workstream exporters do. Optional `yamllint` was unavailable;
the schema validators ran. Pairwise `git merge-tree` checks passed against
each requested stack ref, without creating another branch or merge commit.
The full integration build and all device procedures below remain for the
orchestrator. Build outputs and detailed logs: `build/kernel-display/`.

## Contract and evidence

- Phase A performs reads only. It maps all six original disp0 ranges,
  including `0x206200000`, and selects **exactly one** window with the known
  format at `+0x402c` and an advancing 16-bit counter at `+0x1034`.
- Geometry must match the live simple-framebuffer: 1536×2048, stride 6144,
  one enabled plane, matching low 32 address bits. Both size-field orders
  are logged/accepted because §5.1 and §12.4 disagree on their order.
  A 32-bit register cannot contain `0x87c95c000`: the raw word and the
  firmware CPU address are reported separately. Matching low bits alone
  **does not prove translation or bypass**.
- Polling samples `0x102c` and `0x1034` every approximately 1 ms for 1 s;
  reports frame deltas, changes, VBI samples, packet samples and status OR.
  Reading the status never acknowledges it. Sticky bit `0x8` is recorded,
  not presented as proof that this direct flip completed.
- Optional phase B requests **one** DT interrupt index (0..4) exclusively
  for 1 s. Index 0 is the AIC-140 candidate. No loop tries other lines.
  It acknowledges exactly the raw observed status at `ADP+0x102c`, including
  error bits, and disables its Linux IRQ at 1024 invocations. It never writes
  the hardware mask at `0x1028`. A silent candidate with the inherited mask
  is inconclusive; do not enable another mask bit to make it fire.
- D2 uses reserved existing VRAM, not a new DMA allocation. It saves all
  scratch contents, paints four bars and a black corner marker, arms a
  10 s delayed restore before the first scratch mutation, then performs one
  experimental `writel` to `0x4030`, preserving the `0x2a` encoding.
  It samples status/frame/base for 200 ms and immediately reverts on a
  readback mismatch, frozen frame counter or fatal status (`0xa42`).
- The exact original base word is restored after 10 s, on explicit `flip=0`,
  every failure after mutation, suspend, reboot/kexec, driver shutdown and
  module removal. Scratch bytes are restored after a 100 ms scanout drain.
  Failed restore readbacks remain pending for another teardown attempt.
  Only one experimental flip is allowed per bind; repeated `flip=1` returns
  `EALREADY`. All lifecycle paths serialize with debugfs and delayed work.
- No writes to DP/PHY, CLCD timings `0x50..0x60`, FIFO, format, stride,
  layer enable, clock/reset/PMGR controls, DART registers/tables, or `0x1028`.
  No power-domain/IOMMU consumer is attached to the observer node.
  `ps_disp0` and `ps_dp` keep `apple,always-on`, including standalone builds.

The **already-pulled** live iOS ADT, `build/firmware/private/IODeviceTree.txt`
(`/vram`, around line 490), gives CPU address `0x87c95c000`, length
`0x2400000` (36 MiB). The j71t-only DTS reserves that range with `no-map`.
The first 12 MiB is the current framebuffer; the default scratch is the
next 12 MiB at CPU `0x87d55c000`. D2 requires the running framebuffer to be
inside the reservation and checks all range/alignment/32-bit-wrap bounds.
Other boards are deliberately not enabled.

**Reservation is not DART evidence.** D2 defaults to disabled
(`mapped_bytes=0`). Before supplying a nonzero value, establish from a
read-only inspection of the *current boot's* display DART mapping/bypass
that the interval starting at the framebuffer CPU address maps affinely to
the bus address in `0x4030`, for at least 24 MiB. Save that evidence with the
test logs. The historical iOS `/vram` length, a reservation in `/proc/iomem`,
and a matching base register do not establish that coverage. This round
does not implement a DART page-table decoder; if coverage is unknown, stop
at D1. Never change translation mode, tables, or mapping to pass this gate.

## 1. Offline preparation on the Mac

Repeat source installation/build in the isolated Docker worktree:

```sh
scripts/build-kernel.sh --run display 'python3 /work/scripts/display/install-round1.py'
scripts/display/build-round1.sh > build/kernel-display/build.log 2>&1
scripts/display/check-round1.sh > build/kernel-display/check.log 2>&1
cat build/kernel-display/COMMIT build/kernel-display/RELEASE
test ! -s build/kernel-display/DIRTY
shasum -a 256 build/kernel-display/Image.gz build/kernel-display/apple_adbe_probe.ko
```

The build is `LLVM=1 ARCH=arm64 W=1 Image.gz dtbs modules` with debug info off.
The check script runs the driver's actual scratch arithmetic under ASan/UBSan
and validates the new binding/DTBs with the existing local dtschema installation.
Export after committing only the kernel branch:

```sh
scripts/display/export-round1.sh
```

The export script intentionally refuses a nonempty patch directory. Preserve
an existing export before re-exporting; `series` and `SHA256SUMS` accompany it.

## 2. Integration build and warm boot — orchestrator only

Do not boot the standalone display kernel as the normal Omarchy kernel: it
does not include the other peripheral/storage/boot workstreams. Merge the
requested stack, then build its module against that exact integration kernel:

```sh
INTEGRATION_TREE=dispint scripts/integrate.sh a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install a9/power a9/idle a9/boot-pin a9/display
TREE=dispint scripts/display/build-round1.sh > build/kernel-dispint/display-build.log 2>&1
cat build/kernel-dispint/COMMIT build/kernel-dispint/RELEASE
test ! -s build/kernel-dispint/DIRTY
TREE=dispint scripts/boot/stage-kernel.sh display-d1-d2
```

`integrate.sh` applies the display fragment only for `a9/display*`. It builds
Image/DTBs; the second build also produces the matching `.ko`. Never load
the standalone `.ko` into an integration kernel. Leave the known working
boot entry/default available. Warm kexec into the staged entry:

```sh
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -r'
scripts/omarchy/ssh.sh 'sudo -n sh -c "echo 120 > /sys/module/apple_boot_watchdog/parameters/timeout"; sudo -n linux-a9-kexec load display-d1-d2'
scripts/omarchy/ssh.sh 'sudo -n systemd-run --unit=display-d1-d2-kexec --on-active=2s /usr/bin/systemctl kexec'
# After SSH returns: new boot_id, expected entry, both CPUs and usable root.
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -r; cat /proc/cmdline; nproc; findmnt /'
```

Install only the matching module over the existing Linux SSH transport:

```sh
cat build/kernel-dispint/apple_adbe_probe.ko | scripts/omarchy/ssh.sh 'cat > /tmp/apple_adbe_probe.ko'
scripts/omarchy/ssh.sh 'sudo -n mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || mountpoint -q /sys/kernel/debug'
```

All commands below are for a root shell on the **iPad**, opened by the
orchestrator with `scripts/omarchy/ssh.sh`. Keep independent SSH recovery and
a camera available. Do not suspend the tablet during an observation.

## 3. D1 phase A — first, with both experiments disabled

```sh
insmod /tmp/apple_adbe_probe.ko
dmesg | grep -E 'apple-adbe-probe|simpledrm|genpd'
D=/sys/kernel/debug/206200000.display-subsystem
cat "$D/state"
cat "$D/regs"
grep -E 'simpledrm|apple-adbe-probe' /proc/interrupts || true
readlink /sys/bus/platform/devices/87c95c000.framebuffer/driver
```

If the debugfs path differs, use the exact path printed by the driver's
`ready` message. Save the complete logs, not only filtered extracts.
Expect one discriminator match; correct mode/stride/base; approximately
60 frame increments in the one-second sample (record the measured rate);
no display/brightness change, no new genpd warnings, simpledrm still bound,
and no `apple-adbe-probe` IRQ registration after probe. The driver does not
register any `/dev/dri` device or remove an aperture.

Negative gate, still without an enabled D2 mapping:

```sh
echo 1 > "$D/flip"                 # expected: EACCES; no mutation
cat "$D/state"                     # used=0 flipped=0
rmmod apple_adbe_probe
```

Stop on a mismatch. Do not substitute a guessed ADP window or plane address.
The first probe prints all window discriminator samples even when it refuses
to bind, so retain those dmesg lines.

## 4. D1 optional phase B — one candidate on one load

Only after phase A passes:

```sh
insmod /tmp/apple_adbe_probe.ko irq_index=0
cat "$D/state"
dmesg | tail -180
grep apple-adbe-probe /proc/interrupts || true
rmmod apple_adbe_probe
```

The probe blocks for the additional one-second test, then frees the IRQ.
Read the logged Linux IRQ separately from AIC number 140. Expect approximately
the frame count if iBoot's inherited mask enables VBI; zero is inconclusive.
The raw status OR should contain bit `0x1` if this is the VBI source.
`guard=tripped`, any error bits, or a rapidly repeating line is a stop signal.
Do not sweep all candidates automatically. If a later investigation calls for
another index, perform one separately logged load with that index and keep
`mapped_bytes=0`.

## 5. D2 — one direct flip, only with current mapping evidence

After independent read-only mapping proof described above, and only if at
least `0x1800000` (24 MiB) from CPU `0x87c95c000` is already covered:

```sh
insmod /tmp/apple_adbe_probe.ko mapped_bytes=0x1800000 scratch_offset=0xc00000
cat "$D/state"
cat "$D/regs" > /tmp/adbe-before.regs
echo 1 > "$D/flip"
cat "$D/state"
sleep 11
cat "$D/state"
cat "$D/regs" > /tmp/adbe-after.regs
diff -u /tmp/adbe-before.regs /tmp/adbe-after.regs || true
dmesg | tail -220
```

Camera: note bars visible or absent, immediate or frame-boundary change, any
tear line, and restoration of the live desktop. Software cannot infer tearing.
Expect test word `0x7d55c02a` **only if** the original word was `0x7c95c02a`.
The code computes it from the saved bus word, checks low flags equal `0x2a`,
and rejects wrapping, overlap, tiling/planar flags, or insufficient coverage.
After the deadline: `used=1 flipped=0 restored=1`; the full saved base word
must match. Other plane words and the interrupt mask must match exactly;
frame counters/status may naturally advance or remain sticky.

```sh
echo 1 > "$D/flip"                 # expected EALREADY, no second store
echo 0 > "$D/flip"                 # idempotent explicit restore
rmmod apple_adbe_probe
```

Each further trigger requires a fresh load and renewed verification that
the same mapping evidence remains applicable. For an early explicit restore,
repeat the proven load, trigger once, then immediately `echo 0 > "$D/flip"`.
For unload regression, repeat the load/trigger and immediately
`rmmod apple_adbe_probe`; the desktop must return without waiting 10 s.
Reboot/kexec and suspend hooks use the same serialized restore path; exercise
them only after the ordinary timer and unload tests pass, using the known
working entry for reboot. Do not combine first-flip discovery with a reboot.

Interpretation: bars with tearing favors a FIFO implementation; bars without
tearing is evidence for a frame-boundary latch; unchanged image with a new
base readback suggests direct writes do not commit. A stale packet-synced
bit is not new completion evidence. Do not add a FIFO kick in this round.
If restore readback differs or the panel remains wrong, preserve logs and
recover using the known working warm entry (or tethered boot if necessary).
RAM scratch remains reserved even after unload; no newly allocated DMA pages
can become scanout after being freed.

## 6. Evidence to return

Save full dmesg, state/regs before/during/after, the current mapping proof,
the exact module parameters and build COMMIT/RELEASE/hashes, observed window,
raw base plus CPU framebuffer address, measured frame rate, candidate IRQ
counts/status, and a camera recording of flip/revert behavior. Distinguish
offline validation from device results; no device result is claimed by this
implementation round.

---

# Round 2: FIFO measurement (phases C/D) and the KMS driver

Kernel branch `a9/display`, tip **`9d230dad6bb824aa7d78455e99f3a32e4f30cbef`**
(binding `88e87c4b17`, probe + measurements `d3f6f56870`, KMS driver
`9d230dad6b`), on top of round 1 (`95149d2807`). Patches:
`patches/display/round2/`. Study: [§13](ds-display-kms.md#13-d3-the-adp-parameter-fifo-protocol).

What changed for the orchestrator:

- Both modules now live in `drivers/gpu/drm/adbe/`: `apple_adbe_probe.ko`
  (measurements) and `apple_adbe_drm.ko` (KMS). Both bind the same
  `adbe0,s8000` node, so **only one may be loaded at a time**; always
  `rmmod apple_adbe_probe` before `insmod apple_adbe_drm.ko`.
- Round 1's debugfs `flip`/`mapped_bytes` experiment is gone. Its `0x2a`
  tag gate rejected the live plane word `0x7c95c000`, so it could not run;
  phase D with `packet=sync` covers the direct store. Phase A is unchanged
  and is still the default.
- **No DTS change** (`git diff 95149d2807 9d230dad6b -- arch/` is empty).
  The Image does change: `display.config` now sets `CONFIG_DRM_APPLE_ADBE=m`
  and `CONFIG_CMA_SIZE_MBYTES=128` (it was 16, which is too small for a
  12 MiB fbdev buffer plus compositor buffers).
- Round 2 writes to the live pipe, on purpose and only when a parameter
  asks: `0x102c` W1C acks, the FIFO port `0x10c0`, `0x1028` (only with
  `irq_mask`, or while KMS vblank is on; the exact boot word `0x2` is
  restored), and `0x4030`/`0x4038` (`packet=sync` and the restore fallback).
  It still never writes DP/PHY, CLCD timings, clocks, PMGR or DART
  registers or tables. The DART is only read: TCR `+0xc` and TTBRs `+0x40`
  of `0x206304000`/`0x206300000`, plus page-table walks of translating
  streams.

> Private graphics-dependent excerpt omitted from this source snapshot.

## R2.1 Build and boot (orchestrator)

```sh
cp build/kernel-nextint/dtbs/s8003-j71t.dtb /tmp/j71t-before-r2.dtb
INTEGRATION_TREE=nextint scripts/integrate.sh a9/periph a9/pcie 97313d95c3 a9/touch a9/audio a9/nvme-install a9/power a9/idle a9/boot a9/vdec a9/display a9/aop
TREE=nextint scripts/display/build-round2.sh > build/kernel-nextint/display-r2-build.log 2>&1
cat build/kernel-nextint/COMMIT build/kernel-nextint/RELEASE; test ! -s build/kernel-nextint/DIRTY
cmp /tmp/j71t-before-r2.dtb build/kernel-nextint/dtbs/s8003-j71t.dtb && echo "DTB unchanged: warm kexec OK"
TREE=nextint scripts/boot/stage-kernel.sh display-r2
```

(The branch list is the a9/nextint `92b11076f3` merge order. If other
branches moved, use the current stack.) If `cmp` differs, another branch
changed the DT: cold boot. Otherwise warm kexec as in §2 with the
`display-r2` entry. After boot:

```sh
cat build/kernel-nextint/apple_adbe_probe.ko | scripts/omarchy/ssh.sh 'cat > /tmp/apple_adbe_probe.ko'
cat build/kernel-nextint/apple_adbe_drm.ko | scripts/omarchy/ssh.sh 'cat > /tmp/apple_adbe_drm.ko'
scripts/omarchy/ssh.sh 'uname -r; modinfo -F vermagic /tmp/apple_adbe_*.ko; grep Cma /proc/meminfo; sudo -n mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; true'
```

Gate: vermagic starts with `uname -r` and `CmaTotal` is 131072 kB.
The commands below run as root on the iPad. `D=/sys/kernel/debug/206200000.display-subsystem`.
After every step save `dmesg | grep apple-adbe` and `cat $D/state`, then
`rmmod apple_adbe_probe`. Run the steps in order, and stop at the first
FAIL. Keep a camera on the panel from R2.4 onward.

## R2.2 Phase A regression and read-only DART snapshot

```sh
insmod /tmp/apple_adbe_probe.ko dart=1
dmesg | grep apple-adbe | tail -40; cat $D/state; rmmod apple_adbe_probe
```

PASS: same discriminator, mode and ~59 frames/s as round 1 (`mode 1536x2048
stride=6144 bus-offset=0x800000000`), no visible change, and then:

- `DART win0 ... tcr=...`, `DART win1 ... tcr=...`, and per stream
  `translate=0|1 ttbr=...` (record all of them: this is the measurement);
- one of these lines:
  `DART: no translating stream maps the scanout; bypass, bus 0x7c95c000 linear`
  or `DART winN sidM maps the live scanout; X of 0x2400000 bytes ... linear`;
- `DART: boot VRAM from bus 0x7c95c000: 37748736 of 37748736 bytes verified linear`.

Bypass means the KMS driver can work (R2.6). A translating scanout stream
means phase D with `scratch=vram` still works if all 36 MiB verify, but
`apple_adbe_drm` will refuse to bind: report the TTBR values and stop
before R2.6. If reading the DART registers faults (an SError or panic),
reboot to the known entry and report it. Neither the KMS driver nor
phase D can run in that case.

## R2.3 VBI interrupt (phase B with the mask bit)

```sh
insmod /tmp/apple_adbe_probe.ko irq_index=0 irq_mask=0x9
dmesg | grep 'phase B'; rmmod apple_adbe_probe
```

PASS: `phase B index=0 ... hits=H vbi=V synced=0 frames=F ... guard=no mask 0x2->0xb->0x2`,
with V within ±3 of F (≈59) and H ≈ V. The final `0x2` is the restored
exact word. FAIL: V=0 (index 0 is not VBI, so try one load with
`irq_index=1 irq_mask=0x9` and report; KMS needs index 0), `guard=tripped`,
or a mask that is not restored. Without `irq_mask` you get round 1's
inconclusive result: iBoot enables only `0x2`.

## R2.4 Phase C: one FIFO kick that re-sends the live plane

```sh
insmod /tmp/apple_adbe_probe.ko phase=c
dmesg | grep -E 'C re-send|phase C'; cat $D/state; rmmod apple_adbe_probe
```

PASS: `C re-send: packet=base words=3 base 0x7c95c000; ParmSt A->B 0x1040 P->Q; burst busy=.. max=..; poll N samples 3 frames ptr-busy=.. ParmSt-seen=.. synced=1 first=..us abort=0 errors=0 ...; readback 0x7c95c000; result 0`,
then `phase C done`, and **no visible change** on camera. A, B, P, Q,
ParmSt-seen, ptr-busy, `first` and the `t=` trace in `$D/state` are the
§13.9 measurements. Record them whatever their values. `synced=1` is the
"one kick = one 0x8" answer, and `synced=0` with `result -110` means no
packet-synced event. FAIL/stop: `FIFO not ready ... not kicked` (busy FIFO,
nothing was written), `errors≠0`, or any visible change.

## R2.5 Phase D: flip to a second buffer and back

```sh
insmod /tmp/apple_adbe_probe.ko phase=d
dmesg | grep -E 'DART|phase D|D flip|D revert|restore'; cat $D/state; rmmod apple_adbe_probe
```

PASS log: `phase D: vram buffer CPU 0x87d55c000 bus 0x7d55c000, flip 0x7c95c000 -> 0x7d55c000, hold 2000 ms`,
then `D flip: ... synced=1 ... errors=0 ...; readback 0x7d55c000; result 0`,
then `phase D hold: frames≈118 ... base=0x7d55c000`, then `D revert: ... synced=1 ...; readback 0x7c95c000; result 0`,
and finally `phase D done: flip result 0, revert confirmed`.
PASS camera: the desktop (frozen copy) with a **magenta band 256 rows tall
across the middle** for ~2 s, appearing whole in one frame (no partial
band = no tear), then the live desktop again.

If the band does not appear (readback changed, picture did not), run the
full layer set, iOS's form (§13.1 item 7): first `phase=c packet=full`
(PASS as R2.4 with `words=33`), then `phase=d packet=full`. If that does
not move the picture either, try `phase=c packet=sync` then `phase=d
packet=sync` (direct store + empty armed packet, the upstream ADP driver's
method). Record which `packet=` moved the picture. The KMS driver takes the
same value as `commit=`.

Then prove that ordinary DMA (CMA) memory is scannable, which the KMS
driver depends on, with the `packet=` that worked:

```sh
insmod /tmp/apple_adbe_probe.ko phase=d scratch=dma [packet=...]
```

PASS: as above, with `dma buffer CPU 0x8xxxxxxxx bus 0x...`, the band
visible and the revert confirmed. Optional: `tag=42` (= `0x2a`, iOS's low
bits) if the default (the live bits, `0`) misbehaves.

Fallback when the screen goes wrong: the probe reverts by itself, early
on error bits or a stalled frame counter. If the FIFO revert is not
confirmed, it stores the boot base directly and logs `restore by direct
store`. `rmmod apple_adbe_probe` retries that restore (and so do reboot,
kexec and suspend). Should anything stay wrong, reboot to the known entry:
simpledrm is untouched by the probe, so a reboot restores it. A buffer
that might still be scanned is never freed (`buffer intentionally
leaked`).

## R2.6 KMS driver

Preconditions: R2.2 says bypass, R2.3 passed on index 0, and R2.5 moved
the picture with `scratch=dma`. Deploy the updated
`scripts/omarchy/rootfs/hypr/uwsm-env-ipad` with `scripts/omarchy/hot-apply.sh`
(it installs it as `~/.config/uwsm/env.d/90-ipad`). The new file picks the
`platform-*display-subsystem-card` node; alternatively set `AQ_DRM_DEVICES`
by hand. Then:

```sh
systemctl stop sddm                      # Hyprland would lose simpledrm under it
rmmod apple_adbe_probe 2>/dev/null
insmod /tmp/apple_adbe_drm.ko commit=base   # or the packet= value that worked in R2.5
dmesg | grep -E 'apple-adbe|adbe|simpledrm|fbcon|\[drm\]' | tail -30
ls -l /dev/dri/by-path/; cat /sys/kernel/debug/dri/*/name
A=$(grep -l adbe /sys/kernel/debug/dri/*/name | xargs dirname)/adbe_state; cat $A
```

PASS:

- `adopting 1536x2048 at 0x87c95c000 (plane 0x7c95c000, stride 0x1800, bus offset 0x800000000, DART bypass, inten 0x2)`,
  then `[drm] Initialized adbe 1.0.0 ... for 206200000.display-subsystem`,
  with simpledrm gone and fbcon on the new framebuffer (the console is
  visible and redrawn);
- `$A`: `kicks≥1 synced≈kicks busy=0 vbl-fallbacks=0 timeouts=0 aborts=0 errors=0`,
  and `base` is no longer `0x7c95c000`;
- no `pipe error bits`, `FIFO busy`, `flip not acknowledged` or
  `park packet not confirmed` lines.

```sh
systemctl start sddm                     # Hyprland on the adbe card
grep 206200000 /proc/interrupts; sleep 5; grep 206200000 /proc/interrupts; cat $A
```

PASS: the desktop works and the interrupt count rises by ≈300 in 5 s while
something animates (vblank ≈60 Hz). `synced` follows `kicks`, and
`timeouts`/`vbl-fallbacks` stay 0. Scroll a page: there must be no tear
line. As the desktop user, `hyprctl monitors` shows **`eDP-1`** where
simpledrm showed `Unknown-1`. The iPad monitor rule matches any output
(`output = ""`), so the scale and transform must be as before. DPMS:
`hyprctl dispatch dpms off` makes the panel black (the backlight is
unchanged), and `$A` shows `base=0x7c95c000` (parked on the boot buffer,
now cleared to black). `hyprctl dispatch dpms on` brings the desktop back.

Not failures this round: slow first frames, or a slower software desktop
than on simpledrm. The dumb buffers are write-combined DMA memory, and
llvmpipe/kms_swrast reads back from them. This round measures correctness
only.

Unload regression: `systemctl stop sddm; fuser -v /dev/dri/card*` (nothing
may still hold the adbe card, or rmmod returns `EBUSY`), then `rmmod
apple_adbe_drm`. PASS: the panel turns black (parked), `dmesg` is clean and
SSH is fine. There is no console after unload, and a reboot or kexec
brings simpledrm back.

Fallbacks: if `insmod` refuses (`DART ... translates`, `plane ... is not
the boot framebuffer`, `frame counter is not advancing`), nothing changed
and simpledrm is still bound, because the aperture is taken only after
every check. If the panel shows garbage or freezes after insmod,
`rmmod apple_adbe_drm` (it parks on the black boot buffer, with the mask
restored) and then reboot to the known entry. If `timeouts` grows, flips
complete by timer, not by hardware: report `$A` and the dmesg.
Kexec/shutdown leave the pipe on the boot buffer, so the next kernel's
simpledrm works.

## R2.7 Evidence to return

Full dmesg for every load, `$D/state` for R2.2–R2.5 (including the `t=`
traces), `$A` before and after Hyprland, the DART TCR/TTBR lines, the
`packet=` that moved the picture, camera clips of R2.5 and of DPMS off/on,
and `/proc/interrupts` deltas. Keep offline results separate from device
results: this round makes no device claim.

---

# Round 2b: rebindable probe, frame-number tokens, 0x400 completions

Kernel branch `a9/display`, tip **`9d776859c0dfb28c6a59458c2ba47186cc027199`**
(one commit on top of round 2's `9d230dad6b`). Patches: `patches/display/round2/`
(four now). **No DTS change**: the DTB is still identical to round 1's. The
Image changes because `display.config` now also sets `CONFIG_MODULE_UNLOAD=y`.
That changes the module ABI, so load only `.ko` files from the same build.

## Diagnosis of the R2.6 numbers (nextint4, `display-r2-6-kms.txt`)

- **The frame counter reset was ours.** The preamble's low 16 bits (the
  "token", §13.2, hardware use unknown until now) are loaded into the frame
  counter `0x1034`. Round 2 sent a serial 1, 2, 3...: after kick 2 the
  counter read 663 = 2 + 661 frames (11 s), and after kick 28 it read
  31 = 28 + 3. iOS sends a frame number there. The driver and probe now
  send `frame + 1` by default (`token=next`); `token=same|serial` exist to
  measure the difference.
- **The ISR was not eating `0x8`.** The ISR counts bit 3 on every status
  read, flip or not, and the pre-push clear runs under the same lock
  before the push. `synced=0` over ≈300 VBI interrupts means bit 3 was never
  set. `status=0x0` in `adbe_state` only shows that the ISR acked that
  frame's bits.
- **`aborts` was the wrong name.** `aborts == kicks == 28`, `errors=0`, and
  the base readback followed every flip (`0xa400000`, `0xb000000`), so
  `0x400` is the per-packet completion this pipe gave for our packets. iOS
  stops waiting on `0x408` and takes its error branch when `0x8` is absent,
  so `0x400` may be the pipe flagging the mismatched token: watch for `0x8`
  with `token=next`. The driver already completed flips on `0x400`. The
  **park path and probe phases C/D waited for `0x8` only**, so a DPMS-off,
  an unbind or a phase C/D run would have timed out into the direct-store
  fallback. All of them now accept either bit, and count `synced` (0x8) and
  `stop-0x400` separately.
- **The KMS flips themselves looked healthy** (no busy FIFO, no errors, no
  timeouts, base following each flip). Whether the panel showed them is not
  verified without a camera; see R2b.5.

Camera-free evidence: the pipe has no known scanout CRC, position or
active-address register (none in the study; a likely source is the
statistics block behind `DPBDriverV1`/`AAPDriverH8`/`MIEDriver` and the
`ALSS` handler in `AppleH8ADBE0`, which is reverse-engineering work). What
this round adds:

- the driver checks the plane base against the pushed value at every
  completion (`latch-mismatch`) and counts frame-counter jumps
  (`frame-jumps`);
- the probe records `frame A->B jumps=N` per kick;
- phase D has `fill=white|black` for an ambient-light-sensor comparison
  (R2b.5).

## R2b.1 Build and boot

As R2.1, with `a9/display` at `9d776859c0`. Then gate on:

```sh
grep -x CONFIG_MODULE_UNLOAD=y build/kernel-nextint/config
cmp /tmp/j71t-before-r2.dtb build/kernel-nextint/dtbs/s8003-j71t.dtb && echo "DTB unchanged: warm kexec OK"
```

Copy both `.ko` files from that build to `/tmp` on the iPad as in R2.1.

## Running a probe step without unloading

Every parameter is writable (`0600`) and is read once when the probe binds,
so a step is: unbind, write the parameters, bind. Unbind restores the pipe
exactly as unload does. Shell helper for the iPad (root):

```sh
P=/sys/module/apple_adbe_probe/parameters
B=/sys/bus/platform/drivers/apple-adbe-probe
K=/sys/bus/platform/drivers/apple-adbe
DEV=206200000.display-subsystem; D=/sys/kernel/debug/$DEV
run() {  # run key=value ...   (unlisted parameters go back to defaults)
	echo $DEV > $B/unbind 2>/dev/null
	echo -1 > $P/irq_index; echo 0 > $P/irq_mask; echo N > $P/dart
	echo a > $P/phase; echo base > $P/packet; echo next > $P/token
	echo Y > $P/arm; echo band > $P/fill; echo vram > $P/scratch
	echo -1 > $P/tag; echo 2000 > $P/hold_ms
	for kv in "$@"; do echo "${kv#*=}" > $P/${kv%%=*}; done
	echo $DEV > $B/bind; dmesg | grep apple-adbe-probe | tail -n 25; cat $D/state
}
insmod /tmp/apple_adbe_probe.ko          # once; binds with phase A
```

If `apple_adbe_drm` holds the device, run `systemctl stop sddm` and
`echo $DEV > $K/unbind` first. That parks the panel on the black boot
buffer, and the probe still binds, because the plane is back on the boot
base. `bind` fails with `EINVAL` on a bad parameter, and nothing is
written. With `CONFIG_MODULE_UNLOAD`, `rmmod` works again as well.

## R2b.2 VBI interrupt: `run irq_index=0 irq_mask=0x9`

PASS criteria as in R2.3: `vbi≈frames≈60`, `guard=no`, and the mask goes
`0x2->0xb->0x2`.

## R2b.3 Phase C: one kick, the token question

```sh
run phase=c                  # token=next (default)
run phase=c token=serial     # round 2's token, for comparison
```

Each prints `C re-send: packet=base token=<name>/<value> armed=1 words=3 ... synced=S@Tus stop=P@Uus errors=0 ...; frame A->B jumps=J; readback 0x7c95c000; result 0`.

- PASS for `token=next`: exactly one completion (`S+P == 1`), `J=0`,
  `B == A+1`, `errors=0`, `result 0`, and no visible change.
- Expected for `token=serial`: `J ≥ 1` and `B` equal to the small token
  value. That confirms the token→counter diagnosis.
- Record `S` versus `P` for both runs (does `token=next` bring `0x8`?), and
  keep the `t=` trace from `$D/state`. Optional: `run phase=c arm=N` (the
  unarmed preamble) records whether any completion bit is raised at all.

## R2b.4 Phase D: flip and back

`run phase=d`, then, if the picture does not move, `run phase=d packet=full`
or `run phase=d packet=sync`. Then `run phase=d scratch=dma` with the
`packet=` that worked. PASS:

- both `D flip` and `D revert` show one completion, `errors=0`, `jumps=0`
  and their readback;
- `phase D hold: frames≈118 jumps=0`;
- `phase D done: ... revert confirmed`;
- on camera, if available, the magenta band, as in R2.5.

## R2b.5 Camera-free check with the ambient light sensor (heuristic)

Hold the panel full black, then full white, with a fixed backlight, while
sampling the sensor. A consistent rise during the white hold means the
scanout reads the new buffer. No difference proves nothing (the sensor may
not see the panel).

```sh
systemctl stop sddm; brightnessctl set 100%      # fixed backlight, no idle dimming
ALS=$(ls /sys/bus/iio/devices/iio:device*/in_illuminance_input | head -n 1)
als() { for i in $(seq 48); do cat $ALS; sleep 0.25; done > $1; }
als /tmp/als-black.txt & run phase=d fill=black hold_ms=6000 >/dev/null; wait
als /tmp/als-white.txt & run phase=d fill=white hold_ms=6000 >/dev/null; wait
als /tmp/als-base.txt                             # no flip: the noise floor
for f in base black white; do sort -n /tmp/als-$f.txt | awk -v f=$f '{a[NR]=$1} END {print f, "median", a[int((NR+1)/2)], "max", a[NR]}'; done
```

Each `run` blocks ~1.2 s, then flips and holds 6 s; samples 6–30 fall
inside the hold. Keep the room light constant and the iPad still. PASS
(heuristic): the white median clearly above the black median, with a gap
larger than the `base` spread; repeat once with the order swapped. Both
phase D runs must also PASS as in R2b.4. If the sensor can't see the panel,
report "inconclusive"; this does not fail the round.

## R2b.6 KMS driver with frame-number tokens

```sh
echo $DEV > $B/unbind 2>/dev/null                 # or rmmod apple_adbe_probe
insmod /tmp/apple_adbe_drm.ko commit=base token=next   # if already loaded: echo $DEV > $K/bind
A=$(grep -l adbe /sys/kernel/debug/dri/*/name | head -n 1 | xargs dirname)/adbe_state
cat $A; systemctl start sddm; sleep 10; cat $A
```

PASS (as R2.6, plus): `latch-mismatch=0`, `timeouts=0`, `vbl-fallbacks=0`,
`synced + stop-0x400 ≈ kicks`, and `frame-jumps` ≈ 0 (a rare jump is a
missed VBI). With round 2's token, `frame-jumps` would grow with every
kick. Record whether `synced` or `stop-0x400` carries the completions. To
compare tokens without unloading:

```sh
systemctl stop sddm; echo $DEV > $K/unbind
echo serial > /sys/module/apple_adbe_drm/parameters/token
echo $DEV > $K/bind
```

DPMS off/on and unbind now take the completion path. `adbe_state` after
`hyprctl dispatch dpms off` must show `base=0x7c95c000` with no `park
packet not confirmed` in dmesg. Fallbacks are as in R2.6. `rmmod
apple_adbe_drm` works now (`fuser -v /dev/dri/card*` first).

---

# Round 3: boot-time KMS, kill switch, 0x400 completion, tear-free check

Kernel branch `a9/display`, tip **`1b83789aa87469552a06d46e70b48e9dccf2b71e`**
(one commit on top of round 2b's `9d776859c0`). Patch:
`patches/display/round3/`. **No DTS change** (`git diff 95149d2807
1b83789aa8 -- arch/` is empty). Config: `CONFIG_DRM_APPLE_ADBE=y` (built
in); the probe stays `=m`.

What changed:

- **Boot-time takeover.** The driver binds at device-initcall time, before
  userspace and before sddm. If simpledrm has already bound, it is unplugged
  as before. If it has not bound yet (a deferred probe), its
  simple-framebuffer device is unregistered (`retiring ... before simpledrm
  binds it`), so simpledrm cannot take the boot framebuffer back.
- **Kill switch.** `apple_adbe_drm.enable=0` on the kernel command line makes
  probe return `-ENODEV` before touching anything, and simpledrm keeps the
  display. A warm kexec inherits the running command line, so remove the
  parameter to switch back.
- **`0x400` is the documented completion.** The R2b.3 trace shows why: the
  FIFO holds the packet (ParmSt 3, `0x1040`=1) until the next boundary, then
  applies it, with `0x400` and VBI in the same sample. The vblank interrupt
  mask is now `0x2 | 0x1 | 0x400` (`inten=0x403`), and `0x8` is only counted.
  A flip completes once that boundary's vblank has been counted, so the
  event carries that frame's sequence and timestamp.
- **Stale vblank on enable fixed.** VBI stays latched while vblank is off,
  and unmasking it fired at once for an old boundary (R2b.2: `vbi=64` for
  `frames=63`). The driver now clears it before enabling.
- **The 36/35 gap was bookkeeping.** In `r2b-6-dpms.txt` the gap appears at
  the DPMS-off read (29 kicks, 28 completions, `inten=0x2`, base already on
  the boot buffer), so the uncounted packet is the park. The park poll acked
  its `0x400` without counting it. The gap then stayed at exactly 1 (36/35).
  Now every status read that sees a completion counts it once, and debugfs
  shows `kicks = completions + in-flight` exactly. `in-flight` is 1 only
  while the FIFO still holds a packet (`0x1040` busy).
- **Tear-free flips.** Flip events are sent only after the packet was
  applied at a frame boundary, and the next commit waits for that event
  (atomic helpers), so the compositor never draws into the buffer being
  scanned out. Commits are atomic and nonblocking (DRM helpers), and vblank
  timestamps come from the VBI interrupt (hard IRQ, µs after the boundary).
  The page-flip event Hyprland uses for presentation timing carries that
  timestamp.
  - Async (tearing) flips are not offered: the FIFO only applies at
    boundaries, so Hyprland's `allow_tearing` falls back to vsync'd flips.
  - There is no hardware vblank counter hook (the frame counter can be
    reloaded by a token), so DRM bridges vblank-off gaps with timestamps,
    the default.
  - New `adbe_state` line: `flip-latency frames 0:a 1:b 2:c 3+:d; us
    avg=… max=…`. This is camera-free evidence that each flip lands on the
    next boundary.
- The probe no longer has a `MODULE_DEVICE_TABLE`, so it can never be
  auto-loaded in place of the KMS driver. To use it: `systemctl stop sddm`,
  `echo 206200000.display-subsystem > /sys/bus/platform/drivers/apple-adbe/unbind`,
  `insmod /tmp/apple_adbe_probe.ko ...`. Rebind the KMS driver with
  `.../apple-adbe/bind`. simpledrm does not come back without a reboot.

Offline: incremental `W=1` build with no warnings, built-in parameter
`apple_adbe_drm.enable` present in vmlinux, isolated `W=1` recompile of
`drivers/gpu/drm/adbe/` clean, host arithmetic test, binding/DTB schema,
DTS unchanged, strict checkpatch 0/0/0, patch/tree identity, clean
`git merge-tree` against nextint `fc52fc4e2b` and the 12 stack branches.

## R3.1 Build and stage two entries

```sh
cp build/kernel-nextint/dtbs/s8003-j71t.dtb /tmp/j71t-before-r3.dtb
INTEGRATION_TREE=nextint scripts/integrate.sh <current nextint stack, with a9/display at 1b83789aa8>
TREE=nextint scripts/display/build-round3.sh > build/kernel-nextint/display-r3-build.log 2>&1
grep -E '^CONFIG_(DRM_APPLE_ADBE=y|MODULE_UNLOAD=y)' build/kernel-nextint/config
grep 'built-in parameter: apple_adbe_drm.enable' build/kernel-nextint/display-r3-build.log
cmp /tmp/j71t-before-r3.dtb build/kernel-nextint/dtbs/s8003-j71t.dtb && echo "DTB unchanged: warm kexec OK"
TREE=nextint scripts/boot/stage-kernel.sh display-r3
TREE=nextint scripts/boot/stage-kernel.sh display-r3-off --cmdline "<running cmdline without one-shot parameters> apple_adbe_drm.enable=0"
```

Keep the known-good entry. `display-r3-off` is the same kernel with the
kill switch.

> Private graphics-dependent excerpt omitted from this source snapshot.

## R3.2 Boot straight onto the driver (warm kexec into `display-r3`)

```sh
dmesg | grep -E 'apple-adbe|simpledrm|retiring|\[drm\]|fb0|fb1' | head -30
for c in /sys/class/drm/card*; do echo "$c $(readlink $c/device/driver)"; done
ls -l /dev/dri/by-path/
systemctl is-active sddm
A=$(grep -l adbe /sys/kernel/debug/dri/*/name | head -n 1 | xargs dirname)/adbe_state; cat $A
```

> Private graphics-dependent excerpt omitted from this source snapshot.

FAIL or a bad picture: boot `display-r3-off` (or the known-good entry). SSH
is unaffected.

## R3.3 Kill switch (warm kexec into `display-r3-off`)

PASS: dmesg shows `disabled by apple_adbe_drm.enable=0; simpledrm keeps the
display`, simpledrm owns the only card, and Hyprland comes up on it (the
iPad uwsm env picks the `framebuffer` card). `cat /proc/cmdline` contains
`apple_adbe_drm.enable=0`. Then kexec back into `display-r3`, whose
command line does not contain the parameter.

## R3.4 Owner visual check (needs the owner's eyes; screenshots can't show tearing)

Copy the test page to the desktop user's home (the orchestrator):

```sh
cat scripts/display/tear-test.html | scripts/omarchy/ssh.sh 'cat > ~/tear-test.html'
```

On the iPad, in Hyprland, open `file:///home/<user>/tear-test.html` in the
browser full screen (F11), or from a terminal run
`chromium --kiosk file://$HOME/tear-test.html` (whatever browser Omarchy
launches works).

1. **Picture.** Before the test page: the desktop looks right. Colours are
   normal (not swapped or tinted), text is sharp, there are no stripes,
   snow or offset lines, the orientation and scale match simpledrm, and the
   cursor and touch follow your finger.
2. **Tearing.** The page moves black/white vertical bars sideways every
   frame, with thin magenta rows every 256 pixels. PASS: every bar is one
   straight, unbroken column from top to bottom. FAIL: a horizontal break
   line where the bars above are shifted against the bars below, flickering
   at changing heights. The frame rate (top-left) may be low with software
   rendering; that is fine. Low fps is not tearing. Watch for 30 s.
3. **Scrolling.** Open a long web page and flick-scroll quickly by touch.
   PASS: the text moves as one sheet; lines are never split or doubled
   across a horizontal seam.
4. **Contrast run (optional).** Repeat step 2 on `display-r3-off`
   (simpledrm). simpledrm copies to the live framebuffer without waiting
   for vblank, so breaks there are expected, and they show what a tear
   looks like.
5. **DPMS.** Lock or idle until the screen blanks, then wake it: black,
   then the desktop again, with no garbage.

Report per step: PASS/FAIL, what was seen, and `$A` read just after the
tear test (its `flip-latency` line is the camera-free counterpart of step 2).

# Round 4: the soak leak, buffer accounting, idle CPU

Kernel branch `a9/display`, tip **`04ca5aa8f24f4c6cca628dd4e0619146289ec019`**
(one commit on round 3's `1b83789aa8`). Patch: `patches/display/round4/`.
**Debugfs only, no behaviour change. No DTS or config change.** Sampler:
`scripts/display/churn-r4.sh` (runs on the iPad, tested offline against
stubs). Trigger: the nextint7 30-minute soak
(`docs/test-logs/nextint7-2026-10-24/`): CMA failures from 128 s, thrashing,
Hyprland OOM-killed, then 24% idle CPU after the restart.

## Findings (offline, from the source and the soak log)

1. **apple_adbe_drm does not leak.** Verified in the source: the driver takes
   no framebuffer or GEM reference of its own. Buffers are GEM DMA objects
   (`drm_gem_dma_*`), framebuffers come from `drm_gem_fb_create`, and the
   atomic helpers drop the old plane state after each commit. The four flip
   completion paths (0x400 + VBI interrupt, 3-VBI fallback, 100 ms timer,
   CRTC disable) only move `adbe->event` and balance the vblank reference
   taken in `adbe_kick()`. An event is armed only after the FIFO wait
   succeeds, so no armed-then-failed path exists. A new commit waits for the
   previous flip_done, so a pending event is never overwritten. The park path
   pushes without an event. The inverse risk is real but not a leak: the
   driver never pins the buffer it scans out, so after a timeout or a `busy`
   skip the helpers may free a buffer the pipe still reads (garbage on
   screen, not leaked memory). `timeouts` and `busy` count those cases, and
   both were 0 in R3.
2. **The CMA failures are the decoder's, and the soak overcommits CMA.**
   This kernel uses 16 KiB pages and `CONFIG_CMA_ALIGNMENT=8`, so CMA aligns
   each buffer to its own order, capped at 256 pages (4 MiB). In this
   kernel's `cma_range_alloc()`, `ret` starts at `-EBUSY`, so `ret: -16`
   also means "area full", not only "pages pinned".

   | Buffer | Bytes | Pages | CMA slot | Fallback block |
   |---|---|---|---|---|
   | vdec CAPTURE, 1080p NV12 (stride 2048 × 1088 × 1.5) | 3,342,336 | **204** | 4 MiB | order 8, 4 MiB |
   | vdec OUTPUT bitstream (GStreamer: 1920 × 1088 × 1.5) | 3,133,440 | **192** | 4 MiB | order 8, 4 MiB |
   | vdec co-located side buffer, one per CAPTURE (by size) | ≈1.0 MiB | **65** | 2 MiB | order 7, 2 MiB |
   | scanout / fbdev, 1536 × 2048 × 4 | 12 MiB | 768 | 12 MiB | order 10, 16 MiB |

   The failing request sizes in the soak (192, 204, 65 pages) are exactly
   the decoder's. Estimated budget, to be confirmed by R4.2 and R4.4:
   - The display holds 12 MiB for fbdev for the whole session, plus
     k × 12 MiB for aquamarine's scanout buffers (k is measured in R4.2).
   - Each decoder instance holds about 6 MiB per CAPTURE buffer plus 4 MiB
     per OUTPUT buffer, so roughly 60–70 MiB for ~10 CAPTURE buffers.
   - The soak ran two decoders at once (the checksumsink loop and gst-play;
     playbin picks `v4l2slh264dec`, rank 257). Two decoders beside the
     display do not fit in 128 MiB.
3. **Why it thrashed.** When CMA has no room, dma-direct falls back to
   `alloc_pages(get_order(size))`: one unmovable order-7 or order-8 block per
   decoder buffer, or order 10 per scanout buffer. Each gst-play start (every
   10 s) asks for tens of such blocks. That drives direct compaction and
   reclaim, evicts page cache that has to come back over the NBD root, and
   keeps kswapd busy. It also explains the stalled SSH and the 20 empty
   decode runs during the thrash.
4. **The OOM line alone can't name the leak.** It shows `total-vm 3.2 GB`
   with anon and shmem RSS at 0. GEM and dma-buf mmaps are PFN mappings that
   never count in RSS, so leaked buffer mappings would look like this. So
   would ordinary VM reservations (llvmpipe thread stacks, LLVM). The memory
   came back when the session stopped, which points at something a process
   held. The sampler records Hyprland's `/dev/dri/card*`, memfd and dma-buf
   mappings, plus the new per-client GEM lines, to decide.
5. **vdec frees symmetrically** (`a9/vdec` at 3001513b95, read-only check):
   - CAPTURE and OUTPUT buffers are vb2-dma-contig (CMA, contiguous, no
     IOMMU) and are GART-mapped in `buf_init`, unmapped in `buf_cleanup`.
   - The side buffer is allocated in `buf_init` and freed in `buf_cleanup`.
   - The H.264 context buffers are allocated at OUTPUT STREAMON and freed at
     STREAMOFF or release.
   - vb2 frees buffer memory at REQBUFS(0) or close, not at STREAMOFF (by
     design), so a player that exits frees everything.
   - The exception is a dma-buf someone else still holds. The compositor can
     hold one: Mesa's kms_swrast imports client dmabufs through PRIME on the
     adbe card. That now shows as `import=` on Hyprland's client line and as
     `import` objects in `adbe_bos`.
6. **Idle CPU.** The driver does not poll: the FIFO wait sleeps in 1 ms
   steps, only in commit work, and the interrupt is a few register reads.
   The 24% was measured right after the OOM, with kswapd busy. R4.3 separates
   four cases:
   - Hyprland is committing frames: `kicks` moves.
   - vblank is held on: `vbi` moves while `kicks` doesn't.
   - Refault thrash: `pgmajfault` moves.
   - Allocator pressure: kswapd CPU is high.

Mitigations for the orchestrator to decide (none is in the kernel commit):
- **Capacity:** use `cma=256M` on the command line (R4.6), or
  `CONFIG_CMA_SIZE_MBYTES=256` in `display.config`, whenever two 1080p
  decoders run beside the desktop.
- **Alignment:** `CONFIG_CMA_ALIGNMENT=4` (256 KiB) would cut the decoder's
  CMA use by about a quarter (204 pages would take 208, not 256; 65 would
  take 80, not 128; 192 would take 192, not 256). It is global: every
  large `dma_alloc` loses its natural alignment above 256 KiB.
  `apple_vxd_dma_alloc_aligned()` already retries with slack.
- **fbdev:** fbdev's 12 MiB could live in the iBoot framebuffer's reserved
  memory, which today only serves as the black park buffer. That is a
  driver change for a later round, if CMA stays tight.

## R4.1 Build and stage

```sh
cp build/kernel-nextint/dtbs/s8003-j71t.dtb /tmp/j71t-before-r4.dtb
INTEGRATION_TREE=nextint scripts/integrate.sh <current nextint stack, with a9/display at 04ca5aa8f2>
TREE=nextint scripts/display/build-round3.sh > build/kernel-nextint/display-r4-build.log 2>&1   # round 3's script, unchanged
cmp /tmp/j71t-before-r4.dtb build/kernel-nextint/dtbs/s8003-j71t.dtb && echo "DTB unchanged: warm kexec OK"
TREE=nextint scripts/boot/stage-kernel.sh display-r4
# after boot:
cat scripts/display/churn-r4.sh | scripts/omarchy/ssh.sh 'cat > /var/tmp/churn-r4.sh && chmod +x /var/tmp/churn-r4.sh'
scripts/omarchy/ssh.sh 'ls -l /var/tmp/vdec/clip1080.mp4 /var/tmp/vdec/clip1080.h264'   # vdec plan R6.3 pushes them
```

The script refuses to run without a `gem` line in `adbe_state`, i.e. on an
older kernel. Run every step below as
`scripts/omarchy/ssh.sh 'bash /var/tmp/churn-r4.sh …'`. It writes the full
samples to `/var/tmp/churn-r4/<mode>-<time>.log` and prints the `S` lines
and a summary.

## R4.2 Baselines: no desktop (B0), then the desktop (B1)

```sh
scripts/omarchy/ssh.sh 'sudo systemctl stop sddm; sleep 10; bash /var/tmp/churn-r4.sh baseline'      # B0
scripts/omarchy/ssh.sh 'sudo systemctl start sddm'   # wait for the desktop, then 60 s more
scripts/omarchy/ssh.sh 'bash /var/tmp/churn-r4.sh baseline; f=$(ls -t /var/tmp/churn-r4/baseline-* | head -n 1); sed -n "/adbe_state/,/meminfo/p" "$f"'   # B1
```

- B0: no `client` lines, `gem dumb=1/12288K import=0/0K fbs=1` (the fbdev
  buffer; `adbe_bos` shows it created by the probe thread), `orphans` 0 or
  1 (that same buffer), `mmaps=0`, `vblank refcount=0 enabled=0`. Note
  `cma-free` and MemAvailable.
- B1: Hyprland's line `client Hyprland[pid] master=y fbs=k dumb=k/…`. k is
  how many 12 MiB scanout buffers aquamarine keeps (expected 2–4). Report k,
  `dumb-in-cma` (it should equal `dumb`) and the drop in `cma-free` from B0.
  Stopping sddm is what the soak recovery did; SSH stays up.

## R4.3 Idle CPU (2 min, desktop untouched, no players)

```sh
scripts/omarchy/ssh.sh 'bash /var/tmp/churn-r4.sh idle 120'
```

PASS (display): at the end `vblank refcount 0 enabled 0`, unless `kicks`
moved in the last 5 s (vblank turns off 5 s after the last use). `vbi`
grows only while `kicks` grows. The kick identity holds. Interpretation of
the printed deltas:

| Reading | Meaning |
|---|---|
| `kicks` + hundreds (> 600 in 120 s) | Hyprland commits frames while idle: something on screen animates (bar clock, cursor, a notification). Userspace; take a screenshot and `hyprctl clients` |
| `vbi` + ~7200 with `kicks` ~0 | a vblank reference is never dropped: driver/core bug (`refcount` stays > 0) |
| Hyprland CPU high, `pgmajfault`/`pgscan_*` growing | refault thrash under memory pressure (see R4.5) |
| kswapd CPU high | allocator pressure (watermarks or high-order requests); the log has `pagetypeinfo` |

## R4.4 Window churn, 10 minutes: the leak test

```sh
scripts/omarchy/ssh.sh 'bash /var/tmp/churn-r4.sh churn 600'
scripts/omarchy/ssh.sh 'cat $(ls -t /var/tmp/churn-r4/churn-* | head -n 1)' > build/display-r4-churn.log
```

One decoder only: gst-play with waylandsink, window opened and closed every
~11 s. The 10 s clip plays to EOS, so the player exits normally and closes
its own window; `timeout -k 3 12` only catches a hung player. If the soak
killed players mid-clip instead, add one run with `timeout 5` (edit the
script) to cover that teardown path too. Samples are taken every third cycle **between
players** (about every 33 s), plus mid-playback samples at cycle 1, 10, 20,
and so on. The script compares the first sample after one minute of warm-up
with the sample taken 5 s after the last player. It prints PASS/FAIL for:

- `cma-free` (CmaFree) drops by at most 16 MiB. Movable pages borrow free
  CMA, so CmaFree is noisy; the GEM counters below are exact.
- Hyprland VmRSS grows by at most 20 MiB and VmSize by at most 64 MiB.
  Its `/dev/dri/card*` and memfd mapping counts grow by at most 1.
- `gem dumb` count unchanged; `gem import`, `orphans`, `fbs` not higher;
  `mmaps` at most +1.
- No gst-play left alive between runs, no `cma: … alloc failed` line, the
  kick identity holds, and `timeouts` and `busy` are 0.

The mid-playback samples answer two questions. If `gem_import` > 0 while
playing, Hyprland imports the player's NV12 dmabufs. Comparing `cmafree`
during play with the sample before gives one decoder's CMA footprint.

FAIL: what grew tells where the leak is.

| What grows | Meaning | Owner |
|---|---|---|
| `dumb` on Hyprland's client line | scanout buffers allocated and never destroyed | aquamarine/Hyprland |
| `orphans`, with `handles=0 mmaps>=1` in `adbe_bos` | handle closed, CPU mapping kept | Mesa (kms_swrast mapping) |
| `import`, or `import` objects left after the player exited | the compositor holds decoder buffers, pinning CMA per player | Hyprland/Mesa dma-buf import |
| Hyprland `memfd_maps`/`memfd_kb` | wl_shm pools kept mapped after the client left | Hyprland |
| Hyprland `rssanon` only | heap leak | Hyprland/llvmpipe |
| `fbs` | framebuffers never removed | userspace (RMFB) |
| CmaFree falls between players, GEM flat | another CMA user: the decoder | vdec: `sudo ls -l /proc/*/fd 2>/dev/null \| grep video`, players in D state (`cat /proc/<pid>/stack`) |
| `players` > 0 between runs | gst-play does not exit | vdec (D state) |

## R4.5 Optional: the soak's overcommit, bounded (at most 5 min)

```sh
scripts/omarchy/ssh.sh 'MIN_AVAIL_KB=300000 bash /var/tmp/churn-r4.sh churn 300 --decode'
```

This adds the soak's parallel decode loop (to `fakesink`; same buffers as
`checksumsink`). The watchdog stops at MemAvailable < 300 MB. Expected, if
findings 2 and 3 hold: `alloc failed` for 204/192/65 pages as soon as the
second decoder starts, and `compact_stall`, `allocstall` and `pgscan_*`
climbing. It measures headroom; it is not a display PASS/FAIL. If SSH lags
more than 30 s:
`scripts/omarchy/ssh.sh 'pkill -f churn-r4; pkill gst-play-1.0; pkill gst-launch-1.0'`.

## R4.6 Optional: CMA headroom

```sh
TREE=nextint scripts/boot/stage-kernel.sh display-r4-cma256 --cmdline "<running cmdline without one-shot parameters> cma=256M"
# warm kexec into display-r4-cma256, then:
scripts/omarchy/ssh.sh 'grep Cma /proc/meminfo'    # CmaTotal: 262144 kB
scripts/omarchy/ssh.sh 'MIN_AVAIL_KB=300000 bash /var/tmp/churn-r4.sh churn 300 --decode'
```

Expected: no `alloc failed`, no abort, `compact_stall` flat. Then kexec back
into `display-r4`, whose command line has no `cma=`.

## R4.7 End check: the session gone (B2)

After R4.4 (and R4.5/R4.6 if run):

```sh
scripts/omarchy/ssh.sh 'sudo systemctl stop sddm; sleep 10; bash /var/tmp/churn-r4.sh baseline; f=$(ls -t /var/tmp/churn-r4/baseline-* | head -n 1); sed -n "/adbe_bos/,/meminfo/p" "$f"'
scripts/omarchy/ssh.sh 'sudo systemctl start sddm'
```

PASS (kernel side): B2 matches B0. That means `gem dumb=1/12288K
import=0/0K fbs=1`, no `client` lines, and `adbe_bos` lists only the fbdev
buffer. CmaFree is within 16 MiB of B0 and MemAvailable within 50 MB. If GEM
is back to B0 but memory is not, the leak is outside the display: record
`grep -E 'Slab|SUnreclaim' /proc/meminfo`.

## Evidence to return

`docs/test-logs/<run>/`:
- `display-r4-baseline.txt`: B0, B1, B2 and their logs.
- `display-r4-idle.txt`.
- `display-r4-churn.txt`: stdout plus the `/var/tmp/churn-r4/churn-*.log`
  file.
- R4.5/R4.6 output, if run.
- `dmesg | grep -E 'cma|oom|adbe'`.
