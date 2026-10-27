# PCIe + NVMe workstream brief (A9 `apcie`)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Goal: Linux support for the A9 PCIe host bridge (`apcie,s8000`) and the Apple
ANS NVMe controller behind it, on iPad 5 j71t (S8003). NVMe is **read-only**
until the human has a full backup. Wi-Fi (BCM4355 behind pci-bridge1) only
needs to enumerate in this round.

## Base and template

- Base: HoolockLinux `hoolock` @ 6831bc70 (Linux 7.3.0-rc1). It boots on the
  device (`PLAN.md` P1).
- Template: [Pauli1Go/HoolockLinux](https://github.com/Pauli1Go/HoolockLinux)
  branch `nvme` (A10 / T8010 iPad 7, booting Debian from NVMe). It's based on
  hoolock around 2026-07-12, so expect cherry-pick conflicts:
  - `f4e712c553` dt-bindings: pci: apple: Add T8010 PCIe host bridge
  - `e0851e9c62` PCI: apple: Add T8010 PCIe host bridge support
    (`drivers/pci/controller/pcie-apple-h9p.c`, ~1170 lines,
    `include/linux/apple-ans.h`)
  - `e01bb37c1c` nvme-pci: Add Apple ANS FlatDMA support
    (`drivers/nvme/host/pci-apple-ans.c`)
  - `d7c63aedb9` arm64: dts: apple: Add T8010 PCIe and NVMe
  - `0ad8e96404` MAINTAINERS

## ADT facts (from `docs/adt/j71t.txt` vs `docs/adt/j172.txt`)

- `apcie`: A9 is `apcie,s8000` and A10 is `apcie,t8010`. **Same `reg` bases**
  on both (raw ADT values 0x610000000 size 0x1000000, 0x601000000,
  0x601004000, ...).
  - A9: clock-gates 57–62 (6), power-gates 56, interrupts 244/247/250/253,
    bridges `pci-bridge0` (→ `s3e`, NVMe) and `pci-bridge1` (→ `wlan`,
    bcm4355).
  - A10: clock-gates 56–58, interrupts 270/273/276/279, bridges 0/2/3.
- `nvme-mmu0`: A9 is `nvme-mmu,s8000` (irq 246, two reg ranges). A10 lists
  `nvme-mmu,t8010`, `nvme-mmu,s8000`.
- `dart-apcie0/1`: `dart,s8000`, `dart,s5l8960x`.
- `pci-bridge0` carries `function-nvme_mmu_force_active` and `nvme-mode`.
- If the ADT isn't enough, the iOS 16.7.10 kernelcache is at
  `build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`.

## Tasks

1. Create your kernel tree with `scripts/build-kernel.sh --tree pcie`
   (worktree `/src/wt/pcie`, branch `a9/pcie`, already created). For git and
   inspection use `scripts/build-kernel.sh --run pcie '<cmd>'`. It's
   non-interactive, runs in `/src/wt/pcie`, and mounts this repo at `/work`.
   (`--shell` needs a TTY and won't work for you.) Fetch Pauli1Go's `nvme`
   branch shallowly, cherry-pick the series, and resolve conflicts.
2. Go register by register through what the T8010 driver programs versus the
   A9 ADT (PHY/port config, clocks, power, IRQs, link training, ANS/NVMMU,
   DART). Write `docs/pcie/a9-vs-a10.md`.
3. Adapt it: s8000/s8003 compatibles plus match data for the differences.
   Add DT nodes in the s800-0-3 dtsi files (pcie, DARTs, NVMe, PMGR power
   domains; check `s800-0-3-pmgr.dtsi`) and enable them for j71t (and the
   other iPad 5 boards if identical).
4. **Read-only NVMe.** Make it impossible for this kernel to write to the
   disk, e.g. force the namespace/gendisk read-only in the Apple ANS path by
   default. Document how it's enforced.
5. Run `scripts/build-kernel.sh --tree pcie` until it builds cleanly, then
   `TREE=pcie scripts/mkpayload.sh`.
6. Export the series with `git format-patch` to `patches/pcie/` in this repo,
   since the Docker volume isn't durable.
7. Write `docs/pcie/test-plan.md` with exact on-device commands for the
   HoolockRD busybox shell (dmesg greps, `/sys/bus/pci/devices`,
   `/proc/partitions`, GPT read via `dd | hexdump`) and the expected output.

## Policy update (2026-10-22, after round 1 launched)

The owner doesn't care about iOS on this device. Round 1 still ships
**read-only** for the first device boot, to prove reads are correct before
anything writes. Round 2 adds writes with these limits:

- Writes are opt-in via a kernel parameter and default to read-only.
- Writes are allowed only on the main data namespace (the one holding the
  APFS container). Never write to any other namespace, or to wherever
  syscfg, NVRAM or the LLB/iBoot boot firmware live (find out where on A9).
  A DFU restore can't regenerate factory calibration.
- Before the first write, a test-plan step dumps all of those regions to the
  Mac.

## Rules

> Private graphics-dependent excerpt omitted from this source snapshot.

Final report: what was done, build status, files/patches, the device test
plan, and risks/open questions.
