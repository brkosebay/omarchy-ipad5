# Kernel updates over Wi-Fi

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Status: built and tested offline (2026-10-23). No device step has been run.
Nothing is published: there is no signing key, release or tag yet. Creating
those is the owner's call (see [Publishing](#publishing-owners-call)).

Arch/Omarchy packages already update on the iPad without the Mac. This adds the
same for the kernel. A **bundle** (kernel, DTBs, initramfs, manifest and
optional signature) is built on the Mac. `linux-a9-update` on the iPad
downloads it, verifies it and stages it as a new `/boot/linux-a9/entries/NAME`,
with the same layout `stage-kernel.sh` produces. It then warm-boots the entry
**once** as a trial. The entry becomes the default only from inside that trial
boot, after its health checks pass. In every other case the default stays the
known-good kernel, so the next `systemctl reboot` returns to it.

The underlying warm-boot path (`linux-a9-kexec`, merged DT, required args) is
[warm-reboot.md](warm-reboot.md). Updates add no new boot mechanism. A trial
boot is exactly `linux-a9-kexec load NAME` followed by `systemctl kexec`.

## The limit, stated plainly

A kernel that hangs or panics before userspace **cannot roll itself back**.
Recovering from it needs a cold boot from the Mac (buttons → DFU →
`scripts/boot.sh pongo` → `scripts/boot.sh linux <saved payload>`). The design
guarantees only this: **such a kernel never becomes the default**. After that
cold boot, the health unit sees that the trial did not run, marks it `failed`
and leaves the default alone.

A9 has no EL2 and no persistent boot counter that survives the SoC reset, and
the SoC reset itself boots iOS. A late hang also stays a hang, because the
watchdog is deliberately off (warm-reboot.md). A panic in the trial kernel is
the one exception that recovers without the Mac. The trial boot arms
kexec-on-panic with the trial Image as the crash kernel and the **default**
entry as stage 2, so a panic after `linux-a9-kexec-panic.service` ran comes
back on the known-good kernel. That path also merges the DT in the reverse
direction (trial dtb → default dtb) when the two differ. This is untested on
hardware.

Keep the cold payload that matches the running cold entry, and its artifacts,
on the Mac. That is the only way out of a hung trial.

## Bundle format, version 1

`linux-a9-kernel-NAME.tar.zst`, a zstd-compressed ustar/pax archive with exactly these
regular files (anything else, including links, `..` and absolute paths, is rejected):

| Member | Content |
| --- | --- |
| `manifest.json` | first member; described below |
| `Image.gz` (or `Image`) | arm64 kernel |
| `initramfs.gz` | `build/boot/initramfs-omarchy.gz` (early-USB/rescue overlay) |
| `dtbs/*.dtb` | pristine board DTBs; default: j71s, j71t, j72s, j72t |

`manifest.json`:

> Private graphics-dependent excerpt omitted from this source snapshot.


- **Signature**: an optional detached `linux-a9-kernel-NAME.tar.zst.sig`, made
  by `ssh-keygen -Y sign -n linux-a9-kernel` over the whole compressed file. It
  is verified **before** anything is decompressed. The device keeps public keys
  in `/etc/linux-a9/update-signers` (ssh `allowed_signers` format, principal
  `linux-a9-kernel`). `ssh-keygen` ships on macOS, in the kbuild image and in
  Arch's `openssh`. minisign is in none of them, so it isn't used.
- **Index**: `linux-a9-latest.json` names the bundle (relative to the index
  URL), its sha256 and size, the signature file, `min_tool_version`, the commit
  and the release. `check`/`fetch`/`update` read it. Its sha256 binds the
  download to the index. The signature is what authenticates the bundle.
