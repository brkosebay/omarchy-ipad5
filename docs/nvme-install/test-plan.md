# NVMe install: owner-run test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


For the orchestrator. Every command runs on the Mac from the repo root. Why
it's done this way: [`DESIGN.md`](DESIGN.md). Scripts: [`README.md`](README.md).

- **Two writers, never at once.** iOS writes the APFS resize and the GPT
  (steps 3–4). Linux writes only p2, the new partition, and the kernel
  refuses everything else (`nvme.write_window`, gate 1).
- **NSIDs 2–7 are never written.** Two independent kernel gates guard them,
  and every tool refuses them too.
- **iOS `sudo` asks for the owner's password**, so those steps use
  `scripts/ipad-ssh.sh -t` and the owner types it.
- **Stop points** are marked **STOP**. At a STOP, report the output; don't
  retry.
- Every Linux step writes a log to `build/nvme-install/logs/`.

## 0. Offline, before any device step

1. **Integration kernel with the write window.** Add
   `patches/nvme-install/nvme-install.config` to `FRAGMENTS` in
   `scripts/integrate.sh`, then:
   ```sh
   scripts/integrate.sh 64221b189e d67fe465f2 f9910570b7 6ca4db7756 fbe859a481
   grep -x CONFIG_NVME_PCI_WRITE_WINDOW=y build/kernel-integ/config
   scripts/omarchy/check-fragment.sh build/kernel-integ/config
   ```
   - `d67fe465f2` is `a9/nvme-install`, which contains pcie `946c7da17f`, so
     it takes pcie's slot.
   - A trial merge of exactly this set was clean.
   - Without the fragment the kernel stays fully read-only, and
     `mkpayload.sh window|nvme` refuses to build.
2. **Build the rest:**
   ```sh
   scripts/nvme-install/build-initramfs.sh   # build/nvme-install/initramfs-omarchy.gz: initramfs-nbd.gz + the new /init
   scripts/nvme-install/build-ios-tools.sh   # build/nvme-install/ios/{resize_apfs,gptcas} (ldid-signed)
   ```
3. **Offline tests.** They all pass today; re-run them after any change.
   ```sh
   scripts/nvme-install/test-gpt.sh          # 25 PASS: plan, write, sgdisk, tamper, reverse (Linux + macOS gptcas)
   scripts/nvme-install/qemu-test.sh fence   # 103 PASS: write window in front of QEMU's NVMe
   scripts/nvme-install/qemu-test.sh init    # 16 PASS: omarchy.root=nvme and every fallback
   scripts/nvme-install/qemu-test.sh tool    # 24 PASS: nvme-install.py end to end
   ```

## 1. Backup: read-only, before the first write (Linux)

Any Linux boot with SSH works, including today's live Omarchy session on
the read-only integ kernel.

```sh
scripts/nvme-install/backup.sh            # add --full for the whole NSID 1 (~32 GB read, slow; optional)
```

Expected:
- `NSID 1 is nvme0n1: 32000000000 bytes, 7812500 LBAs of 4096; p1 at
  512-byte sector 48, 62499912 sectors` (LBA 6 and 7,812,489 LBAs; the
  device name may differ);
