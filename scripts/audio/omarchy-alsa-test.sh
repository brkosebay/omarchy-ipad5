#!/usr/bin/env bash
# iPad 5 (J71t) speaker test under Omarchy: pure ALSA (alsa-utils, python3
# ctypes on libasound), no a9audio/devmem. Runs on the device as root:
#
#   scp scripts/audio/omarchy-alsa-test.sh linux@172.16.42.1:/tmp/
#   ssh linux@172.16.42.1 'sudo bash /tmp/omarchy-alsa-test.sh check'
#
# Modes:
#   check  (default) NOT audible: card, PCM, mixer, PipeWire/udev/ACL state,
#          ACP-style hw_params probe, silent playback (S16/S32, 48k/44.1k,
#          RW/mmap) with a measured sample rate, debugfs and dmesg. The
#          speaker switch is off while it runs and restored afterwards.
#   tone   audible: 2 s 1 kHz tone, aplay -D hw:CARD,0 (VOL, default 164 = -20 dB)
#   lr     audible: left-only 440 Hz, then right-only 660 Hz, 1.5 s each
#   pw     audible through PipeWire (pw-play as the desktop user)
#   all    check, tone, lr
#   mic    capture from the built-in mics: arecord 3 s S16 48k/44.1k and
#          S32 48k, per-channel RMS/peak/DC, measured rate, codec state
#          (CAPVOL sets 'Capture Volume', 96 = 0 dB)
#   mic-sweep REG V1 V2 ...   one capture, REG poked to each value every
#          2 s through debugfs cs42l81-reg, levels per window (T2b/T2c)
#   hp     headphones: forced plug report, 2 s tone at HPVOL (default 40 =
#          -30 dB), then forced unplug and the same tone on the speakers
#   jack snap NAME | jack diff A B | jack poll MS | jack watch SECS
#          T1/T1b plug survey: codec status registers, hs-switch GPIOs,
#          jack control; poll = the driver's status poll (0 = off)
# Every step is bounded by timeout(1). If something holds the PCM (e.g.
# PipeWire), the direct-hw steps are skipped and the pw path is offered.
set -u
MODE=${1:-check}
VOL=${VOL:-164}
U=${SUDO_USER:-linux}
[ "$U" = root ] && U=linux
UIDN=$(id -u "$U" 2>/dev/null || echo 1000)
RT=/run/user/$UIDN
DBG=/sys/kernel/debug
FAILS=0

if [ "$(id -u)" != 0 ]; then
	echo "run as root: sudo bash $0 $MODE (needs debugfs and dmesg)"
	exit 1
fi
T=$(mktemp -d /tmp/a9alsa.XXXXXX)
chmod 755 "$T"
trap 'rm -rf "$T"' EXIT

