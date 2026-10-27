# Research and implementation map

Start with [current status](release/STATUS.md), [evidence](release/EVIDENCE.md)
and [PORTING.md](../PORTING.md). Only iPad6,11 / j71t / S8003 was tested.
The clean OrchardLinux port is in preparation on
[a9/orchard-j71t](https://github.com/brkosebay/linux/tree/a9/orchard-j71t).

Subsystem sources and notes live under `boot/`, `pcie/`, `touch/`, `audio/`,
`display/`, `periph/`, `power/`, `omarchy/` and `sep/`. Repeatable host/device
steps live under [scripts](../scripts/); driver sources and patch exports
are under [drivers](../drivers/) and [patches](../patches/).

Dated notes describe experiments, sometimes on different kernel bases.
A build or mock pass is not device validation. Follow each subsystem's
prerequisites and test plan, and obtain firmware/calibration from your own
unit. [Snapshot scope](release/OMISSIONS.md) explains omitted inputs and
references to unavailable private evidence.
