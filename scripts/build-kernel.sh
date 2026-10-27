#!/usr/bin/env bash
# Build the OrchardLinux j71t kernel (Image.gz + Apple dtbs) inside a native arm64
# Linux container. Sources live in the Docker volume `omarchy-ipad5-src`, not on
# APFS: macOS's case-insensitive filesystem breaks kernel builds.
#
# Usage:
#   scripts/build-kernel.sh                 # clean `a9/orchard-j71t` source (refetched)
#   scripts/build-kernel.sh --tree NAME     # build workstream tree NAME as-is
#   scripts/build-kernel.sh --shell [NAME]  # interactive shell in the container (needs a TTY)
#   scripts/build-kernel.sh --run NAME 'cmd'  # run cmd non-interactively in tree NAME
#                                           # (for agents: git fetch/cherry-pick/grep ...)
#
# Workstream trees are git worktrees of the main clone at /src/wt/NAME on
# branch `work/NAME`, created from `a9/orchard-j71t` on first use and never reset, so
# Each subsystem workstream edits and builds its own tree.
#
# Output: build/kernel/ (pristine) or build/kernel-NAME/
#         {Image.gz, dtbs/*.dtb, config, COMMIT}
# KERNEL_MEMORY optionally caps this build container (e.g. 1536m, no swap).
# KERNEL_JOBS optionally caps parallel compile jobs (default: nproc).
# KERNEL_LOCALVERSION optionally labels an uncommitted integration overlay
# without changing its tested config. RELEASE and SOURCE.patch record it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VOLUME="omarchy-ipad5-src"
IMAGE="linux-a9-kbuild"
TREE=""
SHELL_MODE=0
RUN_CMD=""

while [ $# -gt 0 ]; do
	case "$1" in
	--tree) TREE="$2"; shift 2 ;;
	--shell) SHELL_MODE=1; TREE="${2:-}"; [ -n "$TREE" ] && shift; shift ;;
	--run) TREE="$2"; RUN_CMD="$3"; shift 3 ;;
	*) echo "unknown arg: $1" >&2; exit 1 ;;
	esac
done

OUT="$ROOT/build/kernel${TREE:+-$TREE}"
mkdir -p "$OUT"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker build --platform linux/arm64 -t "$IMAGE" - <<'EOF'
FROM fedora:latest
RUN dnf -y install clang lld llvm make flex bison bc dtc git perl python3 \
        gzip xz openssl-devel elfutils-libelf-devel ncurses-devel diffutils \
        findutils hostname rsync cpio && dnf clean all
EOF
fi

PREP='
set -euo pipefail
git config --global --add safe.directory "*"
git config --global user.name "linux-a9"
git config --global user.email "linux-a9@localhost"
cd /src
if [ ! -d linux/.git ]; then
	git clone --depth 1 --branch a9/orchard-j71t https://github.com/brkosebay/linux.git linux
fi
if [ -z "$TREE" ]; then
	git -C linux fetch --depth 1 https://github.com/brkosebay/linux.git a9/orchard-j71t
	git -C linux checkout -q -B a9/orchard-j71t FETCH_HEAD
	cd /src/linux
else
	if [ ! -d "/src/wt/$TREE" ]; then
		mkdir -p /src/wt
		# Pruned trees come back on their existing branch; new ones start at a9/orchard-j71t.
		git -C linux worktree prune
		if git -C linux rev-parse -q --verify "refs/heads/work/$TREE" >/dev/null; then
			git -C linux worktree add "/src/wt/$TREE" "work/$TREE"
		else
			git -C linux worktree add -b "work/$TREE" "/src/wt/$TREE" a9/orchard-j71t
		fi
	fi
	cd "/src/wt/$TREE"
fi
'

if [ "$SHELL_MODE" = 1 ]; then
	exec docker run --rm -it --platform linux/arm64 -v "$VOLUME:/src" \
		-v "$ROOT:/work" -e TREE="$TREE" "$IMAGE" \
		bash -c "$PREP"'exec bash'
fi

# The repo is mounted at /work, e.g. for `git format-patch -o /work/patches/...`.
if [ -n "$RUN_CMD" ]; then
	exec docker run --rm --platform linux/arm64 -v "$VOLUME:/src" \
		-v "$ROOT:/work" -e TREE="$TREE" -e RUN_CMD="$RUN_CMD" "$IMAGE" \
		bash -c "$PREP"'eval "$RUN_CMD"'
fi

memory_args=()
if [ -n "${KERNEL_MEMORY:-}" ]; then
	memory_args=(--memory "$KERNEL_MEMORY" --memory-swap "$KERNEL_MEMORY")
fi

docker run --rm --platform linux/arm64 "${memory_args[@]}" \
	-v "$VOLUME:/src" \
	-v "$ROOT/configs:/configs:ro" \
	-v "$ROOT/scripts/boot/check-early-pstore.py:/check-early-pstore.py:ro" \
	-v "$OUT:/out" \
	-e TREE="$TREE" \
	-e KERNEL_JOBS="${KERNEL_JOBS:-}" \
	-e KERNEL_LOCALVERSION="${KERNEL_LOCALVERSION:-}" \
	"$IMAGE" bash -c "$PREP"'
echo "kernel: $(pwd) $(git log -1 --format="%h %s")"

# This path runs before paging/console; compilation alone cannot validate it.
if grep -q "apple_j71t_reserve_pstore(" arch/arm64/mm/init.c; then
	python3 -B -I /check-early-pstore.py --source "$PWD"
fi

# Keep a tree-local config once it exists, so workstreams can add options.
if [ ! -f .config ]; then
	cp /configs/hoolock-16k.config .config
	# Debug info triples build size; disk on the host is tight.
	scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
fi
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
build_args=()
if [ -n "$KERNEL_LOCALVERSION" ]; then
	build_args+=("LOCALVERSION=$KERNEL_LOCALVERSION")
fi
make -j"${KERNEL_JOBS:-$(nproc)}" LLVM=1 ARCH=arm64 "${build_args[@]}" Image.gz dtbs

rm -rf /out/dtbs && mkdir -p /out/dtbs
cp arch/arm64/boot/Image.gz /out/
cp .config /out/config
cp arch/arm64/boot/dts/apple/*.dtb /out/dtbs/
git log -1 --format="%H %s" > /out/COMMIT
git status --short > /out/DIRTY
git diff --binary > /out/SOURCE.patch
cat include/config/kernel.release > /out/RELEASE
echo "done: $(ls /out/dtbs | wc -l) dtbs"
'
