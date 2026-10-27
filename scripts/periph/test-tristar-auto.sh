#!/usr/bin/env bash
# Offline: run the Lightning accessory policy (the kernel's own
# drivers/power/supply/apple_tristar_auto.h, branch a9/usb-host5) through
# scripts/periph/test-tristar-auto.c in the build container, then mutate the
# header eight ways and require the harness to catch each one. No device access.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TREE="${TREE:-usb-host5}"
scripts/build-kernel.sh --run "$TREE" '
set -euo pipefail
test "$(git branch --show-current)" = "a9/$TREE"
out=$(mktemp -d)
trap "rm -rf \"$out\"" EXIT
clang -std=gnu11 -Wall -Wextra -Werror -I drivers/power/supply \
    -o "$out/test-tristar-auto" /work/scripts/periph/test-tristar-auto.c
"$out/test-tristar-auto"
python3 -B -I - "$out" <<"PY"
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1])
src = pathlib.Path("drivers/power/supply/apple_tristar_auto.h").read_text()
mutations = {
    "no device role at unplug": (
        "\t\tif (release) {\n\t\t\tta->detaches++;\n\t\t\treturn TRISTAR_AUTO_TO_DEVICE;\n\t\t}",
        "\t\tif (release) {\n\t\t\tta->detaches++;\n\t\t}"),
    "never 2100 mA": ("ta->ph_ok ? cfg->input_ua : cfg->floor_ua", "cfg->floor_ua"),
    "GetID at the idle rate on a busy link": (
        "(in->link_active ? cfg->busy_probe_ms : cfg->probe_ms)", "cfg->probe_ms"),
    "fights a user role change": (
        "\t\tif (!in->host || !in->owned) {\n\t\t\tta->phase = TRISTAR_AUTO_INHIBIT;",
        "\t\tif (0) {\n\t\t\tta->phase = TRISTAR_AUTO_INHIBIT;"),
    "I2C errors count as unplugged": ("\tif (in->status < 0)\n\t\treturn TRISTAR_AUTO_NONE;\n", ""),
    "any 11 xx ID is the adapter": ("id[0] == 0x11 && id[1] == 0xf0", "id[0] == 0x11"),
    "no ID re-check in host mode": (
        "if (edge || tristar_auto_due(in->now, ta->next_probe)) {", "if (0) {"),
    "no handshake retry": ("if (ret && ta->ph_tries < cfg->handshake_tries)", "if (0)"),
}
mdir = out / "mut"
mdir.mkdir()
for name, (old, new) in mutations.items():
    assert src.count(old) == 1, name
    (mdir / "apple_tristar_auto.h").write_text(src.replace(old, new))
    subprocess.run(["clang", "-std=gnu11", "-w", "-I", str(mdir), "-o", str(mdir / "t"),
                    "/work/scripts/periph/test-tristar-auto.c"], check=True)
    r = subprocess.run([str(mdir / "t")], capture_output=True, text=True)
    assert r.returncode != 0, f"harness missed mutation: {name}"
print(f"PASS mutation check: {len(mutations)} of {len(mutations)} policy mutations caught")
PY
'
