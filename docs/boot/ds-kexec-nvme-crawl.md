# Warm-boot NVMe "crawl": a lost shared MSI between the ANS admin and I/O CQs

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Status 2026-10-26: offline analysis, kernel fix and health gate are done.
No device access was used. The fix still needs a device run, which is
the decisive test of the mechanism (see "Orchestrator commands" below).

## Summary

Some boots start normally and then degrade within minutes: commands
that touch the disk hang, load passes 12, and SSH dies. The NVMe
completion interrupt stops for good. Every command after that completes
only when `nvme_timeout()` polls it (`io_timeout=5`, `admin_timeout=10`).

| Finding | Confidence |
|---|---|
| PCIe, ANS, MSI and IRQ setup is byte-for-byte the same in good and crawling boots. Kexec handoff state does not tell them apart. | **high** |
| Failure signature: one MSI shared by `nvme0q0` (admin) and `nvme0q1` (I/O); the IRQ count frozen for the rest of the boot; a posted but unconsumed CQE in **both** CQs at 253 of 257 timeouts. | **high** |
| Trigger: the vector dies 23–29 s after the NVMe controller comes up, in the same ~1 s window as the first stuck admin command. That command is always Identify (opcode `0x06`). Userspace admin traffic overlaps I/O at that moment. | **high** (as a correlation, 7/7) |
| Mechanism: ANS sends its one MSI only when the vector's aggregated state rises 0→1. nvme-pci services the two CQs in two separate shared-IRQ actions. An admin CQE that lands while the I/O action is still acknowledging its own CQ is never signalled. After that, the timeout path drains only one CQ at a time, so the vector never clears again. | **medium**: fits all the data, but no device test yet |
| Kexec is what causes this. | **not supported**: nothing kexec-specific differs, and the mechanism does not depend on kexec. Every logged crawl was a warm boot, but almost every boot is warm. The two clean cold boots (n=2) prove nothing. |

The fix is `a9/kexec-irqfix` (two commits on `a9/kexecfix`). Commit A
makes every interrupt service all CQs on the vector, and makes the
timeout path drain all of them. Commit B adds an optional, off-by-default
rescue poller. `scripts/boot/kexec-health-gate.py` now runs automatically
in `kexec-host.sh`, so no test runs on a crawling boot.

## Evidence

Reproduce it with `scripts/boot/analyze-nvme-crawl.py <dmesg>...`.

### Good and bad boots have identical setup

> Private graphics-dependent excerpt omitted from this source snapshot.

- Port-0 path: `policy=powered-reset-trial/full-fallback`, then
  `powered reset failed -110; falling back to full port cycle`, then
  `full-bringup-done`. Root-port tunables are `0,1,1`.
- MSI: `Apple ANS: MSI cap 0x50 flags 0x87 addr 0x0:bffff000 data 0x0`.
  Host side before CC.EN: `msi 0x31/0x0`, `irq 0x3000/0x2f0f`. There is no
  MSI-X and no per-vector mask (flags `0x87`: 8 vectors capable, 1 enabled).
- `nvme nvme0: 1/0/0 default/read/poll queues`, the health gate result 0,
  and `vectors 1`.
- `/proc/interrupts`: `69: … PCI-MSI-0000:01:00.0 0 Edge nvme0q0, nvme0q1`.
  It counts on CPU0 only, in good and bad boots alike.

Good boots end their runs at about 17,000 nvme interrupts. Crawling boots
freeze at 9,906–11,059. So the vector works through the whole handoff and
then dies: it is not mis-programmed at boot. A stale AIC mask, a
leftover ANS `INTMS` or a wrong MSI address would kill it from the first
interrupt.

### Crawl boots

Distinct boots with a saved log. `exp12-*`, `docs/periph/logs/aop-stage2f*`
and `round8e` netconsole are copies of these.