- **Command line**: a bundle doesn't carry a full cmdline, because
  `nvme.write_window=` and similar values are specific to this iPad's install.
  An installed entry's `cmdline` is the **default entry's** `cmdline` file (or
  `/proc/cmdline` if it has none), with these changes:
  1. Per-boot tokens are dropped (`linux_a9.entry=`, `BOOT_IMAGE=`, `initrd=`,
     `elfcorehdr=`, `reset_devices`, `linux_a9.phoenix*`).
  2. `remove` keys are dropped.
  3. `required` arguments replace the same key or are appended. `recommended`
     ones do the same unless `RECOMMENDED_ARGS=0`, then `CMDLINE_EXTRA` is applied.

  The result must still contain every key in `REQUIRE_KEYS` (default
  `omarchy.root nvme.write_window`); otherwise the root would not mount
  writable. `--cmdline "..."` replaces the base; required arguments still apply.
- **Versioning**: `format_version` changes only for incompatible layouts.
  `min_tool_version` is the integer in `scripts/boot/device/VERSION` (installed
  as `/usr/local/lib/linux-a9/VERSION`). Bump it when a bundle depends on new
  device-tool behavior. Older tools then refuse the bundle and say to run
  `install-device.sh`.

## Host: building a bundle

```sh
# Ancestry gives the branch heads; same command as the Round 4 runbook
scripts/build-kernel.sh --run integ \
  'git log --first-parent --format="%H%x09%s%x09%P" hoolock..HEAD > /work/build/boot/integ-ancestry.txt'
TREE=integ scripts/boot/make-bundle.sh --ancestry build/boot/integ-ancestry.txt [--sign KEY] [--name NAME]
```

It reads `build/kernel-$TREE/{Image.gz,dtbs,config,COMMIT,DIRTY}` and
`build/boot/initramfs-omarchy.gz`. It writes `build/boot/bundles/`
(`linux-a9-kernel-NAME.tar.zst`, `.sig` if signed, `linux-a9-latest.json`). It
refuses:

- a dirty build (unless `--allow-dirty`);
- a config without `CONFIG_KEXEC_FILE=y` or `CONFIG_NVME_PCI_WRITE_WINDOW=y`;
- an Image whose `Linux version ...-g<hash>` does not match `COMMIT`;
- a required or recommended `module.param` that the Image does not contain (an
  unknown parameter would otherwise be silently ignored);
- an initramfs that is not gzip cpio.

Image and DTB validation reuse `prepare-entry.py` (`image()`, the FDT parser).
Ancestry parsing is `verify-payload.py`'s `integration_branches()`, factored out
unchanged. Use `--require`/`--recommend`/`--remove ARG` (repeatable) to change
the command-line policy. When the required warm-boot arguments are no longer
needed, pass an explicit list. The tar is deterministic (fixed order, owner and
mtime); `created` is the only timestamp.

### Signing key (owner's call; not created by this work)

```sh
ssh-keygen -t ed25519 -C linux-a9-kernel -f ~/.config/linux-a9/kernel-signing   # keep private, with a passphrase
printf 'linux-a9-kernel namespaces="linux-a9-kernel" %s\n' \
  "$(cat ~/.config/linux-a9/kernel-signing.pub)" > build/boot/update-signers
# or commit that line as scripts/boot/device/update-signers to make it the project key
scripts/boot/install-device.sh          # installs it as /etc/linux-a9/update-signers
TREE=integ scripts/boot/make-bundle.sh --sign ~/.config/linux-a9/kernel-signing ...
```

Policy on the device:

- **Remote (http/https) bundles need a valid signature** unless `--insecure`.
- A local file with a `.sig` next to it is verified when a signers file
  exists. A bad signature always fails, even with `--insecure`.
- Unsigned local files are allowed unless `REQUIRE_SIGNATURE=1`.
- https only; plain http mirrors need `ALLOW_HTTP=1` (or `--insecure`), and
  still need the signature.

Limits:

- There is no key rotation or revocation beyond editing the signers file.
- There is no anti-rollback, so an old signed bundle can be offered again.
  `check` shows what it is. Trial boots and the health gate still apply.

## Publishing (owner's call)

