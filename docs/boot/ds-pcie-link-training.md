# PCIe link training on S8003 (j71t): what the registers mean, what iOS does, and the recovery ladder

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Research only. No device access, no kernel edits, no commits.

Sources: the shared Ghidra analysis of
`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12` (iOS 16.7.10, j71t),
the live ADT `build/firmware/private/IODeviceTree.txt`, the decoded tunables in
[../pcie/live-adt-decode.txt](../release/OMISSIONS.md), the round-8d/8e logs,
and the round-8e driver source (`/src/wt/tri33`).

**[V]** = read directly out of the named artifact/line/register capture.
**[I]** = inference, with the reason.

Kext text ranges used below (unslid, from
[../pcie/kernelcache-notes.md](../pcie/kernelcache-notes.md)):
AppleEmbeddedPCIE `0xfffffff005ecd000-0x5ee486c`,
AppleS800xPCIe `0xfffffff005ee5000-0x5eeca04`,
AppleS8003PCIe `0xfffffff0066e0000-0x66e17fc`,
IONVMeFamily `0xfffffff006778000-0x67bce20`.

---

## 0. Bottom line

1. **`last=0x4` is not an LTSSM state.** The `link-up` step polls `port+0x88`
   and prints the raw register, so `last=0x4` means "linksts = 0x4": bit 0
   (link up) clear, bit 2 set. The LTSSM lives in `port+0x804[14:9]` and the
   driver never printed it on the failing path. The two must be logged
   separately or the next run repeats this confusion. **[V]** driver +
   iOS decode function.

2. **The LTSSM encoding is the DesignWare one and it is in the kernelcache**,
   a 0x25-entry name table at `0xfffffff007a60538` used by
   `FUN_fffffff005edebc8` and printed by
   `FUN_fffffff005ee7de4` as
   `(ltssm 0x%x=%s)`. `0x04 = POLL_CONFIG`, `0x05 = PRE_DETECT_QUIET`,
   `0x11 = L0`. Full table in §1.2. **[V]**

3. **A failed attempt is not a slow attempt.** Every passing chain boot reported
   `last=0x5` (link up) 3.6–11.1 ms after `ltssm-start`
   ([round8e.md](../release/OMISSIONS.md)); the failing boots burn the whole 500 ms
   with `last=0x4`. So the RC is not "almost trained" — it never reports
   link-up at all. Raising the timeout is not the fix; it only extends the
   window in which the LTSSM can be sampled. **[V]** measured.

4. **The same boot *can* train after more cycles.** In
   `round8d-chain-boot8-fail/netconsole.txt` a later full port cycle of that
   boot did reach `link-up done last=0x5` and `link-L0 done last=0x702300`
   (LTSSM `0x11`, i.e. L0), then `full-bringup-done` / `link up (LTSSM 0x11)`.
   Two attempts failed first. The only procedural difference for the attempt
   that worked is that it followed the 8d **boot retry**: PERST# asserted,
   LTSSM stopped, all three RC controls verified clear, and a **100 ms
   quiescent hold** in that state before the bring-up. That is the single most
   useful measured fact for round 8f: the failure is recoverable in-boot, and
   what made the difference was time spent in a fully-off state, not a
   different register value. **[V]**

5. **iOS has no link-training retry ladder, no Gen1 fallback, and no runtime
   ANS reset.** Its port enable is one asynchronous attempt with a *5 s* outer
   budget (`500 × 10 ms`); a failure is logged
   `port did not enable within %u ms (state: %u)` and returned to the caller.
   Its only "speed" action is an **upshift** after link-up (root-port config
   `0x80c |= 0x20000`, poll ≤100 ms) and, if the negotiated speed still misses
   `maximum-link-speed`, a **panic** —
   `"failed to train link to desired speed: expected %d actual %d"`. iOS
   therefore never falls back, it fails loudly. **[V]** code; **[I]** that no
   other path retries.

