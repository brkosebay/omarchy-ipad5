# A9 AOP resume, round 2: `0x201` is followed by a reboot

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-26, offline only. No device access was performed. This pass uses the
iOS 16.7.10 J71t kernelcache, the pinned `ipad6faop-20H350` firmware, the saved
ADT/IORegistry, the two handoff captures under `logs/aop-handoff-20261026/`,
the wrapper-wake capture under `logs/aop-wake-20261026/`, and the latest SEP
result in `docs/sep/xart-handshake.md`.

Evidence labels below are **[E]** directly established by code/data or a saved
capture, **[I]** an inference from that evidence, and **[U]** still unverified
on hardware.

## Verdict

**iOS does not resume an AOP that acknowledged IOP power request `0x201` by
sending `0x220` to its state-8 WFI.** [E] The `0x201` type-7 reply sets
RTBuddy's host `_iopStatus` to **2, Sleep**, not 3, Quiesce. RTBuddy then calls
`AppleA7IOP::stop`, which disables notifications, clears CPU RUN and
`auto-boot`, deactivates the IOMapper, and leaves the always-on AOP power
domains up. On the next power-up it calls `AppleA7IOP::start` with
`resume=false`, remaps the firmware, sets RUN and `auto-boot`, and waits for a
new HELLO/boot sequence. This is a CPU stop/reboot, not an AOP rail power
cycle and not an in-place WFI wake.

`0x220` belongs only to the distinct RTBuddy host-status-3 Quiesce path. [E]
The state-3 branch starts the provider with `resume=true`, deliberately skips
RUN, sends one `0x220`, and waits for Running. Since a `0x201` reply produces
status 2, iOS never takes that branch for a `0x201`-slept AOP.

The round-1 experiment reproduced the wrong branch. [E] The wrapper writes
were real, but `setActive(true)` is IOMapper/DMA activation and the secondary
`+0xc00/+0xc04` ports are write-only set/clear aliases. There is no omitted
per-message doorbell: send is one 64-bit store to `+0x4010`. Replaying those
writes and posting `0x220` therefore did not reproduce what iOS does after
`0x201`. The first clean handoff queued `0x220` without the wrapper replay
(`in4008=0x1201`); the second replayed the full wrapper sequence and still
left it queued as `in4008=0x3401` for 20 seconds, with no reply or crash.

The next device experiment should be the already-built, explicit SPI0-reset
cold reload, entry `aop-spi0-accel-f695e71125`. Do not test NMI, another
wrapper replay, or IOMapper activation first.

## 1. The iOS path, end to end

### 1.1 AppleSPU is an endpoint client, not the AOP CPU-resume mechanism

`AppleSPU::start` registers three IOKit power states and joins the provider's
power tree. `AppleSPU::setPowerState` at `0xfffffff005b9e6d8` handles
AppleSPU endpoint bookkeeping: state 2 logs `ap-wake`, while state 0 tears
down report/child state, and the requested state is stored at object `+0x270`.
It contains no wrapper, CPU RUN, auto-boot, IOMapper or PMGR write. [E]

The AOP CPU lifecycle is owned by RTBuddy above the AppleA7IOP provider.
Ghidra had incorrectly recovered the transition switch as part of the
function beginning at `0xfffffff005b65b54`. The short logical routine at
`...5b54` ends at `...5ba4`; the IOP power-transition routine begins at
`0xfffffff005b65bb0`. [E] The shared project now names the merged recovery
`RTBuddy_releaseHold_then_IOPPowerTransition` and comments the boundary.

### 1.2 The decisive RTBuddy state mapping

The RTBuddy status-name table at `0xfffffff005310fc8` is: [E]

| Host status | Name |
|---:|---|
| 0 | Unused |
| 1 | Off |
| 2 | Sleep |
| 3 | Quiesce |
| 4 | Wait Vr |
| 5 | Vers OK |
| 6 | Ready |
| 7 | Pwr Dwn |
| 8 | Running |

Do not confuse RTBuddy host status 8, Running, with the AOP firmware's
internal low-power target 8. They are unrelated number spaces.

`RTBuddyManagement_requestIOPPowerState` at `0xfffffff005b71750` indexes the
following type-6 payload table at `0xfffffff0053110a0`: [E]

| RTBuddy transition target | Wire payload |
|---:|---:|
| 0 | `0x201` |
| 1 | `0x220` |
| 2 | `0x202` |
| 3 | `0x000` |

