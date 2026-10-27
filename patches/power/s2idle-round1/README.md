# J71t s2idle round 1

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Incremental patches, not replacements for the existing workstreams:

| Patches | Owner | Apply after |
| --- | --- | --- |
| 0001–0004 | `a9/pcie` | `2752fedbbf` |
| 0005 | `a9/boot` | round 8f, `7954324dfa` |
| 0006 | `a9/touch` | `6ca4db7756` |

`manifest.json` binds exact base/head commits and patch hashes. The exporter
checks strict checkpatch, reverse applicability and reconstruction of each
owner's full source tree in a private index. Commit only on those branches;
leave the workspace and detached integration uncommitted.

Reproduce with `scripts/power/export-s2idle-round1.sh` and
`scripts/power/build-s2idle-round1.sh`. The builder combines the pinned current
integration with all boot dependencies and these increments; it does not apply
the entire six-patch series directly to an old integration.

The D2365 DT is hardware description only: PMIC irqchip/event handling and RTC
alarm operations are still missing. Only Power/Home are established wake
sources. No hardware test or device access was performed for this export.

Exact offline validation, entry packaging, warm-kexec, freeze/mem and
Power/Home acceptance commands: `docs/power/s2idle-test-plan.md`.
