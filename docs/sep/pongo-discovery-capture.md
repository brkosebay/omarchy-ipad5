# Pongo SEP endpoint-discovery capture

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Date: 2026-10-26

`Pongo-a9sep.bin` now preserves the complete cold-boot mailbox stream needed
to hand endpoint metadata to the Linux driver's manual-declare interface.
This is a passive extension of the already working `sep xnu` sequence: after
`SEPOS BOOTED`, it sends no more messages.

## Capture behavior

- The receive ring holds 256 messages and records every word, including
  SEPROM pings. If more than 256 arrive, the summary reports the overwritten
  count and the dump contains the newest 256.
- Once `SEPOS BOOTED` is observed, Pongo continues draining until the receive
  FIFO and captured-message count are both unchanged for 3 continuous
  seconds. A 10-second hard bound prevents a chatty SEP from hanging boot.
- Endpoint discovery is accumulated from `ep=0xfd`: opcode 0 supplies the
  endpoint 4CC and opcode 1 supplies the four OOL page counts. Both are keyed
  by the message parameter, which is the endpoint ID.
- The end of capture prints the retained raw messages, then every complete or
  partial discovery record. The raw stream preserves all non-discovery traffic
  that arrived during the boot; the discovery end marker is deliberately the
  final line so the host knows the complete dump has flushed.

The machine-readable discovery row is:

```text
SEP-DISC name=sbio id=NN in_min=N in_max=N out_min=N out_max=N have_name=1 have_ool=1
```

Rows with either `have_*` value zero are intentionally printed but are not
safe to declare in Linux. The terminating marker is:

```text
sep discovery: end entries=N complete=N
```

The full pongoterm transcript is written to `build/sep/pongo-sep.log`; the
discovery rows and capture summary are also copied to
`build/boot/autoboot.log` by the runner.

## Build

```sh
scripts/sep/build-pongo-a9sep.sh
```

The script pins upstream pongoOS commit
`cc532fac72e97f7c7f50f891073fde12f519f0a1`, applies
`patches/sep/pongoos/pongoos-a9-sep.patch`, and installs the ignored build
artifact at `build/prebuilt/pongo/Pongo-a9sep.bin`.

## Next cold-DFU run

With the intended Linux test image selected by `build/boot/default-payload`:

```sh
SEP_BOOT=1 SEP_MODE=xnu scripts/boot/autoboot.sh --once
```

Or select it explicitly:

```sh
SEP_BOOT=1 SEP_MODE=xnu LINUX_A9_PAYLOAD="$PWD/build/m1n1-linux-sep.bin" scripts/boot/autoboot.sh --once
```

Do not run `pongo-sep.sh` again in the same boot. The cold SEP sequence is
one-shot. After Linux starts, copy each complete `SEP-DISC` row into the
driver's manual endpoint-declare debugfs interface, using the syntax exposed
by that test kernel.

## Post-discovery underflow-gated wake probe

Keep the successful boot handshake on its legacy path. The patched `sep stat`
does not read the RX FIFO data window. The runner takes two status snapshots
across a 65-second passive hold and refuses to send if `+0x4020` fault bits
18/19 appear; only a fault-free preflight permits the one wake word:

```sh
scripts/sep/pongo-sep.sh --xnu status split32
scripts/sep/pongo-msg64-wake.sh
```

The script sends two `sep stat` commands first. If both are fault-free and the
AP→SEP FIFO is empty, it sends exactly one `sep msg64 0 0 e 0 0`, then one
final `sep stat`. `drained` means send-status bit 17 returned. `stuck`,
`rx-fault-before`, or `rx-fault-after` all require a cold reset and no retry.
The default `sep xnu` path is also available for a later all-64-bit
boot-handshake run by omitting `split32`.

## Why no Pongo-to-Linux handoff page yet

A reserved-memory handoff would remove manual transcription, but it also
creates an ABI, requires cache-ownership rules, and consumes another physical
reservation that both boot stages must agree on. The printed table is enough
for the first SBIO exchange and leaves the successful root-shmem handoff
untouched. Add a binary handoff only after the discovered table and Linux
manual declaration have been validated on hardware.