6. **The only endpoint reset iOS knows is PERST# + port power-down.**
   `APCIEPortControl` is reached through `function-pcie_port_control` on `s3e`
   = `{phandle 0x6d /* apcie */, 'PrtC', 0x6e /* pci-bridge0 */}` in the live
   ADT, and it calls the port's `disableGated` — the same full port cycle our
   driver already runs — and refuses with
   `"endpoint reset not supported"` unless the bridge has
   `allow-endpoint-reset` (it does on j71t). There is no ANS reset register, no
   ANS PMGR domain, and no ANS-coprocessor halt path reachable from the AP side
   while the link is down (the ASC/IOP registers live behind the link).
   **[V]** code + ADT; **[I]** for the "behind the link" consequence.

7. **What our ladder is missing is therefore not another PERST pulse — it is
   the one level above it:** the `apcie` power/clock domain itself
   (PMGR device 56 + clock gates 57–62, already modelled in the DT as
   `ps_pcie`, `ps_pcie_aux`, `ps_pcie_ref`, `ps_pcie_link0..3`) and a PHY
   re-init from a cold register image. Everything the current full-cycle does
   survives in the registers/PHY; only the domain drop clears it. iOS reaches
   that state at every S2R, and its wake path re-runs exactly `configure` +
   `enableGated`, which is the sequence §5 formalises. **[V]** for the domains
   and the iOS sleep path; **[I]** that a domain drop is what un-wedges the
   PHY.

---

## 1. The registers we actually read

### 1.1 `port+0x88` — "linksts", not LTSSM

`0x601000000 + port*0x1000000 + 0x88`.

| bit | meaning | evidence |
|---|---|---|
| 0 | **link up** — the only bit the driver polls | iOS port slot 187 reads `port[0x88] & 1`; our `S8003_PORT_LINKSTS_UP`; every passing boot prints `0x5`, every failing one `0x4` **[V]** |
| 2 | constant `1` in *all* samples (up and down) — not an error flag | `0x4`/`0x5` captured in 8d and 8e; no code reads it **[V]** measured, **[I]** meaning unknown, do not classify on it |
| 6 | L2 reached (PME_Turn_Off handshake complete) | iOS port slot 216; our `S8003_PORT_LINKSTS_L2`; used only in the sleep path **[V]** |

Observed values: **`0x5` = link up**, **`0x4` = link down**. That is the entire
information content of the `last=` field in the current log line.

### 1.2 `port+0x804` — "linkcdmsts", the real LTSSM

`0x601000000 + port*0x1000000 + 0x804`.

* `[14:9]` = LTSSM state, 6 bits. iOS: `local_4c = read32(0x804) >> 9 & 0x3f`
  (`FUN_fffffff005ee7de4`) **[V]**.
* `[8]` observed `1` on a linked port **[V]** measured (`0x702300` at L0),
  meaning **[I]** (link/cdm active).
* `[22:20]` observed `7` on the Gen2 x2 link that trained
  (`0x702300`) **[V]** measured, **[I]** (rate/width field; not decoded).

State table, read from `0xfffffff007a60538`, decoded by
`FUN_fffffff005edebc8` (index ≥ 0x25 → `*UNKNOWN*`) **[V]**:

| val | name | what it means for us |
|---:|---|---|
| 0x00 | DETECT_QUIET | no receiver detected: **refclk / PERST# / endpoint power** |
| 0x01 | DETECT_ACT | idem |
| 0x02 | POLL_ACTIVE | receiver seen, TS1/TS2 flowing |
| 0x03 | POLL_COMPLIANCE | test mode — wrong (compliance entry stuck) |
| 0x04 | POLL_CONFIG | TS1/TS2 exchanged, **never reached config**: lane map, polarity, link number, or rate mismatch |
| 0x05 | PRE_DETECT_QUIET | re-arming: entered when the LTSSM leaves L0 or is enabled. A *stuck* 0x05 means detect never started |
| 0x06 | DETECT_WAIT | idem |
| 0x07–0x0c | CFG_LINKWD_START … CFG_IDLE | width/lane negotiation in progress |
| 0x0b | CFG_COMPLETE | one step from L0 |
| 0x0d–0x10 | RCVRY_LOCK … RCVRY_IDLE | trained but unstable: **signal integrity / rate change** |
| **0x11** | **L0** | good |
| 0x12–0x16 | L0S, L123_SEND_EIDLE, L1_IDLE, L2_IDLE, L2_WAKE | low power |
| 0x17–0x19 | DISABLED_ENTRY/IDLE/DISABLED | the RC has disabled the link — must re-enable from scratch |
| 0x1e/0x1f | HOT_RESET_ENTRY / HOT_RESET | AP is resetting |
| 0x20–0x23 | RCVRY_EQ0–3 | equalization (Gen3+; not used at Gen2) |

