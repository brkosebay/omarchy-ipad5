# Keybag: option B (Linux password unlocks the SEP user bag)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Chosen 2026-10-24. Target model: **one password, no secret on disk, fingerprint
only works while the bag is unlocked** — the same shape as iOS.

```
boot ──► bag locked ──► Omarchy password (PAM) ──► SEP unlock ──► fprintd works
                              │                                      │
                              └─ key derived, never stored           └─ suspend can re-lock
```

## Layers

| piece | role |
|---|---|
| `sep-keybag` helper (root, `/usr/libexec`) | derives the key from the password, calls the driver, wipes memory |
| driver (`sep-a9`) | owns the SKS keybag handle; gates BIO operations on it |
| `pam_exec` `expose_authtok` | runs the helper on the password path (sudo + `omarchy-lock-password`) |
| `pam_fprintd` | normal fingerprint auth once the bag is open |

Two ways to make the bag secret *be* the password:

**B1 — SEP-native passcode (preferred, unproven on A9).** Ask sepOS to create a
user bag whose key is derived from the passphrase, via the AKS/SKS passcode
path (`set passcode` / `change lock state`). Hardware-bound, offline guessing
impossible, exactly what iOS does. Needs: sepOS to accept a new bag with a
chosen passphrase on this iOS 16 build, without disturbing the existing iOS bag.

**B2 — host-side wrapping (fallback).** Create the bag with a random secret,
store only `wrap(secret, KDF(password, salt))` in
`/var/lib/sep-a9/bag.meta`, and unlock by unwrapping. No SEP policy changes,
but a disk image allows offline guessing, so it is weaker than B1.

Both keep the secret out of argv/env/files, and both require the password on
the first unlock after boot. B1 is what "secure" means here; B2 is what we can
ship if sepOS refuses B1.

## Open questions the enrollment trace must answer

1. When iOS sets up the first fingerprint, does it *create* a user bag
   (SKS `CREATE`/`CHANGE_LOCK_STATE`), or does the bag already exist from the
   passcode and enrollment only adds an identity?
2. Which endpoint carries it — `sks` (0x12) or `scrd` — and with what
   in/out lengths?
3. Is there a host-visible bag handle/UUID that survives reboot (so we can
   re-open the same bag), or is it re-derived from the passcode every time?

If (1) says "bag is created at enrollment", B1 is realistic: we can create our
own bag with our own passphrase. If the bag is a passcode artifact, we either
adopt the iOS passcode (option A) or go B2.

## Driver changes implied

- new ioctls (extend the Aurora ABI, do not break it):
  `SEP_BIO_KEYBAG_STATUS`, `SEP_BIO_KEYBAG_UNLOCK`, `SEP_BIO_KEYBAG_LOCK`;
- `GET_INFO.sensor_present` stays 0 and enrol/verify return `-EACCES` until
  the bag is unlocked — fprintd then simply offers nothing, which is the
  correct fail-closed behaviour;
- the driver never persists the key; it holds it only for the SKS call.

## PAM plumbing (same as t2-touchid-linux, hardened)

```
auth [success=1 default=ignore] pam_exec.so quiet expose_authtok seteuid /usr/libexec/sep-keybag pam-unlock
auth sufficient                 pam_fprintd.so
```

`pam-unlock` reads the password from stdin only, derives the key, makes one
`KEYBAG_UNLOCK` call, wipes the buffer, and always exits 0 so it can never
block password auth. First unlock after boot is the password; everything after
that can be a fingerprint.

## Honest limits

- Protects the biometric templates and gates the desktop. The Omarchy rootfs
  is not encrypted by the SEP, and checkm8 means the boot chain is not a trust
  boundary. Same caveat as the rest of this port.
- If B1 is refused, blackbird (patching sepOS policy) is the escape hatch, but
  it turns "SEP-enforced" into "we patched the policy", which is a different
  security claim and should be stated as such.
