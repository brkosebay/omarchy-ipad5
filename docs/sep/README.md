# SEP / Touch ID on iPad 5 (A9)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Goal: run Apple's signed sepOS on j71t and talk to it from Linux, then the
biometric endpoint (EP 0x08 SBIO) + Mesa sensor for Touch ID. Background and
feasibility: see the analysis in the thread; this page is the working log.

Status: **access driver v0 builds and is committed (`a9/sep`); sepOS boot is
still unconfirmed, and the SBIO command table needs a live trace.**

## 1. What pongoOS really does with the SEP on A9

`third_party/pongoOS/src/drivers/sep/sep.c`

- `sep_auto()` (line ~1082) dispatches on `socnum`:
  - `0x8010/0x8011/0x8012` (A10/A10X/T2) → `seprom_fwload_race()`, i.e. blackbird.
  - `default`, which includes A9 → `"No need to pwn SEP, just booting..."`,
    then `tz_lockdown()`, `seprom_boot_tz0()`, and a `bootm` hook that runs
    `seprom_fwload()`.
- So the shipped pongoOS **does not pwn the A9 SEP**; it boots the stock sepOS
  without a pwn. The pwn race is hardcoded to A10's AOP SRAM `0x210E00000` and
  BPR `0x2102d0030`, so it cannot work on A9 as written.
- The unpwned path is gated twice:
  1. `gXNUExpectsBooted` = DT property `/arm-io/sep:sepfw-booted`. Our ADT
     (`docs/adt/j71t.txt`) has `sepfw-load-at-boot` and `sepfw-loaded`, **not**
     `sepfw-booted`. With that false, `sep auto` only locks TZ0 and returns.
  2. `gSEPFW` = `/chosen/memory-map:SEPFW`, staged by iBoot. The current
     palera1n -> pongoOS chain is iBoot-assisted and **does** provide this
     entry; the logs show a 4 KiB-aligned `0x344000`-byte staged image.

Result on our current chain: sepOS boot is still unconfirmed. The old tests
did reach the `0x6a` cold-command completion but then switched to the wrong
resume protocol, so they cannot establish that SEP remained in ROM.

## 2. Patch: `patches/sep/pongoos/pongoos-a9-sep.patch`

Adds diagnostic/boot shell commands to pongoOS (built clean, artifact
`build/prebuilt/pongo/Pongo-a9sep.bin`):

| command | what it does |
|---|---|
| `sep adopt` | use the file last uploaded with `/send` as `gSEPFW` (checks page alignment; the USB loader buffer is `alloc_contig`) |
| `sep xnu [status]` | verified cold path: status → boot_tz0 → optional ART → op 6 + endpoint-`0xfe` root buffer; never sends resume op 8 |
| `sep sboot` | legacy alias for `sep xnu` (kept for existing scripts) |
| `sep info` / `sep stat` | staged-image/TZ0 inspection and status/ring counters; `stat` never reads the RX FIFO data window |
| `sep msg <ep> <tag> <op> <param> <data>` | legacy paired-32-bit raw mailbox message; every SEP reply is logged (`sep_log_msgs`) |
| `sep msg64 <ep> <tag> <op> <param> <data>` | A9-only probe: require an empty AP→SEP FIFO, issue one 64-bit store to `+0x4010`, then `dsb sy` |

The A9 `sep xnu` boot handshake now uses the single-64-bit path by default,
matching `AppleA7IOPV2::postMailbox`. Add `split32` to retain the previously
successful paired-32-bit handshake as a controlled baseline.

On A9, `+0x4020` bits 18/19 are sticky outbox overflow/underflow faults. A
FIFO-data read while bit 17 says empty latches underflow. The old `sep stat`
did exactly that after printing its status, so the following invocation first
showed `recv_sts=0xa8801`. The command now prints only status and the software
ring; it never peeks `+0x4034/+0x4038`.

Mailbox on A9: `sep@0x20DA00000`, registers at **`0x20DA04000`** (32-bit
layout), SPIs **98/99/100/101**, `compatible = "iop,s8000"`. This matches the
MOSEC 2020 A9 numbers (`0x2_0DA04000`).

## 3. Firmware identity: use iBoot's staged SEPFW

The IPSW only ships a **raw IM4P**:

```
build/sep/20H350__iPad6,11/Firmware/all_flash/sep-firmware.j71t.RELEASE.im4p
  magic = DER, type "sepi", 3,408,126 bytes, no IM4M inside
```

`sep-firmware.*.im4p.plist` only carries `rsep`/`sepi` digests, not a manifest.
SEPROM validates an IMG4 (IM4P + signed IM4M), and the manifest for SEP is
personalized at restore time and written to the device's boot storage.

The APFS scan found an IMG4-wrapped `sepi` object:

```
/dev/nvme0n1p1 offset 0x00e0ce0000, 3,414,298 bytes, IM4P type "sepi"
```

So `scripts/sep/get-sep-img4.sh` re-scans and pulls it (root ssh over NCM).
Static inspection corrected an important assumption: the pulled
`build/sep/sep.j71t.img4` contains `IMG4` + `IM4P` but **no `IM4M`**. Its
`0x342000` file length can still occupy a `0x344000` 16-KiB-aligned staging
allocation, so the in-memory length alone does not prove what iBoot staged.
`sep info` now dumps `+0x34010c`: a DER header followed by `IM4M` proves the
manifest-bearing candidate, while payload-looking bytes prove the plain
object. Until that is captured, the upload/adopt path is not interchangeable
with `/chosen/memory-map:SEPFW`; the next `--xnu` test uses staged memory.

Other IMG4s in the container are `aopf` and `trst`, plus the restore set
(`illb`, `ibot`, `bat0`, `batF`, `recm`) in the 8 MiB `nvme0n2` namespace.
There is no SEP GPT partition on this device any more (the repartition for
Omarchy removed the iOS boot partitions), which is why a file-level search was
needed.

`scripts/sep/get-sep-img4.sh` uses root ssh over the NCM link
(`172.16.42.1`). One-time setup from the serial console:

```
mkdir -p /root/.ssh && cat >> /root/.ssh/authorized_keys <<< "$(cat ~/.ssh/id_ed25519.pub)"
chmod 700 /root/.ssh && chmod 600 /root/.ssh/authorized_keys
```



## 4. Current blocker

The 20:02 test reached status 2 and got the `0x6a` acknowledgment for the
staged firmware, but the test then incorrectly sent `op 0x08` and waited for
`0x6c`. Kernelcache control flow proves that `op 0x08` is the firmware-NULL
resume-from-RAM branch. A cold boot sends `op 0x06`, optionally submits the
root shared-memory descriptor on endpoint `0xfe`, and waits for command
completion; it never sends `op 0x08`. PongoOS also watches for
`ep=0 op=0x0d`, but that liveness notification is not yet proven on A9.

The corrected pongoOS binary is `build/prebuilt/pongo/Pongo-a9sep.bin`.

## 5. Next steps

1. Confirm the corrected cold path. The last manual pongoOS session came up
   **iBoot-assisted** (`Booted by: iBoot-8422...`), with TZ0 already locked
   (`tz0-size=0xCF8000`) and `/chosen/memory-map:SEPFW` pointing at an
   aligned staging allocation (`pa 0x804118000`, len `0x344000`). Run exactly
   `scripts/sep/pongo-sep.sh --xnu status`; it prints the staged-image
   discriminators before sending anything and includes a one-page, null-only
   endpoint-`0xfe` root buffer. Score the exact status payloads, `0x6a`, any
   non-`0xff` endpoint traffic, fatal `0xc7`, and panic. Do not send `op 0x08`
   or reuse the same SEPROM state for a second sequence. Do not make ART,
   TMM, or `noshmem` the next variable.
2. Capture SBIO on the jailbroken iPad with `scripts/sep/trace-sbio.sh`:
   the kext logs every command itself (`AppleSEPGenericTransfer::...
   command:0x%x`), so one unlock + enroll + delete gives the command table
   (0x73 is only the init) and the buffer lengths. No Frida needed.
3. Finish the driver transfer layer (kick opcode + completion) and add the
   SBIO client on top.
4. Then the Mesa sensor (`spi2@A088000`) and the keybag (`sks`), which is
   where Touch ID actually lands.

## 6. Safety

- SEP panic from a bad image is per-boot; a reset recovers it.
- Never write syscfg / NVRAM / boot-firmware storage. Reading boot partitions is
  fine.
