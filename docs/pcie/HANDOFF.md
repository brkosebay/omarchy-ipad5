# PCIe / NVMe workstream: handoff (end of round 7, 2026-10-22)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> **Round 8 (system suspend, 2026-10-22):** `a9/pcie` is now at
> **`ca94a3c6f9`**, patches `0036–0042`. These add opt-in s2idle
> (`pcie_apple_h9p.suspend`, default 0 = refuse) plus the non-PCIe
> blockers (GPIO set_wake, `ps_gpio` always-on, PMGR active-wakeup,
> `dwc2.s2idle_keep_gadget`). Built clean, not tried on hardware. Read
> `suspend.md` and `suspend-test-plan.md`. The round-7 text below is
> otherwise unchanged.

Read first: `test-plan.md` (round 7, Omarchy live commands), `wifi.md` (Wi-Fi, incl. round 7),
`read-only.md` (incl. the system-namespace guard), `enable-freeze.md`
(NVMe root cause and RE), `README.md` (files and scripts). Older material: `b1-hang.md`
(round 2), `kernelcache-notes.md` (round-1 RE), `live-adt-decode.txt`,
`read-only.md`.

## State of the code

- Kernel tree: `/src/wt/pcie` (Docker volume `linux-a9-src`), branch
  `a9/pcie`, tip **`868c0c56cc`**, clean. Base: HoolockLinux `hoolock`
  `6831bc70`.
- Exported series: `patches/pcie/kernel/0001–0035` plus `SERIES`.
  - 0001–0005: Pauli1Go's T8010 template.
  - 0006–0011: S8003 support (round 1).
  - 0012–0016: round 2 (tunables, NVMMU FIFO, phase logs, deferred ANS
    enable, DT).
  - 0017–0022: **round 3**:
    - DART fault-handler NULL-deref fix and TCR fix;
    - S5L8960X error reflector and fetch/diag config;
    - per-DART `apple,dma-range`;
    - DT: no `dma-ranges` on the PCIe node;
    - h9p host-state dump and NVMMU fault decode;
    - staged, instrumented ANS enable.
  - 0023: **round 4**: S5L8960X DART windows are base-relative (device
    verified), the fault print shows DART address + IOVA, the stale DART
    error is cleared at attach.
  - 0024–0025: **round 5**:
    - S5L8960X table entries need bits 1:0 = 0b11 (iOS `pa | 3`); new
      io-pgtable quirk; hardware-view walk in the DART fault handler;
    - ASQ/ACQ translation and walk logged before CC.EN.
  - 0026–0031: **round 6**:
    - 0026: system namespaces (NSID != 1) write-protected, independent of
      the read-only Kconfig (`nvme.apple_ans_unlock_system_namespaces`);
    - 0027: bring-up diagnostics behind `nvme.apple_ans_debug`;
    - 0028–0029: D2365 PMIC GPIO binding + driver (cherry-picked from
      a9/periph, identical);
    - 0030: h9p late ports (`pwren-gpios`): endpoint power, link and rescan
      after probe;
    - 0031: j71t Wi-Fi DT (port 1, PMU GPIO 14, wifi0, uart2, dart1).
  - 0032–0035: **round 7**:
    - 0032: brcmfmac parses `brcm,ccode-map(-trivial)` on PCIe DT nodes;
    - 0033: P2P disabled for the iOS 4355 C0 firmware (FWID 01-9787ef4d,
      `p2p_da_override` fails -52);
    - 0034: `pcie_apple_h9p.aspm_l1_ports` opt-in (default 0);
    - 0035: `brcm,ccode-map-trivial` on wifi0.
- m1n1 patches: `patches/pcie/m1n1/0001` (NVMe HMB), `0002` (Wi-Fi
  calibration from `/arm-io/uart2/wlan`); `build/m1n1-pcie/m1n1.bin`.
- Config fragment: `patches/pcie/pcie.config`, now with the Wi-Fi stack
  (26 options). **Always build with `scripts/pcie/build.sh`.**
- Wi-Fi firmware + regdb: `scripts/pcie/mkinitramfs-wifi.sh` (runs
  `scripts/pcie/fetch-regdb.sh` if needed) →
  `build/kernel-pcie/initramfs-wifi.gz` (+ `initramfs-wifi-overlay.gz`).
- Payloads: `scripts/pcie/mkpayload-pcie.sh` →
  `build/kernel-pcie/m1n1-linux-pcie-r7{,-aspm,-debug}.bin` (built for
  `868c0c56cc`; patched m1n1 checked).
- Edit workflow: `scripts/pcie/ktree.sh pull|push PATH` ↔ `/tmp/pcie-edit`.
  Commit in the container (`docker run ... linux-a9-kbuild bash -c 'cd
  /src/wt/pcie && git -c safe.directory=* commit ...'`).
  **Never** run `make` at older commits in the shared tree.