| Boot | NVMe up | 1st I/O submit | 1st admin submit (op) | IRQ count | both CQs pending |
|---|---|---|---|---|---|
| r35-02, 10-24 10:40Z | 17.6 s | 46.0–47.0 | 46.1–47.1 (0x06) | frozen 11059 | 28/29 |
| r43-06, 10-24 19:59Z | 24.2 | 49.4–50.4 | 49.4–50.4 (0x06) | frozen 10890 | 27/28 |
| r46-03, 10-24 22:51Z | 24.7 | 50.8–51.8 | 50.5–51.5 (0x06) | frozen 10940 | 10/11 |
| AOP stage2f, 10-25 12:04Z | 23.6 | 48.5–49.5 | 48.5–49.5 (0x06) | frozen 10706 | 27/28 |
| r52g-05, 10-25 23:16Z | 25.6 | 51.5–52.5 | 51.5–52.5 (0x06) | frozen 10236 | 53/53 |
| r53a-07, 10-26 03:50Z | 27.9 | 52.9–53.9 | 52.9–53.9 (0x06) | frozen 10193 | 46/47 |
| r63-04, 10-26 20:00 local | 38.7 | 60.4–61.4 | 60.4–61.4 (0x06) | frozen 9906 | 53/53 |
| AOP ch3tdq, 10-26 19:38 local | | first timeout 377.3 s | admin CQE pending (cid 0x8004) | 10207 | yes (only dump in the brief) |
| exp12-r15 10-22, aop-r4 10-23, round8e 14:56 | older kernels with no ANS dumps and default timeouts | | | | |

The 19:07 crawl on 2026-10-26 has no saved log.

Each submit window comes from the blk-mq timeout firing between
`timeout` and `timeout + 1 s` after submission. The windows are all the
same width, so in all seven boots the first stuck admin command and the
first stuck I/O were submitted within the same second.

Each "pending" judgement uses the dump printed before polling (`next
status & 1 == phase`). The pending entry carries the timed-out command's
own CID, so the completion was DMA'd into host memory and never consumed.
The four exceptions are the last timeout in their logs; the logs end before
any later observation.

After the first timeouts, the admin and I/O timeouts alternate every few
seconds. Each one polls only its own queue
(`nvme_poll_irqdisable(nvmeq)`), and the other queue has another posted
entry by then. Nothing ever acknowledges both CQs together.

**Correction to `ds-nvme-c-forensics.md` item 7.** That item says
`completion polled` appears nowhere in `build/` or `docs/`. It does now:
there are ten distinct boots with it since 2026-10-22. They are a different
failure from that note's DART/link hang: the link and controller stay
healthy (`CSTS 0x1`), and only the interrupt is lost.

## Mechanism (medium confidence)

NVMe MSI is not MSI-X. With single- or multi-message MSI, one interrupt
status covers every CQ on a vector, and a message is sent when it rises
0→1. The spec expects hosts to mask through `INTMS`/`INTMC` and to get a
new message when they unmask while entries remain. Linux never uses
`INTMS` for MSI, and Apple ANS has `NVME_QUIRK_SINGLE_VECTOR`.

The model that fits all the data: the controller re-checks only the CQ
whose doorbell was written, and the vector fires on the 0→1 rise of the
OR of all CQs. That gives three behaviours:

1. **A one-CQ race is harmless.** A CQE that lands between the handler's
   last phase check and its doorbell is re-signalled after the doorbell.
   Good boots show no isolated `completion polled` over about 17k
   interrupts each.
2. **A cross-CQ race is fatal (race A).** The shared IRQ runs q0's action
   and then q1's. An admin CQE that lands after q0's action has run, while
   q1's action is still completing its requests (tens of µs), leaves q0
   flagged. q1's doorbell does not re-check q0, and the OR stays 1, so no
   further message is ever sent.
3. **It sustains itself.** The I/O timeout drains q1, but q0 is still
   flagged. The admin timeout drains q0, but q1 is flagged again by then.
   A userspace admin poller (Identify, then Get Log Page, back to back in
   the logs) keeps q0 busy, so the crawl never ends.

The death lining up with the first userspace Identify, 23–29 s after
NVMe is ready, matches race A. The admin burst at that point in boot
explains a per-boot hit rate near 50%, even though each command's window
is only microseconds: boot I/O keeps the q1 action busy a few percent of
the time, and a burst holds 10–20 admin commands.

