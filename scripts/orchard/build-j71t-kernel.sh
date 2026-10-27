#!/usr/bin/env bash
# Build the OrchardLinux J71t kernel with Orchard's own recipe, offline.
#
# Usage: [EXPECT_TREE=sha] scripts/orchard/build-j71t-kernel.sh resolve|build HARDWARE WORK OUTPUT
#
# HARDWARE  OrchardLinux/hardware checkout (kernel/patches/j71t, configs/j71t).
# WORK      empty or reused directory on a case-sensitive filesystem that
#           Docker can mount (macOS: a Case-sensitive APFS sparse image
#           attached under /Users), or volume:NAME for a Docker volume (about
#           3 GB on the Docker VM disk; remove it afterwards). Holds the kernel
#           source and build output.
# OUTPUT    new host directory.
#
# The source is HoolockLinux 6831bc701a, cloned shallowly from the
# linux-a9-src volume (no GitHub fetch) with origin set to Orchard's
# upstream.lock URL, so kernel/build --resume accepts it.
#
# resolve: applies the patches as kernel/build does and writes
#          olddefconfig(HARDWARE kernel/configs/j71t/config) to OUTPUT/config.
#          Copy it back into the hardware repo after review.
# build:   prebuilds with -j$(nproc) using kernel/build's exact make flags and
#          KBUILD_* values, then runs the unmodified kernel/build j71t
#          --output OUTPUT --resume in Orchard's toolchain image
#          (ORCHARD_BUILD_NATIVE=1; its -j2 make then has little left to do).
#          kernel/build checks the config is byte-stable, stages modules with
#          depmod and checks the device-tree pin.
# EXPECT_TREE: the patched source tree must equal this git tree (for example
#          series.json filtered_tree, which is also a9/orchard-j71t's tree).
set -euo pipefail

MODE=${1:?usage: build-j71t-kernel.sh resolve|build HARDWARE WORK OUTPUT}
HARDWARE=$(cd "${2:?}" && pwd)
OUTPUT=${4:?}
OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
[[ $MODE == resolve || $MODE == build ]] || { echo "Unknown mode: $MODE" >&2; exit 2; }
[[ ! -e $OUTPUT ]] || { echo "Output exists: $OUTPUT" >&2; exit 1; }
TOOLCHAIN=localhost/orchard-hardware-kernel-toolchain:bookworm
docker image inspect "$TOOLCHAIN" >/dev/null
if [[ ${3:?} == volume:* ]]; then
	WORK=${3#volume:}
	docker volume create "$WORK" >/dev/null
else
	WORK=$(cd "$3" && pwd)
	rm -f "$WORK/.case-probe" "$WORK/.CASE-PROBE"
	touch "$WORK/.case-probe"
	if [[ -e $WORK/.CASE-PROBE ]]; then
		rm -f "$WORK/.case-probe"; echo "WORK must be case-sensitive: $WORK" >&2; exit 1
	fi
	rm -f "$WORK/.case-probe"
fi

# A fresh copy of the recipe (tracked and new files, as in the working tree);
# the kernel work directory WORK/kwork persists across runs. The source is
# cloned once.
commit=$(awk -F= '$1=="commit" {print $2}' "$HARDWARE/kernel/upstream.lock")
url=$(awk -F= '$1=="url" {print substr($0,5)}' "$HARDWARE/kernel/upstream.lock")
git -C "$HARDWARE" ls-files -z -co --exclude-standard | (cd "$HARDWARE" && COPYFILE_DISABLE=1 tar --null -T - -cf -) |
	docker run -i --rm -v linux-a9-src:/src:ro -v "$WORK:/b" -e COMMIT="$commit" -e URL="$url" \
	linux-a9-kbuild bash -euc '
	rm -rf /b/hardware
	mkdir -p /b/hardware /b/kwork /b/py
	tar -xf - -C /b/hardware
	ln -s ../../kwork /b/hardware/kernel/.work
	ln -sfn ../hardware /b/py/orchard_hardware
	git config --global --add safe.directory "*"
	work=/b/kwork/j71t
	[[ -d $work/source/.git ]] && exit 0
	mkdir -p "$work/output"
	git clone -q --depth 1 --branch hoolock file:///src/linux "$work/source"
	[[ $(git -C "$work/source" rev-parse HEAD) == "$COMMIT" ]] || { echo "hoolock is not $COMMIT" >&2; exit 1; }
	git -C "$work/source" remote set-url origin "$URL"
	'

# kernel/build's fresh-build apply step: every patch in order on the pinned
# revision. A source already at EXPECT_TREE is kept (incremental rebuilds);
# anything else is reset to the pinned revision first. Then the marker is
# written in kernel/build's own format, so --resume accepts exactly these
# patch files. (Re-checking each patch by reverse-applying it to the final
# tree would fail once a later patch changes an earlier patch's files.)
apply='
cd /b/kwork/j71t/source
tree_now() { GIT_INDEX_FILE=/tmp/orchard-tree git read-tree HEAD &&
	GIT_INDEX_FILE=/tmp/orchard-tree git add -A . && GIT_INDEX_FILE=/tmp/orchard-tree git write-tree; }
tree=$(tree_now)
if [[ -z $EXPECT_TREE || $tree != "$EXPECT_TREE" ]]; then
	git checkout -q -- . && git clean -qfd
	for patch in /b/hardware/kernel/patches/j71t/*.patch; do
		git apply --check "$patch"
		git apply "$patch"
	done
	tree=$(tree_now)
fi
echo "patched tree: $tree"
[[ -z $EXPECT_TREE || $tree == "$EXPECT_TREE" ]] || { echo "expected tree $EXPECT_TREE" >&2; exit 1; }
for patch in /b/hardware/kernel/patches/j71t/*.patch; do
	printf "%s %s\n" "$(git hash-object "$patch")" "${patch##*/}"
