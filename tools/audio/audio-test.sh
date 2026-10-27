#!/bin/sh
# J71t speaker bring-up checks, run on the iPad from the HoolockRD shell.
#   audio-test.sh probe      drivers, card, controls, idle state (no MMIO)
#   audio-test.sh play       4 s low-volume tone + MCA status
#   audio-test.sh wav        play the test WAVs (needs SmartIO DMA)
#   audio-test.sh diag       PMGR states, pads, forced MCLK, codec registers
#   audio-test.sh variant W R   play with MCLK word W and switch route R
#                            ("off=val,..."), both set before the stream
#   audio-test.sh mclk VAL   play while forcing MCLK word VAL (e.g. 0x50)
#   audio-test.sh route N    play with i2s-switch route variant N (1-3)
#   audio-test.sh fifo       experimental: look for a CPU TX FIFO port
#   audio-test.sh rec [SECS] capture from the built-in mics (48k/44.1k
#                            S16, 48k S32) with levels + codec/MCA state
#   audio-test.sh jack snap NAME   save status regs, pads 54/64/66/68 and
#                            the jack state to /tmp/jack-NAME.txt (T1/T1b)
#   audio-test.sh jack diff A B    compare two snapshots
#   audio-test.sh jack poll MS     start the driver's status poll (0 = stop)
#   audio-test.sh jack force 1|0|auto   report a plug/unplug (debugfs)
#   audio-test.sh hp         headphone tone (forced plug), then speakers
# Needs: a9audio + devmem in PATH (or next to this script), debugfs.
# Every step is bounded (a9audio has poll timeouts; loops are finite).
#
# MMIO rule: a read of a power-gated Apple block can hang the SoC. So
#  - MCA0 registers only while MCA0 is runtime-active (each step that reads
#    them holds it with power/control = on, then checks runtime_status);
#  - the MCLK word and i2s-switch (power domain unknown) only while a
#    stream runs, the state in which the driver itself programs them;
#  - pads/peek only touch always-on blocks (GPIO pads, PMGR ps registers);
#  - debugfs s8000-mca-regs never powers anything up: it prints
#    "runtime-suspended" instead of reading registers.
set -u
D=$(dirname "$0")
PATH="$D:/usr/local/bin:$PATH"
WAV=${WAV:-/usr/share/a9audio}
[ -d "$WAV" ] || WAV=$D
MCA=0x20a0a0000
MCLK=0x20a002000
SW=0x20a003000
DBG=/sys/kernel/debug
MP=/sys/module/snd_soc_apple_s8000_mca/parameters
CP=/sys/module/snd_soc_cs42l81/parameters
GPIO=0x20f100000	# AP GPIO pads, 4 bytes per pin, always on

mountpoint -q $DBG 2>/dev/null || mount -t debugfs none $DBG 2>/dev/null

hex() { printf '0x%x' $(( $1 )); }
rd() { devmem "$(hex $1)" 32; }
wr() { devmem "$(hex $1)" 32 "$2"; }

MCADEV=$(ls -d /sys/bus/platform/devices/*.mca 2>/dev/null | head -1)
mca_state() { cat "$MCADEV/power/runtime_status" 2>/dev/null || echo unknown; }
mca_active() { [ "$(mca_state)" = active ]; }
# keep MCA0 powered for a step; runtime resume masks its IRQ first
mca_hold() { [ -n "$MCADEV" ] && echo on > "$MCADEV/power/control"; }
mca_release() { [ -n "$MCADEV" ] && echo auto > "$MCADEV/power/control"; }

# MCA0 register access: only while runtime-active
mrd() { if mca_active; then rd "$1"; else echo "(skipped: mca $(mca_state))"; fi; }
mwr() { if mca_active; then wr "$1" "$2"; else echo "(skipped write: mca $(mca_state))"; fi; }

# MCLK word / i2s-switch access: only while a playback stream runs
streaming() { grep -qs 'state: RUNNING' /proc/asound/card*/pcm*p/sub0/status; }
srd() { if streaming && mca_active; then rd "$1"; else echo "(skipped: no stream)"; fi; }
swr() { if streaming && mca_active; then wr "$1" "$2"; else echo "(skipped write: no stream)"; fi; }

irqline() { grep -iE 'mca' /proc/interrupts | tr -s ' '; }

snapshot() {	# $1 = label
	echo "--- regs: $1 (mca $(mca_state))"
	if [ -r $DBG/s8000-mca-regs ]; then grep -v '= 0x00000000$' $DBG/s8000-mca-regs
	else echo "(no debugfs s8000-mca-regs)"; fi
}