**What this cannot exclude:** a loss upstream in the h9p MSI path or the AIC
that happens to start during an admin burst. The fix tells the two apart
(next section).

Things checked and ruled out (none fits a vector that works for about
10k interrupts and then dies with completions posted):

- AIC state across kexec: AIC re-initializes, and the IRQ works after the
  handoff.
- ANS interrupt masking at shutdown: Linux never sets `INTMS`.
- The port-cycle path: identical in good boots.
- MSI or INTx confusion: MSI only, the same address and data every boot,
  and `irq: no irq domain found for pci@0,0` appears in good boots too.
- `dmesg -n 8`: the 20:00 crawl happened without it. Console load can at
  most lengthen handler time, which widens the race-A window.

## Fix: `a9/kexec-irqfix`

The branch is `a9/kexecfix` (`a412bc8f04`) plus two commits: `7dcb18c411`
(A) and `e3ad1b3a5c` (B). The validation branch `a9/kexec-irqfix-usb4` is
`a9/usb-host4` (`09e836312a`) plus the same two commits (`8f3730511e`,
`209a5a55e3`), packaged as entry `irqfix-usb4-209a5a55e3`. The patch files
are in `patches/kexec-irqfix/`. Only `drivers/nvme/host/pci.c` and
`drivers/nvme/host/pci-internal.h` change. Commit A's message says "13
logged crawl boots". That count includes duplicate log copies; the
correct figure is 10 distinct boots. Fix the wording when cherry-picking,
if convenient.

### Commit A: service every CQ on the shared MSI

This only applies to the ANS shared vector (ANS DMA ops,
`NVME_QUIRK_SINGLE_VECTOR`, no threaded IRQs):

- `nvme_irq()` loops over every enabled CQ on the vector until a full pass
  finds nothing. The last pass therefore runs after every doorbell write.
  It takes no locks: the actions of one IRQ never run concurrently, and
  every polling path disables the IRQ first, as upstream already relies
  on. Admin CQEs complete inline, and the I/O CQ keeps batching. Threaded
  mode keeps upstream behaviour, because the two threads could run in
  parallel.
- The "did we miss an interrupt" step in `nvme_timeout()` drains **all**
  CQs (one `disable_irq`, each queue's `cq_poll_lock` taken in turn, never
  nested). It logs `Apple ANS: timeout: drained every CQ on vector 0,
  completions on queues 0x…, irq 69 count N`. The first interrupt after a
  drain logs `Apple ANS: MSI resumed after vector drain (irq 69)`.

### Commit B: optional rescue

`nvme.apple_ans_irq_rescue_ms=N` (default 0, off). A delayed work drains
the vector when a CQE is still pending at the same CQ head and phase on
two ticks in a row. That turns a 5 s stall into N–2N ms. It is cancelled at
the start of `nvme_dev_disable()` and restarted when the controller is
live again. With the default of 0 the commit is inert.

### Residual window and trade-offs

The final pass narrows race A from "all of q1's completion work" to the
latency of the posted doorbell write, which is sub-µs. Closing it fully
would need an `INTMS`/`INTMC` mask and unmask, or a non-posted BAR read per
interrupt. The ANS code avoids hot-path BAR reads (a read over a dead link
stalls the CPU), and ANS `INTMS` behaviour is untested. If that window is
hit, commit A turns a permanent crawl into a single 5 s stall followed by
"MSI resumed". Commit B shrinks that stall further.

Alternatives considered:

- **Polled queues as a stopgap:** not available.
  `NVME_QUIRK_SHARED_TAGS` limits ANS to one I/O queue, and
  `nvme_setup_irqs()` reserves `nr_io_queues - 1 = 0` poll queues. Poll
  queues would also only serve `REQ_POLLED` I/O. Commit B is the practical
  form of polling.
- **Two MSI vectors (multi-message, MME=1):** h9p advertises
  `MSI_FLAG_MULTI_PCI_MSI`, but nvme-pci forces one vector for Apple
  controllers ("require all queues to use the first vector"). It is
  untested on ANS. This would be the structural fix if a device test ever
  shows that ANS handles two vectors.
