# Round 8f incremental boot patch

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Apply after `patches/boot/round8e` / a9/boot `bfe22c60a0`.
`BASE`, `HEAD`, `SERIES`, and `manifest.json` bind the single kernel commit.
Only a9/boot is committed. No device access or push was performed.

The patch preserves the 8e normal path and adds named LTSSM sampling,
standard retraining, Gen1 fallback, three long-off cycles, a guarded pre-scan
PHY replay, and an explicit missing-port rescue retry. It preserves the
bus-to-DART map when ANS initially fails to enumerate. It does not claim a
S3E rail/firmware reset or a genpd cycle under live DART consumers/keep-on.

> Private graphics-dependent excerpt omitted from this source snapshot.