- sepOS booting means the AP loses access to TZ0 (`tz_lockdown`), so this can
  only be started from pongoOS/m1n1 before Linux — not from Linux later.

## 7. Linux driver (workstream `sep`)

Source of truth: `drivers/sep/sep-a9.c`; built into the kernel workstream by
`scripts/sep/build-driver.sh` (copies it to `drivers/soc/apple/sep-a9.c`,
adds `CONFIG_APPLE_SEP_A9`, builds the object). The exported series is under
`patches/sep/`; patch 0006 adds the reconstructed GenericTransfer framing.

What it does today:

- maps `0x20DA00000`, mailbox at `+0x4000`, enables the `0x1000` interrupt bit
  the way pongoOS does;
- **touches no SEP registers at probe**; the mailbox is armed by writing 1 to
  `/sys/kernel/debug/sep-a9/start`, then polls every 10 ms and logs 64-bit
  messages as `ep/tag/op/param/data`;
- debugfs `/sys/kernel/debug/sep-a9/`: `status`, `log`, `endpoints`,
  `advertise`, `ool`, `sbio`, `xfer`, `send` (patch 0002);
- passive discovery: a 256-entry endpoint table built from the `0xfd`
  op 0 (4CC) / op 1 (OOL page ranges) pairs, or declared by hand from a
  pongoOS capture through `advertise`; the driver never guesses an endpoint;
- `ool EP SEND RECV` allocates send/receive buffers inside the advertised
  ranges and publishes them through `cntl` (`0x04`/`0x02` send,
  `0x05`/`0x03` recv), waiting for TX room before each message;
- single-chunk `0xfc` GenericTransfer v1 transfers matched on
  endpoint+tag+command: seven little-endian `u32` header fields occupy OOL
  `+0x00..+0x1b`, payload starts at `+0x1c`, and mailbox `data` is the chunk
  sequence (zero for the first chunk); tag `0xff` reports status at
  receive OOL `+0x10`;
- `/dev/sep-bio` speaking the Aurora Silicon UAPI (v4) so their libfprint
  driver, fprintd and Omarchy's fingerprint UI can be reused unchanged. See
  `docs/sep/userspace.md`; it sends nothing (enrol/verify return `-ENOSYS`)
  and `GET_INFO` reports `sensor_present = 0` until a debugfs `0x73` on the
  selected sbio endpoint succeeds.

Not in yet: interrupt delivery, the `0xfc`/`0xfd`/`0xfe` multi-chunk state
machine, unsolicited SBIO events, and the real enrol/match payloads. The
tri53-sep DT has the SEP node (patch 0001) and reserves the root shmem page
(patch 0003).

Safety: the driver is built in but inert unless the kernel command line has
`sep_a9.enable=1`, because reading a powered-down SEP block can stall the AP.

Test: entry `tri53-sep-*` (`scripts/sep/package-tri53-sep.sh`), warm kexec,
`scripts/sep/sep-dump.sh` (preconditions, no MMIO), and
`scripts/sep/sep-dump.sh --arm` once sepOS is known to be running. Exact steps:
`docs/sep/linux-live-test.md`.

### Bring-up sequence on Linux

Current interface (branch `a9/tri53-sep`, patches 0001-0003); the full,
orchestrator-ready sequence is `docs/sep/linux-live-test.md`:

```
D=/sys/kernel/debug/sep-a9
echo 1 > $D/start                  # arm: drain + record only, nothing sent
cat $D/status $D/endpoints $D/log  # 0xfd op0/op1 pairs -> endpoint table
echo "EP" > $D/sbio                # only if EP's complete advertisement is 'sbio'
echo "EP SEND RECV" > $D/ool       # hex sizes inside the advertised ranges
echo "EP 73 01000000" > $D/xfer    # SBIO init (cmd 0x73, u32 1), empty reply
echo "EP 2b f5010000" > $D/xfer    # GetProtectedConfig for user 501, 32-byte reply
cat $D/xfer
```

`EP` must come from a complete `sbio` advertisement seen in the same run (or
declared through `advertise` from a pongoOS capture of both data words); the
driver never guesses it. Publishing OOL buffers is once per sepOS boot: no
warm kexec afterwards.