- **`INTMS`/`INTMC`:** the spec's recommended pattern, but ANS behaviour
  is unknown and a wrong guess could mask the vector. It could be a later
  opt-in experiment.
- **AIC or h9p changes:** no evidence yet. These are the next step only if
  the device run refutes the model.

### Lineage and merges

`pci.c` and `pci-internal.h` are byte-identical on `tri63`, `aop-wake5`,
`usb-host4` and `tri53-sep6`. On all four, `git cherry-pick
a412bc8f04..a9/kexec-irqfix` applies cleanly, and so does a merge. The
old `a9/sep` (before ANS) does not, and does not need it.

### Validation (offline only)

- ARM64 W=1 builds of `pci.o` and `pci-apple-ans.o` are clean. A full
  `usb-host4` build plus the fix is packaged as `build/kexec-irqfix/`
  (`scripts/boot/build-kexec-irqfix.sh`, `package-kexec-irqfix.py`): same
  config, DTB, initramfs and cmdline as `usb-host4-09e836312a`. Only the
  kernel and the entry name change.
- `checkpatch --strict` is clean apart from the required `Co-Authored-By`
  trailer.
- New production-C test `tests/ans-io-shared-vector.c`, run from
  `test-ans-io-offline.py`. It runs the real `nvme_poll_cq`, `nvme_irq`,
  drain and rescue code against a model controller that only signals on
  0→1. Legacy per-queue dispatch loses the vector to races A and B. The
  fix leaves nothing pending, the timeout drain restarts the vector and
  reports "MSI resumed", and the gates hold (non-ANS, threaded, disabled
  queue).
- On `kexec-irqfix`: `test-ans-io-offline` 9/9,
  `test-ans-handoff-offline` 23/23, `test-kexecfix-offline` and
  `test-ans-keep-power-offline` pass. `kexecfix` still passes too (the new
  test skips there).

## Health gate

`scripts/boot/kexec-health-gate.py` runs after `kexec-host.sh` sees the
new boot ID, and before `SET_DEFAULT`. Each probe is one bounded SSH
round trip. It records `boot_id` and uptime, times `sudo -n true`, does a
16 KiB `O_DIRECT` read from `/dev/nvme0n1` (a real NVMe command), compares
the `nvme0q` IRQ count before and after, and greps this boot's dmesg
(skipping `[omarchy-rd]` replays) for `completion polled`, `Apple ANS:
timeout`, `drained every CQ` (this also covers rescue drains),
`reset controller`, `disable controller` and `Disabling device`. The
rescue's bring-up line `IRQ rescue every N ms` is not a failure.

A probe fails when SSH hangs past 30 s (one retry for link blips), sudo
takes over 5 s, the read takes over 3 s or fails, the IRQ count does not
advance across the read, a bad dmesg line appears, or the boot ID changes.
The gate passes after uptime ≥ 120 s **and** ≥ 60 s of clean probes. That
covers every boot-time death above (≤ 62 s uptime).

On failure, `kexec-host.sh` prints `UNHEALTHY BOOT …`, exits 3 and leaves
the default unchanged. `HEALTH_GATE=0` skips the gate, and
`HEALTH_GATE_ARGS` passes extra options. `--quick` (two probes 2 s apart)
is meant to run before each test step: the 19:38 crawl began at 6 min of
uptime. Offline tests: `scripts/boot/test-kexec-health-gate.py` (16 cases,
including a fake SSH that hangs or blips) and `scripts/boot/test-host.py`
(6 cases, including gate failure → exit 3 with the default untouched).

## Orchestrator commands

Everything below is event-driven: run each step when its trigger occurs.

**0. Before the next `kexec-host.sh`, run the gate once by hand on the
current, good boot.** This checks what could not be checked offline:
passwordless `sudo -n` for `dd` and `dmesg`, the `/dev/nvme0n1` name, and
`timeout` and `$RANDOM` on the device. If any of these is wrong, every
gated kexec would exit 3 for a reason that has nothing to do with a crawl.

