# Read-only SBIO GetProtectedConfig probe

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. This is the single next SEP hardware experiment after the
successful framed SBIO initialization in
`docs/sep/logs/tri53-sep7-sbio-20261026-2306/`.

## Result that unlocks this rung

The 23:06 fresh-DFU run reproduced the eight-endpoint gate, published SBIO's
16-KiB OOL pair, and sent GenericTransfer opcode `0x73` with a little-endian
`u32 1`. SEP returned a valid empty version-1 reply with status zero and Linux
set `sbio_ready=1`:

```text
tx=15 rx=22 tx_busy=0 dropped=0
gt_reply version=1 total=0 offset=0 flags=0x0 reserved=0x0
         command=0x73 chunk=0 mailbox_data=0x0
RESULT sbio-init=pass
```

This establishes that the earlier `0x101` was rejection of the unframed
request. It does not establish a sensor, keybag, or SKS prerequisite.

## Scope and evidence

The next request is the captured, read-only `GetProtectedConfig` transaction:

```text
endpoint  0x08 (sbio)
command   0x2b
input     f5 01 00 00        (little-endian user ID 501)
reply     exactly 32 bytes
```

`AppleMesaSEPDriver` forwards exactly four caller bytes. The userspace caller
fills those bytes with the biometric user's ID, and the captured iOS session's
only user is 501. The nearby trace text `value:0` is the BM command-header
field, not this payload. The 32-byte reply is two four-field configurations:
four little-endian `u32` values for the configured unlock, identification,
login, and Apple Pay flags, followed by the corresponding four effective
flags. The device-side probe validates framing and size but records the values
without making policy claims. Full evidence is in
`docs/sep/workstreams/protected-config-re.md`.

This probe neither powers Mesa nor reads/writes the sensor, SKS, syscfg,
NVRAM, boot firmware, APFS, or any persistent biometric store.

No new kernel is required. Reuse the already validated entry
`tri53-sep7-56c3395908`; the existing single-chunk GenericTransfer path handles
the 4-byte request and 32-byte reply.

## Exact fresh-DFU procedure

### 1. Preflight, fresh capture, and generated scripts

```sh
PKG=build/sep/tri53-sep7
P=build/boot/m1n1-linux-tri53-sep7-56c3395908.bin
RUN="$PKG/sbio-config-$(date +%Y%m%d-%H%M)"
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
test -f "$RUN/remote-sbio-config.sh"
```

Require planner `GO`, capture `16/16/0`, root shmem `0x87adb0000`, and only
the four initial `cntl/arts/xars/xarm` advertisements. Stop before another
write if any value differs.

### 2. Wake and boot the unchanged tri53-sep7 payload

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

Require the sep7 entry, reserved root-shmem page, retained/on SEP power domain,
both 64-bit mailbox audit lines, healthy NVMe, and both FIFOs empty.

### 3. Reproduce the arts+xarm gate

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

Continue only at exactly `tx=10 rx=17`, eight complete endpoints,
`published=2`, `sbio_ep=-1 sbio_ready=0`, and empty FIFOs.

### 4. Initialize SBIO and send one GetProtectedConfig

Do not run `remote-sbio-init.sh` first. The new script contains the proven
init sequence and then the single `0x2b` request:

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-sbio-config.sh" | tee "$RUN/5-sbio-config.txt"

scripts/sep/sep-dump.sh | tee "$RUN/6-final.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | \
  grep -E 'sep-a9|sep-shmem|PMGR sep' >"$RUN/dmesg-sep.txt"
```

The script enforces these intermediate gates:

```text
after SBIO OOL:  tx=14 rx=21, published=3, sbio_ready=0
after init 0x73: tx=15 rx=22, published=3, sbio_ready=1
```

A full `0x2b` pass is:

```text
out_len=32
<exactly 64 lowercase hex digits>
ep=0x08 cmd=0x2b ret=0 sep_status=0x0
gt_reply version=1 total=32 offset=0 flags=0x0 reserved=0x0
         command=0x2b chunk=32 mailbox_data=0x0
tx=16 rx=23 tx_busy=0 dropped=0
endpoints_complete=8 published=3
sbio_ep=8 sbio_ready=1
RESULT sbio-protected-config=pass
```

The script prints the payload once as `protected_config_hex=...` for the log.
Decode it offline as eight little-endian `u32` values in this order:

```text
set:       unlock, identification, login, Apple Pay
effective: unlock, identification, login, Apple Pay
```

`0xffffffff` is treated by Apple's AP-side code as "unset" and omitted from
the returned dictionary; other values are coerced to booleans. Do not infer
why a set and effective value differ during the run, and do not send `0x2b`
twice.

## Stop rules

- Any preflight, counter, endpoint, FIFO, header, size, status, or final-word
  mismatch: stop immediately and preserve the logs.
- Timeout or SEP status other than zero: do not retry in the same boot.
- A 32-byte reply with unexpected content is still evidence; record it without
  changing inputs.
- After either pass or failure: no more SBIO commands in that boot. Cold-reset
  before another SEP experiment because the published OOL pages are not handed
  safely across an arbitrary warm kexec.

