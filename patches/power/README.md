# Power workstream patches

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


Current **overclock round 2** starts at kernel `d8ed7cdb76` and is listed in
`overclock-r2.series`. Apply that explicit series in order; the other similarly
numbered files in this directory belong to the earlier idle/SPI work below.
The authoritative complete branch is `a9/power`. No workspace commits were made.

See [the current implementation and commands](../../docs/power/overclock-round2.md).
The `power.config` fragment enables the drivers; tuning still defaults off
unless `apple_soc_cpufreq.a9_tuning=1` is supplied at boot. No voltage increases
are implemented. The immutable ceiling for future validated voltage support is
stock maximum +50000 uV; nonzero deltas currently return `EOPNOTSUPP`.

## Earlier idle/SPI round

Apply 0001–0003 in order. Kernel branch `a9/power` is based on the integration
snapshot `5a97367e23`, so export only these three workstream commits when
rebasing onto another integration. No workspace commits were made by this agent.

1. Opt-in Twister WFI residency accounting (no deeper-state claim).
2. Opt-in S8000 PIO SPI runtime autosuspend (SPI1 codec and SPI3 touch).
3. Keep the WFI state description within the cpuidle ABI size.

Both experiments default off. Merge `power.config`, then use
`cpuidle_apple_a9.enable=1` and/or `spi_apple_s8000.autosuspend=1` at boot.
Full evidence, build provenance and device test steps:
[`docs/power/round2.md`](../../docs/power/round2.md).
Exact next-iOS upload/run/pull commands:
[`docs/power/ios-round2.md`](../../docs/power/ios-round2.md).