**Read this instead of guessing.** A single `0x804` sample at the moment
`0x88` bit 0 is still clear after 100 ms classifies the failure better than any
amount of retry:

* `0x00/0x01/0x05/0x06` → nothing on the other end: refclk, PERST#, or the
  endpoint is held in reset / unpowered. (Sample repeatedly: `0x05` is also the
  transient first state after enable, so one sample can be misleading — see
  §7 item 3.)
* `0x02/0x04` → the two ends see each other and talk, but never agree:
  lane map, polarity, link number, or a rate mismatch. This is the state in
  which a **Gen1 retry** is the correct next experiment, and in which
  re-running the PHY-IP lane init (not another power cycle) is the right move.
* `0x0d–0x10` → trains and falls over: signal integrity; Gen1.
* `0x17–0x19` → the port is disabled; `port+0x80` bit 0 must be re-armed.

### 1.3 What the current failure does and does not tell us

8d boot 8 and 8e tri33 all fail with `linksts=0x4` after 500 ms, on two and
three consecutive full power cycles, and 8d's third cycle then succeeds with
`0x5` → L0. The LTSSM value at failure was never captured. Everything below
assumes it is `0x00`–`0x06` (nothing detected) or `0x04` (never configured);
**§7 puts that measurement in the 8f log so the assumption stops being one.**

---

## 2. The port state machine both sides use

### 2.1 iOS's port power-up (`AppleS800xPCIePort::enablePortHardware` `0xfffffff005ee73dc`, behind the S8003 REQUIRE wrapper `0xfffffff0066e1634`)

| # | action | delay/limit |
|---|---|---|
| 1 | `rc[0x124 + p*0x80] &= ~1` (reset/refclk request low) | — |
| 2 | `rc[0x114 + p*0x80] \|= 1`; poll `rc[0x2c] & 0x10` | 10 µs steps |
| 3 | settle | 100 µs |
| 4 | `rc[0x100 + p*0x80] \|= 1` (port power/enable) | — |
| 5 | poll `rc[0x12c + p*0x80] & 1` (port ready) | busy, no delay |
| 6 | `rc[0x100 + p*0x80] &= ~0x100` | — |
| 7 | if `muxed-auxclk-auto-dis`: `rc[0x100] \|= 0x100000` | j71t: absent |
| 8 | `IODelay(t-refclk-to-perst)` | **100 µs** (`0x64` in the bridge) |
| 9 | `rc[0x124 + p*0x80] \|= 1` (reset release / refclk on) | — |
| 10 | `rc[0x4020 + p*0x40] = 3` (link-state counters) | — |
| 11 | `port[0x210]` W1C ("linkpmgrsts") | — |
| 12 | unless `no-refclk-gating`: `rc[0x114] &= ~0x100` — *allows* refclk gating after the link is up | — |
| 13 | `function-dart_force_active(1)`, `function-nvme_mmu_force_active(1)` | — |
| 14 | clamp + program LNKCTL2 TLS from `maximum-link-speed` (port 0 = 2) | — |
| 15 | `dbi-overrides`, `pcie-rc-tunables`, `apcie-config-tunables` | — |

then, back in `enableGated` `0xfffffff005ed954c`: IRQ mask `port[0x104]`,
`port[0x100] = 0xffd000` (ack all), `port[0x140] |= 0x80000000`, MSI
(`port[0x124]`, `port[0x128]`), and for state 1 (powering up):

| # | action | note |
|---|---|---|
| 16 | **PERST# deassert** — `function-perst` GPIO `0xa0` ← 0 | j71t has no `early-end-point-perst-ctrl`, so this is the normal path |
| 17 | if `no-phy-power-gating`: S8003 phy-ip lane init (slot 229, `0xfffffff0066e0700`) | j71t has it |
| 18 | **LTSSM enable**: `port[0x80] \|= 1` | — |
| 19 | arm `ltssm-timeout` timer (apcie `+0x164`, default **100 ms**) and wait | — |

