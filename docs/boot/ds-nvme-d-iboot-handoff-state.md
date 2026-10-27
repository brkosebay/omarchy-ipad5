# What iBoot actually leaves in ANS / NVMMU / DART / apcie, and the outgoing sequence that reproduces it

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Research only. No device access, no kernel edits, no commits. Sources: the decrypted
`iBoot.j71t.RELEASE.im4p.dec` (iOS 16.7.10, 20H350, `build/power/oc-r3/bootrefs/20H350__iPad6,11/`,
disassembly `build/power/oc-r3/iboot.asm`, load base `0x870000000`), the live ADT
(`build/firmware/private/IODeviceTree.txt`), the shared Ghidra kernelcache findings already recorded in
[enable-freeze.md](../pcie/enable-freeze.md) and [ds-nvme-b](ds-nvme-b-ios-ans-restart.md), and the
logs in [round8b](../release/OMISSIONS.md) / [round8c](../release/OMISSIONS.md) /
[round8d.md](../release/OMISSIONS.md).

**[V]** = read directly out of the named artifact/line. **[I]** = inference, with its reason.

---

## 0. Bottom line

1. **iBoot never disables ANS when it jumps to xnu.** The kernel-load path starts the
   controller (`0x870001f58` → `0x870017e4c` → `0x87001a1d8` + `0x87000bba0`) and then loads
   and jumps; the *only* function that sends SHN and clears `CC.EN` (`0x87000c840`) is reachable
   from the device-close/restart/halt/error paths, not from the jump. **[V]** call graph;
   **[I]** that no indirect close runs in the jump path.
   So at the **iBoot → xnu handoff**: `CC = 0x460001` (EN=1, SHN=00), `CSTS = 0x1`
   (RDY=1, **SHST=00**, CFS=0), the port is powered/linked/LTSSM-enabled, PERST deasserted, and
   iBoot's own HMB + NVMMU SART + DART tables are still live.

2. **iBoot does own a "stop ANS" sequence, and it is exactly Round 8d's contract**:
   `SHN=01` → wait `CSTS & 0xc == 8` → `CC.EN=0` → wait `RDY=0`.
   `0x87000c958`…`0x87000c9c4`. **[V]** This is the strongest precedent available for 8d, and it
   is *not* what 8b/8c handoff mode did.

3. **No Apple path ever leaves `CSTS.SHST = 10b` for the next kernel.** Not iBoot's handoff
   (SHST=00, never sent), not iBoot's stop (SHST=10b but then EN=0 and RDY=0), not xnu
   (`CC=0` in `EnableController`). The 8b/8c "handoff" state — SHST complete **with** `CC.EN=1`
   — is a state neither Apple component produces. That is the one state common to both failing
   first same-entry jumps.

4. **iBoot pairs "controller state" with "port state" and never mixes them:**
   port **up** → it either leaves ANS running (jump to xnu) or closes ANS with the port left up
   (device power-off/halt); port **down** → it closes ANS first and then takes the port fully
   down (`0x87001a720`: clear RC `0x124`, `0x114`, `0x100`). Our kexec crosses these: in all
   three rounds the outgoing shutdown changes the controller state but the incoming then runs a
   **full port bring-up** regardless. The 8c hang is precisely a port bring-up that faults before
   any endpoint BAR access, i.e. while the S3E was in the non-Apple state.

5. **The single most useful correction to the working theory:** our cold boot does **not** run
   iBoot. `third_party/pongoOS/src` contains no PCIe/NVMe code at all, and the m1n1 checkout used
   here exposes only a PCIe trace helper — the payload is loaded over USB, so on the
   DFU → pongoOS → m1n1 → Linux path nothing has programmed the ANS/DART/port when Linux first
   touches them. **[V]** for pongoOS (no `nvme`/`pcie` symbol in the tree); **[I]** for m1n1.
   Two measurements support it: the driver's `CAP=… CC=… CSTS=… BFH=…` line is printed **before**
   the `CC=0` write (8b log line 21 at 11.699706 vs "stopping controller (CC=0)" at 11.739744), and
   on a cold boot that same line reads `CC=0/CSTS=0`; and the DART reads `0xffffffff` at 0.062 s
   (unclocked). **[V]**
   Therefore the cold baseline that "always works" is the **power-on-reset** state, not an iBoot
   state; it proves our driver can initialise from POR, not that it can accept a firmware handoff.

6. **Recommendation for 8d (and 8e if 8d still fails):** keep 8d's disabled-controller contract,
   and pair it with the port policy instead of always cycling the port:

   | outgoing controller | outgoing port | incoming must |
   | --- | --- | --- |
   | EN=1, SHN=00, RDY=1 (iBoot handoff, §8A) | up | **not** power-cycle; `CC=0`/`RDY=0` only (xnu behaviour) |
   | EN=0, SHN=00, RDY=0 (iBoot stop, §8B) | up | **not** power-cycle either — iBoot's stop leaves the port up |
   | EN=0, SHN=00, RDY=0 | fully down (`0x124`,`0x114`,`0x100` all clear) | power-cycle (full bring-up) — this is 8d **full-cycle**, and it matches iBoot's restart order |
   | EN=1, SHN=01, **SHST=10b** | up or down | *no Apple equivalent; this is the 8b/8c state* |

   i.e. **8d handoff mode should also take the port down**, or its incoming must skip the power
   cycle. The current 8d text ("incoming handoff still uses 8c's full port sequence") re-creates
   the un-paired combination.

   Concrete asks for the 8d implementer, in priority order:

   1. Keep the SHST → `CC.EN=0` → `RDY=0` → BME=0 contract exactly as written. It is iBoot's own
      stop sequence and needs no change.
   2. Record, in the outgoing log and in the entry cmdline, **whether port 0 was left up or taken
      down**, and make the incoming policy derive from that record instead of from the mode name.
   3. If the pairing cannot be changed before the next run, prefer **full-cycle** for the first
      same-entry chain: the only combination that has ever worked on this unit is "controller
      disabled + port taken down to at least PERST-asserted/LTSSM-stopped" (tri32 → 8c entry), and
      8d full-cycle is the closest tested member of that family. 8d *handoff* (port left up) is the
      combination that has never been tested with a matching outgoing controller.
   4. Add the eight `STEP shutdown …` lines from §9 so the next failure names its step.

