# Omarchy bring-up workstream brief

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

## Facts (verified 2026-10-22)

> Private graphics-dependent excerpt omitted from this source snapshot.

## Round 1 tasks

> Private graphics-dependent excerpt omitted from this source snapshot.

## Rules

- **Never touch the iPad** (no `scripts/boot.sh`, `linux-run.sh`,
  `ipad-ssh.sh`, palera1n, pongoterm). The orchestrator boots and tests.
- Write only under `docs/omarchy/`, `scripts/omarchy/`,
  `configs/omarchy.fragment`, `build/omarchy/`, `patches/omarchy/` and the
  kernel tree `/src/wt/omarchy`. Leave other workstreams' trees/docs and
  shared scripts alone (propose changes to shared scripts in your report).
- Large downloads are OK. Keep total new disk usage ≤ 30 GB and clean up
  intermediate tarballs/containers.
- Private data (firmware, calibration, SSH keys) stays in `build/` images,
  never in committed files.
- Don't commit in this repo, and don't push anything anywhere.
- Label claims **verified** (evidence) or **inferred**.

Final report: what's built (sizes, versions), how Omarchy was installed and
which platform it believes it's on, boot-path design, kernel fragment,
Hyprland plan, the test plan summary, and open risks.