step() { echo; echo "=== $*"; }
ok() { echo "  OK    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
note() { echo "  NOTE  $*"; }
to() { local s=$1; shift; timeout -k 2 "$s" "$@"; }
asuser() {	# run as the desktop user inside their PipeWire/systemd session
	to 10 runuser -u "$U" -- env XDG_RUNTIME_DIR="$RT" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus" "$@"
}
now() { date +%s.%N; }

mountpoint -q $DBG 2>/dev/null || mount -t debugfs none $DBG 2>/dev/null

CARD=$(awk '/iPad5 Speakers/ { print $1; exit }' /proc/asound/cards)
if [ -z "$CARD" ]; then
	echo "no 'iPad5 Speakers' card:"; cat /proc/asound/cards
	echo "dmesg:"; dmesg | grep -iE 'cs42l81|s8000-mca|apple-sio|sio-dma|asoc|simple-card' | tail -20
	exit 1
fi
DEV=hw:$CARD,0
PCM=/dev/snd/pcmC${CARD}D0p
PPCM=/proc/asound/card$CARD/pcm0p
D0=$(dmesg | wc -l)

# --- mixer save/restore ------------------------------------------------------
cget() { to 5 amixer -c "$CARD" cget "name='$1'" 2>/dev/null | sed -n 's/^ *: values=//p'; }
cset() { to 5 amixer -q -c "$CARD" cset "name='$1'" "$2"; }
SAVED_VOL=$(cget 'Speaker Playback Volume')
SAVED_SW=$(cget 'Speaker Playback Switch')
restore() {
	[ -n "$SAVED_VOL" ] && cset 'Speaker Playback Volume' "$SAVED_VOL"
	[ -n "$SAVED_SW" ] && cset 'Speaker Playback Switch' "$SAVED_SW"
	rm -rf "$T"
}
trap restore EXIT

# --- helpers -----------------------------------------------------------------
busy() {	# prints who holds the PCM; returns 0 if it is held
	local st pids p
	st=$(head -1 "$PPCM/sub0/status" 2>/dev/null)
	pids=$(to 5 fuser "$PCM" 2>/dev/null)
	[ "$st" = closed ] && [ -z "$pids" ] && return 1
	printf '  PCM %s (status: %s) held by:' "$PCM" "${st:-?}"
	for p in $pids; do printf ' %s(%s)' "$(cat /proc/$p/comm 2>/dev/null)" "$p"; done
	echo
	return 0
}

dbg_state() {	# $1 = label
	echo "  --- $1: s8000-mca-regs"
	if [ -r $DBG/s8000-mca-regs ]; then
		grep -v '= 0x00000000$' $DBG/s8000-mca-regs | sed 's/^/    /'
	else
		echo "    (no debugfs s8000-mca-regs)"
	fi
	echo "  --- $1: cs42l81-regs"
	if [ -r $DBG/cs42l81-regs ]; then
		sed 's/^/    /' $DBG/cs42l81-regs
	else
		echo "    (no debugfs cs42l81-regs)"
	fi
}

mktone() {	# out freq secs amp(dBFS) which(both|left|right)
	to 20 python3 -I - "$@" <<'EOF'
import math, struct, sys, wave
out, f, secs, db, which = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
rate, n = 48000, int(48000 * float(sys.argv[3]))
amp, fade = 32767 * 10 ** (db / 20), int(0.02 * 48000)
frames = bytearray()
for i in range(n):
    g = min(1.0, i / fade, (n - 1 - i) / fade)
    v = int(amp * g * math.sin(2 * math.pi * f * i / rate))
    frames += struct.pack('<hh', v if which != 'right' else 0, v if which != 'left' else 0)
with wave.open(out, 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(frames))
EOF
}

# silent stream with a measured rate: hw_ptr frames per second mid-stream
silent() {	# fmt rate rw|mmap [snap]
	local fmt=$1 rate=$2 acc=$3 snap=${4:-} secs=4 bpf=4 mm= pid rc h1 h2 s1 s2 t0 t1
	[ "$fmt" = S32_LE ] && bpf=8
	[ "$acc" = mmap ] && mm=-M
	t0=$(now)
	head -c $((rate * bpf * secs)) /dev/zero |
		to $((secs + 8)) aplay -q -D "$DEV" -t raw -f "$fmt" -c 2 -r "$rate" \
			-F 20000 -B 160000 $mm 2>"$T/aplay.err" &
	pid=$!
	sleep 1
	h1=$(awk '/^hw_ptr/ { print $3 }' "$PPCM/sub0/status" 2>/dev/null); s1=$(now)
	if [ -n "$snap" ]; then
		echo "  --- during the stream: $PPCM/sub0/hw_params"
		sed 's/^/    /' "$PPCM/sub0/hw_params" 2>/dev/null
		dbg_state "during the stream"
	fi
	sleep 2
	h2=$(awk '/^hw_ptr/ { print $3 }' "$PPCM/sub0/status" 2>/dev/null); s2=$(now)
	wait $pid; rc=$?
	t1=$(now)
	awk -v f="$fmt" -v r="$rate" -v a="$acc" -v rc="$rc" -v secs="$secs" \
	    -v h1="${h1:-0}" -v h2="${h2:-0}" -v s1="$s1" -v s2="$s2" -v t0="$t0" -v t1="$t1" '
	BEGIN {
		m = (s2 > s1 && h2 > h1) ? (h2 - h1) / (s2 - s1) : 0
		e = t1 - t0
		res = (rc == 0 && m > r * 0.98 && m < r * 1.02 && e < secs + 1.5) ? "OK  " : "FAIL"
		printf "  %s  %-6s %5d Hz %-4s rc %d, measured %.0f Hz (%+.2f%%), %d frames in %.2f s\n",
			res, f, r, a, rc, m, m ? (m / r - 1) * 100 : -100, r * secs, e
		exit(res == "FAIL")
	}' || { FAILS=$((FAILS + 1)); sed 's/^/        aplay: /' "$T/aplay.err"; }
}

# ACP-style probe: open hw with ACP's flags, exact format/rate, mmap, sizes
acp_probe() {
	cat > "$T/probe.py" <<'EOF'
import ctypes as C, sys
a = C.CDLL("libasound.so.2")
a.snd_strerror.restype = C.c_char_p
dev = sys.argv[1].encode()
FMT = {"S16_LE": 2, "S32_LE": 10}
ACC = {"mmap": 0, "rw": 3}
FLAGS = 0x10000 | 0x20000 | 0x40000   # NO_AUTO_RESAMPLE|CHANNELS|FORMAT, as ACP
U = C.c_ulong
fails = 0

def err(r):
    return a.snd_strerror(r).decode()

def sizes(pcm, hw, period, buf, buffer_first):
    p, d, b = U(period), C.c_int(0), U(buf)
    calls = [lambda: a.snd_pcm_hw_params_set_buffer_size_near(pcm, hw, C.byref(b)),
             lambda: a.snd_pcm_hw_params_set_period_size_near(pcm, hw, C.byref(p), C.byref(d))]
    for c in (calls if buffer_first else calls[::-1]):
        r = c()
        if r < 0:
            return r
    return 0

def probe(fmt, rate, acc, period, buf, label):
    global fails
    for buffer_first in (True, False):          # ACP tries both orders
        pcm, hw = C.c_void_p(), C.c_void_p()
        r = a.snd_pcm_open(C.byref(pcm), dev, 0, FLAGS)
        if r < 0:
            fails += 1
            return print(f"  FAIL  open {sys.argv[1]}: {err(r)}")
        a.snd_pcm_hw_params_malloc(C.byref(hw))
        what = ""
        for name, fn, args in (("any", a.snd_pcm_hw_params_any, ()),
                               ("access", a.snd_pcm_hw_params_set_access, (ACC[acc],)),
                               ("format", a.snd_pcm_hw_params_set_format, (FMT[fmt],)),
                               ("channels", a.snd_pcm_hw_params_set_channels, (2,)),
                               ("rate", a.snd_pcm_hw_params_set_rate, (rate, 0))):
            r = fn(pcm, hw, *args)
            if r < 0:
                what = f"set {name}: {err(r)}"
                break
        if not what:
            r = sizes(pcm, hw, period, buf, buffer_first)
            what = f"sizes: {err(r)}" if r < 0 else ""
        if not what:
            r = a.snd_pcm_hw_params(pcm, hw)
            what = f"commit: {err(r)}" if r < 0 else ""
        if not what:
            p, d, b = U(), C.c_int(), U()
            a.snd_pcm_hw_params_get_period_size(hw, C.byref(p), C.byref(d))
            a.snd_pcm_hw_params_get_buffer_size(hw, C.byref(b))
            print(f"  OK    {fmt:6} {rate:5} {acc:4} {label:12} period {p.value:5} "
                  f"buffer {b.value:6} batch {a.snd_pcm_hw_params_is_batch(hw)} "
                  f"pause {a.snd_pcm_hw_params_can_pause(hw)}")
        a.snd_pcm_hw_params_free(hw)
        a.snd_pcm_close(pcm)
        if not what:
            return
    fails += 1
    print(f"  FAIL  {fmt:6} {rate:5} {acc:4} {label:12} {what}")

for fmt, rate, acc in (("S16_LE", 48000, "mmap"), ("S32_LE", 48000, "mmap"),
                       ("S16_LE", 44100, "mmap"), ("S32_LE", 44100, "mmap"),
                       ("S16_LE", 48000, "rw")):
    probe(fmt, rate, acc, rate // 40, rate // 10, "acp-25ms")      # ACP default fragments
    probe(fmt, rate, acc, rate * 2, rate * 2, "acp-tsched")        # 2 s timer-sched buffer
    probe(fmt, rate, acc, 1024, 32768, "pw-1024")                  # PipeWire quantum
sys.exit(1 if fails else 0)
EOF
	to 30 python3 -I "$T/probe.py" "$DEV" || FAILS=$((FAILS + 1))
}

# --- steps ---------------------------------------------------------------------
step_info() {
	step "1. kernel, card, PCM devices"
	echo "  kernel: $(uname -r) $(uname -v)"
	sed 's/^/  /' /proc/asound/cards
	ls -l /dev/snd/ | sed 's/^/  /'
	[ -e "$PCM" ] && ok "$PCM present" || bad "$PCM missing"
	grep -E '^(id|name|subname|stream):' "$PPCM/info" 2>/dev/null | sed 's/^/  /'

	step "2. mixer (ACP's analog-output-speaker path uses the 'Speaker' element)"
	local caps
	to 5 amixer -c "$CARD" sget Speaker | sed 's/^/  /'
	caps=$(to 5 amixer -c "$CARD" sget Speaker | sed -n 's/.*Capabilities: //p')
	case " $caps " in
	*" pvolume"*" pswitch"*) ok "Speaker element has playback volume + switch ($caps)" ;;
	*) bad "no Speaker volume+switch element (capabilities: ${caps:-none})" ;;
	esac
	to 5 amixer -c "$CARD" cget "name='Speaker Playback Volume'" | grep -q dBscale &&
		ok "Speaker volume has a dB scale" || bad "Speaker volume has no dB scale"
}

