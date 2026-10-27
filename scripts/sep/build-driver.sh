#!/usr/bin/env bash
# Build the A9 SEP mailbox driver in the `sep` kernel workstream.
#
# Source of truth is drivers/sep/sep-a9.c in this repo: it is copied into
# drivers/soc/apple/ of the workstream tree and wired into that directory's
# Kconfig/Makefile (idempotently, so re-running is safe).
#
# Usage:
#   scripts/sep/build-driver.sh            # build sep-a9.ko
#   scripts/sep/build-driver.sh clean      # remove the object, keep the source
#   TREE=sep-dev scripts/sep/build-driver.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-sep}"

if [ "${1:-}" = clean ]; then
	"$ROOT/scripts/build-kernel.sh" --run "$TREE" \
		'rm -f drivers/soc/apple/sep-a9.o'
	exit 0
fi

"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -euo pipefail
cp /work/drivers/sep/sep-a9.c drivers/soc/apple/sep-a9.c
mkdir -p include/uapi/linux
cp /work/drivers/sep/uapi/apple-sep.h include/uapi/linux/apple-sep.h

if ! grep -q APPLE_SEP_A9 drivers/soc/apple/Kconfig; then
	cat >> drivers/soc/apple/Kconfig <<"EOF"

config APPLE_SEP_A9
	tristate "Apple A9 Secure Enclave mailbox"
	depends on ARCH_APPLE || COMPILE_TEST
	help
	  Mailbox driver for the Secure Enclave on Apple A9 (s8000/s8003).
	  Exposes raw SEP messaging and endpoint OOL buffer setup. Used to
	  bring up sepOS and the biometric (SBIO) endpoint on iPad 5.
EOF
fi

if ! grep -q sep-a9.o drivers/soc/apple/Makefile; then
	echo "obj-\$(CONFIG_APPLE_SEP_A9) += sep-a9.o" >> drivers/soc/apple/Makefile
fi

[ -f .config ] || cp /work/configs/hoolock-16k.config .config
scripts/config -e APPLE_SEP_A9
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make -j"$(nproc)" LLVM=1 ARCH=arm64 drivers/soc/apple/sep-a9.o
'
