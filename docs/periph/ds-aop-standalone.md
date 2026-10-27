# AOP without iOS: does a power-off stop it, and what starts it cold

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-25 correction: **`+0xb88` is not verified CPU status.** It mirrors
inbox `+0x4008` in every saved boot today, including FIFO occupancy. Earlier
running/halted conclusions from its bit0 are withdrawn; execution remains
unknown. The current forced-cold path accepts RUN `+0x44` bit4 clear after
10 ms, with auto-boot/outbox disabled and fault-free mailboxes, under the
owner's accepted crash risk. See [aop-force-cold.md](aop-force-cold.md).
Historical captures and earlier path descriptions below retain their provenance.


Current forced-stop/cold-load implementation: [aop-force-cold.md](aop-force-cold.md).
It is a separate owner-authorized `aop_force_cold=1` opt-in; the historical
attach restrictions below still describe the other paths. Hardware untested.

2026-10-24. Offline only: **no device access, no kernel edits, no commits**. This
does not redo [`ds-aop-takeover.md`](ds-aop-takeover.md),
[`ds-aop-recovery.md`](ds-aop-recovery.md),
[`ds-orientation-paths.md`](ds-orientation-paths.md),
[`ds-aop-running-image.md`](ds-aop-running-image.md) or the driver design in
`scripts/periph/aop/kernel/apple-a9-aop.rst`. It answers one question: can Linux
own the AOP on a boot where iOS never ran.

Tags: **[V]** pinned in the kernelcache, ADT, firmware or a saved capture;
**[I]** inference that fits the evidence but is not measured; **[U]** unresolved.

Sources: shared Ghidra project (iOS 16.7.10 / 20H350 j71t kernelcache,
SHA-256 `c6b95fcf…1e0a`), the extracted kexts in `build/periph/aop-r1/kexts/`
(local Ghidra runs this round in `build/periph/aop-r10/`), the live ADT/IOService
planes in `build/firmware/private/`, `docs/adt/j71t.txt`, and the Linux tree in
the `linux-a9-src` volume.

---

## 0. Answer in one page

| Question | Answer | Tag |
| --- | --- | --- |
| Does a full power-off *stop* the AOP in software? | **No.** Nothing in the platform halt path reaches the AOP: `IOService::systemWillShutdown` only acks the PMRD worker, and `IOPlatformExpert::haltRestart` tail-calls `_PE_halt_restart`, which the PMU kext chains. The only code that halts the core is `AppleA7IOP::stop`, reached from `RTBuddy::powerStateTransition` cases 0/2 — a driver power-state transition, not a platform halt. | [V] code, [I] that the transition never fires at power-off |
| Does a full power-off *reset* the AOP in hardware? | **Unknown; this is the one thing to measure.** The AON island is on a PMU rail that PMGR cannot gate (`aop*` are flagged always-on, gate arrays are empty). Whether the D2365 drops that rail at S5 is not in any artifact we have. | [V] flags, [U] rail |
| Is there a cheap state after which Linux can take the cold path today? | **Yes, if the core comes back stopped.** The driver's stopped branch already loads the pinned image, patches the tag table, stages the ext segment, programs remap + IRQ masks + outbox, and releases the core (`+0x44` bit 4, `auto-boot=1`). Its hard preconditions are five AON domains ON, an empty outbox and no mailbox fault. | [V] driver, `a9_aop_cold_start()` |
| Can a *running* survivor be reset in software? | **No safe path exists yet.** The AP can ask the IOP to change power state (management type 6, payloads `0x201`/`0x220`/`0x202`/`0x000`), and `AppleA7IOP::stop` exists, but its idle check is a stub, so there is no DMA-drain proof. A software reset is a new, separate opt-in at best. | [V] code, [I] effect |
| What is the cheapest path to an AOP that talks to Linux with no iOS? | One controlled power-off, then measure. If `+0xb88` bit 0 reads 0, the existing cold loader runs unchanged. See §5. | [I] |

---

## 1. Q1 — does a full power-off stop the AOP?

### 1.1 The only code that halts the core

