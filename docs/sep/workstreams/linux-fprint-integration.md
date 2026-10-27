# Linux fprint integration: /dev/sep-bio → libfprint → fprintd → PAM/sudo → Omarchy

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

Out of scope (other workstreams, referenced not duplicated): SBIO transport and
xART (`docs/sep/touchid-next.md`, `docs/sep/linux-bringup-next.md`), Mesa
sensor front end on `spi2@A088000`, the 68-byte EnrollMode/MatchMode payload RE
(`libBKDM1.dylib`), SKS/keybag protocol itself (`docs/sep/keybag.md`).

Confidence labels: **[V]** verified (source read / trace / live log),
**[I]** strong inference, **[D]** design choice made here, **[U]** unknown.
"aurora.c" means the upstream driver
`aurora-silicon/aurora-sep-userspace`, file `libfprint/libfprint-1.94.100-apple-sep.patch`
(blob `bdb15bb1`, from `aurora-silicon/linux@3b2bd35a8591`), fetched 2026-10-26.

## Safety (unchanged, inherited)

Never write syscfg, NVRAM, or boot-firmware storage. No unbounded or guessed
SEP messages; this workstream invents **no new SEP opcodes** — the only real
SEP traffic it ever consumes is the existing debugfs-gated sequence owned by
the transport workstream. All SEP-touching device steps below are proposals for
the orchestrator's guarded plans; nothing here was executed against the device.
The mock engine is pure in-kernel state and must provably not move the SEP
`tx`/`rx` counters.

## Parent audit status

The Aurora ABI audit is accepted, but this document is not yet authorization
to wire real enroll or verify into `sep-a9.c`. Three corrections govern the
implementation work:

- Durable enrollment is an AP-owned opaque catacomb export, not data that
  remains exclusively inside the SEP. The verified save order is `0x6c`
  export → durable write/fsync → `0x37` confirm → `0x33` ID → `0x36` hash;
  host `CompleteSaveCatacomb` has no SEP opcode.
- The first mock backend is a separate `sep-bio-mock` misc module. It must run
  with the real SEP driver absent, make no MMIO/SPI/storage calls, and identify
  every success as synthetic test behavior. This keeps ABI validation out of
  the hardware driver's failure domain.
- Real integration remains **NO-GO** until `/dev/sep-bio` has single-owner
  open/release cancellation, pollable event generations, bounded async/reply
  framing, and reset/lifetime semantics. Passing the mock does not satisfy the
  Mesa, SBIO, SKS, or persistence gates.

## 1. Decision: reuse Aurora's libfprint driver; no native plugin

Reuse `libfprint-1.94.100-apple-sep.patch` as-is, maintain a small A9 delta
patch queue around it, and do **not** write a native `drivers/sep` libfprint
plugin.

| option | verdict | why |
|---|---|---|
| Verbatim upstream patch (0001) + retry patch (0002) | **chosen** | Our UAPI is byte-identical (vendored `drivers/sep/uapi/apple-sep.h` is already a superset: it adds `SEP_BIO_INFO_LIVE_COUNT_VALID` semantics in `reserved[0]/[1]`, which aurora.c reads). The driver's hard logic — nonce binding, token/deadline refusal, empty-gallery identify shortcut, label encoding via `fpi_print_generate_user_id`, UUID-not-print comparison, errno mapping — is 881 lines of already-correct, LGPL-2.1 code. Duplicating it forks the security-relevant matching-acceptance path. **[V]** (patch fetched and read; header compared line-by-line) |
| Native plugin (`drivers/sep/sepfprint.c`) | rejected | No functional gain: the ABI was cloned precisely so their driver works. It would duplicate the "single place a match may be declared" logic (`aurora_result_is_match`), risking divergence in the fail-closed rules. Only becomes attractive if the A9 kernel needs semantics their driver cannot express within the v4 ABI — none identified. |
| Port their *fprintd*/Omarchy UI | already moot | Stock fprintd, stock D-Bus/polkit, stock Omarchy wizard and lock screen (`docs/sep/userspace.md`). |