---

## 1. Where the iBoot facts come from

`iboot.asm` is a linear disassembly of the decrypted payload at `0x870000000`. The ANS driver is a
compact block at `0x87000c2xx`–`0x87000e4xx`; the PCIe code is at `0x87001a1xx`–`0x87001adxx`.
Names below are descriptive; none of them are iBoot symbols (iBoot is stripped).

| Role | Address | Evidence |
| --- | --- | --- |
| device accessors: read32 / write32 / write64 | `0x87000c404` / `0x87000c358` / `0x87000c3a0` | **[V]** all ANS register call sites use these three with a constant `w1` offset |
| read CC (`0x14`) / write CC | `0x87000db90` / `0x87000db9c` | **[V]** leaf: `ldr w0,[x19,#0xc4]; mov w1,#0x14; b …` |
| read CSTS (`0x1c`) | `0x87000daac` | **[V]** same shape with `#0x1c` |
| ANS instance object lookup | `0x87000dbe0`, `0x87000c258`, `0x87000c22c` | **[V]** |
| controller **enable** (mode-driven) | `0x87000cd84(dev, mode)` | **[V]** §2.2 |
| controller **CC.EN/RDY** writer | `0x87000ca00(dev, enable)` | **[V]** §2.1 |
| HMB / scratch programming (`0x1808/0x180c/0x1810/0x1818`) | `0x87000e07c` | **[V]** §2.3 |
| controller **stop** (SHN → SHST → EN=0 → RDY=0) | `0x87000c840` | **[V]** §2.4 |
| device close (stop + queue destroy + free) | `0x87000bc5c`, `0x87000bca0` (instances 0 and 1) | **[V]** §2.5 |
| device power-state setter | `0x87000d0d8` (stop then re-enable) | **[V]** §2.5 |
| PCIe port **init/train** (port 0) | `0x87001a1d8` | **[V]** §3 |
| PCIe port **down** (RC `0x124`,`0x114`,`0x100`) | `0x87001a720` | **[V]** §3 |
| "all PCIe down" (RC `0x24`, GPIO 0x54–0x57/0x52/0x53/0x1a) | `0x87001a764` | **[V]** §3 |
| delay helpers (µs) | `0x870042f7c` | **[V]** `delay(100)` used at `0x87001a31c`, i.e. `t-refclk-to-perst` |

Ranges are also useful: **SHN is written from exactly one place**, `0x87000c840` (bits 15:14), and
`CC.EN` from exactly two, `0x87000ca00` (bit 0 only) and `0x87000cd84` (full `0x460000` during
enable). **[V]** — a scan of every direct call site of `0x87000db9c` and of the three ANS register
helpers returns only those functions.

---

## 2. iBoot's ANS driver, decoded

### 2.1 `CC.EN` + `RDY` writer — `0x87000ca00(dev, enable)`

```
0x87000ca28  cc = read32(0x14)                 ; if 0xffffffff -> -0x138b
0x87000ca40  write32(0x14, (cc & ~1) | enable) ; only bit 0
0x87000ca48  deadline = now + (enable && dev->c3 == 1 ? 0x03938700 : 0x0E4E1C00)
loop:
0x87000ca74  csts = read32(0x1c)               ; 0xffffffff -> -0x138b
             enable ? (csts & 1) -> 0 : !(csts & 1) -> 0
0x87000ca98  delay 1000 us                     ; = 1 ms poll interval
             expired -> -0x1771 (enable) / -0x1770 (disable)
```

**[V]** The two deadlines are `0x03938700` (60 000 000) and `0x0E4E1C00` (240 000 000); with a
1 ms poll they read as 60 ms / 240 ms if the unit is ns. **[I]** on the unit. The *short* deadline
is selected only when enabling and the recorded mode (`dev+0xc3`) is already `1` **[V]**; after a
stop the mode is `7`, so the re-enable takes the long one.

### 2.2 Controller enable — `0x87000cd84(dev, mode)`

```
0x87000cdb8  ca00(dev, 0)                 ; EN=0, wait RDY=0   <- always, even from POR
0x87000cdc4  dev->c3 = mode
0x87000cdd0  e07c(idx, mode)              ; HMB/scratch, §2.3
0x87000cde4  read64(0x00)                 ; CAP: MPSMIN, MPSMAX, DSTRD sanity
0x87000ce0c  dev->d8 = 4 << MPSMIN
0x87000ce8c  zero admin queues
0x87000ceb8  write32(0x1800, mode)        ; mode = 1 on the normal path (0x87000bc3c)
0x87000cecc  write32(0x14, 0x0046_0000)   ; EN=0, SHN=00, IOSQES=6, IOCQES=4, MPS=0, AMS=0
0x87000cef0  write32(0x24, AQA)           ; (ACQS-1)<<16 | (ASQS-1)
0x87000cf00  write64(0x30, ACQ)
0x87000cf08  write64(0x28, ASQ)
0x87000cf2c  ca00(dev, 1)                 ; CC = 0x460001, wait RDY=1
0x87000cf38  dev->c2 = 1                  ; "running"
```