```sh
python3 -I scripts/boot/kexec-health-gate.py --quick
```

The `HEALTH FAIL:` line names the cause. `HEALTH_GATE=0
scripts/boot/kexec-host.sh …` is the escape hatch until it is fixed. No
script wraps `kexec-host.sh` (it is only quoted in `report-r53.py`,
`run-compute-triangle.sh` and the device updater's comments). Manual runs
now take about 2 minutes longer, and exit 3 means "booted but unhealthy",
not "DFU needed".

**1. Identify the admin poller (current kernels, on a good boot).** This
also tells whether the 7/7 same-second coincidence is meaningful: it is
only strong if admin commands are sparse.

```sh
scripts/omarchy/ssh.sh 'echo "qid == 0" | sudo tee /sys/kernel/tracing/events/nvme/nvme_setup_cmd/filter >/dev/null;
  echo 1 | sudo tee /sys/kernel/tracing/events/nvme/nvme_setup_cmd/enable >/dev/null;
  sleep 90; sudo cat /sys/kernel/tracing/trace | grep -v "^#" | head -100;
  echo 0 | sudo tee /sys/kernel/tracing/events/nvme/nvme_setup_cmd/enable >/dev/null'
```

This needs tracefs and `CONFIG_TRACING`. If it is unavailable, use
`ps -eo pid,comm,args` plus `systemctl list-units --state=running` and
look for udisks2, smartd or a sensors poller. The comm column names the
process: udisks2, smartd, a sensors or status-bar loop, and so on. Masking it (`sudo systemctl mask --now …`) is a
mitigation that needs no kernel change, and it is also an experiment: if
the model holds, crawls stop.

**2. Zero-kernel confirmation, if a crawl happens on a current kernel.**
Do this from an SSH session that is already open. Stop the poller found in
step 1 and watch the count:

```sh
sudo kill -STOP <pid>; for i in $(seq 8); do grep nvme0q /proc/interrupts; sleep 5; done
```

If the count starts moving again within about 15 s (one admin timeout,
then one I/O timeout, both CQs drained), that supports the model.

**3. Device test of the fix.** The gate runs automatically.

```sh
ENTRY="$(cat build/kexec-irqfix/entry-name)" \
  scripts/boot/kexec-host.sh "$(cat build/kexec-irqfix/payload-path)"
scripts/omarchy/ssh.sh 'sudo dmesg | grep -v omarchy-rd | grep -E "Apple ANS: (timeout|.*drained every CQ|MSI resumed)|completion polled"; grep nvme0q /proc/interrupts'
```

> Private graphics-dependent excerpt omitted from this source snapshot.

- **No timeout lines over 10+ warm boots:** fixed. At today's ~50% rate,
  chance alone would explain this with p ≈ 0.001.
- **`drained every CQ …` followed by `MSI resumed`:** the model is
  confirmed, and the residual window was hit and recovered after one
  stall. Optionally retest with the rescue variant below.
- **`drained every CQ …` repeating with no `MSI resumed`, and a frozen
  count:** the model is refuted, and the loss is upstream of the
  controller. Capture the endpoint view (`CSTS`, `INTMS`, MSI capability
  and queue state go to kmsg) with
  `echo endpoint | sudo tee /sys/class/nvme/nvme0/apple_ans_diag; sudo dmesg | tail -40; grep nvme0q /proc/interrupts`,
  then move the investigation to the h9p MSI status and AIC mask/pending
  state for that hwirq.

**4. Rescue variant (optional).** Same payload, with
`nvme.apple_ans_irq_rescue_ms=200`:

```sh
ENTRY="$(cat build/kexec-irqfix/entry-name-rescue)" \
  scripts/boot/kexec-host.sh "$(cat build/kexec-irqfix/payload-path)" "" \
  "$(cat build/kexec-irqfix/cmdline-rescue)"
```

**5. Adopting it in other lineages:** `git cherry-pick
a412bc8f04..a9/kexec-irqfix`, done by whichever agent owns that branch.