step_userspace() {
	step "3. userspace audio stack (why PipeWire may show no devices)"
	local f
	for f in /usr/lib/spa-0.2/alsa/libspa-alsa.so \
		 /usr/share/alsa-card-profile/mixer/profile-sets/default.conf; do
		[ -e "$f" ] && ok "$f" || bad "missing $f (package pipewire-audio / alsa-card-profiles)"
	done
	to 10 pacman -Q pipewire wireplumber pipewire-audio alsa-card-profiles \
		pipewire-alsa pipewire-pulse 2>&1 | sed 's/^/  pacman: /'
	command -v pw-play >/dev/null && ok "pw-play present" ||
		note "pw-play missing (package pipewire-audio)"
	echo "  user $U ($UIDN): $(id -nG "$U" 2>/dev/null)"
	echo "  user units: $(asuser systemctl --user is-active pipewire.service \
		wireplumber.service pipewire-pulse.service 2>&1 | paste -sd' ')"
	echo "  --- wpctl status (as $U)"
	asuser wpctl status 2>&1 | sed -n '1,30p' | sed 's/^/    /'
	echo "  --- wireplumber log (this boot, last lines)"
	to 15 journalctl -b --no-pager -o cat _UID="$UIDN" _COMM=wireplumber 2>/dev/null |
		tail -8 | sed 's/^/    /'
	echo "  --- udev (PipeWire's ALSA monitor needs SOUND_INITIALIZED)"
	to 5 udevadm info -q property -p "/class/sound/card$CARD" 2>/dev/null |
		grep -E '^(SOUND_INITIALIZED|ID_PATH|ID_ID|SOUND_FORM_FACTOR|ACP_|PULSE_)' |
		sed 's/^/    /'
	echo "  --- device ACLs (uaccess for the active seat session)"
	to 5 getfacl -p "/dev/snd/controlC$CARD" "$PCM" 2>/dev/null |
		grep -E '^(# file|user:|group:)' | sed 's/^/    /'
	echo "  --- sessions"
	to 5 loginctl list-sessions --no-legend 2>/dev/null | sed 's/^/    /'
	echo "    seat0 $(to 5 loginctl show-seat seat0 -p ActiveSession 2>/dev/null)"
	echo "  --- UCM (no match expected: ACP then uses its profile sets)"
	to 5 alsaucm -c "hw:$CARD" list _verbs 2>&1 | head -3 | sed 's/^/    /'
}