Delta queue around the upstream patches **[D]**:

- apply upstream 0001 (`aurora` driver + misc-udev subtype) verbatim;
- apply upstream 0002 (kernel `RETRY` status → libfprint retry during
  enrol/verify/identify) — useful for 1st-gen Touch ID per
  `docs/sep/userspace.md`; apply verbatim;
- **skip** upstream 0003 (lid-covered reader) — no lid on j71t; skipping also
  avoids `DeviceAllow=char-input r` in the fprintd drop-in;
- ours: `0004` only if a real A9 gap appears (candidates: none known today;
  the reader-name/mis-detection fix in §7 would be the first).

Patch queue lives at `patches/sep-userspace/` (parallel to `patches/sep/`
kernel patches); upstream patches are pinned by the blob/commit above and
copied verbatim, so the queue stays reviewable against upstream.

## 2. Contract audit: what aurora.c requires from the kernel

Read from the upstream source; each row is what the driver *does*, not what we
guessed. Current `drivers/sep/sep-a9.c` state from reading it directly.

| # | UAPI element | aurora.c behavior | current sep-a9.c | gap |
|---|---|---|---|---|
| 1 | `GET_INFO` | probe: requires `version == 4` and `sensor_present != 0`, else the reader is not exported (fail-closed). Sets enroll stages from `enroll_stages` if > 0 (class default 8). Also read mid-flight by the identify fast path. | `version=4`, `sensor_present = sbio_ready` (set only by debugfs `xfer 73`), `enrolled=0 capacity=0 stages=0` | none for probe; `capacity/enrolled/stages` must be filled by the real engine |
| 2 | fd readiness | enroll/verify register `g_unix_fd_add(fd, G_IO_IN, …)`; `*_POLL` ioctls are called **only when the fd is readable**, repeatedly, while state is `PENDING/PROGRESS` | fops = `{owner, open, unlocked_ioctl, llseek}` — **no `.poll`** | **critical**: without `.poll` + waitqueue wakeups, every operation hangs after `START` forever |
| 3 | single open | open comment: "the kernel admits one opener at a time; EBUSY means something else holds the sensor" → `FP_DEVICE_ERROR_BUSY` | `sep_bio_open` sets `private_data`, always succeeds | enforce `-EBUSY` on second concurrent open |
| 4 | close cancels | `aurora_close`: "closing the descriptor cancels anything in flight, by contract"; also zeroes the nonce | no `.release` | implement release→cancel |
| 5 | `CANCEL` | called unconditionally, errors ignored (must be idempotent, cheap) | `-ENOSYS` | implement |
| 6 | errno mapping | `ENODEV`→REMOVED, `EPERM/EACCES`→NOT_SUPPORTED, `ENOSPC`→DATA_FULL, `ENOENT`→DATA_NOT_FOUND, default→PROTO | partial (only `ENODEV`/`-ENOSYS` paths exist) | kernel must emit exactly this set; note `EACCES` doubles as keybag-locked (`docs/sep/keybag.md`) |
| 7 | enroll progress | `PROGRESS` + `stage > last_reported` → one `fpi_device_enroll_progress`; guidance → finger-status (`PLACE/LIFT_AND_MOVE`→NEEDED, `HOLD_STILL`→PRESENT); `DONE` → print carries the 16-byte UUID; `FAILED` → error incl. `status` | `ENROL_POLL` always returns `IDLE` | state machine + wakeups |
| 8 | verify semantics | `DONE`+`MATCH`+nonzero token+`deadline_ns` (CLOCK_MONOTONIC) in the future ⇒ match; `NOT_COMPARED` ⇒ *error*, never FAIL; otherwise FAIL. Nonce from `getrandom`, sent in `VERIFY_START` | `VERIFY_POLL` always `IDLE`/`NOT_COMPARED` | engine must fill `result/uuid/token/deadline_ns` |
| 9 | identify fast paths | empty gallery ⇒ immediate no-match **without kernel touch** (this is what makes the *first* enrolment possible); non-empty gallery + `reserved[0] & SEP_BIO_INFO_LIVE_COUNT_VALID` + `reserved[1] == 0` ⇒ no-match without capture | header supports the flag; driver never sets it | engine must maintain a live SEP identity count for exactly this reconciliation |
| 10 | labels | host-side only: `fpi_print_generate_user_id(print)` ≤ 127 bytes into `ENROL_START.label`; device label in `LIST` is untrusted bytes, bounded before use | empty list | nothing kernel-side beyond carrying 128-byte labels opaquely |
| 11 | delete/clear | `DELETE` by UUID (`ENOENT` if absent), `DELETE_ALL` | `-ENOSYS` | implement against the identity store |

