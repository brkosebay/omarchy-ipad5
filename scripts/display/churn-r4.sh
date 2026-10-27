#!/usr/bin/env bash
# Display round 4: buffer-leak and idle-CPU sampler. Runs ON THE iPad as the
# desktop user (passwordless sudo for debugfs, dmesg and pagetypeinfo), from
# an SSH shell; the orchestrator copies it over (docs/display/d1-d2-test-plan.md,
# "Round 4").
#
#   churn-r4.sh baseline              one sample (run with sddm stopped)
#   churn-r4.sh idle [SECONDS]        idle desktop: counter and CPU deltas (default 120)
#   churn-r4.sh churn [SECONDS] [--decode]
#                                     restart a gst-play/waylandsink window every
#                                     ~10 s for SECONDS (default 600), sampling every
#                                     third cycle between players; --decode adds the
#                                     soak's parallel v4l2slh264dec loop; cycle 1
#                                     and every 10th also sample mid-playback
#
# Output: /var/tmp/churn-r4/<mode>-<time>.log (full samples) and a summary on
# stdout. One "S ..." line per sample holds the key numbers; the summary
# compares the first sample after warm-up with the last.
set -uo pipefail

MODE=${1:-}
CLIPDIR=${CLIPDIR:-/var/tmp/vdec}
MIN_AVAIL_KB=${MIN_AVAIL_KB:-250000}	# watchdog: stop before the box thrashes
OUT=/var/tmp/churn-r4
mkdir -p "$OUT"
LOG="$OUT/${MODE:-none}-$(date +%H%M%S).log"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
TCK=$(getconf CLK_TCK)

sudo -n true || { echo "needs passwordless sudo" >&2; exit 2; }
ADIR=${ADIR:-$(sudo -n sh -c 'for d in /sys/kernel/debug/dri/*; do [ -e "$d/adbe_state" ] && { echo "$d"; break; }; done')}
[ -n "$ADIR" ] || { echo "no adbe_state: apple_adbe_drm not bound?" >&2; exit 2; }
sudo -n grep -q '^gem ' "$ADIR/adbe_state" || { echo "adbe_state has no gem line: not the round-4 kernel" >&2; exit 2; }

PLAYER_PID= DECODE_PID=
cleanup() {
	[ -n "$DECODE_PID" ] && kill "$DECODE_PID" 2>/dev/null
	[ -n "$PLAYER_PID" ] && kill "$PLAYER_PID" 2>/dev/null
	pkill -u "$(id -u)" -x gst-play-1.0 2>/dev/null
	pkill -u "$(id -u)" -f 'gst-launch-1.0 -q filesrc location=.*clip1080.h264' 2>/dev/null
}
trap 'cleanup; exit 130' INT TERM

kv() {	# kv KEY FILE-CONTENT: value of "KEY=value" (first match)
	printf '%s\n' "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -n 1
}

meminfo() { awk -v k="$1:" '$1 == k { v = $2 } END { print v + 0 }' /proc/meminfo; }
vmstat() { awk -v k="$1" '$1 == k { v = $2 } END { print v + 0 }' /proc/vmstat; }
ticks() { [ -r "/proc/$1/stat" ] && awk '{ print $14 + $15 }' "/proc/$1/stat" || echo 0; }

