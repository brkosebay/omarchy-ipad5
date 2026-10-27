# AOP stage 3: system bring-up and accelerometer

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 streaming update: the orchestrator's `3a7b3805b3` cold run now
streams `apple-a9-aop-accel` at 10 Hz; see `logs/20261025-2006-*` and the
saved `stage3-calstate-full-tilt-20261025T190705Z` capture. Flat screen-up Z
is approximately -64700 counts. [AOP autorotation](aop-autorotate.md) covers
the existing IIO buffer/proxy ABI, six-pose matrix measurement, repeatable
Omarchy install/test commands and the dormant nextint integration branch.
The historical "pending" cold statements below predate this device result;
the `aa54566a91` warm two-boot result remains unestablished here.

Reload crash and wake experiments, 2026-10-26: opt-in `aop_spi0_reset=1` (replays the
firmware SPI0 controller reset before reloading a live AOP) and `aop_wake_wrapper=1`
(replays the iOS resume wrapper writes before `0x220`) on `a9/aop-wake` `f695e71125`.
Entries and the one-DFU owner test are in
[the last section](#opt-in-spi0-pre-reset-reload-and-resume-wrapper-wake-a9aop-wake).
Not run on hardware yet.

Sleep/wake handoff, 2026-10-26: opt-in `aop_handoff=sleep` on `a9/aop-handoff`
`621fe3f144`, plus a reserved-RAM shutdown transcript and full crashlog dump for
both warm modes. Entries, design, evidence and the exact owner-run test are in
[the last section](#opt-in-sleepwake-handoff-aop_handoffsleep-shutdown-transcript-full-crashlog).
Not run on hardware yet.

Warm-reload implementation, 2026-10-25: opt-in `aop_quiesce=1`, kernel
`aa54566a91516e88eee0d79757bcb5422aa30162` on `a9/aop-stage3`, directly after
`3a7b3805b3`. The full and conservative warm entries below keep the calibration
state fix and all kexecfix/ANS protections. **No device operation was performed;
physical warm-reload success is pending the two-boot test below.** Workspace
changes are uncommitted; only the two owned kernel files were committed.

Host/Docker implementation and calibration-state report-gate fix, 2026-10-25.
The orchestrator's DFU cold `cb891a1db9` run reached `rate-acked`, then
`failed-no-report`: all seven transactions completed, 64/64 pings, no crash.
The replacement below publishes the missing calibration-state property before
rates. Firmware execution and the production-driver replay validate the fix
offline; **the new payloads still need an orchestrator-run DFU cold test**.
This implementation session performed no device operations, workspace commit
or push.

The v6 `2ed` result is the baseline: real ECID, zero NONC, D-cache disabled,
normal IOP mask selection, deferred full firmware release. These entries
repeat that boot and continue in the same attempt. They do not adopt the
stage-2 instance from a previous kernel.

Kernel branch: `a9/aop-stage3`, based on
`c34db1b80e859565c6cdb180b0b177c396b439d1` (`a9/tri49a`). The standalone AOP
base remains `93e851b61436f117899db973e2abe646fa545f29` (`a9/aop`), unchanged.
Commits: `abb3febdf11a20ae82800864a4d7ff3c17049808`,
`6db175435d05fe79c3e966ecfdbe37a61a44b209`, then
`ee163ac12e4c9a541fbdde5b367d69dbb5c22f39` (ping-race fix), then
`f16b07c1d090fe7428ef8385a87b532ef809f10b` (endpoint-enable fix), then
`625e0f0f89f4d194e5c9ccd7ad435cb30a734466` (property-reply fix), then
`cb891a1db9a3c2eaeeea21e0cab65fbcf4ad53a0` (optional calibration), then
`3a7b3805b31c2cba71deeff6b5eff6adf7b245d0` (calibration-state report gate), then
`aa54566a91516e88eee0d79757bcb5422aa30162` (opt-in warm shutdown, HEAD).
Release: `7.3.0-rc1-aop-stage3-gaa54566a9151`.
The final provenance is recorded in `build/periph/aop-stage3/COMMIT`,
`build/kernel-aop-stage3/{COMMIT,RELEASE}` and the payload manifest.

> Private graphics-dependent excerpt omitted from this source snapshot.

## DFU cold rate-ACK/no-report: missing calibration-state gate

The [state](../release/OMISSIONS.md) and
[dmesg](../release/OMISSIONS.md)
come from `build/periph/aop-stage23/stage3-caloptional-full-20261025T182236Z/`.
This is a later failure than the optional-ARot rejection below. Calibration
`1a/0b/19` succeeded; `18` was unsupported and tolerated. Batch/raw/report
`16/5c/0` all succeeded, with values `100000/0/100000` microseconds. The
nonzero report interval is not an off request; raw interval zero is also
what iOS sends when no separate raw stream is requested.

There are **no unread accel entries**: ep33 TX read/write pointers are both
`0x380`; RX read/write pointers are both `0x240`. Eight consumed RX packets
are one service announcement plus seven property replies; the seven doorbells
are seven AP commands. Other initialized report RX rings are drained too.
The history ring on ep37 contains its initial retained entry, belongs to
`mca`, and is not accel's report queue. A lost accel notification or alternate
queue is not needed to explain the captured silence.

Pinned firmware `af5d51a3…047c8` and iOS kernelcache `c6b95fcf…81e0a` show:

- `0100cd44`, reached by property key `0`, updates the caller's interval,
  computes the aggregate rates, starts the sensor through virtual slots
  `+68/+6c/+70`, and programs its rate through `+74`. Accel's combo wrapper
  `0100aab6 -> 0100abe0` merges accel/gyro requirements and calls the hardware
  backend. An extra stream-start property after the rates is unnecessary.
- Accel sample processing `01006d58` calls **`0100d31c`** before making the
  `0xc2` report. That predicate requires active sampling (source+`68`, or
  test mode), then **source+`50` nonzero OR debug override `0106d244` bit0**.
  It also checks startup deadlines and rate policy. With state zero it returns
  false at `0100d336 -> 0100d35e`, before the rate-policy call. The frozen
  HELLO snapshot has both source+`50` and the override bit zero; this is an
  initialization baseline, not a claim to have sampled the failure-time PC.
- Property `0x3d` at `0100ca1e..0100ca30` writes its u32 value directly to
  source+`50`, and marks its stats object dirty. Sending calibration blobs
  and receiving interval ACKs never performs that write.
- iOS `b9d9e4` sends `0x3d` **after** the calibration table: value **1** for
  incomplete calibration, **2** when the completeness callback succeeds.
  `ba0c34` requires low-temperature offset, interrupt calibration, and either
  ordinary or range sensitivity. ARot is not a completeness requirement.
  iOS `b99ef0` then sends the same `16/5c/0` interval sequence. Its power
  callback `b9e6d8` also restores key0 on wake; it adds no new start command.
- `010093d0` builds the motion subpacket and posts through the rate manager;
  `0100b544` wraps the EPIC report for the attached service/AFK source.
  iOS `bacf58` routes category<`0x10` reports through the interface report
  callback; `b9eb5c` dispatches the accel payload. The Linux ep33/channel
  `10000007`/subtype `c2` receive path already matches this route.

The driver now sends and requires the correlated zero-status ACK for
**`0x3d=2` before rates** on this device. It determines completeness from
successfully acknowledged `1a`, `19`, and `0b` or `27`; otherwise it uses
honest incomplete state `1`. Factory bytes and optional-ARot handling stay
intact. `cal_sent` still counts only calibration blobs. Debugfs adds
`cal_state`; a separate `calibration-state-acked` checkpoint identifies the
new operation. Missing, unsupported or mismatched state replies fail configure
before rates/IIO, with no retry. The normal sequence is now **eight** commands:
`1a,0b,18,19,3d=2,16=100000,5c=0,0=100000`.

Reproduce without device access:

```sh
python3 -B scripts/periph/aop/re-stream.py
uv run --with unicorn python -B scripts/periph/aop/re-stream.py --firmware-gate
# Refresh iOS exports if desired:
python3 -B scripts/periph/aop/re-stream.py --refresh
# Separate private firmware project; does not replace the shared iOS server:
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python -B \
  scripts/periph/aop/re-calibration.py --function 0100cd44 --function 0100d31c \
  --function 01006d58 --function 010093d0 --function 0100b544
python3 -B scripts/periph/aop/test-offline.py
```

Pinned hashes, capture fixture, iOS exports and `firmware-gate.json` are under
`build/periph/aop-stage3/re/stream/`; named firmware exports are under
`re/calibration/`. The Unicorn check executes the **actual Thumb setter
`0100c8ac` and predicate `0100d31c`** from the pinned snapshot. It models
active sampling and expired deadlines, and stubs only the final rate-policy
call `0100a8bc` to zero. State0 suppresses reports; real setter `3d=1/2`
opens the gate; `3d=0` closes it. This is instruction execution, not a sensor,
interrupt, timer or hardware-success emulator. The shared iOS function/comment
updates and private firmware names were saved in Ghidra.

The ASan/UBSan replay compares complete latest TX6/7 and injects **verbatim
RX4..7**, preserving all seven measured TX-to-RX delays
`6554584/1817500/27708/1712125/2777500/811792/2749666 ns`.
RX1..3 are outside this capture's ring window: their documented statuses are
modeled. The exact historical seven-command sequence has consumed queues and
no reports for three seconds; a subsequent modeled `3d=2` ACK opens reporting.
The corrected full production continuation reaches state2, first report, IIO
and increasing reports. In that corrected sequence, captured rate frames
need their sequence/tag incremented for the inserted command; its new state
ACK and samples are explicitly modeled. Other calibration/startup regressions
remain covered. New negative cases reject missing/unsupported/wrong-tag state
ACKs before rate setup; rejected sensitivity publishes incomplete state1.

Running the same replay against saved `cb891a1db9` reproduces
`failed-no-report properties=7 state=0 reports=0`
(`offline-stream-before.log`). The fixed suite passes (`offline-stream.log`,
then `offline.log` from the build). This proves the software regression and
firmware gate; only the next DFU run can establish physical streaming.

## DFU cold key 0x18 rejection: root cause and fix

The [cold state](../release/OMISSIONS.md)
and [dmesg excerpt](../release/OMISSIONS.md)
come from `build/periph/aop-stage23/stage3-propertyfix-full-cold-20261025T175311Z/`.
Tags `1,2,3` have subtype `0x84` and raw IOReturn `0,0,0xe00002c7`.
The old shared decoder mapped **every** nonzero IOReturn to `-EREMOTEIO`
(`-121`); that Linux errno does not identify the firmware's error.
`0xe00002c7` is `kIOReturnUnsupported`, not invalid argument or a timeout.
See Apple's [IOReturn definitions](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOReturn.h).

Pinned firmware `af5d51a3…047c8`, with ext `2528d268…b9de22`, establishes:

- The accel service uses the combo accel setter `0100aa40` (vtable
  `010361f0`, property slot `+0x20`). It first calls the combo setter
  `0100a918`; on `e00002c7` it falls back to accel setter `01006a14`,
  then common motion setter `010091ec`.
- `0100a918` handles `0x19` and `0xf0`; all other keys return unsupported.
  `01006a14` has no `0x18` implementation. At `01009208`, the common setter
  compares the key with `0x18` and branches at `0100920a` directly to
  `01009280`, returning the constant at `010092b0`: **`0xe00002c7`**.
  This branch reads no payload, length, rate, enable or calibration state.
  Thus a different layout, send order, delay or post-enable retry cannot
  make ARot supported in this firmware.
- `0x19` must be traced through the combo override, not just the generic
  accel implementation. `0100a918 -> 01070ab8` accepts versions 1–4;
  its size table at `010361d4` requires 16 bytes for version 3. The saved
  interrupt-calibration blob is version 3/16 bytes and satisfies the two
  period-product bounds. Its payload needs no conversion. The generic
  accel handler's different version-3 layout is not the selected path.
- Batch/raw/report keys `16/5c/0` have implementations in `0100c8ac`.
  No extra sensor-enable property is needed before the calibration table.

The shared iOS kernelcache `c6b95fcf…81e0a` independently establishes:

- `b9d9e4` constructs and walks the accel calibration table at `6d5e7f8`.
  `b96b60 -> b96aec -> bad638` sends the complete
  OSData bytes, without truncating/repacking these unflagged entries.
  The private `accel-calibration.bin` generator already matches its order
  and exact device bytes: `1a/20, 0b/36, 18/16, 19/16`; `27` is absent.
- `b96d70` walks to the end even after an error. It explicitly excludes
  `-0x1ffffd39` = `e00002c7` from the aggregate status. `ba0c34` checks
  calibration completeness using offset, sensitivity and interrupt-calibration
  alternatives; **orientation is not required**. `b9d9e4` subsequently sends
  calibration-state `0x3d`. It does not turn an unsupported ARot into an ACK.
- `b99ef0` sends rates in order batch `16`, raw `5c`, report `0`.
  The implementation keeps that order and requires their successful replies.

The fix preserves all factory bytes and transactions. A correlated, well-formed
`e00002c7` reply becomes `-EOPNOTSUPP`; only the calibration loop logs it,
increments `cal_unsupported`, and continues. Other remote errors, malformed,
missing or mismatched replies still fail. Unsupported **rate** properties
also fail. `cal_sent` still counts only successful ACKs. ARot becomes true
only on its zero-status ACK: for this firmware expect **`cal_sent=3
cal_unsupported=1 arot_sent=0`**. IIO is registered with
`firmware_applied_arot=false`, after the first report. No host ARot transform
is added. Its generic existing warning says "ARot not sent"; here that means
not acknowledged/applied. Actual report axes, signs and residual screen matrix
remain subject to physical validation; the previous assumption that sending
ARot proved its application is superseded by this evidence.

Reproduce without device access:

```sh
python3 -B scripts/periph/aop/re-calibration.py
# Refresh iOS exports from the shared server, if desired:
python3 -B scripts/periph/aop/re-calibration.py --refresh
# Re-export/name the firmware handlers in the separate private project:
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python -B \
  scripts/periph/aop/re-calibration.py --firmware
python3 -B scripts/periph/aop/test-offline.py
```

Exports, pinned hashes, instruction windows and the generated private fixture
are under `build/periph/aop-stage3/re/calibration/`. The understood iOS
functions were renamed/commented in shared Ghidra and saved. The actual ring
windows start at TX `0x80` and RX `0x40`; treating them as zero-based would
misidentify the third reply. The replay compares the **complete captured TX2/3**
and injects **verbatim RX1/2/3** with the measured TX-to-RX delays
`5367167/6685667/844458 ns`. It compares all four factory values exactly.
Startup and transactions after captured tag3 remain a modeled firmware peer.
Against the old driver the replay stops at `cal_acked=2 tag=3 last=e00002c7`
(`offline-calibration-before.log`). With this fix it reaches three ACKs plus
one unsupported, rates, first report, IIO and increasing reports
(`offline-calibration.log`, also `offline.log` from the build). Negative
cases include a different error at ARot and unsupported batch rate, plus all
prior tag/channel/subtype/short-reply, ping/watchdog and AFK regressions.
ASan/UBSan, warning-free `W=1`, checkpatch, config/DT/ancestry checks and both
real payload extraction/checksum tests are required by `build-stage3.sh`.

## Opt-in warm reload: quiesce the old firmware before halting it

The three warm failures in `20261025-1815-*`, `1820-*`, and `1825-*` are
distinct from the calibration failures on the DFU boots at `1853` and `1922`.
The first/third warm runs reached system-ready and crashed during application
STARTEPs; the second crashed before readiness. All three accepted RUN clear,
drained, and received fresh HELLO/EPMAP. **All have `stale_outbox=0`**; the
second alone has `stale_inbox=8`. Merely draining more stale output or reverting
the property/calibration fix does not explain or fix this evidence. Their
core-halt-only loader did not shut down RTKit system power or sensor drivers.

The implemented choice is **(a)**, plus the already supported halt/drain part
of **(b)**, under immutable boot/module parameter **`aop_quiesce=1`**, default
false. It requires the existing forced/deferred stage3 `aop_run=1/2` profile.
The loader and default-off behavior remain the baseline. This opt-in must run
in the **outgoing** kernel; booting it after an old AOP READY kernel cannot
retroactively quiesce that instance. Begin this experiment from DFU.

Evidence for the selected sequence:

- [Asahi Linux RTKit shutdown](https://github.com/AsahiLinux/linux/blob/asahi/drivers/soc/apple/rtkit.c)
  and [m1n1 power switching](https://github.com/AsahiLinux/m1n1/blob/main/src/rtkit.c)
  set **AP QUIESCED `0x10` before IOP sleep**, waiting for each ACK. They warn
  against bare AP OFF `0`, which prevents restarting. The local Hoolock
  `idevice` m1n1 has the same AP-before-IOP ordering. These implementations
  use a newer wire envelope; only ordering/state semantics are reused.
- Pinned A9 iOS `b71750` uses exact old-transport type6 words `201/220/202/0`
  for target states 0..3. `b65b54` sends the low-power IOP request, waits for
  its state transition, then reaches the provider stop path. `b71504` also
  supports AP `201` for its own state0, but the experiment selects the
  independently supported AP quiesced `10` to follow the restartable Asahi
  ordering. It does **not** copy iOS's two `201` senders without considering
  the restart requirement, or send bare OFF0.
- Pinned AOP firmware `0103131c` dispatches IOP type6 and AP typeB through
  decoder `010314f4`. **`10 -> internal 20`; `201 -> internal 8`**. Encoder
  `01031408` returns exactly `10/201`. AP ACK builder `01031498` sends typeB;
  IOP builder `01031444` sends type7. The power task `010329a8`, callback
  walker `01032bb6`, and transition `01032c18` walk power clients and reach
  the low-power/ACK path. State8 is a resumable deep-sleep request, distinct
  from wire0/internal1. Actual Thumb codec execution proves these encodings;
  it does not emulate sensor power, callbacks, timers or DMA.
- iOS V2 `b4e7c8` clears wrapper `+44` bit4 and auto-boot; V2 unmap hook
  `b4e7c4` is a return stub. The examined V2 stop/start/hooks and empty J71t
  gate lists establish no stronger reset pulse. Thus **(b)** PMGR/wrapper
  reset and SRAM/BSS scrub are omitted: no AOP-only sequence has been proved,
  and the prior loader already copies the same main/ext regions as iOS.
  **(c)** an iOS panic-recovery reset recipe remains unproved; the normal
  low-power path above is the supported evidence available here. This was a
  bounded investigation, not an exhaustive proof that no reset exists.

The outgoing `.shutdown` now performs these steps when selected:

1. Stop the boot/poll/ping workers and IRQ, invalidate the IIO consumer, keep
   synchronous RX and system logging/IOReport ACKs alive. Inline watchdog pings
   continue during rate/AFK waits and stop before AP power is requested.
2. ACK zero batch/raw/report properties on any discovered accel channel,
   including a partially failed configure. Send AFK `a4` to every initialized
   app and require its stopped state within one shared one-second deadline.
   Suppress late INIT/grant/start work: accel can stream before every other
   source finishes initializing. Enabled sources still awaiting INIT have
   no host grants to release and receive routing-disable only.
3. Send management type5 low32=1 for stopped/still-uninitialized apps, using
   iOS `b717a4`. There is **no invented management stop ACK**. AFK shutdown
   ACKs are the application proof. Keep all ring/ext/system RAM reserved.
4. Drain pending RX, reset AP ACK freshness, send **`00b0000000000010`**,
   require fresh typeB/`10` within one second. Reset IOP state freshness, send
   **`0060000000000201`**, require type7/`201` within one second. Wrong ON
   ACKs and missing replies cannot satisfy either transition.
5. Wait 20 ms, SHA256 all `pool_used` bytes (ext plus every granted system/AFK
   allocation), wait another 20 ms, require identical bytes. This is logged
   evidence of stable granted RAM before halt, **not an independent hardware
   DMA-idle indication**. It does not observe ungranted or peripheral memory.
6. Only after successful power ACKs and stability, disable outbox, mask the
   wrapper's notifications, clear CPU RUN and auto-boot, wait the existing
   10 ms, and drain both FIFOs with their existing 64-word bounds. Log every
   store, stale word and result. No PMGR domain or fault bit is written.

`QUIESCE step=...` logs each operation/endpoint and status. Terminal checkpoints
flush consoles to ramoops. Failure sends no further power request/halt/retry;
RAM is retained. `.shutdown` is void and **cannot veto kexec**, so a failed or
missing shutdown transcript means **do not trigger AOP in the next deferred
kernel**. Recover with DFU. No automatic recovery or adoption is claimed.
`reset_hook=none` remains accurate; the added proof is a firmware power
transition before the supported halt, not a hardware reset.

Reproduce without device access:

```sh
uv run --with capstone --with unicorn python -B scripts/periph/aop/re-warm-reload.py
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test
scripts/periph/aop/build-stage3.sh
```

`re/warm/evidence.json`, SHA-pinned instruction windows, and iOS Ghidra exports
are under `build/periph/aop-stage3/`. Firmware decompilations of the task,
transition and ACK builders are also in `re/calibration/0103*.c` (the private
firmware project; the shared server stays on the iOS kernelcache). Add
`--refresh` to refresh the iOS exports. The production-C replay covers two
modeled full startups/streams/shutdowns, conservative and default-off paths,
late source INIT suppression, rate/AFK/AP/IOP missing or wrong ACKs, crash,
changing granted RAM, halt and drain failure. The real MMIO helpers separately
reject shutdown stores before quiescence and verify exact stop/drain stores.
**Firmware replies and reports in this shutdown replay are modeled.** No
physical warm boot or sensor success follows from these host tests.

New entries, sharing the clean committed kernel:

- FULL: `aop-stage3-warmaccel-aa54566a91`, variable `AOP_WARM_ACCEL_ENTRY`,
  payload `build/periph/aop-stage3/m1n1-aop-stage3-warmaccel-aa54566a91.bin`.
- Conservative: `aop-stage3-warmsys-aa54566a91`, variable `AOP_WARM_SYS_ENTRY`,
  payload `build/periph/aop-stage3/m1n1-aop-stage3-warmsys-aa54566a91.bin`.

`entries.env` adds `AOP_WARM_{ACCEL,SYS}_{ENTRY,PAYLOAD,DIR}` and preserves
every existing `AOP_COLD_*` variable and payload. `manifest-warm.json` records
the new variants separately. Cmdlines are **926/924 bytes** respectively,
including `aop_quiesce=1` and `pcie_apple_h9p.ans_keep_power=1`.

Build verification completed: ASan/UBSan production-helper/shutdown replay,
real Thumb codec execution, W=1 active/IIO objects, checkpatch (zero errors
and warnings), full Image.gz plus 105 DTBs, clean branch/source/config checks,
exact PCIe equality with `a9/kexecfix`, all DT/pool/ramoops invariants, three
real libfdt/pstore recovery fixtures, and both actual payload extraction,
linked opt-in diagnostics, firmware/ECID/calibration and checksum checks.
Generated objects in this worktree were then cleaned. Logs: `offline.log`,
`offline-warm-final.log`, `re-warm-reload.log`, `w1.log`, `checkpatch.log`,
`build.log`, `check-build.log`, `package-warm.log`, `package-warm-test.log`,
`cleanup.log`. The warm payload SHA256 values are:

- FULL: `03df0e914d60c52b9c0ac1e38f75f9d60e210cba18aa94bb5fa45d4a78f7d36f`
- Conservative: `91a7f245f30632498042ed5c57e49b99830672b4483ab13b04ec7185f1f926dd`

### Exact owner-run cold → full → same-entry warm kexec → full test

The builder/sub-agent did **not** execute these commands. Keep the existing
root server and recovery/default entry; the upload below sets no default.
Start from a fresh DFU, directly into the new FULL payload:

```sh
source build/periph/aop-stage3/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_WARM_ACCEL_PAYLOAD"
# Wait for SSH. Install/verify the same four private files on the real root.
(cd build/periph/aop-stage3/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n install -d -m 0755 /lib/firmware/apple/aop'
for file in boot-ecid.bin ipad6faop-20H350.bin ipad6faop_ext-20H350.bin accel-calibration.bin; do
  cat "build/periph/aop-stage3/root-firmware/$file" |
    scripts/omarchy/ssh.sh "sudo -n tee /lib/firmware/apple/aop/$file >/dev/null && sudo -n chmod 0600 /lib/firmware/apple/aop/$file"
done
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
# Stage the identical kernel/DT/initramfs/cmdline for the upcoming warm boot.
scripts/boot/upload-entry.sh "$AOP_WARM_ACCEL_ENTRY" "$AOP_WARM_ACCEL_DIR"
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh stage3-warm-full-cold
# Physically tilt; require fresh changing axes and a second report/liveness capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh stage3-warm-full-cold-tilt
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id' > build/periph/aop-stage3/warm-before-boot-id.txt
```

Proceed only if the first FULL run passes all streaming/calibration/liveness
criteria below. A stopped/no-report first run is useful diagnostic evidence
but does not establish the requested two-full-tests pass. Then:

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_WARM_ACCEL_ENTRY"
# SSH disconnect is expected. Wait for SSH in the NEW deferred kernel.
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline; cat /sys/bus/platform/devices/210800000.aop/cold_release'
# Require a changed boot ID, SAME entry, aop_quiesce=1 and triggered=0.
WARM_EVIDENCE="build/periph/aop-stage3/warm-handoff-$(date -u +%Y%m%dT%H%M%SZ)"
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$WARM_EVIDENCE"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$WARM_EVIDENCE" --entry "$AOP_WARM_ACCEL_ENTRY"
```

Inspect the retained **previous-kernel** console. The checker requires the
matching entry, every sent AFK shutdown's stopped state and routing disable,
AP10 then IOP201 ACKs, equal RAM hashes, successful halt/drain, and
`QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1` followed by
`shutdown-quiesce-complete`. Missing/failed/truncated evidence is not a pass;
do not release the second AOP in that case. If complete:

```sh
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh stage3-warm-full-second
# Physically tilt again and retain this independent 60-second capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh stage3-warm-full-second-tilt
```

Pass requires **both boots** to reach `phase=streaming`, `system_ready=1`,
`iop_crashed=0`, `fault_control=0`, `cal_sent=3 cal_unsupported=1 arot_sent=0`,
`cal_state=2 rate_set=1 iio=1`, with report and ping counters increasing for
60 seconds and fresh changing IIO X/Y/Z on tilt. Startup must have fresh
HELLO/EPMAP, all advertised enables and successful system/AFK grants without
ep1 crashlog, repeated INIT/HELLO, timeout or mailbox fault. A shutdown ACK,
stable pool, second HELLO or system-ready alone does **not** establish warm
reload success. Retain both startup captures and the preceding shutdown
console, including all per-step statuses, pool hashes and drain counts.

Optional isolation experiment: another fresh DFU directly into
`$AOP_WARM_SYS_PAYLOAD`, upload `$AOP_WARM_SYS_ENTRY`/`$AOP_WARM_SYS_DIR`,
trigger/hold with `stage3-warm-sys-cold`, kexec that same conservative entry,
check its retained shutdown transcript, then trigger/hold
`stage3-warm-sys-second`. Require system-ready, all three buffering grants,
increasing pings, no apps/IIO, no crash/fault for both 60-second holds. This
does not substitute for the two FULL tests.

On any failure retain the captures/ep1 buffer and recover via fresh DFU; do
not retry force-cold in the same warm epoch. If a **successful logged quiesce**
still leads to a crash, that falsifies its sufficiency and makes an evidenced
sensor-specific or AOP-domain reset the next investigation. No syscfg, NVRAM,
LLB/iBoot/boot-firmware writes, shared PMGR cycle or QEMU process termination
is part of this test.

## Full-mode false timeout: cause and regression

The [full state](../release/OMISSIONS.md) and
[dmesg excerpt](../release/OMISSIONS.md) show ep1
granted at `0x85800c000/0x1000`, ping 1 sent at 33.378794, ep4 request and
ping 1 ACK received at 33.379335/33.379341, ep4 granted at
`0x85800d000/0x4000`, then `failed-ping-timeout` at 33.379560. Ping 2 was
sent immediately. Ep2 was later granted at `0x858011000/0x1000`; all three
system endpoints stayed healthy. The capture is
`build/periph/aop-stage23/stage3-full-20261025T162915Z/`.

`a9_aop_ping_tick()` sampled `now` **before** taking `aop->lock`. The deferred
boot worker runs on `system_long_wq`, while the background pinger and RX run
on the driver's ordered queue. Boot also services RX and pings inline. A
background tick can therefore sample time, wait for the mutex, and acquire
it after the inline path updates both `ping_sent_ns` and `ping_ack_ns`.
Unsigned `now - ping_sent_ns` and `now - ping_ack_ns` then wrap: the interval
guard allows an early second ping and the watchdog falsely reports 5 seconds
without an ACK. This is a shared-code race, not a full-mode deadline setting;
conservative can also hit it.

The fix acquires the protocol mutex before checking stop/refusal state and
sampling time. The 1-second ping cadence, real 5-second watchdog and continued
management/system servicing after nonfatal accel failures are retained.

[`tests/stage3.c`](../../scripts/periph/aop/tests/stage3.c), run by
`test-offline.py`, compiles the actual management/ping/system/AFK/property
callbacks and the actual full boot continuation. Its deterministic mutex
wait replays the captured ordering and timestamps. Before the fix it aborts
with `failed-ping-timeout ping_seq=2 ping_acks=1`
(`build/periph/aop-stage3/offline-race-before.log`); after the fix it holds at
ping 1, accepts the late ep2 grant and reaches system readiness in both modes.
A fake firmware peer then exercises full-only STARTEP `0x21`, AFK v8,
AOP-initialized rings, the real saved calibration bytes, correlated replies,
batch/raw/report properties, first report and IIO registration. Missing,
mismatched and error property replies, absent calibration, missing reports,
real watchdog expiry and stop/refusal during a mutex wait are also covered.

The later hardware run disproved the earlier audit's assumption that only
accel needs enabling. STARTEP `0050002100000002` **was transmitted** at
29.058592, with `status=0`, before `wait-service`. No AFK INIT followed.
The firmware probed MPU6900, AK8963 and BMP284, then repeatedly logged
`waiting for endpoint enable, _routing=2c`. See the
[state](../release/OMISSIONS.md),
[excerpt](../release/OMISSIONS.md) and
`build/periph/aop-stage23/stage3-pingfix-full-20261025T164218Z/`.

## Routing 0x2c: enable evidence and correction

This is an application **endpoint enable**, sent on management endpoint 0:
`0050002c00000002` (type 5, endpoint `0x2c` in bits47:32, low32 `2`).
It is distinct from the ep4 word `0480000000000000`, which is IOReport
traffic. Moving accel's STARTEP earlier, echoing IOReport, or extending the
service timeout cannot set wakehint's enable bit.

Pinned firmware `af5d51a3…047c8` proves the following:

- `0100f27c` resolves each source's name through the routing table at
  `01052f68` and saves routing at source+`0xa4`. Its entries are
  `accel=0x21`, `wakehint=0x2c`. The table is in the immutable firmware.
- `0100f32a..0100f34e` calls `0102b374` until routing's bit0 is set,
  logging the captured message and sleeping one second on each failed test.
  Only then does it attach the source and enter AFK startup at `0100f5dc`,
  whose state0 sends INIT opcode `0x80`, version8.
- Management RX `0103131c` handles type5/low32=2 by calling `0102b330`,
  which uses the same routing lookup `0102ad10` and ORs bit0 into the endpoint
  state. No sensor property, ring message or special IOReport ACK is needed
  to enable it.
- `01011e74` invokes the source initializer, then waits for source+`0x24`.
  AFK state8 in `0100f5dc` sets that byte after buffer/ring startup. Thus AP
  must also service AFK for preceding sources to reach accel. The frozen
  **HELLO-time** SRAM construction links give the source chain
  `2c,2b,29,20,26,25,21,32,2d,2a,28,31,30,27,24,23,22`.
  This is construction evidence, not a sampled failure-time PC or a capture
  of successful initialization.

The iOS enable path is also explicit: `fffffff005b1fa24`
(`RTBuddyEndpointEventSource_enable`) calls RTBuddy virtual slot `+0x578`
with the endpoint and run=1. The pinned vtable at `fffffff006d4c000` maps that
slot to `b67528`; its gated callback `b675a0` tail-calls `b717a4`, which
constructs exactly the type5/low32=2 STARTEP word. The saved live iOS registry
has `AOPEndpoint13 -> AppleSPU@1000000c -> wakehint`, with IOReport group
`AOP endpoint 44`; `AOPEndpoint2` similarly owns accel on wire33. This is
static iOS path plus saved registry evidence, not a new live iOS trace.

Full mode now sends STARTEP for **every advertised application endpoint**
before entering the service wait, as the iOS endpoint clients do. For this
EPMAP these are `0x20..0x2d,0x30..0x32`. The bounded ascending send order
includes accel then wakehint, without waiting in between. Firmware can then
advance through its own initialization order; RX negotiates each source's
AFK v8, reserved buffers and AOP-owned rings. The service table grows from
16 to 64 slots so other announcements cannot fill it before accel arrives.
Only the discovered accel service receives calibration (including ARot),
batch/raw/report properties or an IIO consumer. Conservative still returns
at system-ready before any application STARTEP.

Reproduce the evidence without device access:

```sh
python3 -B scripts/periph/aop/re-endpoint-enable.py
# Optional fresh iOS decompilation from the shared Ghidra program:
python3 -B scripts/periph/aop/re-endpoint-enable.py --refresh
```

Exports and SHA-pinned assembly are in `build/periph/aop-stage3/re/enable/`.
The iOS enable function was renamed/commented in shared Ghidra and saved.

The production-code offline replay now includes the new capture's system
request words/timestamps, IOReport messages and sensor syslog slots. Its
21-only case receives no INIT or service for five seconds while pings and
`_routing=2c` logs continue. Its firmware peer gates each INIT on that
source's enable and waits for its AFK handshake before advancing. With all
advertised enables, the real continuation reaches calibration, rates,
reports and IIO. A separate case delays accel's announcement until after
the other 16 services, exercising the expanded table. Factory calibration
bytes are compared exactly; ARot and correlated replies remain mandatory.

Running this new peer against the saved pre-fix driver reproduces
`failed-no-accel-service enabled=200000000 current=2c ping_acks=6`
(`offline-enable-before.log`); the replacement passes (`offline-enable.log`).
For a diagnostic replay of another saved driver revision:

```sh
python3 -B scripts/periph/aop/test-offline.py --active-source path/to/saved-driver.c
```

## Property reply 0x84: captured success discarded by the driver

The [third full state](../release/OMISSIONS.md)
and [excerpt](../release/OMISSIONS.md)
show `system_ready=1`, all 17 endpoints running, all 12 announced services,
63/63 ping ACKs, no system/fabric errors, and `failed-configure (-110)`.
The capture is `build/periph/aop-stage23/stage3-enablefix-full-20261025T170134Z/`.
The `spu.cpp:858: AssertMacros: error: 0` syslog did not prevent AFK/service
startup or the successful calibration response.

`rings.txt` decodes unambiguously:

- Ep33 TX entry `+0`: ring length `0x44`, channel `0x10000007`, SPU notify
  category `0x10`, subtype `4`, tag `1`, payload length `28`. Its payload is
  `zero u32 | key 0x1a u32 | 20 factory bytes`, exactly matching the saved
  low-temperature accel offset. TX `rptr=wptr=0x80` proves AOP consumed it.
- Ep33 RX entry `+0x80`: ring length `0x2c`, same channel, category `0x20`,
  subtype `0x84`, tag `1`, four-byte **zero** IOReturn. This is the response
  to the property. Accel reports use `0x00/0xc2`.
- `transcript.txt` index 241 sends ep33 `0xa2` at 15.890041756; indices
  252/253 receive ep33 `0x85` at 15.896013298 followed by ep34 `0x86`.
  Those are RX notification and the last source's START_ACK. Dmesg dispatch
  logs the reply at 15.916032, then times out at 17.909997. Reception and
  dispatch are separate timestamps, not an inferred firmware latency.
- Debugfs retained `last_reply category=0x20 subtype=0x84 tag=0x0001
  size=4 word0=0 seen=1`, but `cmd_status=-110`. Both the receive dispatch
  and shared reply decoder incorrectly required subtype `4`. `cal_sent`
  increments only after an ACK is accepted, explaining its zero value.

Pinned firmware `01011d5a..01011d74` loads the request subtype and original
u16 transaction tag, ORs subtype with `0x80` at `01011d6c`, sets category
selector 2, and calls `010090a6`. That builder writes `selector << 4` to
category `+5`, subtype at `+6` and tag at `+8`. Property request `4`
therefore receives reply `0x20/0x84`, including calibration and rate commands.

In the shared iOS kernelcache, `AppleSPUInterface_dispatchSubpacket`
(`fffffff005bacf58`) routes reply-category packets to `bad150`, now named
`AppleSPUInterface_completeTaggedReply`. It finds the outstanding u16 tag,
copies the reply and wakes the command gate; it does not require reply
subtype to equal request subtype. `bae344` registers that waiter before
sending. `bae168` reads the leading IOReturn after completion. `bad898`
sends key/value using notify subtype4; `b9d9e4` / `b96b60` send the ADT
calibration entries through that setter, including ARot as key `0x18`.
The request encoding was already correct. This is static iOS/FW evidence
corroborated by the saved Linux capture, not a fresh iOS trace.

The fix accepts only `0x20/0x84` replies matching the pending tag **and**
discovered accel endpoint/channel. Short/error replies terminate configure
with their errors; unrelated replies cannot advance it. Queue and completion
logs now show channel, tag, key/size and completion status; `cal_sent`
continues to count acknowledged blobs. No extra status command, delay or
retry is introduced. The shared protocol header change also ships in the
replacement conservative payload.

Reproduce the decoded evidence without device access:

```sh
python3 -B scripts/periph/aop/re-property-reply.py
# Optional fresh shared iOS decompilation:
python3 -B scripts/periph/aop/re-property-reply.py --refresh
```

SHA-pinned assembly, Ghidra exports, decoded metadata and the generated
private fixture are in `build/periph/aop-stage3/re/property/`. The understood
iOS handler was renamed/commented in shared Ghidra and saved.

The ASan/UBSan production-code replay imports the capture's exact AFK receive
words/timestamps, variable-sized grants/descriptors and ring entries. It
discovers all 12 real services on their actual channels, replays the
orientation message and late ep34 START_ACK, and compares the complete
first production TX entry against the capture, including the 20 private
factory bytes. It publishes the captured zero-status reply only on the
second ep33 RX notification. The old `f16b07c1d0` driver reproduces
`failed-configure cal_acked=0 tag=1 last_reply=20/84/1/00000000`
(`offline-property-before.log`). That historical peer replay proceeded through four
calibration ACKs (`1a/20 B`, `0b/36 B`, `18/16 B`, `19/16 B`), the three
rate properties, first report, IIO registration and streaming
(`offline-property.log`). Replies **after** the captured first success and
motion samples were modeled peer behavior, not captured hardware successes.
The later DFU capture and optional-calibration replay above supersede its
modeled zero-status ARot reply.
The earlier peer's fabricated reply subtype4 is corrected to `0x84`.
Wrong channel, tag or subtype cannot advance configure; truncated replies,
nonzero IOReturn, missing replies/calibration and absent reports fail with
their expected errors. Earlier ping-race/watchdog and endpoint-enable
regressions remain covered.

## Cold baseline variants and shared checkpoints

Both use `apple_a9_aop.attach_stage=3 aop_force_cold=1 aop_cold_test=5
 aop_defer=1`, keep `pcie_apple_h9p.ans_keep_power=1`, and have 910/912-byte
cmdlines. Probe performs no stop/load/release; sysfs permits one trigger.

Replacement entries (retain all earlier fixes and the calibration-state gate):

- `aop-stage3-coldsys-3a7b3805b3`:
  `build/periph/aop-stage3/m1n1-aop-stage3-coldsys-3a7b3805b3.bin`
- `aop-stage3-coldaccel-3a7b3805b3`:
  `build/periph/aop-stage3/m1n1-aop-stage3-coldaccel-3a7b3805b3.bin`

`build/periph/aop-stage3/entries.env` selects these entries. Each has its
extracted kernel, DTB, initramfs, cmdline, provenance and `SHA256SUMS` in the
same-named subdirectory. The older `6db175435d`, `ee163ac12e` and
`f16b07c1d0`, `625e0f0f89` and `cb891a1db9` payloads are historical artifacts; use the new
variables for testing. They require independent DFU boots, not warm kexec.

- **CONSERVATIVE**, `aop_run=1`, `AOP_COLD_SYS_ENTRY`: start advertised
  system endpoints 1/crashlog, 2/syslog, 4/IOReport. Answer their buffer
  requests from the reserved pool, send AP readiness, request IOP state
  `0x220`, notify AP state `0x20`, and wait for IOP/AP acknowledgements and
  a buffer grant for each advertised supported buffering endpoint. Stop
  initialization at `phase=system-ready result=cold-system-ready`. RX and
  watchdog pings continue. No application STARTEP, AFK or accel properties.
- **FULL**, `aop_run=2`, `AOP_COLD_ACCEL_ENTRY`: identical system stage,
  then STARTEP every advertised app endpoint, negotiate SPU/AFK v8 for each,
  supply reserved buffers,
  attach AOP-owned rings, receive `accel`/`0x10000007`, send all available
  real calibration blobs and require each correlated property reply. Log and
  tolerate `kIOReturnUnsupported` only for calibration; all other errors fail.
  Publish and ACK calibration-state `3d=2`, then set batch/raw/report to `100000/0/100000` us and require the first report
  before registering the existing `apple-a9-aop-accel` IIO consumer.

Every new grant shares the existing monotonically increasing allocator with
ext. Ext occupies the first aligned 48 KiB; system and AFK grants follow,
without overlap, inside `0x858000000..0x8583fffff`. Nothing is returned to
Linux allocation or reused after an error. Unknown firmware PAs are never
mapped. Oversize/duplicate/malformed grants fail initialization with evidence.

`STAGE3 checkpoint=...` is printed to dmesg and the continuous ramoops
console; each checkpoint flushes consoles and drains WC stores. A flush timeout
is printed explicitly. Startup and register traces remain bounded; wrapping
transcript/counters remain available after the startup log budget is full.
The existing pre-RUN hard checkpoint/fence and mailbox fault rules remain.
Known crash messages, repeated HELLO, duplicate EPMAP and repeated AFK INIT
halt further traffic. Accel/configuration failures leave management ping and
system servicing active when no fatal/fabric condition exists; IIO is invalidated.

Milestones, in order (buffer requests can interleave):

1. Existing `COLD checkpoint=before-stop`, `before-run`,
   `run-written-before-autoboot`, `autoboot-written`; HELLO proto 12, EPMAP.
2. `STAGE3 checkpoint=handshake-complete`: fresh HELLO/EPMAP, no app start.
3. `STAGE3 checkpoint=system-endpoints-started`: system STARTEPs/readiness
   and power messages sent. Check their `status=0`; this alone is not readiness.
4. `SYSTEM grant ep=1|2|4 ... tx=0` and
   `STAGE3 checkpoint=system-buffer-grant`: a bounded reserved PA was published.
5. `SYSTEM IOP power ack state=0x20`, `SYSTEM AP power ack state=0x20`,
   `SYSTEM ready ...`, `STAGE3 checkpoint=system-ready`: power and all
   expected system grants completed. Conservative initialization ends here.
6. Full only: `before-accel-startep`, management STARTEPs for all 17 apps,
   `application-endpoints-enabled`, `afk-init-v8` (first expected on `0x2c`),
   per-source AFK grants/ring/start checkpoints, then `AFK grant ep=0x21`,
   `afk-rings-ready`, `afk-start-ack`, `ACCEL service ep=0x21 ... id=10000007`,
   `accel-service-ready`. The service must arrive on the negotiated ring.
7. A correlated reply for each available blob and
   `STAGE3 checkpoint=calibration-acked`. This device has keys
   `0x1a`, `0x0b`, `0x18`, `0x19`; the absent range-sensitivity blob is skipped.
   Each queue log must identify ep `0x21`, channel `0x10000007` and a new tag;
   each completion must be subtype `0x84`, matching tag/channel. Expect
   `ACCEL calibration ack` for `1a/0b/19`, and
   `ACCEL calibration unsupported key=0x18 ... ioreturn=0xe00002c7`.
   `calibration-acked` means all supported blobs acknowledged, with every
   unsupported blob counted and logged; it does not claim ARot application.
8. The preceding calibration step must include
   `ACCEL calibration state ack key=0x3d value=2` and
   `calibration-state-acked`, before `calibration-acked`. Then require
   `rate-acked`, `first-report`, `iio-registered`, then
   `accelerometer streaming at 100000 us, calibration blobs 3 (ARot not acknowledged; validate report frame)`.

For the configure regression specifically, require calibration replies in
order `1a/0,0b/0,18/e00002c7,19/0`, then successful ACKs for `3d=2,16,5c,0`, with
eight completed property transactions (seven successes and one unsupported)
and no `unhandled ... category=20 subtype=0084` on accel.
Final state must show `cal_sent=3 cal_unsupported=1 arot_sent=0 rate_set=1 iio=1`,
`cal_state=2`, `command tag=0x0008 pending=0 status=0`, `doorbells=8`,
and reports increasing over the 60-second hold.
The previous endpoint-enable and ping-race pass predicates below still apply.

Conservative pass: after a 60-second hold, still `phase=system-ready`,
`system_ready=1`, `system_grants=3` for the v6 map, zero system errors,
`iop_crashed=0`, `fault_control=0`, increasing `ping_acks` between
`ready-state.txt` and `state.txt`, no application endpoint initialized, no
IIO accel. This tests sustained system bring-up independently of accel.

Full pass: same system/liveness predicates plus `phase=streaming`, three
calibration ACKs and logged optional ARot rejection, valid TX/RX rings,
`cal_state=2`, increasing `reports`, IIO
`name=apple-a9-aop-accel`, fresh changing X/Y/Z after tilt. HELLO, buffers or
`iio-registered` alone do not prove useful motion. Flat screen-up gravity and
four physical orientations still need checking: the existing residual mount
matrix remains identity until measured. The IIO scale remains the prior
16.16-g interpretation; physical validation is required.

For this regression, verify ep1/ep4/ep2 grants all have `tx=0`, ep2 is not
blocked by waiting for accel, and `system-ready` precedes
`before-accel-startep`. A ping ACK interleaved with a grant must not produce
an immediate second ping or `failed-ping-timeout`. Full must advance beyond
the captured `wait-service`/wakehint enable stall through `calibration-acked`,
`rate-acked`, `first-report`, `iio-registered`. Compare `ready-state.txt` to
`state.txt` after the 60-second hold: ping ACKs and full-mode reports must
increase, with no system/report errors, fault, crash, repeated handshake or
AFK restart. Specifically require successful management TX
`0050002c00000002`, followed by RX `2c80000000000008`, AFK progress for
preceding sources, then accel RX `2180000000000008`. The old repeating
`_routing=2c` wait must stop. A different routing wait, INIT without ring
progress, pool exhaustion or table saturation is a failure with useful
next-stage evidence. All grants must remain non-overlapping and within the
4 MiB pool. Tilt and fresh changing IIO axes remain mandatory for full pass.

Failure classification: `failed-system-bringup` after 20 s means missing
power ACK or expected system buffer (the per-endpoint state identifies it);
`failed-system-buffer-*` identifies malformed/exhausted/TX buffer work;
`failed-no-accel-service`, `failed-afk-*`, `failed-ring-*`, `failed-configure`,
`failed-no-report`, `failed-iio-register` identify later stages.
`failed-rtkit-crashlog`, `failed-mailbox-fault`, `failed-ping-timeout`, a
restarted management/AFK transcript, AP loss or a frozen/dead stream fails the
run regardless of earlier milestones. Preserve `system_buffers.txt` and
pstore. A failed accel stage with continuing pings is useful system evidence,
but is not a full pass. No automatic retry, reset or clock experiment is added.

## Protocol evidence

Shared Ghidra target: iOS 16.7.10/20H350 J71t kernelcache
`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`.
Exports: `build/periph/aop-stage3/re/`. Reproduce without device access:

```sh
python3 -B scripts/periph/aop/re-stage3.py
# Re-query the running shared database if desired:
python3 -B scripts/periph/aop/re-stage3.py --refresh
```

- `fffffff005b661e4`: system identities 1, 2, 4; application endpoints
  begin at `0x20`. `b71afc` creates them during EPMAP; `b717a4` builds STARTEP.
- `b716d0`: type A/low1 is AP readiness, distinct from the power notification.
  `b71504` sends type B/`0x20` for AP-on and waits for type B response.
  `b71750` selects the IOP type-6 state table; `0x220` is on, type-7/`0x20`
  reports IOP-on. RX power acknowledgements are recorded, not echoed.
- `b767f8`, `b6b268`, and raw crash handler `b6d5f8`: buffer opcode
  bits55:54, size-unit bits53:52, count bits51:44, PA bits43:0.
  Bit52 selects 4 KiB, otherwise bit53 selects 1 MiB; both bits select 4 KiB
  due to instruction order. Replies use type1 or type2 and exact size count.
  The draft's assumption that every request/reply is type1 was corrected.
- `b765e4`: syslog init count and length are 24 bits, not 8; log index is
  low32. `b7698c` uses stride `32 + message_size`. Linux bounds the entire
  slot, truncates displayed text, and ACKs type5/low32 even for a bad slot.
- `b6b268`: IOReport type4 and type12 are completion notifications.
  Type8 receives a bare type8 ACK; echoing type12 would risk an ACK loop.
- Raw `b6d5f8`: with an existing crash mapping, any subsequent message
  indicates crash; without it, opcode1 is a crash/data indication too.
  The draft's type1-only crash test was corrected. Crash data stays in the
  reserved grant and is exposed through debugfs, never an arbitrary mapping.
- Saved `build/firmware/private/IOService.txt` has `AOPEndpoint2` as accel's
  parent (`1636..1660`). `b6a914` subtracts `0x1f` before formatting the
  endpoint name, proving wire `0x21`. Existing AFK v8 implementation and
  its previously corrected AOP-owned ring contract are reused.

Seven understood RTBuddy functions were renamed in shared Ghidra and saved.
No shared server replacement or firmware-source mutation occurred.

## Build and offline validation

```sh
scripts/periph/aop/build-stage3.sh
```

This resumes only the dedicated branch's three owned source files (active
driver, system header and shared protocol header); unrelated
kernel edits are refused. It runs the ASan/UBSan production-helper suite,
W=1 active/IIO objects, checkpatch (new-header advisory suppressed because
MAINTAINERS already covers `drivers/soc/apple/a9-aop*`), a full Image.gz/DTBs
build, clean commit/config/source/linked-symbol checks, runtime-reservation
DT checks and real libfdt/ramoops recovery fixtures. It then extracts both
actual payloads, compares entry bytes/checksums, validates all required
firmware and checks the sub-1000-byte cmdlines. Logs and `stage3.patch` are
under `build/periph/aop-stage3/`. The builder then runs `make clean` only
in its new worktree to reclaim generated objects; exported payloads, Image.gz,
DTBs, config and provenance remain. No hardware success is asserted.

The `3a7b3805b3` rebuild completed successfully: ASan/UBSan offline suite
(latest exact seven-command silence, corrected state-gated continuation,
all earlier captured regressions and strict reply failures), W=1 active/IIO
objects, checkpatch (0 errors/warnings), full Image.gz and 105 DTBs,
clean commit/source/config and linked-symbol checks, three real pstore/libfdt
recovery fixtures, and both real payload extraction/checksum/firmware/cmdline
checks. The actual Thumb setter/predicate execution also passed separately.
Both entries retain `ans_keep_power=1`, kexecfix ancestry and exact PCIe source,
unchanged config/DTBs, and 910/912-byte cmdlines. Generated objects in this
branch's worktree were cleaned; all exports remain. Hardware execution of
this revision is pending. Logs: `offline.log`, `offline-stream.log`,
`offline-stream-before.log`, `w1.log`, `checkpatch.log`, `build.log`,
`check-build.log`, `package.log`, `package-test.log`, `cleanup.log` under
`build/periph/aop-stage3/`. `re/stream/firmware-gate.json` records the
instruction-execution result; the old failure log records seven commands,
state0 and zero reports. No workspace files were committed or pushed.

Both variants use the same committed kernel; conservative never enters the
calibration path. Payload SHA256:

- FULL: `5241c7c25a6ef86012711cff1b6e5dd93b3d16a46c7c9bab023a5182ac586efe`
- Conservative: `edb803c091b29eeb1983579b35e420c0e8be56ba541810132c5e2ffec83f2e3e`

## Historical calibration-state-only commands — AOP_COLD entries

The preferred next experiment is the `AOP_WARM_ACCEL` two-boot sequence above.
The commands in this section remain the independent DFU-only baseline for
the preserved `3a7b3805b3` payloads, whose outgoing shutdown has no power
quiesce. They were not executed by the builder.

Before either trigger, install **all four files** from
`build/periph/aop-stage3/root-firmware/` into the real device root at
`/lib/firmware/apple/aop/`, mode 0600:
`boot-ecid.bin` (already installed by v6; verify it matches),
`ipad6faop-20H350.bin`, `ipad6faop_ext-20H350.bin`, and
`accel-calibration.bin` (generated from the real saved IODeviceTree).
The initramfs includes the same bytes, but does not replace this requirement
after switch_root. Installation commands, when SSH is up:

```sh
(cd build/periph/aop-stage3/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n install -d -m 0755 /lib/firmware/apple/aop'
for file in boot-ecid.bin ipad6faop-20H350.bin ipad6faop_ext-20H350.bin accel-calibration.bin; do
  cat "build/periph/aop-stage3/root-firmware/$file" |
    scripts/omarchy/ssh.sh "sudo -n tee /lib/firmware/apple/aop/$file >/dev/null && sudo -n chmod 0600 /lib/firmware/apple/aop/$file"
done
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
```

The next preferred test is **FULL after a fresh DFU**, booting the payload
directly. The current full calibration file remains byte-identical; root
firmware installation/verification above can be performed once SSH is up,
before the deferred trigger. Do not warm-kexec either entry after AOP READY.

```sh
source build/periph/aop-stage3/entries.env
scripts/boot.sh pongo   # orchestrator puts the iPad in DFU; reach pongoOS
scripts/boot.sh linux "$AOP_COLD_ACCEL_PAYLOAD"
# Wait for SSH; run the four-file install/sha256 verification above.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh stage3-calstate-full

# Additional read-only capture, with tilt between captures if streaming:
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh stage3-calstate-full-tilt

# Optional independent conservative test: another fresh DFU, not warm kexec.
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_COLD_SYS_PAYLOAD"
# Wait for SSH; verify the installed firmware as above.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh stage3-calstate-conservative
```

`trigger-cold.sh` verifies the deferred entry, RAM backend, reservations and
live host dmesg witness before the single sysfs write. The hold keeps that
witness running for 60 seconds after initialization finishes. Captures default
to `build/periph/aop-stage23/<label>-<UTC>/` and include before/after state,
system buffers, transcript/rings/raw samples, IIO and dmesg.

For auto-rotation validation, once IIO is streaming, the existing userspace
path can be exercised with `ipad-autorotate --list`, `monitor-sensor --accel`
and `scripts/omarchy/test-autorotate.sh --hardware` (see tablet-usage.md).
Do not infer screen orientation correctness solely from a registered IIO node.

After a failed run and recovery, collect retained consoles without replay:

```sh
scripts/boot/collect-hang-evidence.sh --transport ssh --out build/aop-stage3-recovery
```

RAM-only logs may be lost by force restart/DFU. Keep the existing root server
and recovery entry. No syscfg, NVRAM, LLB/iBoot or boot-firmware writes are
part of these procedures.

## 2026-10-26 warm attempt from an unquiesced AOP (owner-requested)

> Private graphics-dependent excerpt omitted from this source snapshot.

Reading: halt + reload over a live, unquiesced AOP crashes deterministically at
application STARTEP. This does not test the two untried warm paths: (1) quiesce in
the outgoing kernel and then reload (`aop_quiesce=1`); (2) quiesce to IOP sleep `201`
**without** halting, then wake (`220`) and re-HELLO in the next kernel without a
firmware reload. Path 2 is how Asahi hands RTKit coprocessors across m1n1 → Linux.
Both need one healthy AOP instance first, which currently only a DFU cold boot provides.

### 2026-10-26 DFU session: cold stream, six poses, one quiesce warm hop

- **Cold (DFU → `aop-stage3-warmaccel-aa54566a91`):** streaming, with 648 → 4278
  reports, 0 errors and 420/420 pings. Tilt moves all three axes. Six-pose matrix
  measured: `1,0,0;0,1,0;0,0,-1`, worst error 1.42 m/s²
  (`build/periph/aop-autorotate/poses-20261026T111823Z-manual/matrix.json`).
  Auto-rotate is installed and follows the sensor through iio-sensor-proxy.
- **Warm hop** (`linux-a9-kexec reboot` into the same entry, so the outgoing kernel had
  `aop_quiesce=1`): `check-warm-shutdown.py` found **no shutdown transcript**. The
  ramoops console of the previous boot ends at ~9 s (3104 lines), so the quiesce
  `.shutdown` output was never captured. Whether it ran is unknown. Triggered anyway
  (owner wants empirical tries): halt accepted, drained, stale 0/0, HELLO/EPMAP, then
  **RTKit crashlog at `system-buffer-grant`** (`failed-rtkit-crashlog -5`, before
  system_ready). That is earlier than the unquiesced-reload crash, which was at accel
  STARTEP. The crash dump is again capped at 256 bytes (header and version string, plus an `AgrC`
  record at +0xc0). Logs: `logs/aop-warm-20261026/quiesce-hop/`.
- **To fix before the next warm test:** (1) keep the ramoops console capturing through
  shutdown, since it stops at ~9 s; (2) dump the full RTKit crashlog instead of 256 bytes;
  (3) then retest quiesce-reload and the sleep/wake handoff (`aop_handoff=sleep`,
  in progress).

## Opt-in sleep/wake handoff (`aop_handoff=sleep`), shutdown transcript, full crashlog

Kernel `621fe3f144debd33b4f5dc3bc740207279892644` on new branch `a9/aop-handoff`:
`4d9a18367e` (handoff, transcript, crashlog) plus fix `621fe3f144` (resume-path
watchdog priming, image hash before the tag table), both on top of `aa54566a91`
(so it keeps the full `3a7b3805b3` calibration gate,
the `aop_quiesce=1` path and every kexecfix/ANS commit; PCIe source equals
`a9/kexecfix`). Release `7.3.0-rc1-aop-handoff-g621fe3f144de`. Only
`apple-a9-aop-active.c` and the new `a9-aop-handoff.h` differ. Config, DTBs, pool
and ramoops reservations are byte-identical to tri49a. **No device operation was
performed; nothing below is a hardware result.**

### What it does

**Outgoing kernel (`.shutdown`, `aop_handoff=sleep`):** the exact `aop_quiesce=1`
sequence (rates 0, AFK `a4` and stopped state, routing disable, AP `10`, IOP `201`,
two-window stable-pool SHA-256), then **no RUN clear, no halt, no drain**. Instead
it requires an idle mailbox (both FIFOs empty, no faults, outbox enabled, auto-boot 1)
and writes a versioned record to the pool's last 16 KiB page (`0x8583fc000`). The
record is VALID only if all 11 handoff steps succeeded (`steps=0x7ff`): ACK values,
idle wrapper, layout and digest are re-checked by the same validator the next
kernel runs. Otherwise it is written as FAILED with the failing step and errno.
Contents: generation (hop count), writer id, entry name, SRAM image fingerprint
(SHA-256 of the patched image `[0, 0x389b8)`: boot and text, 0 changed words
from load through HELLO in every capture. The 0x1df-byte tag table is excluded.
Its only literal reference, `0x1000b5c`, loads STKG into the guard `0x01047474`,
and the Ghidra xref scan finds no direct writes. A computed-pointer tag update
must still never refuse a hop.), pool SHA-256 over `[0, pool_used)`, ext
offset/size, the three system grants with syslog geometry, every AFK grant
(ep, tag, offset, size), the EPMAP bitmap, HELLO range, wrapper status, step bits.

**Incoming kernel:** probe reads the tail page. A record counts only if auto-boot
reads 1 (a live instance): every warm-kexec capture shows 1, DFU shows 0. After DFU
a record that survived in DRAM is reported as `stale-after-cold-boot` and ignored, so
DFU is always a clean first cold start (the cold load clears the record). With a
live record the driver queues one automatic attempt: wait (bounded, 180 s) for the
ramoops console and `accel-calibration.bin`, then validate digest, state, layout,
ACKs, pool hash, SRAM hash (read under the PMGR guard) and wrapper state. Any
mismatch is reported (`HANDOFF refused reason=...`, sysfs `handoff`) and nothing is
sent. A valid record is **consumed before the wake** (state CONSUMED, consumer id),
then the driver sends exactly one `0x0060000000000220` and classifies the first reply:

- **HELLO** → fresh handshake: HELLO reply, EPMAP, then the proven cold
  `system_start` (system STARTEPs, AP ready, IOP `220` (a no-op), AP `20`).
- **anything else** (expected: type 7 `0x20`) → in-place resume: the certified
  session is adopted (EPMAP bitmap, endpoints, the three system buffers and syslog
  geometry from the record), only AP `0x00b0000000000020` is sent and ACKed.
- **nothing in 20 s** → `failed-handoff-no-wake-reply`.

Both paths then run the unchanged cold continuation: STARTEP for all 17 apps, AFK
INIT, calibration (3 ACKs, `0x18` unsupported, `0x3d=2`), rates, first report, IIO.
A record in any state blocks `cold_release` (-EBUSY), and with no record a live
auto-boot=1 instance refuses the cold trigger (`refused-handoff-live-aop-without-record`,
the 2026-10-26 4/4 crash condition). One attempt, **no fallback reload in the same boot**.

**Grants, decided from iOS `RTBuddySyslog_allocateBuffer` (`b767f8`):** a system
buffer request carrying a nonzero PA maps that existing buffer and sends no reply;
the driver accepts it only if it equals the recorded PA and size. A PA-0 request
re-grants the recorded buffer (if it fits) at the recorded address, with a reply,
like iOS's reuse. Anything else fails (`failed-handoff-system-buffer`). The crashlog
buffer is mapped only after the firmware's request on the HELLO path (any ep1 word
after mapping means crash), but immediately on the resume path. AFK buffers are
granted fresh to the firmware, which re-INITs after its own `a4`/`87` stop: a request
matching a recorded (ep, tag, size) gets that region back, zeroed. This bounds pool
use across hops (a full start uses `0x56000`..`0x186000` of the 4 MiB pool) and
replays no ring state. The tail page is excluded from the allocator at probe, so all
cold offsets (ext 0, grants `0xc000/0xd000/0x11000`, AFK from `0x12000`) are unchanged.

### Wake word and the headline risk

Evidence (`scripts/periph/aop/re-handoff.py`, `build/periph/aop-handoff/re/evidence.json`):

- Real Thumb execution of the pinned decoder/encoder: `220 → internal 0x21 → ACK 0x20`,
  `201 → 8 → 201`, `10 → 0x20 → 10`. The decoder ignores bit 9 for low byte `0x20`,
  so this firmware has no separate "INIT" state.
- iOS resume (`b65b54` case 1): provider `start(fw, iopStatus==3)`, status 4,
  `RTBuddyManagement_sendWake220` (`b71fc4`, exactly `0x0060000000000220`), then
  `waitForBootState` waits for status 8. HELLO is accepted only in status 4 (`b7196c`);
  a bare type 7 `0x20` sets status 8 directly (`b71018`). iOS sleep (case 0/2) sends
  `201/202` and then **stops the provider (a RUN cycle)**.
- Firmware `rtk_power_transition` (`01032c18`) for target 8: ACK `201` (`01032d84`),
  WFI (`01032d98`), on wake current=8/target=previous, power-up callbacks (`01032e2e`),
  then ACK of the restored state, `0x20` (`01032e64`). There is no HELLO sender in
  that path. A callback-sent HELLO would precede the ACK, hence first-word
  classification.

**A fresh HELLO after `220` without a RUN cycle is not firmware-evidenced.** The
firmware evidence points to the in-place resume path. Whether the woken firmware
really services the inbox, keeps its sensor drivers and restarts the apps after
STARTEP is what this test decides. Shared Ghidra: `fffffff005b71e08` renamed
`RTBuddyManagement_epmapRepeatRunningBitmap`, comment at `b65b54`, saved.

### Shutdown transcript that survives the kexec (both warm modes)

The quiesce-hop console stopped at ~9 s because it ends exactly at systemd's
"Apply Kernel Variables": a sysctl lowers `kernel.printk`, so `.shutdown`'s INFO
lines never reach ramoops (the 1 MiB console zone was not full). `ignore_loglevel`
was **not** used: it would push the per-word stage-3 trace through fbcon
synchronously during the timing-sensitive trigger, which the proven cold runs never
did. Instead, with `aop_quiesce=1` or `aop_handoff=sleep`, `.shutdown` appends every
`SHUTDOWN`/`QUIESCE`/`HANDOFF`/checkpoint line to the tail page at `+0x800`
(checksummed, ~14 KiB, written line by line), independent of console levels. The
next probe of either mode replays it once as `AOP-PREV ...` dmesg lines, keeps it in
debugfs `previous_shutdown`, then invalidates it. The record also carries
`steps`/`failed_step`/`failed_errno`. The writer id is printed at probe
(`SHUTDOWN-LOG writer=`) and inside the record, for continuity checks.
`check-warm-shutdown.py` and `check-handoff.py` accept consoles, dmesg with
`AOP-PREV` lines or `previous_shutdown`. **Never run `dmesg -n 8` on the iPad**
(2026-10-26: before the kexec it broke the 17:35 quiesce; in a running kernel at 19:38 the
console flood starved IRQs into NVMe completion-polled timeouts). The reserved transcript
needs no console level.

### Full crashlog

On a crashlog word the driver freezes the whole buffer (debugfs `crashlog`, raw),
lists every section and prints all used bytes, not 256. `capture-stage23.sh` now
saves `crashlog.bin` and decodes it to `crashlog.txt`, along with
`previous_shutdown.txt` and `handoff.txt` (sysfs). `decode-crashlog.py` also reads old
dmesg dumps. On the quiesce-hop dump it shows header `CLHE` v`0x301` with
`0x650` bytes used, of which only `0x100` were printed: `Cver` (RTKit-2062.142.1,
the pinned UUID), `Ctim`, and a truncated `CrgA` register section with
`0x010427f8`, `0x01047e78` and `0x01046f98` in the image's data span. The rest
(about 1.3 KiB) was never logged.

### Entries (`source build/periph/aop-handoff/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-handoff-accel-621fe3f144` | `AOP_HANDOFF_ACCEL` | FULL: `aop_run=2 aop_handoff=sleep` |
| `aop-handoff-sys-621fe3f144` | `AOP_HANDOFF_SYS` | conservative: `aop_run=1 aop_handoff=sleep` |
| `aop-quiesce-accel-621fe3f144` | `AOP_QUIESCE_ACCEL` | optional quiesce-reload retest: `aop_run=2 aop_quiesce=1` with the transcript/crashlog fixes |

Payloads: `build/periph/aop-handoff/m1n1-<entry>.bin`; entry dirs
`build/periph/aop-handoff/<entry>/`. SHA256 values, cmdline sizes and build/test logs
are listed under Build below. All three keep exactly one
`pcie_apple_h9p.ans_keep_power=1`, debug info is off, and the cmdlines are under 1000 bytes.

### Exact owner-run test: DFU → FULL → handoff hop → second hop

The builder ran none of these. Keep the root server and default entry; nothing
sets a default. Start from a fresh DFU:

```sh
source build/periph/aop-handoff/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_HANDOFF_ACCEL_PAYLOAD"
# Wait for SSH. The four private files are byte-identical to aop-stage3's; verify.
(cd build/periph/aop-handoff/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
# (If any differs or is missing, install all four exactly as in the stage-3 section.)
scripts/omarchy/ssh.sh 'cat /sys/bus/platform/devices/210800000.aop/handoff; sudo -n dmesg | grep -E "HANDOFF (mode|probe)|SHUTDOWN-LOG|AOP-PREV shutdown"'
# Expect record=missing (reason=missing, or reason=wrapper result=stale-after-cold-boot),
# "HANDOFF probe magic=. auto_boot=00000000".
scripts/boot/upload-entry.sh "$AOP_HANDOFF_ACCEL_ENTRY" "$AOP_HANDOFF_ACCEL_DIR"
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh handoff-full-cold
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh handoff-full-cold-tilt
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; sudo -n dmesg | grep "SHUTDOWN-LOG writer="' \
    > build/periph/aop-handoff/hop0-identity.txt
```

Proceed only if the cold run passes the stage-3 criteria (streaming, `cal_sent=3
cal_unsupported=1 cal_state=2`, IIO, tilt). Then hop 1:

```sh
# (no dmesg -n 8: never raise the console level, see the warning above)
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_HANDOFF_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel. Do NOT trigger cold_release.
HOP=build/periph/aop-handoff/hop1-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$HOP"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  cat /sys/bus/platform/devices/210800000.aop/handoff;
  cat /sys/bus/platform/devices/210800000.aop/cold_release' > "$HOP/identity.txt"
# The wake is automatic; this waits for phase=streaming, then holds 60 s.
AOP_CAPTURE_DIR="$HOP/capture" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/capture-stage23.sh handoff-full-hop1
python3 -B scripts/periph/aop/check-handoff.py --entry "$AOP_HANDOFF_ACCEL_ENTRY" \
    --previous "$HOP/capture/previous_shutdown.txt" --previous "$HOP/capture/dmesg.txt" \
    --current "$HOP/capture/dmesg.txt"
# Physically tilt again; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh handoff-full-hop1-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$HOP/evidence"   # ramoops copy
```

Hop 2 repeats hop 1 exactly (`hop2-...`, labels `handoff-full-hop2*`). The checker
must then report generation 2, and the previous writer must be hop 1's
`SHUTDOWN-LOG writer=` (from `$HOP/capture/dmesg.txt` of hop 1).

**Pass**, for the cold boot and **both** hops: `phase=streaming system_ready=1
iop_crashed=0 fault_control=0`, `cal_sent=3 cal_unsupported=1 arot_sent=0 cal_state=2
rate_set=1 iio=1`, report and ping counters increasing for 60 s, fresh changing IIO
X/Y/Z on tilt. For each hop: `check-handoff.py` PASS (the previous transcript has
AP `0x10`, IOP `0x201`, equal pool hashes, idle mailbox, `steps=0x7ff`, a VALID record,
`shutdown-handoff-complete` and no halt/drain; the current kernel validated, consumed
and woke once and shows no `FORCE-COLD`/`COLD-W step=cold-stop`), sysfs `handoff`
shows `result=adopted used=1 gen=<1|2> path=<hello|resume>`, and debugfs state shows
`handoff ... adopt=1`. Record which `path=` fired: it is the experiment's answer.
A wake ACK, system-ready or a VALID record alone is not a pass.

**On failure**, retain `$HOP/capture` (`crashlog.bin/.txt`, `previous_shutdown.txt`,
`handoff.txt`, `dmesg.txt`, state) and the evidence directory, then recover with
DFU. Do not try `cold_release`: it is refused in that boot by design. `refused` means
the record or the live state was not certified (see `reason=`). `failed-handoff-*` or
`failed-rtkit-crashlog` after `HANDOFF consumed` means the wake itself failed. The
next kernel then finds a CONSUMED/FAILED record and refuses again until DFU.

**Optional conservative check:** a fresh DFU into `$AOP_HANDOFF_SYS_PAYLOAD`, upload
`$AOP_HANDOFF_SYS_ENTRY`/`$AOP_HANDOFF_SYS_DIR`, `trigger-cold.sh handoff-sys-cold`,
kexec that entry, capture as above, then `check-handoff.py --sys ...`. Require
system-ready, three grants (adopted or regranted), increasing pings, and no
apps/IIO/crash for both holds. This does not replace the FULL test.

**Optional quiesce-reload retest** (separate DFU, not part of the handoff pass): DFU into
`$AOP_QUIESCE_ACCEL_PAYLOAD`, upload its entry, `trigger-cold.sh quiesce-full-cold` plus
tilt, `linux-a9-kexec reboot $AOP_QUIESCE_ACCEL_ENTRY`, then in the new kernel:

```sh
Q=build/periph/aop-handoff/quiesce-hop-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_QUIESCE_ACCEL_ENTRY"
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh quiesce-full-second
```

This time the transcript shows whether the quiesce ran (halt/drain included), and a
crash is captured whole in `crashlog.bin`.

### Build and offline validation

```sh
scripts/periph/aop/build-handoff.sh
python3 -B scripts/periph/aop/check-handoff.py --self-test
python3 -B scripts/periph/aop/decode-crashlog.py docs/periph/logs/aop-warm-20261026/quiesce-hop/dmesg.txt
uv run --with capstone --with unicorn python -B scripts/periph/aop/re-handoff.py
# Optional private firmware re-export:
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python -B scripts/periph/aop/re-handoff.py --firmware
```

`build-handoff.sh` runs `test-offline.py` with ASan/UBSan. The new
`tests/handoff.c` compiles the production record/validate/consume/wake/adopt/write,
quiesce and shutdown functions, plus the transcript code, against the actual
protocol callbacks and a modeled peer. It covers: a cold start, then a VALID gen-1
record with no halt/drain; hop 1 via a modeled HELLO path (2 system adoptions
without reply plus 1 PA-0 regrant, 17 AFK regions reused, calibration/rates/IIO);
consumed-once; hop 2 via in-place resume with a slow peer (AP-on ACK after
1.5 s, first ping ACK after 2 s, which fails on the pre-fix driver with
`failed-ping-timeout`); 21 refusal cases with no TX and an
untouched record; wake failures (silent, crashlog-first, mismatched PA); outgoing
failures that write FAILED records; and the replayed handoff and `aop_quiesce=1`
transcripts, which the real Python checkers then accept. Existing harnesses were
extended (mailbox handoff owner, forced-cold image fingerprint/record clear, sysfs
refusal). **Firmware replies, sleep and wake are modeled; no hardware wake follows.**

Build verification completed (2026-10-26): 50 offline PASS lines, W=1 clean,
checkpatch 0 errors/0 warnings on both commits, full Image.gz plus 105 DTBs, the clean-branch,
config, DT, pool and linked-string checks, three pstore recovery fixtures, and
extraction/checksum/firmware/ECID/cmdline checks of all three payloads. Generated
objects were cleaned. Cmdlines are 925 (sys), 927 (handoff FULL) and 923
(quiesce FULL) bytes. `root-firmware/SHA256SUMS` is identical to aop-stage3's.
Logs are under `build/periph/aop-handoff/`: `offline.log`, `re-handoff.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `handoff.patch` and `manifest-handoff.json`.
Payload SHA256:

- `m1n1-aop-handoff-accel-621fe3f144.bin`: `74d6a487def9726a25acc8dad96ad187effd561a2ef77cfc5c39d5b31746e659`
- `m1n1-aop-handoff-sys-621fe3f144.bin`: `aee126d5dc4feffdb6a2d1940c3ed8b79a968e96ac64f3fa43be8f865f97a17f`
- `m1n1-aop-quiesce-accel-621fe3f144.bin`: `ddc0aa010054e96fdab324d15a9fce0ba9a0fef4f69e67d4a95baeaee70c084c`

### 2026-10-26 13:27 DFU: sleep/wake handoff and quiesce-reload (aop-handoff 621fe3f144)

- **Cold** (`aop-handoff-accel-621fe3f144`): streaming, 700 reports, 69/69 pings.
- **Hop 1, sleep/wake handoff:** the outgoing quiesce is now **proven** (`AOP-PREV` transcript):
  IOP ack `0x201`, RAM stable (identical SHA over 0x186000), record valid, `steps=0x7ff`,
  no failed step, halt skipped. The incoming kernel validated and consumed the record and
  sent `0x0060000000000220`. **No reply in 20 s** (`failed-handoff-no-wake-reply`,
  `iop_crashed=0`). The inbox register went `0x21101` → `0x1201` and stayed: the wake word
  was never consumed. `cpu44=0` before and after. The slept AOP CPU does not service
  the mailbox; waking it needs a CPU-level action (RUN/IRQ/NMI), not a message alone.
- **Quiesce-then-reload over that cleanly slept AOP** (`aop-quiesce-accel-621fe3f144`,
  trigger): HELLO, EPMAP, system_ready, 3 grants, 1 ping, then **crash at the accel
  STARTEP**: the same point as the unquiesced reloads, so 5/5. A clean quiesce does not make
  reload work. The full crashlog was captured for the first time (0x650 bytes: Cver,
  Ctim, CrgA registers, Ccst, Cmbx, Crtk task `rtk_ep_work`, `power`, Ccdp, Csta), with
  firmware addresses 0x010427f8 / 0x01047e78 / 0x01046f98 / 0x010272ed / 0x0102c6fb /
  0x0102f252. Logs: `logs/aop-handoff-20261026/`.

## Opt-in SPI0 pre-reset reload and resume-wrapper wake (`a9/aop-wake`)

Kernel `f695e71125f8073247991fe6f803ae6e6487f98f`, one commit on the new branch
`a9/aop-wake`, on top of `a9/aop-handoff` `621fe3f144` (it keeps the handoff, the
transcript/crashlog fixes, the `3a7b3805b3` calibration gate, `aop_quiesce=1` and every
kexecfix/ANS commit). Release `7.3.0-rc1-aop-wake-gf695e71125f8`. Only `apple-a9-aop-active.c` and the new
header `a9-aop-reload.h` differ from `621fe3f144`. Config, DTBs, pool and ramoops
reservations are byte-identical to tri49a. The two experiments are the ones in
[ds-aop-reload-crash-and-wake.md](ds-aop-reload-crash-and-wake.md). Both are default off
and independent. **No device operation was performed; nothing below is a hardware
result.**

### `aop_spi0_reset=1`: reset the retained SPI0 state before a reload

Needs `aop_run=1/2`. Works with or without `aop_quiesce=1`, because the reload path in
the new kernel is the same whether or not the old one quiesced. Probe maps only AP PA
`0x210100000..0x210100fff` and refuses unless the wrapper is at `0x210800000`. Nothing
reads SPI0 at probe.

On the forced-cold path, after the proven `COLD halt errno=0` and drain and before the
firmware wait or any SRAM write:

1. Runs only if the boot snapshot (before the stop) read auto-boot 1, i.e. a live
   instance. A DFU cold start reads 0 and logs `SPI0 reset skipped
   reason=fresh-instance`; the block is never touched, so the proven cold path is
   unchanged.
2. It logs `SPI0 reset begin pa=210100000 cpu=halted ps_spi0=<raw> errno=<e>` and flushes
   it to ramoops (`COLD checkpoint=spi0-reset`) before the first AP access to the block.
   Inside one PMGR guard over the five AOP domains (IRQs off, AOP CPU halted), it records
   `+0, +0x8, +0x8d0, +0x8d4, +0x280, +0x28c`. Then it replays `0x0102f2f4` exactly:
   `saved = +0`, write `saved & ~0x101 | 0x100`, poll until bit 8 is set, then until bit 0
   is clear (1 ms bound each; the firmware spins forever), wait 100 µs, write
   `saved & ~0x101`, wait 100 µs, write `saved`, record again. The firmware's "provider
   power transition 3" is an AOP-local call with no AP register equivalent. It is not
   replayed and the log says `provider3=not-replayed`. A poll timeout still writes
   `saved` back.
3. Requires `(+0x8d0 & 0x40) == 0` afterwards. Otherwise `failed-spi0-reset`: no load, no
   RUN, AOP left halted.
4. The pinned reload then runs unchanged. One extra read happens after the remap and
   outbox-enable stores and before RUN, with the CPU still halted: `+0x8d0` must still have
   bit 6 clear. If it reasserted, the result is `failed-spi0-reasserted` and there is no RUN
   or auto-boot. The SPI0 block is never accessed while the AOP runs.

Log lines: `SPI0 reset begin …` with `COLD checkpoint=spi0-reset`, then `SPI0 reset errno=… ran=… pa=210100000 before ctl=… 8d0=…`, `SPI0 reset after
… ch3_pending=<before>-><after> provider3=not-replayed`, `SPI0 pmgr ps_spi0 before=…
errno=… after=… errno=…`, `SPI0 pre-run errno=… 8d0=…`. The debugfs `state` file gains a
`spi0_reset …` line.

**Deviation from the RE doc (ps_spi0).** The RE doc reads the ps_spi0 gate's raw
`0x200` as "inherited as on" and proposes holding that domain active. In
`pmgr-pwrstate.c`, PS_ACTUAL is bits 7:4, PS_TARGET is bits 3:0, ACTIVE is `0xf` and bit 9
is WAS_CLKGATED. So `0x200` decodes as actual = target = 0 (gated) plus WAS_CLKGATED. This
driver never powers a domain on. It reads ps_spi0 only through the read-only PMGR observer,
before and after the reset (`errno=-112` = off, raw still logged). The SPI0 accesses happen
under the AOP-domain guard. The new firmware read `0x40` from this block right after every
earlier reload, so the block is most likely clocked inside the AOP island and ps_spi0 is
not its gate. If ps_spi0 does gate it, the first SPI0 read faults (SError/hang, an accepted
risk). The last ramoops lines would then be `SPI0 reset begin … ps_spi0=…` and `COLD
checkpoint=spi0-reset`, with no `SPI0 reset errno=` line. Record what the ps_spi0 value
says either way.

### `aop_wake_wrapper=1`: replay the iOS resume wrapper before `0x220`

Needs `aop_handoff=sleep`. It runs after the record is validated and consumed and the
`handoff-before-wake` checkpoint is flushed, immediately before the single
`0x0060000000000220`. It makes exactly the doc's steps 3–7 through the logged control-write
path (`COLD-W step=handoff-wrapper-wake …`):

```text
+0x4000 = 0x1111   +0x0c00 = 0x1111
+0x4004 = 0x0001   +0x0c04 = 0x0001
+0x4004 = 0x1000   +0x0c04 = 0x1000
+0x4020 = read(+0x4020) | 1
```

It does not touch `+0x44` (RUN), auto-boot, or the NMI pair `+0xc004/+0xc024`, and it
enables no AP IRQ (stage 3 polls). It logs one summary: `HANDOFF wrapper-wake errno=0 writes=7
in4008=…->… out4020=…->… set4000=…->… setc00=…->… cpu44=… run=untouched
auto_boot=untouched nmi=untouched setactive=not-replayed`. If any store fails, the result
is `failed-handoff-wrapper-wake` and no `0x220` is sent. When the wake window closes
(first reply, or 20 s of silence), the driver logs `HANDOFF wake-window end path=…
in4008=… out4020=… wrapper=…`. That line tells "never consumed" (`00001201`, as at 13:27)
apart from "consumed" (bit 17 set). The first reply is still logged as `HANDOFF wake first
ep=… word=… path=…`. The sysfs `handoff` line gains `wrapper=0/1`.

**`setActive(true)` is not implemented.** The doc gives it only as provider vtable
`+0x6c8`, `0xfffffff005b4b6f4`, an "optional platform service", with no register-level
definition. It is the next single variable if the wrapper-only wake fails. RUN and NMI
stay out of this test.

**No automatic fallback anywhere.** A failed wake still never reloads in the same boot:
with a record of any state, `cold_release` returns -EBUSY. In handoff mode a live instance
without a record is still refused (`refused-handoff-live-aop-without-record`). So in the
combined entry `aop_spi0_reset=1` cannot fire. Probe prints
`spi0_applies=never(handoff-refuses-live-reload)`, and its DFU cold start logs
`SPI0 reset skipped reason=fresh-instance`. The probe line
`EXPERIMENT spi0_reset=… wake_wrapper=… setactive=not-implemented automatic_fallback=none`
identifies the mode in every entry.

### Entries (`source build/periph/aop-wake/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-wake-accel-f695e71125` | `AOP_WAKE_ACCEL` | FULL: `aop_run=2 aop_handoff=sleep aop_wake_wrapper=1` |
| `aop-spi0-accel-f695e71125` | `AOP_SPI0_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |
| `aop-wakespi0-accel-f695e71125` | `AOP_WAKESPI0_ACCEL` | FULL: `aop_run=2 aop_handoff=sleep aop_wake_wrapper=1 aop_spi0_reset=1` (no fallback; SPI0 reset inert, see above) |

Payloads are `build/periph/aop-wake/m1n1-<entry>.bin`; entry dirs are
`build/periph/aop-wake/<entry>/`. All three are deferred test5 (`attach_stage=3
aop_force_cold=1 aop_cold_test=5 aop_defer=1`), keep the full kexecfix set and exactly one
`pcie_apple_h9p.ans_keep_power=1`, and have debug info off. Cmdlines are 943 (wake), 937
(spi0) and 964 (combined) bytes. The four private root files are byte-identical to
aop-stage3's.

### Exact owner-run test: one DFU, cold → stream → wrapper-wake hop → (if it fails) SPI0-reset reload

The builder ran none of this. Keep the root server and the default entry; nothing sets a
default.

**A. DFU cold start in the wake entry.**

```sh
source build/periph/aop-wake/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_WAKE_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'cat /sys/bus/platform/devices/210800000.aop/handoff;
  sudo -n dmesg | grep -E "EXPERIMENT|HANDOFF (mode|probe)|SHUTDOWN-LOG"'
# Expect record=missing … wrapper=0, "HANDOFF probe magic=. auto_boot=00000000",
# "EXPERIMENT spi0_reset=0 wake_wrapper=1 setactive=not-implemented automatic_fallback=none".
scripts/boot/upload-entry.sh "$AOP_WAKE_ACCEL_ENTRY" "$AOP_WAKE_ACCEL_DIR"
scripts/boot/upload-entry.sh "$AOP_SPI0_ACCEL_ENTRY" "$AOP_SPI0_ACCEL_DIR"   # staged for leg C
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/trigger-cold.sh wake-full-cold
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh wake-full-cold-tilt
```

Go on only if the cold run passes the stage-3 criteria: streaming, `cal_sent=3
cal_unsupported=1 cal_state=2`, IIO, tilt.

**B. Hop with the wrapper wake (the primary experiment).**

```sh
# (no dmesg -n 8: never raise the console level, see the transcript section)
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_WAKE_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel. Do NOT trigger cold_release.
HOP=build/periph/aop-wake/hop1-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$HOP"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  cat /sys/bus/platform/devices/210800000.aop/handoff;
  cat /sys/bus/platform/devices/210800000.aop/cold_release' > "$HOP/identity.txt"
# The wake is automatic; this waits for a terminal phase, then holds 60 s.
AOP_CAPTURE_DIR="$HOP/capture" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/capture-stage23.sh wake-full-hop1
grep -E 'COLD-W step=handoff-wrapper-wake|HANDOFF (wrapper-wake|wake220|wake first|wake-window end|consumed)' \
    "$HOP/capture/dmesg.txt" | tee "$HOP/wake-lines.txt"
python3 -B scripts/periph/aop/check-handoff.py --wrapper --entry "$AOP_WAKE_ACCEL_ENTRY" \
    --previous "$HOP/capture/previous_shutdown.txt" --previous "$HOP/capture/dmesg.txt" \
    --current "$HOP/capture/dmesg.txt"
# Only if it streams: physically tilt again; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh wake-full-hop1-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$HOP/evidence"   # ramoops copy
```

**Pass (B):** `check-handoff.py --wrapper` PASS. That means the previous transcript has AP
`0x10`, IOP `0x201`, a stable pool, an idle mailbox, `steps=0x7ff` and a VALID record, with
no halt or drain. The current kernel validated and consumed the record, made exactly the
seven `COLD-W step=handoff-wrapper-wake` stores (`4000 0c00 4004 0c04 4004 0c04 4020`)
with `HANDOFF wrapper-wake errno=0 writes=7`, then sent one wake220. Specifically:

- `HANDOFF wake-window end … in4008=` has bit 17 (`0x20000`, empty) set. It must not be
  `00001201`.
- The first reply is the type-7 `0x20` IOP ACK (wire `0x0070000000000020`), logged as
  `HANDOFF wake first ep=0 word=70000000000020 path=resume` (56-bit payload, `%014llx`). A HELLO (`path=hello`) also counts as a wake; record
  which one fired.
- sysfs `handoff` shows `result=adopted used=1 gen=1 path=<resume|hello> … wrapper=1`.
- `phase=streaming system_ready=1 iop_crashed=0 fault_control=0`, `cal_sent=3
  cal_unsupported=1 arot_sent=0 cal_state=2 rate_set=1 iio=1`.
- Report and ping counters increase for 60 s, and the IIO X/Y/Z values change on tilt.
- No `FORCE-COLD`, no `COLD halt`, no crashlog.

Also record the `set4000`/`setc00` transitions in the summary line: they show the mask
state each AP view actually latched. A wake ACK alone is not a pass. If B passes, a
second identical hop (`hop2-…`, `wake-full-hop2*`) is optional; it must report
generation 2.

**Fail (B)** means `failed-handoff-no-wake-reply`, `failed-handoff-wrapper-wake`, or
`failed-*` / crashlog after `HANDOFF consumed`. Keep `$HOP` (`dmesg.txt`, `state.txt`,
`registers.txt`, `handoff.txt`, `crashlog.bin/.txt`, `previous_shutdown.txt`). Classify
from `wake-lines.txt`:

- inbox still `00001201` at the window end: the word was never consumed, so the wrapper
  replay is not sufficient. The next single variable is `setActive(true)`, not RUN or NMI.
- inbox empty but no reply: consumed, but the firmware did not answer.
- crashlog: decode it.

Do not try `cold_release` in this boot: it is refused by design. Continue with C.

**C. Only if B failed: kexec into the SPI0-reset reload entry and trigger.** The failed
kernel writes a FAILED record on shutdown. The SPI0 entry runs `aop_quiesce=1` (not
handoff), so it ignores that record and allows the one deferred trigger.

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_SPI0_ACCEL_ENTRY"
# Wait for SSH in the NEW kernel.
Q=build/periph/aop-wake/spi0-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|AOP-PREV (HANDOFF record|STAGE3)' "$Q/new-kernel.txt"
# Expect "EXPERIMENT spi0_reset=1 wake_wrapper=0 … spi0_applies=live-reload spi0_pmgr=ps_spi0"
# and the failed hop's AOP-PREV transcript.
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh spi0-reload
grep -E 'COLD (halt|stale)|SPI0 |AOP released|failed-|STAGE3 checkpoint' "$Q/trigger/dmesg.txt" \
    | tee "$Q/spi0-lines.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" --entry "$AOP_SPI0_ACCEL_ENTRY"
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh spi0-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass (C):** `check-spi0-reload.py` PASS, with these lines in order: `FORCE-COLD`,
`COLD halt errno=0`, drain, `SPI0 reset begin`, `COLD checkpoint=spi0-reset`, `SPI0 reset
errno=0 ran=1`, `SPI0 pre-run errno=0 … ch3_pending=0`, `AOP released`.

- The SPI0 reset words are `asserted = saved & ~0x101 | 0x100`, and `ch3_pending=<before>->0`.
  The RE predicts `1->0`; if it reads `0->0`, record that.
- After release: HELLO, EPMAP, system-ready and 3 grants, then the accel STARTEP gets its
  normal `0x20` ACK. This is where the 5/5 reloads crashed with `failed-startep -110`.
- Then 3 calibration ACKs, `0x18` unsupported, `0x3d=2`, rates, first report, IIO, and
  streaming with report/ping counters increasing for 60 s.
- Tilt changes X/Y/Z. There is no crashlog.
- The drain lines show whether the unconsumed `0x220` was still queued: `COLD stale
  fifo=inbox … word=0060000000000220`.
- Record the `SPI0 pmgr ps_spi0` line.

**Fail (C)** takes one of these forms:

- `failed-spi0-reset`: bit 6 did not clear, or a poll timed out. The AOP is left halted.
- `failed-spi0-reasserted`: bit 6 came back before RUN. The next experiment is then the
  firmware-order fix (reset before the IRQ enable), not an IMU reset.
- A complete crashlog with `PC=0x0102f252`, `DFAR=0x4c`, `r5=0` (`crashlog.txt`): the
  reset did not reach the state the firmware IRQ reads.
- A kernel fault or hang right after `SPI0 reset begin` / `COLD checkpoint=spi0-reset`,
  with no `SPI0 reset errno=` line: the block was not accessible from the AP. Collect the
  ramoops evidence with `collect-hang-evidence.sh` after the DFU.

In every case keep `$Q`, then recover with DFU.

**Optional, not part of this DFU.** `$AOP_WAKESPI0_ACCEL_*` can replace the wake entry in
legs A and B. It should behave identically: the cold start logs `SPI0 reset skipped
reason=fresh-instance`, and there is no reload after a failed wake.

### Build and offline validation

```sh
scripts/periph/aop/build-wake.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-handoff.py --self-test
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake
```

`build-wake.sh` first creates `/src/wt/aop-wake` from `621fe3f144`. `build-kernel.sh`
would start a missing tree at `hoolock`, so the script does this directly. It then runs
the offline suite, a W=1 build of the driver objects, checkpatch, the commit, the full
Image.gz/DTB build, `check-handoff-build.py --wake` (clean branch, only the two files
differ, kexecfix PCIe source, config, DTBs, linked strings), the pstore fixture, packaging,
the package test and `make clean`. The offline harness is extended as follows:

- `tests/force-cold.c` compiles the production SPI0 and wrapper functions beside the real
  stop/drain/loader against a fault-injectable SPI0 model. It covers: fresh instance
  untouched; halt and drain required; the begin line flushed before the first access (a
  failed flush means no access); exact `110/10/11` stores with two 100 µs delays,
  all inside the guard with the CPU halted; bit 6 cleared; the pre-RUN read lands between
  store 15 (outbox enable) and store 16 (RUN); a reasserted bit means no RUN or auto-boot;
  both poll timeouts restore the saved word; sticky bit and domains-off refusals; ps_spi0
  is read only. On the wrapper side: the exact seven stores, no +44/auto-boot/c004/c024,
  and a fault after any store stops the sequence.
- `tests/handoff.c` replays a modeled sleeper that needs bits 0/12 unmasked. It stays silent
  without the wrapper (the 13:27 transcript). With it, all seven stores precede the single
  `0x220`, then resume, stream and write a VALID record. A failed store sends no wake and
  certifies FAILED.
- `check-handoff.py --wrapper` and the new `check-spi0-reload.py` have self-tests.

**Firmware replies, the sleeper and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 54 offline PASS lines (50 before).
- W=1 clean; checkpatch 0 errors/0 warnings (442 lines).
- Full Image.gz plus 105 DTBs.
- `check-handoff-build.py --wake` PASS, plus the three pstore recovery fixtures.
- Extraction, checksum, firmware, ECID and cmdline checks of all three payloads.
- `make clean` brought the tree from 2.3 GB to 1.8 GB.
- `root-firmware/SHA256SUMS` is identical to aop-stage3's.

Logs are under `build/periph/aop-wake/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `run.log`, `wake.patch` and `manifest-wake.json`.
Payload SHA256:

- `m1n1-aop-wake-accel-f695e71125.bin`: `a5e7afbfa0a1bf240362ed52e1300e18f1d50efd285fed85d31b10f412e4a6db`
- `m1n1-aop-spi0-accel-f695e71125.bin`: `cdfb6f9443ab0de4e054232c744a42a5809a1b14ba739e466488e85850fb09c6`
- `m1n1-aop-wakespi0-accel-f695e71125.bin`: `7f74017cc12b2a4de0e05ddd01600c6d2e780ad441afef78580925689ec67b57`

### 2026-10-26 14:55 DFU: wrapper wake (aop-wake f695e71125)

- **A, cold:** streaming (90+ reports, pings 10/10, tilt visible).
- **B, hop with `aop_wake_wrapper=1`:** the quiesce was clean again (`iop-ack 0x201`, `steps=0x7ff`).
  The wrapper writes all landed (masks 0x1111 → 0x110 in both views; +0x4020 bit 0 was
  already set). One `0x220` was sent, and the inbox stayed `0x3401` for the whole wake window:
  **still never consumed**, `failed-handoff-no-wake-reply`, `iop_crashed=0`, cpu44=0.
  `idle4040` went 0x7f → 0x58 on the TX. So the wrapper unmask alone does not wake a
  state-8 AOP; the CPU is not executing. The next variables are `setActive(true)` (not
  implemented) or a CPU-level resume. Logs: `logs/aop-wake-20261026/`.

### 2026-10-26 15:52 DFU: SPI0-reset reload (aop-spi0-accel-f695e71125)

Cold boot into the SPI0 entry: streaming (289 reports), with `SPI0 reset skipped reason=fresh-instance`
as designed. Warm kexec into the same entry: the previous kernel's quiesce completed
(`quiesced=1 halted=1 drained=1 steps=0x1bff`); preflight `spi0_reset=1 spi0_applies=live-reload
spi0_pmgr=ps_spi0`. The trigger crashed again at the accel STARTEP (6/6), **but the reset never ran**:
`SPI0 reset skipped reason=fresh-instance auto_boot=0`. **Logic bug:** the outgoing quiesce clears
auto-boot when it halts, so after a clean quiesce a live reload is misclassified as a fresh
instance. The kernel did not hang. The SPI0 reset itself is still untested. Fix: decide "retained
state" from the previous-shutdown record (halted=1 / steps) rather than from auto_boot.
Logs: `logs/aop-spi0-20261026/`.

## SPI0 reset decided from the previous shutdown (`a9/aop-wake2`)

Kernel `61561869d00295658dffac65974c95b5c08e5428`, one commit on the new branch `a9/aop-wake2`, on top of `a9/aop-wake`
`f695e71125`. Release `7.3.0-rc1-aop-wake2-g61561869d002`. Only `apple-a9-aop-active.c` and `a9-aop-reload.h`
differ from `f695e71125`. Config, DTBs, pool and ramoops reservations are byte-identical to
tri49a. It fixes the 15:52 logic bug and changes nothing else. **No device operation was
performed; nothing below is a hardware result.**

### The decision

`aop_spi0_reset=1` used to run the reset only if the boot snapshot (before the stop) read
auto-boot 1. The outgoing `aop_quiesce=1` halt writes auto-boot 0, exactly like a DFU cold
start, so the 15:52 reload skipped the reset. The trigger now decides "firmware ran since the
last SoC reset" from three inputs, none of which touch SPI0:

1. **auto-boot** from the boot snapshot (`values[5]`), as before.
2. **The previous kernel's shutdown transcript** in the reserved pool tail page (the
   `AOP-PREV` lines). Probe already validates it (magic, version, length, FNV checksum) and
   replays it once. It now also parses three whole-line prefixes:
   - `SHUTDOWN begin … hello=1 …`: that kernel's firmware ran. This is line 1, so it
     survives a truncated transcript.
   - `QUIESCE step=complete … halted=1 … steps=0x…`: it was also quiesced and halted.
     `halted` counts only with `A9_HO_STEP_HALTED` (bit 11) set in `steps`. It only
     labels the reason; it does not change run/skip.
   - `SPI0-CARRY retained=1 …` (new, see below): an earlier kernel in this chain ran it.
3. **Wrapper `+0x4020` bit 0 (outbox enable)** from the boot snapshot (`values[3]`). The
   pool DRAM can outlive a DFU (the handoff probe already guards its record against this).
   The wrapper cannot. All 12 saved snapshots with the DFU reset pattern (b88 = inbox =
   outbox = `0x00020001`, auto-boot 0, including the 15:52 cold leg) have the enable set.
   The first store of every Linux stop (`a9_aop_force_stop`) clears it. Both post-quiesce
   reload snapshots read it clear: `0x00021100` at 15:52 and `0x00022200` in
   `stage3-warm-full-second-20261026T112716Z`. Transcript evidence counts only while this
   bit is clear.

| auto-boot | transcript | `+0x4020` bit 0 | Decision | `reason=` |
|---|---|---|---|---|
| 1 | any | any | run | `live-auto-boot` (unquiesced reload) |
| 0 | missing | any | skip | `fresh-instance` (DFU cold) |
| 0 | magic present, invalid | any | skip | `prev-transcript-invalid` |
| 0 | valid, no `hello=1` and no carry | any | skip | `prev-no-run` |
| 0 | valid, `hello=1` or carry | 1 | skip | `stale-transcript` (record outlived a reset) |
| 0 | valid, `hello=1` + `halted=1` + bit 11 | 0 | run | `prev-quiesce-halted` (the 15:52 case) |
| 0 | valid, `hello=1`, no halt | 0 | run | `prev-live` |
| 0 | valid, carry only | 0 | run | `prev-carried` |

**Missing or corrupt record: skip.** This is the conservative side. Running the reset on a
block with nothing retained would be the first AP access to SPI0 on a fresh instance, with
no evidence and possibly a gated `ps_spi0`. That is an unclassified SError/hang risk. A
wrong skip only repeats the known accel STARTEP crash, which leaves the kernel running.
The missing case is also the DFU-cold signature the task requires to skip, even when a
Linux stop footprint is present.

**SPI0 `+0x8d0` bit 6 is not decision evidence.** Reading it is not proven safe, and the
`SPI0 reset begin` line plus the flushed `COLD checkpoint=spi0-reset` must precede the
first AP access to the block. It is still recorded inside the guarded reset, as before
(`ch3_pending=<before>-><after>`).

**Carry.** An `aop_spi0_reset=1` kernel writes one transcript line right after
`SHUTDOWN begin`: `SPI0-CARRY retained=<0|1> source=<…>`. The value is 1 if this
kernel's firmware said HELLO (`source=hello`). Otherwise, if this kernel decided, it is
that decision (`source=<reason>`, so a stale or fresh skip carries 0). If it never
triggered, it is the previous transcript's evidence (`source=prev-unbound`). A chain
"cold run → kexec, no trigger → kexec, trigger" therefore still resets. The consumer
always applies the `+0x4020` binding, so a carried 1 cannot bypass it. Entries without
`aop_spi0_reset=1` write no such line; their transcripts are unchanged.

Everything after a run decision is unchanged from `f695e71125`: the `begin` line flushed to
ramoops before the first SPI0 access, the guarded 110/10/11 replay, bit 6 required clear
after the reset and again just before RUN, `failed-spi0-reset` /
`failed-spi0-reasserted` with no load or RUN, no automatic fallback, and no NMI, `0x220` or
wrapper writes.

### Log lines

- Probe: `SPI0 evidence prev=<missing|invalid|valid> prev_hello=… prev_halted=…
  prev_steps=0x… prev_carry=…: decided at the trigger with the boot auto-boot and +0x4020`.
- Trigger, after `COLD halt` and the drain and before anything else in the SPI0 path:
  `SPI0 reset decision=<run|skip> reason=<reason> auto_boot=<n> out4020=<hex> prev=…
  prev_hello=… prev_halted=… prev_steps=0x… prev_carry=…`. A skip appends
  `: block not touched`. A run is followed by the unchanged `SPI0 reset begin …`.
- Shutdown transcript: `SPI0-CARRY retained=… source=…` (replayed next boot as
  `AOP-PREV SPI0-CARRY …`).
- debugfs `state`: the `spi0_reset` line ends with `decision= reason= auto_boot= out4020=
  prev= prev_hello= prev_halted= prev_steps= prev_carry=`. `skipped=` now holds the skip
  reason.

`check-spi0-reload.py` now requires exactly one `SPI0 reset decision=run` line, after
`COLD halt` and before `SPI0 reset begin`, with a run reason. It rejects `decision=skip`.
`--reason <r>` pins the reason.

### Entry (`source build/periph/aop-wake2/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-spi0b-accel-61561869d0` | `AOP_SPI0B_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake2/m1n1-aop-spi0b-accel-61561869d0.bin`, entry dir
`build/periph/aop-wake2/aop-spi0b-accel-61561869d0/`. Deferred test5 (`attach_stage=3
aop_force_cold=1 aop_cold_test=5 aop_defer=1`). The full kexecfix set, exactly one
`pcie_apple_h9p.ans_keep_power=1`, debug info off, cmdline 938 bytes. The four
private root files are byte-identical to aop-stage3's.

### Exact owner-run test: one DFU, cold → stream → kexec into the same entry → SPI0-reset reload

The builder ran none of this. Keep the root server and the default entry; nothing sets a
default.

**A. DFU cold start in the new entry.**

```sh
source build/periph/aop-wake2/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_SPI0B_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake2/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
# Expect "EXPERIMENT spi0_reset=1 wake_wrapper=0 ... spi0_applies=live-reload spi0_pmgr=ps_spi0"
# and normally "AOP-PREV shutdown transcript missing" + "SPI0 evidence prev=missing ...".
# Staged now: leg B kexecs into this same entry.
scripts/boot/upload-entry.sh "$AOP_SPI0B_ACCEL_ENTRY" "$AOP_SPI0B_ACCEL_DIR"
C=build/periph/aop-wake2/spi0b-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh spi0b-cold
grep -E 'SPI0 ' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh spi0b-cold-tilt
```

Expect exactly one `SPI0 reset decision=skip reason=fresh-instance auto_boot=0
out4020=00020001 prev=missing …: block not touched` and no `SPI0 reset begin`. If the DRAM
still held an old transcript, the line reads `prev=valid` with `reason=stale-transcript`
(it showed a run) or `reason=prev-no-run` (it did not). Either is also a pass, but record
it. This replaces the old `SPI0 reset skipped reason=fresh-instance` wording. Any `decision=run` on this DFU cold start is a fail:
stop and keep `$C`. Go on only if the cold run passes the stage-3 criteria: streaming,
`cal_sent=3 cal_unsupported=1 cal_state=2`, IIO, tilt.

**B. Kexec into the same entry, preflight, trigger.**

```sh
# (no dmesg -n 8: never raise the console level, see the transcript section)
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_SPI0B_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake2/spi0b-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_SPI0B_ACCEL_ENTRY"
```

Preflight must show:

- `AOP-PREV SHUTDOWN begin mode=quiesce entry=aop-spi0b-accel-61561869d0 … hello=1 phase=streaming`;
- `AOP-PREV SPI0-CARRY retained=1 source=hello`;
- `AOP-PREV QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`;
- `EXPERIMENT spi0_reset=1 wake_wrapper=0 … spi0_applies=live-reload spi0_pmgr=ps_spi0`;
- `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1`;
- `check-warm-shutdown.py` PASS.

If the evidence line shows `prev=missing`, `prev=invalid` or `prev_hello=0 prev_carry=0`, do
not trigger. The decision would be a skip, and the trigger would only repeat the known
STARTEP crash. Keep `$Q` and report. Otherwise trigger once:

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh spi0b-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP released|failed-|STAGE3 checkpoint' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_SPI0B_ACCEL_ENTRY" --reason prev-quiesce-halted
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh spi0b-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass (B):** `check-spi0-reload.py --reason prev-quiesce-halted` PASS, with these lines in
order: `FORCE-COLD`, `COLD halt errno=0`, drain, `SPI0 reset decision=run
reason=prev-quiesce-halted auto_boot=0 out4020=<bit 0 clear, 00021100 at 15:52> prev=valid
prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1`, `SPI0 reset begin pa=210100000
cpu=halted`, `COLD checkpoint=spi0-reset`, `SPI0 reset errno=0 ran=1`, `SPI0 pre-run
errno=0 … ch3_pending=0`, `AOP released`. Specifically:

- The SPI0 reset words are `asserted = saved & ~0x101 | 0x100`, and `ch3_pending=<before>->0`.
  The RE predicts `1->0`; if it reads `0->0`, record that.
- After release: HELLO, EPMAP, system-ready and 3 grants, then the accel STARTEP gets its
  normal `0x20` ACK. This is where 6/6 reloads crashed.
- Then 3 calibration ACKs, `0x18` unsupported, `0x3d=2`, rates, first report, IIO, and
  streaming with report/ping counters increasing for 60 s.
- Tilt changes X/Y/Z. There is no crashlog.
- Record the `SPI0 pmgr ps_spi0 before=… after=…` line and the debugfs `spi0_reset` line
  (`$Q/trigger/state.txt`).

**Fail (B)** takes one of these forms:

- `decision=skip` despite a clean preflight. The test is void, not an SPI0 result. Report
  the reason and the `out4020` value. `stale-transcript` here would mean a Linux stop did
  not leave outbox enable clear, which contradicts the binding evidence.
- `failed-spi0-reset`: bit 6 did not clear, or a poll timed out. The AOP is left halted.
- `failed-spi0-reasserted`: bit 6 came back before RUN. The next experiment is the
  firmware-order fix (reset before the IRQ enable), not an IMU reset.
- A complete crashlog with `PC=0x0102f252`, `DFAR=0x4c`, `r5=0`: the reset ran but did not
  reach the state the firmware IRQ reads.
- A kernel fault or hang right after `SPI0 reset begin` / `COLD checkpoint=spi0-reset`, with
  no `SPI0 reset errno=` line: the block was not accessible from the AP. Collect the ramoops
  evidence with `collect-hang-evidence.sh` after the DFU.

In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake2.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake2
```

The workspace sources `scripts/periph/aop/kernel/{apple-a9-aop-active.c,a9-aop-reload.h}`
now carry this fix. Re-running `build-wake.sh` would therefore commit it onto `a9/aop-wake`
rather than reproduce `f695e71125`. Build the old entries from `a9/aop-wake` as it is.

`build-wake2.sh` creates `/src/wt/aop-wake2` from `f695e71125` and runs the offline suite.
Then it runs a W=1 build of the driver objects, checkpatch and the commit. After that come
the full Image.gz/DTB build and `check-handoff-build.py --wake2` (clean branch, only the two
files differ, kexecfix PCIe source, config, DTBs, linked decision/carry strings). The pstore
fixture, packaging, the package test and `make clean` finish it. New offline coverage:

- `tests/force-cold.c` builds the real 15:52 transcript from `logs/aop-spi0-20261026/dmesg-after.txt`
  (63 lines, 3479 bytes, as the header recorded) and runs the production
  `a9_aop_spi0_prereset` on it. It checks four cases:
  - **DFU cold** (no record, auto-boot 0, `0x20001`): skip `fresh-instance`, no SPI0 access.
  - **Post-quiesce-halt reload** (that transcript, auto-boot 0, `0x21100`): run
    `prev-quiesce-halted`, the exact 110/10/11 reset, then the proven loader with the
    pre-RUN read.
  - **Unquiesced reload** (auto-boot 1): run `live-auto-boot`, whatever the record says.
  - **Missing/corrupt record** with auto-boot 0: skip (`fresh-instance` /
    `prev-transcript-invalid`), even with a Linux stop footprint.

  It also covers: the same transcript with `+0x4020` at reset skips `stale-transcript` and
  the loader then makes no SPI0 read; no-HELLO skips unless carried; a truncated record
  still runs `prev-live`; `halted=1` without step bit 11 is not halted; mid-line or
  inexact tokens are ignored.
- `tests/handoff.c` runs the production quiesce shutdown with `aop_spi0_reset=1`. It checks
  that `SPI0-CARRY retained=1 source=hello` lands after `SHUTDOWN begin`, and that the next
  probe's production replay parses hello/halted/steps `0x1bff`/carry. A second kernel that
  never releases the AOP carries `prev-unbound` to a third (`prev-carried`). A stale skip
  carries 0. Without the opt-in the transcript has no carry line.

**Firmware replies and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 56 offline PASS lines in `test-offline.py` (54 before), plus the checker self-test and the
  `--wake2` plan check (58 lines in `offline.log`).
- W=1 clean; checkpatch 0 errors/0 warnings (327 lines).
- Full Image.gz plus 105 DTBs, byte-identical to tri49a's.
- `check-handoff-build.py --wake2` PASS, plus the three pstore recovery fixtures.
- Extraction, checksum, firmware, ECID and cmdline checks of the payload
  (`stage3-package.py --wake2`).
- `make clean` brought the tree from 2.3 GB to 1.8 GB; `/src` has 6.6 GB free.
- `root-firmware/SHA256SUMS` is identical to aop-wake's.

Logs are under `build/periph/aop-wake2/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `wake2.patch` and `manifest-wake2.json`. Payload SHA256:

- `m1n1-aop-spi0b-accel-61561869d0.bin`: `b96ca7a4387b30f142138f6f68600810134c465d2fc1b90046a6808ab57078ec`

### 2026-10-26 16:27 DFU: SPI0-reset reload (aop-spi0b-accel-61561869d0)

Cold boot into the spi0b entry: streaming (323 reports), `SPI0 reset decision=skip reason=fresh-instance`.
Warm hop (same entry): `AOP-PREV QUIESCE ... halted=1 steps=0x1bff`, `SPI0-CARRY retained=1`, evidence
`prev_hello=1 prev_halted=1`. Trigger:
- `SPI0 reset decision=run reason=prev-quiesce-halted`, `begin` (ps_spi0 0x200 read-only, errno -112),
  **reset ran without a hang**: `8d0 0x4040 → 0x0`, `ch3 0x18011445 → 0x00010447`, `ch3_pending 1→0`;
  ctl/clk unchanged (`0x00040001`, `0x01b71b00`); provider transition 3 not replayed.
- **`SPI0 pre-run errno=-5 8d0=0x40 ch3_pending=1 cpu=halted` → `failed-spi0-reasserted`**: channel 3's
  pending bit came back while the AOP CPU was still halted, so a hardware source re-raises it (likely the
  accelerometer's DRDY/interrupt or an SPI channel-3 engine/FIFO state). The driver correctly refused to
  release the AOP (no crash, iop_crashed=0). Logs: `logs/aop-spi0b-20261026/`.
Next: find what quiets the channel-3 source (channel/interrupt disable, a sensor-side stop over SPI,
or the firmware's own provider transition 3) before the reload.

## SPI0 channel-3 teardown before RUN (`a9/aop-wake3`)

Kernel `63eadc3456ab9a66dfee1693574a7d9e0b708eec`, one commit on the new branch `a9/aop-wake3`, on top of `a9/aop-wake2`
`61561869d0`. Release `7.3.0-rc1-aop-wake3-g63eadc3456ab`. Only `apple-a9-aop-active.c` and `a9-aop-reload.h`
differ from `61561869d0`. Config, DTBs, pool and ramoops reservations are byte-identical to
tri49a. It implements section 5 of `ds-aop-spi0-ch3-round3.md` and changes nothing else.
**No device operation was performed; nothing below is a hardware result.**

### What it does

Everything up to and including the guarded `0x0102f2f4` controller reset is unchanged: the
retained-state decision, the flushed `SPI0 reset begin` line, the 110/10/11 replay, and the
`failed-spi0-reset` refusal if `+0x8d0` bit 6 survives the reset or a poll times out. Only
after a reset that leaves bit 6 clear, with the AOP CPU halted, AP notifications masked and
before the image copy, the driver now:

1. Logs `SPI0 CH3TD begin pa=210100280 cpu=halted` and flushes `COLD checkpoint=spi0-ch3-teardown`
   to ramoops before the first channel-3 access.
2. Inside the same AOP-domain PMGR guard as the reset, snapshots SPI0
   `+{0x280,0x284,0x288,0x28c,0x298,0x8d0,0x8d4}`, then runs the firmware's own channel
   teardown (`AOPSPIMaster_teardownChannel` `0x0102e4f8`) plus the fault handler's
   acknowledge (`0x0102f20a`):

   ```text
   write32(+0x280, read32(+0x280) & ~0x411)   /* run bit 0 off            */
   poll +0x280 bit 1 == 1                     /* 1 ms bound, like the reset */
   write32(+0x280, read32(+0x280) & ~0x450)   /* class-0 IRQ (bit 6) off  */
   write32(+0x280, read32(+0x280) & ~0x510)   /* class-1 IRQ (bit 8) off  */
   write32(+0x298, 0)                         /* channel descriptor       */
   write32(+0x28c, read32(+0x28c) | 0x20)     /* C3C bit 5 acknowledge    */
   ```

   If idle bit 1 never sets, nothing after the first store is written.
3. Re-reads all seven words and requires `(C3 & 0x141) == 0` (bits 0, 6, 8), `C3 & 0x2`,
   `+0x298 == 0`, `C3C & 0x20` and `(+0x8d0 & 0x40) == 0`.
4. Copies the image and does the unchanged mailbox/start stores. Then it samples `+0x8d0`
   31 times, each inside the guard and at least 5 ms apart (`usleep_range(5000, 6000)`). The
   span from the first to the last sample must be at least 150 ms, longer than the 16:27
   re-latch (~105 ms). Only the `before-run` checkpoint separates the last sample from the
   RUN store.
5. On pass, releases RUN exactly once. A failed boot is not retried and there is no
   automatic fallback.

Any failure keeps the old no-load/no-RUN behaviour and names its own reason:

| `failed:` reason | Where | Meaning |
|---|---|---|
| `failed-spi0-reset` | reset | unchanged: bit 6 survived the reset, or a reset poll timed out |
| `failed-ch3-teardown-checkpoint` | teardown | the begin line could not be flushed; channel 3 untouched |
| `failed-ch3-teardown-guard` | teardown | AOP domains not verifiably on; channel 3 untouched |
| `failed-ch3-teardown-idle` | teardown | C3 bit 1 never set within 1 ms; only the stop store was written |
| `failed-ch3-teardown-run` / `-class0` / `-class1` | teardown | C3 bit 0 / 6 / 8 still set |
| `failed-ch3-teardown-desc` | teardown | `+0x298` not 0 |
| `failed-ch3-teardown-ack` | teardown | C3C bit 5 not set |
| `failed-ch3-teardown-pending` | teardown | `+0x8d0` bit 6 still set after the acknowledge |
| `failed-ch3-reasserted` | window | bit 6 came back after the copy (replaces `failed-spi0-reasserted`) |
| `failed-ch3-window-guard` | window | a sample's guard was refused |
| `failed-ch3-window-short` | window | 31 samples took less than 150 ms (bad clock; cannot happen with `usleep_range`) |

Teardown failures stop before the load, the image copy and RUN. Window failures stop after
the copy but before RUN and auto-boot. In both cases the AOP stays halted.

### Log lines (prefix `SPI0 CH3TD`)

In order, after `SPI0 pmgr ps_spi0 …`:

- `SPI0 CH3TD begin pa=210100280 cpu=halted: first channel-3 store follows`, then
  `COLD checkpoint=spi0-ch3-teardown …`.
- `SPI0 CH3TD before c3=… route=… rate=… c3c=… desc=… 8d0=… 8d4=…`.
- `SPI0 CH3TD writes stop=… idle_poll=… poll_errno=0 class0=… class1=… desc=00000000 c3c=…`.
  On an idle timeout: `… poll_errno=-110 masks=skipped: idle bit 1 never set, no further store`.
- `SPI0 CH3TD after c3=… route=… rate=… c3c=… desc=… 8d0=… 8d4=…`.
- `SPI0 CH3TD check errno=<e> run=<0|1> class0=… class1=… idle=… desc=… ack=… ch3_pending=… result=<pass|failed-ch3-teardown-*>`.
- After the copy, only on a re-latch: `SPI0 CH3TD reasserted sample=<i> at_ms=<t> c3=… route=…
  rate=… c3c=… desc=… 8d0=… 8d4=… cpu=halted` (the channel-3 words read in the same guard).
- `SPI0 CH3TD window errno=<e> samples=<n>/31 elapsed_ms=<t> min_ms=150 result=<pass|failed-ch3-*>`.
- The unchanged `SPI0 pre-run errno=… 8d0=… ch3_pending=… cpu=halted` (the last sample).
- debugfs `state` gains `spi0_ch3 ran= result= errno= poll_errno= before_c3= before_c3c=
  before_desc= before_8d0= after_c3= after_c3c= after_desc= after_8d0= window= samples=
  elapsed_ms= reasserted= reassert_sample= reassert_ms=`.

Expected readbacks from the RE (16:27 post-reset words): `C3 00010447 → stop 00010046 →
class0/class1 00010006`, `C3C 00200009 → 00200029`, `desc <old> → 00000000`, `8d0 00000000`.
C3 bits 10 and 16 and C3C bits 0 and 21 are unnamed. The driver and the checker require only
the bits listed above; record the whole words.

`check-spi0-reload.py --ch3td` adds these requirements to the wake2 checks: exactly one
before/writes/after/window line, the flushed teardown begin after `SPI0 reset errno=0`, the
check line `errno=0 run=0 class0=0 class1=0 idle=1 desc=00000000 ack=1 ch3_pending=0
result=pass`, the written words' bits, the readback bits, `window errno=0 samples=31/31` with
`elapsed_ms >= 150` before `SPI0 pre-run errno=0` and `AOP released`, and no `failed-ch3`,
`CH3TD reasserted` or `masks=skipped`.

### Entry (`source build/periph/aop-wake3/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-ch3td-accel-63eadc3456` | `AOP_CH3TD_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake3/m1n1-aop-ch3td-accel-63eadc3456.bin`, entry dir
`build/periph/aop-wake3/aop-ch3td-accel-63eadc3456/`. Same cmdline as the spi0b entry
apart from the entry name: deferred test5 (`attach_stage=3 aop_force_cold=1 aop_cold_test=5
aop_defer=1`), the full kexecfix set, exactly one `pcie_apple_h9p.ans_keep_power=1`, debug
info off, 938 bytes. The four private root files are byte-identical to aop-wake2's.

### Exact orchestrator test: one DFU, cold → stream → kexec into the same entry → teardown reload

This is the 16:27 flow with the new entry. The builder ran none of it. Keep the root server
and the default entry; nothing sets a default.

**A. DFU cold start in the new entry.**

```sh
source build/periph/aop-wake3/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TD_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake3/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
# Expect "EXPERIMENT spi0_reset=1 wake_wrapper=0 ... spi0_applies=live-reload spi0_pmgr=ps_spi0"
# and normally "AOP-PREV shutdown transcript missing" + "SPI0 evidence prev=missing ...".
# Staged now: leg B kexecs into this same entry.
scripts/boot/upload-entry.sh "$AOP_CH3TD_ACCEL_ENTRY" "$AOP_CH3TD_ACCEL_DIR"
C=build/periph/aop-wake3/ch3td-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3td-cold
grep -E 'SPI0 ' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3td-cold-tilt
```

Expect exactly one `SPI0 reset decision=skip reason=fresh-instance auto_boot=0
out4020=00020001 prev=missing …: block not touched`, and **no** `SPI0 reset begin` and no
`SPI0 CH3TD` line: a DFU cold start never touches SPI0. As before, `prev=valid` with
`reason=stale-transcript` or `reason=prev-no-run` is also a pass; record it. Any
`decision=run` or `SPI0 CH3TD` line here is a fail: stop and keep `$C`. Go on only if the cold
run passes the stage-3 criteria: streaming, `cal_sent=3 cal_unsupported=1 cal_state=2`, IIO,
tilt.

**B. Kexec into the same entry, preflight, trigger.**

```sh
# (no dmesg -n 8: never raise the console level, see the transcript section)
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TD_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake3/ch3td-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TD_ACCEL_ENTRY"
```

Preflight must show the same lines as 16:27:

- `AOP-PREV SHUTDOWN begin mode=quiesce entry=aop-ch3td-accel-63eadc3456 … hello=1 phase=streaming`;
- `AOP-PREV SPI0-CARRY retained=1 source=hello`;
- `AOP-PREV QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`;
- `EXPERIMENT spi0_reset=1 wake_wrapper=0 … spi0_applies=live-reload spi0_pmgr=ps_spi0`;
- `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1`;
- `check-warm-shutdown.py` PASS.

If the evidence line shows `prev=missing`, `prev=invalid` or `prev_hello=0 prev_carry=0`, do
not trigger: the decision would be a skip. Keep `$Q` and report. Otherwise trigger **once**:

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3td-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP released|failed|STAGE3 checkpoint' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'SPI0 CH3TD' "$Q/trigger/dmesg.txt" | tee "$Q/ch3td-lines.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|PC=' "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TD_ACCEL_ENTRY" --reason prev-quiesce-halted --ch3td
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3td-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass (B)** (section 5): `check-spi0-reload.py --reason prev-quiesce-halted --ch3td` prints
both PASS lines, with these lines in order: `FORCE-COLD`, `COLD halt errno=0`, `SPI0 reset
decision=run reason=prev-quiesce-halted …`, `SPI0 reset begin`, `COLD checkpoint=spi0-reset`,
`SPI0 reset errno=0 ran=1`, `SPI0 reset after … ch3_pending=<1|0>->0`, `SPI0 CH3TD begin`, `COLD
checkpoint=spi0-ch3-teardown`, `SPI0 CH3TD before/writes/after`, `SPI0 CH3TD check errno=0 …
result=pass`, `SPI0 CH3TD window errno=0 samples=31/31 elapsed_ms=<≥150> … result=pass`,
`SPI0 pre-run errno=0 … ch3_pending=0`, `AOP released`. Specifically:

- The idle poll succeeds (`poll_errno=0`, `idle_poll` has bit 1).
- The exact readbacks match: C3 bits 0/6/8 clear and bit 1 set, `desc=00000000`, C3C bit 5 set,
  `8d0` bit 6 clear. Record the words against the prediction `00010447 → 00010046 → 00010006`,
  `00200009 → 00200029`.
- `+0x8d0` bit 6 stays zero through the ≥150 ms pre-RUN window.
- After release: HELLO, EPMAP, system-ready and 3 grants, then the accel STARTEP gets its
  normal `0x20` ACK (where 6/6 pre-wake2 reloads crashed). Then 3 calibration ACKs, `0x18`
  unsupported, `0x3d=2`, rates, first report, IIO, and streaming with report/ping counters
  increasing for 60 s. The AOP syslog line `mpu6900 device ID 0x10 found` should appear
  (`$Q/mpu-lines.txt`); the accel samples are the deciding evidence.
- Tilt changes X/Y/Z. There is no crashlog.
- Record the `spi0_reset` and `spi0_ch3` debugfs lines (`$Q/spi0-state.txt`).

This proves the retained cause was the old channel-3 engine/IRQ configuration, and that the
teardown is sufficient for a production reload.

**Fail (B)**. Each of these retains the no-RUN behaviour and must not be retried:

- `decision=skip` despite a clean preflight: the test is void, not an SPI0 result. Report
  the reason and `out4020`.
- `failed-spi0-reset`: unchanged reset failure (bit 6 survived it, or a poll timed out).
- `failed-ch3-teardown-idle`: idle bit 1 did not assert within the bounded poll.
- `failed-ch3-teardown-run|class0|class1|desc|ack|pending`: a write did not read back, or
  bit 6 was still pending after the acknowledge. Keep the before/writes/after lines.
- `failed-ch3-reasserted`: `+0x8d0` bit 6 re-latched while C3 bit 6 was masked. Keep the
  `SPI0 CH3TD reasserted sample=… at_ms=…` snapshot and stop. Per section 5, the next round
  recovers the remaining channel neighbour/FIFO registers. It does not add guessed FIFO or raw
  MPU writes.
- `failed-ch3-teardown-checkpoint|guard`, `failed-ch3-window-guard|short`: environment
  refusals, not SPI0 results. Report them.
- After `AOP released`, the fresh firmware crashes (`AOP crashlog`, especially `PC=0x0102f252
  DFAR=0x4c r5=0`, the NULL `table[3]` load) before `table[3]` allocation.
- HELLO succeeds but the MPU probe or streaming fails (no `mpu6900 … found`, accel STARTEP
  error, or no reports).
- A kernel fault or hang right after `SPI0 CH3TD begin` / `COLD checkpoint=spi0-ch3-teardown`
  with no `SPI0 CH3TD before` line: a channel-3 access was not tolerated. Collect the ramoops
  evidence with `collect-hang-evidence.sh` after the DFU.

In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake3.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake3
```

The workspace sources `scripts/periph/aop/kernel/{apple-a9-aop-active.c,a9-aop-reload.h}`
now carry the teardown. Re-running `build-wake2.sh` would therefore commit it onto
`a9/aop-wake2`; build the old entry from `a9/aop-wake2` as it is.

`build-wake3.sh` has the same steps as `build-wake2.sh`. `/src` was 98% full, so instead of
checking out a second tree it moves the clean `/src/wt/aop-wake2` worktree (still exactly at
`61561869d0`) to `/src/wt/aop-wake3` and branches there. Branch `a9/aop-wake2` is untouched;
recreate its worktree with `git worktree add` if it is needed again. New offline coverage in
`tests/force-cold.c`, which runs the production functions against a channel-3 model that
starts from the 16:27 words (`C3 18011445`, reset → `00010447`, `C3C 00200009`) with a level
source that re-latches bit 6 at 100 ms while C3 bit 6 is on:

- exact stores `+0x280 00010046, 00010006, 00010006`, `+0x298 0`, `+0x28c 00200029` after
  the 110/10/11 reset and its own flushed begin line, inside the guard with the CPU halted;
  route/rate/`8d0`/`8d4` are never written; none of it after a failed reset, failed flush or
  refused guard;
- each readback refusal (idle timeout with one store, stuck run/class0/class1 bit, descriptor,
  acknowledge, still-pending) returns nonzero with its own reason; a latch between the reset
  and the teardown is acknowledged and passes;
- the loader takes 31 guarded samples over exactly 150 ms (fake clock), the level source
  stays quiet because C3 bit 6 is off, and RUN is written once;
- a re-latch despite the mask at 100 ms (sample 20) or at the last sample (150 ms), or with
  class-0 re-enabled, refuses with `failed-ch3-reasserted`, the sample/time and the register
  snapshot, and no RUN or auto-boot; a refused sample guard or a 4 ms clock also refuses.

`check-spi0-reload.py --self-test` covers the `--ch3td` fixture and 16 rejected variants. The
real 16:27 log is rejected (`failed-spi0`).

**Firmware replies and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 63 offline PASS lines in `offline.log`: 60 in `test-offline.py` (56 before: three new
  force-cold groups and the `--ch3td` checker self-test), the two checker self-test lines run
  again on their own, and the `--wake3` plan check.
- W=1 clean; checkpatch 0 errors/0 warnings (320 lines).
- Full Image.gz plus 105 DTBs, byte-identical to tri49a's.
- `check-handoff-build.py --wake3` PASS: clean branch, only the two files differ from
  `61561869d0`, kexecfix PCIe source, config, DTBs, every `SPI0 CH3TD` format and
  `failed-ch3-*` reason linked, `failed-spi0-reasserted` gone. The three pstore recovery
  fixtures also pass.
- Extraction, checksum, firmware, ECID and cmdline checks of the payload
  (`stage3-package.py --wake3`). The cmdline equals the spi0b entry's apart from the entry
  name.
- `make clean` brought the tree from 2.3 GB to 1.8 GB; `/src` has 2.6 GB free.
- `root-firmware/SHA256SUMS` is identical to aop-wake2's.

Logs are under `build/periph/aop-wake3/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `wake3.patch` and `manifest-wake3.json`. Payload SHA256:

- `m1n1-aop-ch3td-accel-63eadc3456.bin`: `4cf461c20e90f9fe4bed74d3a2bc2ffb068b34c265c78803a89f111f17cb13ce`

### 2026-10-26 17:35 DFU: cold pass, outgoing quiesce failed (aop-ch3td-accel-63eadc3456)

Logs: `logs/aop-ch3td-20261026/`. **Leg A passed:** firmware hashes matched, `SPI0 evidence
prev=missing`, `SPI0 reset decision=skip reason=fresh-instance auto_boot=0 out4020=00020001`, no
`SPI0 CH3TD` line, AOP ready, IIO, accel streaming at 100 ms, live tilt (x 110 → ≈19000).
**Leg B never reached the trigger.** After about 4 minutes of streaming, with `dmesg -n 8` run
just before `linux-a9-kexec reboot`, the outgoing quiesce sent all 17 AFK stops (status 0), but
only 0x20..0x27 reached `state=stopped`. 0x28..0x2d and 0x30..0x32 stayed `stopping`:
`QUIESCE step=complete status=-110 … steps=0x7 failed_step=afk-acked`. The new kernel saw
`prev_halted=0 prev_steps=0x7`, `check-warm-shutdown.py` refused, and nothing was triggered.
The channel-3 teardown was not exercised. Diagnosis and fix: `a9/aop-wake4` below.

## Sequential AFK quiesce (`a9/aop-wake4`)

Kernel `9ab7e00531a75e9f31f500a1ec51ac1ee349e83d`, one commit on the new branch `a9/aop-wake4`, on top of `a9/aop-wake3`
`63eadc3456`. Release `7.3.0-rc1-aop-wake4-g9ab7e00531a7`. Only `apple-a9-aop-active.c` differs from
`63eadc3456`. The channel-3 teardown and `a9-aop-reload.h` are byte-identical to wake3.
**No device operation was performed; nothing below is a hardware result.**

### Diagnosis of 17:35

The pre-fix quiesce sent AFK `0xa4` to every running endpoint back to back while holding the
lock, with no RX drain in between, then waited 1 s in aggregate for every `0x87`.

- **The AOP→AP outbox FIFO holds 8 words.** Six stale-FIFO captures stop at
  `COLD stale fifo=outbox index=7`, and `stale_outbox` never exceeds 8. At 17:35 exactly 8
  endpoints acked (0x20..0x27, the first 8 sent) and the 9 later ones did not.
- **All 17 sends returned status 0.** The AP→AOP inbox never stayed full for 20 ms. Note that
  0x29..0x32 is exactly 8 words, so a firmware that consumed 0x28's stop and then blocked on
  its full outbox would also leave every send succeeding. The replies past the 8th were
  **dropped or stalled**; the transcript cannot tell which, and the fix is the same.
- **The one procedural difference from 16:27 (same code, all 17 acked) was `dmesg -n 8` just
  before the kexec.** With `cold_run`, `startup_count` was 270 < 512, so every mailbox word also
  printed a `stage3 R/T` line. Each send therefore printed two console lines (trace and
  transcript), synchronously, to the 192x128 fbcon plus netconsole and ramoops. Measured on this
  iPad at boot with both consoles live, consecutive printk lines are 2.3–4 ms apart (median
  3.0 ms, n=682), against 0.024 ms before fbcon took over. That turns the burst from well under
  a millisecond into about 100 ms, during which nothing drained the outbox. The shutdown-time
  console cost itself is inferred from the boot measurement, not measured.
- Nothing distinguishes 0x28: only 0x21 (accel) streams, and the other endpoints carried one
  INIT/START exchange each. The cut is positional (9th reply), not per-endpoint.

The host model (`tests/shutdown.c`) reproduces both runs with the pre-fix loop. It uses an
8-word outbox that loses words posted while full, and a firmware that handles stops serially
at 2 ms each. With a quiet console every ACK arrives (16:27). With 6 ms per send and 3 ms per
received word, exactly 0x20..0x27 stop and 0x28..0x32 stay stopping, with 9 replies lost (17:35).

**Still to confirm on the device:** the outgoing kernel's console was saved by the current
kernel to `/run/linux-a9/pstore/console-ramoops-0` (tmpfs, gone after DFU). Its `stage3 T
xxa4…` / `stage3 R xx87…` lines show the send spacing and which ACKs arrived. Step 0 below
pulls it.

### The fix

- **iOS `AppleSPU_stopEndpoint` order** (`a9_aop_quiesce_afk`): for each running endpoint,
  drain RX inline, send one `0xa4`, then service RX inline until that endpoint's
  `0x87`/`0xc3`. At most one stop is outstanding, so its ACK always has room in the outbox.
- **Bounded:** 2 s per endpoint (`AOP_QUIESCE_EP_MS`, was a 1 s aggregate for all 17) and 10 s
  for the whole sequence (`AOP_QUIESCE_AFK_MS`). Inline RX keeps the watchdog pings going.
- **First missing ACK ends the sequence:** no further stop, no disable, no power step, and
  **no `0xa4` resend**. There is no firmware evidence that a second stop to a stopping endpoint
  is tolerated; that is an open RE question (the firmware AFK `0xa4` handler for a
  STOPPING/STOPPED endpoint). `failed_step=afk-acked` as before.
- The 17 `endpoint-disable` STARTEP sends drain RX before each word, since unsolicited words
  could still fill 8 slots during a slow burst. The AP- and IOP-power waits get the same 2 s
  bound (were 1 s).
- The default (non-quiesce) shutdown stops endpoints the same way (best effort, 200 ms each,
  warn only).
- The per-word console trace is kept: with one stop outstanding it costs about four lines per
  endpoint, and it is evidence.

### Transcript lines

- `QUIESCE step=afk-send ep=0x.. status=0`, unchanged, now interleaved with its ACK line.
- `QUIESCE step=afk-ack ep=0x.. state=stopped ms=<ack latency> rx=<words received while
  waiting>`. The `ms=`/`rx=` fields are new; the checkers still match the `state=stopped`
  prefix.
- On a miss: `QUIESCE step=afk-timeout ep=0x.. limit_ms=… rx_halt=… crashed=… fault=…`, then
  `QUIESCE step=afk-unsent ep=0x.. state=running` for each later endpoint. Also
  `QUIESCE step=afk-budget …` (10 s total exhausted) and `QUIESCE step=afk-unstopped …` (a
  failed endpoint). `check-warm-shutdown.py` and `check-handoff.py` reject all four.
- A clean run still ends with `steps=0x1bff failed_step=none`.

### Entry (`source build/periph/aop-wake4/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-ch3tdq-accel-9ab7e00531` | `AOP_CH3TDQ_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake4/m1n1-aop-ch3tdq-accel-9ab7e00531.bin`, entry dir
`build/periph/aop-wake4/aop-ch3tdq-accel-9ab7e00531/`. Same cmdline as the ch3td entry
apart from the entry name (939 bytes): deferred test5, the full kexecfix set, exactly one
`pcie_apple_h9p.ans_keep_power=1`, debug info off. The four private root files are
byte-identical to aop-wake3's.

### Exact orchestrator test: one DFU, cold → stream → kexec into the same entry → teardown reload

**0. Before the DFU, on the idle iPad (current kernel):** keep the 17:35 outgoing console.

```sh
E=docs/periph/logs/aop-ch3td-20261026
scripts/omarchy/ssh.sh 'sudo -n ls -la /run/linux-a9/pstore/'
scripts/omarchy/ssh.sh 'sudo -n cat /run/linux-a9/pstore/console-ramoops-0' > "$E/prev-console-ramoops-0.txt"
grep -nE 'QUIESCE|stage3 [RT] (2[0-9a-f]|3[0-2])(a4|87)' "$E/prev-console-ramoops-0.txt" | tail -80
```

Expected if the diagnosis holds: `stage3 T 20a4…` … `T 32a4…` spaced several ms apart, and
`stage3 R xx87…` only for 0x20..0x27. Any `R 2887…` or later in the file would mean the reply
arrived and was not processed, which contradicts the diagnosis: report it.

**A. DFU cold start in the new entry.** This is the wake3 leg A with the new variables. Expect
the same results (`decision=skip reason=fresh-instance`, no `SPI0 CH3TD` line, streaming,
`cal_sent=3 cal_unsupported=1 cal_state=2`, IIO, tilt).

```sh
source build/periph/aop-wake4/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TDQ_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake4/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
scripts/boot/upload-entry.sh "$AOP_CH3TDQ_ACCEL_ENTRY" "$AOP_CH3TDQ_ACCEL_DIR"
C=build/periph/aop-wake4/ch3tdq-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdq-cold
grep -E 'SPI0 ' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdq-cold-tilt
```

**B. Kexec into the same entry. Never raise the console level** (no `dmesg -n 8` in either
kernel). The reserved transcript does not depend on the console level, and the fix is
host-tested at `-n 8` anyway.

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TDQ_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake4/ch3tdq-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(afk-|halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TDQ_ACCEL_ENTRY"
```

Preflight must show:

- `AOP-PREV SHUTDOWN begin mode=quiesce entry=aop-ch3tdq-accel-9ab7e00531 … hello=1 phase=streaming`;
- `AOP-PREV SPI0-CARRY retained=1 source=hello`;
- 17 pairs `AOP-PREV QUIESCE step=afk-send ep=0x.. status=0` /
  `AOP-PREV QUIESCE step=afk-ack ep=0x.. state=stopped ms=<n> rx=<n>`, 0x20..0x2d and
  0x30..0x32, each send directly followed by its ACK. Record the `ms=` values: they are the
  first measured AFK stop latencies;
- `AOP-PREV QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`;
- `EXPERIMENT spi0_reset=1 wake_wrapper=0 … spi0_applies=live-reload spi0_pmgr=ps_spi0`;
- `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1`;
- `check-warm-shutdown.py` PASS.

If the quiesce failed again, the transcript now names the endpoint (`afk-timeout ep=…
limit_ms=… rx_halt=… crashed=… fault=…`, then `afk-unsent` lines). Do not trigger. Keep `$Q`,
pull `/run/linux-a9/pstore/console-ramoops-0` as in step 0, and report. Otherwise trigger
**once**. (Superseded at 19:38: the `dmesg -n 8` this step used to start with flooded the
console, starved IRQs and voided the run. It is removed.)

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdq-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP released|failed|STAGE3 checkpoint' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'SPI0 CH3TD' "$Q/trigger/dmesg.txt" | tee "$Q/ch3td-lines.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|PC=' "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TDQ_ACCEL_ENTRY" --reason prev-quiesce-halted --ch3td
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdq-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass/fail (B)** are exactly the wake3 criteria above (section 5 of
`ds-aop-spi0-ch3-round3.md`): the idle poll succeeds, the readbacks match, `+0x8d0` bit 6 stays
zero through the ≥150 ms window, then HELLO, the accel STARTEP `0x20` ACK, calibration, IIO,
streaming for 60 s and tilt with no crashlog. The fail forms are the same too (`failed-ch3-*`,
a crash with `PC=0x0102f252 DFAR=0x4c`, an MPU probe/streaming failure, a hang after `SPI0 CH3TD
begin`). In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake4.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake4
```

`build-wake4.sh` moves the clean `/src/wt/aop-wake3` worktree (at `63eadc3456`) to
`/src/wt/aop-wake4` and branches there; branch `a9/aop-wake3` is untouched. `/src` had
filled to 206 MB free, so the clean, superseded worktrees `/src/wt/aop-wake` and
`/src/wt/aop-handoff` were removed with `git worktree remove` (no `--force`), freeing 3.6 GB.
Their branches stay at `f695e71125` and `621fe3f144`, and `build-kernel.sh --tree <name>`
recreates either one from its branch. Only
`apple-a9-aop-active.c` may differ from the parent, and both reload/handoff headers must
byte-match it. New offline coverage in `tests/shutdown.c` (with an optional outbox depth,
per-send/per-word console cost and serial-firmware model added to `tests/stage3.c`; all are
off for the existing replays):

- the pre-fix burst reproduces 16:27 (quiet console: all 17 ACKs) and 17:35 (`-n 8`: exactly
  0x20..0x27 stopped, 0x28..0x32 stopping, 9 replies lost);
- production under the 17:35 conditions, with all 17 endpoints running, completes to
  halt/drain with `steps=0x1bff`, no reply lost, and never more than one stop outstanding;
- an ACK at 1.5 s passes (the old 1 s aggregate would have failed);
- a missing ACK at 0x28 fails `afk-acked` after 9 stops, with no disable or power step,
  within 2 s; ACKs at 700 ms each trip the 10 s total; the existing missing-ACK mode now stops
  after 2 stops;
- the default-off path under the same conditions stops all 17 endpoints with nothing lost.

**Firmware replies are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 65 offline PASS lines in `offline.log`: 61 in `test-offline.py` (60 before; the new
  16:27/17:35 shutdown group), plus the SPI0 and shutdown checker self-tests run again on their
  own, and the `--wake4` plan check.
- W=1 clean; checkpatch 0 errors/0 warnings (189 lines).
- Full Image.gz plus 105 DTBs, byte-identical to tri49a's.
- `check-handoff-build.py --wake4` PASS: one commit on a clean branch, only the active driver
  differs from `63eadc3456`, kexecfix PCIe source, config, DTBs, the new `afk-ack … ms= rx=`,
  `afk-timeout`, `afk-budget`, `afk-unsent` and `afk-unstopped` formats linked and the old
  `afk-ack` format gone, every wake3 `failed-ch3-*` reason still linked. The three pstore
  recovery fixtures also pass.
- Extraction, checksum, firmware, ECID and cmdline checks of the payload
  (`stage3-package.py --wake4`). The cmdline equals the ch3td entry's apart from the entry
  name.
- `make clean` left the tree at 1.8 GB; `/src` had 2.0 GB free afterwards.
- `root-firmware/SHA256SUMS` is identical to aop-wake3's.

Logs are under `build/periph/aop-wake4/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `wake4.patch` and `manifest-wake4.json`. Payload SHA256:

- `m1n1-aop-ch3tdq-accel-9ab7e00531.bin`: `dd0538797676f3cb5aec3da0d0cbc648f4e1dce12212a5b008790688f71476f4`

### 2026-10-26 19:50 DFU: quiesce pass, teardown refused on the C3C readback (aop-ch3tdq-accel-9ab7e00531)

Logs: `logs/aop-ch3tdq-20261026/`. A first attempt at 19:38 was void: `dmesg -n 8` in the new
kernel before the trigger flooded the console, starved IRQs and produced NVMe completion-polled
timeouts. `dmesg -n 8` is now removed from every procedure in this file.

- **Cold leg passed:** `decision=skip reason=fresh-instance`, no `SPI0 CH3TD` line, AOP ready,
  streaming, live tilt (flat → upright y≈-61000).
- **Sequential quiesce passed:** 17 × `afk-ack … state=stopped ms=0 rx=1`, so each AFK stop
  ACK arrived within the first 1 ms poll and was the only word received.
  `QUIESCE step=complete status=0 … steps=0x1bff`, `check-warm-shutdown.py` PASS, `SPI0 evidence
  prev=valid prev_hello=1 prev_halted=1 prev_carry=1`.
- **Reload:** `decision=run reason=prev-quiesce-halted`; reset `8d0 0x4040 → 0`, `ch3 18011445 →
  00010447`, `ch3_pending 1->0`. Teardown:
  - `before c3=01011445 rate=019000c8 c3c=00200009 desc=00000000 8d0=00000040`: bit 6 had
    re-pended between the reset and the teardown, the case the host test anticipated;
  - `writes stop=01011044 idle_poll=02010446 poll_errno=0 class0=02010006 class1=02010006
    desc=00000000 c3c=00200029`;
  - `after c3=00010406 c3c=00200009 desc=00000000 8d0=00000000`;
  - `check errno=-5 run=0 class0=0 class1=0 idle=1 desc=00000000 ack=0 ch3_pending=0
    result=failed-ch3-teardown-ack`, so RUN was refused, with no crash.
- **Finding: C3C bit 5 is a self-clearing pulse.** `0x00200029` written, `0x00200009` read
  back, and the acknowledge worked (`+0x8d0` 0x40 → 0). Every other gate held. C3 status bits
  also move under the stores: bit 24 → 25 at the idle poll, bit 25 gone and bit 10 back by the
  final readback. Neither is gated.

## Pulse acknowledge (`a9/aop-wake5`)

Kernel `cecb33eef51a293a7e1a301b74488c8217a1ae30`, one commit on the new branch `a9/aop-wake5`, on top of `a9/aop-wake4`
`9ab7e00531`. Release `7.3.0-rc1-aop-wake5-gcecb33eef51a`. Only `apple-a9-aop-active.c` and `a9-aop-reload.h`
differ from `9ab7e00531`. **No device operation was performed; nothing below is a hardware
result.**

### Change

- The teardown no longer requires C3C bit 5 to read back set. The acknowledge is judged by
  `+0x8d0` bit 6 == 0 after the writes (the existing `failed-ch3-teardown-pending` gate),
  followed by the unchanged 31-sample, ≥150 ms pre-RUN window (`failed-ch3-reasserted`).
  `failed-ch3-teardown-ack` no longer exists.
- The C3C readback is still recorded: the `after … c3c=` word, the debugfs `after_c3c=`, and the
  check line field `c3c_bit5=<0|1>`, which replaces `ack=`. `check-spi0-reload.py --ch3td`
  requires the written C3C word to carry bit 5 and `c3c_bit5` to agree with the readback, and
  accepts either value.
- Unchanged: the reset and its refusals, the flushed begin line, the guarded RMW stores, the
  1 ms idle poll, the run/class-0/class-1/idle/descriptor/pending gates and their reasons, the
  window, RUN once with no retry, the sequential AFK quiesce, `a9/kexecfix` and
  `ans_keep_power=1`.

Check line on pass, as 19:50 would now read:

```text
SPI0 CH3TD check errno=0 run=0 class0=0 class1=0 idle=1 desc=00000000 c3c_bit5=0 ch3_pending=0 result=pass
```

### Entry (`source build/periph/aop-wake5/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-ch3tdp-accel-cecb33eef5` | `AOP_CH3TDP_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake5/m1n1-aop-ch3tdp-accel-cecb33eef5.bin`, entry dir
`build/periph/aop-wake5/aop-ch3tdp-accel-cecb33eef5/`. Same cmdline as the ch3tdq entry
apart from the entry name (939 bytes): deferred test5, the full kexecfix set, exactly one
`pcie_apple_h9p.ans_keep_power=1`, debug info off. The four private root files are
byte-identical to aop-wake4's.

### Exact orchestrator test: one DFU, cold → stream → kexec into the same entry → teardown reload

**Never run `dmesg -n 8`**, in either kernel, at any point.

**A. DFU cold start in the new entry.**

```sh
source build/periph/aop-wake5/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TDP_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake5/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
scripts/boot/upload-entry.sh "$AOP_CH3TDP_ACCEL_ENTRY" "$AOP_CH3TDP_ACCEL_DIR"
C=build/periph/aop-wake5/ch3tdp-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdp-cold
grep -E 'SPI0 ' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdp-cold-tilt
```

Expect `SPI0 reset decision=skip reason=fresh-instance auto_boot=0 out4020=00020001 prev=missing
…: block not touched`, no `SPI0 reset begin` and no `SPI0 CH3TD` line, then streaming,
`cal_sent=3 cal_unsupported=1 cal_state=2`, IIO and tilt. Any `decision=run` or `SPI0 CH3TD`
line here is a fail: stop and keep `$C`.

**B. Kexec into the same entry, preflight, trigger once.**

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TDP_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake5/ch3tdp-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(afk-|halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TDP_ACCEL_ENTRY"
```

Preflight must show, as at 19:50:

- `AOP-PREV SHUTDOWN begin mode=quiesce entry=aop-ch3tdp-accel-cecb33eef5 … hello=1 phase=streaming`;
- `AOP-PREV SPI0-CARRY retained=1 source=hello`;
- 17 × `afk-send … status=0` each followed by `afk-ack … state=stopped ms=<n> rx=<n>` (19:50:
  `ms=0 rx=1`);
- `AOP-PREV QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`;
- `EXPERIMENT spi0_reset=1 wake_wrapper=0 … spi0_applies=live-reload spi0_pmgr=ps_spi0`;
- `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff prev_carry=1`;
- `check-warm-shutdown.py` PASS.

If any of these is missing, do not trigger; keep `$Q` and report. Otherwise trigger once:

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdp-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP released|failed|STAGE3 checkpoint' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'SPI0 CH3TD' "$Q/trigger/dmesg.txt" | tee "$Q/ch3td-lines.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|PC=' "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TDP_ACCEL_ENTRY" --reason prev-quiesce-halted --ch3td
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdp-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass (B)** (section 5 of `ds-aop-spi0-ch3-round3.md`, with the pulse acknowledge):
`check-spi0-reload.py --reason prev-quiesce-halted --ch3td` prints both PASS lines. In order:
`SPI0 reset errno=0 ran=1`, `SPI0 CH3TD begin`, `COLD checkpoint=spi0-ch3-teardown`,
`before/writes/after`, then `SPI0 CH3TD check errno=0 run=0 class0=0 class1=0 idle=1
desc=00000000 c3c_bit5=<0|1> ch3_pending=0 result=pass`, `SPI0 CH3TD window errno=0
samples=31/31 elapsed_ms=<≥150> … result=pass`, `SPI0 pre-run errno=0 … ch3_pending=0`,
`AOP released`. After release: HELLO, EPMAP, system-ready and 3 grants, the accel STARTEP
`0x20` ACK, 3 calibration ACKs, `0x18` unsupported, `0x3d=2`, rates, first report, IIO,
streaming with report/ping counters rising for 60 s, `mpu6900 device ID 0x10 found` (expected,
`$Q/mpu-lines.txt`), tilt changing X/Y/Z, and no crashlog. Record the before/writes/after
words, `c3c_bit5`, the window `elapsed_ms`, and the debugfs `spi0_reset`/`spi0_ch3` lines.

**Fail (B)** keeps the no-RUN behaviour and is never retried:

- `failed-ch3-teardown-pending`: the acknowledge did not clear `+0x8d0` bit 6;
- `failed-ch3-reasserted` (with `SPI0 CH3TD reasserted sample=… at_ms=…`): bit 6 came back
  within the window while C3 bit 6 was masked. Stop; per section 5 the next round recovers the
  remaining channel/FIFO registers, with no guessed writes;
- `failed-ch3-teardown-idle|run|class0|class1|desc`, `failed-spi0-reset`;
- `failed-ch3-teardown-checkpoint|guard`, `failed-ch3-window-guard|short`: environment
  refusals, report them;
- after `AOP released`: a crashlog (especially `PC=0x0102f252 DFAR=0x4c`), or HELLO without
  MPU probe/streaming;
- a hang after `SPI0 CH3TD begin` with no `before` line: collect the ramoops evidence after the
  DFU.

In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake5.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake5
```

`build-wake5.sh` moves the clean `/src/wt/aop-wake4` worktree (at `9ab7e00531`) to
`/src/wt/aop-wake5` and branches there; branch `a9/aop-wake4` is untouched. New offline
coverage:

- `tests/force-cold.c`: the C3C model is now a pulse by default, with a latching variant. A
  scripted C3 read sequence replays the 19:50 trace word for word (`01011445, 01011445,
  02010446, 02010446, 02010006, 00010406`, bit 6 re-pended before the teardown, desc 0). The
  production code writes exactly `01011044, 02010006, 02010006, 0, 00200029`, reads C3C
  `00200009` and `+0x8d0` 0, passes the teardown, the 31-sample 150 ms window and RUN once.
  The same trace with bit 6 still set afterwards refuses `failed-ch3-teardown-pending` before
  the copy. A latching bit 5 also passes. An acknowledge with no effect passes with nothing
  pending and refuses `-pending` when bit 6 is set.
- `check-spi0-reload.py --self-test`: the fixture is now the 19:50 words, with
  `c3c_bit5=0` and `=1` both accepted. It rejects the wake4 check line (`ack=0 …
  failed-ch3-teardown-ack`), an unwritten bit 5, a `c3c_bit5` that disagrees with the
  readback, and all the earlier bad variants.

**Firmware and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 66 offline PASS lines in `offline.log`: 62 in `test-offline.py` (61 before; the new 19:50
  trace group), plus the SPI0 and shutdown checker self-tests run again on their own, and the
  `--wake5` plan check.
- W=1 clean; checkpatch 0 errors/0 warnings (47 lines).
- Full Image.gz plus 105 DTBs, byte-identical to tri49a's.
- `check-handoff-build.py --wake5` PASS: one commit on a clean branch, only the active driver
  and the reload header differ from `9ab7e00531`, kexecfix PCIe source, config, DTBs,
  `c3c_bit5=` linked, `failed-ch3-teardown-ack` gone, every other `failed-ch3-*` reason and the
  sequential-quiesce lines linked. The three pstore recovery fixtures also pass.
- Extraction, checksum, firmware, ECID and cmdline checks of the payload
  (`stage3-package.py --wake5`). The cmdline equals the ch3tdq entry's apart from the entry
  name.
- `make clean` left the tree at 1.8 GB; `/src` has 32 GB free.
- `root-firmware/SHA256SUMS` is identical to aop-wake4's.

Logs are under `build/periph/aop-wake5/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `wake5.patch` and `manifest-wake5.json`. Payload SHA256:

- `m1n1-aop-ch3tdp-accel-cecb33eef5.bin`: `79db9b905d3431584a995490d75459d8f8f2ef1d0baace8e3d445c67c44a35b2`

### 2026-10-26 20:35 DFU: first AOP restart after a warm reload (aop-ch3tdp-accel-cecb33eef5)

Logs: `logs/aop-ch3tdp-20261026/`, including `reload-crashlog-decoded.txt`. The quiesce
completed (`steps=0x1bff`), then the SPI0 reset ran. `SPI0 CH3TD check errno=0 … c3c_bit5=0
ch3_pending=0 result=pass` (the pulse acknowledge was accepted), `window errno=0 samples=31/31
elapsed_ms=182 … result=pass`, pre-run `8d0=0`, RUN released. The fresh firmware then reached
HELLO, EPMAP, the system endpoints, 3 grants, syslog, `SYSTEM ready IOP=20 AP=20`, and app
STARTEP 0x20..0x27 succeeded. **The accel STARTEP 0x28 timed out (-110) and the AOP crashed
with `SCMErr:0x3,3`**, stack `0x01033834/0x010333c6/0x01033236/0x01034a64/0x0102f1a2`. RE round 4
(`ds-aop-spi0-ch3-round4.md`) identifies this as SPI0 global IRQ `0x10014` still latched in
the AOP interrupt controller. It preempts SPI lifecycle event 1 before the firmware's own
SPI0 reset and MPU6900 init.

## Retained IRQ 0x10014 cleared, NVMe crawl fix (`a9/aop-wake6`)

Kernel `a72759f96388d68238f941d52434188db6352334`, the top of three commits on the new branch `a9/aop-wake6`, on top of
`a9/aop-wake5` `cecb33eef5`. Release `7.3.0-rc1-aop-wake6-ga72759f96388`. **No device operation was performed;
nothing below is a hardware result.**

| Commit | Subject | Origin |
|---|---|---|
| `56efc8218d` | nvme-pci: apple-ans: service every CQ on the shared MSI | `a9/kexec-irqfix` `7dcb18c411`, `-x`, reworded "All 13 logged" → "All 10 logged" |
| `e4c177ae28` | nvme-pci: apple-ans: optional rescue for unsignalled completions | `a9/kexec-irqfix` `e3ad1b3a5c`, `-x` |
| `a72759f963` | soc: apple-a9-aop: clear the retained SPI0 IRQ 0x10014 on warm reload | new |

Only `apple-a9-aop-active.c`, `a9-aop-reload.h` and `drivers/nvme/host/{pci.c,pci-internal.h}`
differ from `cecb33eef5`. The two NVMe files are byte-identical to `a9/kexec-irqfix`. The
channel-3 teardown, window, pulse acknowledge, sequential quiesce, `a9/kexecfix` PCIe code and
config are unchanged.

### Change (round 4, section 3 of `ds-aop-spi0-ch3-round4.md`)

1. **Disable** (in `a9_aop_spi0_prereset`, after `decision=run` and the halt/drain check,
   before `SPI0 reset begin`, CPU halted): `AOP INTC disable begin irq=0x10014 pa=210800a00
   value=00100000`, then the flushed `COLD checkpoint=intc-disable`, then
   `0x210800a00 = 0x00100000` through the forced-cold control write (`COLD-W/COLD-R` with
   before/after), then `AOP INTC disable errno=… before=… value=00100000 after=… result=…`.
2. The unchanged SPI0 reset, channel-3 teardown, image copy and 31-sample ≥150 ms window.
3. **Acknowledge** (in `a9_aop_cold_start`, after `SPI0 pre-run errno=0` and before the
   `before-run` checkpoint and RUN): `AOP INTC pre-ack scm_lo= scm_hi= c3_interval= c3_cursor=
   current=` (guarded SPI0 `+0x0c/+0x10/+0x290/+0x294` and INTC `+0x81c`, the pending state of
   `0x10014` at the end of the window), then `0x210800980 = 0x00100000` (`COLD-W/COLD-R`), then
   `AOP INTC ack errno=… before=… value=00100000 after=…`, then `AOP INTC post-ack errno=…
   scm_lo=… current=… result=…`. `+0x81c` is read only at these two points, as the doc
   specifies, not in each of the 31 samples.
4. Fresh firmware re-enables `0x10014` itself in event 1.

**Readbacks are logged, not gated.** Both are write-one command banks that the firmware never
reads back (round 4 §3.1), and the SCM/interval/`+0x81c` words are explicitly not gates
(§3.3). Gating on an invented value would repeat the C3C bit-5 mistake. The distinct
refusals are those of the write path and its guards, each leaving the AOP halted:

| `failed:` reason | Meaning |
|---|---|
| `failed-intc-disable-checkpoint` | the begin line could not be flushed; no INTC store, no SPI0 access |
| `failed-intc-disable` | the disable store failed (mailbox fault bits or dead state); no SPI0 access |
| `failed-intc-ack-guard` | the AOP domains were not verifiably on for the pre-ack reads; no acknowledge, no RUN |
| `failed-intc-ack` | the acknowledge store failed; no RUN |
| `failed-intc-post-guard` | the post-ack reads were refused; acknowledged, but no RUN |

All earlier refusals (`failed-spi0-reset`, `failed-ch3-*`) are unchanged. A window refusal
happens before the acknowledge, so a refused reload leaves the source disabled with its latch
in place. The skip path (DFU cold, `decision=skip`) issues no INTC store. debugfs `state`
gains a `spi0_intc` line.

The fallback named in round 4 §3.2 (the interrupt-controller event-3 `+0x808` pulse) is **not**
in this build. It is the next single-variable experiment only if the exact `SCMErr:0x3,3` stack
repeats.

### NVMe crawl fix

The cherry-picked `a9/kexec-irqfix` commits make every ANS shared-MSI interrupt service all
CQs and rescan, and let the timeout path drain all CQs (`docs/boot/ds-kexec-nvme-crawl.md`).
The optional rescue poller stays off (`nvme.apple_ans_irq_rescue_ms` is not on the cmdline),
matching the default irqfix entry. The warm reload needs a healthy kexec, so the procedure runs
`kexec-health-gate.py --quick` on the new boot before the trigger.

### Entry (`source build/periph/aop-wake6/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-ch3tdi-accel-a72759f963` | `AOP_CH3TDI_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake6/m1n1-aop-ch3tdi-accel-a72759f963.bin`, entry dir
`build/periph/aop-wake6/aop-ch3tdi-accel-a72759f963/`. Same cmdline as the ch3tdp entry
apart from the entry name (939 bytes): deferred test5, the full kexecfix set, exactly one
`pcie_apple_h9p.ans_keep_power=1`, no rescue parameter, debug info off. The four private root
files are byte-identical to aop-wake5's.

### Exact orchestrator test: one DFU, cold → stream → kexec into the same entry → reload

**Never run `dmesg -n 8`**, in either kernel, at any point.

**A. DFU cold start in the new entry.**

```sh
source build/periph/aop-wake6/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TDI_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake6/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
scripts/boot/upload-entry.sh "$AOP_CH3TDI_ACCEL_ENTRY" "$AOP_CH3TDI_ACCEL_DIR"
C=build/periph/aop-wake6/ch3tdi-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdi-cold
grep -E 'SPI0 |AOP INTC' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdi-cold-tilt
```

Expect `SPI0 reset decision=skip reason=fresh-instance auto_boot=0 out4020=00020001 prev=missing
…: block not touched`, and no `SPI0 reset begin`, no `SPI0 CH3TD` line and no `AOP INTC` line;
then streaming, `cal_sent=3 cal_unsupported=1 cal_state=2`, IIO and tilt. Any `decision=run`,
`SPI0 CH3TD` or `AOP INTC` line here is a fail: stop and keep `$C`.

**B. Kexec into the same entry, preflight, health gate, trigger once.**

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TDI_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake6/ch3tdi-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(afk-|halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TDI_ACCEL_ENTRY"
# NVMe health on this boot (two probes 2 s apart); exit 0 = healthy, 1 = crawl.
python3 -I scripts/boot/kexec-health-gate.py --quick --expect-boot-id "$(head -1 "$Q/new-kernel.txt")" \
    | tee "$Q/health-gate.txt"
```

Preflight must show, as at 20:35: `AOP-PREV SHUTDOWN begin mode=quiesce
entry=aop-ch3tdi-accel-a72759f963 … hello=1 phase=streaming`, `AOP-PREV SPI0-CARRY retained=1
source=hello`, 17 `afk-send`/`afk-ack … state=stopped` pairs, `AOP-PREV QUIESCE step=complete
status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`, `EXPERIMENT
spi0_reset=1 …`, `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff
prev_carry=1`, a `check-warm-shutdown.py` PASS, **and the health gate exiting 0**. If any of
these fails (including `HEALTH FAIL` / exit 1 from the gate), do not trigger. Keep `$Q`, report
and recover with DFU. Otherwise trigger once:

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdi-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP INTC|COLD-[WR] .*(0a00|0980)|AOP released|failed|STAGE3 checkpoint|STARTEP' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'AOP INTC' "$Q/trigger/dmesg.txt" | tee "$Q/intc-lines.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|SCMErr|PC=' "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3|intc) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TDI_ACCEL_ENTRY" --reason prev-quiesce-halted --ch3td --intc
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdi-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**Pass (B)** (round 4 §3.3, in one warm reload): `check-spi0-reload.py … --ch3td --intc`
prints all four PASS lines. The INTC lines are in order: `AOP INTC disable begin` → `COLD
checkpoint=intc-disable` → `COLD-W step=intc-disable … offset=0a00 … value=00100000` → `AOP INTC
disable errno=0 … result=pass` → `SPI0 reset begin` … `SPI0 CH3TD … result=pass` → `window …
samples=31/31 … result=pass` → `SPI0 pre-run errno=0` → `AOP INTC pre-ack …` → `COLD-W
step=intc-ack … offset=0980 … value=00100000` → `AOP INTC ack errno=0` → `AOP INTC post-ack
errno=0 … result=pass` → `AOP released`. Then:

- **the accel STARTEP 0x28 returns status 0** (not -110);
- **no `SCMErr` crashlog**;
- the accel service reaches ready, IIO registers, and samples change for 60 s (and with tilt);
  `mpu6900 device ID 0x10 found` is expected.

Record the `AOP INTC` lines: the disable/ack before→after words, and the pre/post `scm_lo/hi`,
`c3_interval/cursor` and `current`. Record the health-gate output and the debugfs `spi0_intc`
line too. None of these values is a pass/fail criterion.

**Fail (B):** any STARTEP 0x28 timeout, AOP crash or absent/static accel stream. If the **exact
`SCMErr:0x3,3` stack repeats**, the targeted clear did not remove a level/reasserted source, and
only then is the round-4 §3.2 `+0x808` interrupt-controller reset the next experiment. Any
`failed-intc-*`, `failed-ch3-*` or `failed-spi0-reset` keeps the AOP halted with no RUN; report
the reason. In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake6.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake6
```

`build-wake6.sh` moves the clean `/src/wt/aop-wake5` worktree (at `cecb33eef5`) to
`/src/wt/aop-wake6`. It cherry-picks the two irqfix commits with `-x`, only on a clean tree,
each once: commit A is reworded from "All 13 logged" to "All 10 logged" and checked. It
requires the NVMe files to equal `a9/kexec-irqfix`, then commits the AOP change on top. W=1
covers the AOP objects only (the full build compiles NVMe). New offline coverage:

- `tests/force-cold.c`, with an INTC model in which `+0x81c` reads the old latch until the
  acknowledge. The disable is the 6th store (`+0xa00 = 00100000`), after the flushed begin line
  and before every SPI0 read or write. The acknowledge is the 17th (`+0x980 = 00100000`), after
  the last of the 4+31 `+0x8d0` reads and directly before RUN. Pre/post SCM, interval and
  `+0x81c` words are recorded. The skip path, window refusals (reassert, guard, short clock) and
  checkpoint failures issue no acknowledge. A faulted disable or acknowledge store, a refused
  pre- or post-ack guard and a failed `intc-disable` flush each refuse with their own reason
  and never reach RUN.
- `check-spi0-reload.py --self-test`: the `--intc` fixture around the 19:50 teardown words.
  Different undocumented readbacks are accepted. Missing, failed, misordered,
  wrong-offset/value or duplicated INTC lines and an `SCMErr` crashlog are rejected. The 20:35
  log (wake5) still checks with `--ch3td` alone.

**Firmware and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- 69 offline PASS lines in `offline.log`: 64 in `test-offline.py` (62 before; the round-4
  INTC force-cold group and the `--intc` checker self-test), the SPI0 (3) and shutdown (1)
  checker self-tests run again on their own, and the `--wake6` plan check.
- W=1 clean on the AOP objects; checkpatch 0 errors/0 warnings (228 lines). The first run
  stopped on one 102-column line (a trailing `ret`), which was wrapped; the cherry-picks
  were kept and skipped on the rerun.
- Full Image.gz plus 105 DTBs, byte-identical to tri49a's.
- `check-handoff-build.py --wake6` PASS: exactly the 3 commits above on a clean branch, both
  cherry-picks carrying `(cherry picked from commit …)`, commit A at 10 crawl boots, the NVMe
  files equal to `a9/kexec-irqfix`, only the four files differ from `cecb33eef5`, the kexecfix
  PCIe source, config and DTBs. Linked: every `AOP INTC` format and `failed-intc-*` reason,
  `MSI resumed after vector drain`, `drained every CQ on vector`, `apple_ans_irq_rescue_ms`,
  plus all the wake5 teardown/window/quiesce strings. The three pstore recovery fixtures also
  pass.
- Extraction, checksum, firmware, ECID and cmdline checks of the payload
  (`stage3-package.py --wake6`), including no `nvme.apple_ans_irq_rescue_ms`. The cmdline equals
  the ch3tdp entry's apart from the entry name.
- `make clean` left the tree at 1.8 GB; `/src` has 25 GB free.
- `root-firmware/SHA256SUMS` is identical to aop-wake5's.

Logs are under `build/periph/aop-wake6/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `COMMITS`, `wake6.patch` and `manifest-wake6.json`. Payload
SHA256:

- `m1n1-aop-ch3tdi-accel-a72759f963.bin`: `5e14cb40c6579e8f3c59ee5f5c64eac24fbe405a14eed1377bddd7ecaa095be4`

### 2026-10-26 23:25: reload refused, channel 3 not idle (aop-ch3tdi-accel-a72759f963)

Evidence: `docs/periph/logs/aop-ch3tdi-20261026/` (workspace commit `65b00e5b`). The cold leg ran
through a gated kexec from a sep7 boot, so the AOP was a fresh instance (`prev=missing`,
`fresh-instance` skip, streaming). A same-entry kexec followed, with preflight PASS (17 acks,
`steps=0x1bff`, `prev=valid`). On the reload:

- `AOP INTC disable errno=0 before=ffffffff value=00100000 after=ffffffff result=pass`;
- the SPI0 reset as before: `8d0` 0x40 → 0, C3 `18011445` → `00010447`;
- `SPI0 CH3TD before c3=01011445 … 8d0=00000040`: channel 3 had restarted a transfer since the
  reset (idle clear, bits 12/24 set), exactly as in the wake4/wake5 runs;
- `writes stop=01011044 idle_poll=02010444 poll_errno=-110 masks=skipped`, then
  `failed-ch3-teardown-idle (-110)`, with no RUN. The same starting word in wake4/wake5 polled
  `02010446` (idle) at once.

## Firmware-bounded idle wait, INTC mask readback (`a9/aop-wake7`)

Kernel `04ff8657bb12f8b2a805774a7b1640df61f01bad`, one commit on the new branch `a9/aop-wake7`, on
top of `a9/aop-wake6` `a72759f963`. Its irqfix ancestry (`56efc8218d`, `e4c177ae28`) is kept and
verified. Release `7.3.0-rc1-aop-wake7-g04ff8657bb12`. **No device operation was performed; nothing below is a
hardware result.** Only `apple-a9-aop-active.c` and `a9-aop-reload.h` differ from wake6. The NVMe
files still equal `a9/kexec-irqfix`, and the cmdline is the wake6 one apart from the entry name.

### Firmware findings (pinned iOS 16.7.10 AOP image, offline)

1. **The firmware's own channel-disable wait is bounded at 10 ms, and a miss panics.**
   `AOPSPIMaster_setChannelEnabled(…, false)` (`0x0102ef7c`) acts only if C3 bit 0 is set. It
   stores `C3 & ~0x411` (`0x0102efc2`), loads the deadline from master `+0x30` (`0x0102efc6`),
   and spins on C3 bit 1 against CNTPCT (timer `+0xc` = `0x010275d8`, `mrrc p15 c14`). Past the
   deadline it calls `aop_panic_printf("SCMto:%u", ch)` (`0x0102f006`, format `0x010398da`).
   Lifecycle event 1 stores `+0x30 = 0x2710` when it is zero (`0x0102e6f4..0x0102e702`; the
   live-cold object has `+0x30 = 0`), then converts it through timer `+0x18` (`0x0102ff3c`,
   `us * Hz / 1000000`, literal `0x000f4240`). So the bound is **10 000 µs**. The AP's old 1 ms
   was 10× tighter than the firmware's.
2. **`+0xa00` and `+0xa80` read back the group-1 mask (1 = disabled).** Three firmware anchors
   show it:
   - the INTC sleep command (`aop_interrupt_controller_command` case 4) saves
     `+0xa00..+0xa08` (`0x0102c5ce`), writes `0xffffffff` to each (`0x0102c5da`), writes
     group-7 `+0xc020/+0xc024 = 2` (`0x0102c648/0x0102c650`), then spins until `+0x81c` reads 0
     (`0x0102c654`);
   - the restore (`0x0102c9d2`) writes the saved words to `+0xa00` and their complement to
     `+0xa80` (`0x0102c9e6/0x0102c9f4`);
   - `aop_interrupt_write_enabled` (`0x0102c972`) reads the target bank before storing and
     returns "was enabled" = `(read & bit) == 0` (`0x0102c9c0`).

   Wake6's `before=ffffffff` is therefore the post-quiesce state: the firmware's own sleep path
   had masked every bank-0 source, 0x10014 (bit 20) included. The disable store was a no-op, and
   the readback does prove the source is disabled. This corrects round 4 §3.1 ("the firmware
   does not compare their readback"): it does read them, as the mask. The INTC is the AOP
   wrapper block. The firmware object holds `0xc1800000` "space 2", the same mapping as SPI0's
   `0xc1100000` = `0x210100000`, which is verified on the device, so it is `0x210800000`, the
   block the driver already reads unguarded (`+0x44`, `+0x4000`, `+0x4020`). No separate PMGR
   domain is involved.
3. **No readable register shows whether a masked source is pending.** `+0x81c` (current source)
   only reports an unmasked pending source. The dispatch loop claims from it and re-enables
   afterwards, and the sleep path expects it to read 0 once everything is masked. `+0x900` is
   the group-1 raise (`0x0102c7c6`) and `+0x980` its pre-handler clear (`0x0102c6f2`), both
   stores only, for sources whose handler flag bit 0 is set. The firmware never reads them, so
   their readbacks are logged without meaning. The only firmware-grounded way to see a masked
   0x10014 pending would be to unmask it, read `+0x81c` and re-mask. That means two new INTC
   stores and a possibly claiming read, so it is **not** in this build. It is the next
   experiment if `SCMErr:0x3,3` repeats after a passing teardown.

### Change

1. **Idle wait** (`a9_aop_spi0_ch3_locked`, same guard, CPU halted). After the unchanged stop
   store, poll C3 bit 1 in 1 ms atomic slices, at most 20 (twice the firmware bound). After each
   slice, read (read-only) C3, SCM `+0x0c/+0x10` and `+0x8d0`. Log one `SPI0 CH3TD idle-sample
   slice=N at_us=… c3=… scm_lo=… scm_hi=… 8d0=… idle=0|1` line per slice used, then `SPI0 CH3TD
   idle-poll run_before=… slices=N/20 idle_us=… bound_us=20000 fw_bound_us=10000
   beyond_fw_bound=0|1 result=idle|timeout`. Idle within 20 ms continues exactly as in wake6.
   `beyond_fw_bound=1` (idle after 10 ms) passes but is flagged, because a live firmware would
   have panicked `SCMto` there. No idle after 20 ms gives `failed-ch3-teardown-idle` with only
   the stop store, as before. The worst-case IRQ-off time grows from about 1 ms to about 20 ms.
2. **INTC readback.** Ten words (`+0x808 +0x81c +0x900 +0x980 +0xa00 +0xa04 +0xa08 +0xa80
   +0xc020 +0xc024`) are snapshotted around both stores, as `AOP INTC state
   at=pre-disable|post-disable|pre-ack|post-ack … src_masked=0|1 view=intc|all-ones`.
   `view=all-ones` means `+0x81c` read `ffffffff`, which a live INTC whose sleep path completed
   cannot report, so the readbacks would not come from the INTC. **The disable is now gated:**
   `+0xa00` bit 20 must read 1 after the store, otherwise `failed-intc-disable-readback`, before
   any SPI0 access. `AOP INTC disable …` gains `mask_a80= src_masked= view=`. The acknowledge
   stays ungated (finding 3). The `pre-ack`/`post-ack` lines report `current` from the snapshots.
3. Everything else is wake6: the reset, teardown masks and readbacks, the 31-sample ≥150 ms
   window, the ack before RUN, the quiesce and the NVMe fix. debugfs `spi0_ch3` gains
   `run_before= idle_slices= idle_us=`, and `spi0_intc` gains `states=`.

| new `failed:` reason | Meaning |
|---|---|
| `failed-intc-disable-readback` | after the `+0xa00` store, bit 20 read 0: 0x10014 not provably masked; no SPI0 access, no load |

### Entry (`source build/periph/aop-wake7/entries.env`)

| Entry | Variable prefix | Mode |
|---|---|---|
| `aop-ch3tdw-accel-04ff8657bb` | `AOP_CH3TDW_ACCEL` | FULL: `aop_run=2 aop_quiesce=1 aop_spi0_reset=1` |

Payload `build/periph/aop-wake7/m1n1-aop-ch3tdw-accel-04ff8657bb.bin`, entry dir
`build/periph/aop-wake7/aop-ch3tdw-accel-04ff8657bb/`. The cmdline is 939 bytes, the wake6 one
with the new entry name: deferred test5, the full kexecfix set including exactly one
`pcie_apple_h9p.ans_keep_power=1`, no rescue parameter, debug info off.

### Exact orchestrator test: cold → stream → kexec into the same entry → reload

**Never run `dmesg -n 8`**, in either kernel, at any point. This is the 23:25 flow.

**A. Cold start in the new entry, on a fresh AOP.** Use a DFU boot, or (as at 23:25) a gated
kexec from a boot in which the AOP never ran. Either way the expected lines are the same.

```sh
source build/periph/aop-wake7/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TDW_ACCEL_PAYLOAD"
# Wait for SSH. Verify the four private files (install exactly as in the stage-3 section if any differs).
(cd build/periph/aop-wake7/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin'
scripts/omarchy/ssh.sh 'sudo -n dmesg | grep -E "SHUTDOWN-LOG|AOP-PREV shutdown transcript|EXPERIMENT|SPI0 evidence"'
scripts/boot/upload-entry.sh "$AOP_CH3TDW_ACCEL_ENTRY" "$AOP_CH3TDW_ACCEL_DIR"
C=build/periph/aop-wake7/ch3tdw-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdw-cold
grep -E 'SPI0 |AOP INTC' "$C/trigger/dmesg.txt" | tee "$C/spi0-lines.txt"
# Physically tilt; require fresh changing axes.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdw-cold-tilt
```

Expect `SPI0 reset decision=skip reason=fresh-instance … prev=missing …: block not touched`, and
no `SPI0 reset begin`, no `SPI0 CH3TD` line and no `AOP INTC` line (the skip path reads no INTC
word). Then streaming, `cal_sent=3 cal_unsupported=1 cal_state=2`, IIO and tilt. Any
`decision=run`, `SPI0 CH3TD` or `AOP INTC` line here is a fail: stop and keep `$C`.

**B. Kexec into the same entry, preflight, health gate, trigger once.**

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TDW_ACCEL_ENTRY"
# SSH disconnect is expected; wait for SSH in the NEW kernel.
Q=build/periph/aop-wake7/ch3tdw-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(afk-|halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TDW_ACCEL_ENTRY"
# NVMe health on this boot (two probes 2 s apart); exit 0 = healthy, 1 = crawl.
python3 -I scripts/boot/kexec-health-gate.py --quick --expect-boot-id "$(head -1 "$Q/new-kernel.txt")" \
    | tee "$Q/health-gate.txt"
```

Preflight must show, as at 23:25: `AOP-PREV SHUTDOWN begin mode=quiesce
entry=aop-ch3tdw-accel-04ff8657bb … hello=1 phase=streaming`, `AOP-PREV SPI0-CARRY retained=1
source=hello`, 17 `afk-send`/`afk-ack … state=stopped` pairs, `AOP-PREV QUIESCE step=complete
status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none errno=0`, `EXPERIMENT
spi0_reset=1 …`, `SPI0 evidence prev=valid prev_hello=1 prev_halted=1 prev_steps=0x1bff
prev_carry=1`, a `check-warm-shutdown.py` PASS, **and the health gate exiting 0**. If any of
these fails, do not trigger: keep `$Q`, report and recover with DFU. Otherwise trigger once:

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdw-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP INTC|COLD-[WR] .*(0a00|0980)|AOP released|failed|STAGE3 checkpoint|STARTEP' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'SPI0 CH3TD (before|idle-sample|idle-poll|writes|after|check)' "$Q/trigger/dmesg.txt" | tee "$Q/idle-lines.txt"
grep -E 'AOP INTC' "$Q/trigger/dmesg.txt" | tee "$Q/intc-lines.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|SCMErr|SCMto|PC=' "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3|intc) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TDW_ACCEL_ENTRY" --reason prev-quiesce-halted --wake7
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdw-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

**What to read first** (`idle-lines.txt`, `intc-lines.txt`):

- `AOP INTC state at=pre-disable …`: expected `mask_a00=mask_a04=mask_a08=mask_a80=ffffffff`
  (the quiesce masked all), `current81c=00000000`, `src_masked=1`, `view=intc`, and probably
  `reset808=00000000` and group-7 words with bit 1 set. `view=all-ones` (`current81c=ffffffff`)
  means the AP is not reading the INTC and its words mean nothing: report it.
- `SPI0 CH3TD idle-sample …` / `idle-poll …`: how long channel 3 took to go idle after the stop,
  and how C3 (bits 1, 10, 12, 24/25), the SCM state and `+0x8d0` moved per millisecond.

**Pass (B)** (round 4 §3.3, in one warm reload). `check-spi0-reload.py … --wake7` prints its
PASS lines, including `PASS channel-3 idle wait: …` and the `INFO INTC words …` line. In order:
`AOP INTC disable begin` → `COLD checkpoint=intc-disable` → `AOP INTC state at=pre-disable` →
`COLD-W step=intc-disable … offset=0a00 … value=00100000` → `AOP INTC state at=post-disable …
src_masked=1` → `AOP INTC disable errno=0 … src_masked=1 … result=pass` → `SPI0 reset begin` … →
`SPI0 CH3TD before` → `idle-sample slice=0…n-1` (only the last `idle=1`) → `idle-poll … result=idle`
→ `writes … poll_errno=0` → `check … result=pass` → `window … samples=31/31 … result=pass` →
`SPI0 pre-run errno=0` → `AOP INTC state at=pre-ack` → `AOP INTC pre-ack` → `COLD-W
step=intc-ack … offset=0980` → `AOP INTC state at=post-ack` → `AOP INTC ack errno=0` → `AOP INTC
post-ack errno=0 … result=pass` → `AOP released`. Then, as in wake6:

- **the accel STARTEP 0x28 returns status 0** (not -110);
- **no `SCMErr` (or `SCMto`) crashlog**;
- the accel service reaches ready, IIO registers, and samples change for 60 s (and with tilt).

Record, without treating them as pass/fail: `idle_us`, `slices`, `beyond_fw_bound`, every
idle-sample word, the four INTC state lines, `scm_lo/hi`, `c3_interval/cursor`, the health gate
output and the debugfs `spi0_ch3`/`spi0_intc` lines.

**Fail (B):**

- `failed-ch3-teardown-idle` after `slices=20/20 … result=timeout`: channel 3 stays busy for
  more than twice the firmware bound with the AOP CPU halted, so the transfer is stuck rather than
  slow. The 20 samples show whether C3/SCM/`+0x8d0` still move. No RUN; report them.
- `failed-intc-disable-readback`: bit 20 read 0 after the store. No SPI0 access, no RUN.
- Any STARTEP 0x28 timeout, AOP crash or absent/static accel stream after RUN. If the **exact
  `SCMErr:0x3,3` stack repeats** after a passing teardown, the retained 0x10014 cause survived
  the acknowledge. The next single-variable experiments are the unmask/`+0x81c`/re-mask probe
  (finding 3) or round 4 §3.2's `+0x808` reset.
- Any other `failed-intc-*`, `failed-ch3-*` or `failed-spi0-reset` keeps the AOP halted with no
  RUN; report the reason. In every case keep `$C` and `$Q`, then recover with DFU.

### Build and offline validation

```sh
scripts/periph/aop/build-wake7.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake7
```

`build-wake7.sh` moves the clean `/src/wt/aop-wake6` worktree (at `a72759f963`) to
`/src/wt/aop-wake7` and commits the AOP change on top. There are no cherry-picks. It verifies
the irqfix picks and every earlier AOP parent as ancestors, and the NVMe files against
`a9/kexec-irqfix`. New offline coverage:

- `tests/force-cold.c`. The C3 model has an idle clock (each failed atomic poll read costs
  1 µs). The INTC model has `+0xa00/+0xa80` reading the mask (post-quiesce `ffffffff`), and
  `+0x81c` showing 0x10014 only while latched and unmasked. Covered cases:
  - idle at once (1 slice);
  - idle at 7 ms (7 slices), and at 15 ms (15 slices, `beyond_fw_bound`);
  - never idle (20 slices, `failed-ch3-teardown-idle`, only the stop stored);
  - the wake6 device trace `01011445 → 01011044 → 02010444…`, refused after 20 sampled slices;
  - the mask gate: post-quiesce `ffffffff` passes, `0 → 00100000` passes, and a store that does
    not stick refuses `failed-intc-disable-readback` before any SPI0 access;
  - `view=all-ones` labelling;
  - `+0x8d0` read counts with the per-slice sample (5+31).
- `check-spi0-reload.py --self-test --wake7`:
  - accepted: a wake7 fixture with post-quiesce INTC words and idle after two busy slices, idle
    at 15 ms flagged, and all-ones reported;
  - rejected: timeout; slice count, sequence and flag mismatches; idle lines out of place;
    missing, extra, misordered or unmasked state lines; `src_masked`/`view` disagreeing with the
    words; and the wake6 disable format.

**Firmware and SPI0 are modeled; no hardware result follows.**

Build verification completed (2026-10-26):

- `offline.log` has 73 PASS lines: `test-offline.py` (now including the force-cold idle-wait and
  INTC-readback groups and the `--wake7` checker self-test), the SPI0 and shutdown checker
  self-tests run again on their own, and the `--wake7` plan check.
- W=1 is clean on the AOP objects; checkpatch reports 0 errors and 0 warnings over 372 lines.
- The full Image.gz and 105 DTBs build; the DTBs are byte-identical to tri49a's.
- `check-handoff-build.py --wake7` PASS. It checks: exactly one commit on `a72759f963` in a clean
  branch; ancestry of the irqfix picks and every earlier parent; only the two AOP files differ;
  NVMe equal to `a9/kexec-irqfix`; the kexecfix PCIe source, config and DTBs. Linked strings:
  the idle-sample/idle-poll lines, `AOP INTC state at=`, `failed-intc-disable-readback`, and
  every wake6 INTC/teardown/window/quiesce string. The wake6 unchecked-disable format is
  absent. The three pstore recovery fixtures also pass.
- `stage3-package.py --wake7` checks extraction, checksums, firmware, ECID and the 939-byte
  cmdline. `root-firmware/SHA256SUMS` is identical to aop-wake6's.
- After `make clean` the tree is 1.8 GB; `/src` has 17 GB free.

Logs are under `build/periph/aop-wake7/`: `offline.log`, `worktree.log`, `prepare.log`,
`w1.log`, `checkpatch.log`, `build.log`, `check-build.log`, `package.log`,
`package-test.log`, `cleanup.log`, `COMMITS`, `wake7.patch` and `manifest-wake7.json`. Payload
SHA256:

- `m1n1-aop-ch3tdw-accel-04ff8657bb.bin`: `37f642ee7be86e8c6e313448ee44482d009b33ced97e8d9992949d3408fb5afd`

## INTC event-3 reset experiment (`a9/aop-wake8`, 2026-10-27)

Wake7's device run at 10:59 passed the cold leg, all 17 outgoing AFK ACKs,
quiesce `steps=0x1bff`, health gate, INTC disable readback, CH3 idle in 122 us,
teardown, and the 31/31 window over 184 ms. It still panicked
`SCMErr:0x3,3` at accel STARTEP. Logs:
`docs/periph/logs/aop-ch3tdw-20261027/`. This is **not** a warm-reload pass.

The new offline analysis is [round 5](ds-aop-spi0-ch3-round5.md). The first
`3` is SCM flags/state (low bits 0/1 set), the second is channel 3. It is not
an error-code enum. The stack interrupts SPI lifecycle event 1 before its
event-5 reset, before MPU channel/sensor init. The firmware's own FIFO stop
sequence exists, but occurs later and is not the immediate missing operation.
No raw sensor or SCM-status write is added.

Wake8 adds the broader firmware INTC lifecycle-event-3 pulse after the
existing targeted acknowledge: AP wrapper `0x210800808 = 1/read/0/read`
(`0102c592/598/59c/5a2`). This is the separate experiment specified by round 4
after the same SCM panic repeats. It resets all AOP-private interrupt state;
its effect on retained delivery must be tested. SCM is allowed to remain
`3/0`. The final `+0x808` read must be zero, and the established C3
run/class0/class1-clear, idle-set, descriptor-zero, pending-bit-clear checks
must still hold after the pulse. Any failure refuses RUN; no retry.

Seven guarded, read-only `SPI0 SCM stage=...` snapshots contain ctl, clock,
SCM low/high, C3 interval/cursor, C3/route/rate/C3C/descriptor and `8d0/8d4`:
`before-controller-reset`, `after-controller-reset`, `after-ch3-teardown`,
`after-copy`, `after-window`, `pre-intc-reset`, `post-intc-reset`. A refused
snapshot guard is terminal. The fresh-instance skip path still reads neither
SPI0 nor INTC. There is no true-cold MMIO snapshot to compare; the round-5
table compares the warm words with firmware's first initialization writes.

Build with `scripts/periph/aop/build-wake8.sh`; parent is wake7
`04ff8657bb12f8b2a805774a7b1640df61f01bad`. The new kernel branch has one AOP
commit and retains `a9/kexecfix` PCIe and `a9/kexec-irqfix` NVMe ancestry.
No workspace commit or device operation is performed by the builder.

The one FULL accel entry is `aop-ch3tdr-accel-<shortsha>`, variables
`AOP_CH3TDR_ACCEL_ENTRY`, `AOP_CH3TDR_ACCEL_PAYLOAD`, `AOP_CH3TDR_ACCEL_DIR`
in `build/periph/aop-wake8/entries.env`. It keeps
`aop_run=2 aop_quiesce=1 aop_spi0_reset=1` and
`pcie_apple_h9p.ans_keep_power=1`, with no NVMe rescue parameter. Its command
line is 901 bytes and contains neither `systemd.debug_shell=ttyGS0` nor
`loglevel=7`. Packaging assembles that exact command line locally because
the shared `scripts/mkpayload.sh` inserts `loglevel=7` unconditionally.

### Orchestrator procedure (not executed by the builder)

Use a fresh DFU/SoC reset. The tested sep7/gated-kexec bootstrap is also
acceptable if no AOP endpoint is triggered before entering wake8. Verify
fresh skip in the cold leg; an ordinary warm boot is not a substitute.

**A. Cold boot this entry, then stream and tilt.**

```sh
source build/periph/aop-wake8/entries.env
scripts/boot.sh pongo
scripts/boot.sh linux "$AOP_CH3TDR_ACCEL_PAYLOAD"
# Wait for SSH. Verify/install the four private files on the real root.
(cd build/periph/aop-wake8/root-firmware && shasum -a 256 -c SHA256SUMS)
scripts/omarchy/ssh.sh 'sudo -n install -d -m 0755 /lib/firmware/apple/aop'
for file in boot-ecid.bin ipad6faop-20H350.bin ipad6faop_ext-20H350.bin accel-calibration.bin; do
  cat "build/periph/aop-wake8/root-firmware/$file" |
    scripts/omarchy/ssh.sh "sudo -n tee /lib/firmware/apple/aop/$file >/dev/null && sudo -n chmod 0600 /lib/firmware/apple/aop/$file"
done
scripts/omarchy/ssh.sh 'sudo -n sha256sum /lib/firmware/apple/aop/*.bin; cat /proc/cmdline'
scripts/boot/upload-entry.sh "$AOP_CH3TDR_ACCEL_ENTRY" "$AOP_CH3TDR_ACCEL_DIR"
C=build/periph/aop-wake8/ch3tdr-cold-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$C"
AOP_CAPTURE_DIR="$C/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdr-cold
grep -E 'SPI0 |AOP INTC|cal_sent=|phase=|failed|SCMErr' "$C/trigger/dmesg.txt" "$C/trigger/state.txt" \
    | tee "$C/cold-check.txt"
# Physically tilt and require fresh changing axes in an independent capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdr-cold-tilt
```

Require `decision=skip reason=fresh-instance` (or a correctly diagnosed
stale transcript after DFU), no `SPI0 reset begin`, `SPI0 CH3TD`, `SPI0 SCM`
or `AOP INTC` line, streaming, `cal_sent=3 cal_unsupported=1 cal_state=2`, IIO
and live tilt. Any cold-path SPI0/INTC access is a failure: preserve `$C` and
stop. Do not proceed to B unless the cold stream is healthy.

**B. Same-entry kexec, preflight and health gate.**

```sh
scripts/omarchy/ssh.sh "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_CH3TDR_ACCEL_ENTRY"
# SSH disconnect is expected. Wait for SSH in the new kernel.
Q=build/periph/aop-wake8/ch3tdr-reload-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' > "$Q/new-kernel.txt"
grep -E 'EXPERIMENT|SPI0 evidence|AOP-PREV (shutdown transcript|SHUTDOWN begin|SPI0-CARRY|QUIESCE step=(afk-|halt|complete))' \
    "$Q/new-kernel.txt" | tee "$Q/preflight.txt"
python3 -B scripts/periph/aop/check-warm-shutdown.py "$Q/new-kernel.txt" --entry "$AOP_CH3TDR_ACCEL_ENTRY"
python3 -I scripts/boot/kexec-health-gate.py --quick --expect-boot-id "$(head -1 "$Q/new-kernel.txt")" \
    > "$Q/health-gate.txt"
GATE_STATUS=$?
cat "$Q/health-gate.txt"
test "$GATE_STATUS" -eq 0
```

Require 17 AFK send/ACK pairs, the outgoing streaming entry with `hello=1`,
`QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff`,
no failed step, `SPI0 evidence prev=valid prev_hello=1 prev_halted=1`, the
shutdown checker PASS, and health-gate exit 0. Stop on any failure; no AOP
trigger on a crawl boot. The commands preserve the health gate's exit status.

**C. Only after B passes: trigger once, collect everything.**

```sh
AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
    scripts/periph/aop/trigger-cold.sh ch3tdr-reload
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP INTC|COLD-[WR].*(0808|0a00|0980)|AOP released|failed|STAGE3 checkpoint|STARTEP' \
    "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"
grep -E 'SPI0 SCM|AOP INTC reset|COLD-[WR].*0808' "$Q/trigger/dmesg.txt" | tee "$Q/scm-reset.txt"
grep -E 'AOP syslog .*mpu6900 device ID 0x10 found|AOP crashlog|SCMErr|SCMto|PC=' \
    "$Q/trigger/dmesg.txt" | tee "$Q/mpu-lines.txt"
grep -E '^spi0_(reset|ch3|intc|intc_reset) ' "$Q/trigger/state.txt" | tee "$Q/spi0-state.txt"
python3 -B scripts/periph/aop/check-spi0-reload.py "$Q/trigger/dmesg.txt" \
    --entry "$AOP_CH3TDR_ACCEL_ENTRY" --reason prev-quiesce-halted --wake8
# Only if it streams: physically tilt; independent 60 s capture.
AOP_CAPTURE_HOLD_SECONDS=60 scripts/periph/aop/capture-stage23.sh ch3tdr-reload-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

Pass requires all wake7 reset/teardown/window/INTC gates, all seven snapshots
in order, exactly two `COLD-W step=intc-reset ... offset=0808` writes (1 then
0) with their readbacks, `AOP INTC reset errno=0 ... after=00000000 result=pass`,
RUN released once, STARTEP `0x28` status 0, no crashlog/SCMErr/SCMto, IIO,
and fresh changing accelerometer reports with report/ping counters increasing
for 60 seconds. The log checker verifies protocol/log invariants; tilt is an
independent physical check. A residual SCM value of `3/0` is not a failure.

Any `failed-intc-reset-{state,assert,deassert,readback,ch3}` or
`failed-spi0-trace-guard` means no RUN. Keep the last snapshot and both reset
readbacks. Existing `failed-ch3-*` refusals are unchanged. A STARTEP timeout,
crash, missing or static stream is a failure even if the pulse passes. No
second trigger or retry. If the same SCM stack repeats, report that the full
INTC reset also failed to remove delivery; do not escalate to guessed SCM,
interval, FIFO or sensor writes.

Host checks: pinned RE export; `test-offline.py` with production functions
compiled under ASan/UBSan; SPI0 and shutdown checker self-tests; `W=1` AOP
object compile; checkpatch; `check-handoff-build.py --wake8`; early-pstore
checks; `tests/stage3-package.py --wake8` for payload extraction, firmware,
checksums, ancestry/config/DT and the clean command line. Build provenance
and logs are under `build/periph/aop-wake8/`. Device execution remains unverified.

### Delivered wake8 artifacts and validation

- Kernel branch: `a9/aop-wake8`, commit
  `a67f399dedbaa93c956c1205239c5142ffce7df5` (one commit on wake7).
- Release: `7.3.0-rc1-aop-wake8-ga67f399dedba`.
- Entry: `aop-ch3tdr-accel-a67f399ded`.
- Payload: `build/periph/aop-wake8/m1n1-aop-ch3tdr-accel-a67f399ded.bin`.
  Boot link: `build/boot/aop-ch3tdr-accel-a67f399ded.bin`; entry link:
  `build/boot/aop-ch3tdr-accel-a67f399ded/`. Links avoid duplicate large files.
- Payload SHA-256:
  `d7def78200d44292faa16c9785f82276db121490edcffdaea12f61200373c2ef`.
- Real extraction/package validation PASS, including the exact 901-byte
  command line and all four pinned/private root firmware files. The kernel
  and workspace AOP sources match. The initial packaging test caught an
  `entries.env` filename-shadowing error; it was corrected and the package
  and extraction test rerun successfully before delivery.
- `make clean` reclaimed this branch's build products: 2.3 GB to 1.8 GB;
  exported payloads/provenance remain. No other stream's processes were stopped.

Repeatable commands (host only, except the explicitly marked procedure above):

```sh
uv run --with capstone --with pyghidra python -B scripts/periph/aop/re-spi0-ch3-round5.py
scripts/periph/aop/build-wake8.sh
python3 -B scripts/periph/aop/test-offline.py
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test --wake8
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test
python3 -B scripts/periph/aop/tests/stage3-package.py --wake8
shasum -a 256 build/boot/aop-ch3tdr-accel-a67f399ded.bin
```

The builder runs the `W=1` object compile/checkpatch before the kernel commit,
then `check-handoff-build.py --wake8` and `tests/cold-pstore.py` in Docker
before cleaning. Host tests exercise the actual loader/MMIO functions,
including fault injection at every new trace guard, either pulse store,
nonzero final reset readback and post-pulse channel fault. First reset
readback 0 or 1 and retained SCM 3 both pass. These prove implementation
behavior, not the hardware effect of the INTC pulse.

Changed workspace files (left uncommitted for the orchestrator):

```text
docs/periph/aop-stage3.md
docs/periph/ds-aop-spi0-ch3-round5.md
scripts/periph/aop/build-wake8.sh
scripts/periph/aop/re-spi0-ch3-round5.py
scripts/periph/aop/kernel/a9-aop-reload.h
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/package-stage3.py
scripts/periph/aop/check-handoff-build.py
scripts/periph/aop/check-spi0-reload.py
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/stage3-package.py
```
