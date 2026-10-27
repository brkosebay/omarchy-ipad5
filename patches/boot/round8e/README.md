Round 8e is one incremental a9/boot commit, bfe22c60a0, after Round 8d

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.

9c11ab8866e1bb6b66f0893f55a8337324e9dfc3. BASE/HEAD/SERIES and manifest.json
pin the export. No device access or hardware validation occurred.

Boot 8's incoming replay has two port-0 link-up timeouts (-110, last=0x4)
before enumeration. All seven passing boots link on their first attempt.
Nested older successes must not be counted as incoming boot-8 successes.
The exact intermittent hardware cause is unproven; the outgoing powered
port/incoming full-cycle mismatch is removed following iBoot/iOS practice.

Keep Round 8d's SHST -> EN/SHN clear -> RDY=0 -> BME=0 certificate. Both
trial modes now retire DART, isolate port 0, disable SART while powered,
fully clear RC124/114/100 bit 0 and certify the state before the jump.
Incoming logs those inherited controls and uses full bring-up. Port 1
and the Round7 control remain unchanged.

If the normal electrical attempts cannot find the root endpoint, retry
one full port-0 bring-up after a verified 100 ms off hold, logging errno
and result. Normal controller/DMA initialization follows a recovered link.
The existing one-shot controller recovery also rearms translations in
both modes and logs its failure code. Exhaustion leaves rescue armed.
Local health/slot errors, write-window guards and namespace policy remain.

The tested config/j71t DTB and rescue initramfs are unchanged. A bounded
software poll cannot interrupt a non-returning MMIO bus transaction.
See docs/boot/round8e.md for extraction, validation and the exact DFU ->
stage -> entry -> 20-chain commands. Build/package via
scripts/boot/{build-round8e.sh,prepare-round8e-entries.py}; hashes and
provenance are in build/boot/round8e/{build.json,SHA256SUMS}.
