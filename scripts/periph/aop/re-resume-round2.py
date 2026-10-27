#!/usr/bin/env python3
"""Offline AOP state-8 resume evidence and private-Ghidra annotations.

Run with the ghidra-mcp virtualenv Python and GHIDRA_INSTALL_DIR set.
"""

import argparse
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / "build/periph/aop-attach/firmware"
PROGRAM = "/pinned-aop.elf"
FW = ROOT / "build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin"
OUT = ROOT / "build/periph/aop-stage3/re/resume-round2"


def verify_artifacts() -> None:
    fw = FW.read_bytes()

    def words(address: int, count: int) -> tuple[int, ...]:
        return struct.unpack_from(f"<{count}I", fw, address - 0x01000000)

    # The first mailbox object uses the +0x4008/+0x4010/+0x4018 receive
    # registers. Its data IRQ is the group-4 source 1; 0x10045/46 are faults.
    assert words(0x01052C48, 4) == (0x00040000, 0x00040001, 0x00010045, 0x00010046)
    assert words(0x01052D08, 4) == (0x00040002, 0x00040003, 0x00010047, 0x00010048)
    # State 8 masks IRQ, snapshots CPSR, moves the transition object, then
    # masks FIQ before sending its acknowledgement and entering WFI.
    assert fw[0x032D4C:0x032D56] == bytes.fromhex(
        "72b6 eff30083 4046 71b6".replace(" ", "")
    )

    wake = (ROOT / "docs/periph/logs/aop-wake-20261026/hop1-dmesg.txt").read_text()
    assert "set4000=00001111->00000110" in wake
    assert "in4008=00023301->00023301" in wake
    assert "in4008=00003401" in wake
    assert "HANDOFF wake-window end path=none" in wake

    OUT.mkdir(parents=True, exist_ok=True)
    evidence = {
        "firmware": str(FW.relative_to(ROOT)),
        "state8": {
            "ack": "0x01032d84: wire 0x201",
            "wfi": "0x01032d98",
            "wake_programming": "none in rtk_power_transition",
        },
        "mailbox_rx": {
            "object": "0x01052c28",
            "wrapper_offsets": ["0x4008 status", "0x4010 data", "0x4018 pop/ack"],
            "data_irq": "0x00040001 (wrapper source bit 0)",
            "error_irqs": ["0x00010045", "0x00010046"],
            "state8_specific_enable": False,
        },
        "device_result": "wrapper bit 0/12 unmasked; 0x220 remained queued for 20 s",
    }
    (OUT / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")


def inspect_private(annotate: bool) -> None:
    import pyghidra

    pyghidra.start(install_dir="/opt/homebrew/opt/ghidra/libexec")
    from ghidra.program.model.listing import CodeUnit
    from ghidra.program.model.symbol import SourceType
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import TaskMonitor

    project = pyghidra.open_project(str(PROJECT), "receiver")
    try:
        with pyghidra.program_context(project, PROGRAM) as program:
            af = program.getAddressFactory()
            fm = program.getFunctionManager()
            refs = program.getReferenceManager()
            listing = program.getListing()

            points = (
                0x0102760C,
                0x01028250,
                0x01028448,
                0x0103131C,
                0x01031444,
                0x010314F4,
                0x01032C18,
                0x01032D84,
                0x01032D98,
                0x010342DE,
                0x01034958,
                0x010349A4,
                0x01038998,
                0x0103899C,
                0x01052C28,
                0x01052CE8,
            )
            for value in points:
                address = af.getAddress(f"{value:x}")
                function = fm.getFunctionContaining(address)
                name = function.getName() if function else "<none>"
                entry = function.getEntryPoint() if function else "-"
                print(f"{address} containing={entry} {name}")
                for ref in refs.getReferencesTo(address):
                    print(f"  xref {ref.getFromAddress()} {ref.getReferenceType()}")

            if not annotate:
                return

            names = {
                0x0102760C: "aop_cpu_wfi",
                0x01028250: "aop_suspend_context_for_power_state",
                0x01028448: "aop_prepare_cache_for_power_state",
                0x0102C524: "aop_interrupt_controller_command",
                0x0102C66C: "aop_interrupt_dispatch",
                0x0102C72E: "aop_interrupt_set_enabled",
                0x0102C7D8: "aop_interrupt_register_handler",
                0x0102CA24: "aop_mailbox_command",
                0x0102D080: "aop_mailbox_data_irq",
                0x0102D0A8: "aop_mailbox_error_irq_0",
                0x0102D0B4: "aop_mailbox_error_irq_1",
                0x0103131C: "rtk_mgmt_rx",
                0x01031444: "rtk_iop_power_ack",
                0x010314F4: "rtk_power_decode",
                0x01032C18: "rtk_power_transition",
                0x01034958: "aop_power_trace_exit",
                0x010349A4: "aop_power_trace_enter",
            }
            comments = {
                0x01032D44: (
                    "All deep transitions mask CPSR IRQ+FIQ. State 8 does not "
                    "program a wake source here; it preserves peripheral/interrupt-controller state."
                ),
                0x01032D84: "State-8 entry sends the wire 0x201 acknowledgement before sleeping.",
                0x01032D98: (
                    "WFI exit is any already-enabled pending interrupt. The transition itself does "
                    "not enable mailbox-inbox-nonempty or select a dedicated wrapper wake bit."
                ),
                0x01032E0C: "Restore the pre-transition CPSR IRQ mask after WFI.",
                0x01032E02: "Restore the pre-transition CPSR FIQ mask after WFI.",
                0x01032E64: "After wake/callback restore, acknowledge the restored target (normally wire 0x20).",
                0x0102CA24: (
                    "Mailbox init registers the data IRQ stored at object+0x24. The AP-to-IOP "
                    "RX object at 0x01052c28 uses status/data/pop offsets 0x08/0x10/0x18 and "
                    "interrupt ID 0x00040001 (wrapper source bit 0); 0x00010045/46 are error IRQs."
                ),
                0x0102C72E: (
                    "Normal interrupt enable/disable handles groups 1/6/7, not group 4. "
                    "Mailbox data IRQ 0x00040001 is registered as a special group-4 source."
                ),
                0x0102D080: (
                    "AP-to-IOP mailbox data ISR: acknowledge the object's group-4 IRQ, then "
                    "wake the RTKit receive task."
                ),
            }
            tx = program.startTransaction("AOP resume round 2")
            try:
                for value, name in names.items():
                    function = fm.getFunctionAt(af.getAddress(f"{value:x}"))
                    if function is not None:
                        function.setName(name, SourceType.USER_DEFINED)
                for value, comment in comments.items():
                    listing.setComment(
                        af.getAddress(f"{value:x}"), CodeUnit.PLATE_COMMENT, comment
                    )
            finally:
                program.endTransaction(tx, True)
            OUT.mkdir(parents=True, exist_ok=True)
            decompiler = DecompInterface()
            decompiler.openProgram(program)
            try:
                for value in names:
                    function = fm.getFunctionAt(af.getAddress(f"{value:x}"))
                    if function is None:
                        continue
                    result = decompiler.decompileFunction(function, 60, TaskMonitor.DUMMY)
                    assert result.decompileCompleted(), result.getErrorMessage()
                    (OUT / f"fw-{value:08x}.c").write_text(
                        str(result.getDecompiledFunction().getC())
                    )
                    (OUT / f"fw-{value:08x}.asm").write_text(
                        "\n".join(
                            f"{insn.getAddress()} {insn}"
                            for insn in listing.getInstructions(function.getBody(), True)
                        )
                        + "\n"
                    )
            finally:
                decompiler.dispose()
            program.save("AOP resume round 2 annotations", TaskMonitor.DUMMY)
    finally:
        project.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotate", action="store_true")
    args = parser.parse_args()
    verify_artifacts()
    inspect_private(args.annotate)
    print(f"PASS AOP resume round 2 evidence: {OUT}")


if __name__ == "__main__":
    main()
