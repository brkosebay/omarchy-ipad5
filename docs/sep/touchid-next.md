# Touch ID bring-up after eight-endpoint discovery

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. The combined `arts` + xarm hardware run passed and exposed
all eight SEP services. This document separates the next transport proof from
the later sensor, keybag, and enrollment work.

## 10:59 update: protected configuration passed; Mesa is next

The fresh tri53-sep7 run in
`docs/sep/logs/tri53-sep7-protected-config-20261027-1059/` returned a valid
32-byte `GetProtectedConfig` reply for user 501. All eight little-endian words
were `0xffffffff`, which Apple's AP code treats as unset. Transport stayed
clean at `tx=16 rx=23`, eight complete endpoints and `sbio_ready=1`.

This closes the read-only SBIO policy rung. Another enrollment-state query
would only characterize the expected empty biometric store; it would not
unblock a fingerprint round trip. The critical path therefore moves to the
AP-side Mesa hardware boundary: a fresh cold boot, volatile 1V8/3V0 power
sequence, exactly one 23-byte SPI status poll, then power-off. It does not
boot sepOS or send any SBIO command. The exact procedure is in
`docs/sep/mesa-status-next.md`.

## 23:06 update: framed SBIO initialization passed

The fresh tri53-sep7 run in
`docs/sep/logs/tri53-sep7-sbio-20261026-2306/` returned a valid empty
GenericTransfer reply for opcode `0x73`, status zero, and set `sbio_ready=1`.
The earlier `0x101` is therefore explained by the missing transfer header; it
was not evidence for a sensor or SKS prerequisite. The next fresh-boot rung is
the single read-only `GetProtectedConfig` request in
`docs/sep/protected-config-next.md`. Do not append it to the completed 23:06
boot.

## 21:03 result and corrected diagnosis

The first SBIO attempt reached the service transport but was malformed. It
published SBIO OOL successfully, sent mailbox `ep=08 tag=fc op=73 data=4`, and
put only `01 00 00 00` at send OOL offset zero. SEP answered
`ep=08 tag=ff op=00 data=0`; receive OOL `+0x10` contained status `0x101`.

Kernelcache reconstruction now shows why. `AppleSEPGenericTransfer` requires a
28-byte seven-`u32` header before the payload, and the mailbox data is a chunk
sequence, not the payload length. Because the old OOL buffer had zeros at
`+0x14`, the error reply's opcode zero matches the malformed header's command
field. This is strong evidence that GenericTransfer rejected the envelope
before dispatching SBIO command `0x73`.

The private symbolic name of SEP status `0x101` is not present in the AP
kernelcache, and the encrypted sepOS image prevents assigning one honestly.
Its bounded meaning here is **GenericTransfer request/header rejection**; it is
not evidence for “sensor absent,” “SBIO not ready,” or an SKS prerequisite.
XNU's lazy SBIO path still calls `0x73` directly, with no prior `scrd`, `sks`,
or `sse` transaction.

## What is established

The successful run in
`docs/sep/logs/tri53-sep6-combined-20261026-2021/` advertised:

```text
cntl=0x00  arts=0x02  sbio=0x08  scrd=0x0a
sse =0x0c  xars=0x10  sks =0x12  xarm=0x13
```

SBIO is exactly 4/4 input pages and 4/4 output pages. The stable pre-SBIO
state is:

```text
tx=10 rx=17 tx_busy=0 dropped=0
endpoints_complete=8 published=2
sbio_ep=-1 sbio_ready=0
```

Kernelcache reconstruction at `0xfffffff0066bb114` shows
`IOSEPBiometricService::initSbioCommunication` sending opcode `0x73` with a
little-endian `u32 1`, length four, and no output. The ordinary SBIO wrappers
at `0xfffffff0066bb29c` and `0xfffffff0066bb474` lazily call it when the
initialized flag is clear. There is no preceding `scrd`, `sks`, or `sse`
transaction in that path.

That intentionally narrow test has now passed. It published SBIO's 16 KiB OOL
pair and sent one `0x73` transfer; nothing else was sent in that boot.

## Dependency map to a Touch ID round trip

### 1. SBIO transport

The corrected immediate probe proves the OOL control words, full GenericTransfer
v1 envelope, and zero-length completion on endpoint `0x08`. A later, separate
fresh boot may send `GetProtectedConfig` (`0x2b`, little-endian user ID 501,
32-byte output) as the first read-only data transfer. The trace's `value:0` is
a separate BM header field, not the four-byte request payload.

`scrd` appears to be a separate Secure Credential transport and `sse` is a
separate secure-element service in the iOS registry; neither is a prerequisite
for SBIO initialization. `sks` is also not required for `0x73`, but it does
become a dependency before real
enroll or match operations: the captured iOS sequence forwards the current
AppleSEPKeyStore user/keybag lock state into `IOSEPBiometricService`.

