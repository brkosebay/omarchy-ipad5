# SEP enrollment persistence end to end (A9 / j71t)

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Workstream: `enrollment-storage-re`.
This is **read-only static work plus log correlation**. Nothing here was run on
the iPad; no device command in this document has been executed by this agent.

Evidence base:

- iOS 16.7.10 j71t kernelcache, `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`,
  Ghidra project `kernelcache.release.iPad6,11_12`, image base `0xfffffff005298000`.
  All `0xfffffff0…` addresses below are static in that image.
- `libBKDM1.dylib` from the iOS dyld shared cache, `build/dsc/out/libBKDM1.dylib`,
  sha256 `df0b2e72b7aed02a6e8c4f274d36fafe644f40fdad31be1909c9683f345484d3`.
  All `0x20a…` addresses below are static in that dylib (arm64, symbols intact).
- Live iOS capture `build/sep/sbio-trace.log` (12,965 lines) and its excerpt
  `docs/sep/traces/2026-10-24-first-enroll.md`.
- ADT `docs/adt/j71t.txt` and the live registry dump
  `build/firmware/private/IODeviceTree.txt`.
- Hardware runs `docs/sep/logs/tri53-sep5-20261026-1730/`,
  `tri53-sep6-20261026-2010/`, `tri53-sep6-combined-20261026-2021/`,
  `tri53-sep6-sbio-20261026-2103/`.

---

## 1. Result at a glance

| question | answer | class |
|---|---|---|
| Does an xART capability handshake exist before enrollment | yes: `xarm` opcode `0x0e`, byte 5 of the reply = AP protected-data capability | verified |
| What does capability 0 actually forbid | the SEP may not issue xART record ops (`0x00/0x01/0x05/0x06/0x07`); XNU **panics** if one arrives while its store is not ready | verified |
| Does capability 0 abort sepOS bootstrap | no: with cap-0 the SEP advertised `sbio/scrd/sks/sse` and stayed quiet | verified (hardware) |
| Does capability 0 block enrollment | not observable from AP code; sepOS is encrypted. The biometric save path is independent of xART | unknown (testable) |
| Who owns the catacomb blob | the **AP**: sepOS serialises it and hands it out; userspace stores it in a file | verified |
| Is `0x33` `CompleteSaveCatacomb` | **no** — `0x33` is `GetCatacombId` (4 in → 16 out). `CompleteSaveCatacomb` has no SEP opcode at all | verified |
| Is `0x37` the commit | yes: 24-byte key in, no payload out, observed 383 ms | verified |
| Integrity of the AP-side xART store | `crc32` per record + a per-store monotonic counter, no MAC | verified |
| Integrity of the catacomb | SEP-side 32-byte catacomb hash (`0x36`) + 16-byte id (`0x33`); the AP file has no cryptographic integrity field | verified / inferred |
| Where iOS puts it | xART: dedicated APFS volume `xart-vol` → `/private/xarts`. Catacomb: `/var/mobile/Library/Catacomb/` | verified (ADT + registry + strings) |
| Can we prototype everything except durability in one boot | yes; and the blob round trip can even be tested with the *empty* catacomb before any enrollment | design |

Two corrections to earlier docs in this repo:

1. An earlier `docs/sep/sbio-commands.md` revision listed
   `CompleteSaveCatacomb 62 → 0x33?`; it was corrected from this evidence.
   `0x33` is `GetCatacombId`, and host command 62 performs no SEP transfer at
   all. Kernel case `0x3e` only asserts on its own
   `_catacombData` (`"!_catacombData.data"` at `0xfffffff0066a7edc`,
   `"_catacombData.data"` at `0xfffffff0066a7f60`), `memcmp`s the 24-byte key
   against the one saved by Prepare, copies the already-buffered `0x6c` result
   out, and releases it.
2. `docs/sep/touchid-next.md` says "userspace maintains a serialized
   `catacomb_state_v1_t`". `catacomb_state_v1_t` is the **SEP-side** record
   (`support/catacomb.c`); the AP file is a different, newer container (an
   NSKeyedArchiver archive) with a legacy `CAT1` decoder.

---

## 2. The three independent stores behind Touch ID

They are often conflated. They are separate, and only one of them is what
`xarm` capability 0 talks about.

```
              ┌──────────────────────────── SEP (sepOS) ───────────────────────────┐
              │  working keys, user bag (sks), catacomb working copy                   │
              │  no filesystem, no bulk media                                          │
              └───────▲───────────────────────▲───────────────────────▲───────────────┘
                      │ xarm/xars             │ sbio 0x6c/0x37/0x36   │ arts (manifest)
                      │ (xART record store)   │ (catacomb export)     │ 20-byte ART
              ┌───────┴───────────┐  ┌────────┴────────────┐  ┌───────┴──────────────┐
              │ AP xART volume    │  │ AP catacomb archive │  │ NVRAM sep.art        │
              │ /private/xarts    │  │ Library/Catacomb    │  │ (absent on j71t)     │
              └───────────────────┘  └─────────────────────┘  └──────────────────────┘
```

