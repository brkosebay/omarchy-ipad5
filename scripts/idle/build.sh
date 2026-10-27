#!/usr/bin/env bash
# Replay the independent a9/idle series and configure/build in its own tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run idle '
set -e
test "$(git branch --show-current)" = a9/idle
test -z "$(git status --porcelain --untracked-files=no)"
if ! git cat-file -e HEAD:drivers/cpuidle/cpuidle-apple-a9-idle.c 2>/dev/null; then
    test "$(git rev-parse HEAD)" = "$(git rev-parse hoolock)"
    while IFS= read -r patch; do
        git am "/work/patches/idle/$patch"
    done < /work/patches/idle/series
fi
cp /work/configs/hoolock-16k.config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
scripts/kconfig/merge_config.sh -m .config /work/patches/idle/idle.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
while IFS= read -r want; do
    case "$want" in CONFIG_*=y|CONFIG_*=m) grep -qxF "$want" .config;; esac
done < /work/patches/idle/idle.config
'
"$ROOT/scripts/build-kernel.sh" --tree idle
