# Boot workstream

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


See [the warm reboot runbook](../../docs/boot/warm-reboot.md) for build,
installation, limitations and ranked orchestrator-only device tests.

> Private graphics-dependent excerpt omitted from this source snapshot.

Round 1's m1n1 shim, watchdog patch, payload variables, FDT merge and optional
Mac cold-boot/developer tools were retained. This round repaired staging
immutability/provenance, late reboot fallback, panic initramfs executable paths,
initrd/DT carry-forward and explicit cold-exit policy; added the host one-command
path, PMGR hook, offline tests and documentation. No device/USB/SSH action or
workspace commit/push was performed by the implementation agent.

> Private graphics-dependent excerpt omitted from this source snapshot.

Device test 1 reached the next kernel but hung at ANS controller enable. The
[ANS follow-up](../../docs/boot/ans-kexec.md) records the evidence, separate
storage patches, early USB overlay, offline checks and next orchestrator test.
The A9 path is PCI S3E, not the later RTKit platform ANS driver; this series does
not invent an ANS PMGR reset or send RTKit commands to this endpoint.
