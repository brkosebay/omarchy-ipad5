# Cold2 v6: defer release; fence the first post-PRNG hardware call

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25. Implementation sub-agent, host/Docker/offline only. No device
access, workspace commit or push. Kernel commits only on `a9/aop` and
`a9/tri49a`; owner accepts crash risk. No syscfg, NVRAM or boot-storage IO.

**The first new hardware-control call after PRNG returns sets bit 2 at
physical `0x210008004`. Its exact register function and responsibility for
the AP loss are not proved.** New entries boot to SSH before any forced
stop/load/release, and a fenced entry parks just before that call. They are
diagnostic experiments, not a verified fix or a guarantee of AP survival.

The orchestrator reports that `tri49a-cold2n-bfcb3c8e48` lost the entire AP
before USB enumeration, required force restart/DFU, and left only the empty
pstore directory listing in [the recovery capture](../release/OMISSIONS.md).
`build/aop-cold2N-kexec.log` confirms no new SSH boot ID. There is no sampled
PC, reset reason, or crash record identifying what happened. Supplying ECID
is consistent with progressing beyond the old fatal, but this remains an
inference for that particular failed boot.

## Pinned continuation and candidate causes

`scripts/periph/aop/re-post-prng.py` checks both original firmware hashes,
exports raw Thumb windows and records the table/object proof in
`build/periph/aop-force-cold/re-post-prng/evidence.json`. `--ghidra` uses its
own project there, leaving the shared kernelcache server untouched. Addresses
beginning `010...` below are linked AOP VAs, not AP physical addresses.

1. After the ECID/non-null-device-info checks (`01034f98..01034faa`),
   `01034fb4` calls `01004020` to construct software PRNG method pointers.
   The seed method is **`0100406a`**, called indirectly at `01034fde`.
   It initializes the preallocated context at `01063f58`, and mixes the
   32-byte PRNG input, eight-byte NONC and eight-byte ECID through
   `010042a4` and software hash/HMAC routines. A zero NONC is not rejected
   by this initializer. No PMU, reset, PLL or DMA programming is visible in
   this immediate path. The normal pinned object already has a context, so
   the conditional `0x100` heap allocation is not needed here.
2. PRNG returns to ext `010721fa`. `01072202` stores **`0x21` to
   `01052ab8+0xc`**, the requested power state in the RTKit root.
3. At `0107220c`, ext invokes **method `0x14` (20), args 0/1**, on object
   **`01047598`**, whose metadata names it **`CPU`**. Its vtable
   `01039b34` selects dispatcher `0102b698`. The raw TBH table independently
   resolves method 20 to `0102b736`, which calls `0102bae0` with flag 1.
   After timer accounting, `0102bb48..0102bb52` reads, ORs and stores a
   control word: object PA `0x210008000` + offset 4, mask **4**.
   Runtime mapping changes the object's low address to `c1008000`; the
   original physical descriptor proves the PA above. This is an AON CPU
   control/gate candidate, not a proved system-reset register. The nearby
   CP15 helper `01031002` changes MMU domain access, not PMU rails.
4. Startup then creates a **`power`** task at `010329a8`, with a `0x400`
   stack and priority 30 (then 31), configures interrupt-controller line
   `0x18`, creates `init`/`phase1` service tasks via `01073b40`, and enables
   AOP IRQs at `010722d2`. The previously constructed `rtk_ep_work` can now
   run. The power worker compares current root state (1 in the old fatal
   captures) with the newly requested `0x21`, and calls `01032c18`.
   That routine has separate lower/idle-state branches that disable IRQ/FIQ,
   invoke platform callbacks and WFI; do not interpret decompiler-mangled
   computed branches as proof that those paths ran in cold2n.

This orders the candidates. AON/CPU control is the first concrete hardware
boundary worth isolating. Scheduler power/idle callbacks, peripheral service
initialization and firmware IRQ handling become possible afterwards.
There is no direct watchdog/system-reset call or PLL write in the immediate
PRNG-return sequence just traced. That does not exclude later or indirect
reset/clock effects. No direct AP I2C transaction appears here; the firmware's
known device table has AOP SPI/GPIO, so later peripheral ownership remains a
question rather than a reason to hand it a guessed bus or PMU mapping.

The Linux loader still stages ext wholly within `0x858000000/0x400000`,
16 KiB aligned, before publishing its PA. Stage 2 grants no AFK buffers and
never starts sensor endpoints. The PRNG seed context is in AOP RAM. Those
facts reduce the immediate new-buffer explanation; they do **not** prove
that all retained firmware mappings or pre-existing DMA are harmless. The
stop predicate still is command readback plus delay, not execution/DMA proof.

## Deferred and fenced entries

All entries use release
**`7.3.0-rc1-tri49a-cold-ff4b74d3b8-gc34db1b80e85`**, with the full
tri48a overlay SHA256
`ff4b74d3b8824f1955bbd2b8a6324fe69610df02a9cce5b34ded208f268f6e14`.
The prior controls and ECID-only **2e** are rebuilt, plus six new entries:

* **2ef:** test 5, `aop_defer=2`: real ECID, zero NONC, post-PRNG fence.
* **2nf:** test 13, `aop_defer=2`: real ECID, fresh NONC, same fence.
* **2ed / 2nd:** corresponding identities with `aop_defer=1`, full firmware.
* **3ed / 3nd:** matching deferred stage-3 entries, for use only after a
  successful full stage-2 handshake with that identity.

