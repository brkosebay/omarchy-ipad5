#!/usr/bin/env python3
"""Install the D1/D2 sources in /src/wt/display, only on a9/display."""
import pathlib
import shutil
import subprocess

tree = pathlib.Path.cwd()
branch = subprocess.check_output(["git", "branch", "--show-current"], text=True).strip()
if branch != "a9/display":
    raise SystemExit(f"refusing to edit {branch!r}; expected a9/display")
source = pathlib.Path("/work/scripts/display/kernel")
for name in ("apple-adbe-probe.c", "apple-adbe-probe.h"):
    shutil.copyfile(source / name, tree / "drivers/video" / name)
shutil.copyfile(source / "adbe0,s8000.yaml",
                tree / "Documentation/devicetree/bindings/display/adbe0,s8000.yaml")
shutil.copyfile(source / "s8003-j71t-display.dtsi",
                tree / "arch/arm64/boot/dts/apple/s8003-j71t-display.dtsi")


def append_once(path, marker, text):
    file = tree / path
    original = file.read_text()
    if marker not in original:
        file.write_text(original + text)


append_once("drivers/video/Makefile", "CONFIG_APPLE_ADBE_PROBE",
            "\nobj-$(CONFIG_APPLE_ADBE_PROBE) += apple_adbe_probe.o\n"
            "apple_adbe_probe-y := apple-adbe-probe.o\n")
append_once("drivers/video/Kconfig", "config APPLE_ADBE_PROBE", """

config APPLE_ADBE_PROBE
	tristate "Apple A9 boot display probe and reversible flip experiment"
	depends on ARM64 && OF && DEBUG_FS
	help
	  Observe the A9 iBoot display without taking over simpledrm. Probe
	  reads registers and polls the frame counter. An optional parameter
	  requests one IRQ briefly, acknowledging its observed status only.
	  A debugfs write can run one auto-reverting scanout experiment using
	  reserved boot VRAM with operator-verified existing DART coverage.
	  No link training, power management or IOMMU programming is done.
	  If unsure, say N. The module is called apple_adbe_probe.
""")
board = tree / "arch/arm64/boot/dts/apple/s8003-j71t.dts"
original = board.read_text().replace('#include "s8003-j71t-display.dtsi"\n', "")
if "#define APPLE_J71T_BOOT_DISPLAY" not in original:
    original = original.replace("/dts-v1/;", "#define APPLE_J71T_BOOT_DISPLAY\n\n/dts-v1/;")
board.write_text(original)
soc = tree / "arch/arm64/boot/dts/apple/s8003.dtsi"
original = soc.read_text()
if "#ifdef APPLE_J71T_BOOT_DISPLAY" not in original:
    soc.write_text(original.replace('#include "s800-0-3.dtsi"',
                                    '#include "s800-0-3.dtsi"\n\n'
                                    '#ifdef APPLE_J71T_BOOT_DISPLAY\n'
                                    '#include "s8003-j71t-display.dtsi"\n'
                                    '#endif'))
