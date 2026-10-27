# iOS takes ANS down and back up: what a cold-equivalent kexec restart must copy

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Research only. No device access, no kernel edits, no commits. Every claim is
tagged **VERIFIED** (decompiled, address given) or **INFERRED**. Addresses are
from the shared Ghidra project (`kernelcache.release.iPad6,11_12`, iOS 16.7.10,
`build/kc/20H350__iPad6,11/`, sha256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`), read with
`scripts/ghidra.py`. Where a fact was already established in
[kernelcache-notes.md](../pcie/kernelcache-notes.md) (Q3 port sequence, Q4
NVMMU) it is cited, not re-derived.

This document does not repeat the round 1–7 history
([ans-kexec.md](ans-kexec.md)) or the sleep-order study
([ds-idle-sleep.md](../power/ds-idle-sleep.md)); it answers one question: *in
what order does iOS take ANS all the way down and back up, and where does our
kexec path differ?*

---

## 1. Answer in brief

1. iOS has **no kexec**. The only handoff it ever performs with a live ANS is
   **iBoot → xnu**: the loader has just read the kernel from ANS, the PCIe link
   is up, ANS is enabled with the loader's queues, and the kernel then does a
   plain **CC=0 → RDY=0 → re-init** without touching PERST, refclk, port power
   or PMGR. The port driver has an explicit "already enabled" path — the string
   `"apcie[%u:%s]::%s trying to enable the port that is already enabled
   (_state = %d)"` at `fffffff00548c4a6` — VERIFIED (string and call site);
   that it returns before touching the link is INFERRED from the message and
   from the fact that the kernel is loaded over that same link.
2. The ANS teardown iOS performs on **system shutdown/restart** is *only*
   "send SHN, wait SHST" (`IONVMeController::performNVMeShutdownTasks`,
   `fffffff00679eac4`). It does **not** clear `CC.EN`, does **not** assert
   PERST, does **not** power the port down. VERIFIED.
3. A full port teardown (PME_Turn_Off → L2 → PERST → refclk off → port power
   off, plus `dart_force_active(0)`/`nvmmu_force_active(0)`) exists, but iOS
   runs it on **sleep**, and in the **firmware-activate** path — not at
   shutdown. VERIFIED.
4. Every time the port is powered back up, iOS re-runs the **DART** hardware
   init and **re-programs the whole NVMMU** (scratch → TCB → INIT → SART)
   because `force_active` transitions 0→1. `AppleNVMeMMU::setActive` does
   *nothing* on the 1→0 edge — it only records the flag. VERIFIED.
5. iOS never reloads ANS firmware on a normal boot. The firmware lives in the
   S3E and survives PERST. A host firmware download happens only when the
   endpoint's `0x1b18` reads `0xbfbfbfbf` ("boot from host", BFH) — VERIFIED
   (`fffffff0067ad84c`, `fffffff0067ad8e4`).
6. The one thing iOS does that we do **not** is re-program NVMMU/DART *after*
   the port power cycle and *before* the new controller enable, and it never
   leaves the endpoint in the half state "powered, PERST asserted, LTSSM
   stopped" that our outgoing kexec creates.
7. The round 7 failure signature (invalidate FIFO `0xa01`, used bitmap
   `0x4000`, DART err `0x500`) is consistent with a **tag whose invalidate was
   never retired** while the endpoint was reset under it. iOS's rule is the
   opposite: queues and tags are retired *before* the controller is stopped,
   and the FIFO is checked empty *before* every invalidate, with a 20 ms fatal
   bound. VERIFIED (iOS behaviour), INFERRED (that this is our failure).

---

## 2. The A9 ANS stack

| Role | iOS object | Where | Notes |
|---|---|---|---|
| PCIe RC + ports | `AppleEmbeddedPCIE`, `AppleS800xPCIePort`, `AppleS8003PCIe` | kext at `fffffff005ed…`/`fffffff005ee…` | port blocks `0x601000000`+`p*0x4000`, NVMMU `0x601004000` |
| NVMe/ANS endpoint driver | `AppleEmbeddedNVMeController` (IONVMeFamily) | vtable `fffffff006f6dd70`, code `fffffff00677…–0x67b…` | matches `IOPCIMatch 0x2002106b` |
| NVMMU ("nvme-mmu0") | `AppleNVMeMMU` | `fffffff005ee8bf0` (start) | registers in `0x601004000` |
| DART | `AppleS5L8960XDART` | `fffffff005fb21f0` (start), hw init `fffffff005fb5b58` (vtable `+0x6c0`) | `0x601…` window `0x80000000+0x3c000000` |
| Port control stub | `APCIEPortControl` | metaclass ctor `fffffff005ee3e34`, vtable `fffffff006e1ba80` | served as `function-pcie_port_control: PrtC` |

**ANS on A9 has no AP-visible coprocessor/RTKit.** `AppleANS2NVMeController`
(which owns an RTBuddy service, AXI2AF windows, SPI-NAND pins,
`PowerStateAction` with "ANS2 will turn OFF now",
`fffffff0057ef092`) requires an `/arm-io/ans` node
(`fffffff0057ee9fb`) and `fRTBuddyService` (`fffffff0057edb86`). The j71t ADT
has no `/arm-io/ans` and the live ioreg instantiates
`AppleEmbeddedNVMeController`, so **all ANS2 firmware/RTKit/power-state code is
inapplicable here** — VERIFIED (ADT + ioreg + class probe strings). The A9 S3E
firmware is reached only through NVMe registers and vendor "tunnel" admin
commands (`CORE_DEBUG_*`).

### ANS BAR registers used by the driver (offsets into the 4 MiB BAR)

| off | dir | meaning | evidence |
|---|---|---|---|
| `0x1800` | W | init mode: `0` regular, `3` "restore boot / restricted init" | `fffffff006780098` (selector), write via vtable `+0x880` = `fffffff0067ad6ac` |
| `0x1808` | R | required scratch (HMB) size | `fffffff00677faf4` |
| `0x180c` | R | required scratch alignment (power of two) | same |
| `0x1810/0x1814` | W | scratch (HMB) DMA address, 64-bit | `fffffff00677f9b4` |
| `0x1818` | W | scratch size | same |
| `0x1824` | R | core mask (`!= 0xffffffff`) | `fffffff00677faf4` |
| `0x1828` | R | log buffer size (`(v<<12)+0x1000`) | same |
| `0x1b00` | W64 | BFH firmware image DMA address | `fffffff0067ad8e4` |
| `0x1b08` | W | BFH image size (must be 16-byte multiple) | same |
| `0x1b10` | W | BFH command: `1` start, `2` complete/ack | same |
| `0x1b14` | R | BFH status, polled | same |
| `0x1b18` | R | BFH marker, `0xbfbfbfbf` = "host, boot me" | `fffffff0067ad84c` |

---

## 3. iOS teardown, in the three flavours that exist

### 3.1 System shutdown / restart — the only "we are leaving" path

`IONVMeController::performNVMeShutdownTasks()` `fffffff00679eac4`:

```text
if (this->fPoweredOn /*+0x1b8*/ != 0) {
        status = ShutdownNVMe(force = 0, flags = 0)      // wrapper fffffff00679eb8c
        if (status) log "ShutdownNVMe() completed with status = 0x%08x"
}
workloop_stop(this+0x128)                                // fffffff0067af178
```

`ShutdownNVMe` (`fffffff00679eb8c`) first tears the queues down
(`fffffff0067a2654` / `…7a26b4` for the normal path,
`fffffff0067a2804` / `…7a2864` for the `flags&1` path — both delete CQ/SQ by
index and clear the per-queue "enabled" bit at `cq->+0x28`), then calls the
virtual `+0x610`, which lands in the SHN sequence below. VERIFIED (decompiled).

The SHN sequence itself, `fffffff0067a242c(this, self_refresh)`:

```text
cq[0].enabled = 0;  cq[1].enabled = 0            // clear software queue state
cc = read32(0x14)                                 // CC
if (cc == 0xffffffff) return 0xe00002c9
if (!self_refresh) {
        cc = (cc & 0xffff3fff) | 0x4000           // SHN = 01 (normal shutdown)
        timeout = *(u64 *)(this + 0x19c)
} else {
        cc = cc | 0xc000                          // SHN = 11 (Apple "self-refresh")
        timeout = *(u64 *)(this + 0x34)
}
write32(0x14, cc)
PollCSTS(mask = 0xc, expect = 2, timeout)         // CSTS.SHST == 10b
on success: this->fShutdownDone /*+0x1c*/ = 1
on failure: panic-class assert
```

`PollCSTS` `fffffff0067a18cc` reads **CSTS `0x1c`**, keeps the last value at
`this+0x1e4`, and on timeout dumps then optionally panics with
`0xc5750000|status`. VERIFIED.

A second, harsher variant exists at `fffffff0067a25c4`: `CC =
(CC & 0xffff3fff) | 0x8000` (SHN = 10b) with no wait at all. It is reached from
a failure/abort path, not from shutdown. VERIFIED (code), INFERRED (purpose).

What is **absent** from this path, and matters to us:

- no `CC.EN` clear (the controller is left enabled, SHN-complete);
- no PERST, no refclk/power change, no PMGR access, no DART/NVMMU touch;
- no HMB de-registration.

### 3.2 Sleep (`setPowerState` → `ThreadEntry`)

`AppleEmbeddedNVMeController::ThreadEntry` `fffffff00677afe0`, proposed state 0:

```text
wait for the in-flight quiesce flag (+0x410/+0x411, 1 ms poll loop)   // fffffff00677b2ac
cancel housekeeping event (this+0x3e0 vtable +0x138)
ShutdownNVMe(force = 1, flags = 0)                                  // fffffff00679eb8c
workloop/serialise stop                                              // fffffff0067af178
```

and on wake (state increases):

```text
workloop start, this+0x3f8 = 0x64646464 ('dddd' marker)
virtual +0x600  == AppleEmbeddedNVMeController::InitializeNVMe(bool)   fffffff00677ac20
   base InitializeNVMe, then boot-arg overrides for
   nvme-clockgating-timeoutus / nvme-powergating-timeoutus,
   then slot +0x9f8
unfreeze queues (this+0x18c, mask 0xffff7bff on +0x188)
```

VERIFIED (decompiled). Note the shape: **sleep = SHN only**, and the port is
taken down *by the port driver*, separately, later in the IOKit cascade
(§3.3), not by the NVMe driver.

### 3.3 Port power-down — `disableGated` → `disablePortHardware`

Already established in [kernelcache-notes.md Q3](../pcie/kernelcache-notes.md#q3-port-bring-up-sequence-s8003);
re-checked here only for order relative to ANS:

```text
root port cfg 0x04 = 0;  cfg 0x18 &= 0xff0000ff          // no routing
PERST# asserted (function-perst, GPIO 0xa0 = 1)
(no PMGR access anywhere in this path)
AppleS800xPCIePort::disablePortHardware  fffffff005ee72c8:
    dart_force_active(0);  nvmmu_force_active(0)
    rc[0x124 + p*0x80] &= ~1                             // port enable
    IODelay(perst-to-refclk-delay-us = 60)
    rc[0x114 + p*0x80] &= ~1                             // refclk gate off
    rc[0x100 + p*0x80] &= ~1                             // port power off
```

The sleep-flavoured wrapper `apple_s8003_port_sleep` in our tree mirrors the
timeout-tolerant version (PME_Turn_Off `port+0x8c = 0x31`, ack, L2, PERST,
60 µs, off) — see [ans-kexec.md](../boot/ans-kexec.md) round 3.

**Key point for us:** `dart_force_active(0)`/`nvmmu_force_active(0)` are what
arm the *next* port power-up to do a full DART + NVMMU re-init. Skipping them
(i.e. never powering the port down at all) means the "active" flag never
transitions, and the re-init never happens. VERIFIED (see §4.2).

### 3.4 The only place iOS does a full port cycle around ANS

`AppleNVMeUpdateUC::FirmwareActivate` `fffffff00678f930` (entitled
`com.apple.nvmefwupdate.allow` client) — this is iOS's own "restart ANS from
scratch" recipe:

```text
1  send FW-activate vendor command (FUN_fffffff0067b1628)
2  ShutdownNVMe(force = 0)                      // SHN + SHST
3  disablePCIPort()   virtual +0x940            // save PCI config, drop link,
                                                //   assert endpoint reset
4  enablePCIPort()    virtual +0x938            // release reset, restore config
5  this+0x417 = 3                               // "regular init" selector
6  InitializeNVMe()   virtual +0x600
   on failure: disablePCIPort(); enablePCIPort();
               this+0x417 = 2;  InitializeNVMe();   // "restricted init"
               this+0x417 = 3;
```

`this+0x417` is consumed by `PreCCEnableHook` `fffffff006780098`:

```text
if (this+0x412) {                    // limited/restore boot flag
        if ((u8)this+0x417 - 1 < 2)  // 1 or 2
                mode = 3; "Restore boot. Restricted init."
        else mode = 0; "Regular init."
} else mode = 0;
write32(0x1800, mode);               // via vtable +0x880 = fffffff0067ad6ac
SetupScratchBuffer(this);            // write64(0x1810), write32(0x1818)
```

VERIFIED. This is the closest thing in iOS to "make ANS cold again without a
power cycle", and it is exactly four things we can copy: SHN, port disable,
port enable, controller init.

---

## 4. iOS bring-up

### 4.1 Port

`enableGated` `fffffff005ed954c` → `enablePortHardware`
`fffffff005ee73dc`, full register list in
[kernelcache-notes.md Q3b](../pcie/kernelcache-notes.md#3b-appleembeddedpcieportenablegated--fffffff005ed954c-in-order).
The parts that matter for ANS:

- if the port is **already enabled**, it takes the "already enabled" path
  (message at `fffffff00548c4a6`); INFERRED that this returns before touching
  PERST/force_active/refclk, since the kernel was just loaded over that link;
- otherwise `rc[0x114+p*0x80] |= 1` (refclk), poll `rc[0x2c] & 0x10`,
  `IODelay(100)`, `rc[0x100+p*0x80] |= 1` (port power), poll `rc[0x12c+p*0x80]
  & 1`, `rc[0x100+p*0x80] &= ~0x100`, `IODelay(t-refclk-to-perst = 100 µs)`,
  then release PERST, then phy-ip lane init, then `port[0x80] |= 1` (LTSSM),
  then wait link-up (`ltssm-timeout`, default 100 ms).

### 4.2 DART and NVMMU are re-initialised *by the port*

In the same function, step 13:

```text
function-dart_force_active(port+0x140)  (1)
function-nvme_mmu_force_active(port+0xb88) (1)
```

`AppleNVMeMMU::setActive` `fffffff005eebf3c` only acts on the 0→1 edge:

```text
scratch page   : write64(0x10, scratch)              fffffff005eea084
TCB base       : write32(0x08, lo); write32(0x0c, hi); INIT   fffffff005eea0f0
INIT           : write32(0x04, 0x10000); while (read32(0x04) & 0x10000);   fffffff005eec028
SART           : write32(0x24, va_start); write32(0x28, va_end);
                 write32(0x2c, pa>>20); write32(0x20, enable)               fffffff005ee9fc0
```

On the 1→0 edge nothing is written — `setActive(false)` just stores the flag.
VERIFIED. So the *only* way iOS gets a fresh NVMMU is the port's
power-cycle → `force_active` 0→1 pair.

DART: `AppleS5L8960XDART` re-runs its hardware init whenever it becomes active
(TCR `+0x0c`, fetch-config `+0x30`, diag-config `+0x20`, error-reflector
`+0x24`, per-stream TTBR enable), per
[enable-freeze.md](../pcie/enable-freeze.md#dart-ios-programs-registers-linux-didnt-verified).

### 4.3 NVMe controller

`IONVMeController::start` `fffffff00679f03c` → `+0x688`
`fffffff006779034`:

```text
if (AppleNVMeController::IsBootFromHost())        // read32(0x1b18)==0xbfbfbfbf
        AppleEmbeddedNVMeController::InitializeController (+0x540 = fffffff0067794e8)
        // BFH: firmware download / restore environment ("-restore")
else
        IONVMeController::InitializeController  (fffffff0067ac008)
```

Then `EnableController` `fffffff0067a16bc` — the exact sequence we already
match (see [enable-freeze.md](../pcie/enable-freeze.md#nvme-enable-path-same-registers-as-linux-verified)):

```text
1  write32(0x14, 0)                                    // CC = 0, EN = 0
2  PollCSTS(mask = 1, expect = 0)                       // RDY = 0
3  virtual +0x6b8: AQA = 0x24, ASQ = 0x28, ACQ = 0x30
4  virtual +0x608 = PreCCEnableHook  fffffff006780098
       write32(0x1800, 0 | 3);  write64(0x1810, hmb); write32(0x1818, size)
5  IOSQES = +0x6c0, IOCQES = +0x6e8 (both 4)
6  write32(0x14, IOSQES<<16 | IOCQES<<20 | 1)           // CC.EN = 1
7  PollCSTS(mask = 1, expect = 1)                       // RDY = 1
8  sw queue enabled bits set
```

### 4.4 Delays iOS actually uses (all VERIFIED)

| step | delay |
|---|---|
| after refclk enable, before PERST release | `t-refclk-to-perst` = 100 µs (ADT) |
| PERST assert → refclk off (teardown) | `perst-to-refclk-delay-us` = 60 µs (ADT) |
| after port power enable | `IODelay(100)` µs, then poll `rc[0x12c]` |
| link train | `ltssm-timeout` 100 ms default, 1 ms poll |
| SHN → SHST | `*(u64*)(this+0x19c)`; ANS2 path uses 500 ms |
| ANS2 pre-power-off settle | `IOSleep(1500)` ms before touching CC |
| BFH download status | `10 ms + 10 ms × (size>>14)` |
| NVMMU INIT busy | poll `0x04 & 0x10000` with no measured delay |
| NVMMU invalidate FIFO (port0 `0x22c` bit0) | 20 ms, **fatal** in iOS |

---

## 5. What iOS re-initialises, and what it never touches

| Item | On every port power-up | On a plain kernel handoff (iBoot → xnu) | Comment |
|---|---|---|---|
| ANS firmware | **not reloaded** unless `0x1b18 == 0xbfbfbfbf` | not reloaded | firmware lives in the S3E |
| ANS `0x1800` mode | regular (0) | regular (0) | 3 only for restore/after FW update |
| ANS HMB (scratch) | rewritten (0x1810/0x1818) | rewritten | same DMA buffer each boot |
| NVMMU TCB array | **not zeroed**; only TCB base + INIT | same | INIT is assumed to clear internal caches |
| NVMMU SART | fully rewritten | rewritten | va_start/end, pa>>20, enable |
| NVMMU invalidate FIFO | checked empty before each invalidate | same | 20 ms fatal |
| DART | full hw init (TCR/fetch/diag/reflector + streams) | re-applied at attach | per-stream TLB invalidate only |
| PMGR PCIe domains | **not touched** — only clock gates at configure | not touched | `PCIE`, `PCIE_AUX`, `PCIE_REF`, `PCIE_LINKn` stay on |
| PCIe port itself | full teardown + power cycle | **skipped** (already enabled) | the crucial difference |

---

## 6. BFH, and what "cold" really means for the S3E

`AppleNVMeController::IsBootFromHost` `fffffff0067ad84c` is literally
`read32(0x1b18) == 0xbfbfbfbf`. `DownloadFirmwareFromHost`
`fffffff0067ad8e4` then:

```text
require size % 16 == 0
require BFH still set
allocate page-aligned buffer, memcpy image
write64(0x1b00, phys);  write32(0x1b08, size);  write32(0x1b10, 1)
poll 0x1b14, timeout = 10 ms + 10 ms per 16 KiB
write32(0x1b10, 2)          // ack/complete
```

Two readings, both worth keeping:

- **INFERRED (likely):** BFH is the S3E ROM's "no valid firmware, load me over
  PCIe" handshake. It is what iBoot/LLB uses after a *real* power-on, and what
  iOS's restore/update path uses. A PERST with the rail still up does **not**
  re-enter BFH on this unit — no boot that reached "Apple ANS: enabling
  controller" can have read the magic, because `nvme_pci_apple_ans_preinit`
  refuses that case with "waits for a boot-from-host firmware upload" and the
  probe would have stopped there. VERIFIED by construction from our code and
  logs, INFERRED as a statement about the hardware.
- **Therefore:** "S3E firmware reset" is not reachable from the AP by register
  writes at all. The reachable levers are PERST, refclk, port power, and the
  NVMe controller reset. If ANS firmware state itself is corrupt, only a PMU
  power cycle clears it; that is the most likely reason a DFU always recovers
  the unit and a warm chain eventually does not (INFERRED).

This is the single most important limit to state in the runbook: **a
cold-equivalent ANS restart is achievable for the SoC-side state (DART, NVMMU,
NVMe queues, PCIe link), and is *not* achievable for S3E-internal firmware
state except through BFH (firmware download) or a real power cycle.**

---

## 7. Where we stand today (`a9/boot` 4e9a87e9ef)

### 7.1 Outgoing (kexec) — `apple_h9p_pcie_park`, `pcie-apple-h9p.c:2700`

```text
cancel late-port work
per port:
  cfg COMMAND = 0                    // IO+MMIO+BME off
  cfg PRIMARY_BUS = 0
  keep rc[0x114+p*0x80] REFCLK_NOGATE set   // refclk stays ON
  PERST# asserted (GPIO 0xa0 = 1)
  LTSSM stopped (port[0x80] bit0 = 0)
  udelay(60)
  NVMMU SART_CTRL = 0                // port 0 only
  dump "kexec parked (powered)"
```

before that, `nvme_pci_apple_ans_shutdown` (`pci-apple-ans.c:830`) cleared
`CC.EN|SHN` and polled `RDY=0` for ≤1 s after the generic path had drained I/O,
deleted the I/O queues and sent normal SHN.

### 7.2 Incoming — `apple_h9p_port_bringup` (`pcie-apple-h9p.c:1725`)

```text
PERST assert → nvme-mode writes (rc[0x14]=1, 100 µs, rc[0x24]=1)
port_power_on → dump "inherited" → LTSSM stop → SART=0
port_off (refclk off + port power off) → usleep(1000..2000)
nvme-mode again → port_power_on → LTSSM stop → LSCNT=3 → linkpmgrsts W1C
… PERST deassert after t-refclk-to-perst …
LTSSM enable, wait L0 ≤100 ms
```

then `nvme_pci_apple_ans_preinit` (`pci-apple-ans.c:492`): link check, read
`0x1824/0x1828/0x1808/0x180c`, read `0x1b18` (refuse if BFH), `CC=0`, poll
`RDY=0|CFS=0` (1 ms × 2 s), then `reset_dma` → `apple_ans_nvmmu_reset`
(`pcie-apple-h9p.c:1347`) which zeroes TCB/SGL/scratch, checks the invalidate
FIFO (10 µs × 20 ms), writes SART off, then
`apple_h9p_nvmmu_program` (`:920`) = scratch → TCB → INIT → SART.

### 7.3 Differences that matter

| # | iOS | us | consequence |
|---|---|---|---|
| D1 | shutdown = SHN only, `CC.EN` left set | we also clear `CC.EN` and poll `RDY=0` | extra controller reset; not inherently wrong, but it is a state iOS never leaves |
| D2 | PERSTs only together with a teardown: on sleep it asserts PERST and removes refclk + port power within 60 µs; in the firmware path it asserts reset and immediately deasserts it again | we PERST and then *leave* PERST asserted with power and refclk on for the whole gap between kernels | a state iOS never parks in; the S3E's PCIe core sees reset without a rail cycle, and the next kernel starts from it |
| D3 | port power-up re-runs DART + NVMMU force_active | our port power cycle does **not** re-program DART or NVMMU afterwards; NVMMU is programmed earlier, in `h9p probe`/`preinit` | ordering differs from Apple's |
| D4 | queues/tags retired before stop; FIFO checked empty with fatal 20 ms | tags retired implicitly by queue deletion; FIFO only checked at the *next* boot's preinit | a stuck tag survives the jump (round 7: bit 14) |
| D5 | PMGR never touched | PMGR never touched (domains held `RPM_ACTIVE`, syscore) | matches |
| D6 | HMB written before CC.EN | same | matches |
| D7 | no BFH handling at boot (only iBoot does) | we detect BFH and refuse | correct for us, but see §6 |
| D8 | port enable has an "already enabled" path (INFERRED early return) | we always do a full power cycle | we never take the "iBoot-like handoff" path |

---

## 8. Gaps, ranked by expected value

1. **The incoming port reset is not followed by an NVMMU/DART re-init.** Apple
   pairs them. Move (or repeat) `apple_ans_nvmmu_reset` + DART
   `attach_config`/TTBR/TLB after the port power cycle and before the endpoint
   is taken out of reset, i.e. inside/after `apple_h9p_port_bringup`.
2. **The outgoing side leaves a half-reset endpoint.** Either (a) hand off like
   iBoot (no PERST, no LTSSM stop, link stays in L0) and let the incoming side
   decide, or (b) do a *complete* iOS-style port teardown (PME_TO, L2, PERST,
   refclk off, port power off) so the next power-up is a real cycle. The
   current middle state is the one state neither iOS path produces.
3. **The NVMMU invalidate FIFO is only drained on the next boot.** Drain and
   log it at shutdown, after queue teardown and before `CC.EN` clear; treat a
   non-empty FIFO as "do not jump" (or at least as a recorded failure cause).
4. **`SHST` completion is not verified on our shutdown path.** iOS waits for
   `CSTS.SHST == 10b` and panics if it fails. We log the `nvme_disable_ctrl`
   result but do not gate on it.
5. **No "already enabled" fast path.** Every warm boot does a full port power
   cycle even when the link is healthy. That is defensible, but it is not what
   iOS does and it is the path that has been failing.
6. **No post-reset health gate.** iOS reads `0x1b18`, `0x1808/0x180c`,
   `0x1824/0x1828` and reacts (BFH → download; invalid → fail). We read them
   and log, but only BFH changes behaviour.

---

## 9. Proposed cold-equivalent ANS restart

Two variants. **A is Apple-faithful** (copy the iBoot→xnu handoff).
**B is a stronger reset** (copy the firmware-activate path). They can be
combined: A when the handoff looks clean, B when it does not.

### A. Apple-faithful handoff (recommended first experiment)

Outgoing, after I/O drain and queue deletion:

```text
1  SHN = 01; wait CSTS.SHST == 10b (mask 0xc, expect 2), timeout ≥ 1 s
2  poll port0[0x22c] bit0 (NVMMU invalidate FIFO empty), 100 ms; log result
3  clear endpoint cfg COMMAND (BME, MEM, IO) — no new DMA
4  clear root-port routing (cfg 0x04, cfg 0x18) — as today
5  do NOT assert PERST, do NOT stop LTSSM, do NOT touch refclk/port power,
   do NOT clear CC.EN
6  log link state + LTSSM + NVMMU SART/TCB + ANS 0x1b18 + CC/CSTS
```

Incoming (this is the new part):

```text
7  if link is up and LTSSM == L0 and PERST deasserted and 0x1b18 != magic:
        skip the port power cycle entirely      (Apple's "already enabled")
   else:
        fall back to B
8  endpoint cfg COMMAND comes back from the PCI core's enable
   (MEM|BME); do not set it before the controller is stopped again
9  CC = 0; poll CSTS.RDY == 0 (and CFS == 0)
10 NVMMU reprogram in Apple's order: scratch(0x10/0x14) → TCB(0x08/0x0c) →
   INIT(0x04 = 0x10000, poll bit16) → SART(0x24/0x28/0x2c, then 0x20)
   (this is `apple_ans_nvmmu_reset`, already implemented)
11 DART: re-apply s5l8960x global config + TTBRs + per-stream TLB invalidate
   for the endpoint's stream (this is what `apple_dart_setup_translation`
   already does on attach — make sure attach happens after step 9/10)
12 EnableController exactly as iOS: AQA/ASQ/ACQ → 0x1800 = 0 →
   0x1810/0x1814 = HMB → 0x1818 = size → CC.EN = 1 → poll RDY = 1
```

Rationale: this is bit-for-bit the state iOS's kernel expects after iBoot, and
it is the only handoff Apple itself performs with a live ANS.

### B. Strong reset (firmware-activate path)

Outgoing: same as A steps 1–4 (and additionally you may assert PERST).

Incoming:

```text
1  PERST asserted, LTSSM stopped, cfg routing cleared
2  refclk off (rc[0x114+p*0x80] &= ~1), port power off (rc[0x100+p*0x80] &= ~1)
   — iOS's disablePortHardware; keep it off ≥ 10 ms (we use 1–2 ms today)
3  refclk on, poll rc[0x2c] & 0x10, IODelay(100)
4  port power on, poll rc[0x12c+p*0x80] & 1, rc[0x100+p*0x80] &= ~0x100
5  IODelay(t-refclk-to-perst = 100 µs)   ← i.e. release PERST *after* refclk
6  phy-ip lane init, LTSSM enable, wait L0
7  NVMMU full reprogram (step A10), DART re-init (step A11)
8  NVMe init (step A12)
```

Note step 2's hold time: iOS does not specify one (it relies on human-scale
sleep); ours is 1–2 ms. If the S3E needs its firmware to notice the reset, a
longer hold is cheap to test (10–50 ms) and is the only tunable that could
explain "works for a few boots then not".

### PMGR domains involved (live ADT, VERIFIED)

| domain | ADT id1 | index | what it powers | our DT node |
|---|---|---|---|---|
| `PCIE` | 30 | 2 | RC core | `ps_pcie` |
| `PCIE_AUX` | 57 | 3 | aux clock | `ps_pcie_aux` |
| `PCIE_REF` | 58 | 9 | reference clock | `ps_pcie_ref` |
| `PCIE_LINK0` | 59 | 4 | port 0 = **ANS** | `ps_pcie_link0` |
| `PCIE_LINK1` | 60 | 5 | port 1 = Wi-Fi | `ps_pcie_link1` |
| `PCIE_LINK2/3` | 61/62 | 6/7 | unused | `ps_pcie_link2/3` |

Our driver holds all seven with `DL_FLAG_RPM_ACTIVE` and marks them syscore
(`pcie-apple-h9p.c:448`), i.e. they never power off. **iOS behaves the same in
effect**: `configure` enables the clock gates (slot 173 for ids 57, 58 and
`port+2`), and we found no gating call in the port teardown path. **Do not add
a PMGR domain cycle**: it would reset the shared DART and the Wi-Fi port, and
it is not what Apple does. The correct lever is the port-local power/refclk
bits, which are already in our code.

---

## 10. Safety (per AGENTS.md)

- No writes to NVMe namespaces 2–7 anywhere in this plan; the only NVMe traffic
  is the admin-queue shutdown/init that the driver already performs.
- No syscfg, NVRAM or boot-firmware access. The BFH path (§6) is **read-only
  detection** in this plan; implementing an actual firmware download would
  write NAND and must not be attempted without an explicit decision and a
  backup.
- A PMGR domain power cycle of `PCIE_LINK0` would also reset the shared
  `dart-apcie0` and (through `PCIE`) the Wi-Fi port. Not recommended, not
  Apple-like.
- A longer port power-off hold (B step 2) is safe: it is the same operation the
  driver already performs, only longer.
- Anything that asserts PERST while the link is up must keep the existing
  "no endpoint BAR access after a known fault" guards.

---

## 11. What to measure on the next device run (orchestrator only)

1. Outgoing, just before the jump: `0x1b18`, `0x1800`, `CC`, `CSTS`,
   `0x1824/0x1828`, NVMMU `0x04/0x20/0x24/0x2c`, port0 `0x22c`, port LTSSM.
2. Incoming, before `CC=0`: the same set. A non-`0xbfbfbfbf` `0x1b18` proves
   the firmware survived; a changed `0x1800` proves a mode was left behind.
3. Incoming, immediately after the port power cycle and **before** NVMMU
   reprogram: whether the invalidate FIFO is non-empty (bit0 clear) — that is
   the round 7 signature and would confirm D4.
4. After a failure, dump the DART `err/addr` together with the *old* admin CQ
   IOVA from the previous boot: if the fault address equals the previous
   kernel's CQ, the fault is the endpoint writing to a stale queue, not a
   missing translation.
5. Repeat with the port power-off hold at 10 ms and 50 ms (variant B) to see
   whether the degradation point moves.

---

## 12. Open questions

- Does iBoot send SHN before handing over? Not observable from the
  kernelcache. It matters only for how much FTL state the kernel inherits.
- Does the S3E treat PERST as a full firmware reset, a PCIe-only hot reset, or
  neither? The BFH mechanism suggests the ROM only runs after a rail cycle;
  a board-level measurement (ANS current, or `0x1b18` right after PERST with
  the rail up) would settle it.
- Is `0x22c` bit0 (the invalidate FIFO empty bit) meaningful while the
  controller is disabled? iOS only reads it with the controller running.
- Why does the ANS2 class carry `IOSleep(1500)` before its CC write? If the S3E
  needs a comparable settle time after a reset, our 1–2 ms hold and our
  immediate `CC=0` may both be too fast.