**[V]** for the order, addresses and the two delay constants (they are also the
source of the driver's `S8003_T_REFCLK_TO_PERST_US` and
`S8003_PERST_TO_REFCLK_US`). The disable path is the mirror: PERST# assert,
root-port cfg `0x04 = 0`, cfg `0x18 &= 0xff0000ff`, dart/MMU force-inactive,
`rc[0x124] &= ~1`, **60 µs**, `rc[0x114] &= ~1`, `rc[0x100] &= ~1`
(`0xfffffff005ee72c8` + `0xfffffff005ed9c98`).

### 2.2 Round-8e's driver against that sequence

`apple_s8003_port_power_on()` / `apple_s8003_setup_port_full()` reproduce steps
1–19 in the same order, with two deliberate deviations **[V]** code:

* `rc[0x114]` gets bit 8 (`REFCLK_NOGATE`) as well as bit 0, and it is *never*
  cleared (iOS clears it at step 12 to allow gating). The safe direction.
* PERST# is held 100 ms with the LTSSM stopped (`perst-hold-100ms`) — iOS does
  not hold after power-on, it deasserts immediately after step 15. Extra hold is
  harmless; **do not shorten it** while chasing this failure, because it is the
  only knob currently known to make the third attempt in 8d succeed.

The `port-off` step reads back `rc[0x100] = 0x1000010` in every cycle. That is
not leakage: the live ADT's `apcie-common-tunables` contains
`offset 0x100 mask 0x31100010 value 0x01000010`, i.e. **bits 4 and 24 are set by
iBoot's tunables and must stay set**; only bit 0 is ours. Same for
`0x114 = 0x101` (bit 0 ours, bit 8 ours by choice).
**[V]** decoded tunables in `../pcie/live-adt-decode.txt` + the measured value.

One inherited-state check worth printing, because it is cheap and it settles the
pairing question every run: in the tri33 boot the **first** incoming read is
`RC124=0x0 RC114=0x100 RC100=0x1000010`, i.e. port 0 arrived fully parked (all
three bit-0 values clear, `0x114` bit 8 left on by the park) — and the port
still failed to train. The second attempt then inherits
`0x1/0x101/0x1000011`, which is the *first attempt's own* re-power, not a
pairing regression. **[V]** log values. Conclusion: in tri33 the park contract
held and the failure lives **above** the three RC bits, in the PHY or the
endpoint — which is what §5's escalation is built on. Print the outgoing
certificate next to the incoming first read anyway, so an 8f run can tell these
two cases apart without re-deriving it from the log.

---

### 2.3 iBoot's port init (`0x87001a1d8`, `build/power/oc-r3/iboot.asm:26743`)

Decrypted iBoot 20H350, load base `0x870000000` **[V]** line by line:

```
87001a28c  rc[0x124 + p*0x80] &= ~1                  ; reset low
87001a298  rc[0x114 + p*0x80] |= 1                   ; helper 0x87001acf8
87001a2a0  rc[0x100 + p*0x80] |= 1
87001a2b0  poll rc[0x12c + p*0x80] & 1               ; 10 us steps, no bound visible
87001a2cc  rc[0x124 + p*0x80] |= 1                   ; reset release  (BEFORE the pins)
87001a2f8  pin helper 0x870040ca8 (PERST)            ; function-perst
87001a318  pin helper 0x870040ca8 (CLKREQ)
87001a31c  delay(100)                                ; t-refclk-to-perst
87001a338  root-port cfg 0xa0 (16-bit): (x & 0xfff0) | 1
87001a348  w0 = 0x8700188a4(p)                       ; one-shot link-state read
87001a354  if (w0) program root-port cfg 0x76c and 0x670 from it
87001a390  rc[0x4020 + p*0x40] = 3
           on error -> 0x87001a720 (port down: 0x124, 0x114, 0x100 all cleared)
```

Two facts worth keeping:

* **iBoot never polls link-up.** It reads the link state once and, if it is
  non-zero, programs the root port's width/rate fields; otherwise it skips them
  and carries on. There is no retry loop and no timeout in this function.
  **[V]**
* **The order of `0x124` versus PERST# is not load-bearing.** iBoot releases
  `0x124` *before* driving the PERST/CLKREQ pins; xnu does it after. Both train.
  What matters is that PERST# is released before LTSSM enable, and that the
  100 µs `t-refclk-to-perst` is honoured somewhere in that window. **[V]** code
  on both sides, **[I]** the "not load-bearing" conclusion.
* **iBoot writes three root-port config registers that Linux never touches:**
  `0xa0` (16-bit, low nibble forced to 1), `0x670`, `0x76c` — the last two from
  the observed link state. They sit in the same vendor/port-logic config region
  as the `0x80c` dword xnu uses for its speed upshift (§3). Our driver programs
  only the standard `LNKCTL`/`LNKCTL2` and reads `LNKCAP2`. Whether these
  Apple-specific mirrors are required for a Gen2 link to *stay* at Gen2 is not
  known; if a Gen1→Gen2 upshift ever sticks at Gen1, this is the first place to
  look. **[V]** code; **[I]** the requirement.

So **no Apple component contains a link-training retry ladder** — not iBoot,
not xnu. §5 is necessarily a Linux-side construction.

---

## 3. What iOS does about a link that will not train, or drops

| question | answer | evidence |
|---|---|---|
| Does iOS retry training? | **No.** One asynchronous enable per `enableGated`; failure is `kIOReturnTimeout` and the caller (`FUN_fffffff005ee3844`) fails the enable. | `FUN_fffffff005ee38f0` logs `port did not enable within %u ms (state: %u)` after `500 × 10 ms`; no re-arm **[V]** |
| Training timeout value | `ltssm-timeout` (apcie `+0x164`), default **100 ms**; it arms a timer whose report is the power-state name "Port %u Link Up Timeout" | enableGated step 19; state-name table `0xfffffff007a60380` entry 6 **[V]** addresses, **[I]** that the timer only reports |
| Gen1 fallback? | **Never.** iOS *raises* speed and panics if it cannot: `FUN_fffffff005ee0658` reads the root port's config dword `0x80c`, ORs `0x20000`, writes it back, polls the negotiated rate for ~100 ms, then (when `+0x1a6 bit0` is set) panics `failed to train link to desired speed: expected %d actual %d`. | **[V]** code; bit 17's exact name is Apple/DWC-specific **[I]** |
| Link-width mismatch | panic `link width is %u, expected %u` (suppressible by a debug flag) | `FUN_fffffff005ee0324` (`handleLinkUp`) **[V]** |
| PHY reset / re-init on retrain | the phy-ip lane init (slot 229) runs on **every** port enable, right before LTSSM enable; there is no separate "PHY reset" register | **[V]** |
| Link-down detection | port interrupt status `port+0x100`; bits 4–7 are the group that is masked before link-up and unmasked after (`0xff002fff` vs `0xff002f0f`), and the handler acks `0xf0`. Handler = `FUN_fffffff005ed8860`, per-source sub-handlers at vtable `+0x650` (AER), `+0x658` ("AF/Link timeout"), `+0x690` (port error), `+0x6c8` (link change). | **[V]** masks/addresses; **[I]** that 4–7 is the link/AER group |
| What link-down *does* | it is reported as a **power state**: table entry 7 = "Port %u Unexpected Link Down"; a later power-on of that port re-runs `enableGated` from scratch (the "port on failed after link down" path is the DriverKit PCIe-error framework, not apcie). Nothing retrains from the interrupt. | state table `0xfffffff007a60380`; `FUN_fffffff005ed8860` **[V]** |
| Deep sleep / L2 | `port[0x8c] = 0x31` (PME_Turn_Off), poll `port[0x8c]` bit 0 (ack) 1 ms × 10, wait `port[0x88]` bit 6 (L2) 10 ms, *continue either way*; then the disable sequence above | **[V]** (driver comments + notes) |
| Endpoint reset | `APCIEPortControl` = `function-pcie_port_control` on `s3e` = `{0x6d /*apcie*/, 'PrtC', 0x6e /*pci-bridge0*/}`. Implemented as the port's `disableGated` (PERST# + full port power-down), and **only** if the bridge has `allow-endpoint-reset`; otherwise it panics `endpoint reset not supported`. | `FUN_fffffff005ee40e8` → slot `0x618` → `FUN_fffffff005ede748` → `FUN_fffffff005ede660` → `FUN_fffffff005ed9c98` **[V]** |
| ANS-specific reset (PMGR domain, coprocessor CPU) | **None reachable.** The S3E/ANS2 has no PMGR domain of its own in the ADT (`apcie` has 56/57–62; the endpoint node has no `power-gates`). `AppleASCANS2` (AppleA7IOP, `_ans2CpuMap`/`_ans2CpuRegs`) is the ANS coprocessor CPU loader, but its registers are inside the endpoint, i.e. behind the link that is down. | ADT `apcie`/`s3e`/`pci-bridge0` nodes; string inventory `AppleASCANS2`, `_ans2CpuRegs` **[V]** code; **[I]** the reachability argument |
| Does iOS power-cycle the ANS domain at runtime? | **No.** The apcie power/clock gates are dropped and re-acquired at **system sleep** (the IOKit `ApplePMGRFunctionPowerGate` cascade), not on a link failure; wake re-runs `configure` + `enableGated`. That is the only Apple precedent for a domain cycle. | [../power/ds-suspend-s2idle.md](../power/ds-suspend-s2idle.md) §1.2 + ApplePMGR notes **[V]** for the cascade, **[I]** for apcie's participation |

