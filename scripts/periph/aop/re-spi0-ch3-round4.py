#!/usr/bin/env python3
"""Pinned offline RE for the round-4 AOP SPI0 channel-3 reload failure.

This script never contacts the iPad.  It verifies the exact iOS 16.7.10 j71t
AOP images and retained reload evidence, exports the controlled SCM panic path,
resolves every saved call-stack frame, and annotates the private Ghidra projects.
Generated evidence is written below build/periph/aop-stage3/re/ and is ignored.
"""

import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[3]
FW_PATH = ROOT / "build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin"
FW_SHA256 = "af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8"
EXT_PATH = ROOT / "build/periph/aop-stage3/root-firmware/ipad6faop_ext-20H350.bin"
EXT_SHA256 = "2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22"
CRASH_PATH = ROOT / "docs/periph/logs/aop-ch3tdp-20261026/reload-crashlog-decoded.txt"
CRASH_SHA256 = "663e8cf5bf1093b3bf9e9ca92e4b9afd3854fc2231ddd13e09d7e9999eeaae30"
DMESG_PATH = ROOT / "docs/periph/logs/aop-ch3tdp-20261026/reload-trigger-dmesg.txt"
DMESG_SHA256 = "e6f121d9c9377ac2963ef6c1cf111749d459e5c454b59fc2ce0514e19aef3a44"
COLD_SRAM_PATH = (
    ROOT
    / "build/periph/aop-stage23/handoff-full-cold-20261026T122838Z/cold_after.bin"
)
COLD_SRAM_SHA256 = "1653ba275a97103c4efcb45f591e0bf2f658617bf250b20330bf68d4222c6e43"

PROJECT = ROOT / "build/periph/aop-attach/firmware"
PROGRAM = "/pinned-aop.elf"
OUT = ROOT / "build/periph/aop-stage3/re/spi0-ch3-round4"
COMBINED_PROJECT = OUT / "combined-ghidra-v2"
COMBINED_PROGRAM = "/aop-round4-combined.elf"

STACK_FRAMES = (
    0x01033834,
    0x010333C6,
    0x01033236,
    0x01034A64,
    0x0102F1A2,
    0x0102C6F8,
    0x010272F8,
    0x010005BC,
    0x01074084,
    0x01072382,
    0x01073C96,
    0x01033B70,
    0x01029EE0,
)

# Exact Thumb function entries established in round 3 or by direct calls in the
# round-4 path.  Unknown helpers are deliberately left unnamed on the first run;
# MAIN_NAMES below is the reviewable annotation set.
MAIN_TARGETS = (
    0x010005A0,
    0x010272EC,
    0x01029EDC,
    0x0102C524,
    0x0102C66C,
    0x0102C72E,
    0x0102C7D8,
    0x0102C972,
    0x0102E110,
    0x0102E1FC,
    0x0102E218,
    0x0102E260,
    0x0102E2C4,
    0x0102E2E0,
    0x0102E318,
    0x0102E4F8,
    0x0102E650,
    0x0102E660,
    0x0102EADC,
    0x0102EB84,
    0x0102EC24,
    0x0102EC50,
    0x0102EC5E,
    0x0102ECA8,
    0x0102ECC6,
    0x0102ED36,
    0x0102ED68,
    0x0102ED94,
    0x0102EDE4,
    0x0102EE68,
    0x0102EE9C,
    0x0102EEB8,
    0x0102EED0,
    0x0102EED8,
    0x0102EF2E,
    0x0102EF5E,
    0x0102EF7C,
    0x0102F018,
    0x0102F0C8,
    0x0102F1B4,
    0x0102F20A,
    0x0102F2AA,
    0x0102F2F4,
    0x0103310C,
    0x01033324,
    0x01033354,
    0x010337BC,
    0x01033A8C,
    0x01034A2C,
    0x01034A44,
)