**[V]** rows 1–11 are quoted/paraphrased from the fetched patch. The identify
fast path (row 9) is why `LIST`'s contract matters even when verify works: a
stale fprintd-side print index after an SEP-side bag loss must not wedge
enrolment — aurora.c already handles it if the kernel reports the live count
honestly.

## 3. Kernel-side UAPI spec (the next `drivers/sep/sep-a9.c` work)

All **[D]** unless noted. Nothing here sends anything to the SEP by itself;
the engine is fed by the transport layer's `sep_transfer()` and the SBIO
state machine from the sibling workstream.

### 3.1 open / release

- `open()`: atomic single-open guard (`test_and_set_bit` on an `bio_open`
  flag). Second concurrent open ⇒ `-EBUSY`. No SEP access, no keybag load (see
  §3.7 for activation). If `sbio_ready` is false the open still succeeds;
  `GET_INFO` reports `sensor_present=0` and libfprint's probe refuses to
  export the reader — the fail-closed path already proven by aurora.c's probe
  behavior **[V]**.
- `open` does *not* mirror Aurora's "activation: loads the keybag and restores
  enrolments" (`docs/sep/userspace.md`): on A9, activation is the
  debugfs-gated discovery→OOL→0x73 sequence until the auto mode in §3.7 is
  earned. Restoring enrolments happens on first `LIST`/operation instead.
- `.release`: run the cancel path (below), wake all waiters, drop the flag.
  If an operation was in flight and its cancel could not complete (transport
  timeout), the client is marked dead and the operation watchdog (§3.4)
  owns the cleanup; the next open must still succeed with a clean `IDLE`.
- `.poll` (new): `EPOLLIN | EPOLLERR` when a poll snapshot has news (state,
  guidance, progress, or result changed since the last `*_POLL` read) or an
  error is latched; 0 otherwise. The fd is the sleep primitive; `*_POLL`
  ioctls themselves never block.

### 3.2 Poll ioctls

`ENROL_POLL` / `VERIFY_POLL` are non-blocking snapshots: copy the current
operation struct under the state lock, clear the "news" latch, return 0. They
are safe to call in any state (aurora.c calls them on every fd wakeup) and
must return `IDLE` when no operation is active. `ENROL_START` /
`VERIFY_START` while an operation is active ⇒ `-EBUSY`; with SBIO not ready ⇒
`-ENOSYS`; with the keybag locked ⇒ `-EACCES` (`docs/sep/keybag.md`).

### 3.3 Cancel

`SEP_BIO_CANCEL` is idempotent and cheap: if an operation is active, send SBIO
`Cancel 0x0b` (verified opcode, `docs/sep/sbio-commands.md` table) through one
single-chunk transfer with the standard 10 s timeout, transition the state
machine to `IDLE`, wake the queue. If none is active, return 0. The same path
serves `.release` and the watchdog.

### 3.4 Threading, timeouts, watchdog

- One operation at a time per device (the SEP is one engine). The operation
  loop — EnrollMode/MatchMode, per-frame `0x06` config, chunked `0x65` relay,
  `0x22`/`0x07` async fetches, catacomb save trio — runs in a `work_struct`
  kicked by `*_START`, never in ioctl context. The existing 10 ms poller +
  `xfer_done` completion + `cfg_lock/tx_lock` structure in `sep-a9.c` is the
  substrate; unsolicited SBIO messages from the poller feed the same engine.