All keep D-cache off and the previous IOP mask selection. **2e still releases
automatically** and is an independent comparison; prefer deferred entries.
The immutable `aop_defer` selector is exclusive to forced cold. Mode 2 also
requires stage 2, cache off and ECID. Probe creates resources/debugfs/sysfs,
but submits no boot worker and performs no stop, load or release. Root writes
1 to `/sys/bus/platform/devices/210800000.aop/cold_release`. Duplicate and
concurrent triggers, including shutdown races, are rejected; no retry exists.

Deferred attempts keep the documented AP mailbox notification bits masked
in both wrapper views. Linux RX IRQs stay unrequested/disabled, and mailbox
RX remains polling. This does not claim to suppress every possible AOP
sideband interrupt, watchdog effect or fabric fault. The PMGR guards,
no-map reservations, main/ext hashes/readbacks, eight SRAM/status frames,
one-second wake and 20-second HELLO observation remain unchanged.

Mode 2 replaces only ext **+`0x4204` / VA `01072204`**: original bytes
`14 21` (MOVS r1,#0x14) become **`fe e7` (B .)**. Execution parks after
the root `0x21` marker and before the method-20 hardware call, task creation
and IRQ enabling. The source firmware is unchanged; original hash, exact
opcode, size, copied ext and patched-word readback are checked. It cannot
resume or stream sensors. A no-HELLO timeout is expected.

## Console persistence and recovery

Deferred triggers require an enabled, unsuspended console named **ramoops**;
an EFI pstore backend is not a substitute. Checkpoints before stop, before
RUN, after RUN/before auto-boot, and after auto-boot go to the continuous RAM
console. `pr_flush(1000, false)` and a store barrier precede further release
writes. Missing console or pre-release flush failure terminates the attempt.
The pre-RUN snapshot already exists before release.

The unchanged tri48a kernel config has built-in PSTORE_RAM/CONSOLE. Its early
boot code provides 2 MiB no-map ramoops RAM, including a 1 MiB console,
before AOP probing. Actual previous captures registered it at ~0.05 s at
`0x87b800000`. The warm merger retains that node, its console/record sizes
and header reservation into experiment and recovery kernels; the new real
merger test verifies both directions.

The old tri48a initramfs **replayed the previous console into printk**,
recursively nesting history in the console ring. The new private initramfs
overlays the repository's existing `scripts/boot/device/linux-a9-pstore`
reader, which copies/archives records without replay or unlink. Recovery
collection must include `/run/linux-a9/pstore`, `/var/lib/linux-a9/pstore`
and `/var/lib/systemd/pstore`, not just `/sys/fs/pstore`.

**RAM-only logging cannot guarantee retention through force restart/DFU or
loss of DRAM refresh.** A cold boot may also choose a different region. No
fixed PA, guessed retention register, persistent variable or protected-storage
write was added. The host helper starts and verifies a live SSH dmesg witness
before triggering, saving its output on the Mac even if the AP disappears.

## Orchestrator commands — not executed here

Start with the existing normal Linux boot/recovery entry so SSH works. Then
warm-boot the deferred fence entry; it should reach userspace without a forced
AOP attempt. Keep the current root server and recovery/default entry intact.

```sh
source build/periph/aop-force-cold/entries.env
ENTRY="$AOP_COLD2EF_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2EF_PAYLOAD"

# USB/SSH is now up. This checks the entry, ramoops, live console witness
# and runtime reservations, then writes the one sysfs trigger and captures.
scripts/periph/aop/trigger-cold.sh cold2ef-post-prng-v6
```

For a manual trigger, first run a host console capture in a separate terminal:

```sh
scripts/omarchy/ssh.sh 'sudo -n dmesg --follow --raw' | tee build/aop-cold-live.log
# In the other terminal, after checking deferred mode and ramoops:
scripts/omarchy/ssh.sh 'cat /sys/bus/platform/devices/210800000.aop/cold_release'
scripts/omarchy/ssh.sh 'printf "1\n" | sudo -n tee /sys/bus/platform/devices/210800000.aop/cold_release'
AOP_CAPTURE_WAIT_SECONDS=60 scripts/periph/aop/capture-stage23.sh cold2ef-manual-v6
```

Review that capture before each next boot. If 2ef survives and reaches the
marker, try full **2ed** independently with the same two helper commands:

```sh
ENTRY="$AOP_COLD2ED_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2ED_PAYLOAD"
scripts/periph/aop/trigger-cold.sh cold2ed-full-ecid-v6

# Independent NONC isolation, only after reviewing the preceding result:
ENTRY="$AOP_COLD2NF_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2NF_PAYLOAD"
scripts/periph/aop/trigger-cold.sh cold2nf-post-prng-v6
ENTRY="$AOP_COLD2ND_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2ND_PAYLOAD"
scripts/periph/aop/trigger-cold.sh cold2nd-full-identity-v6

# 2e is built if the original automatic ECID-only comparison is needed:
ENTRY="$AOP_COLD2E_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2E_PAYLOAD"

# After recovery, collect all pstore locations, without replay/deletion:
scripts/boot/collect-hang-evidence.sh --transport ssh --out build/aop-v6-recovery
```

Before triggering, require `defer=2 triggered=0` (or mode 1), zero frames,
`halt_accepted=0`, and no `FORCE-COLD`/release log. After triggering, expect
`COLD test=5 ... ecid=1 nonce=0` or test 13/nonce 1, the normal loaded SRAM
frame and `checkpoint=before-run`. The last checkpoint identifies whether
loss began at RUN, auto-boot or later; absence is not a sampled firmware PC.
Wrapper mask snapshots should retain AP bits `0x1001` in both views.

For a successful fence, require coherent cache-off guard, no old PRNG fatal,
**root `01052ab8+0xc=00000021`**, current-task sentinel 4, and the original
single ready worker. The expected 20 s result is `failed-no-hello`. If the
fence itself kills the AP, the later CPU-control call and task initialization
did not execute: inspect PRNG/software memory, ext mapping and the uncertain
stop contract. If fence survives but the same-identity full release kills the
AP, the interval after the fence is implicated, not specifically bit2 alone.
If full release survives, require actual RX HELLO/EPMAP and
`cold-stage2-handshake-complete` before using its matching 3ed/3nd. Stage 3
still needs grants/rings, accel ACK/raw reports, IIO and tilt changes.

## Host validation, commits and workspace files

Kernel heads, clean and unpushed:

* `a9/aop`: **`93e851b61436f117899db973e2abe646fa545f29`**.
* `a9/tri49a`: **`c34db1b80e859565c6cdb180b0b177c396b439d1`**.

Both active/observer W=1 builds and checkpatch pass (0 errors/warnings).
Full Image.gz and 105 DTBs build; tri48a config is identical and only the two
J71t DTBs differ. All 18 entries extract identically from real payloads,
have verified manifests/hashes and command lines below 1000 bytes (deferred
989/990). Hardware validation remains false.

The ASan/UBSan suites exercise the actual trigger/checkpoint/loader functions,
concurrent duplicate admission, shutdown/missing-console/flush failures,
AP mask retention, and an exact two-byte fence on the **real pinned ext**.
Original tests pass. The helper is tested with fake local SSH, including
missing/dead host witnesses refusing the trigger. Logs under
`build/periph/aop-force-cold/`: `offline-deferred-final.log`,
`trigger-helper-deferred.log`, `build-deferred.log`,
`checkpatch-deferred.log`, `package-deferred.log`,
`package-test-deferred.log`, `reservation-deferred.log` and `re-post-prng/`.
The existing real-libfdt early reservation, boot-phase order, and no-replay
copy/archive fixtures also pass; see `pstore-deferred.log`.

Workspace files for this task, **uncommitted**:

```text
docs/periph/aop-cold-stall.md
scripts/periph/aop/build-force-cold.sh
scripts/periph/aop/package-stage23.py
scripts/periph/aop/re-post-prng.py                      (new)
scripts/periph/aop/trigger-cold.sh                      (new; orchestrator only)
scripts/periph/aop/kernel/a9-aop-memory.h               (also on kernel branches)
scripts/periph/aop/kernel/apple-a9-aop-active.c         (also on kernel branches)
scripts/periph/aop/kernel/apple-a9-aop.rst              (also on kernel branches)
scripts/periph/aop/tests/cold-identity.c
scripts/periph/aop/tests/cold-pstore.py                 (new; existing pstore fixtures)
scripts/periph/aop/tests/cold-trigger.c                 (new)
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/force-cold-package.py
scripts/periph/aop/tests/stage23-artifacts.py
scripts/periph/aop/tests/trigger-helper.py              (new)
```

> Private graphics-dependent excerpt omitted from this source snapshot.

---

# Historical: Cold2 v4 PRNG fatal; v5 identity trials

2026-10-25. Host/Docker/offline implementation only. No device access,
workspace commit or push. Kernel commits are confined to `a9/aop` and
`a9/tri49a`. Owner accepts crash risk. No syscfg, NVRAM or boot-storage IO.

**Both trustworthy cache-off snapshots identify a fatal on zero ECID.**
The firmware has reached PRNG initialization and recorded
`PRNG !patchbay ecid=%llx di=%p`. It then writes fatal stop code 4 and enters
an unconditional WFI loop with IRQs disabled, before its first task switch.
This is sufficient to explain the stable SRAM and missing HELLO. An AOP
timer, interrupt-routing, power/DART handshake or AP-ready event is not the
wait at this point. Those could still become subsequent problems after the
fatal is fixed; this work does not claim sensor success.

The earlier v3 analysis below is historical. In particular, its interpretation
of the ext return `010721fb` as the platform call was wrong: the ext literal
at `0107231c` is **PRNG object `01047a80`**, and `010721f8 BLX r6` invokes its
method 1. Platform initialization has already completed. Its earlier
cache-incoherence warning was correct for v3, but no longer prevents decoding
these cache-off v4 captures.

## Frozen evidence and exact fatal path

The source captures are unchanged:

* `build/periph/aop-stage23/cold2c-v4-20261025T132921Z/` (cache off).
* `build/periph/aop-stage23/cold2b-v4-20261025T133030Z/` (cache off + IOP IRQs).

Each retains one complete before/after pair, not seven independent raw dumps.
All seven post-release frame hashes equal that capture's final raw snapshot.
The guards match STKG (`c7ea00ab` / `ab0070c4`). Cache-off is present in the
loaded reset instruction. Samples remain non-atomic, and no PC was sampled.
The [derived report for both captures](../release/OMISSIONS.md) contains
hashes, raw-word profiles, validated saved returns, task decoding and the
structured fatal record. No historical capture was edited.

Addresses here are linked firmware VAs, not AP PAs. Main/ext pins remain
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8` /
`2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22`.
The important instructions and structures agree in both captures:

* `01034e9c` is the PRNG method dispatcher. Its object at `01047a80`
  has zero override-seed/identity fields, so method 1 imports the `PRNG`,
  `ECID`, and `NONC` tags through literal addresses `01038b4f`, `01038b77`,
  and `01038b87` respectively. The tag payloads start eight bytes later.
  PRNG has 32 random bytes, but ECID and NONC are zero.
* `01034f98 LDRD r5,r6,[sp,#0x18]` loads the imported ECID;
  `01034f9c ORRS` / `01034fa0 BEQ 0103500c` tests whether it is zero.
  `0103500c` reads device-info pointer `[r4,#0x3c]` (valid `01047404`),
  sets the two ECID argument words to zero, and `0103501a` calls the fatal
  formatter `01034a44`. A separate check at `01034faa` fatals if device-info
  is null. Here the missing field is ECID, not device-info or PRNG seed.
* The boot stack contains the validated return `0103501f` at `01046b30`.
  Its saved AAPCS varargs have an alignment word, ECID low/high zero, and
  device-info `01047404` at `01046b40`. This is corroborating saved data,
  not a claim that every residual stack word is a live frame.
* `0103310c` records the fatal in **`01052b08`**, not in the endpoint-4
  context. Depth is 1, first type `0x20` at +0x2c, record size `0x160`
  at +0x34, and format pointer **`01039ac4` at +0x38**. That string is
  `PRNG !patchbay ecid=%llx di=%p`. Its crash-buffer pointer at +0x14 is
  zero, explaining why a search for a constructed CLHE record found none.
* `01034a68` loads stop reason 4 and `01034a6a` calls `01034a2c`, which
  disables IRQs, stores **4 to `0106ca3c`**, and calls `010328d8`.
  `010328e2 BL 0102760c` / `010328e6 B 010328e2` is unconditional.
  The mode byte at `0106ca38` is zero, selecting `DSB; WFI` at
  `01027612..01027618`, with the wait instruction at **`01027616`**.

There is **no resumable wait object**. An interrupt may wake WFI, but the
caller loops back without returning to PRNG initialization. Neither an AON
tick nor a 0x220 mailbox wake repairs this fatal continuation. The evidence
pins the executed non-returning path; it does not pretend to read a current
PC from SRAM.

## Scheduler, RTKit and logs

The task registry is `0106d27c` (array pointer) / `0106d280` (capacity),
as proved by `010293c4` and diagnostic exporter `010297a8`. Capacity is 8,
array `01043568`, with **one populated task**:

* TCB `01043b08`, name `rtk_ep_work`, ID 0, effective/base priority 25.
* TCB +0x0c wait object = 0; suspended byte +0x16 = 0. Its stack is
  `010435a0 + 0x400`; context `010439b0` retains initial SP `010439a0`,
  LR trampoline `01029edd`, entry argument `01033a8d` and context argument
  `0106c8a8`. It has not switched to the worker stack.
* Ready bitmap `0106c754 = 02000000`, ready count `0106c758 = 1`
  (incremented by enqueue `01029654`, decremented by dequeue `01029a54`).
  Priority-25 queue at `0106c824` has head/tail `01043b08`.
  Current-task global **`01052a0c = 4`** is the pre-task sentinel, not a
  TCB pointer or thread ID 4. The lists at `010529fc` and `01052a04`
  have null heads and self-tail pointers. There is no separate idle task
  in this initialized registry; this wait is on the boot system stack.

The endpoint registry has capacity 8 and slot 0 transport `010448e0`.
The management context list at `01052a68` leads to `01043500`; context
state is **1**, endpoint handle and received-version fields are zero.
The receiver `010311ac` changes that context to 3 only on accepted version
negotiation. RTKit root `01052ab8 + 0xc` also remains 1; the ext instruction
`01072202` would set it to `0x21` immediately after PRNG returns. Thus
there is a constructed transport/worker, but startup has not dispatched it
to send HELLO.

The normal log endpoint context is `0106c8c8`: its PA fields +8/+0xc are
`ffffffff/0`, length +0x10 is `0x1000`, and no AP log buffer has been
allocated. Endpoint-4 context `0106c908` has been constructed, but the fatal
recorder's crash-buffer pointer is null. There is no initialized external
log/crash ring to follow here. The useful panic evidence is the in-SRAM
structured record and its format pointer. The inspector follows only bounded
pointers into the local captured data RAM; it never follows a DMA/MMIO PA.

## Next cause and entries

Only one demonstrated cause warrants a new intervention: **missing loader
identity**. Two entries isolate it, both keeping D-cache off and the original
IOP-mask behavior:

* **cold2e / cold3e**, `aop_cold_test=5`: real ECID, NONC left at zero.
  This changes the exact missing prerequisite; it tests whether supplying
  ECID is sufficient to get past this initialization.
* **cold2n / cold3n**, `aop_cold_test=13`: real ECID plus eight fresh random
  NONC bytes, matching the iOS identity pair. Use this first for the fuller
  loader contract, then the e variant if isolating NONC is useful.

The identity is read from the already-pulled private IODeviceTree's
`/chosen/unique-chip-id`. `make-boot-ecid.py` exports its eight raw
little-endian bytes to ignored `build/periph/aop-force-cold/boot-ecid.bin`;
the packager stages `apple/aop/boot-ecid.bin` in the initramfs. No ID is
hardcoded, invented or put in a public DTS or boot argument. NONC is generated
by `get_random_bytes()` at each selected load. The saved iOS boot-nonce is
not reused; this is not a persistent boot-nonce/NVRAM operation.

Selectors 0..7 and 12..15 are valid bit combinations; 8..11 are rejected
because NONC requires ECID. The real-ID file is requested only when forced
cold and bit2 is set. Missing, zero or non-eight-byte IDs fail the load before
copy/release. Both requested tag locations/lengths are checked before either
field is changed. Both pinned image hashes, instruction check, ext/main SRAM
readback, PMGR guard, reservations, eight status/SRAM frames, one 1-second
wake, 20-second HELLO deadline and stage2/stage3 gates are retained.
There is no assertion bypass, timer/clock/IRQ-controller register guess,
new AP protocol word, DART/SART change or firmware reset pulse.

Final kernel heads (clean; no push):

* `a9/aop`: **`485d77bcc7a4a2c46d2d62e3d527fbb597f67994`**,
  after feature commit `6a269c523c6830434aa47836b6deda1e41d55c03`.
* `a9/tri49a`: **`6a6abc93057eccabc1955b50c9c1a9592135289e`**,
  after feature commit `ddf93b8f6e259527873b3d44f4ade16f10d54daf`.

Release `7.3.0-rc1-tri49a-cold-bfcb3c8e48-g6a6abc93057e`.
Full tri48a overlay SHA256
`bfcb3c8e48662dbd995e1fbf3ba641c3903807fe7013a14e125ab36f1c1c6275`.
All twelve entries (the previous eight controls plus four new entries),
payloads, manifests and `entries.env` are in `build/periph/aop-force-cold/`.
e cmdlines are 997 bytes; n cmdlines 998 bytes. Hardware validation is false.

## Orchestrator commands — not executed here (v5)

Review each capture before another boot. Keep the recovery/default entry and
the current root server intact. These commands only describe the orchestrator's
next device actions:

```sh
source build/periph/aop-force-cold/entries.env
ENTRY="$AOP_COLD2N_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2N_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2n-prng-v5

# Optional independent boot to isolate ECID from NONC:
ENTRY="$AOP_COLD2E_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2E_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2e-ecid-v5

# Only after cold2n reaches HELLO/EPMAP, use the SAME variant at stage3:
ENTRY="$AOP_COLD3N_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD3N_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold3n-prng-v5
```

For n, look for `COLD test=13 dcache_off=1 iop_irqs=0 ecid=1 nonce=1`;
for e, `test=5 ... ecid=1 nonce=0`. The captured reset word must remain
`e3011001` and runtime guard must agree with STKG. The offline decoder must
show `ECID_nonzero=1`, and n should also show `NONC_nonzero=1`.

Progress past this fatal should remove the old `PRNG !patchbay` record and
stop code 4, initialize PRNG context, and allow the ext store of `0x21`.
Those are progress markers, not the success predicate: require actual RX
HELLO/EPMAP and `cold-stage2-handshake-complete`. If it still stalls, preserve
the binaries and decode the new current task, wait object, fatal format and
stack. A new fatal or later scheduler wait must be analyzed on its own;
do not jump to speculative clock/controller writes. If only n works, NONC is
also needed somewhere downstream; it was not the direct zero test shown here.
For stage3 still require AFK grants/rings, accel ACK/raw reports, IIO, and
changing axes on tilt.

Host-only reproduction:

```sh
python3 -B scripts/periph/aop/re-cold-wait.py
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python -B \
  scripts/periph/aop/firmware-attach-re.py --cold-wait
python3 -B scripts/periph/aop/test-offline.py
KERNEL_JOBS=4 scripts/periph/aop/build-force-cold.sh
python3 -B scripts/periph/aop/package-force-cold.py
python3 -B scripts/periph/aop/tests/force-cold-package.py
```

`re-cold-wait.py` verifies the two captures against their seven post-release
hashes and writes `build/periph/aop-force-cold/re-wait/evidence.json` plus
the derived report. Exact instruction windows, named functions and decompiled
code are in `build/periph/aop-attach/firmware/`. The raw Thumb windows are
authoritative where Ghidra's computed-dispatch function recovery is imprecise.
The shared kernelcache Ghidra server was neither replaced nor stopped.

## Validation and this task's workspace files (v5)

Host ASan/UBSan suites pass, including the actual identity-loader function
and actual tag writer on the pinned image, with byte-for-byte checks of
ECID-only versus ECID+NONC and every unaffected byte. Missing/short/long/
zero IDs, missing NONC, and malformed images refuse without a partial identity
patch. The stop/drain/loader suite covers all valid selector combinations;
capture, timeout, mailbox, AFK, observer and reservation suites remain passing.
The decoder reproduces both real v4 fatal/task states and refuses out-of-range
task pointers; the exporter rejects missing, malformed and ambiguous IDs.

Both branches pass W=1 active/observer builds and whole-task checkpatch
with **0 errors, 0 warnings**. Full Image.gz and 105 DTBs build. Config equals
tri48a and only the two J71t DTBs differ. All twelve real payloads extract
identically to their entries; hashes, pinned main/ext, saved calibration,
private ECID, single gzip member, base archive and warm/recovery reservation
retention are verified. Logs are `offline-identity.log`,
`offline-identity-final.log`, `build-identity-final.log`,
`checkpatch-identity-final.log`, `package-identity-final.log`, and
`package-test-identity-final.log` under `build/periph/aop-force-cold/`.

This task's workspace files are **uncommitted**:

```text
docs/periph/aop-cold-stall.md
docs/periph/aop-cold-wait-memory.txt                 (new derived report)
scripts/periph/aop/build-force-cold.sh
scripts/periph/aop/firmware-attach-re.py
scripts/periph/aop/inspect-cold-memory.py
scripts/periph/aop/make-boot-ecid.py                 (new)
scripts/periph/aop/re-cold-wait.py                   (new)
scripts/periph/aop/package-stage23.py
scripts/periph/aop/kernel/a9-aop-afk.h               (also on kernel branches)
scripts/periph/aop/kernel/a9-aop-memory.h            (also on kernel branches)
scripts/periph/aop/kernel/apple-a9-aop-active.c      (also on kernel branches)
scripts/periph/aop/kernel/apple-a9-aop.rst           (also on kernel branches)
scripts/periph/aop/tests/cold-identity.c             (new)
scripts/periph/aop/tests/force-cold-package.py
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/memory.c
```

> Private graphics-dependent excerpt omitted from this source snapshot.

---

# Historical: cold2 v3 cached SRAM and cache/IRQ isolation

2026-10-25. Implementation sub-agent: host/Docker/offline Ghidra only, no
device access, workspace commit or push. Only `a9/aop` and `a9/tri49a` kernel
branches were committed. Owner accepts crash risk. No syscfg, NVRAM or boot
storage access. This follows [cold-hello](aop-cold-hello.md) and the
[device capture](../release/OMISSIONS.md), workspace commit `3fe69d3`.

**The loaded image reaches C, constructors and ext code. The frozen SRAM is
not a coherent view of AOP cached data. Its unchanged globals cannot identify
the exact wait or exclude a crash.** The next builds isolate IOP mailbox
notification masks and make data/stack/log writes visible by disabling AOP
D-cache at reset. Neither experiment is a verified hardware fix.

## Evidence and mapping

The binary pair is retained locally at
`build/periph/aop-stage23/cold2-hello-v3-20261025T130758Z/{cold_before,cold_after}.bin`.
The committed capture directory contains the text summaries; its frozen raw
binaries are in the ignored build directory. Both are exactly `0x9b000` bytes.
Before SHA256 is `39b95ce18a8fefb9b11b67ad9ee6fc029782f0f5d612e0b964c2c75cc3f8ae43`,
after `5a58eb22a7e56ffbab46bdd26efd1769e76ce475c66e617794604762e7aef8ef`.
All seven post-release frame hashes match. There is one retained final raw
snapshot, not a distinct binary for every frame. Samples are non-atomic.

[The mapped report](../release/OMISSIONS.md) preserves the full profile and
validated stack-return candidates. All addresses below are **linked AOP VAs**,
not AP physical addresses. Names are inferred from the stripped instructions.
Main/ext are still the pinned iOS 20H350 images, SHA256
`af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8` /
`2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22`.

* Of 907 changed data words, 570 are in the LPAE table range
  `0103a000..0103efff`, 169 in the initial allocator arena
  `0103f000..01044bff` and 120 in the boot system stack
  `01044c00..01046bff`. Other changes include cache geometry at `010529a0`,
  mailbox objects and runtime callbacks. The remaining 360 BSS changes include
  zeroing and constructed objects; they are not 360 zero-only words.
* **Guard import ran.** `01027412 -> 01000b5c` copies STKG into `01047474`.
  The patched `514b4c00` is saved on the new boot stack at `01046b84`, while
  AP-visible `01047474` remains the file default `00000aff`. C entry has
  already returned from guard init and cache-geometry setup. Seeing only the
  global would have incorrectly bounded progress before guard init.
* **Constructors and ext execution ran.** Stack `01046bdc` holds return
  `0102741b` after `01027416 -> 010275a4` (constructor loop). `01046bcc` holds
  `010275b5` after its `BLX r0`. `01046b9c` holds `010721fb`, matching the
  pinned ext `010721f8 BLX r6`, which invokes the platform device's method 1.
  `01046b5c` holds `01034485`, after the platform initializer's
  `01034482 BLX r4` into its interrupt-controller object `01052da8`.
  Other saved return candidates include allocator/MMU/mutex paths. These are
  instruction-validated saved words, **not a captured PC or reliable live
  backtrace**. Old frames and partially evicted cache lines can coexist.
* **Caching explains the contradiction.** Reset first clears SCTLR.C at
  `010000a4..b4`, then `0100029c MOVW r1,#0x1005` / ORR / SCTLR write enables
  M, C and I. The observations are consistent with AP reads not snooping
  dirty AOP D-cache lines. The new STKG
  reaches a stack spill while its global backing is stale; the EC0p backing
  object at `01047510+8` is also zero despite the ext return. Dirty AOP cache
  lines are the leading explanation, independently supported by these two
  contradictions. Constant backing hashes do not establish a halted core.
* **RTKit structures are partly visible.** `01052a3c` contains registry count
  8 and array `01052a1c`. `01052a68` holds management context-list head/tail
  `01043500/0104353c`; those heap bytes are zero in AP backing. The scheduler's
  current-task backing at `01052a0c` is still sentinel 4, despite task/stack
  construction. These are more examples of visibility limits, not a proved
  state-machine state 0 or a nonexistent task.
* **Mailbox layout is consistent.** Platform dispatcher `010341f0` constructs
  objects `01052c28` and `01052ce8` with the `wrapper+0xb80` view and the latter's
  companion `wrapper+0x4000` view. The capture contains mapped bases
  `c1800b80` and `c1804000`. `0102ce1c` selects TX/RX data offsets `0x10/0x18`
  and status `8/0x20` in the small view, or `0x30/0x38` and status `0x20/8`
  in the companion view. This supports the existing AP `+4010/+4038` FIFO
  pair. `+ba0` and `+4020` remain identical/empty; there is no evidence of a
  second populated outbox to pop.
* **No identified panic record.** Log endpoint context `0106c8c8`, whose
  buffer PA fields are at +8/+c in `010326f0`, and crash endpoint 4 context
  `0106c908` remain zero in backing. No changed CLHE/CLHT/CLSD/PANIC/panic:
  signature or changed log text was found. `deadc0de` at `01052998` is a lock
  state used by `010285xx`, not a crash signature. The untouched exception
  stacks and scratch do not exclude a cached panic/log buffer.

## What is it waiting for?

**A live PC/wait is not recoverable from this cache-incoherent capture.** The
strongest useful bound is execution through platform/transport constructors,
including ext. Interrupt-controller bring-up and later RTKit scheduling remain
candidates. In particular `0102c654` is a real `wrapper+0x81c != 0` spin in
interrupt-controller method 4; the saved constructor return does not prove the
core is in that loop. No speculative write to acknowledge that register is
added, and reading it could consume an interrupt, so it is not added to snapshots.

Candidate ranking for the next device run:

1. **IOP mailbox notifications.** AP-visible mask `+4000` stays `0x110`, bits4/8,
   while firmware clears the secondary mask `+c00`. The one `0x220` wake changes
   inbox status/idle but is not consumed into a reply. Clearing both IOP bits in
   both mask-clear views is a bounded, already-used wake-family operation. It
   tests the AP/IOP notification asymmetry; AP polling alone never bypassed an
   IOP-side IRQ mask. Whether these AP mask bits actually gate the IOP on this
   revision remains a hardware question.
2. **Noncoherent data / retained cached state.** Cache-off is primarily a
   diagnostic, and may also change retained-state/cache behavior. It supplies
   observable runtime guard, EC0p, scheduler, endpoint and stack state. If it
   produces HELLO it implicates cache/timing/visibility effects, without proving
   which cache bug caused the original silence.
3. **Clock/timer or another initialization wait.** Not excluded. Inspect the
   cache-off stack/state first. There is no justified AON frequency or timer
   reprogramming from this capture, so none is guessed.
4. **Ext access.** Complete inability to fetch ext at `0x858000000` is strongly
   disfavored by the validated ext call/return and patched guard in its call
   chain. This does not validate all pages or later DMA. No DART/SART mapping or
   alternate physical address is invented.
5. **AP must send first / other FIFO.** Not supported by this evidence. The
   existing pinned state-transition routine sends HELLO itself, and neither RX
   status view contains a word. Keep the single documented wake; no synthetic
   HELLO/reply or alternate FIFO pop is added.

## New entries

Immutable boot selector `aop_cold_test=0..3`, rejected without forced cold or
outside its range:

* `0`: stock cold2/cold3, preserving the v3 experiment.
* `1`, suffix **c**: replace exactly one pinned ARM reset instruction at +29c,
  `e3011005` (MOVW r1,#1005) -> `e3011001` (MOVW r1,#1001). Early startup already
  clears C. Keep MMU and I-cache enabled, all other instructions/tags unchanged.
  Both original hashes and the exact old opcode must match; patch only the
  temporary copy, compare its SRAM readback, and include it in the baseline
  snapshot. Main/ext files in the initramfs retain their original pinned hashes.
* `2`, suffix **i**: unchanged firmware, append two checked writes of `0x110`
  to `+4004/+c04` after vendor mask setup and before RUN/auto-boot. Both store
  failures terminate before release. No additional control write after release.
* `3`, suffix **b**: both changes. This suffix means **both**, distinct from
  the historical cold2b capture label. Full entry identity includes the hash.

Every variant keeps the eight guarded SRAM/status frames, exact reserved
aperture, one wake at 1 second, 20-second HELLO deadline, no Linux RX IRQ and
single attempt. Stage2 stops after HELLO/EPMAP; matching stage3 continues AFK.
No domain force-on, clock write, storage access or reset pulse is added.
Two pre-existing resource-size format warnings were fixed, and the build
script now actually fails if W=1 emits warnings (a negated grep under `set -e`
was not a reliable failure gate).

Final kernel commits (both clean, original v3 commits remain ancestors):

* `a9/aop`: `680c26dbbaf20ce54c3ff28a3bf8519892d5f561`
  (experiment parent `966011c1bc`).
* `a9/tri49a`: `dca67f0fab5da2fcd4297a1a135d7570abceded6`
  (experiment parent `fb05999999`).

Release `7.3.0-rc1-tri49a-cold-fbc27badd2-gdca67f0fab5d`.
Full tri48a overlay SHA256
`fbc27badd27ca96650d5345bed499dfc6d637f1058cd8d68fc994a308a6d307f`.
All eight payloads/entry directories, pinned firmware manifest, hashes and
`entries.env` are under `build/periph/aop-force-cold/`. Stock cmdlines are
980 bytes; test cmdlines are 997 bytes. Hardware validation remains false.

## Orchestrator commands — not executed here

Start with cache-off to get interpretable state if still silent. Use separate
boots and review each capture before the next; do not loop through the entries.
Keep recovery/default and the existing root server intact.

```sh
source build/periph/aop-force-cold/entries.env
ENTRY="$AOP_COLD2C_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2C_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2c-cache-v4

# Independent IRQ-only test after reviewing the previous capture:
ENTRY="$AOP_COLD2I_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2I_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2i-irq-v4

# Combination, if the separate tests warrant it:
ENTRY="$AOP_COLD2B_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD2B_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold2b-both-v4

# Only after stage2 of this same variant reaches HELLO/EPMAP, e.g. cache-off:
ENTRY="$AOP_COLD3C_ENTRY" scripts/boot/kexec-host.sh "$AOP_COLD3C_PAYLOAD"
AOP_CAPTURE_WAIT_SECONDS=240 scripts/periph/aop/capture-stage23.sh cold3c-cache-v4
```

The collector automatically adds the pinned firmware mapping when host images
are available. Reinspect any complete capture without touching hardware:

```sh
python3 -B scripts/periph/aop/inspect-cold-memory.py /path/to/capture \
  --firmware build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin \
  --ext build/periph/aop-r2/firmware/apple/aop/ipad6faop_ext-20H350.bin \
  --changed-csv build/periph/aop-force-cold/re-stall/changed-words.csv

# Reproduce exact instruction windows with host Capstone:
python3 -B scripts/periph/aop/firmware-attach-re.py --cold-stall --windows-only
# Reproduce decompilation/names using the installed Ghidra Python environment:
${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python -B \
  scripts/periph/aop/firmware-attach-re.py --cold-stall
```

Look for `cold_test`, `COLD test=`, then the unchanged success predicates:
RX HELLO/EPMAP and `cold-stage2-handshake-complete`. Cache-off baseline must
contain `e3011001` at +29c. Expect runtime guard==STKG, populated EC0p backing,
and additional visible stack/heap/state changes if C executes normally. A
silent cache-off run is valuable: retain both binaries and inspect those now
visible words, current-task state, log/crash context buffers and validated
return candidates. The code still does not claim a live PC.

IRQ-only/combined: loaded mask is pre-start; **released** should show
`+4000` without bits4/8 (possibly further firmware changes in +c00). If those
bits reappear, preserve the frame that shows it. HELLO in IRQ-only while stock
remains silent supports the IRQ candidate. Cache-off alone working supports a
cache-related/timing effect. Combined alone working does not isolate causality.
For stage3 require reserved grants/rings, accel ACKs/raw reports, IIO and changing
axes after tilt; HELLO or SRAM activity alone is not sensor success.

## Validation and workspace files

> Private graphics-dependent excerpt omitted from this source snapshot.

Logs under `build/periph/aop-force-cold/`: `build-stall-final.log`,
`offline-stall-final.log`, `checkpatch-stall.log`, `package-stall.log`,
`package-test-stall.log`; mapped word CSV in `re-stall/`, new named firmware
functions and instruction windows in `build/periph/aop-attach/firmware/`.
The shared kernelcache Ghidra server was not replaced or stopped.

This task's workspace edits, **uncommitted** (only the three kernel templates
below are also committed on the authorized kernel branches):

```text
docs/periph/aop-cold-stall.md                         (new)
docs/periph/aop-cold-stall-memory.txt                 (new, derived report)
docs/periph/aop-cold-hello.md                         (follow-up link)
scripts/periph/aop/build-force-cold.sh
scripts/periph/aop/capture-stage23.sh
scripts/periph/aop/firmware-attach-re.py
scripts/periph/aop/inspect-cold-memory.py
scripts/periph/aop/kernel/apple-a9-aop-active.c
scripts/periph/aop/kernel/a9-aop-memory.h
scripts/periph/aop/kernel/apple-a9-aop.rst
scripts/periph/aop/package-stage23.py
scripts/periph/aop/tests/force-cold.c
scripts/periph/aop/tests/force-cold.py
scripts/periph/aop/tests/force-cold-package.py
scripts/periph/aop/tests/memory.c
```

> Private graphics-dependent excerpt omitted from this source snapshot.

