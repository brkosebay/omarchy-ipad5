# Combined `arts` MANIFEST + xarm capability probe

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


## Hardware result (20:21): pass

The fresh-DFU run in
`docs/sep/logs/tri53-sep6-combined-20261026-2021/` passed the AND-gate
hypothesis. The MANIFEST completed successfully, the captured-tag xarm
capability-0 reply drained, and sepOS advertised four additional services:

| service | endpoint | OOL pages (in/out) |
|---|---:|---:|
| `scrd` | `0x0a` | 4/4 |
| `sbio` | `0x08` | 4/4 |
| `sks ` | `0x12` | 4-8/4-8 |
| `sse ` | `0x0c` | 4/4 |

The stable boundary was `tx=10 rx=17`, eight complete endpoints, two
published endpoints (xarm and arts), and both FIFOs empty. This resolves the
previously unknown SBIO endpoint as `0x08` for this firmware. The next bounded
probe is documented in `docs/sep/touchid-next.md`; it publishes only SBIO OOL
and sends only the read-only initialization opcode `0x73`.

Date: 2026-10-26. This is the next fresh-DFU experiment after the successful
tri53-sep6 MANIFEST run in
`docs/sep/logs/tri53-sep6-20261026-2010/`.

## Result that motivates this probe

The isolated tests established both halves independently:

- tri53-sep5 published xarm OOL and sent the captured tag-2 capability-0
  reply. All five AP-to-SEP words drained, but discovery stayed at four
  endpoints.
- tri53-sep6 published arts OOL and sent the personalized MANIFEST. SEP
  returned `0000000000070102` after 11,681 microseconds, but discovery again
  stayed at four endpoints.

The next bounded hypothesis is an AND gate: sepOS waits until the AP has both
accepted the boot manifest and answered its xarm capability query.

## What XNU establishes, and what remains inferred

Observed in the iOS 16.7.10 kernelcache and boot capture:

1. Raw discovery order is `cntl`, `xars`, `xarm`, then `arts`.
2. `AppleSEPXARTService_start` initializes xarm's 32 KiB send/receive OOL
   buffers when the xarm service starts. It does not eagerly publish xars OOL.
3. `AppleSEPARTStorageService_start` initializes arts' 16 KiB OOL pair and
   immediately invokes the first-connected handler.
4. That handler copies the personalized IM4M and synchronously sends MANIFEST.
5. The real j71t xarm opcode-`0x0e` answer advertises capability 1, but XNU
   defers it while the independent filesystem-backed xART/Gigalocker store is
   unready. `AppleSEPXART_initGigalocker` sends the pending word when that
   backing store becomes ready.

Static analysis does not prove the wall-clock ordering between Gigalocker
initialization and the arts MANIFEST completion. The controlled Linux order is
therefore:

```text
xarm OOL -> arts OOL -> successful arts MANIFEST -> wait 2 s -> xarm cap-0 reply
```

This preserves XNU's endpoint/OOL initialization order and places the xarm
reply after a known boot-manifest commit point. Capability 0 remains essential:
Linux does not yet implement persistent xART storage, so capability 1 would
invite unsupported fetch/save requests.

## Expected result

The strongest pass is new endpoint-`0xfd` name/OOL pairs. The live iOS registry
shows the complete service set:

```text
cntl xars xarm arts scrd sbio sks sse
```

Endpoint numbers for the four new services are not assumed. Success requires
their complete advertisements from this boot. In particular, a complete
`sbio` advertisement should make the Linux status change from `sbio_ep=-1` to
the advertised endpoint ID. This experiment stops there: it does not publish
SBIO OOL and does not send SBIO init.

No xarm completion is expected for the AP's tag-2 reply. Success is its mailbox
consumption followed by unsolicited discovery traffic.

## Artifact

No tri53-sep7 kernel is needed. tri53-sep6 already contains the one-shot arts
MANIFEST guard plus the proven 64-bit generic OOL/send paths.

| item | value |
|---|---|
| entry | `tri53-sep6-739ef4e3ca` |
| payload | `build/boot/m1n1-linux-tri53-sep6-739ef4e3ca.bin` |
| SHA-256 | `d2030c010549925bbce752f2d0aa68421ef0055e6f80128c831cc168d3568fcf` |
| kernel commit | `739ef4e3ca736c3853a7849ad1cbb45ee753a161` |
| IM4M SHA-256 | `fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54` |

The updated `scripts/sep/arts-manifest-plan.py` derives the xarm endpoint and
tag from the fresh capture and writes two additional guarded scripts:

- `remote-combined-prep.sh`: arm, declare, xarm OOL first, then arts OOL;
- `remote-combined-commit.sh`: require MANIFEST success, observe for two
  seconds, then send exactly one captured-tag capability-0 xarm reply.

## Exact fresh-DFU procedure

### 1. Host preflight

```sh
PKG=build/sep/tri53-sep6
P=build/boot/m1n1-linux-tri53-sep6-739ef4e3ca.bin
RUN="$PKG/combined-$(date +%Y%m%d-%H%M)"
mkdir -p "$RUN"

(cd "$PKG/tri53-sep6-entry" && shasum -a 256 -c SHA256SUMS)
test "$(shasum -a 256 "$P" | awk '{print $1}')" = \
  d2030c010549925bbce752f2d0aa68421ef0055e6f80128c831cc168d3568fcf
test "$(stat -f %z build/sep/full/im4m.der)" = 6157
test "$(shasum -a 256 build/sep/full/im4m.der | awk '{print $1}')" = \
  fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54

if test -f build/sep/pongo-sep.log; then
    wc -l <build/sep/pongo-sep.log
else
    echo 0
fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled
```

