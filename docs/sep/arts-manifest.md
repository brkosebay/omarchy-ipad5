# A9 SEP `arts` MANIFEST gate

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. This is the next controlled test after the successful
tri53-sep5 transport run in
`docs/sep/logs/tri53-sep5-20261026-1730/`. It is an orchestrator procedure;
nothing here touches the device automatically.

> **Hardware result, 20:10:** tri53-sep6 received a successful MANIFEST reply
> `0000000000070102` after 11,681 microseconds. Both FIFOs were empty and the
> endpoint table remained at four through ten seconds. The isolated arts path
> is therefore proven, but arts alone is not the discovery gate. The next
> experiment is the guarded combined sequence in
> [`docs/sep/arts-xarm-combined.md`](arts-xarm-combined.md).

## Conclusion

The consumed xarm tag-2 reply was correct. It was not supposed to produce an
xarm completion: `AppleSEPXART_handleSEPDrivenMsg` rewrites SEP's opcode
`0x0e` request in place and sends a result word with the same tag. Capability
zero is represented by the all-zero payload already used in tri53-sep5.
`xars` is the AP-driven half of xART and is not a prerequisite for that reply.

The missing XNU action is the separate legacy `arts` endpoint. On j71t:

- the SEP node has `ap-manifest` and `sepfw-loaded`, but no `has-art`;
- `AppleSEPARTStorageService` publishes 16 KiB send and receive OOL buffers;
- its first-connected handler copies the personalized IM4M DER into the
  AP-to-SEP OOL buffer;
- it sends MANIFEST as endpoint `arts`, tag `1`, opcode `0x06`, parameter and
  data zero;
- SEP replies on the same endpoint/tag with opcode `0x07`; parameter zero is
  success;
- because j71t has no `has-art`, XNU does not follow with ART_LOAD or
  ART_NO_ART.

For the captured endpoint ID `0x02`, the request and successful empty-payload
reply are:

```text
AP -> SEP: ep=02 tag=01 op=06 param=00 data=00000000
           raw=0000000000060102
SEP -> AP: ep=02 tag=01 op=07 param=00 data=00000000
           raw=0000000000070102
```

The four endpoint-0 opcode-1 words observed after tri53-sep5's xarm OOL setup
were OOL descriptor acknowledgements. They do not indicate a rejected xarm
reply and require no AP response.

## Binary evidence

The relevant iOS 16.7.10 kernelcache routines are:

- `AppleSEPXART_handleSEPDrivenMsg` at `0xfffffff0062f5188`;
- `AppleSEPDiscovery_init` at `0xfffffff0062de7f0`;
- `AppleSEPARTStorage_handle_first_connected` at
  `0xfffffff0062fcf0c`;
- `AppleSEPARTStorage_msgSendInternal` at `0xfffffff0062fd474`;
- `AppleSEPARTStorage_find_firmware_manifest` at
  `0xfffffff0062fd214`;
- `AppleSEPARTStorageService_start` at `0xfffffff0062fe1d8`.

The discovery driver only consumes endpoint-`0xfd` name/OOL pairs and starts
the matching service; it does not send a discovery-complete acknowledgement.
The ART storage message encoder allocates its first tag-pool entry as tag 1
and shifts MANIFEST enum value 3 left once, producing wire opcode 6. Its reply
opcode is 7.

`build/firmware/private/IODeviceTree.txt` records `ap-manifest` on the live
SEP node and no `has-art`. The exact ticket extracted from the personalized
firmware is:

```text
build/sep/full/im4m.der
length: 6157 (0x180d) bytes
sha256: fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54
prefix: 30 82 18 09 16 04 49 4d 34 4d
```

The DER length is self-consistent (`0x1809 + 4 = 0x180d`). Its prefix and
tail match the IM4M embedded in the staged SEPFW observed by pongoOS. Do not
read the old SEPFW staging physical range from Linux: it is not reserved
across boot. Upload this retained host copy instead.

## tri53-sep6 safety envelope

Kernel branch `a9/tri53-sep6` adds two debugfs files:

- `arts-manifest`: binary, sequential write into the already-published arts
  send OOL buffer;
- `arts-start`: validates DER length and `IM4M`, then sends exactly one
  tag-1/opcode-6 request and waits up to 10 seconds for tag-1/opcode-7.

It cannot issue ART load, store, clear, or delete commands. The request is
one-shot even after a timeout. OOL publication and the request remain
explicit operations; neither happens at probe time.

Do not use the live tri53-sep5 boot for an active test. It has no bounded way
to populate an OOL buffer with the binary IM4M. A read-only
`scripts/sep/sep-dump.sh` is safe; after that, cold-reset for this test.

## Fresh-DFU procedure

Packaged artifact:

| item | value |
|---|---|
| entry | `tri53-sep6-739ef4e3ca` |
| payload | `build/boot/m1n1-linux-tri53-sep6-739ef4e3ca.bin` |
| payload SHA-256 | `d2030c010549925bbce752f2d0aa68421ef0055e6f80128c831cc168d3568fcf` |
| kernel branch | `a9/tri53-sep6` |
| kernel commit | `739ef4e3ca736c3853a7849ad1cbb45ee753a161` |
| base | `a9/tri53-sep5` at `29f1192ef2ed4bb69b56fe6dd93fd51fdafae50c` |
| config / DTBs | byte-identical to tri53-sep5 |

Packaging audited one 64-bit `str x20` to mailbox `+0x10`, followed by
`dsb sy`, and one 64-bit `ldr x0` from `+0x38`, followed by `dmb oshld`.