**[V]** Two consequences worth keeping:
* iBoot's live `CC` is **`0x460001`** — bit-for-bit the value Linux's `nvme_enable_ctrl()` writes
  on this part (`MPSMIN = 0` from `CAP=0x20f00103ff`, so `MPS=0` despite 16 KiB pages). So "our
  running controller" and "iBoot's running controller" are the *same* register image.
* iBoot writes `0x1800 = 1`, xnu writes `0`, Linux writes `0`. The register is per-kernel state and
  is not part of any handoff contract. **[V]** values, **[I]** that it carries no cross-kernel
  meaning.

### 2.3 HMB / scratch — `0x87000e07c(idx, mode)`

```
0x87000e0dc  size  = read32(0x1808)        ; required scratch size   (Linux logs 0xb00000)
0x87000e0f4  align = read32(0x180c)        ; required alignment      (Linux logs 0x1000)
             validate: align is a power of two, size >= align, both page-aligned
0x87000e158  va = 0xb0000000 - max(size, align)      ; iBoot's own ANS VA window
0x87000e1b4  iova = dart_map(dev->iommu, pa, va, pages)
0x87000e1c4  write64(0x1810, iova)
0x87000e1cc  write32(0x1818, size)
```

**[V]** The buffer is mapped through the ANS's mapper handle (`dev+0x18`, `0x87000ddac` →
`0x870028f88`) and the *device-visible* result is what goes into `0x1810`. iBoot's window is its
own: it computes `va = 0xb0000000 - max(size, align)` (≈`0xaee00000`), whereas xnu's
`nvme-scratch-virt-region` is `0xbc000000`. **[V]** both values; **[I]** that the handle is the
NVMMU SART mapper rather than the DART (the returned address has to be inside whichever window the
ANS translates, and the two kernels use different windows — so the SART window is *not* a
cross-kernel constant either).

### 2.4 Controller stop — `0x87000c840`

```
0x87000c958  csts = read32(0x1c)                    ; bail out if 0xffffffff
0x87000c968  cc   = read32(0x14)
0x87000c970  write32(0x14, (cc & 0xffff3fff) | 0x4000)   ; SHN = 01, EN kept
loop:
0x87000c978  csts = read32(0x1c)
0x87000c980  (csts & 0xc) == 8 -> done               ; SHST == 10b  (CFS/RDY ignored)
0x87000c994  delay 1000 us ; deadline 0x0E4E1C00 (240 ms)
0x87000c9c4  ca00(dev, 0)                            ; CC.EN=0, poll RDY=0 via 0x87000ca00
0x87000c9cc  dev->c3 = 7                             ; "off"
```

**[V]** Note precisely what is *absent*: no PERST, no refclk, no port power, no LTSSM, no
`dart_force_active`, no `nvme_mmu_force_active`, no HMB deregistration.

Before the SHN it also retires two iBoot queue objects (`0x87000c884`/`0x87000c8e8` submit a
"flush/sync"-style command and then clear the queue flags) — the analogue of "drain and delete the
I/O queues" in our driver. **[V]** code, **[I]** the command semantics (opcode `0` and `4` in the
built command buffers).

### 2.5 Who calls stop, and who does not

Call sites of `0x87000c840` — the complete list: (a `grep` for the address in the whole
disassembly returns these three `bl` sites, the function prologue, and **no** `adr`/literal
reference, so it is not installed in a vtable either) **[V]**

| Caller | Context | Reached from |
| --- | --- | --- |
| `0x87000bc5c` | device close: stop → destroy I/O queue → free | `0x87000bca0` (both instances) |
| `0x87000ce54` | enable failed halfway | `0x87000cd84` error exit |
| `0x87000d160` | power-state change (`0x87000d0d8`) | vtable only, no direct caller found |

`0x87000bca0` (the real "close ANS") has exactly two callers:

* `0x870017ec4`, inside the **restart dispatcher** `0x870017eac`: `… → bca0 → 0x87000655c →
  0x870005d7c →` a jump table on restart type (`1`…`6`) → `0x870000534` / `0x870041f7c` /
  `0x870006064` / `0x87001df90(3)` / `0x87001bbcc` **[V]**
* `0x8700183ac`, the **halt path**: `0x870017e08 → bca0 → 0x87000655c → 0x87001ecc8 →
  0x870000534(0) → 0x870022904 → b .` (infinite loop) **[V]**

And the **kernel-load path does not call it.** `0x870001f58` is the boot stage that prints the
`DIP>` / `DIP<` progress markers and calls `0x870017e4c`, which is:

```
0x870017e58  … 0x87001a1c0 (set a global)  → 0x87001a1d8(port 0)   ; PCIe port up
0x870017e74  0x87000bba0(0)                                        ; ANS start (instance 0)
0x870017e78  status < 0 -> log
```

**[V]** `0x87000bba0` is the "start NVMe" entry: it checks `0x87000c2a4` (already active?), runs
`0x87000de08` (firmware/state check + admin queue allocation), `0x87000c288`, `0x87000c648`
(queues), then reads the kernel. Nothing after it in that function closes the device.

**Conclusion [I, strong]:** at the iBoot → xnu jump, ANS is *running*.

---

## 3. iBoot's PCIe port code

### 3.1 Per-port init — `0x87001a1d8(port)`