`AppleA7IOP::stop` (`fffffff005b4b8fc`, vtable `0xfffffff006d3e158 + 0x550`; the
V2 table at `0xfffffff006d3ff98` inherits it) [V]:

```text
if (!ignore-mailboxes) { setOutboxEnable(0); setInterruptsEnabled(0); }
if (!cpu-ctrl-filtered) { if (!idle()) retry 10x 1 ms, else log "failed to become idle"; }
                    /* V2 idle check at +0x6d8 is `return 1`, so this never runs */
setCPURun(0)        /* +0x44 = (+0x44 & ~0x10);  *(0x210000600) = 0 */
vt[+0x6a8]()        /* b4e7c4: RET on V2 */
vt[+0x6c8](../release/OMISSIONS.md)       /* b4b6f4: IOMapper setActive(0) — not a PMGR op */
vt[+0x6d0]()        /* b4b698: provider clock/power gate index 0 — rejects: arrays empty */
```

So a clean stop leaves: outbox **disabled**, AP interrupts off, `+0x44` bit 4 = 0,
`auto-boot = 0`, and **the PMGR domains still on** (the gate calls are rejected on
this board because `clock-gates`/`power-gates` are empty) [V].

That is the exact state Linux wants to start from: halted core, powered island,
`+0xb88` bit 0 = 0.

### 1.2 The iOS power-off path never calls it

| Step | Code | What it does to the AOP | Tag |
| --- | --- | --- | --- |
| any service is told about shutdown | `IOService::systemWillShutdown` `fffffff007764310` | calls `FUN_fffffff0077fc6b4`, which clears a flag, wakes the PMRD worker and calls one vtable method (`+0x1e8`) on the acking service; no `terminate`, no power-state change | [V] |
| platform halt/restart | `IOPlatformExpert::haltRestart` `fffffff00778df9c` | tail-calls `_PE_halt_restart` (type 3 → 0) | [V] |
| who owns `_PE_halt_restart` | `AppleARMPMU` kext | saves and replaces the previous routine ("PMU KEXT expected to handle restart but no saved PE_halt_restart routine found", `fffffff0059362f7`) → the last write is a PMU register sequence | [V] strings, [I] sequence |
| RTBuddy's own shutdown hooks | `no-shutdown` (`+0x11a`), `shutdown-sleep` (`+0x11b`), parsed in `RTBuddy_parseBootProperties` `fffffff005b68764` | j71t sets **neither** (live nub has only `watchdog-enable`, `coredump-enable`, `region-*`, `aop-target`), so the default shutdown behaviour, whatever it is, is active | [V] |

The `no-shutdown` property is the interesting one: its existence proves there *is*
a default shutdown action that a nub can suppress. The code that reads it
(`fffffff005b77b78`, `fffffff005b78100`) sits in an RTBuddy buffer-flush routine
(sends a flush completion, signals a workloop timer), not in a "halt the IOP"
routine [V] — but this is a **partial** trace. A RTBuddy/AppleSPU override of
`systemWillShutdown` that stops the IOP was not excluded. **[U]**

### 1.3 What that means, and the one empirical clue

* If the power-off path sends the AOP nothing and the PMU keeps the AON rail up,
  the survivor stays running across a power-off exactly as it stays running
  across DFU and kexec. **[I]**
* If the PMU drops the rail, the island loses state, and the next boot finds a
  core that is not running — but also PMGR domains whose `ACTUAL` may be 0, which
  today makes the driver refuse (`refused-domain-state`). **[I]**

The survivor capture argues for the first reading. `build/periph/aop-r4/live/
active-20261023T160345Z/` is a *true cold* boot (DFU → checkm8 → pongoOS → m1n1 →
Linux) that found the AOP running with `cpu_status=0x00020001`, and the standard
A9 checkm8 entry starts from a powered-off device. If the operator's DFU entry did
begin at "slide to power off", then a full power-off demonstrably does **not**
stop the AOP. That is memory of a procedure, not a measurement. **[I]**

### 1.4 The read-only discriminator

Four words decide everything, and three of them are in the observer's existing
map. Read **PMGR first** (always powered, `0x210200000 + 0x80000`), then the
wrapper only if the domain says on.

