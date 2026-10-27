#!/bin/busybox sh
# /init of the apple-vxd QEMU smoke initramfs (scripts/vdec/qemu-smoke.sh).
# Runs the V4L2 checks, the debugfs dumps and the firmware-boot paths, then
# reports kernel splats and powers off.  The boot is chosen on the cmdline:
#   (none)          V4L2 smoke, compliance, unbind/bind
#   vxd_boot_fw=1   debugfs boot trigger against plain RAM: must fail cleanly
#   vxd_fake_mtx=1  debugfs boot with scripts/vdec/vxd-fake-mtx.c playing
#                   the firmware: mailbox, pings, IRQ path, decode job,
#                   panic, unbind/bind with the IRQ armed
#   vxd_fake_mtx=1 vxd_gst=1
#                   the same fake firmware under GStreamer's v4l2slh264dec
#                   (Arch Linux ARM bundle in /gst, scripts/vdec/gst-bundle.sh)
#   vxd_fake_mtx=1 vxd_media=1
#                   the same under FFmpeg's V4L2 Request API hwaccel and
#                   stock mpv (round 7 bundle in /media,
#                   scripts/vdec/media-bundle.sh)
/bin/busybox --install -s /bin
export PATH=/bin:/usr/bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mount -t debugfs debugfs /sys/kernel/debug
D=/sys/kernel/debug/apple-vxd
DRV=/sys/bus/platform/drivers/apple-vxd

