#!/usr/bin/env bash
# Run one nvme-install.py step on the iPad (Linux, SSH up) from the Mac, with
# the layout this Mac planned. Logs go to build/nvme-install/logs/.
#
#   scripts/nvme-install/run.sh info|check|fence-test|soak|verify [args...]
#   scripts/nvme-install/run.sh hash-outside NAME [--compare OLDNAME]
#       -> build/nvme-install/state/NAME.txt (compared with state/OLDNAME.txt)
#   scripts/nvme-install/run.sh write-image [IMAGE]   # default build/nvme-install/rootfs-nvme.img
#       zstd stream over SSH into p2, then a full read-back sha256 compare
#
# Each run copies nvme-install.py, layout.env, the saved state/ files and the
# Mac's NSID 2-7 checksums to /tmp/nvi on the iPad, runs the step with sudo,
# and copies state/ back. Exit status is the step's (0 pass, 1 FAIL, 2 refused).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/nvme-install"
SSH="$ROOT/scripts/omarchy/ssh.sh"
CMD="${1:?usage: run.sh STEP [args]}"
shift
[ -f "$B/layout.env" ] || { echo "no $B/layout.env (scripts/nvme-install/plan-layout.py)" >&2; exit 1; }
mkdir -p "$B/logs" "$B/state"
LOG="$B/logs/$(date +%Y%m%d-%H%M%S)-$CMD.txt"

stage() {
	local tmp
	tmp=$(mktemp -d)
	mkdir -p "$tmp/nvi/state"
	cp "$ROOT/scripts/nvme-install/nvme-install.py" "$B/layout.env" "$tmp/nvi/"
	cp "$ROOT/build/firmware/private/nvme-ns/SHA256SUMS" "$tmp/nvi/ns-SHA256SUMS" 2>/dev/null || true
	cp "$B"/state/*.txt "$tmp/nvi/state/" 2>/dev/null || true
	tar -C "$tmp" -cf - nvi | "$SSH" 'rm -rf /tmp/nvi && tar -C /tmp -xf -'
	rm -rf "$tmp"
}
unstage() {
	"$SSH" 'tar -C /tmp/nvi -cf - state' | tar -C "$B" -xf -
}
remote() { # remote STEP ARGS...: sudo python3 the tool on the iPad
	"$SSH" "sudo python3 /tmp/nvi/nvme-install.py $* --layout /tmp/nvi/layout.env"
}

stage
rc=0
case "$CMD" in
hash-outside)
	name="${1:?hash-outside NAME [--compare OLDNAME]}"
	shift
	extra="--out /tmp/nvi/state/$name.txt --ns-sums /tmp/nvi/ns-SHA256SUMS"
	if [ "${1:-}" = --compare ]; then
		[ -f "$B/state/$2.txt" ] || { echo "no $B/state/$2.txt" >&2; exit 1; }
		extra="$extra --compare /tmp/nvi/state/$2.txt"
	fi
	remote hash-outside "$extra" 2>&1 | tee "$LOG" || rc=${PIPESTATUS[0]}
	unstage
	;;
write-image)
	img="${1:-$B/rootfs-nvme.img}"
	[ -f "$img" ] || { echo "missing $img (scripts/nvme-install/prep-image.sh)" >&2; exit 1; }
	[ -f "$img.sha256" ] || { echo "missing $img.sha256 (prep-image.sh writes it)" >&2; exit 1; }
	bytes=$(stat -f %z "$img")
	sum=$(cut -d' ' -f1 "$img.sha256")
	echo "streaming $img ($bytes bytes, sha256 $sum) into p2" | tee "$LOG"
	set +e
	zstd -T0 -3 -q -c "$img" |
		"$SSH" "zstd -q -dc | sudo python3 /tmp/nvi/nvme-install.py write-image --layout /tmp/nvi/layout.env --bytes $bytes --sha256 $sum" 2>&1 | tee -a "$LOG"
	rc=${PIPESTATUS[1]}
	set -e
	if [ "$rc" = 0 ]; then
		remote verify-image "--bytes $bytes --sha256 $sum" 2>&1 | tee -a "$LOG" || rc=${PIPESTATUS[0]}
	fi
	;;
*)
	remote "$CMD" "$*" 2>&1 | tee "$LOG" || rc=${PIPESTATUS[0]}
	;;
esac
echo "log: $LOG (exit $rc)"
exit "$rc"
