# /dev/sep-bio mock ABI harness: implementation note

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Status: implemented in the shared checkout, **not committed**, **not run against
any device**. This is a test-only module and userspace conformance tool. It is
not a biometric backend and proves nothing about fingerprint security.

Companion documents:

- `docs/sep/workstreams/linux-fprint-integration.md` — Aurora/libfprint
  contract, userspace ownership, PAM/packaging plan.
- `drivers/sep/uapi/apple-sep.h` — the v4 ABI implemented here.
- `docs/sep/workstreams/sbio-engine-implementation.md` — the unrelated real
  GenericTransfer engine in `sep-a9.c`; this workstream does not touch it.

## 1. What this is, and what it is not

`sep-bio-mock` is a standalone misc module that registers `/dev/sep-bio`
(mode `0600`) and implements the v4 operation/state contract with synthetic
state. It works with the real `sep-a9` driver absent, disabled or not built.
It calls no SEP/Mesa transport function and includes no transport header.

It has **no** access to SEP MMIO/mailbox, SBIO, Mesa or SPI, PMGR, regulators,
NVRAM, storage, or biometric hardware. There is no catacomb blob, no keybag, no
attestation key, no matching engine and no durable identity store. "Identities"
are fake labels plus a non-cryptographic 16-byte tag in module RAM.

The real path is different. sepOS performs the matching and exports opaque
catacomb material; the AP is responsible for persisting that exported blob
durably (the iOS-side precedent is `Library/Catacomb`; the Linux container is a
design choice in the integration document). Templates therefore are **not**
exclusively "in the enclave" and this document does not repeat that stale
claim. The mock intentionally models none of that: it proves the host ABI and
state machine, not enrollment security, key ownership or template storage.

Distinguish throughout this note:

- **[V] observed Aurora ABI behavior** — read from the pinned upstream
  `libfprint/libfprint-1.94.100-apple-sep.patch` (`aurora.c`) and summarized in
  `linux-fprint-integration.md` §2.
- **[D] mock design choice** — chosen here to make the contract testable.
- **[U] unresolved real-hardware work** — not solved or simulated here.

## 2. Files

| file | role |
|---|---|
| `drivers/sep/sep-bio-mock.c` (new) | TEST-ONLY kernel misc module |
| `scripts/sep/build-bio-mock.sh` (new) | builds the object in a named Docker kernel worktree with its own Kconfig/Makefile symbol |
| `tools/sep/sep-bio-conformance.c` (new) | epoll-driven UAPI conformance harness |
| `tools/sep/Makefile` | adds only the `sep-bio-conformance` target; existing `sep-bio-test` target preserved |
| `docs/sep/workstreams/sep-bio-mock-implementation.md` (new) | this note |

> Private graphics-dependent excerpt omitted from this source snapshot.

## 3. Observed Aurora behavior versus mock choices

| area | Aurora behavior [V] | mock implementation |
|---|---|---|
| probe | `GET_INFO`; reader exported only when `version == 4` and `sensor_present != 0` | reports `version=4`, `sensor_present=1` unless the debugfs `gone` switch is on |
| readiness | enroll/verify wait for `G_IO_IN`; `*_POLL` is called only when the fd is readable | `.poll` + waitqueue; `EPOLLIN` only when a per-type news generation changed since that fd's successful `*_POLL` |
| single open | second `open()` is `-EBUSY` and reported as a busy device | mutex-guarded open slot; second open `-EBUSY` |
| close | closing the fd cancels anything in flight "by contract" | `.release` runs the same synchronous cancel path, clears `open`, frees the per-file context |
| cancel | `CANCEL` is unconditional and errors are ignored; must be idempotent | idempotent; transitions `IDLE`, wipes nonce/token/pending label, wakes waiters |
| errno set | `ENODEV`→removed, `EPERM/EACCES`→not supported, `ENOSPC`→full, `ENOENT`→not found, other→protocol | emits `EBUSY`, `ENODEV`, `ENOSPC`, `ENOENT`, `EINVAL`, `ENOTTY`, `ENOSYS`, `EFAULT` only |
| enrol progress | `PROGRESS` + `stage > last_reported` produces one progress event; guidance maps to finger status; `DONE` carries the UUID | 8 stages, monotonic progress, guidance cycles `PLACE`/`HOLD_STILL`/`LIFT_AND_MOVE` |
| verify acceptance | `DONE` + `MATCH` + nonzero token + future monotonic deadline; `NOT_COMPARED` is an error | `MATCH` only with at least one fake identity and `finger on`; token nonzero; deadline `CLOCK_MONOTONIC` now + 5 s |
| empty gallery | userspace short-circuits identify when its print list is empty | mock has no opinion; `VERIFY` on an empty store returns `NO_MATCH` |
| live count | `reserved[0] & SEP_BIO_INFO_LIVE_COUNT_VALID` with exact `reserved[1]` enables a no-capture fast path | flag deliberately **not** set: this backend cannot speak for a real SEP user context |
| labels | 128-byte host label is opaque/untrusted; userspace bounds it | NUL required on input; NUL forced on `LIST` output; stored only in RAM |
| delete | `DELETE` by UUID (`ENOENT` if absent), `DELETE_ALL` | same; both return `-EBUSY` while an operation is active |

