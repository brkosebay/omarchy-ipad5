#!/usr/bin/env bash
# Regenerate the OrchardLinux J71t kernel patch series from a9/release.
#
# Usage: [REF=commit] [SCRATCH_DIR=/Volumes/case-sensitive/dir] scripts/orchard/make-j71t-series.sh OUTPUT_DIR
#
# REF defaults to the current a9/release tip; phase 1 of the Orchard port used
# release 1, REF=9dbc8612b3b0c2dc138abb51add00e1fbc1e3d6e.
#
# Mounts the linux-a9-src volume read-only, clones it (shared objects) into a
# scratch Docker volume (or SCRATCH_DIR, an empty directory on a case-sensitive
# host filesystem, when the Docker VM disk is short), filters a detached a9/release checkout with
# j71t-series.py and writes 0001..NNNN-*.patch plus series.json to OUTPUT_DIR.
# No a9/* branch is modified; nothing is pushed. Copy the patches into
# OrchardLinux/hardware kernel/patches/j71t/ and rebuild configs/j71t/config
# if the release changed (see docs/release/orchard-port.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTPUT=${1:?usage: make-j71t-series.sh OUTPUT_DIR}
[[ ! -e "$OUTPUT" ]] || { echo "Output exists: $OUTPUT" >&2; exit 1; }
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"
if [[ -n "${SCRATCH_DIR:-}" ]]; then
	[[ -d "$SCRATCH_DIR" && -z "$(ls -A "$SCRATCH_DIR")" ]] || { echo 'SCRATCH_DIR must be an empty directory' >&2; exit 1; }
	SCRATCH=$SCRATCH_DIR
else
	SCRATCH=orchard-j71t-series-$$
	docker volume create "$SCRATCH" >/dev/null
	trap 'docker volume rm -f "$SCRATCH" >/dev/null' EXIT
fi

docker run --rm -e REF="${REF:-origin/a9/release}" -v linux-a9-src:/src:ro -v "$SCRATCH:/work" -v "$OUTPUT:/out" \
	-v "$ROOT/scripts/orchard:/tool:ro" linux-a9-kbuild bash -euc '
git config --global --add safe.directory "*"
git config --global user.name orchard-series
git config --global user.email orchard-series@localhost
git clone -q --shared --no-checkout /src/linux /work/k
git -C /work/k checkout -q --detach "$REF"
python3 /tool/j71t-series.py --tree /work/k --output /work/series --ref "$REF"
cp /work/series/* /out/
'
echo "Series: $OUTPUT"
