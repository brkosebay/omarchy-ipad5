#!/usr/bin/env bash
# Build the TEST-ONLY /dev/sep-bio mock ABI harness in a kernel worktree.
#
# Source of truth is drivers/sep/sep-bio-mock.c in this repo: it is copied into
# drivers/soc/apple/ of the worktree and wired in with its own
# APPLE_SEP_BIO_MOCK Kconfig symbol and object. The real sep-a9 driver is not
# copied and is not required; the two symbols can coexist but either module can
# own /dev/sep-bio, so only one should be loaded at a time.
#
# .config is never replaced: an existing worktree config is kept and only the
# APPLE_SEP_BIO_MOCK symbol is enabled; a missing config is seeded from
# configs/hoolock-16k.config.
#
# Usage:
#   scripts/sep/build-bio-mock.sh             # build the object with W=1
#   scripts/sep/build-bio-mock.sh clean       # remove built objects
#   TREE=sep W=0 scripts/sep/build-bio-mock.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-sep-bio-mock}"
W="${W:-1}"

if [ "${1:-}" = clean ]; then
	"$ROOT/scripts/build-kernel.sh" --run "$TREE" \
		'rm -f drivers/soc/apple/sep-bio-mock.o drivers/soc/apple/sep-bio-mock.ko'
	exit 0
fi

"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -euo pipefail
mkdir -p drivers/soc/apple include/uapi/linux
cp /work/drivers/sep/sep-bio-mock.c drivers/soc/apple/sep-bio-mock.c
cp /work/drivers/sep/uapi/apple-sep.h include/uapi/linux/apple-sep.h

if ! grep -q APPLE_SEP_BIO_MOCK drivers/soc/apple/Kconfig; then
	cat >> drivers/soc/apple/Kconfig <<"EOF"

config APPLE_SEP_BIO_MOCK
	tristate "Apple A9 SEP biometric ABI mock (TEST ONLY)"
	depends on ARCH_APPLE || COMPILE_TEST
	help
	  Registers a synthetic /dev/sep-bio that implements the v4 UAPI
	  contract without any SEP, MMIO, storage or biometric hardware
	  access. Identities and tokens are fake and non-cryptographic.
	  Test-only: never enable this on a system that should use a real
	  biometric backend.
EOF
fi

if ! grep -q sep-bio-mock.o drivers/soc/apple/Makefile; then
	echo "obj-\$(CONFIG_APPLE_SEP_BIO_MOCK) += sep-bio-mock.o" >> drivers/soc/apple/Makefile
fi

[ -f .config ] || cp /work/configs/hoolock-16k.config .config
scripts/config -e APPLE_SEP_BIO_MOCK
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make -j"$(nproc)" LLVM=1 ARCH=arm64 W='"$W"' drivers/soc/apple/sep-bio-mock.o
'