### 2. AP-side Mesa sensor transport

SEP does not directly drive the fingerprint sensor. Linux still needs a Mesa
front end for `mesa@0` under `spi2@A088000` (physical controller base
`0x20a088000`) which provides:

- the `mesa_pwr` and `mesa_pwr1v8` power-function sequence plus SPI2 clock and
  power-domain lifetime;
- the Mesa GPIO interrupt for finger/data-ready events;
- an 8 MHz SPI configuration preserving the iOS clock-phase setting (the
  exact Apple-to-Linux SPI mode mapping must be verified before enabling it);
- the observed 7-byte status poll and 11-byte capture command;
- receipt of the 19,668-byte opaque encrypted sensor frame.

The AP must not decode fingerprint pixels. The trace-verified per-frame common
prefix is `0x23`, `0x06`, the opaque frame as SBIO `0x65`, then `0x07`.
`0x65` is split into 16,356 and 3,312 byte chunks with the proven
`0xfc`/`0xfd` request and `0xfe` continue protocol. The `0x06` body is 151 or
167 bytes, and the result/ack tail varies among `0x08`, `0x09`, `0x1f`, and
`0x22`; Linux must recover the selection rule rather than hardcode one tail.

The existing Apple S8000 SPI controller support is the base for this work; the
missing piece is the Mesa sensor/relay driver and its power/IRQ integration.
The smallest safe hardware slice remains deliberately below capture: confirm
the PMU provider and read back candidate LDO registers `0x312`/`0x319`, add a
board-local SPI2/Mesa node with its unresolved IRQ omitted, then perform one
bounded power-on, one 23-byte status poll, and power-off. Do not add patch
upload, TestCMD, capture, `0x06`, or frame relay until those prerequisites and
their selection rules are recovered.

### 3. User state, matching, and enrollment

The shortest safe progression is:

1. prove `0x73` only;
2. prove read-only `0x2b` in a new boot;
3. implement sensor power/SPI/IRQ and one opaque frame relay;
4. implement SKS lock-state handling and the SBIO async state machine;
5. reconstruct the 68-byte `MatchMode` (`0x25`) payload and attempt a
   read-only match path;
6. only then reconstruct the 68-byte `EnrollMode` (`0x03`) payload and the
   catacomb save protocol.

Enrollment is not just an SEP command. `0x6c` exports the catacomb blob;
CompleteSaveCatacomb is host-only and returns that buffered reply, the AP
durably stores it, then `0x37` confirms the save. `0x33` is instead
GetCatacombId (4 bytes in, 16 out), followed by the `0x36` hash. Linux needs
atomic, access-controlled storage for the exported blob and a defined
keybag/user identity. The current xarm capability-0 reply means xART record
storage is unavailable, but xART and biometric catacomb storage are separate;
whether sepOS permits and restores a catacomb while xART remains capability 0
is still an open hardware question.

Until those layouts and the SKS state are recovered, do not send `0x03`,
`0x25`, catacomb save/delete operations, or any guessed payload. Never store
experimental state in iOS APFS, syscfg, NVRAM, or boot-firmware storage.

## Artifact for the next probe

A corrected kernel is required. Use tri53-sep7. It adds the single-chunk
GenericTransfer builder/parser on top of tri53-sep6 and cherry-picks the two
validated shared-MSI NVMe fixes from `a9/kexec-irqfix` (`7dcb18c411`,
`e3ad1b3a5c`). This avoids a userspace Identify stalling NVMe and invalidating
the timed SBIO run roughly 25 seconds after Linux starts.

| item | value |
|---|---|
| entry | `tri53-sep7-56c3395908` |
| payload | `build/boot/m1n1-linux-tri53-sep7-56c3395908.bin` |
| kernel commit | `56c339590811a04a7e3eb94e33f75546692e1265` |
| SHA-256 | `904bd177871b09aeee80c583e42c3a8bbea5c46377cbd6529c5eea1879435a13` |

`scripts/sep/arts-manifest-plan.py` now emits
`remote-sbio-init.sh`. It is deliberately non-rerunnable: it requires the
exact combined-run counters and unpublished SBIO state before making any
write.

## Exact fresh-DFU procedure

### 1. Preflight and fresh capture