step_hw() {	# returns 1 if the PCM is held
	step "4. is the PCM free?"
	if busy; then
		note "something holds the PCM: the direct hw:$CARD,0 steps are skipped."
		note "Play through PipeWire instead: sudo bash $0 pw"
		return 1
	fi
	ok "PCM closed, no holder"

	cset 'Speaker Playback Switch' off   # silent from here; restored on exit

	step "5. hw_params space of $DEV (aplay --dump-hw-params, 1 s silence)"
	head -c 192000 /dev/zero |
		to 10 aplay -D "$DEV" --dump-hw-params -t raw -f S16_LE -c 2 -r 48000 2>&1 |
		sed -n '/^HW Params/,/^TICK_TIME/p' | sed 's/^/  /'

	step "6. ACP-style probe: exact format/rate, mmap and RW, ACP/PipeWire sizes"
	acp_probe

	step "7. silent playback, 4 s each: sample rate measured from hw_ptr"
	silent S16_LE 48000 mmap snap
	silent S16_LE 48000 rw
	silent S32_LE 48000 mmap
	silent S16_LE 44100 mmap
	silent S16_LE 44100 rw
	silent S32_LE 44100 mmap
	return 0
}

step_after() {
	step "8. state after the streams"
	sleep 6   # DAPM powers the codec down 5 s after the last stream
	dbg_state "idle"
	echo "  --- new kernel messages since the script started"
	dmesg | tail -n +$((D0 + 1)) |
		grep -iE 'cs42l81|s8000-mca|apple-sio|sio-dma|smartio|nobody cared|Disabling IRQ|asoc|snd_soc' |
		tail -30 | sed 's/^/    /'
	echo "  --- audio messages since boot (last 15)"
	dmesg | grep -iE 'cs42l81|s8000-mca|apple-sio|sio-dma|smartio|nobody cared' |
		tail -15 | sed 's/^/    /'
}