- Per-SEP-command timeout: 10 s, matching XNU's
  `AppleSEPGenericTransfer::transfer` (10 s lock + 10 s completion,
  `0xfffffff006690140`, fields `+0xa4` command echo, `+0xa8` error status,
  `+0x18` bytes received, `+0x30` request state) **[V]**. Transport failure
  surfaces as `state=FAILED` + opaque `status` (aurora.c prints it), never as
  a silent hang.
- User-wait has **no** kernel timeout: enrolment legitimately waits minutes
  for finger placement; the lock screen / fprintd client owns UX timeouts.
  **[D]**
- Watchdog **[D]**: if the engine is waiting for an SEP reply (not for the
  user) and none arrives within the 10 s transport timeout, fail the operation
  (`FAILED`, status = driver-internal timeout marker) and auto-cancel. This
  guarantees the single-open slot can never be wedged by a dead sepOS. Mock
  `stuck` (§8) drills exactly this.

### 3.5 Unsolicited SBIO events

Async result-ready notifications drive `0x22`/`0x07` fetches
(`docs/sep/sbio-commands.md`); they map to poll wakeups (state/guidance
changes). `DisplayStatusChanged` is *local* Mesa state, never `0x09`
(`docs/sep/linux-bringup-next.md` §3C) — the engine keeps a local display
state flag that gates auto-match conditions. **[V]** (kernelcache-verified
semantics documented there.)

### 3.6 Keybag gating (from `docs/sep/keybag.md`, restated as UAPI rules)

- New ioctls extend v4 without breaking it (`SEP_BIO_KEYBAG_STATUS/UNLOCK/
  LOCK`, magic `0xB1`, next free numbers after `0x0a`). `GET_INFO.version`
  stays 4 — aurora.c checks equality only. **[V]**
- Bag locked ⇒ `sensor_present=0` in `GET_INFO` and `-EACCES` from
  `ENROL_START/VERIFY_START`. fprintd then reports no reader: fail-closed,
  exactly the keybag.md model. First unlock after boot is the password
  (PAM, §5); after that, fingerprint.
- On any `sensor_present` 0→1 transition the kernel emits a uevent on the
  misc device (`kobject_uevent(KOBJ_CHANGE)`, env `SEP_BIO_SENSOR=1`) so
  libfprint's udev client re-probes and fprintd exports the reader
  (`InterfacesAdded`). **[I]** that GUdevClient reacts to `change` uevents;
  this is deliberately a mock-verified assumption (§8 S5). Fallback if it
  does not: unregister/re-register the misc device on the transition
  (heavier, guaranteed `add` event).

### 3.7 Activation model (staged)

- Today (bring-up): `/dev/sep-bio` is live only after the orchestrator's
  manual `sbio` → `ool` → `xfer 73` sequence; `sbio_ready` gates
  `sensor_present`. Unchanged by this workstream.
- Next: kernel auto-activation (`sep_a9.bio=1`, default off) doing
  discovery → select `sbio` → publish OOL → `0x73` internally. Blocked on the
  warm-kexec OOL-stickiness handoff (one OOL binding per sepOS boot,
  `docs/sep/linux-live-test.md` §4 warning): auto mode is only safe on
  cold-boot sessions until OOL metadata persistence exists. **[V]** (OOL
  one-shot semantics) + **[D]** (gating).
- Aurora's "reboot rather than reload" rule applies to us too
  (`docs/sep/userspace.md`, upstream README): the SEP attach is once per boot;
  no reload-retry logic in userspace.

## 4. Enrollment-record ownership