- **xART** (`xarm`/`xars`) is a generic protected record store used by AMFI/ECS
  and by `AppleSEPARTStorage`. It is *not* the biometric store.
  `"AMFI: calling %s without xART storage support"` (`0xfffffff0052f38fd`) and
  `"xART not supported, EACS is a nop."` show it is a boot-integrity dependency,
  not a Touch ID one.
- **Catacomb** is the fingerprint template store. sepOS keeps the working copy;
  the AP is expected to persist the exported blob. This is the store whose loss
  costs an enrollment.
- **Legacy ART** is a 20-byte anti-replay token read from the kernel-only NVRAM
  variable `com.apple.System.sep.art`; j71t has no `has-art`, so the cold path
  skips it (`docs/sep/driver-notes.md` §9). It is unrelated to enrollment data.

---

## 3. `xarm`/`xars`: the capability handshake

### 3.1 Wire format (verified)

`AppleSEPXART_handleSEPDrivenMsg` (`0xfffffff0062f5188`) decodes the eight-byte
mailbox word as: endpoint, tag, opcode/result, `u16` length (bytes 3..4),
flags/capability (byte 5), `u16` CRC (bytes 6..7).

| op | direction | operation | payload |
|---:|---|---|---|
| `0x00` | SEP→AP | fetch main record | 17-byte reply minimum |
| `0x01` | SEP→AP | save main record | ≤ `0x8000` |
| `0x05` | SEP→AP | fetch UUID locker | 16-byte UUID in, record out |
| `0x06` | SEP→AP | save UUID locker | UUID + record |
| `0x07` | SEP→AP | delete UUID locker | UUID in |
| `0x0e` | SEP→AP | protected-data capability | none |

Byte 5 bit 0 selects the namespace: 0 = `USER` (type 1/3), 1 = `SEP` (type 2/4).

### 3.2 Opcode `0x0e` is a question about the AP, not about the SEP

The handler reads `/chosen/protected-data-access` and `/chosen/sepfw-load-at-boot`,
checks the provider's `sepfw-loaded` property, rewrites byte 2 to result `0`,
writes the capability into byte 5, and replies with length 0.

If `sepfw-load-at-boot` and `sepfw-loaded` are both true **and** the capability is
non-zero, XNU does not reply immediately: it stashes the request word in
`DAT_fffffff007a6f2c0`, sets `DAT_fffffff007a6f2b9`, and later
`AppleSEPXART_initGigalocker` (`0xfffffff0062f5c78`) sends the stored reply once
the backing store is mounted. That deferral is why XNU's own answer looks like a
capability-1 value that arrives late.

### 3.3 The capability is a hard contract (verified)

Two globals gate everything (`0xfffffff007a6f2c8`, `0xfffffff007a6f2b8`):

- `0` → any incoming `0x00/0x01/0x05/0x06/0x07` jumps to a `REQUIRE`-fail panic
  helper (`0xfffffff00630f1fc` for op `0x00`, `0xfffffff00630f28c` for `0x01`,
  `0xfffffff00630f364` for `0x07`, `0xfffffff00630f43c` for `0x05`,
  `0xfffffff00630f4cc` for `0x06`), each of which is
  `_panic("REQUIRE fail: %s @ %s:%u:%s: …")`.
- `1` → the record handlers run normally.

So "advertise capability 1" means "I will service every storage op or die".
That is exactly the trap the earlier docs warned about, now confirmed at the
instruction level: it is not a slow path or an error return, it is a panic.

Additional verified consequence: a store whose `crc32` does not match is also a
panic on the fetch path (`FUN_fffffff0062f18fc` → `FUN_fffffff00630e9ec`), while
a bad record *at mount time* is silently wiped (`FUN_fffffff0062f0d40`).

### 3.4 What capability 0 means, and what it does not

Verified:

- Capability 0 is a legitimate, non-error answer (`result = 0`, byte 5 = 0).
- On hardware, after the proven `arts` MANIFEST + cap-0 `xarm` reply, sepOS
  advertised `sbio=0x08`, `scrd=0x0a`, `sse=0x0c`, `sks=0x12`
  (`tri53-sep6-combined-20261026-2021/5-final.txt`, stable at `tx=10 rx=17`,
  `endpoints_complete=8`, both FIFOs empty over 10 s).
- In every observed boot with capability 0, **no** storage opcode followed.

Strong inference:

- Capability 0 tells sepOS "the AP has no protected store". It does not tell it
  "the AP has no catacomb store". The catacomb export/import uses the SBIO
  transport, not `xarm`.
- Therefore enrollment should be able to run with capability 0, and only its
  *durability across a SEP reset* depends on the AP-side catacomb store.

Unknown (must be measured, see §10):