```
0x87001a28c  RC[0x600000124 + p*0x80] &= ~1        ; port enable off
0x87001a298  RC[0x600000114 + p*0x80] |=  1        ; refclk on        (helper 0x87001acf8)
0x87001a2a0  RC[0x600000100 + p*0x80] |=  1        ; port power on
loop 0x87001a2b0: RC[0x60000012c + p*0x80] & 1     ; power ready, 10 us steps (0x87001ac48)
0x87001a2cc  RC[0x600000124 + p*0x80] |=  1        ; port enable on
0x87001a2fc  two pin/GPIO helpers (0x870040ca8)    ; sideband: PERST/CLKREQ among them
0x87001a31c  delay(100)                            ; = t-refclk-to-perst (ADT = 100)
0x87001a338  PHY[0x6100000a0 + p*0x8000] (u16) bit0 = 1
0x87001a354  link speed/width fields programmed from the trained link
0x87001a390  write32(0x6_00004020 + p*0x40, 3)     ; link/LTSSM control
```

**[V]** register addresses and order. **[I]** the meaning of `0x6_00004020` (it is inside the RC
`0x8000` window and is written last, after the link-status read) and the exact position of the
PERST edge: PERST is the `function-perst` GPIO `0xa0` and is driven by one of the two pin helpers,
whose internals this sweep did not resolve. What matters here is only the end state (§3.3).

### 3.2 Port down — `0x87001a720(port)`

```
RC[0x600000124 + p*0x80] &= ~1
RC[0x600000114 + p*0x80] &= ~1
RC[0x600000100 + p*0x80] &= ~1
```