For RTBuddy transition command 0 or 2, the routine at `...5bb0` first changes
the host status to 7, Pwr Dwn, sends `0x201` or `0x202`, and waits. The type-7
receive dispatcher maps the low reply values 1 and 2 to host status 2,
Sleep. The `0x201` ACK does not create status 3 anywhere in the management
dispatcher. [E]

After the reply it:

1. disables the provider's two AP-side interrupt event sources through
   provider slot `+0x580(false)`;
2. releases the endpoint services;
3. calls provider slot `+0x550`, `AppleA7IOP::stop`.

For transition command 1, RTBuddy creates the management endpoint if needed,
ensures the firmware object is available, saves the old `_iopStatus`, and
calls provider slot `+0x548` with `resume = (old_status == 3)`. It then sets
host status 4 and sends `0x220` **only if that saved status was 3** before
waiting for host status 8. [E]

The actual `0x201` lifecycle is therefore:

```text
running firmware
  -> type 6 / 0x201
  -> firmware internal state 8, ACK 0x201, WFI
  -> RTBuddy host status 2 (Sleep)
  -> AppleA7IOP::stop: notifications off, RUN=0, auto-boot=0
  -> later RTBuddy command 1, old status 2
  -> AppleA7IOP::start(..., resume=false)
  -> remap firmware, wrapper setup, RUN=1, auto-boot=1
  -> new HELLO/RTKit boot to RTBuddy host status 8
```

The separate status-3 lifecycle is:

```text
RTBuddy host status 3 (Quiesce)
  -> AppleA7IOP::start(..., resume=true)
  -> wrapper/IOMapper setup, no RUN write
  -> one type 6 / 0x220
  -> restored-state ACK / Running
```

The round-1 handoff produced firmware ACK `0x201` but made the incoming Linux
kernel act as though RTBuddy had recorded status 3. That state substitution is
the root error. [E]

`RTBuddyManagement_notifyAPPowerState` at `0xfffffff005b71504` is a separate
type-B protocol. It maps AP state 3 to wire 0, state 1 to `0x20`, and state 0
to `0x201`. It does not send the AP `0x10` used by the Linux quiesce protocol
and does not change the conclusion above. [E]

### 1.3 What stop writes on J71t

`AppleA7IOP::stop` begins at `0xfffffff005b4b8fc`. On this saved J71t
provider it performs the following observable operations in order: [E]

1. `setOutboxEnable(false)`: read-modify-write wrapper `+0x4020`, clearing bit
   0;
2. disable the two AP `IOInterruptEventSource`s through provider slot
   `+0x580(false)` (RTBuddy already made the same call before entering stop);
3. call the V2 idle hook, which returns success without proving an AOP DMA
   drain;
4. `setCPURun(false)`: read wrapper `+0x44`, write it with bit 4 clear, then
   write `0` to mapped `auto-boot` at `0x210000600`;
5. call the V2 unmap hook, which is a `RET`;
6. call IOMapper `setActive(false)`;
7. call `powerOff`.

`powerOff` at `0xfffffff005b4b698` skips the generic clock/power-gate calls
because this provider has `aot-power=1`. Even without that property, the
saved `clock-gates`, `power-gates`, and `clock-ids` arrays are empty and the
generic index-0 operations reject the index before reaching PMGR. Thus stop
does **not** change `ps_aop`, `ps_aop_cpu`, `ps_aop_busif`, `ps_aop_filter`,
or `ps_aop_gpio`. [E]

The result is precise: iOS turns off AOP CPU execution and mailbox delivery,
but it does not power-cycle the AOP island.

### 1.4 What the subsequent non-resume start writes

`AppleA7IOP::startCPUWithOptions` begins at `0xfffffff005b4b738`. With
`resume=false`, the relevant J71t sequence is: [E]

1. `powerOn`; it performs no PMGR operation for the same `aot-power=1`
   reason;
2. IOMapper `setActive(true)` at provider `+0x128`; this activates DMA
   translation, not AOP power or CPU execution;
3. program the firmware remap descriptor, in this order:

   ```text
   wrapper +0x18 <- 0
   wrapper +0x20 <- 0
   wrapper +0x28 <- 0x0009b000
   wrapper +0x30 <- 0
   wrapper +0x08 <- 0x10e00000
   wrapper +0x10 <- 0x00000002
   wrapper +0x38 <- 1
   ```

   This maps AP PA `0x210e00000`, length `0x9b000`. RTBuddy may retain the
   firmware bytes in its firmware object, so the evidence proves remap and
   reboot, not a fresh byte-for-byte FUD copy on every wake.