- whether sepOS refuses `EnrollMode`/`0x6c` when it has no xART store, or marks
  the catacomb volatile;
- whether the `0x37` commit itself requires the xART store;
- whether sepOS refuses to bump the catacomb counter without xART
  (`libBKDM1` has the string `kReadCatacombSUCommand - may bump catacomb counter`,
  which shows a counter exists but not who enforces it).

---

## 4. `arts` in one paragraph

`arts` is the legacy ART service, not a store. `AppleSEPARTStorageService_start`
(`0xfffffff0062fe1d8`) publishes 16 KiB OOL, and
`AppleSEPARTStorage_handle_first_connected` (`0xfffffff0062fcf0c`) copies the
personalized IM4M and sends MANIFEST (`ep=arts tag=1 op=0x06`) synchronously;
the reply is `op=0x07`, parameter 0 = success. j71t has no `has-art`, so nothing
follows. Hardware already proved this in isolation
(`tri53-sep6-20261026-2010`) and in combination
(`tri53-sep6-combined-20261026-2021`). Keep it in the boot sequence only because
sepOS's discovery gate appears to be the AND of "manifest accepted" and "xarm
answered"; it contributes no enrollment state.

---

## 5. Catacomb: ownership and serialization

### 5.1 SEP side

The SEP is the owner of the *live* catacomb and of the template material that
matching needs. The AP sees it only through SBIO commands. Verified semantics
from `AppleMesaSEPDriver::performSpecificCommandGated`
(`0xfffffff0066a504c`), whose host-command names come from the table at
`0xfffffff006f4ebf0` (index *n* → command *n+1*):

| host cmd | name | SEP op | in | out | what the driver does |
|---:|---|---:|---:|---:|---|
| 60 `0x3c` | GetCatacombState | `0x6b` | 0 | variable (168 observed) | plain transact |
| 61 `0x3d` | PrepareSaveCatacomb | `0x6c` | `0x18` | buffer + `u32` | saves the 24-byte key at `obj+0x1ca1`, keeps the reply buffer at `obj+0x398/0x399` |
| 62 `0x3e` | CompleteSaveCatacomb | **none** | `0x18` | blob | `memcmp(obj+0x1ca1, key, 0x18)`, copies `obj[0x398]/obj[0x399]` out, releases it |
| 63 `0x3f` | ConfirmSaveCatacomb | `0x37` | `0x18` | 0 | commit; `0xffffffff` in the key clears to "no catacomb" and stores wall time |
| 64 `0x40` | LoadCatacomb | `0x6d` | > `0x20` | 0 | restores a stored blob; `+8` = size/id, `+12` = flag |
| 56 `0x38` | GetCatacombId | `0x33` | 4 | `0x10` | identity of the current catacomb |
| 58 `0x3a` | GetCatacombHash | `0x36` | 4 | `0x20` (host 33) | 1 validity byte + 32-byte hash |
| 49 `0x31` | NoCatacomb | `0x2d` | 4 | 0 | `0xffffffff` ⇒ clear |
| 5 `0x05` | ReadCatacombSU | `0x55` | template list | 0 | logs `"Catacomb Restore Complete (size = %lu)"`, sets `obj+0x2bb2=1`, `obj+0x14=0` |
| 72 `0x48` | RemoveUserData | `0x3c` | 4 | 0 | then clears the in-kernel identity table entries for that id |
| 13 `0x0d` | RemoveTemplate | `0x57` | `0x14` | 0 | |
| 15 `0x0f` | RequestMaxIdentityCount | none | 0 | 4 | returns **5** (no SEP call) |

Two details worth keeping:

- The 24-byte key is **not a hash**. With protocol version 2 (the version this
  driver negotiates) userspace sends the component: 24 bytes = 16-byte UUID +
  8 bytes. With version 1 it sends a 4-byte userID instead
  (`-[… performPrepareSaveCatacombCommand:outDataSize:]`, `0x20a1ed87c`).
- `catacomb_header_t` is 32 bytes: the LoadCatacomb case requires
  `payloadSize > sizeof(catacomb_header_t)` and reads `u32` at `+8` and `+12`.
  Field names and the remaining bytes are not recoverable from this binary.

### 5.2 AP side (userspace)

`biometrickitd`/`libBKDM1` owns the file. Verified strings and code:

- directory: `<home>` + `"Library/Catacomb"` → `___GetCatacombDirectoryPath`
  (`0x20a20a580`), created if missing; per-component subdirectory and a
  UUID-named file (`___GetUUIDFileName`, `0x20a20a910`).
- modern container: an `NSKeyedArchiver` archive with keys
  `CatacombVersion`, `CatacombEnrollmentCount`, `CatacombCurrentDate`,
  `CatacombIdentityList`, `CatacombTemplateList`
  (`-[… archiveCatacombDataForComponent:toArchiver:]`, `0x20a1ea24c`;
  `unarchiveCatacombDataForComponent:fromUnarchiver:secureData:identities:`,
  `0x20a1ea52c`). The `secureData` argument of the unarchiver is the natural slot
  for the SEP-produced blob; that is an inference from the signature, not yet
  proven from the archive keys.