play_bg() {	# $1 = seconds; low volume tone, background
	a9audio tone -r 48000 -f 1000 -s "$1" -a -12 >/tmp/a9play.log 2>&1 &
	echo $!
}

# status read, clear, re-read 100 ms later (bit 7 BCLK, bit 9 FSYNC activity)
mca_status() {
	echo "mca irq status: $(mrd $MCA+0x1c)"
	mwr $MCA+0x1c 0x1fff; sleep 0.1
	echo "after clear+100ms: $(mrd $MCA+0x1c)  (bit7 bclk, bit9 fsync)"
}

step_probe() {	# no MMIO at all: dmesg, procfs, debugfs software state
	echo "=== 1. drivers"
	uname -a
	cat /proc/cmdline
	echo "--- SmartIO"
	dmesg | grep -iE 'smartio|apple-sio|sio-dma|20ae00000' | tail -20
	grep -iE 'sio|dma' $DBG/dmaengine/summary 2>/dev/null | head -20
	echo "--- audio"
	dmesg | grep -iE 'cs42l81|s8000-mca|spi-apple|apple-spi|asoc|simple-card|snd|irq.*nobody' | tail -40
	cat /proc/asound/cards 2>/dev/null || echo "no /proc/asound"
	ls -l /dev/snd 2>/dev/null
	echo "=== 2. controls"
	a9audio info
	echo "=== 3. idle state"
	echo "mca device: ${MCADEV:-none}, runtime $(mca_state)"
	echo "mca irq: $(irqline)"
	snapshot idle
	grep -E 'gpio-(16|64) ' $DBG/gpio 2>/dev/null
}

step_play() {
	echo "=== 2. 1 kHz tone, 4 s (audible with SmartIO DMA; discarded with nodma=1)"
	a9audio ctl 'Speaker Playback Volume' "${VOL:-150}"
	mca_hold
	pid=$(play_bg 4)
	sleep 1
	snapshot "playing"
	mca_status
	grep -E 'gpio-64 ' $DBG/gpio 2>/dev/null
	echo "--- dmaengine channels in use (expect one bound to 20a0a0000.mca)"
	grep -iE 'mca|in-use|sio' $DBG/dmaengine/summary 2>/dev/null | head -10
	wait $pid
	mca_release
	cat /tmp/a9play.log
	echo "mca irq: $(irqline)"
	dmesg | grep -iE 'cs42l81|s8000-mca|smartio|apple-sio|sio-dma|nobody cared' | tail -15
}

step_wav() {	# no MMIO
	a9audio ctl 'Speaker Playback Volume' "${VOL:-150}"
	for f in tone-1k-48k left-right-48k tone-1k-44k1 sweep-48k; do
		echo "=== $f"
		a9audio play "$WAV/$f.wav"
	done
	echo "=== 32-bit slots, 440 Hz, 2 s"
	a9audio tone -b 32 -f 440 -s 2 -a -20
	dmesg | grep -iE 'cs42l81|s8000-mca|smartio|apple-sio|sio-dma|nobody cared' | tail -15
}

step_diag() {
	echo "=== A. PMGR states mca0 sio_busif sio_p sio spi1 (bits 7:4 actual, 3:0 target)"
	for a in 0x20e080168 0x20e080150 0x20e080158 0x20e080160 0x20e0801c0; do
		a9audio peek $a 1
	done
	echo "=== B. pads idle (GPIO block only)"
	a9audio pads > /tmp/pads-idle.txt; grep TOGGLING /tmp/pads-idle.txt
	echo "=== C. MCLK forced on through debugfs (the driver powers MCA0 first), no stream"
	echo 1 > $DBG/s8000-mca-mclk; cat $DBG/s8000-mca-mclk
	a9audio pads > /tmp/pads-mclk.txt; grep TOGGLING /tmp/pads-mclk.txt
	for w in 0x50 0x60 0x40 0x18 0x58; do
		echo $w > $DBG/s8000-mca-mclk; echo "--- word $w"
		a9audio pads | grep TOGGLING
	done
	echo 0 > $DBG/s8000-mca-mclk; cat $DBG/s8000-mca-mclk
	echo "--- pad config changes idle -> MCLK forced"
	diff /tmp/pads-idle.txt /tmp/pads-mclk.txt | grep '^[<>] pin' | cut -c1-60 | head -20
	echo "=== D. stream (8 s): codec registers, pads, MCA status, switch reg15"
	a9audio ctl 'Speaker Playback Volume' "${VOL:-150}"
	mca_hold
	pid=$(play_bg 8)
	sleep 1
	cat $DBG/cs42l81-regs
	a9audio pads > /tmp/pads-play.txt; grep TOGGLING /tmp/pads-play.txt
	mca_status
	snapshot playing
	old=$(srd $SW+0x3c)
	case $old in 0x*)
		for v in 0x11 0x01 0x10; do
			swr $SW+0x3c $v; echo "--- switch reg15 (pin0) = $v"
			a9audio pads | grep TOGGLING
		done
		swr $SW+0x3c $old ;;
	*)	echo "switch reg15: $old" ;;
	esac
	wait $pid
	mca_release
	cat /tmp/a9play.log
	echo "mca irq: $(irqline)"
	dmesg | grep -iE 'cs42l81|s8000-mca|sio|nobody cared' | tail -12
}