**[V]** No delays between them (contrast xnu's `perst-to-refclk-delay-us` = 60 µs after `0x124`).
Called from the full-PCIe-down path `0x87001a764` and from the port-init error exit.

`0x87001a764` additionally clears `0x6_00000024` bit 0 and drives GPIO `0x54`–`0x57`, `0x52`,
`0x53`, `0x1a` — the rail/clock pins for the whole PCIe block. **[V]** code; **[I]** pin names.

### 3.3 What that means for the handoff

Because iBoot loaded the kernel over ANS, by construction at the jump: link **up**, LTSSM
**enabled**, port **powered**, refclk **on**, PERST **deasserted**, endpoint config space
enumerated, `COMMAND.MEM|BME` **set**. Nothing in the jump path lowers `0x124`, `0x114` or
`0x100`; those appear only in `0x87001a720`/`0x87001a764`, reached from the same restart/halt
paths as the ANS close, plus the port-init error exit. **[V]** for the code; **[I]** that
nothing else raises PERST (PERST is GPIO 0xa0 — `function-perst` in the live ADT — and the only
PERST-adjacent code found is in the port init/down helpers).

---

## 4. The live ADT, for the fields the table needs

From `build/firmware/private/IODeviceTree.txt` (this is the tree **iBoot** published to xnu, so
every `function-*` property below is something iBoot resolved at boot):

| Node | Property | Value | Meaning |
| --- | --- | --- | --- |
| `apcie@10000000` (`apcie,s8003`) | `#ports` | 4 | **[V]** |
| | `IODeviceMemory` | `0x610000000`+16 MiB, `0x600000000`+0x8000, `0x601000000`/`0x601004000`, `0x602000000`/`0x602004000`, `0x603000000`/`0x603004000`, `0x604000000`/`0x604004000`, `0x600008000`+0x4000, `0x60A000000`+0x40000 | **[V]** PHY/common, RC, then **port _p_ = `0x601000000 + p*0x1000000`**, NVMMU at port+`0x4000`. (Corrects the `+p*0x4000` wording in ds-nvme-b §2.) |
| | `perst-to-refclk-delay-us` | `0x3c` = 60 | **[V]** |
| | `apcie-config-tunables`, `apcie-common-tunables`, `apcie-phy-tunables` | — | the tables our driver replays |
| `pci-bridge0` | `function-perst` | GPIO `0xa0`, flags 0 | **[V]** |
| | `function-clkreq` | GPIO `0xa4`, flags 2 | **[V]** |
| | `function-dart_force_active` / `function-nvme_mmu_force_active` | phandles `0x72` (dart-apcie0) / `0x74` (nvme-mmu0), tag `'Fact'` | **[V]** the port activates DART and NVMMU |
| | `t-refclk-to-perst` | `0x64` = 100 µs | **[V]** |
| | `nvme-mode`, `allow-endpoint-reset`, `maximum-link-speed`=2 | — | **[V]** |
| `s3e@0` (the ANS endpoint) | `device-id`/`vendor-id` | `0x2002`/`0x106b` | **[V]** matches `IOPCIMatch 0x2002106b` |
| | `function-boot_from_host` | GPIO `0x8d`, flags `0x0101` | **[V]** BFH is a **pin**; `0x1b18` mirrors it |
| | `function-pcie_port_control` | tag `'PrtC'`, args `0x6d`, `0x6e` | **[V]** the APCIEPortControl stub |
| | `nvme-scratch-virt-region` | `0xbc000000` + `0x1200000` | **[V]** |
| | `namespaces-present` | `7`, with the per-NS table incl. the 4 MiB write window | **[V]** |
| `nvme-mmu0@1004000` | `sart-region` | `0x87ee00000` + `0x1200000` | **[V]** |
| | `sart-virtual-base` | `0xbc000000` | **[V]** |
| | `tags` / `sgl-entries` | `0x80` / `0x100` | **[V]** |
| | `invalidate-fifo-workaround`, `cache-invalidate-workaround` | present | **[V]** |
| `dart-apcie0@1008000` | `reg` | `0x601008000`+0x4000 | **[V]** |
| | `vm-offset` / `vm-size` | `0x80000000` / `0x3c000000` | **[V]** |
| | `diag-config` / `fetch-config` / `error-reflector` | `0x100` / `0x000e0303` / `0x20fffc000` | **[V]** |
| | `page-size` | `0x1000` | **[V]** |

---

## 5. Field-by-field: iBoot handoff vs our outgoing shutdown

Column meanings: **iBoot-run** = state at the iBoot → xnu jump (§2.5); **iBoot-stop** =
immediately after `0x87000c840` (§2.4); **8b/8c handoff** = the measured outgoing state of the two
failing first same-entry jumps; **8d** = the contract Round 8d implements; **cold-POR** = what our
DFU→pongoOS→m1n1 path actually hands us (§0.5).

| Field | iBoot-run (jump to xnu) | iBoot-stop | 8b/8c handoff (measured) | 8d | cold-POR |
| --- | --- | --- | --- | --- | --- |
| `CC` @ `0x14` | `0x460001` EN=1 SHN=00 IOSQES=6 IOCQES=4 MPS=0 **[V]** `0x87000cecc`+`0x87000cf2c` | `0x460000` (EN=0) **[V]** `0x87000ca40` | `0x464001` EN=1 **SHN=01** **[V]** `8c` log 41.045640 | `0x460000` **[I]** derived from 8d §"Change" | `0x0` (never written) **[V]** |
| `CSTS` @ `0x1c` | `0x1` RDY=1 SHST=00 CFS=0 **[I]** from "no SHN in the jump path" + `0x87000ca74` | `0x8` during the wait **[V]**; `0x0` after the RDY clear **[I]** | `0x9` RDY=1 **SHST=10b** **[V]** | `0x0` | `0x0` **[V]** |
| SHN history | never sent | `01` → completed | `01` → completed | `01` → completed | n/a |
| ANS `0x1800` | `1` (iBoot mode) **[V]** | unchanged | `0` (Linux) — outgoing log `BFH=0xbadadd`, mode not printed | `0` | n/a |
| ANS `0x1810`/`0x1818` | iBoot's HMB IOVA + size, still programmed **[V]** `0x87000e1c4`/`0x87000e1cc` | unchanged | Linux HMB `0x1200000@0x87ee00000`, DMA `0xbc000000` **[V]** | kept | none |
| ANS AQA/ASQ/ACQ `0x24/0x28/0x30` | iBoot's admin queue depth + its two DMA addresses, live because EN=1 **[V]** `0x87000cef0`/`0x87000cf00`/`0x87000cf08` | registers unchanged, controller logic reset **[I]** | inherited values not logged; our own `AQA 0x1f001f ASQ 0xbbffe000 ACQ 0xbbfff000` programmed after `CC=0` **[V]** | reprogrammed inbound | `0` |
| ANS `0x1b18` (BFH) | not `0xbfbfbfbf` (firmware self-hosted) **[I]** | unchanged | `0xbadadd` **[V]** | same | `0xbadadd` **[V]** |
| ANS firmware | running; FTL cache **not** flushed (no SHN) | running, post-shutdown (cache flushed) | **done with shutdown processing** | reset by CC.EN=0 | ROM/self-loaded, never touched by AP |
| NVMMU SART | enabled, iBoot's own window (§2.3), `pa` = iBoot HMB **[V]** code path | unchanged (iBoot never touches NVMMU) **[V]** | `SART 0x1 0xbc000000..0xbd100000 pa 0x87ee` **[V]** | retired in full-cycle, kept in handoff | `SART 0x0` (round-7 warm prints; no cold capture) **[V]** |
| NVMMU TCB / INIT | iBoot TCB array, INIT done **[I]** | unchanged | TCB `0x8:…`, INIT done, `err` all 0 **[V]** | rebuilt inbound | `0x0` **[V]** |
| NVMMU invalidate FIFO (`port0+0x22c` bit0) | empty **[I]** (no outstanding invalidates) | empty | `0xa01`/`0x101` = **empty** (bit 0 set) **[V]** ds-nvme-c §4 | checked empty | `0x1` |
| DART `TCR` / `TTBR0..3` | iBoot's tables, per-stream bytes enabled **[I]** (iBoot maps the ANS via `0x870028f88`) | unchanged | `TCR 0x80`, four TTBRs, later `TCR 0x0/TTBR 0x0` after our reinit **[V]** 8b log 0.428/11.503 | reinit checked | `0xffffffff` at 0.062 s (unclocked) **[V]** |
| DART `err`/`addr` | iBoot's last transaction record | unchanged | `0x500`/`0x100`, `valid=0` **[V]** | cleared at attach | n/a |
| RC `0x600000100 + p*0x80` | bit0=1 (powered) **[V]** `0x87001a2a0` (iBoot sets only bit 0; xnu additionally clears bit 8 **[I]**) | unchanged | `pwr 0x101` **[V]** | handoff: kept; full-cycle: 0 | 0 |
| RC `0x114` (refclk) | bit0=1 **[V]** `0x87001a298` | unchanged | `refclk 0x1` **[V]** | same as power | 0 |
| RC `0x124` (port enable) | bit0=1 **[V]** `0x87001a2cc` | unchanged | bit0=1 (port block dump `en 0x80704c4b` is a *different*, per-port register) **[V]** | same as power | 0 |
| RC `0x12c` (power ready) | bit0=1 **[V]** poll at `0x87001a2b0` | unchanged | `rdy 0x1` **[V]** | same as power | 0 |
| PERST# (GPIO `0xa0`) | deasserted **[I]** (link is up) | unchanged | deasserted (handoff) / asserted (full-cycle) | mode-dependent | GPIO default |
| LTSSM | enabled, link **L0** **[I]** (kernel was read over it) | unchanged | L0; `linksts 0x5`, `stat 0x11` **[V]** | mode-dependent | off |
| Endpoint `COMMAND` | MEM+BME=1 **[I]** | unchanged | BME cleared by us **[V]** | BME=0 | 0 |
| PMGR PCIe domains | untouched, running **[V]** code has no PMGR access in these paths | untouched | untouched (syscore) | untouched | PMGR probe |

Three rows are the whole story: **`CSTS.SHST`**, **`CC.EN`**, and **"was the port touched"**.

---

## 6. What iOS (xnu) does first at boot

Order and addresses are the shared-Ghidra findings already recorded in
[enable-freeze.md](../pcie/enable-freeze.md) (IONS) and [ds-nvme-b §4](ds-nvme-b-ios-ans-restart.md);
they are restated here as the *incoming* contract.

1. `AppleS8003PCIePort` is created per port from the ADT. Its enable path
   `enableGated` `fffffff005ed954c` → `enablePortHardware` `fffffff005ee73dc` first checks
   `_state` and has an explicit "already enabled" branch
   (`"apcie[%u:%s]::%s trying to enable the port that is already enabled (_state = %d)"`,
   string at `fffffff00548c4a6`). **[V]** string + branch; **[I]** whether a *fresh* xnu boot takes
   that branch — `_state` is only 2/4/5 if `start()` derived it from hardware, and that one read is
   the only thing we cannot see offline. This is the single open question in this document.
2. If it does run the full path: refclk → clock-ready poll → power → ready poll → 100 µs →
   PERST release → PHY lanes → LTSSM → L0, then it calls `function-dart_force_active(1)` and
   `function-nvme_mmu_force_active(1)` (both ADT properties above), which is what re-runs the DART
   hardware init `fffffff005fb5b58` and `AppleNVMeMMU::setActive(1)` `fffffff005eebf3c`
   (scratch `0x10` → TCB `0x08/0x0c` → INIT `0x04` → SART `0x24/0x28/0x2c/0x20`). **[V]**
3. `IONVMeController::start` `fffffff00679f03c` → slot `0x688` `fffffff006779034`; the **first
   ANS register read** is `IsBootFromHost` `fffffff0067ad84c` = `read32(0x1b18) == 0xbfbfbfbf`.
   It is not, so normal init. **[V]**
4. `EnableController` `fffffff0067a16bc`: `CC=0` → poll `RDY=0` → AQA/ASQ/ACQ →
   `PreCCEnableHook` `fffffff006780098` (`0x1800=0`, `0x1810`, `0x1818`) → `CC=0x460001` →
   poll `RDY=1`. **No PERST, no port power, no NVMMU/DART teardown on this path.** **[V]**
5. `AppleEmbeddedNVMeController` is instantiated with `IOPolledInterface =
   IONVMeControllerPolledAdapter` and `IOMatchedAtBoot = Yes`, `CurrentPowerState = 1`
   (live ADT) — i.e. xnu expects to take over a controller that is already electrically live.

So xnu's own model of "inherited ANS" is exactly **§8A**: running controller, live port, `CC=0` and
reprogram. It never asks the endpoint to have been shut down first, and it never re-powers the port
as part of the controller reset.

---

## 7. Why the 8b/8c state is the odd one out

| State | Produced by | Port | Safe to hand to a full port bring-up? |
| --- | --- | --- | --- |
| EN=1, SHN=00, RDY=1 | iBoot at the kernel jump | up | n/a — Apple's incoming never cycles the port |
| EN=1, SHN=01, **SHST=10b** | **nobody except our 8/8b/8c handoff** | up | **no evidence, and the one sample hung** |
| EN=0, SHN=00, RDY=0 | iBoot stop (`0x87000c840`) | up | plausible (device closed, link alive) |
| EN=0 + port fully down | iBoot restart order (`bca0` then `0x87001a720`) | down | yes — and this is what the passing entry effectively did (tri32: EN=0 + PERST + LTSSM stop) |

`SHST=10b` is a *terminal* state for the endpoint's shutdown processing. The NVMe spec allows the
host to power the controller off at that point; it says nothing about what a vendor controller does
when its link is then torn down and retrained, and this S3E is a NAND controller with its own
firmware that survives PERST (ds-nvme-b §6). A firmware that has parked its FTL and possibly gated
its PHY/CLKREQ is a plausible partner for an RC that wedges during the next power-up — and the 8c
hang is on the **RC side**, in the port bring-up, before any endpoint BAR access. **[I]**

That also explains why 8b and 8c failed *differently* while sharing the outgoing state: 8b took the
"already enabled, no port reset" path and reached a working controller (then failed in its own
health gate); 8c forced a full port bring-up and the RC stalled. Same outgoing state, two different
incoming policies — and the only sample that touched the port is the only one that hung. Two samples
are not statistics, but they are consistent, and the fix for both readings is the same pairing rule
in §8D.

### 7.1 Delta against ds-nvme-b §9 (this supersedes its variant A steps)

ds-nvme-b §9A proposed: send SHN and wait SHST (steps 1–2), then "do **not** clear `CC.EN`"
(step 5). Those two steps together produce exactly the state in the second row above — SHST
complete with `CC.EN=1` — and that is the contract 8b/8c implemented. The iBoot evidence shows the
inconsistency: iBoot either sends **no** SHN at all (handoff, `CC.EN=1`, `SHST=00`) or sends SHN
**and then** clears `CC.EN` (stop). "SHN + keep EN" is not one of its states.

The rest of ds-nvme-b §9A is still right and is what §8A formalises: live port, no PERST, no port
power change, no PMGR, `CC=0` → `RDY=0` → reprogram on the incoming side. The only change is that
the outgoing side must not inject a shutdown notification into that handoff.

---

## 8. The exact outgoing sequence to reproduce iBoot

All writes are to the endpoint BAR unless marked RC/PHY. Offsets: ANS `0x14` CC, `0x1c` CSTS,
`0x1800/0x1808/0x180c/0x1810/0x1818/0x1b18`; RC `0x600000000 + p*0x80`; port block
`0x601000000 + p*0x1000000`; NVMMU `port+0x4000`; DART0 `0x601008000`.

### 8A. iBoot's *run* handoff (Apple's own kexec-equivalent)

Use this when the incoming will take the live-link path (no PERST, no power cycle) — i.e. the xnu
model. Preconditions: no outstanding command, empty invalidate FIFO.

```text
 1  freeze blk-mq, drain, sync, delete the I/O queues; SQ tail == CQ head on both queues
 2  poll port0 NVMMU +0x22c bit0 (FIFO empty), 20 ms bound; fail the jump otherwise
 3  read and log: CC, CSTS, 0x1b18, 0x1800, 0x1808, 0x180c, 0x1810/0x1814, 0x1818,
    NVMMU 0x04/0x20/0x24/0x28/0x2c, +0x22c, DART TCR + 4 TTBR, err/addr,
    RC 0x100/0x114/0x124/0x12c/0x2c, port +0x208/0x210 (link/LTSSM), PERST GPIO
 4  require: CC.EN=1, CC.SHN=00, CSTS.RDY=1, CSTS.SHST=00, CSTS.CFS=0, 0x1b18 != 0xbfbfbfbf
 5  DO NOT write CC at all.  No SHN.  No CC.EN clear  (iBoot writes nothing here)
 6  DO NOT touch the port: no PERST, no LTSSM stop, no RC 0x124/0x114/0x100, no PME_TO/L2
 7  DO NOT touch NVMMU SART/TCB, DART TCR/TTBR, or 0x1800/0x1810/0x1818
 8  leave PCI COMMAND as the PCI core left it (MEM|BME).  iBoot leaves BME set.
    Clearing BME is a deviation: it stops new DMA but does not stop the firmware.
```

Everything here is the *absence* of a write. If the architecture of the driver makes "no write"
hard (because the generic NVMe shutdown already sent SHN), then this variant is not reachable
without skipping `nvme_disable_ctrl()` for this device — which is exactly the delta against 8b/8c.

### 8B. iBoot's *stop* (what 8d implements, and the only other state iBoot leaves)

```text
 1  freeze/drain/sync, delete I/O queues, require empty software map bitmap
 2  FIFO empty check (port0 +0x22c bit0), 20 ms bound, fatal on failure
 3  CC = (CC & 0xffff3fff) | 0x4000            ; SHN = 01, EN kept      [0x87000c970]
 4  poll CSTS, require (CSTS & 0xc) == 8, 1 ms poll, 240 ms bound     [0x87000c978]  (unit per §2.1)
 5  CC = CC & ~1                                ; EN = 0, SHN = 0       [0x87000ca40]
 6  poll CSTS, require RDY == 0, reject 0xffffffff                    [0x87000ca74]
 7  read back CC (require 0x460000), CSTS (require 0x0), block BME
 8  then EITHER leave the port alone (iBoot's stop does not touch it)
    OR take it fully down and let the incoming cycle it:
       RC[0x600000124 + p*0x80] &= ~1     ; 60 us  (xnu) / 0 us (iBoot)
       RC[0x600000114 + p*0x80] &= ~1
       RC[0x600000100 + p*0x80] &= ~1     ; 0x87001a720
    Optional full block-down (iBoot's restart path only):
       RC[0x600000024] &= ~1 ; GPIO 0x54-0x57,0x52,0x53,0x1a       ; 0x87001a764
```

8d's ordering (SHST → clear EN/SHN → RDY=0 → BME=0) and its guards match steps 3–7 exactly. The
only thing 8d adds beyond iBoot is the link/CFS checking, which is strictly better.

### 8C. What must not be produced any more

* `CC.EN=1` **with** `CSTS.SHST=10b` at the moment of the jump (8b/8c handoff).
* A partial port teardown: any of `0x124`/`0x114`/`0x100` cleared while another stays set.
* Clearing `CC.EN` and *then* reading endpoint registers without the existing link/fault guard.

### 8D. Pairing rule (the actual recommendation)

| outgoing | incoming policy |
| --- | --- |
| 8A (EN=1, SHN=00) | live-link path: verify power/refclk/ready/PERST/link L0, skip the port cycle, `CC=0` → `RDY=0` → reinit. **This is xnu's behaviour and the only iBoot-compatible pairing.** |
| 8B + port left up | same as 8A (the link is still up; only the controller was reset) |
| 8B + port fully down | full bring-up (refclk → power → ready → 100 µs → PERST release → PHY → LTSSM → L0), then `CC=0`/`RDY=0` |

The current 8d text pairs "8B + port left up" (handoff mode) with the full bring-up. Either flip
handoff mode to also take the port down, or keep the port up and skip the cycle inbound. Building
both, with the mode name recorded in the log, is the cheapest way to settle it on the next run.

---

## 9. What to capture on the 8d run so the next failure is attributable

The 8d STEP lines already narrow the port path. Add these to the *outgoing* callback, in this
order, one line per step (they are cheap; only the endpoint `CC`/`CSTS`/`0x1b18` reads cross the link
and they already have the link/fault guard):

```text
STEP shutdown begin CC=… CSTS=… SHST=… CFS=… BFH=…        ; before anything
STEP shutdown fifo=… used=…                                ; NVMMU +0x22c bit0 + SW bitmap
STEP shutdown shst=…                                       ; after the SHN wait
STEP shutdown disabled CC=… CSTS=… BME=… mode=…            ; after the RDY=0 wait
STEP shutdown port rc pwr=… refclk=… en=… rdy=…            ; RC 0x100/0x114/0x124/0x12c
STEP shutdown port linksts=… ltssm=… perst=…               ; port +0x208/+0x210, GPIO 0xa0
STEP shutdown dart tcr=… ttbr=… err=… addr=…               ; DART 0x601008000
STEP shutdown handoff                                     ; last line before the jump
```

Classification rules for the next chain (put them in the runbook, not in code):

* outgoing `CSTS=0x0` + `CC=0x460000` → 8B taken, sample valid.
* outgoing shows `SHST=10b` with `EN=1` → the old contract leaked back in; that sample is not a
  test of 8d.
* incoming hangs after `STEP port … tunables` and before `apple-port-enable` → RC register access,
  i.e. the port's own clock/power state, not the endpoint.
* incoming hangs after `apple-port-enable` → link training against the S3E; compare the outgoing
  `shst=` field of the previous boot.
* incoming reaches `CC=0`/`RDY=0` but not `RDY=1` → controller-side, and then the outgoing SHN
  history is the first thing to check.

A single extra experiment settles the SHN question for good: run one chain with the outgoing
`SHN` write suppressed (step 3 of 8B skipped, `CC.EN=0` only). If the same-entry jump then stops
failing, the poison is the SHST-complete state, not the `CC.EN` value. It is a one-line, offline
buildable variant and it does not weaken 8d.

---

## 10. Limits, and what is still unknown

* **iBoot is stripped.** All names above are ours; the function boundaries come from the linear
  disassembly. The call-graph claim in §2.5 is a complete scan of direct `bl` sites *and* of
  address-taken references; the only stop path that could be reached indirectly is the device
  power-state setter `0x87000d0d8`, which is itself never called directly in this image and which
  stops and then **re-enables** to the requested state — so it cannot produce the SHST-complete
  state either. **[V]** scan, **[I]** the conclusion.
* **We do not know whether a fresh xnu power-cycles the ANS port at boot** (§6.1). `_state` is a
  new object field, so the "already enabled" branch can only fire if `start()` derived it from
  hardware. Resolving it needs one decompilation of `AppleS8003PCIePort::start` in the shared
  Ghidra project (`fffffff005ed…`), which this report did not run (the kernelcache was not opened;
  only the already-recorded findings were reused).
* **No device data exists for iBoot's handoff on this unit.** The cold path never runs iBoot, and
  no iOS boot has been instrumented. The handoff table is therefore code-derived, not measured.
* **The ANS firmware is opaque.** Nothing here says what the S3E does internally on SHN or on
  PERST; ds-nvme-b §6 remains the best statement of that limit. A rail cycle is still the only
  known way to reset it.
* **The 8c hang has no register evidence.** Its last completed step was the port tunables; the
  next MMIO may have wedged. 8d's STEP lines are the right instrument, not a new theory.

---

## 11. Reproduce (read-only, host only)

```sh
# iBoot: the ANS driver
sed -n '12010,12100p' build/power/oc-r3/iboot.asm      # 0x87000bbcc..  start + close
sed -n '12810,12930p' build/power/oc-r3/iboot.asm      # 0x87000c840     stop (SHN/SHST/EN=0)
sed -n '12929,13060p' build/power/oc-r3/iboot.asm      # 0x87000ca00     CC.EN/RDY writer
sed -n '13060,13370p' build/power/oc-r3/iboot.asm      # 0x87000cd84     enable (0x1800/CC/AQA)
sed -n '14360,14460p' build/power/oc-r3/iboot.asm      # 0x87000e07c     HMB 0x1808/0x180c/0x1810
sed -n '26740,27100p' build/power/oc-r3/iboot.asm      # 0x87001a1d8/720 port init / port down
sed -n '24460,24500p' build/power/oc-r3/iboot.asm      # 0x870017e4c     DIP: pcie+nvme start
sed -n '1975,2060p'   build/power/oc-r3/iboot.asm      # 0x870001f58     kernel-load stage

# every caller of the two functions that can touch CC
rg -n 'bl #0x87000c840|bl #0x87000bca0|bl #0x87000bc5c' build/power/oc-r3/iboot.asm
rg -n 'bl #0x87000db9c|bl #0x87000ca00'                 build/power/oc-r3/iboot.asm

# live ADT fields used above
rg -n 'nvme-scratch-virt-region|sart-region|sart-virtual-base|function-perst|function-boot_from_host|function-dart_force_active|t-refclk-to-perst|perst-to-refclk-delay-us' \
  build/firmware/private/IODeviceTree.txt

# the two measured outgoing states
sed -n '1,30p' docs/boot/round8b-handoff-chain-fail/nvme-lines.txt
rg -n 'SHN|SHST|handoff|CC\.EN' docs/boot/round8c-handoff-chain-fail/netconsole.txt | tail -20
```

Cross-references that this document does not repeat: [ans-kexec.md](ans-kexec.md) rounds 7–8c,
[ds-nvme-b](ds-nvme-b-ios-ans-restart.md) §3–§6 (iOS teardown/bring-up, BFH), [ds-nvme-c](ds-nvme-c-forensics.md)
§3 (the only register series), [round8d.md](../release/OMISSIONS.md) (the 8d change and its device plan).
