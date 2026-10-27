Round 8b is an incremental fix after `patches/boot/round8/`, based on

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.

`18eecae442`. `BASE`, `HEAD`, `SERIES` and `manifest.json` pin the exact export.

The queue maximum of 16 sectors violated `blk_validate_limits()` on this
16 KiB-page kernel (`PAGE_SECTORS = 32`). The health gate now copies the
admin queue limits validated after Identify, then sets the namespace block
size. It still issues only NSID 1 Identify and read commands before namespace
publication. Local/validation/allocation/mapping errors and returned command
errors leave the gate inconclusive and continue normal discovery; only the
existing timeout path's `ANS_HEALTH_FAILED` can request the one recovery.

The S5L8960X DART fault predicate requires bit 31. Neither err 0x500 nor a
changing last-transaction address without that bit establishes a fault.

Build/package: `scripts/boot/build-round8b.sh` and
`scripts/boot/prepare-round8b-entries.py`. The integration remains a detached
exact Round 7+8+8b overlay on d5822a7f71cd, without an integration commit.
`RELEASE`, embedded uname, `SOURCE.patch`, `build.json` and SHA256SUMS bind
that overlay to the explicitly named round8b artifact. See
`docs/boot/ans-kexec.md` for validation and orchestrator-only device commands.