- legacy v1 container, still decoded for migration:
  `-[… decodeCatacombDataV1:withSize:]` (`0x20a1ea860`):

```
+0x00  u32  magic 0x31544143  ("CAT1")
+0x04  u32  identityCount
+0x08  u32  templateListSize
+0x0c  identityCount × 28 bytes:
         +0x00 uuid[16]   +0x10 u32 type   +0x14 u32 attribute   +0x18 u32 entity
        then templateListSize bytes of opaque template material
```

  The decoder validates `0xc + 0x1c*count + templateLen <= size`, then pushes the
  template bytes straight into the SEP with host command 5 (`0x55`) — i.e. the
  file is the restore source, and the SEP is the verifier.

- write path `-[… saveCatacombForComponents:]` (`0x20a1ebadc`) plus the C helper
  `_SaveDataToCatacomb` (`0x20a20a058`): open `0x202`/mode 3, write, close, then
  a `fcntl`-shaped call with command **51** (`F_FULLFSYNC`). Confidence on the
  `F_FULLFSYNC` identification: medium — the constant is unambiguous, the
  binding is an `objc_msgSend` stub and is not symbol-resolved.

### 5.3 Sizes seen in the real capture

From `build/sep/sbio-trace.log` (first enrollment, then five unlocks):

| op | request | reply | line |
|---:|---:|---:|---:|
| `0x6c` | 24 | **9611** | 3366 → 3367 |
| `0x37` | 24 | 0 (383 ms) | 3494 → 3501 |
| `0x33` | 4 | 16 | 3416 → 3417 |
| `0x36` | 4 | 32 | 3439 → 3440 |
| `0x6c` | 24 | **236** (empty component) | 3751 → 3752 |
| `0x6c` | 24 | **16379** = 16356 + 23 via `tag=0xfd`, mailbox sequence 1 | 6724 → 6727 |
| 60 | 0 | 168 | 3337 |

Interpretation: 236 bytes is the serialised *empty* catacomb; 9611 after one
identity; 16379 with the full set. The 16379 case crosses the 16,356-byte OOL
payload limit, so the catacomb read **requires the existing `0xfc`/`0xfe`/`0xfd`
multi-chunk state machine** — the largest catacomb is not a single transaction.

The save sequence observed in the capture is exactly:

```
GetCatacombState(60) → Prepare(61) → Complete(62) → [file write + fsync]
  → Confirm(63) → GetCatacombId(56) → GetCatacombHash(58) → (repeat per component)
```

`saveCatacombIfDirtyWithInterval:` re-runs the same cycle periodically, so a
"save" is a routine flush, not a one-shot commit.

### 5.4 Restore at boot

`-[… loadCatacomb]` (`0x20a1ec2b8`) does: `clearTemplateList` →
`sizeOfLegacyCatacomb` → `restoreTemplateListSU` (= host cmd 5, SEP `0x55`) →
`syncTemplateListForUser:` , with `catacombCorrupted:` error codes on failure.
The second path `LoadCatacomb` (64 → `0x6d`, whole blob with a 32-byte header) is
used by `performLoadCatacombCommand:inData:` (`0x20a1ee7f4`); its caller in this
dylib was not located. Working assumption: `0x55` is the normal boot restore and
`0x6d` is the migration/restore-from-backup path. Confidence: medium.

---

## 6. Anti-replay requirements, concretely

Verified from `AppleSEPXART_handleSEPDrivenMsg` and the store layer:

1. **Wire CRC16.** Every save (`0x01`, `0x06`) carries `crc16` over the payload in
   mailbox bytes 6..7. Polynomial `0x1021`, init `0xffff`, no final XOR
   (table at `0xfffffff0055a8bc8`). A mismatch is rejected by the AP before the
   store is touched; a mismatch in SEP→AP direction is rejected by XNU
   (`memcmp(&xart_crc, in_msg_p->b, 2)`).
2. **Per-record CRC32.** The store record holds `crc32(payload)`; it is checked on
   every fetch (mismatch = panic) and on every mount (mismatch = wipe the slot).
3. **Monotonic counter per record.** `ctx+0x420` is a per-store `u64` that only
   grows; each write stamps `counter` into the record and then deletes the old
   slot. At mount the counter is restored as `max(counter, record.counter)`, and
   two records with the same `(type, uuid)` are resolved by keeping the higher
   counter (`FUN_fffffff0062f0d40`). This *is* the anti-replay mechanism.
4. **No MAC, no SEP key.** The store is integrity-protected against corruption,
   not against an attacker who can write the device. Whatever rollback protection
   exists comes from the SEP's own state, not from this file (inference).
