# Video-decoder patch scope

Rounds 5/6/7 now include the pinned Imagination/TI GPL-2.0 CAVLC table,
its unmodified source and offline generator. All 544 entries and all ten
compiled driver objects match the original implementation. The refreshed
series introduce the GPL header directly in patch 0013; round 7 reproduces
`a9/vdec-gpl` (`a8a70e4a04`). Earlier bring-up exports remain available.

See [source hashes, constant audit and tests](../../docs/release/vdec-gpl-provenance.md).
DEVA firmware is external and excluded. Private graphics research is not
included. No branches were pushed during this preparation.
