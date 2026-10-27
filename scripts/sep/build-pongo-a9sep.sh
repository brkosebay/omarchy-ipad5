#!/usr/bin/env bash
# Rebuild the patched pongoOS image used by the A9 SEP cold/discovery path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="$ROOT/third_party/pongoOS"
PATCH="$ROOT/patches/sep/pongoos/pongoos-a9-sep.patch"
OUT="$ROOT/build/prebuilt/pongo/Pongo-a9sep.bin"
UPSTREAM=https://github.com/checkra1n/pongoOS.git
COMMIT=cc532fac72e97f7c7f50f891073fde12f519f0a1
PATHS=(src/drivers/sep/sep.c src/drivers/tz/tz.c src/drivers/tz/tz.h)

if [ ! -d "$TREE/.git" ]; then
	mkdir -p "$(dirname "$TREE")"
	git clone "$UPSTREAM" "$TREE"
	git -C "$TREE" checkout --detach "$COMMIT"
fi

if [ "$(git -C "$TREE" rev-parse HEAD)" != "$COMMIT" ]; then
	echo "build-pongo-a9sep: $TREE is not at pinned commit $COMMIT" >&2
	exit 1
fi

git -C "$TREE" submodule update --init --recursive
if git -C "$TREE" diff --quiet -- "${PATHS[@]}"; then
	git -C "$TREE" apply "$PATCH"
elif ! cmp -s "$PATCH" <(git -C "$TREE" diff -- "${PATHS[@]}"); then
	echo "build-pongo-a9sep: relevant source files have edits not represented by $PATCH" >&2
	exit 1
fi

if command -v sysctl >/dev/null 2>&1; then
	default_jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
	default_jobs=4
fi
make -C "$TREE" -j"${JOBS:-$default_jobs}" PONGO_DISPLAY_NAME=a9sep-statfix-v1

# The experiment depends on the MMIO transaction width, not merely the value.
# Audit the linked Mach-O so source-level refactors cannot silently split it.
OBJDUMP="$(xcrun -f llvm-objdump)"
MSG64_DISASM="$($OBJDUMP --macho --disassemble "$TREE/build/Pongo" |
	awk '/^_mailbox_write_a9_single64:$/ { show=1 }
	     show && /^_/ && !/^_mailbox_write_a9_single64:$/ { show=0 }
	     show { print }')"
if [ "$(printf '%s\n' "$MSG64_DISASM" |
	grep -Ec '[[:space:]]str[[:space:]]+x[0-9]+, \[x[0-9]+, #0x10\]')" -ne 1 ] ||
	! printf '%s\n' "$MSG64_DISASM" | grep -Eq '[[:space:]]dsb[[:space:]]+sy' ||
	printf '%s\n' "$MSG64_DISASM" | grep -Eq '[[:space:]]str[[:space:]]+w'; then
	echo "build-pongo-a9sep: msg64 helper is not one STR X + DSB SY" >&2
	printf '%s\n' "$MSG64_DISASM" >&2
	exit 1
fi
echo "verified _mailbox_write_a9_single64: one STR X + DSB SY"
printf '%s\n' "$MSG64_DISASM"

# `sep stat` must never pop or even sample the FIFO data window. An empty
# +0x4034/+0x4038 read latches OUTBOX underflow (status bit 19) on A9.
STAT_DISASM="$($OBJDUMP --macho --disassemble "$TREE/build/Pongo" |
	awk '/^_sep_stat_cmd:$/ { show=1 }
	     show && /^_/ && !/^_sep_stat_cmd:$/ { show=0 }
	     show { print }')"
if [ -z "$STAT_DISASM" ] || printf '%s\n' "$STAT_DISASM" |
	grep -Eq '[[:space:]]ldr[[:space:]]+[wx][0-9]+, \[x[0-9]+, #0x(34|38)\]'; then
	echo "build-pongo-a9sep: sep stat still reads the RX FIFO data window" >&2
	printf '%s\n' "$STAT_DISASM" >&2
	exit 1
fi
echo "verified _sep_stat_cmd: no +0x4034/+0x4038 FIFO-data load"

mkdir -p "$(dirname "$OUT")"
install -m 0644 "$TREE/build/Pongo.bin" "$OUT"

echo "built $OUT"
wc -c <"$OUT" | awk '{ print "size=" $1 }'
shasum -a 256 "$OUT"