### Mock design choices [D]

- Capacity 3 and 8 enrolment stages, matching the integration document's mock
  target. Real A9 capacity/stages are [U] until the `GetProtectedConfig 0x2b`
  reply layout is recovered.
- 300 ms per enrolment stage; one 100 ms verify step. No kernel-side user
  timeout for enrolment waiting; the conformance tool bounds readiness with
  epoll timeouts.
- Fake UUID and token are derived with an in-module 64-bit mixer, not SHA-256
  and not any crypto API. The integration document's earlier mock sketch
  suggested SHA-256; this implementation deliberately avoids a crypto
  dependency because the values are explicitly non-security test data.
- `GET_INFO.reserved[2] = 0x4d4f434b` ("MOCK" little endian) identifies the
  backend. `reserved[0]` and `reserved[1]` remain zero. This is a mock-only
  convention; production userspace must not depend on it.
- No keybag ioctls exist in this header and none were added. `ATTEST` remains
  `-ENOSYS`.
- All identities and the per-boot counters are volatile. Module unload wipes
  both; debugfs `reset` wipes identities but deliberately does not rewind the
  per-boot counters. Nothing is written outside module RAM.

## 4. Implemented ioctl semantics

### `GET_INFO`

`version=4`, `sensor_present = !gone`, `enrolled=<RAM count>`, `capacity=3`,
`enroll_stages=8`, `reserved[2]` mock marker. The live-count flag is not set.

### `LIST`

Returns at most `min(enrolled, capacity=3, SEP_BIO_MAX_IDENTITIES=32)` entries
in insertion order. Labels are copied with a forced terminating NUL; UUIDs are
the synthetic per-boot tags. There is no on-disk or SEP-backed identity store.

### `ENROL_START`

The full input struct is copied and validated: `flags` and `reserved` must be
zero, and the 128-byte label must contain a NUL. Rejections happen in this
order: active operation → `-EBUSY`, `gone` → `-ENODEV`, three identities already
present → `-ENOSPC`. A successful start copies the label, sets `PENDING`, bumps
the enrolment news generation, and schedules delayed work.

Each 300 ms tick advances one stage. Guidance is
`PLACE`, `HOLD_STILL`, `LIFT_AND_MOVE`, repeating; progress is
`stage*100/8`. The eighth tick appends an identity, assigns a deterministic
16-byte tag derived from the label and the per-boot enrolment counter, and
reports `DONE` with stage 8 / 100 %. Duplicate labels still get distinct tags.

### `ENROL_POLL` / `VERIFY_POLL`

Non-blocking snapshots. `*_POLL` copies the current snapshot to userspace and
only then acknowledges the generation captured before the copy. A faulted
`copy_to_user` returns `-EFAULT` and leaves the news generation unconsumed.
`VERIFY_POLL` after no operation returns `IDLE` / `NOT_COMPARED`.

### `.poll`

`poll_wait` on the device waitqueue, then:

- `EPOLLIN` if either per-type news generation differs from that file's
  acknowledged generation;
- `EPOLLERR` if a fatal mock error is latched.

No operation → no readiness after the fd has consumed its snapshot. Cancel and
completion wake the queue.

### `VERIFY_START`

Copies and validates the 32-byte nonce (`flags` and `reserved` must be zero).
Active → `-EBUSY`; `gone` → `-ENODEV`. After one 100 ms step it reports `DONE`
with:

- `MATCH` only if at least one fake identity exists **and** debugfs `finger` is
  on. Returns identity 0's fake UUID, a nonzero token derived from
  nonce + UUID + per-boot verify generation, and `deadline_ns` =
  `CLOCK_MONOTONIC` now + 5 s.
- `NO_MATCH` otherwise, with zero UUID, zero token and zero deadline.

The nonce is wiped after the step. This token is test data, not a MAC, not a
proof of key possession, and not comparable to a real SEP token.

### `CANCEL`, `DELETE`, `DELETE_ALL`, `ATTEST`

`CANCEL` is idempotent and uses the same internal path as `.release`: mark the
operation inactive under the state lock, cancel delayed work synchronously
without holding that lock, transition both snapshots to `IDLE`, wipe nonce,
token and pending label, bump the relevant news generations, wake waiters.

`DELETE` copies the 16-byte UUID, rejects while an operation is active
(`-EBUSY`), returns `-ENOENT` if absent, otherwise removes and shifts the array.
`DELETE_ALL` has the same busy rule and wipes all identities. `ATTEST` returns
`-ENOSYS`; there is no key or enclave in this module. Unknown ioctls return
`-ENOTTY` (they do not fall through to `ENOSYS`).

## 5. Debugfs surface

One file, created only when debugfs is available:

```
/sys/kernel/debug/sep-bio-mock/control      (0600, root)
```

Writes additionally require `CAP_SYS_ADMIN` or `CAP_SYS_RAWIO`. One command per
write; an optional single trailing newline is accepted. Empty input, embedded
NULs, trailing spaces and unknown commands are `-EINVAL`.

| command | effect |
|---|---|
| `finger on\|off` | choose whether verify can return `MATCH`; default off |
| `gone on\|off` | `sensor_present=0` and `ENODEV` for new starts; default off |
| `stuck on\|off` | keep an active operation pending and never advance it; the work item reschedules so turning it off resumes; CANCEL/release always recovers |
| `fail <hex status>` | the next active step returns `FAILED` with that opaque 32-bit status, one shot |
| `reset` | refused (`-EBUSY`) while the device is open or an operation is active; otherwise wipes RAM identities, pending failure, op result and the fatal latch |

`fail` statuses with bit 31 set also latch the fatal error reported by
`EPOLLERR`; `reset` (or module unload) clears it. This bit convention is a mock
drill, not a proposed UAPI flag. `reset` deliberately leaves the environment
switches (`finger`, `gone`, `stuck`) alone: those model the world outside the
identity store, while `reset` models wiping storage. A fresh module load has all
three off.

The real `sep-a9` debugfs tree is different (`/sys/kernel/debug/sep-a9/…`) and
is not touched by this module.

## 6. Concurrency and lifetime

- One global static `struct sep_bio_mock`; no allocation whose lifetime must be
  tracked across work/ioctl/poll/release.
- `lock` protects all mutable state and the news generations. It is never held
  across `copy_to_user`/`copy_from_user` or `cancel_delayed_work_sync`.
- `op_lock` serializes `START` against `CANCEL`/release. Lock order is
  `op_lock -> lock`; the work item takes only `lock`. This prevents a START from
  installing new work between cancel's "mark inactive" and
  `cancel_delayed_work_sync`.
- `cancel_delayed_work_sync` is called after dropping `lock`, so a running work
  item can take the lock and observe `active=false`.
- Per-file acknowledgements live in the small `kzalloc` context. `.release`
  cancels first and only then clears the open slot, so a new opener cannot start
  work while release is still cancelling.