| artifact | owner | location | notes |
|---|---|---|---|
| live fingerprint templates | SEP working catacomb | sepOS memory while active | matching stays inside sepOS; this does **not** mean the durable representation stays inside the SEP **[V]** |
| identity UUID + label | SEP identity set | exposed via `LIST` | host label = `fpi_print_generate_user_id` (user, finger, date) **[V]** |
| user→print mapping | fprintd | `/var/lib/fprint/<user>/apple-sep/…` | `fpi_print_set_device_stored(TRUE)`: host stores only references **[V]** |
| exported catacomb blob | AP, root-only durable store | proposed `/var/lib/sep-a9/catacomb/` | sepOS exports opaque template material with `0x6c`; iOS persists it under `Library/Catacomb` and restores it later **[V]**; the Linux path/container is **[D]** |
| bag secret | never on disk (B1) / wrapped (B2) | `docs/sep/keybag.md` | out of scope here |

- **[U]** The SEP opcodes behind `LIST`/`DELETE`/`DELETE_ALL` were never
  captured (first-enrol trace had no delete). The Mesa-layer identity-list
  flow is a gap the mock fills on the host side meanwhile; recovering them is
  transport/sensor-workstream RE work (`biometrickitd`/`libBKDM1.dylib`
  callers of `AppleBiometricServicesUserClient::extPerform`).
- Catacomb sync: during bring-up, the blob is dumped/loaded via a guarded
  transport surface. Production **[D]**: `0x6c` exports the blob, the AP must
  durably write and fsync it, and only then may it send `0x37` to confirm;
  `0x33` gets the catacomb ID and `0x36` gets its hash afterwards. Host
  `CompleteSaveCatacomb` performs no SEP transfer. A future kernel path can
  emit `KOBJ_CHANGE` with `SEP_BIO_CATACOMB_DIRTY=1`; a root oneshot
  `sep-catacomb-sync.service` (udev-triggered) reads the blob via a root-only
  ioctl and writes `/var/lib/sep-a9/catacomb.bin` atomically. No daemon, no
  fprintd involvement, no biometric bytes in any non-root-readable place.
- OS-identity binding: Aurora pins the SEP OS identity with
  `apple_sep.os_uuid_hi/lo` (upstream README). Our equivalent anchor is the
  keybag itself; whether the bag handle/UUID survives reboot (keybag.md open
  question 3) decides whether we need an `os_uuid` analog. **[U]**
- `capacity`/`enroll_stages` in `GET_INFO` still need a separate source.
  `GetProtectedConfig 0x2b` is now decoded and contains only configured and
  effective unlock/identification/login/Apple Pay flags; it does not carry
  either value. **[V]**
  Aurora's kernel reports three slots (`ENOSPC` ⇒ "three SEP fingerprint
  slots are full") **[V]**; A9's real capacity is unknown until the layout is
  decoded. Mock uses 3/8.
- Reconciliation: after any SEP-side bag loss, `GET_INFO.reserved[0]` must
  carry `SEP_BIO_INFO_LIVE_COUNT_VALID` with a true `reserved[1]` count so
  aurora.c's identify fast path resolves a stale fprintd index instead of
  hanging enrolment. **[V]** that this is the upstream mechanism; **[D]** that
  our driver must maintain it.

## 5. Permissions, polkit, PAM boundaries

### 5.1 Device and daemon

- `/dev/sep-bio` stays misc `0600` root:root **[V]** (current driver). fprintd
  runs as root; every other consumer (`sep-bio-test`, `sep-keybag`) is root or
  explicit `sudo`. No udev permission rules — this is not a user-device.
- fprintd drop-in (our package): `/usr/lib/systemd/system/fprintd.service.d/
  30-sep-a9.conf` with `DeviceAllow=/dev/sep-bio rw` only (upstream's
  `char-input r` was for the lid patch we skip). Inert unless the unit sets
  `DevicePolicy=closed`; harmless either way, and future-proof against a
  hardened upstream unit. **[I]**
- D-Bus `net.reactivated.Fprint`, stock polkit actions with
  `allow_active=yes` defaults: no new actions, no new bus name **[V]**
  (`docs/sep/userspace.md`).

### 5.2 PAM layout (sudo, polkit, Omarchy lock)