echo "=== VXD-SMOKE cmdline: $(cat /proc/cmdline)"
echo "=== VXD-SMOKE nodes:"
ls -l /dev/video* /dev/media* 2>&1
echo "=== VXD-SMOKE params:"
grep . /sys/module/apple_vxd/parameters/* 2>&1

rebind() {
	dev=$(basename "$(ls -d $DRV/*.vxd)")
	echo "=== VXD-SMOKE unbind/bind $dev"
	echo "$dev" > $DRV/unbind
	echo "=== VXD-SMOKE unbind rc=$? nodes: $(ls /dev/video* /dev/media* 2>/dev/null)"
	echo "$dev" > $DRV/bind
	echo "=== VXD-SMOKE bind rc=$?"
}

v4l2_smoke() {
	echo "=== VXD-SMOKE vdec-smoke"
	/bin/vdec-smoke /dev/video0 /dev/media0
	echo "=== VXD-SMOKE vdec-smoke rc=$?"
	echo "=== VXD-SMOKE vdec-h264-test without firmware (ERROR buffers expected)"
	/bin/vdec-h264-test -s grey64 -t 2000
	echo "=== VXD-SMOKE vdec-h264-test (no firmware) rc=$?"
	echo "=== VXD-SMOKE v4l2-ctl"
	v4l2-ctl -d0 --list-formats-out --list-formats-ext 2>&1
	media-ctl -d /dev/media0 -p 2>&1 | head -30
	echo 0 > /sys/module/apple_vxd/parameters/step_ioctls
	echo "=== VXD-SMOKE v4l2-compliance (step_ioctls=0)"
	v4l2-compliance -d /dev/video0 2>&1 | tail -45
	echo 1 > /sys/module/apple_vxd/parameters/step_ioctls
	# Round 1's crash site: remove() with V4L2 up, then a full re-probe
	# (self-test again) and the same smoke on the new nodes.
	rebind
	v=$(ls /dev/video* | head -1)
	m=$(ls /dev/media* | head -1)
	/bin/vdec-smoke "$v" "$m" | tail -1
}

# $1 = type, $2 = count
pings() {
	i=0
	ok=0
	while [ $i -lt "$2" ]; do
		echo "$1" > $D/ping && ok=$((ok + 1))
		i=$((i + 1))
	done
	echo "=== VXD-SMOKE ping $1 x$2 ok=$ok"
}

fake_mtx() {
	echo "=== VXD-SMOKE fake MTX on the RAM-backed registers"
	/bin/vxd-fake-mtx 0x7ff00000 0x08000000 100 &
	fake=$!
	sleep 1
	echo 1 > $D/boot
	echo "=== VXD-SMOKE fake boot rc=$?"
	cat $D/mbox
	cat $D/cmdbufs
	pings 0x83 20
	pings 0x84 6
	pings 0x80 1
	echo > $D/ping
	echo "=== VXD-SMOKE ping (empty) rc=$?"
	echo 0x99 > $D/ping
	echo "=== VXD-SMOKE ping 0x99 rc=$? (expect 1)"
	echo 1 > $D/consume_check
	echo "=== VXD-SMOKE consume_check rc=$?"
	echo "=== VXD-SMOKE mbox after pings"
	cat $D/mbox
	echo "=== VXD-SMOKE decode job with the mailbox up"
	/bin/vdec-smoke /dev/video0 /dev/media0 | tail -3
	echo "=== VXD-SMOKE vdec-h264-test against the fake firmware"
	/bin/vdec-h264-test -t 3000
	echo "=== VXD-SMOKE vdec-h264-test rc=$?"
	cat $D/cmdbufs
	cat $D/mbox | grep -E 'state|stats'

	pings 0x85 1
	echo 0x83 > $D/ping
	echo "=== VXD-SMOKE ping after panic rc=$?"
	echo "=== VXD-SMOKE mbox after panic"
	cat $D/mbox | grep -E 'state|stats'
	# fw_cleanup with the IRQ armed and a panicked firmware.
	rebind
	kill $fake
	wait $fake
	echo "=== VXD-SMOKE fake-mtx rc=$?"
}

# GStreamer from /gst with its own loader and libraries.
gst() {
	tool=$1
	shift
	GST_PLUGIN_SYSTEM_PATH_1_0=/gst/plugins GST_REGISTRY_1_0=/tmp/gst-registry.bin \
	GST_REGISTRY_FORK=no \
		/gst/lib/ld-linux-aarch64.so.1 --library-path /gst/lib /gst/bin/$tool "$@"
}

# $1 stream, $2 frame bytes (cropped NV12): decode to a file, then count the
# frames and the ones the fake firmware stamped "VXDFAKE!".
gst_file() {
	rm -f /tmp/$1.nv12
	GST_DEBUG=v4l2codecs*:3 gst gst-launch-1.0 -q filesrc location=/streams/$1.h264 ! \
		h264parse ! v4l2slh264dec ! video/x-raw,format=NV12 ! \
		filesink location=/tmp/$1.nv12 2>&1 | tail -5
	rc=$?
	size=$(stat -c %s /tmp/$1.nv12 2>/dev/null || echo 0)
	n=$((size / $2))
	i=0
	stamped=0
	while [ $i -lt $n ]; do
		[ "$(dd if=/tmp/$1.nv12 bs=$2 skip=$i count=1 2>/dev/null | head -c 8)" = VXDFAKE! ] &&
			stamped=$((stamped + 1))
		i=$((i + 1))
	done
	echo "=== VXD-SMOKE GST $1: rc=$rc bytes $size frames $n stamped $stamped"
	rm -f /tmp/$1.nv12
}

gst_run() {
	echo "=== VXD-SMOKE GStreamer against the fake firmware ($(cat /gst/VERSIONS | tr '\n' ' '))"
	/bin/vxd-fake-mtx 0x7ff00000 0x08000000 100 &
	fake=$!
	sleep 1
	echo 1 > $D/boot
	echo "=== VXD-SMOKE fake boot rc=$?"
	echo 0 > /sys/module/apple_vxd/parameters/step_ioctls
	gst gst-inspect-1.0 v4l2codecs 2>&1 | grep -E "v4l2sl|Version"
	gst gst-inspect-1.0 v4l2slh264dec 2>&1 | grep -E "Long-name|video/x-h264|stream-format|alignment|format:|Klass" | head -12
	echo "=== VXD-SMOKE gst-inspect rc=$?"
	found=""
	for e in v4l2slh264dec h264parse checksumsink fakevideosink fpsdisplaysink qtdemux playbin waylandsink; do
		gst gst-inspect-1.0 --exists $e && found="$found $e"
	done
	echo "=== VXD-SMOKE gst elements present:$found"
	gst_file hp176 38016
	gst_file wb176 38016
	# Scaling matrices: GStreamer must queue the SCALING_MATRIX control
	# (the driver refuses a flagged picture without it).
	gst_file hp176q 38016
	gst_file hp176m 38016
	gst_file fhd1080 3110400
	# The real-world clip (b-pyramid with MMCO, weightp/weightb, 8x8):
	# checksumsink prints one line per frame (the device's bit-exact check
	# against ffmpeg's framemd5), fpsdisplaysink the throughput.
	gst gst-launch-1.0 -q filesrc location=/streams/clip720.h264 ! h264parse ! \
		v4l2slh264dec ! checksumsink hash=md5 > /tmp/clip720.sum
	echo "=== VXD-SMOKE GST clip720 checksumsink rc=$? frames $(wc -l < /tmp/clip720.sum), first: $(head -1 /tmp/clip720.sum)"
	gst gst-launch-1.0 -v filesrc location=/streams/clip720.h264 ! h264parse ! \
		v4l2slh264dec ! fpsdisplaysink video-sink=fakevideosink text-overlay=false \
		sync=false 2>&1 | grep -o "rendered: [0-9]*, dropped: [0-9]*, current: [0-9.]*, average: [0-9.]*" | tail -1
	echo "=== VXD-SMOKE GST clip720 fpsdisplaysink rc=$?"
	cat $D/mbox | grep -E 'state|stats'
	cat $D/cmdbufs | head -3
	kill $fake
	wait $fake
	echo "=== VXD-SMOKE fake-mtx rc=$?"
}

# Round 7: FFmpeg (V4L2 Request API hwaccel) and stock mpv from /media,
# with their own loader and libraries (scripts/vdec/media-bundle.sh).
media() {
	tool=$1
	shift
	HOME=/tmp /media/lib/ld-linux-aarch64.so.1 --library-path /media/lib /media/bin/$tool "$@"
}

completions() {
	sed -n 's/.*completions \([0-9]*\).*/\1/p' $D/mbox | head -1
}