- `.owner = THIS_MODULE` on both the misc and debugfs fops prevents module
  removal while a file is open. Module exit still cancels work, removes
  debugfs, deregisters misc and wipes RAM state.
- A second module (including real `sep-a9`) that already owns the misc name
  makes `misc_register` fail; init logs the error and returns it.

## 7. Build

```sh
# kernel object, dedicated worktree, W=1 by default
scripts/sep/build-bio-mock.sh

# existing worktree / no extra warnings
TREE=sep W=0 scripts/sep/build-bio-mock.sh

# remove built objects, keep the copied source
scripts/sep/build-bio-mock.sh clean
```

The script follows `scripts/sep/build-driver.sh`:

1. runs in the Docker kernel worktree named by `TREE` (default
   `sep-bio-mock`);
2. copies `drivers/sep/sep-bio-mock.c` to `drivers/soc/apple/` and the UAPI
   header to `include/uapi/linux/`;
3. idempotently adds `APPLE_SEP_BIO_MOCK` to
   `drivers/soc/apple/Kconfig` and `sep-bio-mock.o` to its Makefile;
4. does not replace an existing `.config`; it only enables the mock symbol,
   seeding a missing config from `configs/hoolock-16k.config`;
5. enables the symbol and builds only `drivers/soc/apple/sep-bio-mock.o`.

It never copies or builds `sep-a9.c`, so the object is proven to compile
without that driver.

Userspace build (strict warnings). The host macOS SDK has no Linux UAPI
headers, so the supported local build is the arm64 kernel container:

```sh
docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work \
    linux-a9-kbuild:latest make -C tools/sep sep-bio-conformance CC=clang
```

`make -C tools/sep` still builds `sep-bio-test` first, as before.

## 8. Conformance harness

```sh
# safe offline/default mode: only fake device state, no debugfs writes
tools/sep/sep-bio-conformance --device /dev/sep-bio

# opt into debugfs drills and the MATCH path
tools/sep/sep-bio-conformance \
    --control /sys/kernel/debug/sep-bio-mock/control
```

The tool refuses to run unless `GET_INFO.reserved[2]` has the mock marker, so it
cannot enrol or delete on a real SEP. Every wait is an `epoll_wait` with a
timeout; the poll ioctls themselves are non-blocking. Readiness is never polled
in a busy loop. Stop `fprintd` first: the device is single-open, so a daemon
holding `/dev/sep-bio` makes the harness fail with `EBUSY`.

Covered in default mode:

1. `GET_INFO`: v4, mock marker, `sensor_present=1`, capacity 3, 8 stages,
   bounded `enrolled`, no live-count claim.
2. second `open()` → `-EBUSY`;
3. idle fd is not readable; after `START`, bounded `EPOLLIN`; a successful poll
   clears readiness until the next 300 ms update; all eight stages observed
   with monotonic progress, cycling guidance and at least 2 s elapsed;
4. `ENROL_START` / `VERIFY_START` while active → `-EBUSY`;
5. `CANCEL` → `IDLE`, idempotent, slot reusable; `DELETE`/`DELETE_ALL` while
   active → `-EBUSY`;
6. close while active → next open succeeds and `ENROL_POLL` is `IDLE`;
7. three complete enrolments → exact `enrolled` counts, fourth `ENROL_START`
   → `-ENOSPC`;
8. `LIST`: bounded count, NUL-terminated labels, label round-trip, nonzero and
   unique UUIDs;
9. `VERIFY` with finger off (the module default) → `DONE`/`NO_MATCH` with no
   token, UUID or deadline;
10. `DELETE` missing → `-ENOENT`, delete existing and observe order, repeat
    delete → `-ENOENT`, `DELETE_ALL` → empty and idempotent;
11. `ATTEST` → `-ENOSYS`;
12. final `ENROL_POLL` is `IDLE`.

The MATCH path needs the debugfs switch because no automatic, side-effect-free
way exists to change the mock's finger state from the ABI itself. Without
`--control` the harness runs the no-match path and prints one explicit `SKIP`
for MATCH/token/deadline; it does **not** silently pass. With `--control` the
MATCH path writes only the mock's RAM switch (`finger off/on`) and restores
`off`; it then checks MATCH UUID == `LIST` identity 0, nonzero token, different
token for a different nonce, and a fresh `CLOCK_MONOTONIC` now+5 s deadline.