EXT_TARGETS = (
    0x01070CF0,
    0x01070D32,
    0x01070D7C,
    0x01070D9C,
    0x01072CAC,
    0x01072D5E,
    0x01072DC4,
    0x01072E3C,
    0x01072ED0,
    0x01072348,
    0x01073C84,
    0x0107402C,
)

MAIN_NAMES = {
    0x010005A0: "aop_irq_exception_wrapper",
    0x010272EC: "aop_irq_frontend",
    0x01029EDC: "aop_task_trampoline",
    0x0102C524: "aop_interrupt_controller_command",
    0x0102C66C: "aop_interrupt_dispatch",
    0x0102C72E: "aop_interrupt_set_enabled",
    0x0102C7D8: "aop_interrupt_register_handler",
    0x0102C972: "aop_interrupt_write_enabled",
    0x0102E660: "AOPSPIMaster_handleEvent",
    0x0102E110: "AOPSPIMaster_prepareTransfer",
    0x0102E318: "AOPSPIMaster_submitTransfer",
    0x0102E4F8: "AOPSPIMaster_teardownChannel",
    0x0102EADC: "AOPSPIMaster_setBuffer",
    0x0102EB84: "AOPSPIMaster_setTransferWords",
    0x0102EC24: "AOPSPIMaster_setChannelDescriptor",
    0x0102EC50: "AOPSPIMaster_setChannelEnabledWrapper",
    0x0102EC5E: "AOPSPIMaster_setAsyncTriggerMode",
    0x0102EDE4: "AOPSPIMaster_setChannelCallback",
    0x0102EE68: "AOPSPIMaster_selectChannelIRQ",
    0x0102EE9C: "AOPSPIMaster_maskChannelIRQs",
    0x0102EEB8: "AOPSPIMaster_readSCMState",
    0x0102EF5E: "AOPSPIMaster_setControllerEnabled",
    0x0102EF7C: "AOPSPIMaster_setChannelEnabled",
    0x0102F0C8: "AOPSPIMaster_transferIRQ",
    0x0102F1B4: "AOPSPIMaster_channelIRQ",
    0x0102F20A: "AOPSPIMaster_channelFaultIRQ",
    0x0102F2AA: "AOPSPIMaster_start",
    0x0102F2F4: "AOPSPIMaster_reset",
    0x0103310C: "aop_record_fatal",
    0x01033324: "aop_fatal_callback_once",
    0x01033354: "aop_crashlog_serialize_sections",
    0x010337BC: "aop_crashlog_write_callstack_section",
    0x01033A8C: "rtk_ep_work_task",
    0x01034A2C: "aop_fatal_stop",
    0x01034A44: "aop_panic_printf",
}

EXT_NAMES = {
    0x01072348: "j71t_platform_start",
    0x01072CAC: "MPU6xxx_start_j71t",
    0x01072D5E: "MPU6xxx_configureSPIChannels_j71t",
    0x01072DC4: "MPU6xxx_configureBuffers_j71t",
    0x01072E3C: "MPU6xxx_initializeRegisters_j71t",
    0x01072ED0: "MPU6xxx_construct_j71t",
    0x01073C84: "ext_work_callback_invoke_and_free",
    0x0107402C: "j71t_start_private_peripherals",
}