# $1 stream, $2 frame bytes (cropped NV12): -hwaccel v4l2request with the
# frames copied back to system memory (av_hwframe_transfer_data, the path
# mpv's v4l2request-copy takes), then count the frames and the stamped ones.
# A software fallback would decode real pixels: no stamps.
media_file() {
	rm -f /tmp/$1.nv12
	c0=$(completions)
	media ffmpeg -hide_banner -loglevel warning -hwaccel v4l2request -i /streams/$1.h264 \
		-f rawvideo -pix_fmt nv12 -y /tmp/$1.nv12 > /tmp/ffmpeg.log 2>&1
	rc=$?
	tail -3 /tmp/ffmpeg.log
	size=$(stat -c %s /tmp/$1.nv12 2>/dev/null || echo 0)
	n=$((size / $2))
	i=0
	stamped=0
	while [ $i -lt $n ]; do
		[ "$(dd if=/tmp/$1.nv12 bs=$2 skip=$i count=1 2>/dev/null | head -c 8)" = VXDFAKE! ] &&
			stamped=$((stamped + 1))
		i=$((i + 1))
	done
	echo "=== VXD-SMOKE FFMPEG $1${3:-}: rc=$rc bytes $size frames $n stamped $stamped completions +$(($(completions) - c0))"
	rm -f /tmp/$1.nv12
}

