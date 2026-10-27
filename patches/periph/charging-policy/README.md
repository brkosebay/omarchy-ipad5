# J71t steady charging policy

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Incremental series on `a9/periph`, after `46864bf1264874b31f9eff73dda5f1b529ca013a`
(the DWC2/ECM fixes in `patches/periph/usb-resume/`). Apply `series` in order.

- `14cc7fc330e770a37135776ffcc4663a38d4eab0`: input-ceiling DT binding.
- `dcc23b697e8e63e318cf3d746e4348faa28b5d1f`: charger and Tristar policy.
- `bcc4784eee43684d2a48905a9cb6f7a30a627319`: J71t board/owner ceilings.

The board cap and owner request are both 2400000 uA at probe. A classified
host/brick and valid CBAT are required to elevate above the 500 mA floor;
unknown/missing board caps and unsupported DT values fall back to 500 mA.
The input ramp is 500 -> 1000 -> 2100 -> 2400 mA, at most once per second,
with 2500 mA battery current above the floor (400 mA at the floor). The real
CBAT floor programs 475 mA input; the 2400 mA target programs code 0xba.

USB suspend reports a 500 mA **source** even when the gadget snapshot's
`current_ma` is zero. This check precedes both `configured` and `host_grant`;
putting it after those checks would still return zero on a real suspend.
The owner policy holds 2400 mA on that source. A reset's low allowance or
lost classification gets a single 5 s grace; an enduring dip steps down to
500 mA, retaining the owner request. Elevation retries after 30 s and ramps
again. Only confirmed VBUS loss or explicit sysfs zero revokes the request.
After a real unplug, manually re-arm on this Mac with the existing service.

Budget changes and snapshot restores write 0xa08 followed by 0xa03 directly.
They do not assert 0xa00[1]. Only true zero/disable asserts it; a later enable
clears an existing disable after installing verified budgets. Ownership
mismatch uses -EUCLEAN and backs off for 60 s, including across sysfs re-arm.
Gauge/source/transport failures never set that latch. Failed safety samples
hold an active budget for at most 60 s, then restore the handoff snapshot
once; recovery needs no service restart. PRESENT=0 with a readable in-range
voltage proves pack presence. Thermal trip/clear is 45/42 C; thermal,
termination and timeout guards step down once to the floor, keeping the owner
request. Voltage/cold/cap guards and rollback remain effective.

Tristar's `connected || host_grant` analog-detection exclusion is unchanged;
no register 0x03 writes or resume detection sweeps were added.

## Reproduce offline

```sh
python3 -B scripts/periph/test-charging.py
scripts/periph/check-usb-resume.sh
scripts/periph/export-charging-policy.sh
KERNEL_JOBS=2 scripts/periph/build-charging-policy.sh
scripts/build-kernel.sh --run periph 'python3 -B -I /work/scripts/periph/check-charging-policy-integration.py'
scripts/periph/check-charging-policy-build.sh
shasum -a 256 -c build/periph/charging-policy/SHA256SUMS
```

The actual driver functions run under ASan/UBSan. Tests cover direct write
order, no 0xa00 writes on nonzero transitions, steady operation, grace
expiry/recovery and classification dips, owner retention, DT validation and
cap, transient PRESENT/voltage failures, unavailable pack after 60 s, source
I/O, status I/O, thermal hysteresis (also during a source I/O error),
timeout/termination, latch expiry,
shutdown/resume and 17 injected transaction/rollback failures. Existing tests
cover 160 USB-budget assertions, DWC2/ECM regressions (including rejection of
the old functions), 4096 Tristar ADC fixtures, 13 mux faults and 11 recovery
scenarios. The full serial arm64 LLVM=1 W=1 Image.gz + 104-DTB build passed at
`bcc4784eee43684d2a48905a9cb6f7a30a627319`; linked-driver checks pass.
Build and check records are under `build/periph/charging-policy/`.

Private Git indexes replay these five USB+charging patches onto the current
`a9/integ` and the available exact round8e/round8f overlays. No boot,
integration or detached build checkout is modified by those replay checks.
`integration.json` records the resolved base hashes and resulting overlay digests.
The binding document/schema and compiled binding example validate; the optional
yamllint style pass was skipped because that package is not installed.
The full W=1 DTB build emits existing simple-bus address warnings from shared
Apple DTS includes; those includes are unchanged by this series. Unchanged
kernel-doc/BPF diagnostics also emit W=1 warnings. A parallel attempt had one
compiler process killed (137); the completed serial build passes.

## Orchestrator: integrate and warm kexec (not executed here)

The standalone `build/kernel-periph/` image lacks the full boot/NVMe work.
The current `a9/integ` also lacks the round8e/round8f boot heads. For the
exact tested round8e baseline, build the composed overlay in a new detached
tree, preserving all existing boot/integration checkouts. This needs space
for another source/build checkout in the Docker volume:

```sh
cd .
KERNEL_JOBS=1 scripts/periph/build-charging-policy-integration.sh round8e
INITRD="$PWD/build/boot/round8e/initramfs.gz" \
ENTRY=charging-round8e-bcc4784eee \
  scripts/boot/kexec-host.sh build/kernel-chargingpolicy-check/Image.gz \
  build/kernel-chargingpolicy-check/dtbs/s8003-j71t.dtb \
  "$(cat build/boot/round8e/entry-handoff/cmdline)"
```

The build script checks the baseline SOURCE.patch, preserves its config,
and verifies that the J71t DTB changes only by the two charging properties.
It refuses a pre-existing detached checkout with different contents. Use
`round8f` and its matching initramfs/entry cmdline instead only after that
baseline build/export is complete; the current round8f replay is an offline
source check, not an additional full image build. The integration helper's
full build is left to the orchestrator; `--check-only` performs just replay.

The warm-boot helper merges runtime calibration into the new DTB, checks
load/DT/SMP, and waits for a different boot_id. It leaves the default entry
unchanged and replaces the old entry name in the selected command line.
Use a fresh ENTRY name if repeating with different image contents. The
selected baseline entry supplies its existing root/NVMe policy and initramfs.

To land the changes in the orchestrator's integration branch instead, these
commands refuse a dirty checkout and skip commits already in its ancestry.
Integrate the matching boot/NVMe work there before using its image on-device:

```sh
scripts/build-kernel.sh --run integ '
set -euo pipefail
test "$(git branch --show-current)" = a9/integ
test -z "$(git status --porcelain)"
for ref in \
  078d2de49b48abb6ce1c1fbe78a6ec7a6d7f5cb3 \
  46864bf1264874b31f9eff73dda5f1b529ca013a \
  14cc7fc330e770a37135776ffcc4663a38d4eab0 \
  dcc23b697e8e63e318cf3d746e4348faa28b5d1f \
  bcc4784eee43684d2a48905a9cb6f7a30a627319; do
  if ! git merge-base --is-ancestor "$ref" HEAD; then
    git cherry-pick "$ref"
  fi
done
'
KERNEL_JOBS=1 scripts/omarchy/build-kernel.sh integ
```

The boot service is now redundant for initial authorization. The driver
accepts its identical write idempotently, but the experiment can exclude that
userspace writer for this boot and stop the recovery rebind fallback during
the causal trace (the runtime mask expires on reboot):

```sh
scripts/omarchy/ssh.sh 'sudo systemctl stop linux-a9-usb-recover.service && sudo systemctl mask --runtime --now linux-a9-charger-limit.service'
```

Keep the existing service as the manual re-arm tool after a real unplug:
`sudo systemctl unmask --runtime linux-a9-charger-limit.service && sudo systemctl restart linux-a9-charger-limit.service`.
No rootfs-image mount/edit is required.

## Orchestrator: one-boot cause/effect capture

Run the capture on the iPad, independently of its USB SSH session. It enables
DWC2 dyndbg and writes 1 Hz samples of `usb/current_max`, input/battery charge
limits, status, pack temperature/voltage/presence, plus initial/followed dmesg.
Samples contain monotonic and realtime nanoseconds; raw dmesg timestamps use
the boot-time axis. Read errors remain visible. It writes no charger register,
syscfg or NVRAM. It restores the per-callsite debug print flags on normal stop,
SIGINT or SIGTERM. Output is bounded to 180 s by default, on tmpfs:

```sh
scripts/omarchy/ssh.sh 'cat > /tmp/charging-cause-effect.py && chmod 755 /tmp/charging-cause-effect.py' \
  < scripts/periph/charging-cause-effect.py
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=charging-cause-effect --collect /usr/bin/python3 /tmp/charging-cause-effect.py --output /run/charging-cause-effect --seconds 180'
```

While it runs, let charging settle and exercise the orchestrator's host USB
suspend/resume/reset test, retaining ACM/Wi-Fi recovery. Do not rebind the
gadget or touch Tristar detection to manufacture a current transition. After
180 s, collect before another boot:

```sh
mkdir -p build/periph/charging-policy/device
scripts/omarchy/ssh.sh 'sudo tar -C /run/charging-cause-effect -cf - .' \
  > build/periph/charging-policy/device/capture.tar
tar -xf build/periph/charging-policy/device/capture.tar \
  -C build/periph/charging-policy/device
```

After collection, restore the fallback with
`sudo systemctl start linux-a9-usb-recover.service` on the iPad.

Compare the first DWC2 event / usb budget drop with the charger transition,
using kernel log timestamps for ordering within a sample interval. USB first
supports link -> policy; charger first followed by budget collapse supports a
current-step -> host reaction. The 1 Hz samples alone cannot order events
within one second. Confirm steady 2400000 uA input / 2500000 uA battery limit,
no reset dip under 5 s changing the limits, no owner revocation on a dip, no
ACM non-L0 -11, and sustained ECM traffic. A long dip should lower the input
without a zero-current ritual and recover automatically after the backoff.

No device was accessed, no workspace commit or push was made, and no
syscfg/NVRAM/boot-firmware storage was written. Hardware validation is pending.