Default `SOURCE` is
`https://github.com/brkosebay/linux/releases/latest/download/linux-a9-latest.json`,
the public kernel fork. GitHub redirects `releases/latest/download/<asset>` to
that asset of the newest non-prerelease release. Because the index names the
bundle relatively, the three files must be assets of **the same** release.
Nothing was created or pushed. If the owner wants this, a release could look
like this (not run):

```sh
cd build/boot/bundles
gh release create a9-kernel-integ-f53bf861ca --repo brkosebay/linux \
  --target <the a9/integ commit pushed to the fork> --title "linux-a9 kernel integ-f53bf861ca" \
  linux-a9-kernel-integ-f53bf861ca.tar.zst linux-a9-kernel-integ-f53bf861ca.tar.zst.sig linux-a9-latest.json
```

Any static https host works too: set `SOURCE=` in `/etc/linux-a9/update.conf`,
or pass a URL to `check`/`fetch`/`update`. Publish only signed bundles, because
the device refuses unsigned remote ones by default.

## Device: `linux-a9-update`

Installed by `scripts/boot/install-device.sh` with the other device tools:

- `/usr/local/bin/linux-a9-update` (python3, stdlib only, plus `zstd`,
  `ssh-keygen`, `sha256sum`, `ip`, `findmnt`, which the installer now also
  checks for);
- `/usr/local/lib/linux-a9/VERSION`;
- `linux-a9-update-health.service` (enabled);
- `/etc/linux-a9/update.conf` (only if missing; every key documented in
  `scripts/boot/device/update.conf`);
- `/etc/linux-a9/health.d/`;
- `/etc/linux-a9/update-signers` if `scripts/boot/device/update-signers` or
  `build/boot/update-signers` exists;
- two Omarchy menu entries.

| Command | Effect |
| --- | --- |
| `check [URL]` | read the index; print latest vs. installed (no root, no writes) |
| `fetch [URL\|FILE] [--insecure]` | download bundle + `.sig` to `/boot/linux-a9/update/cache/`, check index sha256 and signature |
| `install BUNDLE [--name N] [--cmdline ARGS] [--insecure] [--no-preflight]` | verify, stage the entry; **default untouched** |
| `update [URL] [--yes] [--boot]` | check + fetch + install, then offer (or with `--boot` do) the trial boot |
| `boot ENTRY [--yes]` | one-shot trial (below) |
| `rollback [--reboot] [--yes] [--force]` | cancel a pending trial, or swap default back to previous |
| `list` / `status` | entries with default/previous/running/trial marks and results; trial state and config |
| `gc [--keep N] [--dry-run]` | delete old entries this tool installed |
| `health` | the unit's job; harmless to run by hand |

`install`, in order:

1. Check the signature against policy.
2. Stream-extract allowlisted members into a `/run/linux-a9` temporary
   directory. Tar paths are never used as output paths.
3. Check the manifest: format, tool version, an exact file set and every
   sha256.
4. Check the arm64 Image magic and the cpio magic.
5. Pick the DTB for `/proc/device-tree/compatible`.
6. Build the cmdline.
7. **DT preflight.** If the DTB differs from the running entry's pristine DTB,
   run `linux-a9-fdt-merge` now, so an unmergeable kernel is refused while
   Linux works rather than at load time. This needs the running cold entry
   staged, as `linux-a9-kexec` does.
8. Check free space (`MIN_FREE_MB`).
9. Publish atomically, under the same `/run/linux-a9/install.lock` as
   `upload-entry.sh`: `.new.XXXX`, fsync, then rename.