Stop before DFU on any checksum failure.

### 2. Fresh sepOS boot and exact capture plan

```sh
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp \
  -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/boot/autoboot.sh --state
scripts/sep/pongo-sep.sh --xnu status split32 | tee "$RUN/0-pongo-run.txt"

START=$(( $(cat "$RUN/pongo-lines-before") + 1 ))
tail -n "+$START" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/arts-manifest-plan.py "$RUN/pongo.log" \
  --out "$RUN" | tee "$RUN/plan.txt"
cat "$RUN/plan.env"
```

The planner must say `GO`, and `plan.env` must contain the endpoint/tag from
this boot. For the currently stable stream they are:

```text
ARTS_EP=02
XARM_EP=13
XARM_TAG=02
XARM_REQUEST_RAW=00000000000e0213
```

Do not proceed if the capture is not exactly 16/16/0, the root shmem differs
from `0x87adb0000`, the four advertisements differ, or the xarm request is not
one zero-length opcode-`0x0e` word.

### 3. Proven pongo 64-bit wake

```sh
scripts/sep/pongo-msg64-wake.sh "$RUN/pongo-msg64.txt" | \
  tee "$RUN/pongo-msg64-summary.txt"
grep -q 'RESULT pongo-msg64=drained' "$RUN/pongo-msg64-summary.txt"
grep -q 'ring ep=0 tag=0 op=f param=0 data=0' "$RUN/pongo-msg64.txt"
```

Any other result is stop/no-retry/cold-reset.

### 4. Boot the same live SEP into tri53-sep6

```sh
scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state
# Only if it still reports pongo after 15 seconds:
printf 'bootm\n' | build/bin/pongoterm
scripts/boot/autoboot.sh --state
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

Require entry `tri53-sep6-739ef4e3ca`, the no-map root-shmem reservation,
PMGR SEP retained/on, both 64-bit mailbox audit lines, empty FIFOs, and no
fault. Do not arm on a failed preflight.

### 5. Publish xarm OOL first, then arts OOL

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-combined-prep.sh" | tee "$RUN/2-combined-prep.txt"
```

Prep pass is exactly:

```text
tx=8 rx=8 tx_busy=0 dropped=0
endpoints_complete=4 published=2
```

Both FIFOs must be empty. The log must contain eight endpoint-0 opcode-1 OOL
acknowledgement words: four for xarm, then four for arts. Any failure after an
OOL publication requires a cold reset; never republish in that boot.

### 6. Stage the IM4M and run the two commit points once

```sh
scripts/omarchy/ssh.sh \
  'sudo -n tee /sys/kernel/debug/sep-a9/arts-manifest >/dev/null' \
  <build/sep/full/im4m.der

scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/sep-a9/arts-start' | \
  tee "$RUN/3-staged.txt"

scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <"$RUN/remote-combined-commit.sh" | tee "$RUN/4-combined-result.txt"

scripts/sep/sep-dump.sh | tee "$RUN/5-final.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | \
  grep -E 'sep-a9|sep-shmem|PMGR sep' >"$RUN/dmesg-sep.txt"
```

Before the xarm reply, the generated script requires:

```text
ran=1 result=2 ret=0 ep=2 manifest_len=6157
reply tag=01 op=07 param=00 data=00000000 raw=0000000000070102
tx=9 rx=9 tx_busy=0 dropped=0
endpoints_complete=4 published=2
```

Both hardware FIFOs must also be empty at that commit point. If MANIFEST
caused any unsolicited traffic, the exact RX-counter/endpoint/FIFO guards stop
the script before xarm is sent, preserving the unexpected result by itself.

It then sends exactly the capability-0 reply derived from this boot, currently:

```text
13 02 00 00 00000000
raw=0000000000000213
```

The script cannot be safely rerun: its `arts-start ran=0` and exact counter
guards reject a second execution before any duplicate xarm reply.

## Verdict and stop rules

- **Full combined pass:** xarm reply drains, new complete advertisements arrive,
  and one is named `sbio`. Record all names/IDs/page counts and stop. Do not
  publish SBIO or send `0x73` in this boot.
- **Discovery partial pass:** at least one new endpoint pair arrives but the
  expected `scrd/sbio/sks/sse` set is incomplete. Record raw messages and stop.
- **Combined gate incomplete:** xarm reply drains, both FIFOs become empty, but
  the table remains at four after ten seconds. Record as protocol-incomplete;
  do not resend either request.
- **Unexpected xarm request:** record its raw tag/op/length/flags/CRC and stop.
  Capability 0 should not invite storage traffic, so do not guess a response.
- **MANIFEST changes discovery before xarm:** the generated script deliberately
  stops without sending xarm. Preserve that result.
- **Nonzero MANIFEST parameter, mismatched raw reply, changed counters, busy
  FIFO, timeout, or transport failure:** stop immediately and cold-reset. Never
  retry in the same boot.

After any result, do not warm-kexec because both OOL address pairs belong to
that Linux instance. Collect `$RUN`, remove `build/boot/autoboot.disabled`, and
cold-reset.