play_wav() {	# file label
	echo "  >>> $2"
	to 10 aplay -D "$DEV" "$1" 2>&1 | sed 's/^/    /'
	[ "${PIPESTATUS[0]}" = 0 ] && ok "aplay rc 0" || bad "aplay failed"
}

step_tone() {
	step "tone: 2 s 1 kHz, -12 dBFS, Speaker volume $VOL, aplay -D $DEV"
	busy && { note "PCM held: use sudo bash $0 pw"; return; }
	mktone "$T/tone.wav" 1000 2 -12 both || { bad "tone generation"; return; }
	cset 'Speaker Playback Volume' "$VOL"
	cset 'Speaker Playback Switch' on
	play_wav "$T/tone.wav" "1 kHz, both speakers (2 s)"
	dmesg | tail -n +$((D0 + 1)) | grep -iE 'cs42l81|s8000-mca|sio' | tail -5 | sed 's/^/    /'
	dbg_state "after the tone"
}

step_lr() {
	step "lr: left-only 440 Hz, then right-only 660 Hz (1.5 s each), volume $VOL"
	busy && { note "PCM held: use sudo bash $0 pw"; return; }
	mktone "$T/left.wav" 440 1.5 -12 left && mktone "$T/right.wav" 660 1.5 -12 right ||
		{ bad "tone generation"; return; }
	cset 'Speaker Playback Volume' "$VOL"
	cset 'Speaker Playback Switch' on
	play_wav "$T/left.wav" "LEFT channel, low tone 440 Hz: which speaker?"
	sleep 1
	play_wav "$T/right.wav" "RIGHT channel, high tone 660 Hz: which speaker?"
}

step_pw() {
	step "pw: 2 s 1 kHz tone through PipeWire as $U"
	if ! command -v pw-play >/dev/null; then
		bad "pw-play is not installed. PipeWire has no ALSA support without pipewire-audio:"
		note "sudo pacman -S pipewire-audio pipewire-alsa pipewire-pulse"
		note "then as $U: systemctl --user restart pipewire wireplumber pipewire-pulse"
		return
	fi
	asuser wpctl status 2>&1 | sed -n '/Sinks:/,/^ *├─ Sources/p' | sed 's/^/    /'
	mktone "$T/tone.wav" 1000 2 -12 both || { bad "tone generation"; return; }
	chmod 644 "$T/tone.wav"
	asuser pw-play "$T/tone.wav" 2>&1 | sed 's/^/    /'
	[ "${PIPESTATUS[0]}" = 0 ] && ok "pw-play rc 0" || bad "pw-play failed"
	busy || true
}

CPCM=/dev/snd/pcmC${CARD}D0c
CP=/sys/module/snd_soc_cs42l81/parameters

levels() {	# wav: per-channel RMS/peak/DC of a 2-channel S16/S32 WAV
	to 20 python3 -I - "$1" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1])