**Net effect for us:** Apple's answer to every link problem is "run the full
port enable again, and if that fails, fail". It has no deeper ladder to copy,
which is exactly why §5 has to be derived from the register semantics rather
than transcribed.

---

## 4. Why a warm kexec wedges and a power cycle fixes it

The state that survives has to be reachable through everything 8e's park does:
`CC.EN=0`/`RDY=0` at the endpoint, LTSSM stop, PERST# assert, `rc[0x124]`,
`[0x114]`, `[0x100]` cleared, 60 µs/100 ms waits, DART/NVMMU retired. What is
*not* touched by any of that:

1. the **PHY** (`0x600008000` common, `0x60a000000` phy-ip per lane) — it keeps
   its PLL/serializer state and its `phy[0xc]` ready bits, which read `0x7`
   (lanes 0–2) in **both** passing and failing attempts **[V]** measured;
2. the **apcie power/clock domain** (PMGR 56/57–62) — the driver holds it and
   marks it syscore, so it never drops across kexec **[V]** driver;
3. the **endpoint firmware** (ANS), which only a rail cycle resets
   ([ds-nvme-b](ds-nvme-b-ios-ans-restart.md) §6) **[V]** prior work.

`phy[0xc]` reading `0x7` in failing attempts is the one measurement that says
"the PHY thinks it is fine while the LTSSM cannot train". That points at the
PHY's *analog* state (PLL lock / rate) rather than the lane-ready handshake,
and the only software reset for it is the domain.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Nothing in the ADT or the kernelcache shows a PMGR parent shared between
`apcie` and `sgx` (§6), so B is the weakest of the three **[I]**.

---

## 5. The deep-recovery ladder the Linux driver should run before giving up

Design rules: every step is bounded, every step logs the registers that
classify it, and the ladder stops at the first success. The order is
"cheapest and least destructive first". Step 3 is what round 8e already does;
steps 1, 2, 4 and 5 are new, and 4–5 are where the unwedge has to live.

Preconditions for the whole ladder: no endpoint BAR access, no config access
beyond the root port while `rc[0x100]/[0x114]/[0x124]` bit 0 are clear; the
failure point is before enumeration, so nothing else holds the domain.

### Step 0 — capture (always, ~1 ms)

Before touching anything, one line per port:

```text
L0 pcielint=…(0x100) irqmask=…(0x104) linksts=…(0x88) ltssm=…(0x804)
   perst=<gpio> rc100=… rc114=… rc124=… rc12c=… rc2c=…
   phy_lanes=…(phy+0xc) phylnk=…(port+0x210) lnkctl=… lnkctl2=… lnkcap2=…
```