4. establish the V2 wrapper masks:

   ```text
   +0x4000 <- 0x1111      +0x0c00 <- 0x1111
   +0x4004 <- 0x0001      +0x0c04 <- 0x0001
   +0x4004 <- 0x1000      +0x0c04 <- 0x1000
   ```

   `+0xc00/+0xc04` are write-only aliases; their readback is not state.
5. enable the two AP event sources (ADT/AIC 133 and 130);
6. read-modify-write `+0x4020`, setting bit 0;
7. skip the optional timebase sync on J71t because its force property is
   absent;
8. because `resume=false`, `setCPURun(true)` read-modify-writes `+0x44` with
   bit 4 set and writes `1` to `0x210000600` (`auto-boot`).

The firmware then enters from its reset vector and performs a new RTKit HELLO
and endpoint bootstrap. There is no separate doorbell after a mailbox send:
`AppleA7IOPV2::postMailbox` at `0xfffffff005b4e4a8` is one 64-bit store to
`+0x4010`. The NMI helper at `0xfffffff005b4e830` writes
`+0xc004=0x11`, then `+0xc024=1`; neither the `0x201` stop/reboot path nor the
status-3 normal resume path calls it. [E]

## 2. What firmware state 8 actually arms

### 2.1 State-8 entry and WFI

In the pinned firmware, `rtk_power_transition` is at `0x01032c18`. A decoded
`0x201` selects internal target 8. The state-8 path is: [E]

| Address | Operation |
|---:|---|
| `0x01032d4c` | mask CPSR IRQ |
| `0x01032d54` | mask CPSR FIQ |
| `0x01032d6a` | record power-transition entry |
| `0x01032d74` | suspend/context preparation |
| `0x01032d7a` | cache preparation when required |
| `0x01032d84` | send the type-7 `0x201` acknowledgement |
| `0x01032d98` | call `aop_cpu_wfi` |
| `0x01032da8` | after WFI, issue callback event `0x14` |
| `0x01032dac` | record power-transition exit |
| `0x01032e02` / `...e0c` | restore the pre-transition FIQ/IRQ masks |
| `0x01032e2e` | run registered power-up callbacks |
| `0x01032e64` | acknowledge the restored target, normally wire `0x20` |

`aop_cpu_wfi` at `0x0102760c` executes DSB then WFI. On one global mode it
also temporarily changes a coprocessor interrupt-control bit, but it does not
program the Apple wrapper or choose a wake source. The state-8 block likewise
contains no interrupt-controller enable, wrapper mask, mailbox-enable,
doorbell, timer, NMI, or PMGR write. [E]

WFI may return for an already-enabled pending interrupt even while CPSR masks
normal exception entry. The firmware is therefore relying on interrupt state
established before this transition, not arming a new state-8 source. [I]

### 2.2 The only mailbox candidate is the normal runtime data IRQ

The static AP-to-IOP receive-mailbox object is at `0x01052c28`. Its interrupt
fields at object `+0x20..+0x2c` are: [E]

```text
0x00040000  0x00040001  0x00010045  0x00010046
```

The object uses wrapper-relative receive status/data/pop registers
`+0x4008`, `+0x4010`, and `+0x4018`. Its data handler is registered from
object `+0x24`, interrupt ID **`0x00040001`**: group 4, source 1, the normal
AP-to-IOP mailbox-data source corresponding to wrapper mask bit 0. The two
group-1 IDs are error interrupts. The transmit object at `0x01052ce8` uses
group-4 sources 2/3 and group-1 errors `0x47/0x48`. [E]

`aop_mailbox_data_irq` at `0x0102d080` acknowledges the group-4 source and
wakes the RTKit receive task. The general interrupt dispatcher deliberately
does not auto-ack group 4 because the device handler performs that action.
`aop_interrupt_set_enabled` at `0x0102c72e` controls groups 1, 6 and 7; it
does not implement enable/disable for group 4. [E]

The exact answer to the wake-source question is therefore:

- state 8 arms **no dedicated WFI exit source**;
- the only identified source that could make a still-running state-8 WFI
  notice an AP message is the already-registered normal mailbox data
  interrupt `0x00040001`, wrapper bit 0;
- state 8 does **not** enable mailbox-inbox-nonempty as a new wake source, nor
  does it re-arm bit 0;
- registration of the runtime group-4 handler is not evidence for a separate
  always-on wake latch after the host has stopped the AOP CPU.

The hardware captures bound the practical result. With bit 0 and bit 12
unmasked, outbox-enable already set, and one `0x220` accepted into the FIFO,
the AOP did not consume it for 20 seconds. Whether the pre-existing group-4
line could return an otherwise untouched, still-executing WFI in a narrower
laboratory setup is immaterial to the iOS `0x201` lifecycle: iOS stops that
CPU and later reboots it. [E]/[I]