ch, sw, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
data = w.readframes(n)
fmt = {2: 'h', 4: 'i'}[sw]
fs = float(1 << (8 * sw - 1))
v = struct.unpack('<%d%s' % (len(data) // sw, fmt), data)
print('    %d frames, %d ch, %d bit, %d Hz' % (n, ch, 8 * sw, w.getframerate()))
for c in range(ch):
    s = [x / fs for x in v[c::ch]]
    if not s:
        continue
    m = sum(s) / len(s)
    rms = math.sqrt(sum(x * x for x in s) / len(s))
    pk = max(abs(x) for x in s)
    db = lambda x: 20 * math.log10(x) if x > 0 else -999
    tag = '  SILENT' if pk == 0 else '  DC only' if rms - abs(m) < 1e-5 else ''
    print('    ch%d (%s): rms %.1f dBFS, peak %.1f dBFS, dc %+.3f%%%s' %
          (c, 'DMIC1A' if c == 0 else 'DMIC1B', db(rms), db(pk), 100 * m, tag))
EOF
}

step_mic() {
	step "mic: arecord from the built-in mics (DMIC1A = left, DMIC1B = right)"
	if [ ! -e "$CPCM" ]; then
		bad "no capture PCM $CPCM (kernel without capture, or MCA0 has no rx DMA)"
		return
	fi
	pids=$(to 5 fuser "$CPCM" 2>/dev/null)
	[ -n "$pids" ] && { note "capture PCM held by $pids (PipeWire?): stop it or use pw-record"; return; }
	cset 'Capture Switch' on
	[ -n "${CAPVOL:-}" ] && cset 'Capture Volume' "$CAPVOL"
	echo "  Capture Volume: $(cget 'Capture Volume')  (96 = 0 dB, 1 dB steps)"
	local f r t0 t1 rc
	for f in "S16_LE 48000" "S16_LE 44100" "S32_LE 48000"; do
		set -- $f
		echo "  >>> $1 $2 Hz, 3 s"
		t0=$(now)
		to 10 arecord -q -D "$DEV" -f "$1" -r "$2" -c 2 -d 3 "$T/mic-$1-$2.wav" 2>"$T/arec.err" &
		r=$!
		if [ "$1 $2" = "S16_LE 48000" ]; then
			sleep 1
			sed 's/^/    /' /proc/asound/card$CARD/pcm0c/sub0/hw_params 2>/dev/null
			dbg_state "during capture"
		fi
		wait $r; rc=$?
		t1=$(now)
		sed 's/^/    /' "$T/arec.err"
		if [ "$rc" = 0 ]; then
			ok "arecord rc 0 in $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }') s (3 s of data)"
		else
			bad "arecord rc $rc"
		fi
		levels "$T/mic-$1-$2.wav"
		cp "$T/mic-$1-$2.wav" /tmp/ 2>/dev/null
	done
	note "WAVs copied to /tmp/mic-*.wav (scp them back to listen)"
	dmesg | tail -n +$((D0 + 1)) | grep -iE 'cs42l81|s8000-mca|ep 0x29|sio|nobody cared' | tail -10 | sed 's/^/    /'
}

step_hp() {
	step "hp: forced plug, 2 s 1 kHz -12 dBFS at Headphone ${HPVOL:-40} (40 = -30 dB), then speakers"
	busy && { note "PCM held: stop PipeWire first"; return; }
	mktone "$T/tone.wav" 1000 2 -12 both || { bad "tone generation"; return; }
	echo 1 > $DBG/cs42l81-jack || { bad "no debugfs cs42l81-jack"; return; }
	cset 'Headphone Playback Volume' "${HPVOL:-40}"
	cset 'Headphone Playback Switch' on
	echo "  Headphone Jack: $(to 5 amixer -c "$CARD" cget iface=CARD,name='Headphone Jack' | sed -n 's/^ *: values=//p')"
	play_wav "$T/tone.wav" "headphones (speakers must stay silent)"
	grep -E '^(core_on|0219|0220|0225|0226|0227|022e|c96f) ' $DBG/cs42l81-regs | sed 's/^/    /'
	echo 0 > $DBG/cs42l81-jack
	cset 'Speaker Playback Volume' "$VOL"
	cset 'Speaker Playback Switch' on
	play_wav "$T/tone.wav" "speakers (headphones silent)"
	echo auto > $DBG/cs42l81-jack
	dmesg | tail -n +$((D0 + 1)) | grep -iE 'cs42l81|headphones|speakers' | tail -8 | sed 's/^/    /'
}

# one capture; REG is poked with each value at 2 s intervals (window 0 =
# the driver's value); prints the levels of every window
step_mic_sweep() {	# reg val...
	local reg=$1 n secs r v i=0
	shift
	n=$#
	secs=$((2 * n + 3))
	[ -e "$CPCM" ] || { bad "no capture PCM"; return; }
	step "mic-sweep: $reg over $*, ${secs} s capture S16_LE 48 kHz"
	cset 'Capture Switch' on
	to $((secs + 5)) arecord -q -D "$DEV" -f S16_LE -r 48000 -c 2 -d "$secs" "$T/sweep.wav" &
	r=$!
	sleep 3
	echo "$reg" > $DBG/cs42l81-reg; echo "    window 0: driver value $(cat $DBG/cs42l81-reg)"
	for v; do
		i=$((i + 1))
		echo "$reg $v" > $DBG/cs42l81-reg
		echo "    window $i: $(cat $DBG/cs42l81-reg)"
		sleep 2
	done
	wait $r
	to 20 python3 -I - "$T/sweep.wav" "$n" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1]); n = int(sys.argv[2]); rate = w.getframerate()
