#!/usr/bin/env bash
# Snapshot actual kernel functions, then run offline positive/negative tests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/build/periph/usb-resume/source"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
git merge-base --is-ancestor 64221b189e HEAD
out=/work/build/periph/usb-resume/source
cp drivers/usb/dwc2/gadget.c "$out/dwc2-gadget.c"
cp drivers/usb/gadget/function/f_ecm.c "$out/f_ecm.c"
git show 64221b189e:drivers/usb/dwc2/gadget.c > "$out/dwc2-gadget-before.c"
git show 64221b189e:drivers/usb/gadget/function/f_ecm.c > "$out/f_ecm-before.c"
cmp drivers/power/supply/apple_j71t_charger.c /work/scripts/periph/kernel/apple_j71t_charger.c
cmp drivers/power/supply/apple_tristar_power.c /work/scripts/periph/kernel/apple_tristar_power.c
python3 -B -I /work/scripts/periph/test-round4-usb.py
'
python3 -B "$ROOT/scripts/periph/test-usb-resume.py"
python3 -B "$ROOT/scripts/periph/test-usb-resume.py" --baseline
python3 -B "$ROOT/scripts/periph/test-round4-source.py"
python3 -B "$ROOT/scripts/periph/test-charging.py"
python3 -B "$ROOT/scripts/omarchy/test-usb-recovery.py"