MAIN_COMMENTS = {
    0x010005BC: (
        "ARM IRQ exception wrapper calls the Thumb IRQ front end through the "
        "0x010352cc veneer. Ccst frames after this address are the interrupted task."
    ),
    0x0102C58C: (
        "Interrupt-controller lifecycle event 3 resets the mapped AOP interrupt "
        "controller: write 1/read/write 0/read at base+0x808. Candidate warm-reload "
        "fix uses this exact firmware sequence while the old AOP CPU is halted."
    ),
    0x0102C6F8: (
        "IRQ dispatcher calls the registered handler. In the controlled failure this "
        "is AOPSPIMaster_transferIRQ for source 0x10014."
    ),
    0x0102C6EE: (
        "For a group-1 source configured for pre-handler clear, write its source bit "
        "to interrupt-controller base+0x980+4*bank. IRQ 0x10014 maps to +0x980, "
        "bit 20 (0x00100000)."
    ),
    0x0102C9CE: (
        "Write-one group-1 enable control. With enabled=false, IRQ 0x10014 maps to "
        "interrupt-controller base+0xa00, bit 20 (0x00100000); enabled=true uses +0xa80."
    ),
    0x0102F0EE: (
        "Read the 64-bit SCM state through vtable +0x40 (AOPSPIMaster_readSCMState): "
        "SPI0+0x0c is low, SPI0+0x10 is high. Low bits 0/1 classify the event."
    ),
    0x0102F11A: (
        "Select the channel whose hardware +0x110/+0x114 interval contains the SCM "
        "position. The controlled round-4 panic selected channel 3."
    ),
    0x0102F190: (
        "No registered controller callback handled the transfer event: read SCM state "
        "again and panic with SCMErr:0x%llx,%u (state, selected channel)."
    ),
    0x0102F1A2: (
        "Round-4 controlled panic site. Crash Cstr is SCMErr:0x3,3: SCM state 3 and "
        "selected channel 3. This is the global transfer IRQ, not +0x8d0 fault IRQ."
    ),
    0x0102EEB8: (
        "Return the controller's 64-bit SCM state from SPI0+0x0c (low) and +0x10 (high)."
    ),
    0x0102E972: (
        "Fresh channel event 0x14 writes Cn=0x400 before route/rate/config programming; "
        "it does not explicitly clear the hardware interval words at +0x110/+0x114."
    ),
}

EXT_COMMENTS = {
    0x01072382: (
        "Board startup calls j71t_start_private_peripherals. The transfer IRQ "
        "preempts this call chain before MPU endpoint/channel construction."
    ),
    0x01074084: (
        "Call AOPSPIMaster lifecycle event 1 on object 0x01047e78. The controlled "
        "SCMErr interrupt lands here, before the master's event-5 reset and before "
        "MPU6xxx_configureSPIChannels_j71t."
    ),
}