5. **`max` sizes:** payload 1..`0x8000`; main record and locker alike.
6. **Not-found is success, with a 17-byte prefix.** Every main-record reply starts
   with a 17-byte prefix that the driver zeroes (`*buf = 0`, `memset(buf+1, 0, 16)`);
   the record payload is written at `buf + 0x11`. A missing record therefore yields
   a 17-byte all-zero reply with length `0x11` and no payload, not an error. The
   `USER`/`SEP` namespace is carried by the request key (type 1 vs 2), not by the
   reply bytes.

Consequence for us: **the AP-side store is untrusted input to sepOS**. The only
SEP-enforced integrity check on the catacomb is the 32-byte hash (`0x36`).
Whatever we build must treat the store as data and validate it before handing it
back, and must expect sepOS to reject garbage.

---

## 7. Which storage service each layer actually provides

XNU (verified):

- `AppleSEPManager`'s user client exposes `_setGigalockerAvailable(const char *)`
  and copies the path into `_gl_abs_path` (`PATH_MAX`), gated by a command gate
  and a migration state machine.
- `AppleSEPXART_initGigalocker` → `FUN_fffffff0062f0918(path, ctx)`:
  1. `vnode_open(path, 0x102, …)`;
  2. `VNOP_IOCTL(vnode, 0x40044a37, &pair, …)` to learn `(major, minor)`;
  3. `snprintf("/dev/apfs-raw-device.%d.%d", major, minor)` and `vnode_open` it;
  4. `vnode_getattr` → require 4 KiB-aligned size, store `size` at `ctx+0x418`;
  5. scan every `0x9000` slot, wipe invalid ones, rebuild the counter.
- All I/O then goes through `FUN_fffffff0062f2404`: `uio_create` +
  `VNOP_READ`/`VNOP_WRITE` on the raw-device vnode, plus
  `VNOP_IOCTL(vnode, 0x20006416, …)` after each write (flush/notify).
- So XNU's "storage service" is a **file that userspace names, promoted to an
  APFS raw device**, with a fixed-slot record array. XNU provides no general
  secure file store to userspace.

The OS image provides the media (verified in the ADT and live registry):

```
xart-vol:  vol.fs_name "xART"  vol.fs_file "/private/xarts"  vol.fs_role 256
xART:      node under /arm-io/sep/iop-sep-nub  (AAPL,phandle 47)
```

i.e. iOS keeps a dedicated, non-browsable (`nosuid,nodev,nobrowse`) volume for
this store, mounted at `/private/xarts`. The gigalocker file itself is named by
userspace, so that it lives on this volume is a strong inference from the ADT,
not an observed path string.

Userspace provides the catacomb file (`/var/mobile/Library/Catacomb/…`), which is
protected only by the data-protection class of the container — not by the SEP.
That is the honest comparison point for Linux: on iOS the catacomb file's
confidentiality comes from APFS + class keys, not from this driver.

---

## 8. Safe per-boot prototype vs true persistent enrollment

**Per-boot prototype (no new risk).** Everything below works with capability 0,
writes no storage and survives only until the next SEP reset:

- the proven `arts` MANIFEST + `xarm` cap-0 gate;
- SBIO OOL publication and `0x73` init (the corrected 28-byte envelope; the
  21:03 failure was `ep=0x08 cmd=0x73 ret=-5 sep_status=0x101`, i.e. a
  GenericTransfer header rejection, not an SBIO policy answer);
- read-only `0x2b`, `0x3c`, `0x33`, `0x36`;
- one `0x6c` export, which gives a real blob that we can inspect and throw away.

What it cannot do: keep an enrollment across a cold boot, and honestly confirm
(`0x37`) a save that we cannot persist.

**True persistent enrollment** additionally needs, in this order:

1. a durable AP store (below), with the blob written *before* `0x37`;
2. a restore path at boot (`0x55` first, `0x6d` as the alternative) that sepOS
   accepts;
3. a hash check with `0x36` after restore, treating a mismatch as "no catacomb";
4. only then, an actual enrollment (needs the Mesa sensor and the 68-byte
   `EnrollMode` payload, both still missing).

Note that (1)–(3) can be exercised **without enrollment**, using the 236-byte
empty catacomb. That is the cheapest honest persistence test we have.

---

## 9. AP-side storage service design (proposal)

Constraints that shape it:

- Only p2 is writable (`nvme.write_window=1:…`); no new partition, no writes to
  syscfg, NVRAM, or boot-firmware storage. Therefore: a **file tree on the
  existing ext4 root**, never a partition and never NVRAM.
- OOL bindings are once per sepOS boot and SEP state dies with it, so no store
  test may span a warm kexec.
- No secret exists yet that the AP cannot read (rootfs unencrypted, checkm8), so
  do not claim confidentiality we cannot deliver.

### 9.1 Layout