`ltssm` decoded with the §1.2 table. **This line is the whole point of the
round** — it converts every later failure into a state, not a number.

### Step 1 — retrain without a power cycle (10 ms scale)

1. `port[0x80] &= ~1` (LTSSM off), `port[0x80] |= 1` (on). Bound: 20 ms.
2. Root-port config `LNKCTL` bit 5 (`PCI_EXP_LNKCTL_RL`, "retrain link") = 1.
   It is RW1S: it reads back 0 once the retrain is accepted, so do not poll it
   for 1. This is the *only* retrain trigger the spec guarantees; iOS's `0x80c`
   write is not it.
3. Wait 100 ms for `0x88` bit 0. Classify the LTSSM.

If LTSSM was `0x17–0x19` (disabled), skip straight to Step 3 — an L0-off link
cannot be re-armed without a power cycle.

### Step 2 — Gen1 attempt (100 ms scale, the highest-information experiment)

Reprogram `LNKCTL2[3:0] = 1` **before** `ltssm-start`, and pass `speed = 1` into
`apple_s8003_phy_ip_init()` so the Gen2-only phy-ip write `lane+0x10188 = 0x100`
is skipped (`lane+0x20400` is written for Gen1 and Gen2 alike, so it stays).
Then run the normal sequence from `ltssm-start` onward. No PHY/power retune is
needed — the parameterisation already exists in the driver
(`apple_s8003_phy_ip_init(pcie, port, lane_mask, speed)`).

* Trains at Gen1 → **the PHY/electrical path is fine and Gen2 equalization or
  rate negotiation is the problem.** Log it, leave the port at Gen1, and let the
  boot continue (a working root at Gen1 beats a rescue at Gen2). On a later
  boot, iOS's upshift (`cfg 0x80c |= 0x20000`, §3) is the sanctioned way back to
  Gen2 — do **not** implement it before a Gen2 link has ever trained.
* Does not train at Gen1 either → the problem is upstream of rate: PERST#,
  refclk, or the endpoint. Continue to Step 3.

### Step 3 — full port power cycle (already implemented, 1–2 s)

Exactly 8e's sequence, unchanged: PERST# assert → `port[0x80] &= ~1` → poll
`port[0x80] == 0` (250 ms) → `rc[0x124] &= ~1` → 60 µs → `rc[0x114] &= ~1` →
`rc[0x100] &= ~1` → 10 ms settle → poll `rc[0x100] bit0 == 0` → nvme-mode
`rc[0x14]=1`, 100 µs, `rc[0x24]=1` → power-up steps 1–19 of §2.1 → LTSSM.

Two changes:

* **PERST# hold** between power-off and power-on: keep 100 ms, and additionally
  hold PERST# **asserted through the whole power-off window** (assert before
  `rc[0x124]`, not after the 10 ms settle). iOS's order asserts before too
  (disableGated starts with PERST).
* **Keep both repeats.** 8e already retries once; 8d only recovered on the
  third bring-up of the boot, and that one was preceded by the 100 ms
  off-hold. Three plain repeats is the best-evidenced budget
  (§0.4); the register cost is zero and each attempt is ~1–2 s.

### Step 4 — PHY cold re-init (new, ~50 ms)

Do this when the LTSSM never leaves detect/poll (Step 1 classification), and
always before Step 5, because Step 5 loses the register window.

> Private graphics-dependent excerpt omitted from this source snapshot.

Rationale: this is the only step that rewrites PHY state without dropping the
domain, and it is exactly what iOS re-runs after sleep. **[I]** (iOS does not
do it on a *failure*; there is no Apple failure path to copy.)

### Step 5 — apcie domain cycle (new, ~20 ms, the deepest software reset)

This is the level our ladder is missing. All seven domains are already in the
DT (`power-domains = <&ps_pcie>, <&ps_pcie_aux>, <&ps_pcie_ref>,
<&ps_pcie_link0..3>`) and in the driver's `pd_dev[]`, and
`apple_h9p_pcie_detach_genpd()` / `apple_h9p_pcie_attach_genpd()` already exist.