- Round-3 RE helpers (scratch, not committed), in `/tmp/pcie-re`:
  - `g.sh`: Ghidra CLI wrapper;
  - `vt.py`: vtable slot resolver;
  - `kdis.py`, `scan.py`, `barscan.py`, `ptrscan.py`: raw capstone
    disassembly and scans of the kernelcache. They need the
    `/tmp/pcie-re/venv` capstone and `/tmp/kc-re/scripts/kc.py`.

## Device results so far

> Private graphics-dependent excerpt omitted from this source snapshot.

## Root cause of the freeze (details: `enable-freeze.md`)

- **Verified bug:** `apple_dart_s5l8960x_irq()` dereferenced
  `dev_iommu_priv_get(dart->dev)`, which is NULL for the DART's own device.
  - So every PCIe DART fault was an oops in hard IRQ, i.e. a panic, which
    `panic=0` holds forever.
  - simpledrm's deferred-I/O fbdev never flushes the oops to the screen
    (inferred), and the panic stops the other CPU, so no detector output.
- **Trigger (verified in r3-hold and r4; inferred to be the same in round
  2):** the first SQ fetch faults in the DART. Two Linux bugs combine:
  - indexing with the raw IOVA, while the DART sees `IOVA - 0x80000000`
    (fixed by 0023);
  - table entries without bit 1, which the S5L8960X treats as an invalid
    STE (fixed by 0024; verified in iOS's code, inferred on hardware).
- **Verified not the cause:** the NVMe register sequence (a full IONVMeFamily
  BAR-access scan matches Linux). Unused-power-domain shutdown is ruled out
  by timing.

## DMA-zone regression: fixed (verified on device in r3-hold)

`dma-ranges` is gone from the PCIe node. The DARTs carry
`apple,dma-range = <0 0x80000000 0 0x3c000000>` (dart1 `0x3ffff000`), and
apple-dart uses that as the 1-cell master window (base-relative indexing
on S5L8960X since 0023).

Verified in the built DTB:
- no `dma-ranges` on the PCIe node;
- the only remaining `dma-ranges` is the USB complex's, ending at
  0x8ffffffff (above RAM, as in the stock kernel);
- the DART properties are present.

Device (r3-hold): `cma: Reserved 16 MiB at 0x800000000`, `DMA [mem
0x800000000-0x87bbe3fff]`, `software IO TLB: mapped ... (2MB)`.

## Open points

1. NVMe under load: the round-6 soak (test-plan section 3) is the first long
   run of FlatDMA/HMB/MSI. Watch for faults or timeouts.
2. The DART loses its probe-time setup when the PCIe port powers up
   (verified). The attach-time re-apply covers boot, but not a port power
   cycle after attach (suspend).
3. `pte-protection-override` is 0 on this DART, so iOS never sets the
   no-read/no-write bits. Linux sets them on one-direction streaming maps,
   which the S3E doesn't use (data goes through FlatDMA). Revisit if the
   BCM4355 (dart1) faults on a direction-specific map.
4. NSID 1 writes stay off (`CONFIG_NVME_PCI_READ_ONLY`). Enabling them is a
   separate decision (BRIEF policy), and the system-namespace guard stays
   either way.

## Wi-Fi: see `wifi.md`

- Works at driver level (round 6). Power: REG_ON = D2365 PMU GPIO 14 via
  h9p late-port bring-up.
- Round 7:
  - MAC chain verified in code (ADT 00:00:00:00:00:00 → m1n1 → DT → NVRAM
    macaddr), to be confirmed on device;
  - regdb in the initramfs; country to the firmware; P2P off; ASPM L1
    opt-in.
- Open: association, throughput, power numbers; whether Apple's CLM takes
  `<CC>/0`; system suspend (no PM callbacks in h9p/ANS glue); REG_ON off
  when Wi-Fi is off; DEVICE_WAKE and host-wake (GPIO 113) unused.

## Exact next steps

1. Orchestrator runs `test-plan.md` sections 1–5 and 7 live on the current
   Omarchy session, then reboots with round 7 (+ the owner's
   `cfg80211.ieee80211_regdom=`) for section 6, including the ASPM run.
2. Integration owner takes patches 0032–0035 and the refreshed
   `initramfs-wifi-overlay.gz` (regdb).
3. Next PCIe work by result: system suspend/resume for h9p + ANS; RC refclk
   gating for L1.2 if L1 holds; powering the Wi-Fi chip off when the radio
   is off.

Rules unchanged:
- never touch the device;
- NVMe stays read-only (`CONFIG_NVME_PCI_READ_ONLY`, see `read-only.md`);
- write only under `docs/pcie/`, `patches/pcie/`, `scripts/pcie/` and
  `/src/wt/pcie`;
- no commits in the linux-a9 repo.