```
/var/lib/sep-a9/                     0700 root
  catacomb/
    store.meta                       0700, small, fsync'd
    <component-hex>/                 0700
      blob.<gen>.bin                 0600, exact bytes from 0x6c
      rec.json                       0600, {gen, len, sha256, sep_hash, state,
                                            id, created, confirmed}
  xart/                              (later, only if we advertise capability 1)
    store.img                       0x9000 × N, XNU-compatible
```

`store.meta` holds `{format, store_gen, last_component, last_sep_hash}` and is
the tie-breaker if a directory is damaged.

### 9.2 Format of `blob.<gen>.bin`

Opaque SEP bytes plus **our own** 88-byte envelope, so we never have to guess
the SEP's internal header:

```
+0x00  "S9CB"        u32 magic
+0x04  u32 version   = 1
+0x08  u32 type      = 1 (catacomb secure blob)
+0x0c  u32 comp_len  (24 or 4, whatever the SEP key was)
+0x10  comp[key]     (component UUID or userID, copied verbatim)
+0x28  u64 gen
+0x30  u32 len
+0x34  u32 crc32(payload)
+0x38  u8  sha256[32] of payload
+0x58  payload
```

`rec.json` duplicates `{gen, len, sha256}` and the SEP-reported `0x36` hash, so a
mismatch between the two files is detectable (defence against a partial write,
not against root).

### 9.3 Atomicity

Write path (per save cycle):

1. `0x6c` with the real component key → collect the full blob (multi-chunk).
2. `0x3c` and `0x36` → state and hash **before** the write.
3. write `blob.<gen+1>.bin.tmp` → `fsync(file)` → `rename()` →
   `fsync(dir)` → update `rec.json.tmp` → `fsync` → `rename` → `fsync(dir)` →
   update and fsync `store.meta`.
4. only now send `0x37` (confirm).
5. keep exactly one previous generation (`gen-1`); delete older ones after the
   first successful restore.

Invariant: **`0x37` is only ever sent after step 3 succeeded.** A crash before
step 3 loses the save but leaves the SEP's notion unconfirmed; a crash between
3 and 4 leaves a blob the SEP never confirmed, which the restore path must
handle (see 9.6).

### 9.4 Confidentiality

Honest position, in order of what can actually be delivered:

- today: `0600`/`0700`, root-only, written to p2; the blob is assumed to be
  sensitive **plaintext** because we cannot prove the SEP encrypts it;
- next: once SKS/user-bag support exists (`docs/sep/keybag.md`, option B2), wrap
  `payload` with an AEAD key derived from the login password (Argon2id +
  per-store salt in `store.meta`) and drop the plaintext. Cost: no restore before
  the first login, which is acceptable because fingerprints are only useful after
  login anyway;
- never: claiming "SEP-protected" until the key actually lives in the SEP.

### 9.5 Rollback protection

- `gen` is monotonic per component and mirrored in `rec.json` + `store.meta`; a
  store whose `gen` went backwards relative to `store.meta` is treated as
  suspect.
- The authoritative check is the **SEP hash**: after restoring, read `0x36`
  (33 bytes: validity + 32-byte hash) and compare with `rec.json.sep_hash`. A
  mismatch is not "retry" — it means the blob and the SEP disagree, so drop the
  blob and present "no catacomb enrolled".
- Bound to state: an attacker with root can rewrite blob, hash file and meta
  together. With an unencrypted rootfs and checkm8 there is no way around that;
  say so in the release notes rather than implying TPM-like guarantees.

### 9.6 Crash behaviour (state machine)

```
IDLE ──0x6c ok──► EXPORTED ──fsync ok──► DURABLE ──0x37 ok──► CONFIRMED
                     │                      │                  │
             crash → IDLE            crash → DURABLE_UNCONFIRMED
```

- `EXPORTED` lost on crash: nothing to do; SEP state is gone with the reset.
- `DURABLE_UNCONFIRMED`: on next boot restore it, ask `0x36`:
  - hash matches → adopt silently (the SEP never confirmed, but the bytes are
    the ones it produced);
  - hash differs / validity byte 0 → do **not** confirm, drop to "no catacomb".
- Never replay `0x37` for the same `gen`: the commit is the SEP's bump of its
  catacomb counter. Make `gen` the idempotency key.
- A `0x37` that times out is `unknown`, not `failed`: do not resend in the same
  boot; record it and let the boot-time hash comparison decide.

### 9.7 Failure policy (fail closed)

- store missing / crc32 bad / sha256 bad / gen regressed → report "no enrollment"
  to userspace; `fprintd` then simply offers nothing (`GET_INFO.sensor_present`
  stays 0 for now).
- SEP rejects the restore (`0x55`/`0x6d` non-success, or `0x36` validity 0) →
  clear the component, log one line, do not loop.
- never "repair" a blob by guessing; the only legal writes to the store are
  whole-blob, generation-stamped, atomic replacements.

---