## 3. Interpreting the failed wrapper experiments

Both AOP handoffs first completed AP `0x10`, IOP `0x201`, stable RAM, idle
mailboxes and no crash. The first incoming kernel posted `0x220` without the
wrapper replay and retained it as `0x1201`. In the second, the seven V2 wrapper
stores landed; the primary mask went from `0x1111` to `0x110`, and `+0x4020`
bit 0 was already set. One `0x0060000000000220` changed `+0x4008` to occupied
`0x3401`, where it stayed for the complete 20-second window. `iop_crashed`
remained zero and no reply arrived. [E]

The newest SEP capture supplies the same control: once sepOS was running, its
AP-to-SEP FIFO also retained messages despite the same V2 wrapper setup. It
also established that `+0xc00/+0xc04` are write-only aliases, that
`setActive(true)` acts on an IOMapper, and that postMailbox has no second
doorbell. [E]

These results do not identify a missing wrapper write. They show that a
mailbox word can be queued while the receiving instruction stream is not
executing. The iOS trace explains why no such write is needed after `0x201`:
the vendor path discards the sleeping execution context and boots the AOP CPU
again. NMI is not part of that path.

## 4. Exact next device experiment: SPI0-reset reload

The minimal useful test is the already-built opt-in entry
`aop-spi0-accel-f695e71125`. It follows the now-established lifecycle shape:
halt/drain, repair the one proven retained peripheral hazard, load the pinned
image, assert RUN/auto-boot, and perform a new RTKit handshake. It sends no
`0x220`, replays no handoff wrapper wake, and uses no NMI.

From a current failed-wake/quiesced kernel with the SPI0 entry uploaded:

```sh
source build/periph/aop-wake/entries.env
scripts/boot/upload-entry.sh "$AOP_SPI0_ACCEL_ENTRY" "$AOP_SPI0_ACCEL_DIR"
scripts/omarchy/ssh.sh \
  "sudo -n /usr/local/bin/linux-a9-kexec reboot $AOP_SPI0_ACCEL_ENTRY"

# Wait for SSH in the new kernel.
Q=build/periph/aop-wake/spi0-resume-r2-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$Q"
scripts/omarchy/ssh.sh 'cat /proc/sys/kernel/random/boot_id; cat /proc/cmdline;
  sudo -n dmesg; sudo -n cat /sys/kernel/debug/apple-a9-aop-active/previous_shutdown' \
  > "$Q/preflight.txt"
grep -E 'EXPERIMENT|AOP-PREV (HANDOFF record|QUIESCE|STAGE3)' "$Q/preflight.txt"

AOP_CAPTURE_DIR="$Q/trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
  scripts/periph/aop/trigger-cold.sh spi0-resume-r2
python3 -B scripts/periph/aop/check-spi0-reload.py \
  "$Q/trigger/dmesg.txt" --entry "$AOP_SPI0_ACCEL_ENTRY"
grep -E 'FORCE-COLD|COLD (halt|stale|checkpoint)|SPI0 |AOP released|failed-' \
  "$Q/trigger/dmesg.txt" | tee "$Q/spi0-lines.txt"

# Only after the checker passes, physically tilt during this independent capture.
AOP_CAPTURE_HOLD_SECONDS=60 \
  scripts/periph/aop/capture-stage23.sh spi0-resume-r2-tilt
scripts/boot/collect-hang-evidence.sh --transport ssh --out "$Q/evidence"
```

Preflight must show this exact mode family:

```text
EXPERIMENT spi0_reset=1 wake_wrapper=0 ...
spi0_applies=live-reload spi0_pmgr=ps_spi0
```

### Pass

`check-spi0-reload.py` prints PASS and the log contains, in order: [U]

1. `FORCE-COLD stage=3`;
2. `COLD halt errno=0`;
3. any stale FIFO drain, including the retained
   `word=0060000000000220` if it is still present;
4. `SPI0 reset begin pa=210100000 cpu=halted`;
5. `COLD checkpoint=spi0-reset`;
6. `SPI0 reset errno=0 ran=1`, with SPI0 `+0x8d0` channel-3 bit 6 cleared;
7. `SPI0 pre-run errno=0 ... ch3_pending=0`, with bit 6 still clear;
8. `AOP released`;
9. new HELLO, EPMAP, system ready and all three buffer grants;
10. accel STARTEP ACK `0x20`, calibration/rate completion, first report and
    IIO registration;
