#!/usr/bin/env bash
# Offline tests of make-bundle and linux-a9-update (docs/boot/kernel-updates.md).
# No device/SSH calls: fake /boot tree, mocked linux-a9-kexec/systemctl/ip/findmnt.
#
#   scripts/boot/test-update-offline.sh
#   scripts/boot/test-update-offline.sh --real BUNDLE ENTRY_DIR
#       also install a real make-bundle.sh output over a copy of a real staged
#       entry (e.g. build/boot/integ-entry), then trial-boot and promote it
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
real=()
if [ "${1:-}" = --real ]; then
	[ $# -eq 3 ] || { sed -n '5,8p' "$0" >&2; exit 2; }
	b="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
	e="$(cd "$3" && pwd)"
	case "$b" in "$ROOT"/*) ;; *) echo "$2 must be inside $ROOT" >&2; exit 2 ;; esac
	case "$e" in "$ROOT"/*) ;; *) echo "$3 must be inside $ROOT" >&2; exit 2 ;; esac
	real=(-e "REAL_BUNDLE=/work/${b#"$ROOT"/}" -e "REAL_ENTRY=/work/${e#"$ROOT"/}")
fi
docker run --rm --platform linux/arm64 -v "$ROOT:/work:ro" ${real[@]+"${real[@]}"} linux-a9-kbuild bash -c '
set -euo pipefail
command -v zstd ssh-keygen sha256sum >/dev/null
python3 -B -I /work/scripts/boot/test-update-offline.py
for f in /work/scripts/boot/make-bundle.sh /work/scripts/boot/install-device.sh; do bash -n "$f"; done
python3 -B -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" /work/scripts/boot/device/linux-a9-update
'