## 10. Guarded experiment plan (proposals only; nothing here was executed)

Common envelope for every item below: fresh DFU; proven `arts` MANIFEST + `xarm`
cap-0 gate; do not publish `xars`; publish only the endpoint needed; one commit
point per boot; cold reset at the end; never write syscfg/NVRAM/boot storage.

Driver additions the plan needs (none of them exist yet in `drivers/sep/sep-a9.c`):
a one-shot `catacomb` debugfs knob that accepts an explicit subcommand plus a hex
input buffer, publishes no OOL of its own (SBIO's pair is already used), refuses
to run twice per boot, and prints the raw reply header plus the first 64 bytes.
`0x6c` also needs the existing multi-chunk receive path, because a full catacomb
does not fit in one 16,356-byte chunk.

**E1 — read the iOS artifact (host-side, zero SEP risk).**
With iOS still bootable, read-only copy of `/var/mobile/Library/Catacomb/`
(or the equivalent path on this device) plus a `GetCatacombHash`-era screenshot.
Yields the exact 24-byte component key, file names, archive keys, and the real
`secureData` sizes. Read-only; the orchestrator runs it, not this workstream.

**E2 — one read-only state query (smallest SEP experiment).**
After the gate and SBIO OOL (`08 4000 4000`), send exactly one
`0x3c`/SEP `0x6b` GetCatacombState with no input. Record the 168-byte reply.
No `0x6c`, no `0x37`, no storage. This answers "does a fresh, xART-less SEP see
a catacomb at all?" and gives the baseline state bytes.

**E3 — one non-committing export (separate boot from E2).**
Send one `0x6c` with the component key learned in E1 (or, if E1 is unavailable,
the key implied by E2's state). Handle one continuation chunk if the reply
exceeds 16,356 bytes. **Do not send `0x37` in this boot.** Expected on a fresh
SEP: 236 bytes. This proves the export path and gives us the empty-catacomb
bytes without any enrollment. Note that `0x6c` is the first command here that may
mutate SEP state (it allocates the pending save buffer and may bump an internal
counter), so it gets its own boot and never shares one with a restore attempt.

**E4 — first true persistence round trip (no biometrics).**
Store E3's blob in the new store; cold boot; restore with `0x55` (template path)
and, if that is rejected, one `0x6d` with the 32-byte header path; then `0x36`
and compare. This is the smallest experiment that can prove or disprove
"persistent enrollment is possible with capability 0" without a sensor, without
`EnrollMode`, and without any guessed payload.

**E5 — only after E4:** `0x73` + `0x2b` (already planned in `touchid-next.md`),
then the sensor work, then enrollment.

Stop rules for all of them: any unexpected opcode, any FIFO residue, any status
other than 0, any endpoint outside the declared table, or any timeout ⇒ stop,
collect, cold reset, do not retry in that boot.

---

## 11. Unknowns and the experiment that settles each

| unknown | discriminator |
|---|---|
| does sepOS accept `0x6c` with capability 0 | E3: a 236-byte reply vs an error status |
| does `0x37` commit without xART | E3 + a later boot's `0x36` after a deliberate `0x37` |
| is `0x55` or `0x6d` the real restore path | E4: try `0x55`, then `0x6d`, compare `0x36` |
| is the 236-byte blob the "empty catacomb" | E3 size + E2 state bytes |
| what the 24-byte component key is | E1 (file/dir names) or a non-redacted trace |
| whether the catacomb blob is already SEP-encrypted | entropy check of E3's bytes; structural check for the 32-byte `catacomb_header_t` |
| `catacomb_header_t` field names, `sizeof(catacomb_state_v1_t)` | not recoverable from these binaries; needs `biometrickitd` or a live buffer dump |
| whether `IsXARTAvailable` (host cmd 76) works | verified absent from the driver's switch (`0x4c` has no case) ⇒ returns `0xe00002c2`; the userspace path is `isXARTAvailableWithClient:` (`0x20a1df93c`) |

---

## 12. Evidence index

Kernelcache (image base `0xfffffff005298000`):

| item | address |
|---|---:|
| `AppleMesaSEPDriver::performSpecificCommandGated` (host command switch) | `0xfffffff0066a504c` |
| host command name table (88 entries, index n → cmd n+1) | `0xfffffff006f4ebf0` |
| `AppleSEPXART_handleSEPDrivenMsg` | `0xfffffff0062f5188` |
| `AppleSEPXART_initGigalocker` (deferred `0x0e` reply) | `0xfffffff0062f5c78` |
| xART store save (record build + write) | `0xfffffff0062f1df8` |
| xART store fetch (+ crc32, panic on mismatch) | `0xfffffff0062f18fc` |
| xART store lookup | `0xfffffff0062f155c` |
| xART index allocation | `0xfffffff0062f1754` |
| slot read/write via `VNOP_READ/WRITE` + ioctl `0x20006416` | `0xfffffff0062f2404` |
| gigalocker open (`/dev/apfs-raw-device.%d.%d`) | `0xfffffff0062f0918` |
| mount scan: crc, dup-by-counter, wipe | `0xfffffff0062f0d40` |
| panic helpers (`REQUIRE fail`) | `0xfffffff00630f1b4`, `0xfffffff00630e9ec` |
| `AppleSEPXART_initOOLBuffers` / `AppleSEPXARTService_start` | `0xfffffff0062f4abc` / `0xfffffff0062f4ea4` |
| `AppleSEPARTStorageService_start` / `_handle_first_connected` | `0xfffffff0062fe1d8` / `0xfffffff0062fcf0c` |

`libBKDM1.dylib`:

| item | address |
|---|---:|
| `decodeCatacombDataV1:withSize:` (`CAT1`) | `0x20a1ea860` |
| `archiveCatacombDataForComponent:toArchiver:` | `0x20a1ea24c` |
| `unarchiveCatacombDataForComponent:fromUnarchiver:secureData:identities:` | `0x20a1ea52c` |
| `saveCatacombForComponents:` | `0x20a1ebadc` |
| `loadCatacomb` (boot restore via `0x55`) | `0x20a1ec2b8` |
| `performPrepareSaveCatacombCommand:outDataSize:` (v1: 4B userID, v2: 24B component) | `0x20a1ed87c` |
| `performCompleteSaveCatacombCommand:outBuffer:` | `0x20a1edd38` |
| `performConfirmSaveCatacombCommand:` | `0x20a1ee1fc` |
| `performLoadCatacombCommand:inData:` (host cmd 64) | `0x20a1ee7f4` |
| `performGetCatacombHashCommand:outHash:` (33 bytes) | `0x20a1ef9ac` |
| `_SaveDataToCatacomb` (open/write/close/fcntl 51) | `0x20a20a058` |
| `___GetCatacombDirectoryPath` (`Library/Catacomb`) | `0x20a20a580` |
| `isXARTAvailableWithClient:` | `0x20a1df93c` |

Log correlations:

- `build/sep/sbio-trace.log:3337` GetCatacombState 168 B; `:3366/3367` `0x6c`
  24→9611; `:3416/3417` `0x33` 4→16; `:3439/3440` `0x36` 4→32;
  `:3494→3501` `0x37` 383 ms; `:6724/6727` `0x6c` 16,379 = 16,356 + 23.
- `tri53-sep6-combined-20261026-2021/5-final.txt`: `tx=10 rx=17`,
  `endpoints_complete=8`, `sbio=0x08 scrd=0x0a sse=0x0c sks=0x12`.
- `tri53-sep6-sbio-20261026-2103/5-sbio-init.txt`:
  `ep=0x08 cmd=0x73 ret=-5 sep_status=0x101` (envelope rejection).
- `docs/adt/j71t.txt:3436` `xart-vol` → `/private/xarts`, role 256;
  `build/firmware/private/IODeviceTree.txt` `xART` node + `xart-vol`.

Reproduce:

```sh
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff0066a504c
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff0062f5188
python3 scripts/ghidra.py decompile_function_by_address address=0xfffffff0062f0918
objdump -d --start-address=0x20a1ea860 --stop-address=0x20a1eb300 \
    build/dsc/out/libBKDM1.dylib
```

---

## 13. Confidence summary

| claim | confidence |
|---|---|
| host command ↔ SEP opcode mapping (incl. `0x33`=Id, `0x36`=Hash, `0x37`=Confirm, `0x6c`=Prepare, `0x6d`=Load, `0x55`=ReadCatacombSU, `0x2d`=NoCatacomb) | high (switch + name table + trace) |
| `CompleteSaveCatacomb` performs no SEP transfer | high |
| 24-byte component key (v2) / 4-byte userID (v1) | high |
| xART record layout, crc32, monotonic counter, wipe/dup rules | high |
| capability 0 forbids storage ops and a violation panics XNU | high |
| capability 0 does not abort sepOS bootstrap | high (hardware) |
| catacomb file is AP-owned, `Library/Catacomb`, archive + legacy `CAT1` | high for format/paths; medium for exact write durability |
| `0x55` is the boot restore path, `0x6d` the migration path | medium |
| capability 0 still permits enrollment (protocol-wise) | medium — inference, hardware-untested |
| SEP accepts a restored catacomb without xART | unknown |
| `catacomb_state_v1_t` size / `catacomb_header_t` fields | unknown |

## 14. Hard safety restated

No step in this document writes syscfg, NVRAM, or boot-firmware storage. The
proposed store is a file tree under `/var/lib/sep-a9/` on the existing p2 ext4
root, inside `nvme.write_window`. No SEP message is guessed: every opcode used
above is either proven on hardware or reconstructed from the AP-side consumer
with its exact sizes. Anything else stops and cold-resets.
