#!/usr/bin/env python3
"""Install the display round-2 sources in /src/wt/display, only on a9/display.

Moves the round-1 probe from drivers/video/ to drivers/gpu/drm/adbe/ (one
home for the A9 pipe, shared headers), adds the DRM driver, and updates the
binding text. The device tree is not changed.
"""
import pathlib
import shutil
import subprocess

tree = pathlib.Path.cwd()


def git(*args):
    subprocess.check_call(["git", *args], cwd=tree)


branch = subprocess.check_output(["git", "branch", "--show-current"], text=True).strip()
if branch != "a9/display":
    raise SystemExit(f"refusing to edit {branch!r}; expected a9/display")

source = pathlib.Path("/work/scripts/display/kernel")
adbe = tree / "drivers/gpu/drm/adbe"
old_probe = tree / "drivers/video/apple-adbe-probe.c"
if old_probe.exists():
    adbe.mkdir(exist_ok=True)
    git("mv", "drivers/video/apple-adbe-probe.c", "drivers/gpu/drm/adbe/adbe_probe.c")
    git("rm", "-q", "drivers/video/apple-adbe-probe.h")
for name in ("Kconfig", "Makefile", "adbe_addr.h", "adbe_hw.h", "adbe_probe.c", "adbe_drv.c"):
    shutil.copyfile(source / "adbe" / name, adbe / name)
shutil.copyfile(source / "adbe0,s8000.yaml",
                tree / "Documentation/devicetree/bindings/display/adbe0,s8000.yaml")


def drop(path, text):
    file = tree / path
    original = file.read_text()
    if text in original:
        file.write_text(original.replace(text, ""))


drop("drivers/video/Makefile",
     "\nobj-$(CONFIG_APPLE_ADBE_PROBE) += apple_adbe_probe.o\n"
     "apple_adbe_probe-y := apple-adbe-probe.o\n")
kconfig = tree / "drivers/video/Kconfig"
text = kconfig.read_text()
start = text.find("\n\nconfig APPLE_ADBE_PROBE\n")
if start >= 0:
    end = text.find("The module is called apple_adbe_probe.\n", start)
    if end < 0:
        raise SystemExit("unexpected round-1 Kconfig entry")
    kconfig.write_text(text[:start] + text[end + len("The module is called apple_adbe_probe.\n"):])


def insert_before(path, anchor, line):
    file = tree / path
    original = file.read_text()
    if line not in original:
        if anchor not in original:
            raise SystemExit(f"anchor missing in {path}")
        file.write_text(original.replace(anchor, line + anchor, 1))


# "adbe" sorts before "adp" in the sorted driver list.
insert_before("drivers/gpu/drm/Kconfig", 'source "drivers/gpu/drm/adp/Kconfig"\n',
              'source "drivers/gpu/drm/adbe/Kconfig"\n')
insert_before("drivers/gpu/drm/Makefile", "obj-$(CONFIG_DRM_ADP) += adp/\n",
              "obj-y += adbe/\n")
git("add", "-A", "drivers/gpu/drm/adbe", "drivers/gpu/drm/Kconfig",
    "drivers/gpu/drm/Makefile", "drivers/video/Kconfig", "drivers/video/Makefile",
    "Documentation/devicetree/bindings/display/adbe0,s8000.yaml")