- `dumps verified (device sha256 = Mac sha256)`;
- `apfs.env` with `APFS_BLOCK_SIZE=4096`, `APFS_BLOCK_COUNT` ≤ 7812489 and
  `APFS_USED_BYTES` (iOS's real usage);
- `info.txt` lists NSIDs 1–7, `write_window=(not in this kernel)` or empty,
  and the `read-only:` boot line.

Then check the partition table: `cat build/nvme-install/backup/sfdisk.txt`
should show exactly one partition, `start=6, size=7812489,
type=7C3457EF-0000-11AA-AA11-00306543ECAC`.

**STOP** on a hash mismatch, an LBA size other than 4096, or more than one
partition.

An `apfs-nx.py` warning isn't a stop: the dumps are what matter. Plan in
step 2 with `--apfs-used-bytes`. On iOS, `df -k / /private/var
/private/preboot` gives the used column; sum it and multiply by 1024.

- Keep `build/nvme-install/backup/` forever. The files are made read-only,
  and the script refuses to overwrite them. Copying it off the Mac as well
  is the owner's call.
- `--full` stores this exact iOS, so it could be put back without a restore
  and the jailbreak set-up again. It costs about 20 min and up to 32 GB of
  Mac disk (~63 GB free). It's optional: a DFU restore is the documented way
  back.

## 2. Plan the layout, prepare the image (Mac)

```sh
python3 -I scripts/nvme-install/plan-layout.py --backup build/nvme-install/backup \
    --apfs build/nvme-install/backup/apfs.env            # optional: --apfs-gib N
cat build/nvme-install/layout.env
```

- The planner prints the table, `resize_apfs disk0s1 <APFS_BYTES>` and
  `nvme.write_window=1:<P2_FIRST>-<P2_LAST>`.
- Default iOS size: iOS's used space + 3 GiB, rounded up to 1 GiB, at least
  12 GiB. If step 1 couldn't read APFS, add `--apfs-used-bytes N`. **Owner decision:** keep it, or give iOS more room with
  `--apfs-gib`. To replan, delete `layout.env` and `gpt-plan/` first.

The image is made only while the iPad doesn't run from `rootfs.img`: Linux
shut down and `serve-root.sh` stopped. The script refuses otherwise.

```sh
df -h .                                    # a few GB free is enough (APFS clone; resize2fs moves some blocks)
scripts/nvme-install/prep-image.sh         # build/nvme-install/rootfs-nvme.img (+ .sha256), exactly p2's size
```

Expected: `e2fsck` passes 1–5, the label `omarchy-nvme`, `Block count` =
`P2_BYTES`/4096, and `ready: ...`.

## 3. iOS: shrink the APFS container (first write, made by iOS)

The iPad boots iOS normally (a power cycle from Linux does that). Battery
should be ≥ 50 % and on the charger, with no apps open.

1. **Facts** (read-only):
   ```sh
   scripts/ipad-ssh.sh 'ls -l /dev/disk0 /dev/rdisk0 /dev/disk0s1 /dev/disk0s1s*; df -k / /private/var /private/preboot; mount | grep apfs'
   ```
   Report the output. Expected: `disk0`, `disk0s1` and its volumes
   `disk0s1s*`, and no `disk0s2`.
2. **Copy the tools and the plan:**
   ```sh
   scripts/ipad-ssh.sh 'mkdir -p /var/mobile/linux-a9/nvme-install'
   IPAD_SCP=1 scripts/ipad-ssh.sh -r build/nvme-install/ios/resize_apfs build/nvme-install/ios/gptcas \
       build/nvme-install/gpt-plan mobile@127.0.0.1:/var/mobile/linux-a9/nvme-install/
   scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9/nvme-install && for f in resize_apfs gptcas gpt-plan/*; do
       sha256sum "$f" 2>/dev/null || shasum -a 256 "$f" 2>/dev/null || openssl dgst -sha256 "$f"; done'
   cat build/nvme-install/ios/SHA256SUMS; shasum -a 256 build/nvme-install/gpt-plan/*
   ```
   The hashes must match.
3. **Dry run of the GPT tool** (reads only):
   ```sh
   scripts/ipad-ssh.sh -t 'sudo /var/mobile/linux-a9/nvme-install/gptcas --dry-run /dev/rdisk0 /var/mobile/linux-a9/nvme-install/gpt-plan; echo rc=$?'
   ```
   Expected: `primary GPT (LBA 1+5): the original, to be replaced`, the
   same for `backup GPT (LBA 7812495+5)`, then `APFS container: <about 32
   GB> bytes (checkpoint xid N)`, then `doesn't fit the new partition 1 ...
   run resize_apfs first; nothing written`, and `rc=3`. That proves:
   - the tool launches with raw-disk access;
   - the disk still holds exactly the dumped GPT;
   - it reads iOS's live APFS checkpoints.

   **STOP** if it can't open `/dev/rdisk0` (entitlements or sandbox). Retry
   once with `/dev/disk0`, which Hoolock's gdisk uses, before reporting.
   Also **STOP** if it says a GPT copy `is neither the plan's old nor its new
   copy`, or `disk is ... the plan is for ...`. The latter is the wrong disk
   or geometry: never retry with another device.
4. **Resize.** Replace `<APFS_BYTES>` with the value from
   `build/nvme-install/layout.env`. iOS freezes until it is done, and so
   does this SSH session; wait for it.
   ```sh
   grep APFS_BYTES= build/nvme-install/layout.env
   scripts/ipad-ssh.sh -t 'sudo /var/mobile/linux-a9/nvme-install/resize_apfs disk0s1 <APFS_BYTES>; echo rc=$?'
   ```
   Expected: `About to resize disk0s1 to <APFS_BYTES> bytes ...`, then
   `The operation completed successfully`, then `rc=0`.
   - The duration is unknown (minutes). It moves iOS data out of the
     region being cut.
   - If SSH drops, wait 10 minutes, then reconnect and run
     `scripts/ipad-ssh.sh 'df -k /private/var'`.
   - **STOP A** if it prints `APFSContainerResize failed: ...`. Nothing has
     changed. Report the error; the fallback is option C (`DESIGN.md`).
   - If the iPad rebooted itself during the resize (a watchdog panic), boot
     iOS and report `df -k`; don't retry yet.
5. **Check, then boot iOS once with the smaller container**, before
   touching the GPT:
   ```sh
   scripts/ipad-ssh.sh -t 'sudo /var/mobile/linux-a9/nvme-install/gptcas --dry-run /dev/rdisk0 /var/mobile/linux-a9/nvme-install/gpt-plan; echo rc=$?'
   ```
   - Expected: `APFS container: <APFS_BYTES> bytes`, `partition 1 LBAs
     6-<P1_LAST_OLD> -> 6-<P1_LAST>`, `dry run: every check passed`,
     `rc=0`.
   - If it now says the primary or backup GPT `is neither the plan's old
     nor its new copy`, iOS's resizer edited the partition map itself. That
     isn't a failure. Boot Linux, run `backup.sh build/nvme-install/backup-resized`,
     delete `layout.env` and `gpt-plan/`, replan from the new dump with
     `--apfs-gib` = the size just used, and copy the new `gpt-plan/` over.
   - Restart iOS (power button), log in, then check that
     `scripts/ipad-ssh.sh 'df -k /private/var; uptime'` works.
   - **STOP B** if iOS doesn't boot. The only exit is a DFU restore; no
     Linux data exists yet.

## 4. iOS: write the new GPT

```sh
scripts/ipad-ssh.sh -t 'sudo /var/mobile/linux-a9/nvme-install/gptcas --dry-run /dev/rdisk0 /var/mobile/linux-a9/nvme-install/gpt-plan && sudo /var/mobile/linux-a9/nvme-install/gptcas /dev/rdisk0 /var/mobile/linux-a9/nvme-install/gpt-plan; echo rc=$?'
```

Expected: the dry-run lines, then `wrote and verified the new GPT (primary
and backup)`, then `rc=0`.
- The first write-mode open of `/dev/rdisk0` happens here. If it's refused,
  nothing was written; use `/dev/disk0` in both commands.
- If the run is cut off between the two copies, run the same command
  again: `gptcas` finishes a half-written GPT (and `--reverse` undoes one).

Then restart iOS and check:

```sh
scripts/ipad-ssh.sh 'ls -l /dev/disk0s2; df -k /private/var'
scripts/ipad-ssh.sh -t 'sudo /var/mobile/linux-a9/nvme-install/gptcas --dry-run /dev/rdisk0 /var/mobile/linux-a9/nvme-install/gpt-plan'   # "already holds the new GPT"
```

- **STOP C** if iOS doesn't boot. The pongoOS boot is then down too. The
  exit is a DFU restore, which puts back iOS and a one-partition GPT; Linux
  holds no data yet.
- Rollback while iOS runs: `sudo .../gptcas --reverse /dev/rdisk0 .../gpt-plan`,
  then `sudo .../resize_apfs disk0s1 0` (grow back to the partition).

## 5. Linux: validate the write path (window kernel, NBD root)

Session 1. Start `scripts/omarchy/serve-root.sh` in terminal 1 as usual,
then:

```sh
scripts/nvme-install/mkpayload.sh window        # build/nvme-install/m1n1-linux-window.bin
scripts/boot.sh pongo
scripts/boot.sh linux build/nvme-install/m1n1-linux-window.bin
```

On screen, or later with `ssh ... sudo dmesg`:
- `read-only except NVMe Write to NSID 1 LBAs <P2_FIRST>-<P2_LAST>
  (nvme.write_window): no trim, no write zeroes, no destructive admin
  commands`;
- `write window: controller ONCS 0x... VWC 0x... DMRSL ...`. Report it:
  this is the first look at whether the ANS has a volatile write cache;
- `nvme0n1: p1 p2`;
- `Apple ANS: namespaces other than NSID 1 are read-only`.

Then, once SSH is up:

1. `scripts/nvme-install/run.sh info` and `scripts/nvme-install/run.sh check`.
   Expected for `check`: every line `PASS` (window, NSID 1 writable, NSIDs
   2–7 `ro=1`, exactly p1 and p2 at the planned LBAs, PARTUUID, not
   mounted), then `RESULT all PASS`. **STOP** on any FAIL.
2. `scripts/nvme-install/run.sh hash-outside s1-start`. It reads everything
   outside p2 (about 12 GiB, ~40 s) and NSIDs 2–7. NSID 2–4 lines should say
   `matches the Mac's dump`. NVRAM (NSID 5), and possibly NSID 6, can `WARN`
   because iOS rewrites them.
3. `scripts/nvme-install/run.sh fence-test`. This is the one deliberate
   exception to "nothing writes outside p2". It sends seven writes that the
   kernel must refuse, all into the two unowned gaps or straddling p2's
   edges into them. Gap sectors belong to nobody after the resize, so even
   a broken fence costs nothing there. Running it is the owner's choice.
   Expected:
   - all PASS;
   - 7 writes into the unowned gaps, or straddling p2's edges, each
     `Input/output error`;
   - `gap-a/gap-b unchanged`, `kernel logged 7 refused writes` with their
     LBAs;
   - trim and write zeroes not advertised, `BLKDISCARD on p2: Operation not
     supported`;
   - NSIDs 2–7 `write: Operation not permitted`.

   **STOP D** on any FAIL, e.g. `was NOT refused`. Only unowned gap sectors
   can have been touched.
4. `scripts/nvme-install/run.sh soak --size-gib 4`. Expected (~3–5 min):
   - PASS for both sequential passes, the parallel writers, the random
     4 KiB writes and the page-cache path;
   - `sequential region still intact`;
   - INFO lines with write and read MiB/s, IOPS and fsync latency. Report
     them all.

   **STOP E** on any FAIL, or if the shell hangs. Report it with:
   ```sh
   scripts/omarchy/ssh.sh 'sudo dmesg | grep -iE "nvme|nvmmu|dart|fault|timeout|reset|abort"'
   ```
   Don't re-run blindly. The next step is RE of iOS's write path in Ghidra.
5. `scripts/nvme-install/run.sh hash-outside s1-end --compare s1-start`.
   Expected: every REGION and NSID line `unchanged`, `RESULT all PASS`.
   **STOP F** if anything outside p2 changed.
6. Power off: `scripts/omarchy/ssh.sh 'sudo systemctl poweroff'`, then stop
   `serve-root.sh`.

## 6. Persistence across a shutdown and an iOS boot

1. Power on: iOS boots. Use it for a minute (it writes p1, and maybe NVRAM),
   then check `scripts/ipad-ssh.sh 'df -k /private/var'`.
2. Session 2: same `window` payload, `serve-root.sh` running. Then:
   ```sh
   scripts/nvme-install/run.sh check
   scripts/nvme-install/run.sh verify --size-gib 4 --tag 1
   scripts/nvme-install/run.sh hash-outside s2-start --compare s1-end
   ```
   Expected:
   - `check` all PASS;
   - `verify`: `sequential region (tag 1): 1048576 blocks verified`. The
     data survived the shutdown, a cold controller start and iOS. iOS
     doesn't touch p2.
   - The compare should say `REGION p1 ... CHANGED` (iOS ran) and maybe
     `NSID 5 ... CHANGED` (NVRAM). The GPT copies and both gaps must be
     `unchanged`. **STOP** if they aren't.

## 7. Install the root (still session 2)

```sh
scripts/nvme-install/run.sh write-image
P2=$(grep '^P2_PARTUUID=' build/nvme-install/layout.env | cut -d= -f2)
scripts/omarchy/ssh.sh "sudo e2fsck -fn /dev/disk/by-partuuid/$P2"
scripts/nvme-install/run.sh hash-outside s2-end --compare s2-start
```

Expected:
- `write-image`: progress every GiB, `stream sha256 matches the Mac's
  image`, `p2 holds exactly the Mac's image`. That is a full read-back of
  every byte written, against the Mac's hash. It takes ~5–10 min (USB);
  zeros compress away.
- `e2fsck`: clean, label `omarchy-nvme`.
- `hash-outside`: all `unchanged`.

**STOP** on a mismatch: run `write-image` once more. It is idempotent and
overwrites only p2. If it fails twice, report.

Power off as in 5.6.

## 8. Boot Omarchy from the NVMe

```sh
scripts/nvme-install/mkpayload.sh nvme          # adds omarchy.root=nvme
scripts/omarchy/serve-root.sh                    # terminal 1: only the fallback this time
scripts/boot.sh pongo
scripts/boot.sh linux build/nvme-install/m1n1-linux-nvme.bin
```

- On screen (initramfs lines carry `[omarchy-rd]`): `Waiting up to 60s for
  the NVMe root (window, write window 1:...)`, `/dev/nvme0n1p2 is the write
  window ...` (the name may differ), `Switching to
  /usr/lib/systemd/systemd on /dev/nvme0n1p2`.
- `serve-root.sh` should see no NBD connection.
- A fallback shows `...: falling back to the NBD root` and the system comes
  up from NBD. Report the line before it.

Checks:
```sh
scripts/omarchy/ssh.sh 'findmnt -no SOURCE,FSTYPE,OPTIONS /; systemctl is-system-running; systemctl --failed --no-legend
  systemd-analyze; cat /sys/module/nvme/parameters/write_window; sudo dmesg | grep -iE "refused|nvmmu|fault|timeout"'
scripts/omarchy/ssh.sh 'dd if=/dev/urandom of=$HOME/nvme-test bs=1M count=256 conv=fsync status=none && sha256sum $HOME/nvme-test'
scripts/omarchy/ssh.sh 'sudo systemctl start sddm'     # desktop; compare with the NBD boot
```

Expected:
- `/dev/nvme0n1p2 ext4 rw,noatime`;
- `running` (or `degraded` with the list). `fstrim.service`/`fstrim.timer`
  failing is harmless: trim isn't advertised yet. It's not a write-path
  fault;
- the window printed;
- no `refused` line, since ext4 never writes outside its own partition.

Power off, boot iOS once, boot the `nvme` payload again, then
`sha256sum $HOME/nvme-test` must match. Report `systemd-analyze` and how
the desktop feels compared with NBD.

- From now on the `nvme` payload is the normal boot.
- The NBD image and the NVMe root are separate copies (same users and
  keys). Changes on one don't reach the other.

## Recovery

| Situation | Do |
|---|---|
| A step printed FAIL or REFUSED | stop; send the log from `build/nvme-install/logs/` |
| NVMe root doesn't come up, NBD fallback did | boot the `window` payload, then `e2fsck -fn` (step 7) and `run.sh check`; report |
| NVMe root boots but the system is broken | `window` payload (NBD root), then `sudo e2fsck -fy /dev/disk/by-partuuid/$P2`, or `run.sh write-image` again |
| iOS needs its space back, Linux goes | iOS: `gptcas --reverse ...`, then `resize_apfs disk0s1 0` |
| iOS doesn't boot after step 3 or 4 | DFU restore (Finder or `idevicerestore`); NSIDs 2–7 untouched, dumps in `build/firmware/private/nvme-ns/` |
| Want this exact iOS back | only with `backup.sh --full`: ask before using it (writing NSID 1 outside p2 needs a one-off window boot) |

## Report template

```
1 backup:   NSID1 dev, geometry line, apfs.env, sfdisk.txt partition line
2 plan:     planner table, chosen --apfs-gib
3 iOS:      facts, dry-run rc=3 text, resize output + duration, post-resize dry run, iOS reboot ok?
4 GPT:      gptcas output, /dev/disk0s2, iOS reboot ok?
5 session1: boot lines (window, ONCS/VWC), check, fence-test, soak INFO lines, hash compare
6 session2: verify, hash compare (which regions CHANGED)
7 install:  write-image timing + PASS lines, e2fsck, hash compare
8 nvme:     initramfs lines, findmnt, is-system-running, systemd-analyze, desktop impression, nvme-test sha after reboot
```
