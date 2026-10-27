# USB ECM loss after charger budget revocation

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. Offline diagnosis and fix on `a9/periph`; no device access,
workspace commit, push, or live rootfs edit. Hardware confirmation is pending.

## Evidence and causal limits

The supplied log is `build/omarchy/logs/netconsole-boot8-nvme.log`, lines
107943–107965. Kernel `round8e-handoff-bfe22c60a0`:

- 43.682 s: owner authorizes 2400 mA with detected allowance 500 mA.
- 48.414 s: owner override revoked **before** the charging-request warning.
- 48.421 s: request revoked `-34` (`-ERANGE`).
- 49.134 s: ACM notification fails `-11` (`-EAGAIN`).
- 49.159 s: ordinary 500 mA input / 400 mA battery charging resumes.

In `j71t_policy_work()`, that ordering means the source allowance fell below
500 mA before the active-policy guard ran. The source supplier consumes the
USB budget: `composite_suspend()` sets the suspended flag (allowance zero),
and `composite_resume()` restores the configured 500 mA budget. Reset / a
temporary pre-configuration budget can also revoke the allowance. The log
does not contain an interrupt trace proving which event initiated the drop.

`apple_j71t_charger.c` writes only D2365 `0xa00[1]` (input suspend), `0xa03`
(input current), and `0xa08` (battery charge current), with reads/guards and
snapshot rollback. It does not program the USB PHY, DWC2 registers, VBUS
generation, or Tristar routing. Restoring / changing policy does briefly
suspend PMIC input. These logs alone cannot exclude an electrical transient,
but they do not establish one as the initiating cause.

`apple_tristar_power.c` **can** divert D+/D− to the ADC: register `0x01` on
CBTL1610 at I2C `0x1a`, via `apple_tristar_pin_mv()`. Classification refuses
`budget.connected` or `budget.host_grant`, checks again between samples, and
restores routing after each sample. A USB suspend retains `connected` and
does not reset the brick-classification generation. Returning to a 500 mA
budget is a budget lookup, not a command to redo BC1.2 / analog detection.
Neither charger nor Tristar source differs between `a9/periph`'s pre-fix
head and the relevant integration sources. Keeping this guard and the
charging revocation avoids continuing a 2.4 A override on a suspended bus.

Two concrete USB bugs fit the symptom without a composite-device unplug:

1. Apple DWC2 uses no partial power-down and permits clock gating. In
   `dwc2_gadget_exit_clock_gating()`, `call_gadget(resume)` ran before
   `lx_state = DWC2_L0`. A resume callback that queues a notification therefore
   hits `dwc2_hsotg_ep_queue()`'s non-L0 rejection, `-EAGAIN`. This matches the
   ACM warning, although that warning alone does not prove this call site.
2. `ecm_do_notify()` consumed CONNECT/SPEED state before a successful
   `usb_ep_queue()`. An `-EAGAIN` could lose the NETWORK_CONNECTION notification
   macOS needs for carrier. `ecm_resume()` did not retry or refresh it. The
   local gether carrier flag can remain up independently of host notification.

The fixes address these reproducible software faults. Whether they eliminate
this particular hardware failure still requires the orchestrator's test.

## Kernel changes

Both commits follow `64221b189e` (Round 6) on `a9/periph`:

- `078d2de49b48abb6ce1c1fbe78a6ec7a6d7f5cb3`: publish L0 and clear
  `bus_suspended` before invoking clock-gating resume callbacks.
- `46864bf1264874b31f9eff73dda5f1b529ca013a`: retain failed ECM notification
  state and refresh CONNECT followed by SPEED on resume. An existing in-flight
  notification still serializes use of the single request.

Incremental patches, base/head IDs, hashes and ordering are in
`patches/periph/usb-resume/`. Apply these after `patches/periph/0055-*`;
the older top-level periph series is unchanged. The charger guards, PMIC
register policy and analog detector are unchanged.

## Rootfs recovery

`linux-a9-usb-recover.service` starts the installed Python helper. Provisioning
installs Python explicitly; `hot-apply.sh` also installs/enables the service.
The helper:

- observes usb0 with carrier before arming; waits 10 s after carrier loss;
- requires usb0 administratively up, live `usb/online=1`, positive source
  allowance (zero means USB suspend), a configured UDC, and exactly one bound
  gadget with an active ECM function whose `ifname` is usb0;
- writes newline to that gadget's UDC, waits 200 ms, and restores its original
  UDC name, including on a handled stop or sleep error;
- limits attempts to three per observed VBUS session, at least 60 s apart;
  leaves an unbound gadget or a new binding established by another owner alone.

