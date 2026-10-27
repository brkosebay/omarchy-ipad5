#!/usr/bin/env python3
"""Offline round-3 analysis of the pinned AOP SPI0 channel-3 reload state.

The main firmware is annotated in the long-lived private Ghidra project.  A
private combined main+extension program is also built so the j71t MPU6xxx
constructor and channel configuration can be named and exported reproducibly.
"""

import hashlib
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[3]
FW_PATH = ROOT / "build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin"
FW_SHA256 = "af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8"
EXT_PATH = ROOT / "build/periph/aop-stage3/root-firmware/ipad6faop_ext-20H350.bin"
EXT_SHA256 = "2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22"
PROJECT = ROOT / "build/periph/aop-attach/firmware"
PROGRAM = "/pinned-aop.elf"
OUT = ROOT / "build/periph/aop-stage3/re/spi0-ch3-round3"
COMBINED_PROJECT = OUT / "combined-ghidra"
COMBINED_PROGRAM = "/aop-round3-combined.elf"

TARGETS = {
    0x01005C38: "AOPSPIClient_initChannels",
    0x01005C74: "AOPSPIClient_syncTransfer",
    0x01005EC4: "AOPSPIClient_configureChannel",
    0x01005F18: "AOPSPIClient_stopAsyncTransfer",
    0x01005F70: "AOPSPIClient_enableAsyncTrigger",
    0x01005FA0: "AOPSPIClient_startAsyncTransfer",
    0x0100E8B6: "MPU6xxx_setStreamingEnabled",
    0x0100EE18: "MPU6xxx_startAsyncRead",
    0x0102BC10: "AOPGPIO_handleEvent",
    0x0102BCBC: "AOPGPIO_configurePin",
    0x0102E660: "AOPSPIMaster_handleEvent",
    0x0102E110: "AOPSPIMaster_prepareTransfer",
    0x0102E318: "AOPSPIMaster_submitTransfer",
    0x0102E4F8: "AOPSPIMaster_teardownChannel",
    0x0102EADC: "AOPSPIMaster_setBuffer",
    0x0102EB84: "AOPSPIMaster_setTransferWords",
    0x0102EC24: "AOPSPIMaster_setChannelDescriptor",
    0x0102EC50: "AOPSPIMaster_setChannelEnabledWrapper",
    0x0102EC5E: "AOPSPIMaster_setAsyncTriggerMode",
    0x0102ECA8: None,
    0x0102ECC6: None,
    0x0102ED36: None,
    0x0102ED68: None,
    0x0102ED94: None,
    0x0102EDE4: "AOPSPIMaster_setChannelCallback",
    0x0102EE68: "AOPSPIMaster_selectChannelIRQ",
    0x0102EE9C: "AOPSPIMaster_maskChannelIRQs",
    0x0102EEB8: None,
    0x0102EED0: None,
    0x0102EED8: None,
    0x0102EF2E: None,
    0x0102EF5E: "AOPSPIMaster_setControllerEnabled",
    0x0102EF7C: "AOPSPIMaster_setChannelEnabled",
    0x0102F0C8: "AOPSPIMaster_transferIRQ",
    0x0102F1B4: "AOPSPIMaster_channelIRQ",
    0x0102F20A: "AOPSPIMaster_channelFaultIRQ",
    0x0102F2AA: "AOPSPIMaster_start",
    0x0102F2F4: "AOPSPIMaster_reset",
    0x0102FB90: "AOPSystick_handleEvent",
}

EXT_TARGETS = {
    0x01072CAC: "MPU6xxx_start_j71t",
    0x01072D5E: "MPU6xxx_configureSPIChannels_j71t",
    0x01072DC4: "MPU6xxx_configureBuffers_j71t",
    0x01072E3C: "MPU6xxx_initializeRegisters_j71t",
    0x01072ED0: "MPU6xxx_construct_j71t",
}