1. Only in the pre-enumeration path (port 0 failure, nothing enumerated, Wi-Fi
   down or not yet started — otherwise skip to Step 6).
2. `apple_h9p_pcie_detach_genpd(pcie)` — drops the seven device links and
   detaches the genpds, so the PMGR reference count can reach zero.
3. `clk_bulk_disable_unprepare()` on `core`/`aux`/`ref`.
4. Wait **1–10 ms**, then confirm from the PMGR domain's own state (the way
   the `apple-pmgr-pwrstate` driver reads it) that domain 56 actually gated.
   If it did not, do not pretend: see the note after step 6.
5. Re-attach genpds, re-enable clocks, and re-run iOS `configure`: `rc[0x4] =
   0x10 | lane_cfg`, `rc[0x114] |= 1`, poll `rc[0x2c] bit0`, `rc[0x38] = 1`,
   phy + common tunables.
6. Full port bring-up (Steps 3–4 without the loop).

If step 4 shows the domain never gated (PMGR keeps it on because something else
holds it), say so in the log and go to Step 6: that is the difference between
"we tried the deepest reset" and "we could not reach it", and the next run
depends on knowing which one happened.

Caveat to write into the code comment: dropping the domain also resets **port
1's** block and the NVMMU/DART register images, so this step must be followed by
a complete re-init of every port the driver will use in this boot, and it must
never run after enumeration (the PCI core's windows and the endpoint's BAR
mappings are invalidated).

### Step 6 — give up, loudly

Log the last §5-step-0 capture plus the ladder's history (which steps ran,
each step's `linksts`/`ltssm`/`perst` result), then hand over to rescue. State
explicitly in the log that a **power cycle (DFU) is now required** — that is
consistent with every observation so far and with
[ds-nvme-b](ds-nvme-b-ios-ans-restart.md) §6 (only a rail cycle resets the
ANS firmware).

### Things NOT to do

* Don't raise the 500 ms link timeout hoping to "catch a slow train": passing
  boots take 3.6–11.1 ms **[V]**. If you change it, change it to *sample the
  LTSSM periodically* (e.g. every 20 ms) rather than to wait longer.
* Don't clear `rc[0x114]` bit 8 or `rc[0x100]` bits 4/24 — they come from
  iBoot's tunables; the port-off readback `0x1000010` is expected.
* Don't implement iOS's `0x80c |= 0x20000` upshift before a Gen2 link has
  trained at least once; on a link that cannot train it is at best a no-op.
* Don't shorten the 100 ms PERST hold, and don't drop PERST between repeated
  attempts.
* Don't power-cycle the ANS's "PMGR domain": there is none. Step 5 cycles the
  **AP-side** apcie domain; the endpoint is only reset by PERST# or the rail.

---

> Private graphics section omitted from this source snapshot.

## 7. What round 8f should capture (in priority order)

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 8. Limits

* The failing LTSSM has never been sampled; §1.3 and §5 are written so the
  first 8f run replaces that gap with a number rather than with a theory.
* The `0x80c` bit 17 target is read through the same ECAM accessor pair the
  earlier study identified as config-space accessors
  (`0xfffffff005ede888/0xfffffff005ede80c`), so it is root-port config dword
  `0x80c`; the *meaning* of bit 17 on this root port is Apple/DWC-specific and
  is marked **[I]**. Do not build a recovery step on it.
* Apple's failure path is "log and fail", so the ladder in §5 is derived from
  register semantics plus the one measured in-boot recovery (8d attempt 3), not
  from a shipped Apple recovery routine.
* `AppleASCANS2`'s CPU registers (`_ans2CpuRegs`) were located by string only;
  whether they are reachable without the link was not proven, only argued from
  the endpoint's position behind `pci-bridge0` **[I]**.
* The PMGR `devices` table was decoded structurally (names + ids agree with
  every ADT `power-gates`/`clock-gates` list checked) but the per-record parent
  fields were not decoded; §6 therefore claims only "distinct devices, no
  ancestor relationship visible", not a full hierarchy.
* Nothing here was measured on hardware in this round.