# Ghidra follows three Thumb tail calls into their targets when it discovers
# these extension functions without symbols.  Constrain the bodies to the
# exact entry-to-final-instruction ranges so exports and saved analysis do not
# absorb the tail-call targets.
BODY_RANGES = {
    0x01029EDC: 0x01029EF9,
    0x01072348: 0x01072493,
    0x01072D5E: 0x01072DA9,
    0x0107402C: 0x010740C7,
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_inputs():
    expected = {
        FW_PATH: FW_SHA256,
        EXT_PATH: EXT_SHA256,
        CRASH_PATH: CRASH_SHA256,
        DMESG_PATH: DMESG_SHA256,
        COLD_SRAM_PATH: COLD_SRAM_SHA256,
    }
    for path, digest in expected.items():
        actual = sha256(path)
        if actual != digest:
            raise SystemExit(f"refusing unpinned input {path}: {actual}")

    crash = CRASH_PATH.read_text()
    ccst = crash.split("tag=Ccst", 1)[1].split("section off=", 1)[0]
    saved = tuple(int(value, 16) for value in re.findall(
        r"0x(010[0-9a-f]{5})\s+<- firmware address", ccst
    ))
    if saved != STACK_FRAMES:
        raise SystemExit(f"unexpected Ccst frames: {[hex(value) for value in saved]}")
    if "0x454d4353" not in crash or "0x303a7272" not in crash or "0x332c3378" not in crash:
        raise SystemExit("decoded crash no longer contains SCMErr:0x3,3")

    dmesg = DMESG_PATH.read_text()
    required = (
        "SPI0 CH3TD after c3=00010406 route=00000010 rate=019000c8 "
        "c3c=00200009 desc=00000000 8d0=00000000 8d4=00000000",
        "SPI0 CH3TD window errno=0 samples=31/31 elapsed_ms=182 min_ms=150 result=pass",
        "stage3 T 0050002800000002 status=-110 phase=wait-epmap",
        "53 43 4d 45 72 72 3a 30 78 33 2c 33",
    )
    for line in required:
        if line not in dmesg:
            raise SystemExit(f"missing device evidence: {line}")

    cold = COLD_SRAM_PATH.read_bytes()
    intc_object = struct.unpack_from("<6I", cold, 0x52DA8)
    expected_intc_object = (
        0x01039D48,
        0x00000000,
        0xC1800000,
        0x00000002,
        0x00020000,
        0x00000001,
    )
    if intc_object != expected_intc_object:
        raise SystemExit(
            "unexpected live-cold interrupt-controller object: "
            + " ".join(f"{value:08x}" for value in intc_object)
        )
    if struct.unpack_from("<2I", cold, 0x47E78 + 0x78) != (0, 0):
        raise SystemExit("cold SPI0 master unexpectedly has a controller callback")


def make_combined_elf(raw, ext):
    image = raw.ljust(0x6E000, b"\0") + ext
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    eh = struct.pack(
        "<16sHHIIIIIHHHHHH", ident, 2, 40, 1, 0x0107206D, 52, 0,
        0x05000000, 52, 32, 1, 0, 0, 0,
    )
    ph = struct.pack(
        "<IIIIIIII", 1, 0x1000, 0x01000000, 0x01000000,
        len(image), len(image), 7, 0x1000,
    )
    path = OUT / "aop-round4-combined.elf"
    path.write_bytes((eh + ph).ljust(0x1000, b"\0") + image)
    return path, image


def raw_refs(image, target, base=0x01000000):
    needle = struct.pack("<I", target)
    result = []
    offset = 0
    while True:
        offset = image.find(needle, offset)
        if offset < 0:
            return result
        result.append(base + offset)
        offset += 1


def safe_name(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def export_function(program, function, decompiler, prefix, inventory):
    from ghidra.util.task import TaskMonitor

    listing = program.getListing()
    entry = function.getEntryPoint().getOffset()
    name = function.getName()
    stem = f"{prefix}-{entry:08x}-{safe_name(name)}"
    result = decompiler.decompileFunction(function, 120, TaskMonitor.DUMMY)
    if result.decompileCompleted():
        (OUT / f"{stem}.c").write_text(str(result.getDecompiledFunction().getC()))
    (OUT / f"{stem}.asm").write_text(
        "\n".join(
            f"{ins.getAddress()} {ins}"
            for ins in listing.getInstructions(function.getBody(), True)
        ) + "\n"
    )
    inventory[f"0x{entry:08x}"] = {
        "name": name,
        "entry": f"0x{entry:08x}",
        "body_min": str(function.getBody().getMinAddress()),
        "body_max": str(function.getBody().getMaxAddress()),
    }


def analyze_program(program, targets, frames, names, comments, prefix, inventory):
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.model.address import AddressSet
    from ghidra.program.model.listing import CodeUnit
    from ghidra.program.model.symbol import SourceType
    from ghidra.util.task import TaskMonitor

    af = program.getAddressFactory()
    fm = program.getFunctionManager()
    listing = program.getListing()

    tx = program.startTransaction(f"AOP SPI0 channel-3 round-4 {prefix} annotations")
    try:
        # Earlier symbol-less discovery split the four-byte task prologue from
        # its BLX body.  Merge that one understood function before annotating.
        if 0x01029EDC in targets:
            split = fm.getFunctionAt(af.getAddress("01029ee0"))
            if split is not None:
                fm.removeFunction(split.getEntryPoint())
        for entry in targets:
            address = af.getAddress(f"{entry:x}")
            if fm.getFunctionContaining(address) is None:
                DisassembleCommand(address, None, True).applyTo(program)
                CreateFunctionCmd(address).applyTo(program)
            function = fm.getFunctionAt(address)
            if function is not None:
                if entry in BODY_RANGES:
                    function.setBody(
                        AddressSet(address, af.getAddress(f"{BODY_RANGES[entry]:x}"))
                    )
                if entry in names:
                    function.setName(names[entry], SourceType.USER_DEFINED)
        for address, comment in comments.items():
            listing.setComment(
                af.getAddress(f"{address:x}"), CodeUnit.PLATE_COMMENT, comment
            )
    finally:
        program.endTransaction(tx, True)

    functions = {}
    frame_map = {}
    for frame in frames:
        function = fm.getFunctionContaining(af.getAddress(f"{frame:x}"))
        frame_map[f"0x{frame:08x}"] = None if function is None else {
            "entry": f"0x{function.getEntryPoint().getOffset():08x}",
            "name": function.getName(),
            "offset": frame - function.getEntryPoint().getOffset(),
        }
        if function is not None:
            functions[function.getEntryPoint().getOffset()] = function
    for entry in targets:
        function = fm.getFunctionAt(af.getAddress(f"{entry:x}"))
        if function is not None:
            functions[entry] = function

    dec = DecompInterface()
    dec.openProgram(program)
    try:
        for _, function in sorted(functions.items()):
            export_function(program, function, dec, prefix, inventory["functions"])
    finally:
        dec.dispose()
    inventory["frames"].update(frame_map)
    program.save(f"AOP SPI0 channel-3 round-4 {prefix} annotations", TaskMonitor.DUMMY)


def main():
    verify_inputs()
    raw = FW_PATH.read_bytes()
    ext = EXT_PATH.read_bytes()
    OUT.mkdir(parents=True, exist_ok=True)
    combined_elf, image = make_combined_elf(raw, ext)

    inventory = {
        "inputs": {
            "firmware_sha256": FW_SHA256,
            "extension_sha256": EXT_SHA256,
            "crash_sha256": CRASH_SHA256,
            "dmesg_sha256": DMESG_SHA256,
            "cold_sram_sha256": COLD_SRAM_SHA256,
        },
        "observed": {
            "cstr": "SCMErr:0x3,3",
            "post_teardown": {
                "C3": "0x00010406",
                "C3_ROUTE": "0x00000010",
                "C3_RATE": "0x019000c8",
                "C3C": "0x00200009",
                "C3_DESC": "0x00000000",
                "SPI0_8D0": "0x00000000",
                "SPI0_8D4": "0x00000000",
            },
        },
        "decoded_scmerr": {
            "scm_state_u64": "0x0000000000000003",
            "selected_channel": 3,
            "state_low_bit_0": "set; selects the first transfer-event token",
            "state_low_bit_1": "set; selects callback event kind 2",
            "state_position_after_clearing_low_bits": "0x0000000000000000",
            "not_error_code": True,
        },
        "interrupt_controller": {
            "runtime_object": "0x01052da8",
            "aop_mmio_base": "0xc1800000",
            "ap_physical_base": "0x210800000",
            "recommended_targeted_sequence": [
                {
                    "address": "0x210800a00",
                    "value": "0x00100000",
                    "meaning": "disable IRQ 0x10014",
                    "pc": "0x0102c9ce",
                },
                {
                    "address": "0x210800980",
                    "value": "0x00100000",
                    "meaning": "group-1 pre-handler acknowledge/clear for IRQ 0x10014 after SPI teardown",
                    "pc": "0x0102c6ee",
                },
            ],
            "event_3_full_reset_fallback": [
                {"address": "0x210800808", "value": "0x00000001", "pc": "0x0102c592"},
                {"address": "0x210800808", "operation": "read", "pc": "0x0102c598"},
                {"address": "0x210800808", "value": "0x00000000", "pc": "0x0102c59c"},
                {"address": "0x210800808", "operation": "read", "pc": "0x0102c5a0"},
            ],
            "full_reset_required_final_readback": "0x00000000",
            "command_register_readback": (
                "not a pass criterion; +0xa00/+0x980 are write-one command banks"
            ),
        },
        "scm_identifiers": {
            "source_file_strings": ["SpuScm.cpp", "BMP284_scm.cpp"],
            "target_string": "Unknown AOP Target - Unable to select phosphorus scm channel",
            "exact_acronym_expansion_present": False,
        },
        "raw_refs": {
            f"0x{target:08x}": [f"0x{ref:08x}" for ref in raw_refs(image, target)]
            for target in (
                0x010398DA,  # SCMto:%u
                0x010398E3,  # SCMErr:0x%llx,%u
                0x0102EEB9,  # vtable read-SCM-state method
                0x0102F0C9,  # transfer IRQ
                0x0102F1B5,  # per-channel class-1 IRQ
                0x0102F20B,  # per-channel class-0/fault IRQ
            )
        },
        "vtable_0x01039c6c": [
            f"0x{value:08x}" for value in struct.unpack_from("<20I", raw, 0x39C6C)
        ],
        "functions": {},
        "frames": {},
    }

    import pyghidra

    pyghidra.start(install_dir="/opt/homebrew/opt/ghidra/libexec")
    from java.math import BigInteger
    from ghidra.util.task import TaskMonitor

    project = pyghidra.open_project(str(PROJECT), "receiver")
    try:
        with pyghidra.program_context(project, PROGRAM) as program:
            analyze_program(
                program,
                MAIN_TARGETS,
                tuple(frame for frame in STACK_FRAMES if frame < 0x0106E000),
                MAIN_NAMES,
                MAIN_COMMENTS,
                "main",
                inventory,
            )
    finally:
        project.close()

    COMBINED_PROJECT.mkdir(parents=True, exist_ok=True)
    combined_project = pyghidra.open_project(
        str(COMBINED_PROJECT), "aop-round4", create=True
    )
    try:
        program_path = COMBINED_PROGRAM
        fresh = combined_project.getProjectData().getFile(program_path) is None
        if fresh:
            loaded = (
                pyghidra.program_loader()
                .project(combined_project)
                .source(str(combined_elf))
                .language("ARM:LE:32:v8")
                .compiler("default")
                .load()
            )
            try:
                program_path = str(
                    loaded.getPrimary().save(TaskMonitor.DUMMY).getPathname()
                )
            finally:
                loaded.close()
        with pyghidra.program_context(combined_project, program_path) as program:
            af = program.getAddressFactory()
            if fresh:
                tx = program.startTransaction("Set AOP Thumb regions")
                try:
                    context = program.getProgramContext()
                    context.setValue(
                        program.getRegister("TMode"),
                        af.getAddress("010004e0"),
                        af.getAddress("010389b7"),
                        BigInteger.ONE,
                    )
                    context.setValue(
                        program.getRegister("TMode"),
                        af.getAddress("0106e000"),
                        af.getAddress(f"{0x01000000 + len(image) - 1:x}"),
                        BigInteger.ONE,
                    )
                finally:
                    program.endTransaction(tx, True)
            analyze_program(
                program,
                EXT_TARGETS,
                tuple(frame for frame in STACK_FRAMES if frame >= 0x0106E000),
                EXT_NAMES,
                EXT_COMMENTS,
                "ext",
                inventory,
            )
    finally:
        combined_project.close()

    # The receiver project does not define the ARM exception-vector region as a
    # normal function, so preserve the one mixed-mode frame explicitly.  This is
    # verified independently from the pinned bytes at 0x010005a0..0x010005d4.
    inventory["frames"]["0x010005bc"] = {
        "entry": "0x010005a0",
        "name": "aop_irq_exception_wrapper",
        "offset": 0x1C,
        "manual_mixed_arm_thumb_boundary": True,
    }
    (OUT / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print(f"PASS pinned AOP SPI0 channel-3 round-4 export: {OUT}")


if __name__ == "__main__":
    main()
