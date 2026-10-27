# Touch ID userspace: reuse Aurora's stack, not their UI

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Researched 2026-10-23. The short answer: **there is no Aurora-specific UI to
port.** Aurora Silicon plugged into fprintd, and Omarchy's fingerprint UI is
already generic. What is worth copying is their kernel ABI, because that is
what makes the existing libfprint driver and the whole desktop stack work
without changes.

## What Aurora built

| layer | artifact |
|---|---|
| kernel | `apple_sep` driver in [`aurora-silicon/linux`](https://github.com/aurora-silicon/linux) branch `aurora-wip`; exposes **`/dev/sep-bio`** (misc, 0600) with ioctl ABI [`include/uapi/linux/apple-sep.h`](https://github.com/aurora-silicon/linux/blob/aurora-wip/include/uapi/linux/apple-sep.h), interface **version 4**, magic `0xB1` |
| libfprint | [`aurora-silicon/aurora-sep-userspace`](https://github.com/aurora-silicon/aurora-sep-userspace): Fedora libfprint spec + `libfprint-1.94.100-apple-sep.patch` adding `drivers/aurora/aurora.c`, reader name *"Apple secure enclave fingerprint sensor"* |
| fprintd | stock fprintd, plus drop-in `DeviceAllow=/dev/sep-bio rw` |
| D-Bus | stock `net.reactivated.Fprint`; no new bus name, no polkit changes |

The ioctls are `GET_INFO`, `LIST`, `ENROL_START/POLL`, `VERIFY_START/POLL`,
`CANCEL`, `DELETE`, `DELETE_ALL`, `ATTEST`. Enrollment and verification are
start/poll state machines with `PLACE / LIFT_AND_MOVE / HOLD_STILL` guidance
and a progress percentage. `open()` is activation: it loads the keybag and
restores enrolments before returning. **No biometric data crosses the
interface** — the enclave matches; userspace gets a UUID, a label and a
one-shot token.

## What Omarchy does (v4, Quickshell lockscreen)

Omarchy does not talk D-Bus for fingerprints at all — it is PAM plus
`fprintd-list`:

- `bin/omarchy-apply-lock` writes `/etc/pam.d/omarchy-lock-fingerprint`
  (`auth required pam_fprintd.so`) **only when `fprintd-list "$USER"` shows an
  enrolled finger**.
- The lock screen runs that PAM service directly (`PamContext { config:
  "omarchy-lock-fingerprint" }`) and shows the fingerprint indicator.
- `bin/omarchy-setup-security-fingerprint` is the enrollment wizard: it detects
  a reader (`omarchy-hw-fingerprint`, `/usr/share/omarchy-platform/fingerprint-readers`),
  pipes `fprintd-enroll` / `fprintd-verify` with progress parsing, then edits
  `/etc/pam.d/sudo` and `/etc/pam.d/polkit-1` (`sufficient pam_fprintd.so`).
- SDDM has no fingerprint support.

So the entire UI contract is: `fprintd` running, `/dev/sep-bio` present, a
reader visible through libfprint, and `fprintd-enroll` working.

## What we did in the `sep` workstream

`drivers/sep/sep-a9.c` now:

1. vendors the Aurora UAPI header (`drivers/sep/uapi/apple-sep.h`, copied to
   `include/uapi/linux/apple-sep.h` at build time; licence is
   `GPL-2.0 WITH Linux-syscall-note OR MIT`, so vendoring is fine);
2. registers `/dev/sep-bio` with those ioctls;
3. returns `GET_INFO` with `version = 4` and `sensor_present = 0`, an empty
   `LIST`, and `-ENOSYS` for enrol/verify/delete/attest until the SBIO
   endpoint exists.

`sensor_present = 0` is deliberate: libfprint must not expose a reader it
cannot use, and fprintd must not offer fingerprint auth that would only time
out. The moment SBIO works, set it to 1 and the existing userspace stack comes
alive with no UI work.

## What we reuse verbatim once SBIO works

- `libfprint-1.94.100-apple-sep.patch` (the `aurora` driver). Our ioctl ABI is
  byte-identical, so the patch needs no changes. The clamshell/lid patch is
  irrelevant for an iPad; the retry patch is useful for 1st-gen Touch ID.
- The fprintd drop-in, adjusted to whatever device node/subsystem we end up
  with.
- Omarchy's `omarchy-setup-security-fingerprint`, `omarchy-apply-lock`,
  `omarchy-remove-security-fingerprint` and the Quickshell lock plugin — as-is.
- `/usr/share/omarchy-platform/fingerprint-readers` is the upstream hook for
  declaring a non-USB reader, which is exactly what the iPad is.

## What must be ported (and is not in Aurora)

- SBIO framing: their `drivers/soc/apple/sbio.rs` is Rust and profile-gated to
  `t8103/t6020/t6021/t6000/t6001/t8140/t8112`; `profile.rs` returns `-ENODEV`
  for anything else. There is no generic SEP layer, so A9 needs its own.
- Mesa on `spi2@A088000` (1st-gen Touch ID, `biosensor,mesa`) and the keybag
  (`sks`), which on A9 is the iOS user bag rather than an Aurora-created one.
- Trial safety: the FreeBSD/Aurora guidance is to run read-only first
  (`xart_writes=0 provision_keybag=0`). The A9 equivalent is: never touch the
  iOS user keybag until the driver can do read-only matches.

## Answer in one line

Use their **ABI** (`/dev/sep-bio`, v4 ioctls) and their **libfprint patch**;
Omarchy's fingerprint wizard, lock screen and PAM stacks then work with zero
UI changes. Nothing in their UI is Apple-Silicon-specific.
