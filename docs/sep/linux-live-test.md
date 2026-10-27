# SEP live test from Linux (tri53-sep)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26. Plan source: `docs/sep/linux-bringup-next.md`. Run by the
orchestrator only; every command below is written for the host, talking to
Omarchy through `scripts/omarchy/ssh.sh` with `sudo -n`.

> **Current procedure:** the combined tri53-sep6 run exposed
> `sbio=0x08` and the other delayed endpoints. Use the guarded fresh-DFU
> `0x73`-only procedure in `docs/sep/touchid-next.md`. The chronological
> sections below are retained as the transport-development record.

**Next run: section 9** (tri53-sep4: every AP->SEP message as one 64-bit
store, pongoOS `sep msg64` probe, then direct bootm and a 64-bit Linux
`wake-test`). Section 8 (tri53-sep3) was overtaken by the 16:18 pongoOS
split, section 7 is the completed tri53-sep2 run, and sections 1 to 6
describe tri53-sep and the 12:09/13:27 sepOS sessions.

## 0. What is being tested

sepOS was booted by pongoOS at 12:09 (`docs/sep/logs/20261026-1209-xnu-status-sepos-booted.log`)
and Linux has only been warm-kexec'd since. This run boots a kernel with the
SEP driver enabled, arms the mailbox, collects whatever endpoint
advertisements are still queued, and (optionally, last) proves the first SBIO
transaction.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

### Driver behaviour in this build

- Inert without `sep_a9.enable=1`; with it, probe only maps the block and
  prints `mailbox at ... unarmed`. No SEP register is touched until
  `start` is written.
- Arming (`echo 1 > start`) only drains and records. The driver **never
  sends anything by itself**: no discovery request, no endpoint guess, no
  probe. Every message comes from an explicit write to `ool`, `xfer` or
  `send`.
- `/dev/sep-bio` sends nothing (enrol/verify return `-ENOSYS`).

debugfs `/sys/kernel/debug/sep-a9/` (numbers are hex, with or without `0x`):

| file | use |
|---|---|
| `status` | counters, `recv_sts`/`snd_sts` (only when armed), `rx_idle_ms`, endpoint counts, sbio selection |
| `start` | `1` arms (drain + record only), `0` stops polling |
| `log` | every received message; `0xfd` lines get `[ep 0xNN 'name']` or `[ep 0xNN OOL in a-b out c-d pages]` |
| `endpoints` | table built from the `0xfd` pairs: id, name, source (mailbox/manual/mixed), page ranges, state, `max_send`/`max_recv` in bytes |
| `advertise` | declare an advertisement by hand from a pongoOS capture (see 3b); fills the table, sends nothing |
| `ool` | `EP SEND RECV`: allocate and publish both OOL buffers through `cntl` (4 messages) |
| `sbio` | `EP`: select the sbio endpoint; refused unless its complete advertisement says `sbio` |
| `xfer` | `EP CMD [HEXBYTES]`: one single-chunk command; read back `out_len`, payload hex, `ret`, `sep_status` |
| `send` | raw `ep tag op param data`; not used in this plan |

## 1. Warm kexec into the entry

First note which entry is running and that its pristine dtb is staged (the
merge base):

```sh
scripts/omarchy/ssh.sh 'tr " " "\n" </proc/cmdline | grep linux_a9.entry=; ls -l /boot/linux-a9/entries/"$(tr " " "\n" </proc/cmdline | sed -n "s/^linux_a9.entry=//p")"/dtb'
```

(`/boot/linux-a9/entries/<entry>/dtb` is `linux-a9-kexec`'s merge base; if it
is missing, the load refuses.) Then:

```sh
ENTRY=tri53-sep-759c7ffcd4 scripts/boot/kexec-host.sh build/boot/m1n1-linux-tri53-sep-759c7ffcd4.bin
```

> Private graphics-dependent excerpt omitted from this source snapshot.

The shmem-page argument in section 6 holds for any of those running
kernels: they all carry `crashkernel=256M`, and every iomem capture puts that
region at the top of RAM, over `0x87adb0000`.

## 2. Preconditions (no SEP MMIO)

```sh
scripts/sep/sep-dump.sh
```

Without `--arm` this reads only kernel state. Required before going on:

1. `== reservation` shows
   `OF: reserved mem: 0x000000087adb0000..0x000000087adb3fff (16 KiB) nomap non-reusable sep-shmem@87adb0000`.
   If it says `MISSING`, the merged FDT lost the node or the reservation
   failed: **stop, do not arm.**
2. `== power domain` shows the `sep` domain `on` (it is `apple,always-on`, so
   anything else is unexpected: stop).
3. `== probe` shows `sep-a9 20da00000.sep: mailbox at ... unarmed`.
4. `== status` shows `running=0 (mailbox untouched)`.

Also worth one line in the report: `dmesg | grep -i crashkernel`. The crash
region moves below the hole (it used to be `0x86b800000-0x87b7fffff`, which
is the only reason the shmem page survived so far).

## 3. Arm and collect (passive)

```sh
scripts/sep/sep-dump.sh --arm      # arms, waits 1 s, dumps
sleep 5
scripts/sep/sep-dump.sh            # dump again: has anything new arrived?
```

Discovery is finished for this run when `status` shows `recv_sts=... (empty)`,
`rx_idle_ms` is at least 100, and the second dump has no new log lines. Keep
both dumps verbatim.

### 3a. Reading the result

`endpoints` lists only paired data. A service is usable only in state
`complete` (both the op 0 name and the op 1 OOL message were seen for the
same ID). Find the line whose name is exactly `sbio` and copy its ID from
**this** run. Endpoint 0x08 has not been established for j71t, and there is
no other source for the number.

Expected on iOS (live registry): `cntl xars xarm arts scrd sbio sks sse`.
Already known from 12:09: `0x00 cntl` (no OOL), `0x10 xars` (8-8 / 8-8
pages), `0x13 xarm` (name only).

| what `endpoints`/`log` show | meaning | next |
|---|---|---|
| a `complete` line for `sbio` from the mailbox | SBIO is usable | optionally go to section 4 |
| `sbio` present but `incomplete` | half of the pair was consumed earlier | complete it with 3b only if a pongoOS log has the missing data word; otherwise stop |
| no `ep=fd` lines at all, or no `sbio` | **no discovery messages inherited**: pongoOS consumed them and nothing replays them | **stop.** Report, then plan a cold boot with a full pongoOS capture (below) |
| any line marked `CONFLICT` | two advertisements for one ID disagree | stop and report the log |
| other `ep=` traffic (e.g. `ep=00 op=0d`) | unsolicited SEP messages | record; it is not a reply to anything we sent |

There is deliberately **no** message to `cntl` or any endpoint in this phase.
Do not send anything to "ask" for the table: no such command exists.

**No discovery inherited → cold boot needed.** The next sepOS boot must keep
the whole discovery stream: enlarge pongoOS's retention ring to at least 256
entries and dump it only after discovery has been quiet (keep draining the
hardware FIFO while recording), or have pongoOS hand its parsed table to
Linux in reserved memory. The capture must contain, for `sbio`, both
`ep=fd op=0 param=ID data=7362696f` and `ep=fd op=1 param=ID data=XXXXXXXX`.

### 3b. Declaring an advertisement from a pongoOS capture

Only with both data words for the endpoint from a pongoOS log, never from
memory or guesswork. Two equivalent forms (hex):

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' <<'EOF'
D=/sys/kernel/debug/sep-a9
# raw: the two data words exactly as pongoOS printed them for param=ID
#   ep=fd op=0 param=ID data=NAMEWORD   /   ep=fd op=1 param=ID data=OOLWORD
echo "raw ID NAMEWORD OOLWORD" > $D/advertise
# fields: OOLWORD bytes from least significant: in_min in_max out_min out_max
#   e.g. data=8080808 (xars) -> "10 xars 8 8 8 8"
cat $D/endpoints
EOF
```

A write that disagrees with what the mailbox already delivered is refused
(`File exists`). The 12:09 log has no `sbio` pair, so it can't be used for
SBIO.

## 4. Optional, last: SBIO OOL buffers and first SBIO transactions

> **This step changes sepOS state that survives warm kexec.** After the `ool`
> write, sepOS holds the physical addresses of two Linux pages for good, and
> the binding can't be replaced. A later warm kexec would hand those pages to
> a new kernel that reuses them, and any late SBIO reply would be written into
> them. Do this only as the **last step of the session**: afterwards, no warm
> kexec. Finish with a cold reset (DFU, `scripts/boot.sh`), which also resets
> sepOS. Reserving the shmem page does not cover OOL pages.

Precondition: section 3 ended with a `complete` `sbio` line. Set the three
values from that line (`max_send`/`max_recv` are the advertised maxima, the
size to use for the first test):

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' <<'EOF'
D=/sys/kernel/debug/sep-a9
SBIO_EP=0xNN          # from the complete 'sbio' line of THIS run
SBIO_SEND=0xNNNN      # its max_send
SBIO_RECV=0xNNNN      # its max_recv

echo "$SBIO_EP" > $D/sbio || exit 1
echo "$SBIO_EP $SBIO_SEND $SBIO_RECV" > $D/ool || exit 1
dmesg | grep 'sep-a9.*OOL send' | tail -1      # record: PAs of the two pages
cat $D/endpoints

# A. initialize SBIO: command 0x73, u32 1 (little-endian), empty reply
echo "$SBIO_EP 73 01000000" > $D/xfer
cat $D/xfer

# B. GetProtectedConfig: command 0x2b, user ID 501 little-endian, 32-byte reply
echo "$SBIO_EP 2b f5010000" > $D/xfer
cat $D/xfer

cat $D/status
cat $D/log | tail -20
EOF
```