done > /b/kwork/j71t/applied-patches
'
flags='export KBUILD_BUILD_USER=orchard KBUILD_BUILD_HOST=builder
export KBUILD_BUILD_VERSION=1 KBUILD_BUILD_TIMESTAMP="2026-10-22T00:00:00Z"
src=/b/hardware/kernel/.work/j71t/source out=/b/hardware/kernel/.work/j71t/output
'
run() {
	docker run --rm -v "$WORK:/b" -v "$(dirname "$OUTPUT"):/o" -e EXPECT_TREE="${EXPECT_TREE:-}" \
		-e OUT_NAME="$(basename "$OUTPUT")" "$TOOLCHAIN" bash -euc "git config --global --add safe.directory '*'
$1"
}

if [[ $MODE == resolve ]]; then
	run "$apply$flags"'
cp /b/hardware/kernel/configs/j71t/config "$out/.config"
make -s -C "$src" O="$out" LLVM=-19 ARCH=arm64 olddefconfig
mkdir "/o/$OUT_NAME"
cp "$out/.config" "/o/$OUT_NAME/config"
'
	echo "Resolved config: $OUTPUT/config"
	exit 0
fi

run "$apply$flags"'
cp /b/hardware/kernel/configs/j71t/config "$out/.config"
make -s -C "$src" O="$out" LLVM=-19 ARCH=arm64 olddefconfig
cmp "$out/.config" /b/hardware/kernel/configs/j71t/config
make -C "$src" O="$out" -j"$(nproc)" LLVM=-19 ARCH=arm64 \
	LOCALVERSION="-orchard-j71t" DTC_FLAGS=-@ Image.gz modules dtbs >/b/prebuild.log 2>&1 || {
	tail -40 /b/prebuild.log; exit 1; }
cd /b/hardware
ORCHARD_BUILD_NATIVE=1 ORCHARD_BUILD_ENV_ID=linux-a9-docker-orchard-toolchain PYTHONPATH=/b/py \
	./kernel/build j71t --output "/o/$OUT_NAME" --resume
'
echo "Kernel output: $OUTPUT"