With `--control` it also drills: malformed control commands are rejected;
`reset` refused while open; `stuck` stays PENDING and CANCEL recovers;
`fail 0x5` returns `FAILED` with that status; `gone` makes
`sensor_present=0` and starts return `-ENODEV`; a fatal status latches
`EPOLLERR`; after close, `reset` wipes the identity added for the drill and
clears the error latch.

No PAM, fprintd, libfprint, packaging or udev state is touched by this tool.

No duplicate host-side model of the kernel state machine was added. The device
tests drive the shipping module directly, which is stronger evidence than a
model; the only non-device checks are the kernel/userspace builds, checkpatch
and whitespace checks listed below.

## 9. Validation performed

| check | result |
|---|---|
| `W=1 scripts/sep/build-bio-mock.sh` | object built, no compiler warnings |
| dedicated tree check | `CONFIG_APPLE_SEP_BIO_MOCK=y`; `APPLE_SEP_A9` absent from that tree |
| `scripts/checkpatch.pl --no-tree --file` | 0 errors; one intentional `ENOSYS` warning for `ATTEST` (required by the contract) |
| `make -C tools/sep sep-bio-conformance CC=clang` (arm64 kernel container) | built with `-Wall -Wextra -Werror -Wformat=2 -Wshadow -Wpointer-arith -Wstrict-prototypes` |
| `clang --analyze` on the conformance tool | no findings |
| `git diff --check`, trailing-whitespace scan | clean |
| device/module load + conformance run | **not performed** (no device access in this workstream) |

## 10. Residual risks and open work

1. **Nothing has been booted or loaded.** The object compiles and the harness
   compiles, but no module load, no ioctl sequence and no epoll timing has been
   exercised on a kernel. The first real run is the remaining proof.
2. **The mock is not evidence about real SEP behavior.** Capacity 3, 8 stages,
   300 ms timing, 100 ms verify, the token construction, `reserved[2]` and the
   fatal-status-bit convention are mock inventions. Real values and token rules
   remain [U].
3. **No catacomb emulation and no persistence.** The real AP must persist an
   opaque exported blob durably; this module keeps only fake RAM identities.
   Losing module state loses every fake identity, which is intentional.
4. **No security properties.** UUIDs/tokens are non-cryptographic and generated
   from public inputs. `MATCH` is decided by a debugfs switch plus a non-empty
   fake gallery, not by biometric comparison. No claim should be made about
   enrollment integrity, replay resistance, key possession or template
   exclusivity.
5. **Real identity-list opcodes are still unknown.** The mock defines the host
   contract only; `LIST`/`DELETE` against a real SEP still need transport and
   payload RE work.
6. **`LIVE_COUNT_VALID` is not exercised.** The mock leaves the flag clear.
   Aurora's identify fast path and the real driver's maintenance of an exact
   live count remain untested here.
7. **Debugfs control is a test surface.** It is root-only and capability-gated,
   but any root process can flip `finger` and force a `MATCH`. That is
   acceptable for a test module and unacceptable for production; the real
   backend must not expose equivalent controls.
8. **`reset` intentionally does not reset `finger`/`gone`/`stuck`.** Operators
   must turn environment switches off themselves; the conformance tool turns
   all three off on entry and on exit.
9. **Timing assumptions in the harness.** It assumes the 300 ms stage cadence
   and uses a 120 ms quiet window to prove acknowledgement. A heavily paused
   guest can make a strict stage-count check fail even though the kernel
   behaved correctly; rerun before treating that as a kernel bug.
10. **Only one owner of `/dev/sep-bio`.** Loading the real `sep-a9` and this
    module together is resolved by `misc_register` failure, not by dynamic
    multiplexing. Do not expect both.
11. **Keybag and attestation paths remain unimplemented.** `ATTEST` is
    `-ENOSYS`; the header has no keybag ioctls. PAM/fprintd integration is
    downstream and was not touched.