Target stacks, per `docs/sep/userspace.md` and `docs/sep/keybag.md`:

```
# /etc/pam.d/sudo, /etc/pam.d/polkit-1 (written by omarchy-setup-security-fingerprint)
auth sufficient pam_fprintd.so
# with the keybag landed, one line before it:
auth [success=1 default=ignore] pam_exec.so quiet expose_authtok /usr/libexec/sep-keybag pam-unlock
```

- `pam_fprintd` when the bag is locked: fprintd reports no device
  (`sensor_present=0`), the module fails fast, the stack falls through to the
  password; `pam_exec expose_authtok` captures that password and unlocks the
  bag (helper exits 0 unconditionally so it can never block password auth;
  password arrives on stdin only; the helper wipes its buffer and makes one
  `KEYBAG_UNLOCK` ioctl). Next unlock offers the fingerprint — the iOS shape
  from `docs/sep/keybag.md`. **[V]** (documented design), **[D]** ordering here.
- **`seteuid` tension to resolve [D]**: keybag.md's snippet runs the helper
  with `seteuid` (user euid), but `KEYBAG_UNLOCK` needs root for the 0600
  node, and keybag.md also calls the helper "root, /usr/libexec". Resolution:
  drop `seteuid` (helper runs as root, reads stdin only), which requires the
  PAM conversation itself to be privileged. For `sudo`/`polkit` that holds;
  for the Quickshell lock screen's `PamContext` it is **[U]** whether PAM runs
  privileged there (hyprlock-class lockers vary). S5/S8 mock drills decide:
  if the lock-screen conversation is unprivileged, the fallback is a
  user-callable, polkit-guarded D-Bus `unlock` method on a tiny system
  service instead of direct ioctl.
- `omarchy-lock-fingerprint` (`auth required pam_fprintd.so`) is written by
  `omarchy-apply-lock` only when `fprintd-list "$USER"` shows an enrolled
  finger; the Quickshell lock runs it. We consume this file; we never write
  it. **[V]** (`docs/sep/userspace.md`)
- SDDM has no fingerprint support **[V]** — "Omarchy login" honestly means:
  lock-screen unlock, `sudo`, and polkit prompts. The boot autologin is
  unaffected.

### 5.3 The passwordless-image precondition (integration blocker, must be surfaced)

The running image has user `linux` with **no password**, sudo NOPASSWD via
`90-linux-a9-temp`, root locked (`docs/omarchy/README.md` §"linux-a9 layer").
Two consequences **[I]**:

1. With `NOPASSWD`, sudo does not need a password conversation, so the
   `pam_fprintd` line is likely never reached (`pam_authenticate` skipped
   when no authtok is required) — the whole sudo fingerprint flow only
   becomes real once the owner sets a password and removes the NOPASSWD
   drop-in (both already labelled for removal). S3 verifies this empirically
   rather than trusting it.
2. Keybag option B (password → bag) requires a real password to exist at all.
   Until then the bag stays locked ⇒ no fingerprint, which is the correct
   fail-closed behavior, and exactly what the mock drills.

## 6. Packaging, services, concrete targets

Nothing below exists yet; all are new files owned by this workstream. No
existing integration file is modified in this pass.