step_variant() {	# $1 = MCLK word (0 = DT), $2 = route string ("" = DT)
	echo "$1" > $MP/mclk_ctl
	echo "${2:-}" > $MP/route
	mca_hold
	pid=$(play_bg 4)
	sleep 1
	echo "variant mclk=$1 route=${2:-dt}:"
	mca_status
	a9audio pads | grep TOGGLING
	grep -E '^(000f|0012|002e|002f|0033) ' $DBG/cs42l81-regs
	wait $pid
	mca_release
	cat /tmp/a9play.log
	echo 0 > $MP/mclk_ctl
	echo "" > $MP/route
}

step_mclk() {	# $1 = MCLK word value
	mca_hold
	pid=$(play_bg 4)
	sleep 0.5
	echo "MCLK word was $(srd $MCLK), forcing $1"
	swr $MCLK "$1"
	mca_status
	wait $pid
	mca_release
	cat /tmp/a9play.log
}

step_route() {	# $1 = variant
	case $1 in
	1) r0=0x00000018 r5=0x00080808 r15=0x10 ;;	# default (DT)
	2) r0=0x00181800 r5=0x00080818 r15=0x10 ;;	# swap data-field directions
	3) r0=0x00000018 r5=0x00080808 r15=0x11 ;;	# also let pin0 drive clocks
	*) echo "variant 1-3"; return ;;
	esac
	mca_hold
	pid=$(play_bg 4)
	sleep 0.5
	swr $SW+0x00 $r0; swr $SW+0x14 $r5; swr $SW+0x3c $r15
	echo "route $1: sw0=$(srd $SW) sw5=$(srd $SW+0x14) sw15=$(srd $SW+0x3c)"
	mca_status
	wait $pid
	mca_release
	cat /tmp/a9play.log
}

step_fifo() {	# MCA0 held on for the whole step
	[ -n "$MCADEV" ] || { echo "no MCA device"; return; }
	mca_hold
	mca_active || { echo "mca not active ($(mca_state))"; mca_release; return; }
	echo "baseline"; snapshot fifo-baseline > /tmp/base.txt; cat /tmp/base.txt
	# TX configured but not enabled: written words should stay in the FIFO
	mwr $MCA+0x0c 3; sleep 0.01
	mwr $MCA+0x60 0x136
	for off in 0x28 0x2c 0x30 0x34 0x38 0x3c 0x40 0x44 0x48 0x4c 0x50 0x54 0x58 0x5c \
		   0x70 0x74 0x78 0x7c 0x90 0x94 0x98 0x9c 0xa0 0xa4 0xa8 0xac; do
		for i in 1 2 3 4; do mwr $MCA+$off 0x1234; done
		snapshot "after 4 writes to $off" > /tmp/now.txt
		if ! diff -q /tmp/base.txt /tmp/now.txt >/dev/null; then
			echo "== $off changed:"; diff /tmp/base.txt /tmp/now.txt | grep '^[<>] mca'
		fi
		mwr $MCA+0x0c 3; sleep 0.01; mwr $MCA+0x60 0x136
		snapshot fifo-baseline > /tmp/base.txt
	done
	mca_release
}

capture_running() { grep -qs 'state: RUNNING' /proc/asound/card*/pcm*c/sub0/status; }