media_run() {
	echo "=== VXD-SMOKE media stack against the fake firmware ($(head -2 /media/VERSIONS | tr '\n' ' '))"
	/bin/vxd-fake-mtx 0x7ff00000 0x08000000 100 &
	fake=$!
	sleep 1
	echo 1 > $D/boot
	echo "=== VXD-SMOKE fake boot rc=$?"
	echo 0 > /sys/module/apple_vxd/parameters/step_ioctls
	echo "=== VXD-SMOKE ffmpeg hwaccels: $(media ffmpeg -hide_banner -hwaccels | tail -n +2 | tr '\n' ' ')"
	media_file hp176 38016
	media_file wb176 38016
	media_file hp176q 38016
	media_file hp176m 38016
	media_file fhd1080 3110400
	# Zero copy: DRM PRIME frames straight from the CAPTURE queue, nothing
	# mapped (what mpv's direct v4l2request mode and kmssink-style users get).
	c0=$(completions)
	media ffmpeg -hide_banner -loglevel warning -hwaccel v4l2request -hwaccel_output_format drm_prime \
		-i /streams/clip720.h264 -f null - > /tmp/ffmpeg.log 2>&1
	echo "=== VXD-SMOKE FFMPEG clip720 drm_prime rc=$? completions +$(($(completions) - c0))"
	tail -3 /tmp/ffmpeg.log
	# The device's bit-exact check: one md5 per frame, compared on the Mac
	# with the software decode (here only the count and the plumbing).
	c0=$(completions)
	media ffmpeg -hide_banner -loglevel warning -benchmark -hwaccel v4l2request \
		-i /streams/clip720.h264 -f framemd5 /tmp/clip720.framemd5 > /tmp/ffmpeg.log 2>&1
	echo "=== VXD-SMOKE FFMPEG clip720 framemd5 rc=$? frames $(grep -vc '^#' /tmp/clip720.framemd5) completions +$(($(completions) - c0))"
	grep -E 'bench' /tmp/ffmpeg.log | tail -2
	# mpv as Omarchy runs it: the system config (/etc/mpv/mpv.conf of the
	# device, here --config-dir) and no --hwdec on the command line.
	c0=$(completions)
	media mpv --no-input-terminal --config-dir=/media/etc/mpv --vo=null --ao=null --frames=300 \
		--msg-level=all=info,vd=v /streams/clip720.h264 > /tmp/mpv.log 2>&1
	echo "=== VXD-SMOKE MPV clip720 rc=$? completions +$(($(completions) - c0))"
	grep -E 'hardware decoding|Falling back|Could not create|v4l2request|Exiting' /tmp/mpv.log | head -8
	# mpv with the command-line option instead, and the software baseline.
	c0=$(completions)
	media mpv --no-config --no-input-terminal --vo=null --ao=null --frames=60 --hwdec=v4l2request-copy \
		/streams/clip720.h264 > /tmp/mpv.log 2>&1
	echo "=== VXD-SMOKE MPV clip720 --hwdec rc=$? completions +$(($(completions) - c0)) $(grep -o 'Using hardware decoding ([a-z0-9-]*)' /tmp/mpv.log)"
	c0=$(completions)
	media mpv --no-config --no-input-terminal --vo=null --ao=null --frames=60 --hwdec=no \
		/streams/clip720.h264 > /tmp/mpv.log 2>&1
	echo "=== VXD-SMOKE MPV clip720 --hwdec=no rc=$? completions +$(($(completions) - c0))"
	# capture_cached=1: non-coherent CAPTURE buffers for new contexts (QEMU
	# is coherent, so this checks the allocation/flag plumbing, not caches).
	echo 1 > /sys/module/apple_vxd/parameters/capture_cached
	media_file hp176 38016 " cached"
	media_file fhd1080 3110400 " cached"
	v4l2-compliance -d /dev/video0 2>&1 | grep -E "Total for|fail:" | tail -5
	echo "=== VXD-SMOKE compliance (capture_cached=1) done"
	echo 0 > /sys/module/apple_vxd/parameters/capture_cached
	cat $D/mbox | grep -E 'state|stats'
	kill $fake
	wait $fake
	echo "=== VXD-SMOKE fake-mtx rc=$?"
}

if grep -q vxd_media=1 /proc/cmdline; then
	media_run
elif grep -q vxd_gst=1 /proc/cmdline; then
	gst_run
elif grep -q vxd_fake_mtx=1 /proc/cmdline; then
	fake_mtx
elif [ -e /dev/video0 ]; then
	v4l2_smoke
fi

echo "=== VXD-SMOKE debugfs dumps (runtime PM get/put)"
cat $D/mbox 2>&1
cat $D/regs 2>&1 | head -8

if grep -q vxd_boot_fw=1 /proc/cmdline; then
	echo "=== VXD-SMOKE debugfs boot trigger (fake firmware, RAM-backed regs)"
	echo 1 > $D/boot
	echo "=== VXD-SMOKE boot trigger rc=$?"
	cat $D/cmdbufs
fi

echo "=== VXD-SMOKE STEP lines: $(dmesg | grep -c 'STEP ')"
echo "=== VXD-SMOKE kernel splats:"
# Report headers only (KASAN/lockdep/might_sleep all start with BUG:/WARNING:).
dmesg | grep -E 'BUG:|WARNING:|Oops|Unable to handle|Internal error|Kernel panic|double-free|use-after-free|nobody cared' || echo "none"
echo "=== VXD-SMOKE DONE"
# poweroff -f drops whatever the tty has not transmitted yet.
sync
sleep 2
poweroff -f
