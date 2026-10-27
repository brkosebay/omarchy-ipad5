# Licensing scope — decided 2026-10-27

This is a source-release preparation, not a new blanket license grant.
Existing SPDX identifiers, copyright notices, license texts and authorship
retain their scope. Kernel changes are **GPL-2.0**; preserve a file's more
specific SPDX expression (`GPL-2.0-only`, `GPL-2.0-or-later` or an existing
dual license). Do not relabel imported work as our own GPL-2.0-or-later code.

**Owner decision (2026-10-27): original work is GPL-2.0-or-later.** That
matches Linux and the OrchardLinux repositories (also GPL-2.0-or-later), so
our code can move into Orchard unchanged. The full text is `LICENSE`
(template `scripts/release/templates/LICENSE`). The allocation by directory:

| Directory / material | License | Boundary |
|---|---|---|
| `patches/` targeting Linux, `drivers/`, `scripts/**/kernel/`, kernel `.inc` sources, kernel-side test shims | GPL-2.0, subject to original file SPDX | Kernel derivatives; retain dual-licensed bindings/UAPI and imported notices. Exclude non-kernel patch families from this blanket scope. |
| Original host-side `scripts/` Python/shell utilities, including release tooling | GPL-2.0-or-later | Does not cover embedded/imported kernel code, HoolockRD-derived shell functions or other upstream derivatives. |
| Original `tools/` clients, decoders, RE utilities and synthetic tests | GPL-2.0-or-later | Kernel-derived headers, imported code, compiler-generated fixtures and vendor trees need their own provenance/scope review. |
| Original Omarchy tablet/user-session utilities and config | GPL-2.0-or-later | Omarchy/Hyprland/Mesa patches follow their upstream file terms. HoolockRD-derived content stays under its upstream GPLv3 terms. |
| Original `docs/` prose and diagrams | GPL-2.0-or-later | Third-party quotations/excerpts retain their own terms; research is not a redistribution grant for the underlying binaries. |
| `configs/`, board/device-tree changes and bindings | Original/upstream file terms | Preserve Linux/device-tree SPDX and imported config notices; review original config fragments with the kernel scope. |
| m1n1, pongoOS, Mesa/Omarchy and other non-Linux patches | Upstream file terms | Do not label every file under `patches/` GPL-2.0 indiscriminately. |

## Redistribution boundary

- The curated snapshot excludes `third_party/` entirely. Fetch/build scripts
  identify upstream projects rather than shipping their clones or build output.
- Apple firmware, extracted kernelcaches/kexts/dylibs, ADTs, personalized
  IM4M/SHSH, syscfg/NVRAM and per-device calibration are not distributed.
  A developer must obtain inputs locally from their own device.
- Large literal disassembly/decompiler/capture blocks are trimmed from public
  research notes; register/protocol analysis and short interface constants
  remain. This is curation, not a claim of a complete legal provenance audit.
- All private graphics research, its patch families, firmware analysis and
  dependent mixed files are excluded from this publication.
- VXD's former table-provenance issue is resolved by pinned Imagination/TI
  GPL-2.0 source and regenerated round-5/6/7 exports; see
  [the scoped audit](vdec-gpl-provenance.md). Preserve upstream notices and
  the independent comparison input's MIT notice. DEVA firmware is excluded.
- Original author names and upstream project credits remain. Per the release
  privacy policy, non-public email addresses in exported source/patch headers
  use a neutral placeholder; public Git metadata uses the owner's GitHub
  noreply identity. Three exact, published TI/Intel author-credit addresses
  in the pinned VXD GPL/MIT source are preserved so source notices and
  SHA-256 pins remain intact. Do not remove mandatory upstream license notices.

Before publication, check authorship/provenance of retained fixtures and
embedded snippets and rerun the snapshot audit. The private workspace's full Git history and each kernel
branch's reachable history require separate review before any visibility or
push decision. A clean curated source tree does not certify those histories.
