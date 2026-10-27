Round 8d is one incremental a9/boot commit after Round 8c

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.

bd24e6785e050003481478d798a76695006f7341. BASE/HEAD/SERIES and manifest.json
pin the export. No device access or hardware validation occurred.

The first 8c same-entry jump retained EN/RDY after successful SHST, then
stalled after incoming port tunables. Cold tri32's outgoing shutdown instead
clears EN/SHN and waits for RDY=0. Its passing incoming entry reports CC=0,
CSTS=0 after bring-up. The capture does not establish the firmware-internal
stall or iBoot's register state before the port reset.

Both trial modes now require successful SHST, a guarded EN/SHN clear, a
bounded two-second RDY=0 wait, no CFS, and CC readback before BME is blocked
and full-cycle can retire DART/power. Any failure refuses kexec. Handoff
keeps the port powered; full-cycle retains its additional outgoing teardown.

Full bring-up emits STEP lines before each phase, including the separate
IRQ, port-enable and MSI operations after tunables. Host polls have finite
bounds and identify the failed step; timeout paths avoid speculative port
dumps. Returning failures drop the port and reach the unchanged early-USB
rescue. A poll timeout cannot abort an individual MMIO bus stall; recovery
from a SoC-wide stall cannot be guaranteed in software.

All 8c slot/topology/local-error fixes, checked DMA rebuild, health gate,
recovery quota, write guards, tested config/DTB and rescue initramfs remain.

Build/package: scripts/boot/{build-round8d.sh,prepare-round8d-entries.py}.
The integration is the exact detached Round 7+8+8b+8c+8d overlay on
d5822a7f71cd. No integration/workspace commit or push is created. See
docs/boot/round8d.md in the workspace root for
source/evidence limits, validation and the DFU → stage → entry → 20-chain
commands. build/boot/round8d/{build.json,SHA256SUMS} bind build provenance.