# Hyprland's mappings by kind: count and KiB of /dev/dri/card*, memfd, dmabuf.
maps_by_kind() {
	local pid=$1 start end kind
	local -A n=() kb=()
	while read -r range _ _ _ _ path; do
		case "$path" in
		/dev/dri/card*) kind=card ;;
		/memfd:*) kind=memfd ;;
		*dmabuf*) kind=dmabuf ;;
		*) continue ;;
		esac
		start=${range%-*} end=${range#*-}
		n[$kind]=$(( ${n[$kind]:-0} + 1 ))
		kb[$kind]=$(( ${kb[$kind]:-0} + (16#$end - 16#$start) / 1024 ))
	done < "/proc/$pid/maps"
	echo "card_maps=${n[card]:-0} card_kb=${kb[card]:-0} memfd_maps=${n[memfd]:-0} memfd_kb=${kb[memfd]:-0} dmabuf_maps=${n[dmabuf]:-0} dmabuf_kb=${kb[dmabuf]:-0}"
}

NSAMPLE=0 S=
sample() {	# sample LABEL: full dump to the log; the S line goes to $S and the log
	local label=$1 st hp hs mk gem vbl line1 s
	NSAMPLE=$((NSAMPLE + 1))
	st=$(sudo -n cat "$ADIR/adbe_state")
	gem=$(printf '%s\n' "$st" | sed -n 's/^gem //p')
	vbl=$(printf '%s\n' "$st" | sed -n 's/^vblank //p')
	line1=$(printf '%s\n' "$st" | head -n 1)
	hp=$(pgrep -o -x Hyprland || true)
	if [ -n "$hp" ]; then
		hs=$(awk '/^(VmSize|VmRSS|RssAnon|RssFile|RssShmem|VmPTE):/ { printf "%s=%s ", tolower($1), $2 }' "/proc/$hp/status" | tr -d ':')
		mk=$(maps_by_kind "$hp")
	else
		hs="vmsize=0 vmrss=0 rssanon=0 rssfile=0 rssshmem=0 vmpte=0 " mk="card_maps=0 card_kb=0 memfd_maps=0 memfd_kb=0 dmabuf_maps=0 dmabuf_kb=0"
	fi
	s="S n=$NSAMPLE label=$label t=$(cut -d' ' -f1 /proc/uptime)"
	s+=" cmafree=$(meminfo CmaFree) memavail=$(meminfo MemAvailable) memfree=$(meminfo MemFree) shmem=$(meminfo Shmem)"
	s+=" kicks=$(kv kicks "$line1") completions=$(kv completions "$line1") inflight=$(kv in-flight "$line1")"
	s+=" busy=$(kv busy "$line1") timeouts=$(kv timeouts "$line1")"
	s+=" gem_dumb=$(kv dumb "$gem") gem_import=$(kv import "$gem") orphans=$(kv orphans "$gem") mmaps=$(kv mmaps "$gem") fbs=$(kv fbs "$gem")"
	s+=" dumb_cma=$(kv dumb-in-cma "$gem") import_cma=$(kv import-in-cma "$gem")"
	s+=" vbl_ref=$(kv refcount "$vbl") vbl_on=$(kv enabled "$vbl") vbi=$(kv vbi "$vbl") vbl_events=$(kv vbl-events "$vbl")"
	s+=" hypr=${hp:-none} hypr_ticks=$(ticks "${hp:-0}") ${hs}${mk}"
	s+=" kswapd_ticks=$(ticks "$(pgrep -o -x kswapd0 || echo 0)")"
	s+=" pgmajfault=$(vmstat pgmajfault) pgscan_kswapd=$(vmstat pgscan_kswapd) pgscan_direct=$(vmstat pgscan_direct)"
	s+=" compact_stall=$(vmstat compact_stall) allocstall=$(awk '/^allocstall/ { n += $2 } END { print n + 0 }' /proc/vmstat)"
	s+=" players=$(pgrep -c -u "$(id -u)" -x gst-play-1.0 || true) cmafail=$(sudo -n dmesg | grep -c 'cma.*alloc failed')"
	{
		echo "=== sample $NSAMPLE ($label) $(date +%T)"
		echo "$s"
		echo "--- adbe_state"; echo "$st"
		echo "--- adbe_bos"; sudo -n cat "$ADIR/adbe_bos"
		echo "--- meminfo"; grep -E '^(MemTotal|MemFree|MemAvailable|Shmem|CmaTotal|CmaFree|AnonPages|Mapped|Unevictable):' /proc/meminfo
		echo "--- pagetypeinfo CMA (free blocks per order)"; sudo -n grep -E 'type +CMA' /proc/pagetypeinfo | head -n 2
		echo "--- top"; top -b -n 1 -o %CPU | sed -n '7,14p'
	} >> "$LOG"
	S=$s
}

field() {	# field S-LINE KEY, 0 if missing
	local v
	v=$(printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -n 1)
	echo "${v:-0}"
}
num() { local v=${1%%/*}; v=${v%K}; echo "${v:-0}"; }	# "3/36864K" -> 3, "36864K" -> 36864

check() {	# check NAME OK-CONDITION DETAIL
	if eval "$2"; then echo "PASS $1 ($3)"; else echo "FAIL $1 ($3)"; FAILS=$((FAILS + 1)); fi
}

summary_churn() {	# summary_churn FIRST-S-LINE LAST-S-LINE
	local a=$1 b=$2 d
	FAILS=0
	echo "--- summary: sample $(field "$a" n) (t=$(field "$a" t)) -> $(field "$b" n) (t=$(field "$b" t))"
	d=$(( $(field "$b" cmafree) - $(field "$a" cmafree) ))
	check cma-free "[ $d -ge -16384 ]" "delta ${d} kB, allowed >= -16384"
	d=$(( $(field "$b" vmrss) - $(field "$a" vmrss) ))
	check hyprland-rss "[ $d -le 20480 ]" "delta ${d} kB, allowed <= 20480"
	d=$(( $(field "$b" vmsize) - $(field "$a" vmsize) ))
	check hyprland-vmsize "[ $d -le 65536 ]" "delta ${d} kB, allowed <= 65536"
	d=$(( $(field "$b" card_maps) - $(field "$a" card_maps) ))
	check hyprland-card-maps "[ $d -le 1 ]" "delta $d, $(field "$b" card_kb) kB mapped"
	d=$(( $(field "$b" memfd_maps) - $(field "$a" memfd_maps) ))
	check hyprland-memfd-maps "[ $d -le 1 ]" "delta $d, $(field "$b" memfd_kb) kB mapped"
	d=$(( $(num "$(field "$b" gem_dumb)") - $(num "$(field "$a" gem_dumb)") ))
	check gem-dumb "[ $d -eq 0 ]" "$(field "$a" gem_dumb) -> $(field "$b" gem_dumb)"
	d=$(( $(num "$(field "$b" gem_import)") - $(num "$(field "$a" gem_import)") ))
	check gem-import "[ $d -le 0 ]" "$(field "$a" gem_import) -> $(field "$b" gem_import)"
	d=$(( $(num "$(field "$b" orphans)") - $(num "$(field "$a" orphans)") ))
	check gem-orphans "[ $d -le 0 ]" "$(field "$a" orphans) -> $(field "$b" orphans)"
	d=$(( $(field "$b" mmaps) - $(field "$a" mmaps) ))
	check gem-mmaps "[ $d -le 1 ]" "$(field "$a" mmaps) -> $(field "$b" mmaps)"
	d=$(( $(field "$b" fbs) - $(field "$a" fbs) ))
	check fbs "[ $d -eq 0 ]" "$(field "$a" fbs) -> $(field "$b" fbs)"
	check no-straggler-players "[ $MAXPLAYERS -eq 0 ]" "max $MAXPLAYERS players alive between runs"
	d=$(( $(field "$b" cmafail) - $(field "$a" cmafail) ))
	check no-cma-failures "[ $d -eq 0 ] || [ $DECODE = 1 ]" "+$d 'alloc failed' lines$( [ $DECODE = 1 ] && echo ', not judged with --decode')"
	d=$(( $(field "$b" kicks) - $(field "$b" completions) - $(field "$b" inflight) ))
	check kick-identity "[ $d -eq 0 ]" "kicks $(field "$b" kicks) = completions $(field "$b" completions) + in-flight $(field "$b" inflight)"
	check no-timeouts-busy "[ $(field "$b" timeouts) -eq 0 ] && [ $(field "$b" busy) -eq 0 ]" "timeouts $(field "$b" timeouts) busy $(field "$b" busy)"
	echo "RESULT $([ $FAILS -eq 0 ] && echo PASS || echo "FAIL ($FAILS)") log=$LOG"
}

case "$MODE" in
baseline)
	sample baseline; echo "$S"
	echo "log=$LOG"
	;;
idle)
	DUR=${2:-120}
	sample idle-start; a=$S
	sleep $((DUR / 2)); sample idle-mid
	sleep $((DUR - DUR / 2)); sample idle-end; b=$S
	echo "$a"; echo "$b"
	echo "--- idle ${DUR}s deltas"
	for k in kicks completions vbi vbl_events pgmajfault pgscan_kswapd pgscan_direct compact_stall allocstall; do
		echo "$k +$(( $(field "$b" $k) - $(field "$a" $k) ))"
	done
	echo "hyprland cpu $(( ($(field "$b" hypr_ticks) - $(field "$a" hypr_ticks)) * 100 / (TCK * DUR) ))%"
	echo "kswapd cpu $(( ($(field "$b" kswapd_ticks) - $(field "$a" kswapd_ticks)) * 100 / (TCK * DUR) ))%"
	echo "vblank at end: refcount $(field "$b" vbl_ref) enabled $(field "$b" vbl_on)"
	echo "log=$LOG"
	;;
churn)
	DUR=${2:-600} DECODE=0
	[ "${3:-}" = --decode ] && DECODE=1
	[ -r "$CLIPDIR/clip1080.mp4" ] || { echo "missing $CLIPDIR/clip1080.mp4" >&2; exit 2; }
	MAXPLAYERS=0
	sample start; first=$S; echo "$first"
	if [ $DECODE = 1 ]; then
		[ -r "$CLIPDIR/clip1080.h264" ] || { echo "missing $CLIPDIR/clip1080.h264" >&2; exit 2; }
		( while :; do
			gst-launch-1.0 -q filesrc location="$CLIPDIR/clip1080.h264" ! h264parse ! \
				v4l2slh264dec ! fakesink sync=false >/dev/null 2>&1
		done ) &
		DECODE_PID=$!
	fi
	end=$((SECONDS + DUR)) cycle=0 warm= last=$first
	while [ $SECONDS -lt $end ]; do
		cycle=$((cycle + 1))
		# The clip is 10 s: the player exits at EOS and closes its own window;
		# the timeout only catches a hung one.
		timeout -k 3 12 gst-play-1.0 --videosink=waylandsink "$CLIPDIR/clip1080.mp4" >/dev/null 2>&1 &
		PLAYER_PID=$!
		# While playing: CMA with a decoder up, and whether Hyprland imports
		# the player's buffers (gem_import). Not used by the summary.
		if [ $cycle -eq 1 ] || [ $((cycle % 10)) -eq 0 ]; then
			sleep 5; sample "playing$cycle"; echo "$S"
		fi
		wait "$PLAYER_PID"; PLAYER_PID=
		sleep 1	# let the compositor drop the window
		if [ $((cycle % 3)) -eq 0 ]; then
			sample "cycle$cycle"; last=$S; echo "$last"
			p=$(field "$last" players); [ "$p" -gt "$MAXPLAYERS" ] && MAXPLAYERS=$p
			# Warm-up: the first sample a minute in (Hyprland settles its buffers).
			[ -z "$warm" ] && [ "$(field "$last" t | cut -d. -f1)" -ge "$(( $(field "$first" t | cut -d. -f1) + 60 ))" ] && warm=$last
			if [ "$(field "$last" memavail)" -lt "$MIN_AVAIL_KB" ]; then
				echo "ABORT: MemAvailable $(field "$last" memavail) kB < $MIN_AVAIL_KB kB at cycle $cycle"
				break
			fi
		fi
	done
	cleanup; DECODE_PID=
	sleep 5
	sample after-stop; last=$S; echo "$last"
	summary_churn "${warm:-$first}" "$last"
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
