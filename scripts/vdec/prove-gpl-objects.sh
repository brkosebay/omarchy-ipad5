#!/usr/bin/env bash
# Rebuild the original and GPL headers in our own tree; compare full .o files.
# Does not change refs, commits, other worktrees, or the device.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run vdec-gpl '
set -euo pipefail
test "$(git branch --show-current)" = a9/vdec-gpl
D=drivers/media/platform/apple/vxd
O=/work/build/vdec-gpl
H=$D/apple-vxd-h264-tables.h
mkdir -p "$O/before" "$O/after"
original=$(mktemp)
cp "$H" "$original"
trap '\''cp "$original" "$H"; rm -f "$original"'\'' EXIT
cp /work/configs/hoolock-16k.config .config
while IFS= read -r line; do
    case "$line" in
    CONFIG_*=y) symbol=${line#CONFIG_}; scripts/config -e "${symbol%=y}" ;;
    esac
done < /work/patches/vdec/vdec.config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make -s LLVM=1 ARCH=arm64 olddefconfig
grep -q "^CONFIG_DEBUG_INFO_NONE=y" .config
git show 9a897a8fd21c456231c51559f4d898aec388b58f:"$H" > "$H"
make -j2 LLVM=1 ARCH=arm64 W=1 "$D/"
cp "$D/"*.o "$O/before/"
cp "$original" "$H"
make -j2 LLVM=1 ARCH=arm64 W=1 "$D/"
cp "$D/"*.o "$O/after/"
cp .config "$O/config"
for o in "$D/"*.o; do
    cmp "$O/before/$(basename "$o")" "$o"
done
for stage in before after; do
    llvm-objcopy --dump-section .rodata="$O/rodata-$stage.bin" \
        "$O/$stage/apple-vxd-h264.o" "$O/section-copy.o"
done
rm -f "$O/section-copy.o"
cmp "$O/rodata-before.bin" "$O/rodata-after.bin"
llvm-nm -S "$D/apple-vxd-h264.o" | grep apple_vxd_h264_vlc_table
sha256sum "$O/before/"*.o "$O/after/"*.o "$O/rodata-"*.bin
clang --version | head -1
echo "PASS: all ten full driver objects and H.264 .rodata identical"
'