### 1. Host preflight and isolate this boot's pongo transcript

```sh
PKG=build/sep/tri53-sep6
ENTRY=$(jq -r .entry "$PKG/payload-manifest.json")
P=$(jq -r .payload "$PKG/payload-manifest.json")
RUN="$PKG/run-$(date +%Y%m%d-%H%M)"
mkdir -p "$RUN"

(cd "$PKG/tri53-sep6-entry" && shasum -a 256 -c SHA256SUMS)
shasum -a 256 "$P"
test "$(jq -r '.files[.payload].sha256' "$PKG/payload-manifest.json")" = \
     "$(shasum -a 256 "$P" | awk '{print $1}')"
if test -f build/sep/pongo-sep.log; then
    wc -l <build/sep/pongo-sep.log
else
    echo 0
fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled
```

All entry `SHA256SUMS` lines must say `OK`; stop before DFU on any mismatch.

### 2. Fresh DFU, sepOS boot, and discovery capture

```sh
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp \
    -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/boot/autoboot.sh --state
scripts/sep/pongo-sep.sh --xnu status split32 | tee "$RUN/0-pongo-run.txt"
START=$(( $(cat "$RUN/pongo-lines-before") + 1 ))
tail -n "+$START" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/arts-manifest-plan.py "$RUN/pongo.log" \
    --out "$RUN" | tee "$RUN/plan.txt"
```

Pass requires one fresh `SEPOS BOOTED`, root shmem `0x87adb0000`, an
unoverwritten 16-message capture, and exactly the complete `cntl`, `arts`,
`xars`, and `xarm` advertisements. `arts` must be endpoint `0x02` with
4/4/4/4 pages. The planner must print `GO` and the fixed IM4M hash above.

### 3. Wake sepOS with the already-proven 64-bit pongo transaction

```sh
scripts/sep/pongo-msg64-wake.sh "$RUN/pongo-msg64.txt" | \
    tee "$RUN/pongo-msg64-summary.txt"
grep -q 'RESULT pongo-msg64=drained' "$RUN/pongo-msg64-summary.txt"
grep -q 'ring ep=0 tag=0 op=f param=0 data=0' "$RUN/pongo-msg64.txt"
```

Pass is the clean 65-second hold, one raw `e0000` send, a drained AP-to-SEP
FIFO, and one endpoint-0 opcode-`0x0f` indication. On any other result, do not
retry; save the logs and cold-reset.

### 4. Boot the same live sepOS into tri53-sep6

```sh
scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state
# Only if the state still says pongo after 15 seconds:
printf 'bootm\n' | build/bin/pongoterm
scripts/boot/autoboot.sh --state
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

Pass requires Linux entry `$ENTRY`, the `sep-shmem@87adb0000` no-map
reservation, SEP powered on, the single-64-bit TX/RX probe lines, both FIFOs
empty, and no mailbox fault. Do not arm on a preflight failure.

### 5. Arm, declare, and publish only the arts OOL binding

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
    <"$RUN/remote-arts-prep.sh" | tee "$RUN/2-arts-prep.txt"
```

The generated script arms the driver, declares the exact captured endpoint
table, and publishes only `arts 02 4000 4000`. Each of its four OOL-control
words must drain. It does not send MANIFEST or answer xarm. Stop and
cold-reset if OOL publication fails; never publish it twice in one boot.

### 6. Upload the exact IM4M, then cross the one-shot commit point

```sh
test "$(stat -f %z build/sep/full/im4m.der)" = 6157
test "$(shasum -a 256 build/sep/full/im4m.der | awk '{print $1}')" = \
    fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54
scripts/omarchy/ssh.sh \
    'sudo -n tee /sys/kernel/debug/sep-a9/arts-manifest >/dev/null' \
    <build/sep/full/im4m.der
scripts/omarchy/ssh.sh 'sudo -n cat /sys/kernel/debug/sep-a9/arts-start' | \
    tee "$RUN/3-arts-staged.txt"
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
    <"$RUN/remote-arts-commit.sh" | tee "$RUN/4-arts-result.txt"
scripts/sep/sep-dump.sh | tee "$RUN/5-final.txt"
```

Before the commit, `arts-start` must say `ran=0` and `manifest_len=6157`.
The generated commit script then sends exactly one request and never retries.

## Verdicts

Protocol pass is:

```text
reply tag=01 op=07 param=00 data=00000000 raw=0000000000070102
```

- If that is followed by new endpoint-`0xfd` advertisements, record them and
  stop. The `arts` MANIFEST was the discovery gate.
- If the opcode-7 reply succeeds but the endpoint table remains at four,
  record it as `arts pass; combined gate still pending` and stop without a
  second write. The next controlled step is the already-proven xarm
  capability-0 reply in the same protocol state, but derive its endpoint and
  tag from this boot's capture rather than replaying constants blindly.
- If opcode 7 has nonzero parameter, preserve the raw word and stop; SEP
  rejected the personalized manifest.
- If the request drains but no matching opcode-7 reply arrives within 10
  seconds, preserve the FIFO/status/log output and cold-reset. Do not retry.
- If AP-to-SEP does not drain, this is a transport regression, not an arts
  protocol result.

Do not publish `xars`, do not resend the earlier xarm reply, do not send any
ART mutation opcode, and do not warm-kexec after OOL addresses have been
published. End the run with a cold reset after collecting `$RUN`.