Do **not** send `0x09` (it is a state-dependent result fetch during an active
match, not "display status"), and nothing else beyond `0x73` and `0x2b`.

`ool` sends exactly these four messages, in this order:

```text
ep=0 tag=0 op=0x04 param=EP data=SBIO_SEND
ep=0 tag=0 op=0x02 param=EP data=send_pa >> 12
ep=0 tag=0 op=0x05 param=EP data=SBIO_RECV
ep=0 tag=0 op=0x03 param=EP data=recv_pa >> 12
```

### What success looks like

- `ool`: the write succeeds, dmesg has `ep 0xNN: OOL send 0x8... len ... recv 0x8... len ...`
  without `(publish incomplete)`, and `endpoints` shows `published`.
- A (`0x73`): `cat xfer` gives `out_len=0`, an empty hex line and
  `ret=0 sep_status=0x0`; `log` has `ep=NN tag=fc op=73 param=00 data=00000000`;
  dmesg has `sbio ep 0xNN: init (0x73) acknowledged`. This is the first
  transport/liveness proof from Linux.
- B (`0x2b`): `out_len=32` and exactly 64 hex digits, `ret=0`. This proves
  OOL in both directions and the cache handling.

### What failure looks like

`echo ... > xfer` prints the error; `cat xfer` keeps `ret` and `sep_status`.

| error from the write | meaning |
|---|---|
| `Connection timed out` | no matching `tag=fc` reply within 10 s; check `log` for any `ep=NN` line |
| `Input/output error` | SEP answered `tag=ff`; `sep_status` is the u32 at receive OOL offset 0x10 |
| `Protocol error` | a received frame was rejected: bad version/flags, impossible size, wrong tag or sequence progression, a `0xfe` where no credit is due, or a reply before the request finished. The engine quarantines the mailbox (100 ms after the last stale GT word) and `xfer` shows `gt_proto`/`gt_counters`. A multi-chunk `fd` reply is handled, not an error |
| `Message too long` | reply longer than one chunk / the receive buffer |
| `Device or resource busy` | TX FIFO stayed full 100 ms (`tx_busy` in status), or `ool` already published |
| `No such file or directory` | `sbio`/`ool`: no complete advertisement (or not named `sbio`); `xfer`: n/a |
| `Numerical result out of range` | `ool` size not a 4 KiB multiple inside the advertised range |
| `No such device or address` | mailbox not armed |
| `No such device` (xfer) | no published OOL buffers for that endpoint |

If `ool` returns an error after it has started sending, the buffers stay
allocated and marked published (the SEP may already hold part of the
binding): stop and cold-reset. Don't retry `ool`. The driver is conservative:
it marks the endpoint published even when the very first control message
never left (TX full), so that case can't be retried in the same boot either.
Range and advertisement errors (`ERANGE`, `ENOENT`) are refused before
anything is allocated or sent, so those can be retried.

## 5. Report back