COMMENTS = {
    0x01005F18: (
        "MPU async teardown uses the master teardown in this order: clear channel run "
        "and wait idle; mask both per-channel IRQ classes; clear channel+0x18 descriptor."
    ),
    0x01005F70: (
        "For the j71t MPU handle this enables async-trigger mode on SPI0 channel 3, then "
        "configures AOP GPIO pin 14 with command 0x807."
    ),
    0x0100E982: (
        "MPU disable path: stop helpers, write PWR_MGMT_2 (0x6c) |= 0x3f and "
        "PWR_MGMT_1 (0x6b) |= 0x40, then tear down async SPI channel 3."
    ),
    0x0102E50C: (
        "Channel teardown step 1 calls setChannelEnabled(false): reg0 &= ~0x411, "
        "then waits for read-only idle bit 1."
    ),
    0x0102E518: (
        "Channel teardown step 2 masks both callback IRQ classes: reg0 &= ~0x450, "
        "then reg0 &= ~0x510."
    ),
    0x0102E526: "Channel teardown step 3 writes zero to channel+0x18.",
    0x0102E7A4: (
        "Lifecycle event 1 enables SPI0 IRQs 0x10014, 0x10010, and 0x10012 while "
        "the newly zeroed channel-pointer table is still empty."
    ),
    0x0102E93A: (
        "Event 0x14 stores the newly zeroed channel object into table[channel] before "
        "any channel register write; a later channel IRQ is safe after this store."
    ),
    0x0102E972: (
        "Fresh channel init writes reg0=0x400. The j71t MPU configures channel 4 "
        "(commands) first and channel 3 (async FIFO/data) second."
    ),
    0x0102EC76: (
        "Async-trigger enable clears reg0 bits 2/4/10, controls channel+0xc bit 3, "
        "then sets reg0 bit 2."
    ),
    0x0102EE36: (
        "Callback type 0 controls reg0 bit 6 and clears bits 4/10; callback type 1 "
        "controls reg0 bit 8 and clears bits 4/10."
    ),
    0x0102EF9E: (
        "Channel disable writes reg0 &= ~0x411 (bits 0/4/10) and waits until bit 1 "
        "reports idle; enable writes (reg0 & ~0x411) | 1."
    ),
    0x0102F22E: (
        "Fault-class aggregate status is SPI0+0x8d0 bit (2*channel). For channel 3 "
        "that is bit 6 and AOP interrupt ID 0x00010012."
    ),
    0x0102F244: (
        "Hardware acknowledge/mask prefix: reg0 &= ~0x450, then channel+0xc |= 0x20. "
        "Only after these writes does the handler dereference channel_table[channel]."
    ),
    0x0102F252: (
        "Warm-reload crash: channel 3 status is pending while table[3] is NULL; the "
        "LDRB at NULL+0x4c faults before firmware can allocate the MPU channel."
    ),
    0x0102FC04: (
        "SPI0 reset provider transition 3 is systick-only: lock, write 0x0000000c to "
        "AOP VA 0x1013c000 (AP PA 0x21013c000), unlock. It cannot quiet SPI0/GPIO/MPU."
    ),
}

EXT_COMMENTS = {
    0x01072D8A: (
        "Configure MPU synchronous command channel 4 first. The common constructor's "
        "fifth argument is routing/CS selector 4."
    ),
    0x01072D92: (
        "Configure MPU asynchronous FIFO/data channel 3 second, then enable its "
        "async trigger and GPIO route."
    ),
    0x01072EEA: (
        "j71t MPU6xxx SPI wiring: command channel 4, async FIFO/data channel 3, "
        "AOP GPIO pin 14, routing/CS selector 4."
    ),
}


def verify_firmware():
    raw = FW_PATH.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != FW_SHA256:
        raise SystemExit(f"refusing unpinned firmware: {digest}")
    ext = EXT_PATH.read_bytes()
    ext_digest = hashlib.sha256(ext).hexdigest()
    if ext_digest != EXT_SHA256:
        raise SystemExit(f"refusing unpinned extension: {ext_digest}")
    return raw, ext


