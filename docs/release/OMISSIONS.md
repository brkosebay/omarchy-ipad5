# What this snapshot leaves out

This is an early source/research snapshot, not the full lab workspace or an
installer. The private export manifest records the committed source ref and
all exclusions. [Status](STATUS.md) and [evidence](EVIDENCE.md) retain short
observations; raw evidence is private.

All private graphics research is excluded: kernel and userspace sources,
patch exports, firmware analysis, hooks, probes, experiments and their logs.
Mixed files that depend on those inputs are also excluded. The public
verifier rejects new paths or content in that category. Software rendering
is the supported desktop path.

SEP and the Touch ID sensor named Mesa are separate work and are retained.
No sensor patch, fingerprint frame or Apple-derived binary is distributed.
The latest end-to-end sensor experiment stopped when SEP refused the patch
request, before sensor power-on; patch/frame bytes were not logged.

H.264/VXD is retained. Its CAVLC table is generated from pinned
Imagination/TI GPL-2.0 source, with 544/544 values unchanged. Upstream notices,
the generator and the independent comparison input remain; see
[VXD provenance](vdec-gpl-provenance.md). DEVA firmware is excluded.

Apple firmware, kernel binaries, kernelcaches, extracted kexts/dylibs, ADTs,
IM4M/SHSH, syscfg/NVRAM, per-device calibration, probe captures, raw boot logs,
photos, bulk disassembly/decompiles, build trees and vendored clones are not
included. Inputs must come from your own device. Large literal capture/code
blocks are trimmed from retained notes. Register/protocol analysis and short
interface constants remain; this curation is not a complete provenance audit.

Private release audits, source inventories, scrubber and owner publication
checklists remain outside the public tree. Only the generic verifier ships.
MACs/identifiers are redacted, network addresses are examples and the local
account placeholder is `linux`. Configure your own credentials and tool paths.

Markdown targets that were omitted point here. Historical commands may need
private local inputs and are not a supported installation sequence. Use
[PORTING.md](../../PORTING.md) and current status before dated notes.