- both `sep-dump.sh` outputs from section 3 (and section 2's);
- `dmesg | grep -E 'sep-a9|sep-shmem|crashkernel'`;
- if section 4 ran: the `OOL send` dmesg line and both `cat xfer` outputs;
- whether the session ended with a cold reset.

## 6. Memory-safety notes

- **Root shared-memory page, PA `0x87adb0000`: reserved no-map (16 KiB).**
  pongoOS handed it to sepOS on endpoint `0xfe` with op 6. XNU keeps its
  equivalent buffer for the system's lifetime, so sepOS may use it at any
  time. It lies in the System RAM m1n1 gives Linux
  (`0x800000000-0x87b7fffff`) and was never reserved. It survived today only
  because `crashkernel=256M` lands on `0x86b800000-0x87b7fffff`, and the
  phoenix crash payload (bottom-up from `0x86b800000`, about 20 MB) doesn't
  reach 245 MB into it. Both kexec paths place the new kernel low:
  `kexec_file_load` and kexec-tools' `locate_hole(..., 1)` are both
  first-fit lowest, and the earlier warm boots show Kernel code at
  `0x8022xxxxx`. So the loaded image can't collide with the no-map node. The
  address is specific to this pongoOS session; after a new sepOS boot, take
  it from the `root shmem ... data=` line. The durable fix is for pongoOS to
  publish it to m1n1/Linux.
- **SEPFW staging, PA `0x804f48000` + `0x344000`: not reserved.** XNU frees it
  as soon as sepOS reports alive: `AppleSEPManager_handleOSAlive_releasesSEPFW`
  (`0xfffffff0062f960c`, "SEP/OS is alive") calls the firmware object's vtable
  slot `0x550` (`0xfffffff0062e7740`). That slot `ml_static_mfree`s the SEPFW
  descriptor's pages and removes `SEPFW`/`SEPPatches`/`uStuff` from
  `/chosen/memory-map`. The 12:09 boot got the matching liveness message
  (`ep=0 op=0x0d`). Linux has also used that range as ordinary RAM across
  every kexec since then, so reserving it now would protect nothing.
- **OOL buffers:** see the warning in section 4.

## 7. Completed: xART retest with the wrapper activated (tri53-sep2)

Date: 2026-10-26. This replaced the 14:00 attempt
(`docs/sep/logs/20261026-1400-linux-xart-reply-unconsumed.log`), in which the
four xarm OOL control words and the reply entered the AP-to-SEP FIFO and were
never consumed (`snd_sts=0xc101` for 40 s). Cause and order:
`docs/sep/xart-handshake.md` section 6. One new sepOS boot, one attempt, no
retries, and a cold reset at the end.

The 15:00 run completed this procedure and failed at the first OOL-control
word even though the primary mask reached `0x110`; see
`docs/sep/logs/20261026-1500-linux-wrapper-arm/`. Do not repeat this matrix.
The next experiment is the bounded endpoint-0 nap-wake discriminator in
`docs/sep/xart-handshake.md` section 7, using a kernel with an atomic
`wake-test` operation before any OOL publication.

| item | value |
|---|---|
| entry | `tri53-sep2-7b5d382d79` |
| payload | `build/boot/m1n1-linux-tri53-sep2-7b5d382d79.bin` |
| entry dir | `build/sep/tri53-sep2/tri53-sep2-entry/` |
| branch | `a9/tri53-sep2` = `7b5d382d79` on `a9/tri53-sep` `759c7ffcd4` (tri53-b1, full `a9/kexecfix`) |
| kernel commit | `7b5d382d79` wrapper activation on arm, OOL drain check, debugfs `fifo` |
| config | identical to tri53-sep's (tri53-b1 + `CONFIG_APPLE_SEP_A9=y`), debug info off |
| DTBs | byte-identical to tri53-sep's: `sep@20da00000`, `sep-shmem@87adb0000` (no-map, 16 KiB) |
| cmdline | tri53-sep's with the entry renamed, 887 bytes: one `pcie_apple_h9p.ans_keep_power=1`, `sep_a9.enable=1`, `crashkernel=256M` |
| packaging | `scripts/sep/package-tri53-sep2.sh`; manifest `build/sep/tri53-sep2/payload-manifest.json` |

### 7.1 Driver changes in tri53-sep2

- `start` (arm) first reads the SEP PMGR power-state word, read-only (the
  `apple,pmgr` child labelled `sep`, through the pmgr syscon). Unless its
  actual state is `0xf` and the reset bit is clear, the write fails with
  `Input/output error` (`No such device` if the node is missing), dmesg says
  `arm refused: ...`, and the SEP block is not touched. The driver never
  writes PMGR, CPU RUN `+0x44`, the NMI pair, reset or auto-boot. Then it
  does exactly this, from SEP base `0x20da00000`:

  ```text
  +0x4000 = 0x1111   +0x0c00 = 0x1111
  +0x4004 = 0x0001   +0x0c04 = 0x0001
  +0x4004 = 0x1000   +0x0c04 = 0x1000
  +0x4020 = read(+0x4020) | 1, read back, mb()
  ```

  dmesg gets `arm: sep power domain on (pwrstate=...)`, `arm: before ...`,
  `arm: wrote ...` and `arm: after ... (AP->SEP FIFO empty|not empty|full)`.
  Each snapshot line has `set4000 clr4004 setc00 clrc04 snd4008 rcv4020`.
  Arming still sends nothing.
- `ool` is refused with `Device or resource busy`, and nothing is
  allocated or sent, if the AP-to-SEP FIFO is not empty. After the four
  `cntl` words it waits up to 1 s for `snd_sts` bit 17. dmesg then says
  `ool ep 0x13: AP->SEP FIFO drained in N us (snd_sts A -> B): transport pass`,
  or `... not drained after N us ...: transport FAIL`, and in that case the
  write fails with `Connection timed out`. The buffers stay published
  either way: never retry `ool`.
- `send` makes the same 1 s check and reports it (`send ep 0x13 tag 0xTT: ...`).
  It only logs: the write succeeds once the word is queued.
- `fifo` (new, read-only): `ap->sep snd_sts=... (empty|not empty|full)`,
  `sep->ap recv_sts=...`, the live mask views, the last arm (PMGR word,
  `before`, `rmw4020 read/wrote`, `after`) and the last drain result.
  Before arming it prints `running=0 (mailbox untouched)` and reads no
  register.
- `status` now shows a non-empty `snd_sts` as `not empty` (tri53-sep printed
  `room`, which hid the 14:00 symptom).
- Unchanged: inert without `sep_a9.enable=1`, manual `advertise`, no
  automatic send or retry, and the `0x87adb0000` reservation.

### 7.2 The shmem reservation must match this boot

`sep-shmem@87adb0000` reserves the page that the 13:27 pongoOS session handed
to sepOS (`sep: -> root shmem ep=fe tag=1 pages=1 data=87adb0`). That address
belongs to one pongoOS session. The new boot's line must again say
`pages=1 data=87adb0`. If it differs, do not kexec into tri53-sep2: its
no-map node would protect the wrong page, and sepOS's real shared page would
be ordinary Linux RAM. Rebuild the DT for the new address first.
`scripts/sep/xart-retest-plan.py` enforces this.

### 7.3 Procedure

Pick a run directory once and reuse it in every step:

```sh
RUN=build/sep/tri53-sep2/run-YYYYMMDD-HHMM
```

**Step 0 (host, before DFU).** Check the artifacts, and record where this
boot's part of the appended pongoOS log will start:

```sh
(cd build/sep/tri53-sep2/tri53-sep2-entry && shasum -a 256 -c SHA256SUMS)
ls -l build/boot/m1n1-linux-tri53-sep2-7b5d382d79.bin build/prebuilt/pongo/Pongo-a9sep.bin
strings -n 20 "build/boot/$(readlink build/boot/default-payload)" | grep -o 'crashkernel=[^ ]*'
mkdir -p "$RUN"
if [ -f build/sep/pongo-sep.log ]; then wc -l <build/sep/pongo-sep.log; else echo 0; fi >"$RUN/pongo-lines-before"
```

The cold Linux (`build/boot/default-payload`) must carry `crashkernel=256M`.
Until tri53-sep2 runs, that crash region is the only thing keeping Linux off
`0x87adb0000`.

**Step 1: pongoOS cold boot and capture of the new xarm tag.**

```sh
SEP_BOOT=1 SEP_MODE=xnu scripts/boot/autoboot.sh --once
tail -n +"$(( $(cat "$RUN/pongo-lines-before") + 1 ))" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/xart-retest-plan.py "$RUN/pongo.log" --out "$RUN" | tee "$RUN/plan.txt"
```

Go only on exit status 0 (`GO: ...`). The script requires:

- exactly one `SEPOS BOOTED`;
- `overwritten=0`;
- the shmem match from 7.2;
- consistent `ep=fd` pairs;
- exactly one xarm message, with `op=e param=0 data=0`.

It prints `E` (the xarm endpoint) and `T` (the tag). It also writes
`$RUN/plan.env` and `$RUN/remote-xart.sh`, built only from this boot's lines.

On exit status 3 (`STOP: ...`), keep the logs, do not kexec, and cold-reset.
Do not run `pongo-sep.sh` again in this boot, and never reuse tag 2 from 13:27.
Run against the 13:27 log, the script reproduces the 14:00 commands exactly
(`E=0x13 T=0x02`, the four `raw` advertisements, `13 8000 8000`). As a check,
an altered shmem address and an altered xarm word each give `STOP`.

**Step 2: one warm kexec into tri53-sep2.** On the cold Linux, check the
crash region and the merge base first:

```sh
scripts/omarchy/ssh.sh 'tr " " "\n" </proc/cmdline | grep -E "^(linux_a9.entry|crashkernel)="; sudo -n grep -i "crash kernel" /proc/iomem; ls -l /boot/linux-a9/entries/"$(tr " " "\n" </proc/cmdline | sed -n "s/^linux_a9.entry=//p")"/dtb' | tee "$RUN/0-cold.txt"
ENTRY=tri53-sep2-7b5d382d79 scripts/boot/kexec-host.sh build/boot/m1n1-linux-tri53-sep2-7b5d382d79.bin
```

> Private graphics-dependent excerpt omitted from this source snapshot.

**Step 3: preconditions, then arm (wrapper, no mailbox word).**

```sh
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
scripts/sep/sep-dump.sh --arm | tee "$RUN/2-arm.txt"
```

`1-pre` needs:

- the `sep-shmem@87adb0000` reservation line;
- `sep on` in the power-domain section;
- the probe line ending `unarmed`;
- `running=0`, and `fifo` showing `arm: never`.

`--arm` refuses on its own unless the genpd line says `on`. `2-arm` needs:

- `arm: sep power domain on (pwrstate=...)`, with actual state `f`;
- `arm: after ... (AP->SEP FIFO empty)`;
- `fifo` showing `ap->sep snd_sts=... (empty)`.

The expected primary mask readback is `0x110` (`set4000`/`clr4004`; it was
`0x111` at 14:00), and `rcv4020` bit 0 should be set. Mirror readbacks
(`setc00`/`clrc04`) need not match the primary view, so they are not a gate.

Stop if:

- `start` failed or dmesg says `arm refused`: do not guess any PMGR, RUN or
  NMI write;
- the FIFO is not empty;
- `log` shows a new xarm request: record its raw word and derive the answer
  from its fields (xart-handshake section 2).

**Steps 4 to 6: declare this boot's advertisements, publish xarm OOL, reply
once.** All three are in the generated script:

```sh
cat "$RUN/remote-xart.sh"
scripts/omarchy/ssh.sh 'sudo -n sh -s' <"$RUN/remote-xart.sh" | tee "$RUN/3-xart.txt"
```

For the expected `E=13` it reads as follows (only `T` and the advertisement
words come from the new capture):

```sh
D=/sys/kernel/debug/sep-a9
cat "$D/fifo"; grep -q "^ap->sep snd_sts=.* (empty)$" "$D/fifo" || exit 1
echo "raw 00 636e746c 00000000" >"$D/advertise"     # one line per complete pair
echo "raw 02 61727473 04040404" >"$D/advertise"
echo "raw 10 78617273 08080808" >"$D/advertise"
echo "raw 13 7861726d 08080808" >"$D/advertise"
cat "$D/endpoints"
echo "13 8000 8000" >"$D/ool" || exit 1             # fails unless drained <= 1 s
cat "$D/fifo"
echo "13 TT 00 00 00000000" >"$D/send"              # result 0, capability 0
cat "$D/fifo"; cat "$D/status"
```

The `ool` write is the 1 s transport gate. If the four `cntl` words are not
consumed, the write fails, the script stops, and the reply is not sent. The
reply is capability 0 (`data=00000000`). Never send the capability-1 form in
the same boot.

**Step 7: capture 10 s, then cold reset.**

```sh
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/4-plus5s.txt"
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/5-plus10s.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | grep -E 'sep-a9|sep-shmem|crashkernel' >"$RUN/dmesg-sep.txt"
```

Then cold-reset (DFU, `scripts/boot.sh`), which also resets sepOS. After
step 5 there is no warm kexec, no second `ool`, no second reply, and no SBIO
step in this boot.

### 7.4 Reading the result

| observation | meaning | next |
|---|---|---|
| `arm refused`, or `--arm` says `REFUSED` | sep domain off or in reset | stop; report the PMGR word; no PMGR/RUN/NMI experiment |
| `ool` fails with `Device or resource busy`, nothing sent | FIFO not empty before publishing | stop, cold reset |
| `ool ... transport FAIL` (`Connection timed out`), no reply sent | observed at 15:00; ordinary V2 mask replay was insufficient | cold reset; next one-variable test is the bounded nap-wake pair (xart-handshake section 7), not IOMapper `setActive` or a guessed PMGR/RUN/NMI write |
| `ool ... transport pass` and `send ep 0x13 ... transport pass` | **transport pass**: all five words consumed | read `log` |
| transport pass plus new `ep=fd` pairs (expected `scrd`, `sbio`, `sks`, `sse`) | **xART-gate pass** | record every pair; SBIO (section 4) in a later boot |
| transport pass, no new `ep=fd` lines | the wrapper fix worked; xART semantics or another gate | report |
| a new xarm message (e.g. op `00` fetch) | SEP-driven storage request | record the raw word and stop; answer it from its fields, never by guessing |
| `ool` passes but the reply's `send ... transport FAIL` | sepOS stopped consuming after the OOL words | record `fifo` and `log`, stop |

Report back: `$RUN/plan.txt`, `0-cold.txt` through `5-plus10s.txt`,
`dmesg-sep.txt`, the pongoOS capture `$RUN/pongo.log`, and whether the
session ended with a cold reset.

## 8. Overtaken: SEP PMGR word kept, pongoOS wake split, Linux wake-test (tri53-sep3)

> **Do not run this section.** Only its pongoOS half ran, at 16:18
> (`docs/sep/logs/20261026-1618-pongo-nap-split.log`). One paired-32-bit
> wake word sent by pongoOS right after discovery stayed queued
> (`snd_sts 0x2cc01 -> 0xcd01`). The PMGR word was `0x4f0000ff`, SEP
> `+0x28/+0x44/+0xb88` read 0, the mask was `0x111`, and no Linux was
> involved. So the PMGR AUTO bit is not the cause. tri53-sep3 was never booted.
> Section 9 replaces it, and tri53-sep4 keeps everything below except the
> refusal on bit 28.

Date: 2026-10-26. The 15:42 run (`docs/sep/logs/20261026-1542-linux-nap-wake/`)
booted tri53-sep2 straight from pongoOS. Linux inherited the SEP PMGR word as
`0x4f0000ff` (`PMGR sep inherited @80400 reg=4f0000ff warm=0`). The
pmgr-pwrstate cold path then set bit 28 (`APPLE_PMGR_AUTO_ENABLE`), so arm
read `pwrstate=5f0000ff`. After that, the first nap-wake word was not
consumed in 1 s (`snd_sts 0x2cc01 -> 0xcd01`). tri53-sep3 keeps the word
exactly as pongoOS leaves it.

This run splits the question in two:

- **pongoOS:** does one wake word drain with no Linux involved at all?
- **Linux:** with the PMGR word kept at `0x4f0000ff`, does the bounded wake
  pair drain and get an answer?

As before: one new sepOS boot, one attempt per step, no retries, and a cold
reset at the end. There is no warm kexec in this session.

> Private graphics-dependent excerpt omitted from this source snapshot.

### 8.1 Kernel changes in tri53-sep3

- **PMGR.** `ps_sep` carries `apple,no-auto-pm`. pmgr-pwrstate only ever
  *reads* such a domain. That means no AUTO_ENABLE, no warm re-confirm write,
  no `apple,min-state` floor and no idle gate. Power-off is refused
  (`-EBUSY`). Power-on succeeds only if the domain is already on, and it
  writes nothing. Reset assert and deassert are refused. The domain stays
  `GENPD_FLAG_ALWAYS_ON`. Probe prints
  `PMGR sep retained @80400 reg=4f0000ff: no AUTO, never written`. If the
  domain was inherited off, it prints `... is off: left untouched, not
  always-on` instead, and arm will refuse.
- **sep-a9 probe** reads the PMGR word, then reads SEP `+0x28 +0x44 +0xb88
  +0x4008 +0x4020`, but only if that word says on and out of reset. All of
  this is read-only. Sample:
  `probe: pmgr=4f0000ff sep28=... sep44=... sepb88=... snd4008=... rcv4020=...`.
  sep-a9 probes about 10 ms before pmgr-pwrstate, so this is the word
  pongoOS left.
- **Arm** (`start`) requires the PMGR word to be exactly `0x4f0000ff`. If bit
  28 is set, the write fails with `Input/output error` and dmesg says
  `arm refused: PMGR sep AUTO_ENABLE (bit 28) set (pwrstate=...), not
  repaired, SEP untouched`. Any other value gives `arm refused:
  pwrstate=... is not the pongoOS handoff word 4f0000ff`. Neither case is
  repaired live.

  The wrapper sequence and polling are unchanged from tri53-sep2. Arm logs
  `arm: core before ...` and `arm: core after ...`, the same snapshot as
  probe. It warns `arm: PMGR sep word changed`, `arm: CPU control +0x44
  changed` or `arm: +0x4008 bit 0 changed` if one of them moved, and
  `arm: +0x4020 bit 0 clear after arm` if that bit is not set afterwards.
  The driver still never writes PMGR, `+0x44` RUN, NMI or reset. It never
  writes `+0x4008` at all, and the `+0x4020` RMW only sets bit 0.
- **`fifo`** gains these lines: `pmgr now=... (handoff|NOT handoff
  4f0000ff)` (a live read-only PMGR read, armed or not), `probe ...`,
  `arm before ...`, `arm after ...` and `wake-test: never|wake|no-wake|word1-stuck|error`.
  The first three lines are unchanged, so the `remote-xart.sh` gate still
  works.
- **`wake-test`** (new, mode 0600). `echo 1 > wake-test` runs XNU's
  endpoint-0 nap wake once per boot.
  - It sends nothing and refuses unless all of these hold:
    - the mailbox is armed (else `ENXIO`);
    - it has not run yet in this boot (`EALREADY`);
    - no endpoint has published OOL (`EBUSY`);
    - the PMGR word is still exactly `0x4f0000ff` (`EIO`);
    - the AP->SEP FIFO is empty (`EBUSY`).
  - It posts raw `0x00000000000e0000` (ep 0 tag 0 op 0x0e param 0 data 0) with
    the ordinary SND0/SND1 write. It waits up to 1 s for the FIFO to drain.
    The second word is posted only if the first one drained. It then waits
    up to 1 s for ep 0 op `0x0f` or `0x15`.
  - Nothing is retried. There is no 200-pair loop.
  - Write status:
    - `0` = wake indication received;
    - `Connection timed out` = word 1 not consumed (word 2 not sent);
    - `No data available` = no indication.
  - `cat wake-test` prints `result=...`, both drain times and `snd_sts`
    values, the indication (raw word and the delay since word 1), any other
    message seen in the window, and the PMGR word before and after. It reads
    no register.
  - dmesg gets `wake-test: pwrstate=4f0000ff ..., posting
    00000000000e0000`, the word-1 `transport pass/FAIL` line,
    `wake-test: pwrstate before=... after=...` and one `wake-test: result=...`
    line.
- Unchanged: inert without `sep_a9.enable=1`, manual `advertise`, `ool`
  (1 s drain gate), `send`, `xfer`, the `sep-shmem@87adb0000` reservation,
  and no automatic send.

### 8.2 Procedure

Pick one run directory and reuse it in every step:

```sh
RUN=build/sep/tri53-sep3/run-YYYYMMDD-HHMM
P=build/boot/m1n1-linux-tri53-sep3-c987851174.bin
```

**Step 0 (host, before DFU).**

```sh
(cd build/sep/tri53-sep3/tri53-sep3-entry && shasum -a 256 -c SHA256SUMS)
ls -l "$P" build/prebuilt/pongo/Pongo-a9sep.bin build/bin/pongoterm
mkdir -p "$RUN"
if [ -f build/sep/pongo-sep.log ]; then wc -l <build/sep/pongo-sep.log; else echo 0; fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled     # this boot is driven by hand
```

**Step 1: DFU -> patched pongoOS.** The owner holds DFU.

```sh
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/boot/autoboot.sh --state       # must print: pongo
```

palera1n can hang after `Booting pongoOS...`. Once `--state` prints `pongo`,
Ctrl-C is fine.

**Step 2: sepOS boot and discovery capture, then gate.**

```sh
scripts/sep/pongo-sep.sh --xnu status
tail -n +"$(( $(cat "$RUN/pongo-lines-before") + 1 ))" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/xart-retest-plan.py "$RUN/pongo.log" --out "$RUN" | tee "$RUN/plan.txt"
```

Go only on exit status 0 (`GO: root shmem 0x87adb0000 matches the
reservation`, exactly one xarm request). That gives this boot's `E` and `T`
in `$RUN/plan.env` and the generated `$RUN/remote-xart.sh`.

On `STOP`, do not bootm into tri53-sep3: its no-map node would protect the
wrong page. Keep the logs and cold-reset. Never run `pongo-sep.sh` twice in
one boot.

**Step 3: pongoOS split (the only pongoOS write after discovery).**

```sh
scripts/sep/pongo-split.sh "$RUN/pongo-split.txt" | tee "$RUN/pongo-split-summary.txt"
```

In one held pongoterm session, the script runs:

1. `sep stat`;
2. `peek` of `0x20e080400` (SEP PMGR), `0x20da00028`, `0x20da00044`,
   `0x20da00b88`, `0x20da04008` (AP->SEP status) and `0x20da04020`
   (SEP->AP status);
3. `sep msg 0 0 e 0 0`, i.e. raw `0x00000000000e0000`, exactly once;
4. a 1 s wait on the host;
5. the same six peeks, then `sep stat`.

The pongoOS SEP IRQ task consumes and prints every reply after `sep msg`
(`SEP->AP ep=...`), and stores it in the ring. So a `0x0f`/`0x15` answer
shows up here, **not** in Linux's `log` later. The script also lists the new
ring entries. Never re-run it: exit status 3 means the word may already be
queued.

The old build had one more place a reply could end up: `sep stat` read the FIFO
for a `raw-register` line. That behavior is now removed because an empty read
latched outbox-underflow bit 19 and invalidated the later transport test.
Current `sep stat` reports `fifo_peek=0`; replies can appear only through the
IRQ-drained software ring. Do not use an older binary for this procedure.

| `RESULT pongo-wake=` | meaning | then |
|---|---|---|
| `wake` | snd drained, ep 0 `0x0f`/`0x15` answered: the nap gate is real and the transport works with no Linux involved | step 4, full Linux test |
| `drained-no-wake` | word consumed, no indication: the transport is alive, sepOS not in nap | step 4, full Linux test |
| `stuck` | word still queued after 1 s with no Linux involved: the failure is below the Linux PMGR write (dead sepOS, wrapper delivery, pongoOS state) | step 4 for read-only data only: arm, check whether the queued pongoOS word drains after the wrapper arm. `wake-test` will refuse with `EBUSY`; that one refused write is allowed. No OOL. Cold reset |
| `fifo-busy-before` | AP->SEP FIFO already not empty before the word | stop, no bootm, cold reset |

Also record:

- whether `pmgr` read `4f0000ff` both times (the summary says
  `handoff 4f0000ff`);
- the `sep28`, `sep44` and `sepb88` values. They are the baseline that the
  Linux `probe:` line is compared against.

**Step 4: bootm straight into tri53-sep3 (no kexec).**

```sh
scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state       # wait for: linux
```

If it still says `pongo` after 15 s, send `printf 'bootm\n' | build/bin/pongoterm`
once. This is autoboot's "first bootm eaten" case.

**Step 5: preconditions, and the PMGR word still 0x4f0000ff before arm.**

```sh
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

`1-pre` needs all of these:

- the cmdline shows `linux_a9.entry=tri53-sep3-c987851174` and
  `sep_a9.enable=1`;
- the `sep-shmem@87adb0000` reservation line;
- under `== pmgr`: `PMGR sep raw before floor @80400 reg=4f0000ff` and
  `PMGR sep retained @80400 reg=4f0000ff: no AUTO, never written`, with no
  `5f0000ff` anywhere;
- under `== probe`: `probe: pmgr=4f0000ff sep28=... sep44=... sepb88=...
  snd4008=... rcv4020=...`. Compare these with the step 3 "after" peeks and
  record any difference. Between those peeks and this line only m1n1 and
  early Linux run, and sep-a9 probes before pmgr-pwrstate, so no PMGR
  domain driver has run yet. A changed `pmgr`/`sep28`/`sep44`/`sepb88` is
  therefore most likely m1n1's doing, and that is the next split if arm then
  refuses;
- the probe line ending `unarmed`, and `sep on` in the power domain;
- `fifo` showing `running=0`, `pmgr now=4f0000ff (handoff 4f0000ff)`,
  `arm: never` and `wake-test: never`.

Stop if `pmgr now` or any PMGR line is not `4f0000ff`. Record it, do not
arm, and never write PMGR.

**Step 6: arm (wrapper, no mailbox word).**

```sh
scripts/sep/sep-dump.sh --arm | tee "$RUN/2-arm.txt"
```

`2-arm` needs all of these:

- `arm: sep power domain on (pwrstate=4f0000ff)`;
- `arm: core before pmgr=4f0000ff ...`;
- `arm: after ... (AP->SEP FIFO empty)`;
- `arm: core after pmgr=4f0000ff ...`;
- no `changed` or `bit 0 clear` warning;
- `fifo` with `pmgr now=4f0000ff (handoff 4f0000ff)`.

`log` may show messages that were queued for the AP at handoff; record them.

Stop if:

- `start` failed with `arm refused`. Record the line. If it is the bit-28
  case, the PMGR fix did not hold: report the `== pmgr` lines and do not
  repair.
- The FIFO is not empty. That is expected only after a pongoOS `stuck`.
  Wait 1 s, `cat fifo`, and record whether the queued word drained after the
  arm. Do not send anything.

**Step 7: wake-test (one write).**

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' <<'EOF' | tee "$RUN/3-wake.txt"
D=/sys/kernel/debug/sep-a9
echo 1 >"$D/wake-test"; echo "wake-test rc=$?"
cat "$D/wake-test"
cat "$D/fifo"
cat "$D/log"
dmesg | grep -E 'sep-a9.*wake-test' | tail -8
EOF
```

| `result=` | meaning | next |
|---|---|---|
| `wake` (rc 0) | both words drained and ep 0 `0x0f`/`0x15` came back: nap was the gate, and keeping the PMGR word fixed the transport | step 8 |
| `no-wake` (`No data available`) | words consumed (check `word1/word2 drained=1`), no indication: transport alive, sepOS not in the XNU nap state | record `other=`; stop, no OOL, cold reset. The next boot may test xarm OOL directly |
| `word1-stuck` (`Connection timed out`) | the first word was not consumed although PMGR stayed `4f0000ff` | stop, cold reset. If pongoOS gave `wake`/`drained-no-wake`, something else Linux does between bootm and arm still breaks delivery. That is the next split. Do not try RUN/NMI/PMGR writes |
| refused `Device or resource busy` | FIFO not empty or OOL published; nothing sent | stop |
| refused `Input/output error` | PMGR word no longer `4f0000ff`; nothing sent | stop, report `fifo` and `== pmgr` |
| `error` | the first or second post found the FIFO full | stop |

Check that `pwrstate before=4f0000ff after=4f0000ff` in `cat wake-test`. This
is the last PMGR check before any xarm traffic.

**Step 8: only on `result=wake`. Declare, publish xarm OOL, reply with this
boot's tag.**

```sh
cat "$RUN/plan.env" "$RUN/remote-xart.sh"
scripts/omarchy/ssh.sh 'sudo -n sh -s' <"$RUN/remote-xart.sh" | tee "$RUN/4-xart.txt"
```

The generated script does these steps in order:

1. checks that `fifo` says `ap->sep snd_sts=... (empty)`;
2. declares only this boot's advertisements (`raw ID NAME OOL` lines);
3. publishes xarm's `E 8000 8000` OOL. This step fails, and stops the
   script, unless the four `cntl` words drain within 1 s;
4. sends the capability-0 reply `E T 00 00 00000000`, with `T` from **this**
   boot's `plan.env`. Never reuse tag 2 from an earlier boot.

After step 8, the OOL buffers belong to this sepOS boot: no warm kexec.

**Step 9: capture 10 s, then cold reset.**

```sh
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/5-plus5s.txt"
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/6-plus10s.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | grep -E 'sep-a9|sep-shmem|PMGR sep|crashkernel' >"$RUN/dmesg-sep.txt"
rm -f build/boot/autoboot.disabled
```

Then cold-reset (DFU), which also resets sepOS. Run step 9's captures even
when the run stopped at step 6 or 7. The xART-gate pass condition is the same
as in 7.4: new `ep=fd` pairs (expected `scrd`, `sbio`, `sks`, `sse`) after the
reply. A new xarm request (e.g. op `00`) means a SEP-driven storage request:
record its raw word and stop.

### 8.3 Report back

- `$RUN/plan.txt`, `pongo.log`, `pongo-split.txt` and
  `pongo-split-summary.txt`;
- `1-pre.txt` through `6-plus10s.txt` (whichever ran) and `dmesg-sep.txt`;
- the three PMGR comparisons:
  - the pongoOS peeks;
  - `probe:` and `retained`;
  - `arm: core before/after` and `wake-test pwrstate before/after`;
- the SEP `+0x28/+0x44/+0xb88` values from the pongoOS peeks against the
  `probe:` line;
- the two verdicts, `RESULT pongo-wake=` and `wake-test result=`;
- whether the session ended with a cold reset.

## 9. Next DFU: one 64-bit store per AP->SEP message (tri53-sep4)

Date: 2026-10-26. Every AP->SEP word so far went out as two 32-bit stores to
`+0x4010/+0x4014`. That covers pongoOS `sep msg`, every sep-a9 up to
tri53-sep3, and the xarm OOL words at 14:00 and 15:00.

- **Consumed:** the boot handshake. `status`, `boot_tz0`, `fwload` and the
  root-shmem word were all acked, possibly because SEPROM or early sepOS
  polls the mailbox.
- **Not consumed:** every post-discovery word, i.e. the xarm OOL words at
  14:00 and 15:00, the 15:42 Linux wake and the 16:18 pongoOS wake (no Linux
  involved). sepOS presumably waits on the notification by then.

XNU's `AppleA7IOPV2::postMailbox` (`0xfffffff005b4e4a8`) instead does one
64-bit `str` to `+0x4010`. The access width is now the leading hypothesis
for that post-discovery notification (`docs/sep/xart-handshake.md` section 7).
A `drained` result below therefore says the width matters once sepOS waits
for a notification. It does not by itself say the width is the only
difference from XNU.

This run changes only the post-discovery store width:

- **pongoOS.** The boot handshake stays on the paired-32-bit path that
  worked (`split32`). After discovery, one `sep msg64 0 0 e 0 0` is sent:
  one 64-bit store to `+0x4010`, then `dsb sy`.
- **Linux.** Boot tri53-sep4 directly, without kexec. Every message it sends
  is one 64-bit `writeq()` to `+0x4010` followed by `mb()`. Arm, then run
  `wake-test`.

The generated xart script runs only if `wake-test` reports `result=wake`.
As before: one sepOS boot, one attempt per step, no retries, and a cold reset
at the end.

| item | value |
|---|---|
| entry | `tri53-sep4-5c9be8757e` |
| payload | `build/boot/m1n1-linux-tri53-sep4-5c9be8757e.bin` |
| entry dir | `build/sep/tri53-sep4/tri53-sep4-entry/` |
| branch | `a9/tri53-sep4` = `5c9be8757e` on `a9/tri53-sep3` `c987851174` (tri53-b1, full `a9/kexecfix`) |
| kernel commit | `5c9be8757e`: one-store TX, AUTO_ENABLE downgraded to a warning |
| config | identical to tri53-sep3's (tri53-b1 + `CONFIG_APPLE_SEP_A9=y`), debug info off |
| DTBs | byte-identical to tri53-sep3's (`apple,no-auto-pm` on `ps_sep`, `sep-shmem@87adb0000`) |
| cmdline | tri53-sep3's with the entry renamed, 887 bytes: one `pcie_apple_h9p.ans_keep_power=1`, `sep_a9.enable=1` |
| TX check | `build/sep/tri53-sep4/tx-disasm.txt`: `sep_send_raw` disassembled from this commit's `vmlinux` |
| packaging | `scripts/sep/package-tri53-sep4.sh`; manifest `build/sep/tri53-sep4/payload-manifest.json` |
| pongoOS | `build/prebuilt/pongo/Pongo-a9sep.bin` with `sep msg64` (workspace `04b616c4`); probe script `scripts/sep/pongo-msg64-wake.sh` |

### 9.1 Changes in tri53-sep4 (against tri53-sep3)

- **TX.** `sep_send_raw` is the only AP->SEP write: `ool`, `xfer`, `send` and
  `wake-test` all call it. It now does `writeq(low | high << 32, mbox +
  0x10)` then `mb()`. It is `noinline`, so the built code has exactly one
  store site:

  ```text
  ffff80008079dfc4  ldr  x8, [x19, #0x10]   // sep->mbox
  ffff80008079dfc8  dmb  oshst              // writeq's leading barrier
  ffff80008079dfcc  mov  w21, wzr
  ffff80008079dfd0  add  x8, x8, #0x10
  ffff80008079dfd4  str  x20, [x8]          // the one 64-bit store to SEP +0x4010
  ffff80008079dfd8  dsb  sy                 // mb(), the same barrier as pongoOS msg64
  ```

  The packaging script refuses unless `tx-disasm.txt` shows exactly this: one
  no-offset store, 64-bit, to `mbox+0x10`, between `dmb oshst` and `dsb sy`,
  and no 32-bit store at `+0x10/+0x14`. The `+0x4014` register define is
  gone. Probe prints `tx: one 64-bit store per message to +0x4010 (writeq)`.
  `wake-test` logs `posting 00000000000e0000 (64-bit store to +0x4010)`, and
  `cat wake-test` shows `tx=writeq+0x4010`.
- **PMGR gate.** Arm and `wake-test` share one check.
  - Still refused, touching nothing (`Input/output error`): the domain is
    off, it is in reset, or the word differs from `0x4f0000ff` in any bit
    other than 28.
  - Bit 28 (AUTO_ENABLE) alone is now only a warning:
    `arm: PMGR sep AUTO_ENABLE (bit 28) set (pwrstate=...), not repaired,
    continuing`.
  - `fifo` shows `pmgr now=... (handoff|handoff+AUTO|NOT handoff 4f0000ff)`.
- **Unchanged from 8.1:**
  - `apple,no-auto-pm` (the domain is never written, so 28 should stay
    clear);
  - the probe/arm snapshots and warnings;
  - the wrapper arm and polling;
  - the `wake-test` rules: once per boot; it refuses unless armed, with no
    OOL published and an empty FIFO; 1 s drain for word 1, word 2 only after
    that, then a 1 s wait for ep 0 `0x0f`/`0x15`;
  - the `fifo` lines that `remote-xart.sh` greps;
  - the reservation, and no automatic send.

### 9.2 Procedure

```sh
RUN=build/sep/tri53-sep4/run-YYYYMMDD-HHMM
P=build/boot/m1n1-linux-tri53-sep4-5c9be8757e.bin
```

**Step 0 (host, before DFU).**

```sh
(cd build/sep/tri53-sep4/tri53-sep4-entry && shasum -a 256 -c SHA256SUMS)
ls -l "$P" build/prebuilt/pongo/Pongo-a9sep.bin build/bin/pongoterm scripts/sep/pongo-msg64-wake.sh
strings -n 5 build/prebuilt/pongo/Pongo-a9sep.bin | grep -c 'AP->SEP64 ep='     # must be 1
grep -nE 'oshst|str\s+x[0-9]+, \[x[0-9]+\]$|dsb' build/sep/tri53-sep4/tx-disasm.txt   # one str x20, [x8] between dmb oshst and dsb sy
mkdir -p "$RUN"
if [ -f build/sep/pongo-sep.log ]; then wc -l <build/sep/pongo-sep.log; else echo 0; fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled
```

If the pongoOS binary has no `AP->SEP64` string, `sep msg64` has not been
built in: stop and rebuild pongoOS (`scripts/sep/build-pongo-a9sep.sh`)
before the DFU. Do not fall back to `sep msg`. A stuck 32-bit word would
leave the FIFO non-empty, and Linux's `wake-test` would then refuse.

**Step 1: DFU -> patched pongoOS** (the owner holds DFU).

```sh
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/boot/autoboot.sh --state       # must print: pongo
```

**Step 2: sepOS boot on the legacy split32 handshake, then gate.**

```sh
scripts/sep/pongo-sep.sh --xnu status split32
tail -n +"$(( $(cat "$RUN/pongo-lines-before") + 1 ))" build/sep/pongo-sep.log >"$RUN/pongo.log"
grep 'AP->SEP TX=' "$RUN/pongo.log"                 # must say: split32 legacy
python3 -B -I scripts/sep/xart-retest-plan.py "$RUN/pongo.log" --out "$RUN" | tee "$RUN/plan.txt"
```

`split32` keeps the boot handshake that worked, so the post-discovery word
is the only 64-bit store in this pongoOS session.

Go only on `GO` (shmem `0x87adb0000`, exactly one xarm request). On `STOP`,
do not bootm into tri53-sep4: keep the logs and cold-reset.

**Step 3: pongoOS single-store probe (the only pongoOS write after
discovery).**

```sh
scripts/sep/pongo-msg64-wake.sh "$RUN/pongo-msg64.txt" | tee "$RUN/pongo-msg64-summary.txt"
grep -E 'SEP->AP|AP->SEP64' "$RUN/pongo-msg64.txt"
```

The script sends exactly `sep stat`, `sep msg64 0 0 e 0 0` and `sep stat`.
`sep msg64` refuses a non-empty FIFO. Otherwise it does one 64-bit store to
`+0x4010` and then `dsb sy`. Never run the script twice in one boot.

If an ep 0 `0x0f`/`0x15` reply comes, it shows up only here. `sep msg64`
turns on pongoOS's reply logging, and the IRQ task consumes and prints each
reply as `SEP->AP ...`. The reply also raises the second `sep stat`'s
`total`. That reply is useful, but the verdict is AP->SEP status bit 17
after 1 s.

| `RESULT pongo-msg64=` (exit) | meaning | then |
|---|---|---|
| `drained` (0) | the single 64-bit store was consumed: the access width was the missing notification under pongoOS | step 4, full Linux test |
| `stuck` (5) | one 64-bit store is not enough under pongoOS. pongoOS leaves the wrapper mask at `0x111`, i.e. inbox bit 0 masked; Linux's arm clears it | optional read-only Linux datum: steps 4 to 6 only, then record whether the queued word drains once arm unmasks bit 0. `wake-test` is valid only if the FIFO is empty again; it refuses otherwise. No OOL. Cold reset |
| `busy-before` (4) | FIFO not empty before the probe; nothing sent | no valid experiment: no bootm, cold reset |
| incomplete (3) | the word may be queued | no bootm, cold reset |
| not reached (1) | no write marker | may re-run once |

**Step 4: bootm straight into tri53-sep4.**

```sh
scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state       # wait for: linux; if still pongo after 15 s: printf 'bootm\n' | build/bin/pongoterm
```

**Step 5: preconditions.**

```sh
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

`1-pre` needs all of these:

- the entry `tri53-sep4-5c9be8757e` and `sep_a9.enable=1`;
- the `sep-shmem@87adb0000` reservation;
- under `== pmgr`: `PMGR sep retained @80400 reg=4f0000ff`;
- under `== probe`, both lines:
  - `probe: pmgr=4f0000ff sep28=... sep44=... sepb88=... snd4008=...
    rcv4020=...`. Compare it with pongoOS's 16:18 values and with this
    boot's `sep stat` `snd_sts`;
  - `tx: one 64-bit store per message to +0x4010 (writeq)`;
- the probe line ending `unarmed`, and `sep on`;
- `fifo` with `running=0`, `pmgr now=4f0000ff (handoff 4f0000ff)`,
  `arm: never` and `wake-test: never`.

If `pmgr now` says `handoff+AUTO`, record it and go on: arm only warns. Stop
on `NOT handoff`.

**Step 6: arm (wrapper, no mailbox word).**

```sh
scripts/sep/sep-dump.sh --arm | tee "$RUN/2-arm.txt"
```

`2-arm` needs all of these:

- `arm: sep power domain on (pwrstate=4f0000ff)`;
- `arm: core before/after pmgr=4f0000ff ...`;
- `arm: after ... (AP->SEP FIFO empty)`;
- no `changed` or `bit 0 clear` warning.

A `PMGR sep AUTO_ENABLE (bit 28) set ..., continuing` warning is recorded,
not a stop.

After a pongoOS `stuck`, `arm: after` shows the queued word (`not empty`). In
that case:

1. wait 1 s;
2. `cat fifo` and record whether `ap->sep snd_sts` became `(empty)`, which
   would mean the inbox unmask let the 64-bit word through;
3. only if it did, step 7 is still a valid one-shot test.

**Step 7: 64-bit wake-test (one write).**

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' <<'EOF' | tee "$RUN/3-wake.txt"
D=/sys/kernel/debug/sep-a9
echo 1 >"$D/wake-test"; echo "wake-test rc=$?"
cat "$D/wake-test"
cat "$D/fifo"
cat "$D/log"
dmesg | grep -E 'sep-a9.*(wake-test|AUTO_ENABLE)' | tail -10
EOF
```

`cat wake-test` must show `tx=writeq+0x4010`.

| `result=` | meaning | next |
|---|---|---|
| `wake` (rc 0) | both 64-bit words drained and ep 0 `0x0f`/`0x15` came back | **step 8** |
| `no-wake` (`No data available`) | both words drained (`word1/word2 drained=1`), no indication: **transport pass** with the 64-bit store, but sepOS was not in the nap state | record `other=`; no xart this boot. The next cold boot does xarm OOL and the reply directly. Cold reset |
| `word1-stuck` (`Connection timed out`) | even a single 64-bit store, after the Linux arm, was not consumed | stop, cold reset. With pongoOS `drained`, Linux differs from pongoOS beyond the store width (wrapper-arm writes, mask views). That is the next split |
| refused `Device or resource busy` | FIFO not empty (queued pongoOS word) or OOL published; nothing sent | stop |
| refused `Input/output error` | PMGR word off, in reset, or differs beyond bit 28; nothing sent | stop, report `fifo` and `== pmgr` |
| `error` | a post found the FIFO full | stop |

**Step 8: only on `result=wake`.**

```sh
cat "$RUN/plan.env" "$RUN/remote-xart.sh"
scripts/omarchy/ssh.sh 'sudo -n sh -s' <"$RUN/remote-xart.sh" | tee "$RUN/4-xart.txt"
```

The script is the same as in section 8: this boot's advertisements, then
xarm `E 8000 8000` OOL (stops unless the four `cntl` words drain within
1 s), then the capability-0 reply `E T 00 00 00000000` with this boot's
`T`. All five of these words are 64-bit stores now. After this step: no
warm kexec.

**Step 9: capture 10 s, then cold reset** (also after a stop at step 6 or 7).

```sh
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/5-plus5s.txt"
sleep 5; scripts/sep/sep-dump.sh | tee "$RUN/6-plus10s.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | grep -E 'sep-a9|sep-shmem|PMGR sep|crashkernel' >"$RUN/dmesg-sep.txt"
rm -f build/boot/autoboot.disabled
```

Then cold-reset (DFU). The xART-gate pass condition is the same as in 7.4:
new `ep=fd` pairs (`scrd`, `sbio`, `sks`, `sse`) after the reply.

| pongoOS | Linux | reading |
|---|---|---|
| `drained` | `wake` or `no-wake` | the access width was the transport bug; Linux and pongoOS agree |
| `drained` | `word1-stuck` | width is necessary but Linux breaks delivery some other way: next split |
| `stuck` | queued word drained after arm | both the 64-bit store and inbox bit 0 unmasked are needed |
| `stuck` | still queued after arm | neither width nor mask: stop and re-read the wrapper path |

### 9.3 Report back

- `$RUN/plan.txt`, `pongo.log`, `pongo-msg64.txt` and
  `pongo-msg64-summary.txt`;
- `1-pre.txt` through `6-plus10s.txt` (whichever ran) and `dmesg-sep.txt`;
- both verdicts, `RESULT pongo-msg64=` and `wake-test result=`, and after a
  pongoOS `stuck`, the post-arm `fifo`;
- the PMGR word at each point: probe, retained, arm, `pmgr now`, and
  `wake-test before/after`;
- whether the session ended with a cold reset.

## 10. Fresh-DFU xART run: safe 64-bit RX and TX (tri53-sep5)

Date: 2026-10-26. The 17:02 pongoOS run resolved the transport split:

- a 65-second passive hold left `recv_sts=0x28801` and the capture ring at
  16 entries, proving that status-only sampling does not underflow the
  outbox;
- one 64-bit AP->SEP wake word drained (`snd_sts` returned to bit 17 set);
- sepOS answered with endpoint 0 opcode `0x0f`.

`tri53-sep4` already checks `recv_sts` bit 17 before every receive pop, so it
does not read an empty data register. It still assembled a nonempty word from
two 32-bit loads at `+0x4034/+0x4038`. `tri53-sep5` changes only that pop to
XNU's one 64-bit load from `+0x4038`; TX remains one 64-bit store to
`+0x4010`. Do not run Linux `wake-test` in this procedure: pongoOS has already
woken sepOS and consumed the `0x0f` indication.

| item | value |
|---|---|
| entry | `tri53-sep5-29f1192ef2` |
| payload | `build/boot/m1n1-linux-tri53-sep5-29f1192ef2.bin` |
| payload SHA-256 | `a39100c3124a60d5d2b17962a64e0a0b627d6500f995d3dbcda82c4418e746c1` |
| branch | `a9/tri53-sep5` = `29f1192ef2ed4bb69b56fe6dd93fd51fdafae50c` |
| base | `a9/tri53-sep4` = `5c9be8757e161c9f53bfb192c5a119f1dbff0a90` |
| config / DTBs | byte-identical to tri53-sep4 |
| RX audit | `build/sep/tri53-sep5/rx-disasm.txt`: one `ldr x0, [x8]` after `add x8, x8, #0x38`, then `dmb oshld`; no `ldr w` |
| TX audit | `build/sep/tri53-sep5/tx-disasm.txt`: one `str x20, [x8]` after `add x8, x8, #0x10`, then `dsb sy` |
| packaging | `scripts/sep/package-tri53-sep5.sh`; manifest `build/sep/tri53-sep5/payload-manifest.json` |

This is one fresh-DFU, one-shot run. After any post-write failure, collect
the state and cold-reset; do not retry a mailbox write in the same boot.

### 10.1 Host preflight

```sh
RUN=build/sep/tri53-sep5/run-$(date +%Y%m%d-%H%M)
P=build/boot/m1n1-linux-tri53-sep5-29f1192ef2.bin
mkdir -p "$RUN"
(cd build/sep/tri53-sep5/tri53-sep5-entry && shasum -a 256 -c SHA256SUMS)
test "$(shasum -a 256 "$P" | awk '{print $1}')" = a39100c3124a60d5d2b17962a64e0a0b627d6500f995d3dbcda82c4418e746c1
grep -E 'add.*#0x38|ldr[[:space:]]+x0|dmb[[:space:]]+oshld' build/sep/tri53-sep5/rx-disasm.txt
grep -E 'add.*#0x10|str[[:space:]]+x20|dsb[[:space:]]+sy' build/sep/tri53-sep5/tx-disasm.txt
if test -f build/sep/pongo-sep.log; then
    wc -l <build/sep/pongo-sep.log
else
    echo 0
fi >"$RUN/pongo-lines-before"
touch build/boot/autoboot.disabled
```

Pass: every `SHA256SUMS` line is `OK`, the payload hash matches, RX shows one
64-bit load at `+0x38`, and TX shows one 64-bit store at `+0x10`. Stop before
DFU on any mismatch.

### 10.2 Fresh DFU and sepOS discovery

The owner enters DFU, then:

```sh
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k build/prebuilt/pongo/Pongo-a9sep.bin
scripts/boot/autoboot.sh --state
scripts/sep/pongo-sep.sh --xnu status split32 | tee "$RUN/0-pongo-run.txt"
START=$(( $(cat "$RUN/pongo-lines-before") + 1 ))
tail -n "+$START" build/sep/pongo-sep.log >"$RUN/pongo.log"
python3 -B -I scripts/sep/xart-retest-plan.py "$RUN/pongo.log" --out "$RUN" | tee "$RUN/plan.txt"
```

Pass gates:

- state is `pongo` before `pongo-sep.sh`;
- `A9 AP->SEP TX=split32 legacy`, `SEPOS BOOTED`, and `post-boot capture
  quiescent` appear once;
- capture is `total=16 retained=16 overwritten=0` with no RX fault;
- root shmem is exactly `0x87adb0000`;
- the only complete advertisements are `cntl 00`, `arts 02`, `xars 10`, and
  `xarm 13` with the recorded page counts;
- `plan.txt` says `GO` and `xarm request ... E=0x13 T=0x02` (raw `e0213`).

If palera1n stalls at softboot or reports `e0004051`, re-enter fresh DFU and
restart this section; no valid sepOS session exists. On any discovery/parser
failure, do not send the wake word and do not boot tri53-sep5.

### 10.3 Pongo 64-bit wake gate

```sh
scripts/sep/pongo-msg64-wake.sh "$RUN/pongo-msg64.txt" | tee "$RUN/pongo-msg64-summary.txt"
grep -E 'AP->SEP64|ring ep=0 tag=0 op=f|RESULT pongo-msg64=' \
    "$RUN/pongo-msg64.txt" "$RUN/pongo-msg64-summary.txt"
grep -q 'RESULT pongo-msg64=drained' "$RUN/pongo-msg64-summary.txt"
grep -q 'ring ep=0 tag=0 op=f param=0 data=0' "$RUN/pongo-msg64.txt"
```

Pass requires all of the following:

- passive preflight stays fault-free for 65 seconds, with ring `16 -> 16`;
- exactly one `AP->SEP64 ... raw=e0000` line;
- final AP->SEP status has bit 17 set (`snd_sts=0x2dd01` in the proven run);
- ring grows `16 -> 17` with `ep=0 tag=0 op=f param=0 data=0`;
- verdict is exactly `RESULT pongo-msg64=drained`.

Any `busy-before`, `stuck`, `rx-fault-*`, incomplete output, or a drained word
without opcode `0x0f` is a stop: keep the logs and cold-reset. Never run the
wake script twice in one sepOS boot.

### 10.4 Boot the same live sepOS into tri53-sep5

Only after every 10.3 gate passes:

```sh
scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state
# If it still says pongo after 15 seconds:
printf 'bootm\n' | build/bin/pongoterm
scripts/boot/autoboot.sh --state
scripts/sep/sep-dump.sh | tee "$RUN/1-pre.txt"
```

Pass requires state `linux`, entry `tri53-sep5-29f1192ef2`,
`sep_a9.enable=1`, the `sep-shmem@87adb0000` reservation, `sep on`, and probe
lines:

```text
tx: one 64-bit store per message to +0x4010 (writeq)
rx: one 64-bit load per message from +0x4038 (readq, nonempty only)
```

`fifo` must show `running=0`, AP->SEP empty, `arm: never`, and `wake-test:
never`. Stop without arming if the PMGR word is neither `0x4f0000ff` nor the
previously accepted handoff+AUTO variant, if the reservation is missing, or
if either FIFO/fault state is unexpected.

### 10.5 Arm, declare, publish xarm OOL, and reply to tag 2

```sh
scripts/sep/sep-dump.sh --arm | tee "$RUN/2-arm.txt"
cat "$RUN/plan.env" "$RUN/remote-xart.sh"
scripts/omarchy/ssh.sh 'sudo -n sh -s' <"$RUN/remote-xart.sh" | tee "$RUN/3-xart.txt"
sleep 2
scripts/sep/sep-dump.sh | tee "$RUN/4-plus2s.txt"
sleep 8
scripts/sep/sep-dump.sh | tee "$RUN/5-plus10s.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' | \
    grep -E 'sep-a9|sep-shmem|PMGR sep|crashkernel' >"$RUN/dmesg-sep.txt"
```

Arm pass:

- `arm: sep power domain on` and core before/after values agree;
- `arm: after ... (AP->SEP FIFO empty)`;
- `running=1`, no fault bit, and no `changed`/`bit 0 clear` warning.

Do **not** write `wake-test`; pongoOS already completed the wake exchange.

The generated script must declare the four captured endpoints, issue xarm OOL
`13 8000 8000`, and reply `13 02 00 00 00000000`. OOL pass is four `cntl`
words consumed within one second (`AP->SEP FIFO drained ... transport pass`).
Reply transport pass is the later `fifo` showing AP->SEP empty and a total TX
delta of five: four OOL-control words plus the tag-2 response. Do not resend
either operation.

The xART-gate protocol pass is a new discovery set after the reply, including
the previously blocked services such as `sbio`, `sks`, `scrd`, or `sse`.
If all five TX words drain but those endpoints do not appear within ten
seconds, record it as **transport pass, xART protocol incomplete**, not as a
mailbox failure.

Finally:

```sh
rm -f build/boot/autoboot.disabled
```

Cold-reset after the captures, whether the run passed or stopped. Report the
entire `$RUN` directory, especially `plan.txt`, both pongo logs, `1-pre.txt`
through `5-plus10s.txt`, and `dmesg-sep.txt`.

## 11. tri53-sep6: one-shot `arts` MANIFEST probe

The tri53-sep5 run proved the fixed 64-bit transport and consumed the xarm
capability reply, but produced no new endpoint advertisements. That xarm
operation has no response-to-the-response. The next protocol action is the
separate `AppleSEPARTStorageService` MANIFEST transaction on `arts`.

Use the fresh-DFU, stop-on-first-result procedure in
[`docs/sep/arts-manifest.md`](arts-manifest.md). It deliberately publishes
only the 16 KiB arts OOL binding, uploads the exact 6,157-byte personalized
IM4M, and sends one tag-1/opcode-6 request. It does not publish xarm/xars or
issue an ART storage mutation command.

## 12. tri53-sep6 combined gate

The isolated arts MANIFEST completed successfully on hardware, but discovery
remained at four endpoints. Since the isolated xarm capability-0 reply had the
same outcome, the next test combines both proven prerequisites.

Use [`docs/sep/arts-xarm-combined.md`](arts-xarm-combined.md). The updated
planner generates guards for this order: xarm OOL, arts OOL, successful
MANIFEST, two-second observation, then exactly one captured-tag xarm
capability-0 reply. The run stops at discovery; it does not start SBIO.
