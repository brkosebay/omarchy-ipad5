# Round 3: the SoC freeze at CC.EN

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


## What the device showed (orchestrator, 2026-10-22)

| Boot | m1n1 / HMB | Result |
|---|---|---|
| integ + patched m1n1 | iBoot SART HMB at 0x87ee00000 via 0xbc000000 | Through `HMB DMA 0xbc000000 size 0x1200000 programmed, enabling controller (RDY timeout 120s)`, then the w1 HDQ attach and `OF: Bad cell count ... pmic@3c`, then **nothing** for minutes: no lockup, RCU or hung-task output, no shell. |
| same kernel, stock m1n1 (no HMB) | none, NVMe stops at ENODEV before any write | Clean boot to the shell in about 2 s (`docs/test-logs/2026-10-22-integ3-nonvme-dmesg.txt`). |
| round 1 B1 | DART HMB | Last line `LTR cap at 0x178, L1SS cap at 0x180`. Round 1 logged nothing between that and CC.EN, so it fits the same event. |

So the freeze is in the NVMe path, at or right after CC.EN (verified by the A/B).

**Timing (verified from the clean boot's log).** CC.EN was written at about
0.243 s. In the clean boot, the `OF: Bad cell count` lines run until 0.250 s,
`apple-tristar-power ... CBTL1610` follows at 0.255 s, and `clk: Disabling
unused clocks` / `PM: genpd: Disabling unused power domains` only come at
0.684 s. The frozen boot never printed the Tristar line. So whatever stopped
the machine happened within about 5-12 ms of CC.EN, long before Linux powers
off unused domains. **Unused-power-domain shutdown is ruled out as the trigger**
(inferred from timing).

## Root cause

### Why nothing was printed: verified bug

`apple_dart_s5l8960x_irq()` (hoolock `drivers/iommu/apple-dart.c`) did:

```c
struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dart->dev);
...
dev_err_ratelimited(..., addr | cfg->dma_min);
```

`dart->dev` is the DART's own platform device. It's not behind an IOMMU, so
`dev->iommu` is NULL and `dev_iommu_priv_get()` returns NULL (verified in
`include/linux/iommu.h`). **So every S5L8960X DART fault was a NULL dereference
in hard-IRQ context: an oops that panics ("Fatal exception in interrupt").**
With `panic=0` the machine then sits there forever. The other CPU is stopped
by the panic, so no lockup or RCU detector runs, and no shell comes up.

The panic text never reaches the screen either (inferred from the code):
- The console is fbcon on simpledrm, which uses `drm_fbdev_shmem`. That
  emulation draws into a shadow buffer and copies it to the real framebuffer
  from a deferred-I/O worker every 50 ms (`CONFIG_FB_DEFERRED_IO=y`).
- After a panic that worker never runs, so the last ~50 ms of messages and
  the oops stay invisible.
- `CONFIG_DRM_PANIC` is off, and simpledrm in this tree has no drm_panic hook
  anyway (verified: no `get_scanout_buffer` in `drivers/gpu/drm/sysfb/`).
- simplefb is no alternative: `FB_SIMPLE` depends on `!DRM_SIMPLEDRM`.

This explains every symptom: the freeze within ms, no detector output, no
shell, and the last visible lines being ones printed shortly before.

### What triggered it: inferred, ranked

1. **A DART translation fault right after CC.EN.** The controller issued a
   DMA that missed our DART mappings, and the broken handler turned it into a
   panic.

   The address it used is the open question:
   - an iBoot-era address the controller still remembers (iBoot ran it with
     its own HMB and queues before handing over);
   - our ASQ/ACQ IOVA, if the raw-IOVA indexing assumption (hypothesis 2 in
     the round-2 handoff) is wrong;
   - an MSI write that the root complex didn't catch as MSI and that fell
     through to the DART.

   The fixed handler prints `translation fault: ... at 0x...`, which decides
   between these.
2. **A faulting transaction that stalls the fabric.** iOS programs the DART
   error reflector (below) so that faulting DMAs complete; Linux never did.
   Without it, a fault can leave the inbound transaction hanging, and the next
   CSTS read (polled every 1-2 ms) then wedges a CPU. This needs no panic, but
   it would have shown lockup-detector output on the other CPU, which never
   appeared. Less likely than 1 on its own; probably both happened.
3. **An NVMMU fault.** The old handler printed one ratelimited line and never
   acked anything. iOS has no ack: it panics on every NVMMU error. Now logged
   once, then masked.

Ruled out:
- the NVMe register protocol (below, verified against iOS);
- unused-power-domain shutdown (timing).

### Round 3 on device: trigger verified

Payload `r3-hold`, kernel `fbe06b833b` (logs
`docs/test-logs/2026-10-22-pcie-r3-hold-pre.txt`, `-release-stream.txt`,
`-release.txt`):

- **No freeze.** CC.EN, a 100 ms watch with no host-side change, then CSTS
  polling. The controller went ready, and the first admin command
  (Identify) was submitted.
- At +0.74 s (including the 300 ms slow-mode sleeps):
  `apple-dart 601008000.iommu: translation fault: status:0x80000102
  stream:0 code:0x2 (NO PMD FOR IOVA) at 0x3bffe000`. The ASQ was at IOVA
  **0xbbffe000**. The fault IRQ fired once and was handled; the command timed
  out after 15 s, and the controller was disabled with no hang.
- **So the DART sees IOVAs with the 0x80000000 window base removed**
  (verified). Round 3 had indexed the tables with the raw IOVA (TTBR2 for
  0xbbffe000) while the hardware walked TTBR0 → no L1 entry → NO PMD.
  Hypothesis 2 of the round-2 handoff was wrong: what iOS prints is the
  DART-side address. The original upstream `mask = ~dma_min` was right.
  Patch 0023 goes back to it for every S5L8960X window (`iova & ~0x80000000`,
  checked to equal `iova - 0x80000000` for this window).
- Round 2 had the same mapping (raw, via `dma-ranges`). So the round-2
  freeze was almost certainly this exact fault on the first SQ fetch, a few
  ms after CC.EN, turned into a silent panic by the handler bug (inferred,
  strongly).
- DART state before the port powers up (verified): at probe (0.062 s)
  every register reads 0xffffffff. At attach (0.24 s) it is in reset state:
  - TCR 0, reflector 0x200700, fetch 0, diag 0;
  - error 0x100 (bit 31 "fault" clear), address 0x1e0f000.

  So the probe-time setup is lost, and the attach-time re-apply (patch 0018)
  is what takes effect. The 0x100/0x1e0f000 error is left over from before
  Linux attached, its origin unknown (iBoot or the port power-up; inferred).
  0023 writes it back at attach. If bit 8 is a status bit rather than a
  latched error it may stay 0x100: only bit 31 (or a new address) marks a
  new fault, which is also what the glue checks.
- `BFH=0xbadadd` (not the 0xbfbfbfbf boot-from-host marker).
- Not seen yet: MSI delivery (the command never completed), FlatDMA data
  transfers (Identify never got that far).

### Round 4 on device: base-relative indexing alone isn't enough

Integration kernel with 0023, patched m1n1
(`docs/test-logs/2026-10-22-pcie-r4-integ6.txt`): the same fault,
`status:0x80000102 code:0x2 (NO PMD FOR IOVA) at 0x3bffe000 (IOVA
0xbbffe000)`. TTBRs `0x80803f7c 0x808037d8 0x808037dc 0x80803ad0` (4 valid
tables). No freeze, admin timeout, clean disable.

Ghidra, AppleS5L8960XDART (verified):

| Function | What it does |
|---|---|
| `_createL2Tables` `fffffff005fb3650` | one zeroed 4 KB table per TTBR; `DART[0x40 + (sid*4 + idx)*4] = (pa >> 12) & 0xffffff \| 0x80000000`. Same as Linux. |
| `_createL3Table` `fffffff005fb33b4` | L2 entry (Linux: table entry / STE) = **`(pa & 0xffffff000) \| 3`**. Linux wrote `pa \| 1`. |
| leaf writer `fffffff005fb42e4` | PTE = `(pfn << 12) \| 2 \| valid`: bit 1 SP_DIS, bit 0 valid, as in DART1 |
| subpage leaf writer `fffffff005fb4208` | subpage start at bits 61:50, end at 47:36 (DART1: 63:52, 51:40) |
| protection helper `fffffff005fb4108` | 0x80 (no write) / 0x100 (no read), masked by `pte-protection-override` (0 on this DART, so iOS sets none) |
| table lookup `fffffff005fb4558`, PTE store `fffffff005fb3fa0` | TTBR = `pg >> 18 & 3`, L2 = `pg >> 9 & 0x1ff`, L3 = `pg & 0x1ff`, with `pg` REQUIREd `< vm-size`, i.e. **window-relative**: 0023's masking is right |
| error-bit names (DART strings) | "TTBR invalid", "STE invalid", "PTE invalid", ...; code 0x2 = STE invalid |

**Cause:** the STE lacked bit 1. Verified in iOS's code. That this is what
the hardware rejects is inferred: the fault names exactly that level, and
nothing else differs.
- **Fix, 0024:** `IO_PGTABLE_QUIRK_APPLE_DART_S5L8960X`. Table entries get
  bits 1:0 = 0b11, and leaf PTEs drop the DART1 subpage fields (iOS leaves
  them clear; SP_DIS is set).
- 0024 also adds a hardware-view walk to the fault handler. 0025 logs that
  walk, and Linux's own translation, for the ASQ/ACQ before CC.EN.

**IOVA window at 0x0 instead of 0x80000000?** No (inferred, but consistent
with all of the above):
- iOS hands devices 0x80000000-based addresses (vm-offset) and indexes from
  0. That is exactly what Linux now does: the device sees 0xbbffe000, the
  tables are indexed with 0x3bffe000.
- The root complex sends 0x40000000-0x7fffffff to the NVMMU. How it routes
  device addresses below 0x40000000 isn't known.

## iOS comparison (iOS 16.7.10 iPad6,11 kernelcache, shared Ghidra)

### NVMe enable path: same registers as Linux (verified)

- `IONVMeController::start` `fffffff00679f03c` → slot 0x688
  `fffffff006779034` (InitializeController wrapper).
  - It first calls slot 0x8a0 `fffffff0067ad84c`:
    `return read32(0x1b18) == 0xbfbfbfbf`, the boot-from-host (BFH) marker.
  - If BFH is not set, it goes to slot 0x540 `fffffff0067794e8`
    (`AppleEmbeddedNVMeController::InitializeController`, `burnin-size`),
    then `IONVMeController::InitializeController` `fffffff0067ac008`.
- Controller enable `fffffff0067a16bc`, in order:
  1. CC = 0, poll RDY = 0;
  2. slot 0x6b8 `fffffff0067a3a50`: AQA 0x24, ASQ 0x28, ACQ 0x30 (DMA
     addresses from the queue descriptors);
  3. slot 0x608 PreCCEnableHook `fffffff006780098`: 0x1800 = 0 ("Regular
     init"), then 0x1810 (64-bit) and 0x1818 = scratch buffer;
  4. CC = IOSQES << 16 | IOCQES << 20 | 1, poll RDY = 1.
- **A capstone scan of all of IONVMeFamily** (`fffffff006778000-fffffff0067bce20`)
  for vtable read32/write32/read64/write64 calls with constant register
  offsets found nothing else on the regular path.
  - 0x1b00/0x1b08/0x1b10/0x1b14 are the BFH firmware upload
    (`fffffff0067ad8e4`, only when 0x1b18 == 0xbfbfbfbf).
  - 0x1b20/0x1b24 are a NAND info read.
  - 0x1200-0x1210, 0x13ec and 0x4004-0x4908 belong to other controller
    types (ANS2), inferred from the `AppleANS2NVMeController.cpp` paths
    around them.
  - Linux matches the regular path: CC=0, AQA/ASQ/ACQ, 0x1800, 0x1810/0x1818,
    CC=0x460001.
- `AppleEmbeddedNVMeController::enablePCIPort(bool)` (the s3e's
  `function-pcie_port_control` 'CtrP') exists, but only error strings point
  at it. Not traced; probably link power management, not the enable path
  (inferred).

### DART: iOS programs registers Linux didn't (verified)

`AppleS5L8960XDART` start `fffffff005fb21f0` reads, among others:

| ADT property | field | j71t dart-apcie0 value |
|---|---|---|
| `diag-config` | +0xb1c | 0x00000100 |
| `fetch-config` | +0xb20 | 0x000e0303 |
| `pte-protection-override` | +0xaa8 | 0 |
| `error-reflector` | +0xaf8 | 0x20fffc000 |
| `vm-offset`, `vm-size` | allocator range | 0x80000000, 0x3c000000 |

The hardware init `fffffff005fb5b58` (vtable `fffffff006e45f70` + 0x6c0) does:
1. DART+0x24 = error-reflector >> 12;
2. stream setup (slot 0x6b8);
3. DART+0x0c (TCR) = value, read back and checked against 0x80808080;
4. DART+0x30 = fetch-config;
5. DART+0x20 = diag-config (`| 0x1200` only when a flag word is negative;
   it's 0x00030000 here).

It runs from `fffffff005fb3e00`, which REQUIREs the "active" flag (+0xaa0):
iOS re-initializes the DART every time it becomes active, i.e. after the
PCIe port powers up and calls `function-dart_force_active` (round-1 Q3,
step 13). So a PCIe DART's state probably doesn't survive a port power-up
(inferred). Linux probes the DART at 0.06 s, before the port bring-up at
0.10 s.

### NVMMU errors (verified)

The iOS handler `fffffff005eea2fc` reads:
- 0x34: error type, one bit per type;
- 0x38: PRP;
- 0x30: tag in bits 29:23;
- 0x3c/0x44: address;
- 0x40: crypto tag;
- 0x48: fetch status.

Then it panics, whatever the type. There's no ack.

## Round-3 changes (kernel `a9/pcie` `fbe06b833b`, patches 0017-0022)

| Patch | What |
|---|---|
| 0017 | apple-dart: fault handler no longer dereferences NULL (prints the raw error address). S5L8960X TCR writes replace only the stream's byte (it used to OR into stream 0's byte and clear the others). |
| 0018 | apple-dart: `apple,error-reflector`, `apple,fetch-config`, `apple,diag-config` written at reset **and again at every translation setup** (attach happens after link-up). Logs `boot state: ...` at probe and `attach: ...` (values found before re-applying) at attach. |
| 0019 | apple-dart: per-DART `apple,dma-range` IOVA window, the default for 1-cell masters. (It also made S5L8960X index raw IOVAs for such windows: wrong, fixed by 0023.) |
| 0020 | DT: PCIe DARTs get the window, reflector and configs from the ADT; `dma-ranges` removed from the PCIe node (DMA-zone regression). |
| 0021 | h9p: `apple_ans_host_state()` / `apple_ans_host_dump()` (SoC-side port, NVMMU and DART state, nothing that crosses the link); the NVMMU fault handler decodes like iOS, then masks itself. |
| 0022 | ANS glue: BFH check; MSI/AQA/ASQ/ACQ/host dump before CC.EN; dry-run, hold/go and slow modes; a SoC-side watch after CC.EN before the first CSTS read; CSTS reads gated on host state. |
| 0023 (round 4, `320a9f634d`) | apple-dart: S5L8960X windows are base-relative again (device-verified); fault print shows the DART address and the IOVA; the stale error is cleared at attach. |
| 0024 (round 5, `73a80e55aa`) | io-pgtable-dart: S5L8960X table entries `pa \| 3`, no DART1 subpage fields in leaves; fault handler walks the tables as the hardware does |
| 0025 (round 5, `ede8790b3d`) | h9p + glue: before CC.EN, log Linux's ASQ/ACQ translation and the hardware-view (window-relative) and raw table walks |

Knobs (all `nvme.` module parameters, also writable in sysfs):

| Param | Default | Meaning |
|---|---|---|
| `apple_ans_enable` | 1 | 0 = program everything, stop before CC.EN |
| `apple_ans_hold` | 0 | seconds to wait before CC.EN for `echo 1 > /sys/module/nvme/parameters/apple_ans_go`; timeout leaves the controller off |
| `apple_ans_slow_ms` | 0 | sleep after each bring-up message, so it reaches the screen (50 ms deferred flush) and a `cat /proc/kmsg` reader |
| `apple_ans_watch_ms` | 100 | SoC-side-only watch after CC.EN |
| `apple_ans_dart_hmb` | 0 | DART-mapped HMB when the loader gives none (unchanged) |

## Residual risks

- If the trigger was a fabric stall rather than the panic, the reflector
  is the fix. If the reflector address or the fetch/diag values were wrong for
  Linux's page-table layout, DART behaviour could change. They are iOS's own
  values for this DART (live ADT).
- The `attach:` line could show the DART's TTBRs/TCR being wiped by the port
  power-up. Then translation would still work (attach programs them after
  link-up), but the probe-time clear would be lost; harmless.
- The hold runs in `reset_work` with `shutdown_lock` held: `reboot` during a
  hold waits until the hold ends.
- The glue's checks cover CC writes and CSTS reads made through nvme-core.
  After the glue gives up, nvme-pci's teardown
  (`nvme_remove_dead_ctrl` → `nvme_dev_disable` → `nvme_pci_ctrl_is_dead`)
  still does a config read (`pci_device_is_present()`) and a direct CSTS
  `readl()`. Over a dead link either can stall a CPU, so a freeze *after* a
  `giving up on the controller` line is the teardown, not the enable.
- A link that is down is re-sampled for ~3 ms before it counts as lost, so
  a retrain right after CC.EN is logged as a `CC.EN+...` line, not taken
  for a fault.
- The TCR fix changes what reset does to every S5L8960X DART: all four
  streams are now really disabled. On j71t the only such DARTs are the two
  PCIe ones (verified in the DTB; only dart0 is enabled).
- A real fabric hang still freezes the SoC; only the slow mode plus a live
  `cat /proc/kmsg` can show the last step then.
