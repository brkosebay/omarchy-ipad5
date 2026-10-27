Round 8c is one incremental `a9/boot` commit after Round 8b

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.

`24ef187b3091aaeb17455fe2ba3bfcd9f935b603`. BASE/HEAD/SERIES and manifest.json
pin the export. Hardware validation is pending; no device was accessed.

The Round 8b slot-14 `-ENODEV` came from the host/port guard before NVMMU
invalidation. The old log does not identify the precise guard branch or
request owner. It does not establish a controller failure. The old retirement
routine unconditionally reset even on these local errors, including cleanup
of a slot that might never have been mapped.

Use PCI topology for port ownership instead of live bridge bus-number
registers. Track published slots and log qid/CID/opcode/NSID on quarantine.
Unpublished slots need no invalidation. Local retirement errors retain the
slot without reset; only an invalidate FIFO timeout or its latched poison
can schedule reset from this path. Failed host/port guards log their branch
without touching the NVMMU.

Outgoing handoff still certifies drain/SHST and clears BME, deliberately
retaining CC.EN. Incoming handoff now always performs full port bring-up,
including same-entry jumps. Full-cycle differs in outgoing teardown; Round 7
remains the old electrical-sequence control. The checked DART rebuild,
pre-root health gate, write window, initramfs, config and DTB are unchanged.

Build/package: scripts/boot/{build-round8c.sh,prepare-round8c-entries.py}.
The integration is the exact detached Round 7+8+8b+8c overlay on d5822a7f71cd.
No integration or workspace commit is created. See docs/boot/ans-kexec.md,
"Round 8c", for evidence limits, offline validation and DFU → entry →
20-chain commands. Logs, release, overlay and file hashes are bound in
build/boot/round8c/{build.json,SHA256SUMS}.