The entry holds `Image.gz dtb initramfs.gz cmdline COMMIT manifest.json
SHA256SUMS` (`./name` format, which `linux-a9-kexec`'s `verify_entry` checks).
Entries are immutable. An identical reinstall is a no-op; different contents
under an existing name are refused, so use `--name`. A bundle from the cold
entry's tree therefore needs a new name, because the cold entry already
exists. A bundle downloaded into the cache is deleted after installing.

### Trial boot, health and promotion

```
installed --boot--> trial-booting --(next boot is the trial)--> trial-checking --pass--> good (set-default)
                         |                        |                              \--fail/timeout--> failed
                         |                        \--(next boot is anything else)--> failed
                         \--rollback--> rolled-back
```

- `boot ENTRY` runs `linux-a9-kexec load ENTRY` first. If the load fails, there
  is no trial record and no reboot. Then it writes the trial record (`state.json`
  and the marker `/boot/linux-a9/update/trial`) with the requesting `boot_id`,
  syncs, and runs `systemctl kexec`. If `systemctl kexec` fails, the image is
  unloaded and the trial cancelled. The default symlink is never touched.
- `linux-a9-update-health.service` (`ConditionPathExists=` the marker, ordered
  after `multi-user.target network-online.target sshd.service`) does one of
  the following:
  - **Same boot_id** as the request: does nothing.
  - **Running entry is not the trial**: the trial hung, panicked into the
    default, was rebooted before its checks, or a Mac cold boot happened. It is
    marked `failed` and the default is unchanged.
  - **Running the trial**: it polls the checks every 5 s until all pass or
    `HEALTH_TIMEOUT` (300 s) runs out:
    - `multi-user.target` is active;
    - each unit in `HEALTH_UNITS` (`sshd.service`) is active;
    - if `HEALTH_NETWORK=1`, a non-loopback interface has a global address
      (optionally limited to `HEALTH_NETWORK_IFACES`, e.g. `wlan0`);
    - if `HEALTH_ROOT=nvme`, `/` is `/dev/nvme*` mounted `rw`, and an fsync'd
      write under `/boot/linux-a9/update` works;
    - if `HEALTH_FAILED_UNITS_OK=0`, no unit has failed;
    - every executable in `/etc/linux-a9/health.d/` exits 0 (for example a
      touch or display check).

    It never uses `systemctl is-system-running --wait`, which would wait on
    the unit itself. When everything passes, `linux-a9-kexec set-default ENTRY`
    runs: the old default becomes `previous` and panic recovery is re-armed.
    **This is the only automatic default change, and it only ever happens on
    the trial kernel itself.** On failure the entry is marked `failed` with the
    failing checks, and the default stays put. `AUTO_REBOOT_ON_FAILURE=1` then
    warm-reboots into the default at once. It is off by default: kexec out of a
    half-working kernel can hang and need the Mac, while staying up keeps the
    local desktop for a rollback. With remote-only access (Wi-Fi broken in the
    trial), the trade-off may favour turning it on.
- **Rollback.**
  - With a trial pending, `rollback` marks it `rolled-back` and the health unit
    will not promote it, even if it is mid-check; it checks a trial id under
    the lock. `--reboot` then warm-reboots into the default.
  - With no trial pending, `rollback` runs `set-default previous`, which swaps
    the two, and marks the replaced default `rolled-back`. It refuses to make a
    `failed`/`rolled-back` entry the default unless `--force`, so a second
    rollback can't silently reinstate the bad kernel.
- State lives in `/boot/linux-a9/update/state.json` (entry statuses, trial,
  last result, a 50-event history). `journalctl -t linux-a9-update` has the
  log lines.

### Disk use and what is written

- Each entry is about 13 MiB (Image.gz 10.3 MiB, initramfs 2.2 MiB).
- `gc` keeps the `KEEP` (3) newest entries **installed by this tool** (those
  with a `manifest.json`), **plus** default, previous, the running entry, the
  trial entry and `PROTECT=` names.
- gc never deletes entries staged another way (`stage-kernel.sh`,
  `kexec-host.sh`, the cold-boot entry). `linux-a9-kexec` needs the pristine
  DTB of the entry named by the cold boot's `linux_a9.entry=` for every merge
  and for panic recovery. If a later cold payload names a bundle-installed
  entry, add it to `PROTECT=`.
- gc also removes `.new.*`/`.del.*` leftovers and all but the newest cached
  download. `install` runs no gc implicitly; `MIN_FREE_MB` refuses an install
  that would leave the filesystem too full.
- Written at runtime: `/boot/linux-a9/entries/*`, `/boot/linux-a9/update/*`,
  the `default`/`previous` symlinks (via `linux-a9-kexec`), and temporary files
  in `/run/linux-a9` (tmpfs). `install-device.sh` writes the `/etc`/`/usr/local`
  files listed above.
- Nothing touches syscfg, NVRAM, boot firmware, NVMe namespaces n2–n6 or the
  partition table. There are no raw block-device writes; all writes are
  ordinary file I/O on the Linux root.

### Omarchy menu

`install-device.sh` adds `scripts/boot/device/omarchy-menu-update.jsonc` to
`~/.config/omarchy/extensions/omarchy-menu.jsonc`, keyed by its own first id so
existing installs also get it, beside the shipped **Update › Kernel**:

- **Update › iPad Kernel** opens a floating terminal that runs
  `linux-a9-update update`: check, then prompts to download+install, then
  prompts for the trial boot.
- **Update › Roll Back iPad Kernel** shows `status`, then runs
  `rollback --reboot`, which prompts first.

Both use sudo inside the terminal. The current NOPASSWD rule is temporary, so
a password prompt there is expected later.

## Offline evidence

```sh
scripts/boot/test-update-offline.sh
scripts/boot/test-update-offline.sh --real build/boot/bundles/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst build/boot/integ-entry
```

Both run in `linux-a9-kbuild` (Python 3.14, zstd, OpenSSH) with a fake `/boot`
tree and mocked `linux-a9-kexec` (logging; emulating set-default's previous
rotation), `systemctl`, `ip` and `findmnt`. A throwaway signing key and a local
HTTP server cover the remote path. On 2026-10-23, 25 tests passed in the plain
run, and the `--real` run adds a 26th (all passed):

- **make-bundle**: manifest hashes, signature and index; refusals for a dirty
  tree, a missing config option, an unknown parameter and a COMMIT/Image
  mismatch.
- **Install**: the entry layout and `sha256sum -c`; cmdline merging (strip,
  remove, required, recommended, `CMDLINE_EXTRA`, `REQUIRE_KEYS`); idempotent
  and immutable entries.
- **Refused bundles**: a tampered member, a flipped byte with a signature,
  hostile tar members (`..`, absolute paths, symlinks), the tool-version gate,
  a board mismatch, DT preflight merge/unknown-provenance refusal, and the
  space check.
- **Signatures and downloads**: remote unsigned/forged/lost-signature
  refusals, the https-only rule, index hash binding and `--insecure`.
- **Trial boots**: no trial on a failed load or a failed `systemctl`;
  promotion only inside the trial after its checks; failures of each check
  kind (unit, network, read-only or non-NVMe root, failed units, health.d
  hook) keep the default; hang→cold boot marks the trial failed;
  auto-reboot is opt-in.
- **Rollback and gc**: rollback during a trial and after promotion (with
  `--force` guard); gc keeps protected, foreign and running/trial entries;
  `list`/`status`.

The `--real` test installs the actual 12 MiB `integ-f53bf861ca` bundle (9
branch heads, 4 boards) over a copy of `build/boot/integ-entry`. Image and DTB
are identical, so no merge is needed. The cmdline equals the cold one minus
`linux_a9.entry=` (it already carries the three required arguments). The test
then trial-boots and promotes the bundle. `systemd-analyze verify` of the unit
passed in the Arch ARM image. The menu merge was checked to produce valid
JSONC, to be idempotent and to coexist with existing entries. The refactored
`verify-payload.py` still passes the Round 4 verification. These are offline
checks. They do not establish that a warm trial boot succeeds on hardware.

## Orchestrator device test plan

Precondition: warm reboot itself works, meaning [Round 4](ans-kexec.md#ordered-orchestrator-only-device-plan)
step 2 (same-entry warm boot) passed with `integ-f53bf861ca` cold-booted and
staged as default. If it did not pass, a trial boot hangs the same way and
needs DFU; stop here. Keep the Round 4 payload on the Mac, plus netconsole/ACM
and DFU recovery, ready.

1. **Host: build and test.** No device involved.

   ```sh
   scripts/build-kernel.sh --run integ \
     'git log --first-parent --format="%H%x09%s%x09%P" hoolock..HEAD > /work/build/boot/integ-ancestry.txt'
   TREE=integ scripts/boot/make-bundle.sh --ancestry build/boot/integ-ancestry.txt --name integ-f53bf861ca-b1
   scripts/boot/test-update-offline.sh --real build/boot/bundles/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst build/boot/integ-entry
   ```

   The name must differ from the cold entry `integ-f53bf861ca`, which exists
   with different contents. Signing is optional for this local test. To
   exercise it, create the key (above) first and add `--sign`.

2. **Install the tools.** This does not reboot.

   ```sh
   scripts/boot/install-device.sh
   scripts/omarchy/ssh.sh 'linux-a9-update status; linux-a9-update list; cat /usr/local/lib/linux-a9/VERSION; systemctl is-enabled linux-a9-update-health.service; readlink /boot/linux-a9/default'
   ```

   Require tools v2, the unit enabled, default `entries/integ-f53bf861ca`,
   `running: integ-f53bf861ca`, and the two new menu rows under Update.

3. **Copy and install the bundle.** Default must stay unchanged.

   ```sh
   B=build/boot/bundles/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst
   # /var/tmp, not /tmp: step 7 reuses the file after reboots (tmpfs /tmp is gone by then)
   scripts/omarchy/ssh.sh 'cat > /var/tmp/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst' < "$B"
   [ ! -f "$B.sig" ] || scripts/omarchy/ssh.sh 'cat > /var/tmp/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst.sig' < "$B.sig"
   scripts/omarchy/ssh.sh 'sudo linux-a9-update install /var/tmp/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst && linux-a9-update list && readlink /boot/linux-a9/default && cd /boot/linux-a9/entries/integ-f53bf861ca-b1 && sha256sum -c SHA256SUMS && diff <(tr " " "\n" <cmdline) <(tr " " "\n" <../integ-f53bf861ca/cmdline); true'
   ```

   Require `same dtb as the running kernel`, default unchanged, and a cmdline
   diff of exactly one `> linux_a9.entry=integ-f53bf861ca` line. `diff` itself
   exits 1 for that, and the trailing `true` hides it, so judge the output. Optionally rehearse the
   Wi-Fi download path from the Mac instead of `cat`. Run
   `python3 -m http.server 8099 --directory build/boot/bundles`, then on the
   iPad `sudo linux-a9-update fetch http://<mac-ip>:8099/linux-a9-latest.json --insecure`
   (or `ALLOW_HTTP=1` with a signed bundle), then `install` the printed path.

4. **Trial boot.** This is a warm reboot into the new entry.

   ```sh
   old=$(scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id')
   scripts/omarchy/ssh.sh 'sync; sudo linux-a9-update boot integ-f53bf861ca-b1 --yes' || true
   # after USB/SSH returns (allow ~2 min plus up to HEALTH_TIMEOUT for the verdict):
   scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; cat /proc/cmdline; findmnt /; cat /sys/devices/system/cpu/online'
   scripts/omarchy/ssh.sh 'systemctl status linux-a9-update-health.service --no-pager; journalctl -b -t linux-a9-update --no-pager; linux-a9-update status; readlink /boot/linux-a9/default /boot/linux-a9/previous; cat /sys/kernel/kexec_crash_loaded'
   ```

   Require all of:

   - a boot_id different from `$old`;
   - `linux_a9.entry=integ-f53bf861ca-b1`;
   - NVMe root rw and both CPUs;
   - `last: integ-f53bf861ca-b1 good`;
   - default `entries/integ-f53bf861ca-b1` and previous `entries/integ-f53bf861ca`;
   - crash_loaded=1 (re-armed by set-default).

   If the trial hangs, recover through DFU with the Round 4 payload. After that
   cold boot, require `linux-a9-update status` to show the trial as failed and
   default still `integ-f53bf861ca`. This is the "never promoted" guarantee.

5. **Plain reboot keeps the promoted kernel.** Optional.
   `scripts/omarchy/ssh.sh 'sync; sudo reboot' || true`, then require
   `linux_a9.entry=integ-f53bf861ca-b1`.

6. **Roll back.**

   ```sh
   scripts/omarchy/ssh.sh 'sudo linux-a9-update rollback --yes; readlink /boot/linux-a9/default /boot/linux-a9/previous'
   scripts/omarchy/ssh.sh 'sudo linux-a9-update rollback --yes'; echo "expect nonzero (refused): $?"
   scripts/omarchy/ssh.sh 'sync; sudo linux-a9-kexec reboot default' || true
   scripts/omarchy/ssh.sh 'cat /proc/cmdline; linux-a9-update list'
   ```

   Require:

   - the first command makes default `integ-f53bf861ca` and previous
     `integ-f53bf861ca-b1` (marked rolled-back);
   - the second rollback is refused without `--force`, because previous is
     now the rolled-back entry;
   - the reboot lands on `linux_a9.entry=integ-f53bf861ca`.

   The menu's **Roll Back iPad Kernel** is `rollback --reboot`: the same swap
   followed by the warm reboot in one step.

7. **Negative trial: a failing check must not promote.**

   ```sh
   scripts/omarchy/ssh.sh 'sudo linux-a9-update install /var/tmp/linux-a9-kernel-integ-f53bf861ca-b1.tar.zst --name integ-f53bf861ca-b2 && printf "#!/bin/sh\necho deliberate test failure; exit 1\n" | sudo tee /etc/linux-a9/health.d/99-fail-test >/dev/null && sudo chmod +x /etc/linux-a9/health.d/99-fail-test && echo HEALTH_TIMEOUT=30 | sudo tee -a /etc/linux-a9/update.conf'
   scripts/omarchy/ssh.sh 'sync; sudo linux-a9-update boot integ-f53bf861ca-b2 --yes' || true
   # after return, wait ~40 s:
   scripts/omarchy/ssh.sh 'linux-a9-update status; readlink /boot/linux-a9/default'
   scripts/omarchy/ssh.sh 'sudo rm /etc/linux-a9/health.d/99-fail-test; sudo sed -i "/^HEALTH_TIMEOUT=30$/d" /etc/linux-a9/update.conf; sync; sudo reboot' || true
   scripts/omarchy/ssh.sh 'cat /proc/cmdline'
   ```

   Require b2 `failed` with `health.d/99-fail-test: deliberate test failure`,
   default still `integ-f53bf861ca`, and the plain reboot landing on
   `integ-f53bf861ca`.

8. **Menu and housekeeping.** On the iPad, open Update › iPad Kernel. With
   nothing published, expect a clean download error (HTTP 404 from GitHub) and
   no change. Open Update › Roll Back iPad Kernel, answer `N`, and expect no
   change. Then run:

   ```sh
   scripts/omarchy/ssh.sh 'sudo linux-a9-update gc --dry-run; sudo linux-a9-update gc; linux-a9-update list'
   ```

   Require that `integ-f53bf861ca` and the default/previous entries survive.
   Delete `/var/tmp/linux-a9-kernel-*` afterwards.

Report boot IDs, cmdlines, `status` output and `journalctl -t linux-a9-update`
for each step. A disconnected SSH client alone is not evidence of anything.


Round 5 requires tools **v2** for added-node DTBs: the merger remaps phandle
references by node path, preserving specifier arguments and genuine conflict
checks. New bundles also require `crash_kexec_post_notifiers` so an armed crash
kernel does not bypass the panic dump. Install v2 before installing these
bundles; the manifest tool-version gate prevents an older merger from being
used. The [Round 5 runbook](ans-kexec.md#round-5--suspended-state-panic-recovery-and-phandle-safe-updates)
covers the RAM reservation, phoenix quiesce, retained logs and the suspended-state
recovery gate. The earlier v1 bundle evidence above remains historical.