11. report and ping counters advance for 60 seconds, tilt changes X/Y/Z, and
    no crashlog is produced.

The accel STARTEP result is essential: the previous un-reset reload failed
there 5/5 times with the retained SPI0 channel-3 interrupt and faulted at
`PC=0x0102f252`, `DFAR=0x4c`, `r5=0`.

### Fail and next action

- `failed-spi0-reset`: bit 6 did not clear or a reset poll timed out. The AOP
  remains halted; stop and collect the capture.
- `failed-spi0-reasserted`: bit 6 returned before RUN. The next change is the
  firmware-order fix that keeps SPI0 reset until just before its IRQ is
  enabled, not an IMU reset and not NMI.
- crashlog `PC=0x0102f252`, `DFAR=0x4c`, `r5=0`: the reset did not remove the
  retained state seen by the firmware IRQ handler.
- the log ends after `SPI0 reset begin` / `COLD checkpoint=spi0-reset`, with no
  reset result: AP access to `ps_spi0` or SPI0 hung/faulted. Recover by DFU and
  collect ramoops.
- boot/handshake succeeds but STARTEP, calibration, streaming or tilt fails:
  keep the whole capture; this is not a warm-strategy pass.

Do not add an automatic fallback. The entry remains explicit opt-in and one
attempt per boot.

## 5. What the SPI0 warm strategy still lacks

1. **Hardware proof.** The entry and checker are built and offline-tested,
   but no SPI0-reset reload result has been captured yet. [U]
2. **A general AOP reset primitive.** This is CPU halt/reload plus a targeted
   SPI0 controller reset. It is not a proven AOP isolation/reset or PMGR power
   cycle, and other retained peripheral state could still matter. [U]
3. **DMA-idle proof.** AppleA7IOPV2's idle hook is a stub. The experiment is
   constrained to the established buffers and quiesce record; it does not
   make arbitrary live reload safe. [E]
4. **`ps_spi0` accessibility.** A gated/inaccessible block may hang or fault;
   the persisted checkpoint exists specifically to classify that outcome.
   [U]
5. **Reassertion ordering.** Bit 6 may clear and then return before RUN. The
   driver detects and refuses this but does not yet implement the ordering
   alternative. [U]
6. **Runtime-state continuity.** A reboot loses RTKit endpoint state. Linux
   must repeat HELLO/EPMAP, buffer grants, accel calibration and rates; this
   is intentionally not transparent resume. [E]
7. **General IOMapper support.** XNU's `setActive` is not a missing wake or
   power operation, but an eventual driver using mapped DMA addresses must
   still provide the correct IOMMU/IOMapper setup. No AOP DART requirement is
   established for the present physical-buffer path. [I]/[U]

No experiment may write syscfg, NVRAM, or boot-firmware storage.

## 6. Reverse-engineering artifacts and annotations

The reproducible offline pass is:

```sh
GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec \
  ${HOME}/.local/share/uv/tools/ghidra-mcp/bin/python \
  scripts/periph/aop/re-resume-round2.py --annotate
scripts/ghidra-mcp.sh save
```

It verifies the pinned mailbox-object words and wrapper-wake capture, writes
`build/periph/aop-stage3/re/resume-round2/evidence.json`, and exports the
annotated private-firmware functions as `.c` and `.asm`.

Private firmware names/comments added or confirmed include:

- `rtk_power_transition` `0x01032c18`;
- `aop_cpu_wfi` `0x0102760c`;
- `aop_suspend_context_for_power_state` `0x01028250`;
- `aop_prepare_cache_for_power_state` `0x01028448`;
- `aop_interrupt_controller_command` `0x0102c524`;
- `aop_interrupt_dispatch` `0x0102c66c`;
- `aop_interrupt_set_enabled` `0x0102c72e`;
- `aop_interrupt_register_handler` `0x0102c7d8`;
- `aop_mailbox_command` `0x0102ca24`;
- `aop_mailbox_data_irq` `0x0102d080`;
- `rtk_mgmt_rx` `0x0103131c`, `rtk_iop_power_ack` `0x01031444`, and
  `rtk_power_decode` `0x010314f4`.

Shared kernelcache annotations correct the merged RTBuddy function boundary,
mark the status-2 versus status-3 decision at `...5ca0/...5cd0`, document the
type-7 mapping at `...71d2c`, identify `AppleSPU_setPowerState` at
`...b9e6d8`, and comment the AppleA7IOP start/stop sequences at
`...b4b738/...b4b8fc`.