| target | role |
|---|---|
| `scripts/sep/build-fprint-pkgs.sh` | builds packages in the `linux-a9-alarm:latest` container (same pattern as `scripts/omarchy/fetch-live-pkgs.sh`): `libfprint-a9` from the 1.94.100 source + `patches/sep-userspace/` queue (pinned upstream 0001/0002 + ours), UAPI header copied at build time from `drivers/sep/uapi/apple-sep.h` (single source of truth, mirroring upstream's build-time `apple-sep.h`); `provides=("libfprint=1:1.94.100" "libfprint-2.so")`, `conflicts=(libfprint)`, release suffix sorts above the distro build |
| `patches/sep-userspace/` | the patch queue (§1) |
| `apple-sep-userspace-a9` (package, built by the same script) | fprintd drop-in `30-sep-a9.conf`; `/usr/share/omarchy-platform/fingerprint-readers` entry for the iPad reader (**[U]** exact format — S2 inspects the file on device; it is the documented upstream hook, `docs/sep/userspace.md`); later `sep-keybag` + `sep-catacomb-sync` units |
| `fprintd` | stock ALARM repo package, unpatched — `pacman -Sw` alongside, supplies `pam_fprintd.so` |
| delivery | `scp` + `sudo pacman -U` per `fetch-live-pkgs.sh`'s documented offline path (`docs/omarchy/round3-live.md`), or a new `scripts/omarchy/rootfs/` phase in the next image build |
| kernel | standalone test-only `drivers/sep/sep-bio-mock.c` first; the real §3 semantics move into `sep-a9.c` only after the mock conformance gate passes |
| `tools/sep/sep-bio-conformance.c` (new) | UAPI conformance harness (§8 S0); `tools/sep/sep-bio-test.c` stays as the manual smoke tool |
| `tools/sep/pam-check.c` (new) | 10-line `pam_start`/`pam_authenticate` runner against a test PAM service, so PAM ordering is testable without `pamtester` or touching live stacks |

Service changes: none beyond the drop-in. fprintd stays D-Bus-activated; no
new units, no new udev rules (the kernel's uevent needs none).

> Private graphics-dependent excerpt omitted from this source snapshot.

## 7. The mock engine

**[D]** Implemented as a standalone test-only misc module in
`drivers/sep/sep-bio-mock.c`, with explicit `TEST-ONLY` / fake-RAM-state dmesg
lines. It registers `/dev/sep-bio` only when the real driver is absent; a name
conflict fails cleanly at `misc_register()`. Keeping it separate makes “no
hardware access” statically reviewable and prevents mock state from sharing the
real transport's lifetime. The exact implementation and validation record is
`docs/sep/workstreams/sep-bio-mock-implementation.md`.

- Loading the mock must require neither a platform device nor `sep_a9.enable`.
  The module contains no SEP/SPI/PMGR access, so it is safe in QEMU
  (`scripts/omarchy/qemu-smoke.sh`) and on the no-SEP-chain boot kernels.
- `GET_INFO`: `version=4 sensor_present=1 enrolled=<n> capacity=3
  enroll_stages=8`.
- `ENROL_START`: latch label, kick the engine work item; per 300 ms tick:
  `stage++`, guidance cycles `PLACE`→`HOLD_STILL`→`LIFT_AND_MOVE`,
  `progress_percent = stage*100/total`. The fake UUID is a deterministic,
  explicitly non-cryptographic mix of the label and a per-boot counter. After
  the last stage: `DONE` + UUID, identity added to the mock set. A fourth
  `ENROL_START` returns `-ENOSPC` before an operation is created.
- `VERIFY_START`: latch the 32-byte nonce. After one tick: `DONE`,
  `result=MATCH` iff the mock set is non-empty and the debugfs `finger` switch
  is on; the fake token is a deterministic, non-cryptographic mix of nonce,
  UUID, and a per-boot counter, and `deadline_ns` is monotonic now + 5 s.
  Finger absent ⇒ `NO_MATCH`. Match returns identity 0's fake UUID.
- `CANCEL`/release/watchdog/state rules exactly per §3 — that is the point:
  the mock exists to prove the *contract*, not to fake results.
- Debugfs `/sys/kernel/debug/sep-bio-mock/control` (0600) accepts exactly:
  `finger on|off`, `gone on|off`, `stuck on|off`, `fail <hex status>`, and
  `reset`. There is deliberately no guessed `retry`, keybag lock, or forced
  capacity protocol; `fail` exercises opaque FAILED-status handling without
  assigning a real SEP meaning to a number.
- Static boundary: the module includes no SEP, Mesa/SPI, PMGR, regulator,
  storage, or MMIO interface. It cannot change the real driver's `tx`/`rx`
  counters because the real driver is absent when the mock owns the misc name.

## 8. Staged mock-device test plan

> Private graphics-dependent excerpt omitted from this source snapshot.

### S0 — UAPI conformance (mock or inert kernel, no SEP)

> Private graphics-dependent excerpt omitted from this source snapshot.

### S1 — mock enrol/verify through `sep-bio-test`

Run `tools/sep/sep-bio-conformance` against the mock first without debugfs,
then with `--control /sys/kernel/debug/sep-bio-mock/control`: 8 stages,
guidance lines, DONE with a fake UUID, verify match + non-match, bounded
stuck/cancel, opaque failure, gone-sensor, fatal-poll, reset, list, and delete
drills. `sep-bio-test info|enrol|verify|list` remains an optional manual smoke
tool. Exit criteria: the conformance transcript is green and identifies the
backend as the mock before any mutating ioctl.

> Private graphics section omitted from this source snapshot.

> Private graphics section omitted from this source snapshot.

> Private graphics section omitted from this source snapshot.

### S5 — keybag fail-closed drill (mock)

1. `echo lock > /sys/kernel/debug/sep-a9/mock` ⇒ `sensor_present=0`,
   enrol/verify `-EACCES`, `fprintd-list` shows no device (restart or
   uevent), `pam_fprintd` falls through — the whole fail-closed chain.
2. `echo unlock > …` ⇒ assert the `KOBJ_CHANGE` uevent is seen
   (`udevadm monitor`) and that fprintd exports the reader again without a
   restart. If not, the §3.6 fallback (misc re-register) is the next design
   task — this is the deliberate de-risking of the assumption.
3. Optional: `pam_exec` stand-in (`/usr/libexec/sep-keybag` prototype calling
   the mock lock state) to rehearse the password→unlock→reader-appears flow
   and the PAM-privilege question of §5.2.

### S6 — guarded, real SEP (consumes, never invents)

Only after the transport workstream lands real enrol/verify payloads
(`docs/sep/touchid-next.md` progression). This workstream's steps are
passive-consumption only:

1. Boot per the existing guarded plans (fresh DFU, `sep_a9.enable=1`,
   discovery → `sbio` → `ool` → `xfer 73`; `sensor_present` becomes honest).
2. Assert `mock=0` in `status` before any operation.
3. `sep-bio-test info` (sensor_present=1) → S0 conformance subset →
   `sep-bio-test enrol` only when the 68-byte EnrollMode layout is recovered
   (zeroed payloads prove nothing and are not sent;
   `docs/sep/linux-bringup-next.md` §4).
4. Stop rules inherited verbatim from `docs/sep/touchid-next.md`: no OOL
   republish, no `0x73` retry, no second SBIO command in the first boot, one
   attempt per step, cold reset at the end, nothing to
   syscfg/NVRAM/boot-firmware.

## 9. Unknowns and open questions

- SEP opcodes for identity `LIST`/`DELETE` (never captured) — **[U]**; mock
  defines the host contract meanwhile.
- Honest sources for `GET_INFO.capacity/enroll_stages` — **[U]**;
  `GetProtectedConfig 0x2b` has been ruled out by its recovered 32-byte
  protected-policy layout.
- Token TTL source (`deadline_ns`) from a real SEP match — **[U]**; mock uses
  5 s, aurora.c only requires a future monotonic deadline.
- Whether libfprint's GUdevClient re-probes on `change` uevents — **[U]**,
  settled by S5.
- Whether Quickshell's `PamContext` runs PAM privileged — **[U]**, settles
  §5.2's helper-privilege choice.
- Exact `/usr/share/omarchy-platform/fingerprint-readers` format — **[U]**,
  settled by S2.
- Live-count maintenance for the identify fast path before `LIST` exists —
  **[U]**; at minimum the driver must not claim
  `SEP_BIO_INFO_LIVE_COUNT_VALID` when it cannot count (aurora.c treats an
  unavailable count correctly by not trusting zero) **[V]**.

## 10. Evidence anchors

> Private graphics-dependent excerpt omitted from this source snapshot.