step_rec() {	# $1 = seconds per take; no MMIO beyond the held MCA0
	s=${1:-3}
	echo "=== capture: built-in mics (DMIC1A left, DMIC1B right)"
	a9audio ctl 'Capture Switch' 1
	[ -n "${CAPVOL:-}" ] && a9audio ctl 'Capture Volume' "$CAPVOL"
	mca_hold
	a9audio rec -r 48000 -s "$s" /tmp/mic-48k.wav > /tmp/a9rec.log 2>&1 &
	pid=$!
	sleep 1
	if capture_running; then echo "capture stream RUNNING"; else echo "capture stream NOT running"; fi
	snapshot capturing
	echo "--- codec (capture registers)"
	grep -E '^(000d|000e|0011|0012|0014|0015|0016|001e|001f|0020|0021|0115|0116|011f|0120|0124|0203|0204|0205|0208|020f|0210|0211|0212|0215|0216|002e|002f|0033) ' $DBG/cs42l81-regs
	grep -iE 'mca|sio|in-use' $DBG/dmaengine/summary 2>/dev/null | head -10
	wait $pid
	cat /tmp/a9rec.log
	a9audio rec -r 44100 -s "$s" /tmp/mic-44k1.wav
	a9audio rec -r 48000 -b 32 -s "$s" /tmp/mic-48k-s32.wav
	mca_release
	snapshot "after capture"
	dmesg | grep -iE 'cs42l81|s8000-mca|ep 0x29|sio-dma|nobody cared' | tail -15
}

jack_snap() {	# $1 = name
	f=/tmp/jack-$1.txt
	{
		echo "# $1 $(date +%T)"
		# the status registers are read to clear: one read per snapshot
		grep -E '^(000e|0201|0219|0220|022e|002e|002f|0033|007b|007c|0106|0528) ' $DBG/cs42l81-regs
		for p in 54 64 66 68; do
			printf 'gpio %d: ' $p
			a9audio peek $(hex "$GPIO + 4 * $p") 1 | cut -d' ' -f3
		done
		cat $DBG/cs42l81-jack 2>/dev/null
	} > "$f"
	cat "$f"
}

step_jack() {
	case "${1:-}" in
	snap)	jack_snap "${2:?name}" ;;
	diff)	diff "/tmp/jack-${2:?a}.txt" "/tmp/jack-${3:?b}.txt" && echo "no difference" ;;
	poll)	echo "${2:?ms}" > $CP/jack_poll_ms
		echo poll > $DBG/cs42l81-jack 2>/dev/null
		cat $DBG/cs42l81-jack ;;
	force)	echo "${2:?1|0|auto}" > $DBG/cs42l81-jack; cat $DBG/cs42l81-jack
		a9audio info | grep -iE 'jack|switch' ;;
	*)	echo "jack snap NAME | diff A B | poll MS | force 1|0|auto" ;;
	esac
}

step_hp() {	# headphones need a reported plug: forced here (T3 before T1)
	echo "=== headphones: forced plug, 1 kHz -12 dBFS at Headphone ${HPVOL:-40} (0.5 dB steps, 40 = -30 dB)"
	echo 1 > $DBG/cs42l81-jack
	a9audio ctl 'Headphone Playback Volume' "${HPVOL:-40}"
	a9audio ctl 'Headphone Playback Switch' 1
	mca_hold
	pid=$(play_bg 4)
	sleep 1.5
	grep -E '^(core_on|0201|0219|0220|0225|0226|0227|022e|0232|c96f) ' $DBG/cs42l81-regs
	wait $pid
	cat /tmp/a9play.log
	echo "=== speakers: forced unplug, same tone"
	echo 0 > $DBG/cs42l81-jack
	a9audio ctl 'Speaker Playback Volume' "${VOL:-150}"
	pid=$(play_bg 3)
	sleep 1.5
	grep -E '^(core_on|0219|0220|0225|0226|0227|022e|0232) ' $DBG/cs42l81-regs
	wait $pid
	mca_release
	cat /tmp/a9play.log
	echo auto > $DBG/cs42l81-jack
	dmesg | grep -iE 'cs42l81|headphones|speakers' | tail -10
}

case "${1:-}" in
probe)	step_probe ;;
play)	step_play ;;
wav)	step_wav ;;
diag)	step_diag ;;
variant) step_variant "${2:?word}" "${3:-}" ;;
mclk)	step_mclk "${2:?value}" ;;
route)	step_route "${2:?variant}" ;;
fifo)	step_fifo ;;
rec)	step_rec "${2:-3}" ;;
jack)	shift; step_jack "$@" ;;
hp)	step_hp ;;
*)	echo "usage: $0 probe|play|wav|diag|variant W R|mclk VAL|route N|fifo|rec [S]|jack ...|hp" ;;
esac
