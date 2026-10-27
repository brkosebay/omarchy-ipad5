# ios-sep-art

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


Probe the 20-byte SEP anti-replay token (ART) from a jailbroken iPad. The
tool calls `AppleSEPUserClient::GetART` once; it never invokes SetART,
ClearART, PurgeSystemToken, or an NVRAM writer. This is read-only with respect
to persistent storage, but GetART itself talks to SEP and its kernel
error-recovery path may re-submit the volatile ART page.

## Kernelcache facts (iOS 16.7.10, iPad6,11 / j71t)

- `AppleSEPUserClient` instance vtable: `0xfffffff006eb4960`.
- Its `IOExternalMethodDispatch` table: `0xfffffff006eb4fc0`,
  0x18-byte entries.
- **GetART selector: `0x43`**
  - function `0xfffffff0062eac08`
  - scalar input 0, structure input 0
  - scalar output 0, **structure output 20 bytes**
  - it calls `0xfffffff0062efd20`, which asks the running SEP for a
    20-byte token (tag-1 `op 0x03`, then five `op 0x04` reads with
    params 0..4).
- `AppleSEPUserClient::initWithTask` checks
  `com.apple.private.applesepmanager.allow`; a missing entitlement therefore
  fails at `IOServiceOpen`, before dispatch.
- The provider is `AppleSEPManager`.

The ART length is **20 bytes**, not 32/48. This selector reads the live
sepOS ART-storage endpoint; the cold booter instead obtains its candidate
from `AppleSEPARTService`'s NVRAM-backed `com.apple.System.sep.art` path.
Those values are not proven interchangeable. On this iPad the live NVRAM
statistics say `com.apple.System.sep.art` is absent and the manager's DT
provider has no visible `has-art` property, so ART is not the leading boot
hypothesis. Treat this tool as a presence probe: an all-zero result is not a
usable token.

## Build

```sh
sh tools/ios-sep-art/build.sh
```

Needs Xcode command-line tools and `ldid` (`brew install ldid`). Output:
`build/ios-sep-art/ios-sep-art`, signed with
`sep.entitlements.plist`.

## Run (orchestrator)

Boot the jailbroken iOS side, then:

```sh
tools/ios-sep-art/run.sh
```

It uploads the binary to `/var/mobile/linux-a9/ios-sep-art`, runs it,
pulls the token to `build/sep/art/art.bin`, and prints the SHA-256 and
length. It first runs `--probe`, which reads registry properties and opens
the user client without calling GetART. It tries the `mobile` user first;
export `IPAD_PW` if
`IOServiceOpen` needs the sudo fallback.

`build/` is not committed. The token is device-specific; do not commit
it.

## Feed it to pongoOS

Fresh DFU, then:

```sh
scripts/sep/probe-sep.sh pongo
scripts/sep/pongo-sep.sh --xnu art status build/sep/art/art.bin
```

or interactively:

```text
/send build/sep/art/art.bin
sep art
sep xnu art status
sep stat
```

`--xnu art status` runs the conditional stock ART path:
`status 1 -> boot_tz0 -> status 2 -> artload (op 7) -> fwload/boot (op 6)`.
The expected acknowledgments are `0x6b` for ART and `0x6a` for the firmware
command, followed by sepOS `ep=0 op=0xd`. Cold boot never sends `op 0x08`;
`0x6c` is the resume-from-RAM acknowledgment.

If GetART fails with `kIOReturnNotPrivileged`, first check that the private
entitlement survived signing (`ldid -e`). A successful call on booted iOS may
use the running sepOS transport; it does not prove the cold SEPROM path would
accept the same bytes.
