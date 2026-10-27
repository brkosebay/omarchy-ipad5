# omarchy-ipad5: instructions for people and AI agents

This repository is a curated source snapshot of Linux and Omarchy on an
Apple A9 iPad 5. Start with `README.md`, `PORTING.md`,
`docs/release/STATUS.md` and `docs/release/EVIDENCE.md`.
Only j71t / S8003 has device evidence. Another board is a port.

## Navigate and change the repo

- `scripts/`: repeatable build, extraction, boot and validation steps. Add
  reusable steps here; document inputs, outputs and stop conditions.
- `configs/`: kernel/config fragments. Public kernel source is the clean OrchardLinux series on
  `a9/orchard-j71t`; m1n1 base is HoolockLinux `idevice`.
- `patches/`: exported kernel/m1n1/userspace series, with BASE/HEAD where
  available. Directories may be different historical bases: read the series
  metadata before applying. Do not concatenate every patch directory.
- `drivers/`, `tools/`: experimental driver sources, RE tools, clients and
  offline tests. Read each tool's usage before running it.
- `docs/`: protocol/register findings, designs and test plans. Dated notes
  are historical; current support lives in `docs/release/STATUS.md`.
- `build/`, `third_party/`, `docs/adt/`: local generated/private inputs,
  ignored. Firmware and live calibration must come from the user's device.
- Public kernel source: https://github.com/brkosebay/linux/tree/a9/orchard-j71t.
  This is the sole kernel entry point. The installer port is under review; historical
  exports are separate experiments, not a combined stack.
- Private graphics research and its dependencies are excluded. Do not restore
  them. Mesa in `docs/sep/`, `drivers/mesa/` or `mesa-a9` names the Touch ID
  sensor; keep that work and keep sensor patch/frame bytes private.

Build kernels in the Docker `omarchy-ipad5-src` volume or another case-sensitive
filesystem. Keep debug info disabled unless specifically needed. Do not
silently reset a user's kernel worktree; use a separate branch/worktree for
new work. Preserve edits from other sessions.

For a driver change, state the evidence and hypothesis, add a bounded,
opt-in implementation, run the relevant offline checks, and write a device
test with expected observations and explicit stop conditions. Device access
is a separate owner-authorized step. A build or mock pass is not hardware
validation. Mark uncertain conclusions as inferred; retain contradictory
observations instead of choosing one without evidence.

## Reverse engineering

A7–A11 lack EL2, so the m1n1 hypervisor cannot trace iOS here. Use the
matching Apple DeviceTree, local Ghidra kernelcache analysis and carefully
scoped iOS observations. Document addresses, OS build and structure layouts;
rename understood functions and save the local Ghidra project.
Do not commit Apple firmware, kernelcache binaries, bulk decompiles,
disassembly listings, extracted plists, private captures or calibration.
Short interface constants and our register/protocol descriptions are useful.

## Safety rules

- **Never write syscfg, NVRAM or boot-firmware storage (LLB/iBoot).** Never
  write protected NVMe namespaces. The tested j71t fences NSIDs 2–7; identify
  every namespace independently on another device. Back up factory data and
  verify hashes before the first allowed main-disk write.
- Main-disk repartitioning requires explicit owner authorization and a tested
  recovery path. DFU restore is for the main iOS disk; it does not recreate
  unique factory calibration. Keep iOS bootable as a reference when possible.
- Initial bring-up is RAM-only, tethered: palera1n → pongoOS `bootm` → m1n1
  → Linux. Do not install a persistent bootloader as an incidental change.
- One device operator at a time. Keep waits bounded, risky probes opt-in,
  one variable per boot, and logs on the host across resets. An unpowered
  MMIO read can hang the SoC. Use the correct board's IRQ/power/DART data.
- A9 SEP mailbox: one 64-bit TX store, one 64-bit RX pop; **never read an
  empty outbox**. Do not guess writes to security/factory state.
- Warm kexec must preserve runtime FDT fixups. Do not force a pristine DTB
  through the merger or bypass a refused provenance/safety check.
- Preserve a live NBD root's host server. Kill only PIDs you started, never
  QEMU processes by name. Keep disk use bounded; avoid copying build images.
- Overclock defaults remain stock. Respect thermal/voltage/revert guards;
  measurements on one S8003 are not safe settings for another device.
- Before sharing, run `python3 -B scripts/release/verify.py . --existing-repo` and manually
  review identifiers and provenance. Do not add private values to test data.

Original work is GPL-2.0-or-later (`LICENSE`); kernel changes are GPL-2.0.
Preserve all existing author, SPDX and upstream license notices, and never
relabel imported or Apple-derived material (see docs/release/LICENSING.md).
