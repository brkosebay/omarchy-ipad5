#!/usr/bin/env bash
# Verify the integrated artifact's required configuration and build provenance.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K="$ROOT/build/kernel-${TREE:-bootinteg}"
for want in CONFIG_KEXEC=y CONFIG_KEXEC_FILE=y CONFIG_CRASH_DUMP=y CONFIG_PROC_VMCORE=y CONFIG_HOTPLUG_CPU=y CONFIG_ARM_PSCI_FW=y CONFIG_DEBUG_INFO_NONE=y CONFIG_NVME_APPLE_ANS_EARLY_USB=y CONFIG_PSTORE=y CONFIG_PSTORE_RAM=y CONFIG_PSTORE_CONSOLE=y CONFIG_PSTORE_DEFAULT_KMSG_BYTES=1048576; do
    grep -qxF "$want" "$K/config" || { echo "missing $want" >&2; exit 1; }
done
for symbol in APPLE_J71T_BOOT_WATCHDOG SOFTLOCKUP_DETECTOR HARDLOCKUP_DETECTOR_BUDDY DETECT_HUNG_TASK; do
    grep -qxF "CONFIG_$symbol=y" "$K/config" || { echo "missing CONFIG_$symbol=y" >&2; exit 1; }
done
[ ! -s "$K/DIRTY" ] || { echo 'built tree was dirty' >&2; cat "$K/DIRTY"; exit 1; }
[ -s "$K/Image.gz" ] && [ -s "$K/dtbs/s8003-j71t.dtb" ]
printf 'PASS required config and clean source: '
cat "$K/COMMIT"