It never writes a charger current limit. A rebind temporarily interrupts
**both** ECM and ACM. NBD uses the same USB transport. This is a bounded
fallback, not a substitute for the kernel fix. It cannot see macOS's en14
carrier when Linux's usb0 carrier remains 1; that case is handled by the
kernel notification fix and requires hardware validation. Do not infer USB
health from the administrative `UP` flag alone.

## Offline validation and reproduction

```sh
scripts/periph/check-usb-resume.sh
scripts/periph/export-usb-resume.sh
scripts/periph/build-usb-resume-integration.sh --check-only
scripts/periph/configure-kernel.sh
scripts/build-kernel.sh --tree periph
scripts/periph/check-usb-resume-build.sh
```

The first script snapshots real kernel functions. ASan/UBSan tests reject
the original DWC2 ordering and ECM lost-CONNECT behavior and pass the fixed
functions, including SPEED retry, resume with a request in flight, and closed
link / shutdown. It also runs the 160 USB-budget assertions, Tristar's 4096
ADC fixtures and 13 routing faults, existing charger guard / rollback tests,
and eleven fake-sysfs recovery scenarios. The integration check uses a private
Git index to replay onto both current `a9/integ` and the exact tested
`d5822a7f71cd + round8e` overlay; it does not modify either checkout.

Build and test records are under `build/periph/usb-resume/`. The standalone
build outputs are `build/kernel-periph/{Image.gz,dtbs,config,COMMIT,RELEASE}`.
The completed build is clean at `46864bf126`, release
`7.3.0-rc1-g46864bf12648`, with Image.gz and 104 DTBs. The linked-driver / DT
checks and embedded-release verification pass; exported patches have zero
strict-checkpatch errors, warnings or checks. Systemd unit verification and
the eleven recovery tests pass. The complete round8e integration was replayed
in a private index; its additional full image build is left to the orchestrator.
Verify the exported image, J71t DTB, config, patches and recovery archive with:

```sh
shasum -a 256 -c build/periph/usb-resume/SHA256SUMS
```

That standalone peripheral kernel does not include the complete boot/NVMe
integration; use the following integration build for the iPad.

## Orchestrator commands (not executed against the device)

Build the exact round8e baseline plus the two USB patches, in a **new detached**
`usbresume-check` tree. Existing boot and integration checkouts are preserved.
This needs space for another kernel checkout/build in the Docker volume.

```sh
scripts/periph/build-usb-resume-integration.sh
```

It builds `build/kernel-usbresume-check/`, verifies that the config and J71t
DTB match the tested round8e build, and records the entire applied overlay
in `build/periph/usb-resume/integration.patch` and the output `SOURCE.patch`.
Use its Image.gz with the existing round8e handoff entry's initramfs and
command line, giving the resulting entry a fresh name. Alternatively, to
land the fixes in the orchestrator's integration branch:

```sh
scripts/build-kernel.sh --run integ '
set -euo pipefail
test -z "$(git status --porcelain)"
git cherry-pick 078d2de49b48abb6ce1c1fbe78a6ec7a6d7f5cb3 46864bf1264874b31f9eff73dda5f1b529ca013a
'
scripts/omarchy/build-kernel.sh integ
```

Install only the recovery files on a running rootfs, through a working
transport (set `OMARCHY_HOST` to its Wi-Fi IP if ECM is already broken):

```sh
scripts/omarchy/ssh.sh 'sudo tar -xf - -C / && sudo systemctl daemon-reload && sudo systemctl enable --now linux-a9-usb-recover.service' < build/periph/usb-resume/rootfs-recovery.tar
scripts/omarchy/ssh.sh 'systemctl status linux-a9-usb-recover.service --no-pager; journalctl -b -u linux-a9-usb-recover.service --no-pager'
```

The archive is generated by `export-usb-resume.sh`, with root ownership and
executable helper permissions. The broader `scripts/omarchy/hot-apply.sh`
installs it too. No host-side mount
or edit of a possibly live rootfs image is needed. To disable recovery:
`sudo systemctl disable --now linux-a9-usb-recover.service` on the iPad.

On the new kernel, observe past the original 48 s window and exercise a
host USB suspend/resume while retaining ACM/Wi-Fi recovery. Record the Linux
UDC state, usb0 carrier and `usb/{online,current_max}`, plus macOS en14 media
status. Confirm ECM passes packets, the resume-time ACM `-11` is absent,
and the charger can still revoke a suspended-bus override. First check the
kernel with the recovery service stopped; then enable the fallback. A host
inactive / local carrier=1 failure is especially useful to distinguish from
the service's carrier=0 trigger.