v = struct.unpack('<%dh' % (w.getnframes() * 2), w.readframes(w.getnframes()))
db = lambda x: 20 * math.log10(x) if x > 0 else -999
# window 0 = 0.5..3 s, window i = 3 + 2(i-1) + 0.3 .. 3 + 2i
for i in range(n + 1):
    a, b = (0.5, 3.0) if i == 0 else (3.3 + 2 * (i - 1), 3.0 + 2 * i)
    seg = v[int(a * rate) * 2:int(b * rate) * 2]
    out = []
    for c in (0, 1):
        s = [x / 32768.0 for x in seg[c::2]]
        rms = math.sqrt(sum(x * x for x in s) / len(s)) if s else 0
        out.append('ch%d rms %.1f peak %.1f' % (c, db(rms), db(max((abs(x) for x in s), default=0))))
    print('    window %d: %s' % (i, ', '.join(out)))
EOF
}

jack_snap() {	# name: one read of the latched status registers
	local f=/tmp/jack-$1.txt
	{
		echo "# $1 $(date +%T)"
		grep -E '^(000e|0201|0219|0220|022e|002e|002f|0033|007b|007c|0106|0528) ' $DBG/cs42l81-regs
		grep -E 'gpio-(54|64|66|68) ' $DBG/gpio 2>/dev/null
		cat $DBG/cs42l81-jack
		to 5 amixer -c "$CARD" cget iface=CARD,name='Headphone Jack' | sed -n 's/^ *: values=/jack control: /p'
	} > "$f"
	cat "$f"
}

step_jack() {
	case "${1:-}" in
	snap)	jack_snap "${2:?name}" ;;
	diff)	diff "/tmp/jack-${2:?a}.txt" "/tmp/jack-${3:?b}.txt" && echo "no difference" ;;
	poll)	echo "${2:?ms}" > $CP/jack_poll_ms; echo poll > $DBG/cs42l81-jack; cat $DBG/cs42l81-jack ;;
	watch)	to $((${2:-20} + 2)) dmesg -w | grep --line-buffered -iE 'cs42l81.*(status|headphones)' &
		sleep "${2:-20}"; kill $! 2>/dev/null; cat $DBG/cs42l81-jack ;;
	*)	echo "jack snap NAME | diff A B | poll MS | watch SECS"; exit 2 ;;
	esac
}

case "$MODE" in
check)	step_info; step_userspace; step_hw && step_after ;;
tone)	step_tone ;;
lr)	step_lr ;;
pw)	step_pw ;;
all)	step_info; step_userspace; step_hw && step_after; step_tone; step_lr ;;
mic)	step_mic ;;
mic-sweep) shift; step_mic_sweep "${1:?reg}" "${@:2}" ;;
hp)	step_hp ;;
jack)	shift; step_jack "$@" ;;
*)	echo "usage: sudo bash $0 check|tone|lr|pw|all|mic|hp|jack ...   (VOL=0..198, default 164 = -20 dB)"; exit 2 ;;
esac
echo
echo "=== done: $FAILS failure(s)"
[ "$FAILS" = 0 ]