def make_combined_elf(raw, ext):
    """Create a minimal ELF mapping main+BSS-gap+extension at 0x01000000."""
    image = raw.ljust(0x6E000, b"\0") + ext
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    eh = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident,
        2,
        40,
        1,
        0x0107206D,
        52,
        0,
        0x05000000,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    ph = struct.pack(
        "<IIIIIIII", 1, 0x1000, 0x01000000, 0x01000000, len(image), len(image), 7, 0x1000
    )
    path = OUT / "aop-round3-combined.elf"
    path.write_bytes((eh + ph).ljust(0x1000, b"\0") + image)
    return path, image


def raw_refs(raw, target):
    needle = struct.pack("<I", target)
    refs = []
    start = 0
    while True:
        offset = raw.find(needle, start)
        if offset < 0:
            return refs
        refs.append(0x01000000 + offset)
        start = offset + 1


def main():
    raw, ext = verify_firmware()
    OUT.mkdir(parents=True, exist_ok=True)
    combined_elf, image = make_combined_elf(raw, ext)

    import pyghidra

    pyghidra.start(install_dir="/opt/homebrew/opt/ghidra/libexec")
    from java.math import BigInteger
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.model.listing import CodeUnit
    from ghidra.program.model.symbol import SourceType
    from ghidra.util.task import TaskMonitor

    project = pyghidra.open_project(str(PROJECT), "receiver")
    inventory = {
        "firmware_sha256": FW_SHA256,
        "extension_sha256": EXT_SHA256,
        "channel_3": {
            "device": "MPU6xxx/MPU6900 async FIFO/data path",
            "command_channel": 4,
            "async_channel": 3,
            "gpio_pin": 14,
            "routing_or_chip_select": 4,
            "fault_irq": "0x00010012",
            "fault_status": "SPI0+0x8d0 bit 6",
        },
        "raw_refs": {
            f"0x{target:08x}": [f"0x{ref:08x}" for ref in raw_refs(raw, target)]
            for target in (0x01047E78, 0x01039C6C, 0x0102E661, 0x0102E660)
        },
        "functions": {},
        "ghidra_refs": {},
    }
    try:
        with pyghidra.program_context(project, PROGRAM) as program:
            af = program.getAddressFactory()
            fm = program.getFunctionManager()
            listing = program.getListing()
            refs = program.getReferenceManager()
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                addresses = set(TARGETS)
                for probe in (0x01005C70, 0x01047E78, 0x01039C6C):
                    address = af.getAddress(f"{probe:x}")
                    function = fm.getFunctionContaining(address)
                    if function is not None:
                        addresses.add(function.getEntryPoint().getOffset())
                        inventory["functions"][f"probe_0x{probe:08x}"] = {
                            "entry": str(function.getEntryPoint()),
                            "name": function.getName(),
                        }

                tx = program.startTransaction("AOP SPI0 channel-3 round-3 function recovery")
                try:
                    for entry, name in TARGETS.items():
                        address = af.getAddress(f"{entry:x}")
                        DisassembleCommand(address, None, True).applyTo(program)
                        if fm.getFunctionAt(address) is None:
                            if not CreateFunctionCmd(address).applyTo(program):
                                raise RuntimeError(f"could not create function at 0x{entry:08x}")
                        function = fm.getFunctionAt(address)
                        CreateFunctionCmd.fixupFunctionBody(program, function, TaskMonitor.DUMMY)
                        if name is not None:
                            function.setName(name, SourceType.USER_DEFINED)
                    for address, comment in COMMENTS.items():
                        listing.setComment(
                            af.getAddress(f"{address:x}"), CodeUnit.PLATE_COMMENT, comment
                        )
                finally:
                    program.endTransaction(tx, True)

                for target in (0x01005C38, 0x01005C74, 0x01047E78, 0x01039C6C, 0x0102E660):
                    address = af.getAddress(f"{target:x}")
                    inventory["ghidra_refs"][f"0x{target:08x}"] = [
                        {
                            "from": str(ref.getFromAddress()),
                            "type": str(ref.getReferenceType()),
                        }
                        for ref in refs.getReferencesTo(address)
                    ]

                for entry in sorted(addresses):
                    function = fm.getFunctionAt(af.getAddress(f"{entry:x}"))
                    if function is None:
                        continue
                    result = dec.decompileFunction(function, 120, TaskMonitor.DUMMY)
                    if result.decompileCompleted():
                        (OUT / f"fw-{entry:08x}.c").write_text(
                            str(result.getDecompiledFunction().getC())
                        )
                    (OUT / f"fw-{entry:08x}.asm").write_text(
                        "\n".join(
                            f"{ins.getAddress()} {ins}"
                            for ins in listing.getInstructions(function.getBody(), True)
                        )
                        + "\n"
                    )
                    inventory["functions"][f"0x{entry:08x}"] = function.getName()

                tx = program.startTransaction("AOP SPI0 channel-3 object evidence")
                try:
                    listing.setComment(
                        af.getAddress("01005c70"),
                        CodeUnit.EOL_COMMENT,
                        "Static device registry references the spi0 controller object at 0x01047e78.",
                    )
                finally:
                    program.endTransaction(tx, True)
                program.save("AOP SPI0 channel-3 round-3 annotations", TaskMonitor.DUMMY)
            finally:
                dec.dispose()
    finally:
        project.close()

    COMBINED_PROJECT.mkdir(parents=True, exist_ok=True)
    combined_project = pyghidra.open_project(
        str(COMBINED_PROJECT), "aop-round3", create=True
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
            fm = program.getFunctionManager()
            listing = program.getListing()
            tx = program.startTransaction("j71t MPU6xxx SPI0 channel map")
            try:
                if fresh:
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
                for entry, name in EXT_TARGETS.items():
                    address = af.getAddress(f"{entry:x}")
                    DisassembleCommand(address, None, True).applyTo(program)
                    if fm.getFunctionAt(address) is None:
                        if not CreateFunctionCmd(address).applyTo(program):
                            raise RuntimeError(
                                f"could not create extension function at 0x{entry:08x}"
                            )
                    function = fm.getFunctionAt(address)
                    CreateFunctionCmd.fixupFunctionBody(program, function, TaskMonitor.DUMMY)
                    function.setName(name, SourceType.USER_DEFINED)
                for address, comment in EXT_COMMENTS.items():
                    listing.setComment(
                        af.getAddress(f"{address:x}"), CodeUnit.PLATE_COMMENT, comment
                    )
            finally:
                program.endTransaction(tx, True)

            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for entry, name in EXT_TARGETS.items():
                    function = fm.getFunctionAt(af.getAddress(f"{entry:x}"))
                    result = dec.decompileFunction(function, 120, TaskMonitor.DUMMY)
                    if not result.decompileCompleted():
                        raise RuntimeError(result.getErrorMessage())
                    (OUT / f"ext-{entry:08x}.c").write_text(
                        str(result.getDecompiledFunction().getC())
                    )
                    (OUT / f"ext-{entry:08x}.asm").write_text(
                        "\n".join(
                            f"{ins.getAddress()} {ins}"
                            for ins in listing.getInstructions(function.getBody(), True)
                        )
                        + "\n"
                    )
                    inventory["functions"][f"0x{entry:08x}"] = name
            finally:
                dec.dispose()
            program.save("j71t MPU6xxx SPI0 channel map", TaskMonitor.DUMMY)
    finally:
        combined_project.close()

    (OUT / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print(f"PASS pinned AOP SPI0 round-3 export: {OUT}")


if __name__ == "__main__":
    main()