```sh
PKG=build/sep/tri53-sep7
P=build/boot/m1n1-linux-tri53-sep7-56c3395908.bin
RUN="$PKG/sbio-init-$(date +%Y%m%d-%H%M)"
mkdir -p "$RUN"

(cd "$PKG/tri53-sep7-entry" && shasum -a 256 -c SHA256SUMS)
test "$(shasum -a 256 "$P" | awk '{print $1}')" = \
  904bd177871b09aeee80c583e42c3a8bbea5c46377cbd6529c5eea1879435a13
test "$(stat -f %z build/sep/full/im4m.der)" = 6157
test "$(shasum -a 256 build/sep/full/im4m.der | awk '{print $1}')" = \
  fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54

if test -f build/sep/pongo-sep.log; then
    wc -l <build/sep/pongo-sep.log
else
    echo 0
fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled

PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp \
  -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/sep/pongo-sep.sh --xnu status split32 | tee "$RUN/0-pongo-run.txt"

START=$(( $(cat "$RUN/pongo-lines-before") + 1 ))
tail -n "+$START" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/arts-manifest-plan.py "$RUN/pongo.log" \
  --out "$RUN" | tee "$RUN/plan.txt"
cat "$RUN/plan.env"
```

Require planner `GO`, capture `16/16/0`, root shmem `0x87adb0000`, and the
four initial endpoints. Stop before any further write if they differ.

### 2. Wake and boot tri53-sep7

```sh
scripts/sep/pongo-msg64-wake.sh "$RUN/pongo-msg64.txt" | \
  tee "$RUN/pongo-msg64-summary.txt"
grep -q 'RESULT pongo-msg64=drained' "$RUN/pongo-msg64-summary.txt"
grep -q 'ring ep=0 tag=0 op=f param=0 data=0' "$RUN/pongo-msg64.txt"

scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state
# Only if still in pongoOS after 15 seconds:
printf 'bootm\n' | build/bin/pongoterm
scripts/boot/autoboot.sh --state
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

Require the sep7 entry, reserved no-map page, retained/on SEP domain, both
64-bit mailbox audit lines, and empty FIFOs.

### 3. Reproduce the successful combined gate

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-combined-prep.sh" | tee "$RUN/2-combined-prep.txt"

scripts/omarchy/ssh.sh \
  'sudo -n tee /sys/kernel/debug/sep-a9/arts-manifest >/dev/null' \
  <build/sep/full/im4m.der

scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-combined-commit.sh" | tee "$RUN/3-combined-result.txt"

scripts/sep/sep-dump.sh | tee "$RUN/4-eight-endpoints.txt"
```

Do not continue until the stable result is exactly `tx=10 rx=17`, eight
complete endpoints, `published=2`, `sbio_ep=-1 sbio_ready=0`, and both FIFOs
empty. The endpoint table must match the eight IDs/page ranges above.

### 4. Run the one-shot SBIO hello

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-sbio-init.sh" | tee "$RUN/5-sbio-init.txt"

scripts/sep/sep-dump.sh | tee "$RUN/6-final.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | \
  grep -E 'sep-a9|sep-shmem|PMGR sep' >"$RUN/dmesg-sep.txt"
```

The generated script first publishes `08 4000 4000`. Before sending init it
requires:

```text
tx=14 rx=21 tx_busy=0 dropped=0
endpoints_complete=8 published=3
sbio_ep=8 sbio_ready=0
```

It then sends exactly one transfer:

```text
ep=0x08  tag=0xfc  op=0x73  param=0  data=0
send OOL (32 bytes):
  01000000 04000000 00000000 00000000
  00000000 73000000 04000000 01000000
```

A full pass is:

```text
out_len=0
ep=0x08 cmd=0x73 ret=0 sep_status=0x0
gt_reply version=1 total=0 offset=0 flags=0x0 reserved=0x0 command=0x73 chunk=0 mailbox_data=0x0
tx=15 rx=22 tx_busy=0 dropped=0
endpoints_complete=8 published=3
sbio_ep=8 sbio_ready=1
ep=08 tag=fc op=73 param=00 data=00000000
RESULT sbio-init=pass
```

Both FIFOs must be empty. Record the elapsed completion time from dmesg.

## Stop rules

- Any mismatch before SBIO publication: stop without publishing.
- Any OOL failure, missing/extra acknowledgement, counter change, unexpected
  endpoint traffic, FIFO residue, timeout, tag/op mismatch, nonzero return, or
  nonzero SEP status: stop immediately, collect state, and cold-reset.
- Never republish OOL or retry `0x73` in that boot.
- Even on a full pass, do not send `0x2b` or another SBIO command. Collect the
  run, remove `build/boot/autoboot.disabled`, and cold-reset because the OOL
  addresses belong to that Linux instance.

## Current live boot

The failed 21:03 boot has already yielded the useful read-only baselines:
`7-live-error.txt` and `dmesg-live.txt` in its committed log directory. No
further live dump is needed; release it without another SEP write. The
tri53-sep7 test must start from a fresh DFU because the failed boot already
bound SBIO's one-shot OOL addresses.