| `aop` PS TARGET/ACTUAL | `+0xb88` bit 0 | `+0x4020` bit 0 | Reading | Next |
| --- | --- | --- | --- | --- |
| 0 / 0 | *do not read* | *do not read* | island unpowered; the AOP cannot be running | cold path, but §5 step 4 |
| f / f | 0 | 0 | halted, outbox off = exactly an `AppleA7IOP::stop` result | cold path **today** |
| f / f | 0 | 1 | core halted without the vendor stop (watchdog/reset); outbox left enabled | cold path **today** (the loader ORs the enable bit) |
| f / f | 1 | 1 | survivor still running (= today's captures) | stop needed (§3) or accept no takeover |
| any | — | faults 18/19 | mailbox fault latched; the only record of a bus error | stop, never clear |

`0x00020001` is therefore not one value but at least two facts: bit 0 = CPU
running (`AppleA7IOPV2_isCPURunning`, `fffffff005b4e4b8` reads `+0xb88` and
returns bit 0), and bit 17, which tracks the mailbox "empty" bit of `+0x4020` in
every capture so far but whose meaning *inside the status register* is not
pinned. A halted-but-powered AOP should read `0x00020000`; a dead rail reads
`0x00000000` or faults the bus. [V] bit 0, [I] bit 17.

### 1.5 Device test (read-only, one boot)

1. From iOS, power off normally; wait 30 s. Do not connect USB until step 2.
2. Enter DFU by the usual button sequence, then palera1n → pongoOS → m1n1 → Linux.
   **No iOS boot in between.**
3. With `apple_a9_aop.debug=1` only (no `active`, no `attach`, no `ring_scan`),
   read `/sys/kernel/debug/apple-a9-aop/status`: it prints `cpu_control=…`,
   `cpu_status=…`, `inbox=…`, `outbox=…`, `idle=…`, `auto_boot=…` and the five
   genpd/raw domain states.
4. Classify with §1.4. Nothing is written; the observer's only MMIO is `readl`.

Risk: low, with one caveat — if the island is unpowered, a wrapper read may fault
the AP bus. The observer already skips wrapper reads unless all five domains are
ON, so the PMGR state is read first and the wrapper only after it. Keep it that
way; do not add a raw `devmem` poke "to check".

---

## 2. Q2 — the cold start, as iOS performs it, and the diff

2026-10-25 cold2b audit: [aop-cold-hello.md](aop-cold-hello.md) checks this
sequence against the pinned V2 instructions and firmware ext mapper. No
remap/RTSZ/tag correction was established. The current forced experiment
adds frozen SRAM/FIFO evidence, synchronous HELLO observation for 20 s and
one retained-state wake after 1 s without HELLO. iOS sends that wake only for
old status 3; Linux's equivalence and the hardware fix remain unverified.
The comparisons below describe the original loader, before that experiment.

### 2.1 Where the image goes

The region is a boot-time carveout, not a driver allocation. The live
`carveout-memory-map` node (`build/firmware/private/IODeviceTree.txt:318–345`)
declares [V]:

| Region id | Base | Size | Use |
| --- | --- | --- | --- |
| **31** | `0x210e00000` | `0x9b000` | AOP firmware target (`region-base`/`region-size`) |
| **29** | `0x210e9b000` | `0x5000` | the remainder of the `0xa0000` aperture |

That closes the `0x9b000` vs `0xa0000` question from
[`ds-aop-running-image.md`](ds-aop-running-image.md) §3: iBoot tiles the aperture
into a firmware target plus five pages. Neither range is reserved in the Linux
boot DT (`grep 10e00000` over the j71t DTS: no hit) [V] — a gap, see §2.4.

### 2.2 What iOS programs, exactly

From `AppleA7IOP::start` (`fffffff005b4b738`, decompiled this round) and
`AppleA7IOPV2_mapFirmware` (`fffffff005b4e72c`) [V], wrapper base `0x210800000`:

```text
# mapFirmware, in this order (single-range descriptor check first)
+0x18 = 0                      # IOP address base, low
+0x20 = 0                      # IOP address base, high
+0x28 = len & 0xfffff000        # 0x9b000
+0x30 = 0
+0x08 = phys_lo                 # 0x210e00000
+0x10 = phys_hi
+0x38 = 1                       # enable

# start
+0x4000 = 0x1111               # interrupt set/mask, primary
+0xc00  = 0x1111               # same, second block
+0x4004 = 0x0001 ; +0xc04 = 0x0001     # unmask bit 0
+0x4004 = 0x1000 ; +0xc04 = 0x1000     # unmask bit 12
+0x4020 = (+0x4020 & ~1) | 1     # outbox enable
+0x44   = (+0x44 & ~0x10) | 0x10 # CPU run
*(0x210000600) = 1              # auto-boot
```

Two corrections to the older notes, both mechanical [V]:

* `0x1111` is written to **two** blocks (`+0x4000` and `+0xc00`), and the clears
  go to `+0x4004`/`+0xc04`; the driver already does exactly this.
* `0x06000000000220` is not a special "boot-complete request": it is entry 1 of
  the power-request table at `fffffff0053110a0`, i.e. "IOP, go to state 1", sent
  when the old status was 3. The table is
  `{0: 0x…0201, 1: 0x…0220, 2: 0x…0202, 3: 0x…0000}` and it is the AP→IOP
  `type 6` power vocabulary (§3). The firmware answers with `type 7`.

`start` on a cold IOP also calls `prepare`, `setActive(1)` on the IOMapper, the
two interrupt sources, and (only with `force-timebase-sync`) `syncTimebase`. On
j71t the mapper has no AOP DART and the clock/power gate arrays are empty, so
those calls are no-ops [V empty arrays, I no DART].

### 2.3 The handshake

Unchanged from [`ds-aop-takeover.md`](ds-aop-takeover.md) §2.3/§3.5: device HELLO
`0x0010|max<<16|min` → AP `0x00200000000c000c` → EPMAP `0x80` chunks → AP replies,
AP power `0xa0`, STARTEP `0x50|ep<<32|2`, then AFK v8 (`0x80`/`0xa0`,
`0x89`→`0xa1`, `0x8a/8b/8c`, `0xa3`, `0x85`, ready `0xc0`), then calibration and
the 10 Hz reports. iOS additionally waits for IOP state 8 (or `0x8000`) with a
20 s default timeout before it considers the IOP booted.

### 2.4 Diff against the a9/aop driver

`a9_aop_cold_start()` in `scripts/periph/aop/kernel/apple-a9-aop-active.c` is a
faithful port of the sequence above [V both sides]:

| Step | iOS | driver | Verdict |
| --- | --- | --- | --- |
| image copy | raw main at region+0, aligned u32, no BSS clear | identical (`a9_aop_copy_verify`) | match |
| tag table | AOPt/EC0p/WrAd/McRA/nCal/SOC_/SOCR/STKG/PRNG/RTTO (+NONC/ECID when a chip id exists) | same set, NONC/ECID left at defaults | match for j71t (Linux has no ECID) |
| ext segment | page-aligned DRAM buffer, PA in `EC0p` | `dma_alloc_coherent`, 32-bit PA checked | match |
| remap | `+0x18,+0x20,+0x28,+0x30,+0x08,+0x10,+0x38` | same order, same values | match |
| IRQ masks | `0x1111` to `+0x4000`/`+0xc00`, clear 1 then `0x1000` | same | match |
| outbox | `+0x4020 |= 1` | same | match |
| release | `+0x44 |= 0x10`, then `auto-boot = 1` | same | match |
| provider power-on | clock/power gate index 0 | not called | no-op on j71t (arrays empty) |
| IOMapper setActive | called | not called | no-op [I] |
| idle before halt | V2 stub returns success | n/a | no drain proof either way |

**What is actually missing** is not the loader:

1. **Domain state.** `apple_pmgr_observe()` requires all five AON domains ON and
   the driver never forces one on [V]. Note that the Linux PMGR driver already
   powers on any `apple,always-on` domain that reads inactive at probe
   (`pmgr-pwrstate.c`: "always-on domain %s is not on at boot" → power on), so
   `ps_aop` may come up by itself; `busif`, `cpu` and `filter` are declared as
   `ps_aop`'s *parents* with no flag and no other consumer, so they may stay off.
   **[V]** for the code, **[U]** for what happens after a rail cycle.
2. **Reservation.** `0x210e00000/0xa0000` is not in the boot DT. The driver
   `devm_ioremap_resource()`s it (busy in `iomem_resource`), but the linear map
   and the page allocator still own those pages. Add a `no-map` carveout through
   the existing FDT merge (`scripts/boot/device/linux-a9-fdt-merge`) before the
   first cold load, or the loader writes into memory Linux may hand out. [V]
3. **iBoot's region post-processing.** iBoot reads eight u32 from region+0..0x1f
   and applies `(word & 0xff0000) | 4` to `0x200220700 + i*0x40000` and
   `0x200220708 + i*0x40000` (`i=0..3`) on every normal boot
   ([`ds-aop-running-image.md`](ds-aop-running-image.md) §3) [V]. Purpose [U];
   the Linux chain never runs iBoot, so this is simply absent.
4. **`aop/reg[3]` = `0x210000508`, 8 bytes.** No AP code we inspected writes it
   and the driver ignores it [V]; it stays [U].
5. **Consistency gate after release.** iOS waits for IOP state 8; the driver waits
   for HELLO then EPMAP with its own bounds. Adequate, but a firmware that boots
   and then stalls will look like "no HELLO" — log both cases distinctly.

---

## 3. Q3 — a safe software reset of a running AOP

### 3.1 What iOS has

* **No reset register.** The `aop` node has no `resets` property, its
  `clock-gates`/`power-gates` are empty, and `ps_aop` is `apple,always-on`, so no
  supported PMGR off/on sequence exists and the DT describes no reset line for
  the core. [V]
* **A power-request vocabulary.** Management `type 6` with payload
  `0x201` (state 0), `0x220` (state 1), `0x202` (state 2), `0x000` (state 3),
  built from `fffffff0053110a0` and sent through `rtbuddy->vt[+0xe8]`; the IOP
  answers `type 7` (low byte, `0x20` = boot complete). This is the only
  AP-initiated "please change power state" that exists. [V]
* **Stop, then start, in one driver.** `powerStateTransition` case 0/2 sends the
  request for the target state, waits (up to 20 s) for the IOP to report it,
  disables interrupts, unmaps the firmware segments, then calls `stop`; case 1
  reloads, patches and starts. So iOS *can* stop and restart the same core. [V]
* **No crash-recovery restart of the AOP.** The coredump family
  (`RTBuddyCoredumpMap`, `…Decoder`, `RTBuddyCrashlogEndpoint`) decodes what a
  crashed IOP left behind; no AP-side "kill and re-launch" path was found. [V]
  found-nothing → [I] that none exists.
* **Hibernation is not a restart.** It restores AP-side objects and jumps to state
  4 without a load; it is the one path that attaches to a live IOP and it needs an
  image we do not have. [V]

### 3.2 What a software reset would have to look like

```text
1  send type 6 / 0x201 once (ask for state 0)
2  wait <= 2 s for a type 7 word whose low byte is 2 (or 0)
3  if it arrives: +0x4000 = 0x1111, +0xc00 = 0x1111   (mask both blocks)
                 +0x4020 &= ~1                          (outbox off)
                 +0x44   &= ~0x10                       (CPU run off)
                 *(0x210000600) = 0                     (auto-boot off)
4  verify +0xb88 bit 0 == 0 and +0x4020 bit 0 == 0. If not: stop, no retry.
5  re-run the stopped path (section 2)
```

### 3.3 Why it is not safe yet

* **No drain proof.** `stop`'s idle check on V2 is `return 1`
  (`fffffff005b4c460`), so even iOS's own stop does not establish that the AOP
  finished its DMA before the CPU is halted. [V]
* **Unproven firmware reaction.** Whether AOP firmware honours `type 6 0x201` is
  [U]; a firmware that ignores it leaves a running core with a stopped AP, and a
  second release attempt is unmodelled.
* **Forbidden by the current contract.** Steps 3–4 write `+0x44`, `auto-boot` and
  the mask registers — exactly what the running-attach path forbids. A reset must
  therefore be a new immutable opt-in, exclusive with `attach`, with one attempt
  per boot. [V design]

If a stop is not achievable, the honest statement is: **there is no software way
to reset a running AOP on this board today**, and the only resets are a rail cycle
or a future hardware/firmware path.

---

## 4. Q4 — what makes the always-on domain dangerous

Hard "don't" list, with the reason:

1. **Do not gate or force `ps_aop`, `ps_aop_busif`, `ps_aop_cpu`, `ps_aop_filter`.**
   `ps_aop` carries `apple,always-on` in the Linux DTS; the other three are its
   declared parents, have no flag and no other consumer. All four share the ADT
   PMGR descriptor flag (`35`) with the DRAM controllers (`DCS0`–`DCS3`), `SIO`,
   `MCC`, `SMX` and `SF` — the always-on infrastructure class. PMGR also exposes
   hardware state counters named `AOP_IDLE`, `AOP_CPU_IDLE`, `AOP_BUSIF_IDLE`,
   `AOP_FILTER_IDLE`, `AOP_FILTER_POWER_ON`, `AOP_CPU_POWER_ON`, `AOP_CLK_ON`,
   `LPO_CLK_ON`, `S2R_AOP_STATE`, `AOP_DDR_STATE`, `AWAKE_STATE`
   (`fffffff0055f83c0`; reported by `AppleS8000PerformanceController`,
   `fffffff0064313bc`) — the AON controller tracks AOP idle/clock and an
   S2R-specific AOP/DDR state. Gating any of it risks the suspend path and the
   memory controller state. [V] names and flags, [I] consequence.
2. **Do not gate `ps_aop_gpio`.** 28 pins, 7 interrupt groups to AIC 113–119,
   `role="AOP"`, `no-resume-restore`, and Linux pinctrl consumes the domain. The
   IMU chip-select and interrupt are almost certainly here, and so may be wake
   lines for other blocks. [V] node, [I] IMU lines.
3. **Do not clear mailbox fault bits 18/19** in `+0x4008`/`+0x4020`. They are the
   only record of an AOP bus error; the driver already refuses on them. [V]
4. **Do not write the firmware region while the core may be running.** Whether the
   AOP executes in place (B2) or from on-chip memory (B1) is still open
   ([`ds-aop-running-image.md`](ds-aop-running-image.md) §4.4, §11 A1), and the
   region may be firmware scratch. [V] problem statement.
5. **Do not touch the ext buffer unless it is reserved.** It was allocated by the
   dead AP; Linux owns those pages now. [V]
6. **Do not steal the AIC lines.** Four lines (130–133) belong to the AOP;
   `AOP.OutboxNotEmpty` is an AP wake source, and the driver uses 133 (rx) and 130
   (tx). Masking them in the AIC removes AP wake and the mailbox path. [V]
7. **Do not confuse the two GPIO blocks.** AOP GPIO is `0x2100f0000/0x4000` (28
   pins); the AP GPIO controller is `0x20f100000/0x100000` (208 pins). Only the
   former belongs to the AOP. [V]
8. **SEP is a different IOP.** `sep@DA00000` (AP `0x20da00000`) is `iop,s8000`
   too, with its own clock gate (`clock-gates = <125>`) and its own
   `aot-power = 1`. Sharing the A7IOP/RTBuddy *driver* does not make it part of
   the AOP island; do not extend AOP assumptions (or writes) to it. [V]
9. **The AOP does not talk to the PMU.** Its firmware device table has `spi0`,
   `aop-gpio`, systick, PMGR and unnamed blocks — no I2C controller. PMU traffic
   is the AP's `i2c0`/D2365. So a PMU-only reset cannot reach the AOP, and the AOP
   cannot restore PMU state. [V] table, [I] conclusion.
10. **The RTC is the PMU, not the AOP.** `AppleD2365PMURTC` is an I2C device;
    `AppleARMPE` reads time of day through it. Any AON/rail cycle loses wall
    clock state that Linux must re-set from NTP/RTC-over-PMU. [V]
11. **`0x210000600` (`auto-boot`) is live shared state.** Writing 1 is release,
    writing 0 is halt; it is not a scratch register. [V]

---

## 5. Ranked plan

> Private graphics-dependent excerpt omitted from this source snapshot.

Order matters: 1 is cheap and decides between 2 and 3, and 0 may make 1
unnecessary. Steps 2–4 all end in the same acceptance test, which is the one that
matters: **Linux boots, no iOS has ever run, and the AOP's HELLO arrives at a
Linux driver as the first and only AP.**

---

## Appendix A — addresses this round

| Address / offset | What |
| --- | --- |
| `fffffff005b4b738` | `AppleA7IOP::start` (vtable +0x548); busy → `0xe00002d5` |
| `fffffff005b4b8fc` | `AppleA7IOP::stop` (vtable +0x550) |
| `fffffff005b4bb4c` | vtable +0x580: interrupt-source enable/disable |
| `fffffff005b4e530` | vtable +0x660: `+0x4000 = +0xc00 = 0x1111` |
| `fffffff005b4e548` | vtable +0x698: `+0x4004`/`+0xc04 = 1` |
| `fffffff005b4e5a8` | vtable +0x670: `+0x4004`/`+0xc04 = 0x1000` |
| `fffffff005b4e5d8` | vtable +0x678: outbox enable, `+0x4020` bit 0 |
| `fffffff005b4e72c` | vtable +0x6a0: remap, seven writes (§2.2) |
| `fffffff005b4e7c8` | vtable +0x6b0: `+0x44` bit 4, then `0x210000600` |
| `fffffff005b4c460` | vtable +0x6d8: idle check, returns 1 on V2 |
| `fffffff005b4b698` | vtable +0x6d0: provider clock/power gate (rejects: empty arrays) |
| `fffffff005b4b630` | vtable +0x5b0: `providerPowerOnForLoad` |
| `fffffff005b65b54` | `RTBuddy::powerStateTransition` (0/2 = stop, 1 = load+start) |
| `fffffff005b68764` | `RTBuddy::parseBootProperties` (`no-shutdown` +0x11a, `shutdown-sleep` +0x11b) |
| `fffffff0053110a0` | AP→IOP power request table: `0x201 / 0x220 / 0x202 / 0x000` |
| `fffffff005311d24` | coredump decode strings (`RTBuddyCoredumpMap`, decoders) |
| `fffffff007764310` | `IOService::systemWillShutdown` (ack only) |
| `fffffff00778df9c` | `IOPlatformExpert::haltRestart` → `_PE_halt_restart` |
| `fffffff0059362f7` | PMU kext: "expected to handle restart" |
| `fffffff0055f83c0` | PMGR counter names (`AOP_IDLE` … `S2R_AOP_STATE`, `AOP_DDR_STATE`) |
| `fffffff0064313bc` | `AppleS8000PerformanceController` reporter init (consumes them) |
| live DT `carveout-memory-map` | region 31 = `0x210e00000/0x9b000`, region 29 = `0x210e9b000/0x5000` |

## Appendix B — reproduction

```sh
# the stop/start routines (function definitions are missing from the shared project)
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleA7IOP.macho \
  --output build/periph/aop-r10/startstop \
  --function 0xfffffff005b4b738 --function 0xfffffff005b4b8fc

# the IRQ/outbox/remap helpers
~/.local/share/uv/tools/ghidra-mcp/bin/python scripts/periph/ghidra-local.py \
  --binary build/periph/aop-r1/kexts/AppleA7IOP.macho \
  --output build/periph/aop-r10/irq \
  --function 0xfffffff005b4e530 --function 0xfffffff005b4e548 \
  --function 0xfffffff005b4e5a8 --function 0xfffffff005b4e5d8

# shared project: the power transition and the property parser
scripts/ghidra.py decompile_function_by_address address=0xfffffff005b65b54
scripts/ghidra.py decompile_function_by_address address=0xfffffff005b68764

# the carveout map
grep -n 'region-id-31\|region-id-29' build/firmware/private/IODeviceTree.txt
```
