#!/usr/bin/env bash
# Reuse the dtschema installation made under build/periph, inside Docker.
# If absent, --install prepares it using disposable container packages.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "${1:-}" = "--install" ]; then
  "$ROOT/scripts/build-kernel.sh" --run periph '
  dnf -y install python3-pip swig python3-devel
  python3 -m pip install --target /work/build/periph/schema-lib dtschema==2026.9
  '
fi
mkdir -p "$ROOT/build/periph/schema-tools"
cat > "$ROOT/build/periph/schema-tools/which" <<'EOF'
#!/bin/sh
command -v "$@"
EOF
chmod +x "$ROOT/build/periph/schema-tools/which"
exec "$ROOT/scripts/build-kernel.sh" --run periph '
test -x /work/build/periph/schema-lib/bin/dt-doc-validate
export PYTHONPATH=/work/build/periph/schema-lib
export PATH=/work/build/periph/schema-tools:/work/build/periph/schema-lib/bin:$PATH
schemas="net/bluetooth/brcm,bluetooth.yaml:serial/samsung_uart.yaml:iio/light/tsl2583.yaml:power/supply/apple,d2365-charger.yaml:power/supply/apple,j71t-cbtl1610-power.yaml:power/supply/ti,bq27545-hdq.yaml:gpio/apple,d2365-pmic-gpio.yaml:mfd/apple,pmic.yaml:nvmem/apple,pmic-nvmem.yaml:usb/dwc2.yaml"
make LLVM=1 ARCH=arm64 dt_binding_check DT_SCHEMA_FILES="$schemas"
make LLVM=1 ARCH=arm64 dtbs_check DT_SCHEMA_FILES="$schemas"
'
