#!/usr/bin/env bash
# QEMU smoke test of the apple-vxd V4L2 paths for the committed a9/vdec HEAD.
#
# Builds a small KASAN+lockdep kernel from `git archive HEAD` of the vdec
# tree (the worktree, its .config and its objects stay untouched), an
# initramfs with busybox + v4l-utils + vdec-smoke, and boots QEMU virt with
# a RAM-backed stand-in VXD node (coreId/coreRev preloaded so probe
# passes; mem= keeps the guest kernel off that RAM).  This checks driver
# logic only (probe, the V4L2 self-test, open/ioctl/release, the media
# node, the no-firmware job path, debugfs, the firmware-boot error path,
# and the R3a/R3b mailbox + command-buffer paths against a userspace
# stand-in for the firmware); it says nothing about the real block.  No
# device is touched.
#
# Usage: scripts/vdec/qemu-smoke.sh [build|run|all|media]   (default all;
#        media = only the round-7 boot, after a build)
#   run boots four times:
#     default   V4L2 + vdec-smoke + v4l2-ctl/media-ctl + v4l2-compliance,
#               then unbind/bind (remove + re-probe) and vdec-smoke again
#     nov4l2    apple_vxd.v4l2=0: no nodes, probe + debugfs still work
#     fwboot    debugfs boot trigger with a fake 64 KiB firmware: the boot
#               recipe runs against RAM and must fail without a splat
#     fakemtx   the same boot with scripts/vdec/vxd-fake-mtx.c playing the
#               MTX through /dev/mem (2 vCPUs): ring geometry, the 0x80
#               init message, 28 pings through ring wrap, IRQ-delivered
#               completions (GICD_ISPENDR), a decode job with the mailbox
#               up, the 11 test streams, a firmware panic, unbind/bind
#               with the IRQ armed
#     gst       (with build/vdec-r6/gst from scripts/vdec/gst-bundle.sh)
#               the fake firmware under GStreamer 1.28's v4l2slh264dec:
#               five streams to files (every frame stamped by the fake;
#               two with scaling matrices),
#               a 300-frame 720p clip through fpsdisplaysink
#     media     (with build/vdec-r7/media from scripts/vdec/media-bundle.sh)
#               round 7: FFmpeg's V4L2 Request API hwaccel (five streams
#               to files, every frame stamped; drm_prime zero copy;
#               framemd5) and stock mpv with the iPad's /etc/mpv/mpv.conf
# Output: build/vdec-qemu/{Image,initramfs.gz,vxd.dtb,boot-*.log}
# Env: TREE (vdec), QEMU_ACCEL (hvf; tcg also works), TIMEOUT (s, 240),
#      MEDIA_TIMEOUT (s, 900), SMOKE_JOBS (kernel make -j, 4).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vdec-qemu"
TREE="${TREE:-vdec}"
MODE="${1:-all}"
ACCEL="${QEMU_ACCEL:-hvf}"
TIMEOUT="${TIMEOUT:-240}"
VXD_BASE=0x7ff00000
mkdir -p "$OUT"

if [ "$ACCEL" = hvf ]; then CPU=host; else CPU=max; fi
QEMU=(qemu-system-aarch64 -M virt,gic-version=3 -accel "$ACCEL" -cpu "$CPU"
      -m 1G -nographic -no-reboot)

build() {
	"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -e
Q=/src/tmp-vdec-qemu
mkdir -p $Q
head=$(git rev-parse HEAD)
if [ "$(cat $Q/COMMIT 2>/dev/null)" != "$head" ]; then
	rm -rf $Q/src && mkdir -p $Q/src
	# Docker'"'"'s VM disk is shared by every lane: leave out the sources of
	# other architectures and of big subsystems this allnoconfig build never
	# compiles (every Kconfig and Makefile stays: crypto/Kconfig sources each
	# arch/*/crypto/Kconfig), about 0.7 instead of 1.7 GB.
	P=""
	for d in alpha arc csky hexagon loongarch m68k microblaze mips nios2 openrisc \
		parisc powerpc riscv s390 sh sparc um x86 xtensa; do
		P="$P :(exclude,glob)arch/$d/**/*.[chS] :(exclude,glob)arch/$d/**/*.dts*"
	done
	for d in drivers/gpu/drm/amd drivers/gpu/drm/nouveau drivers/gpu/drm/i915 \
		drivers/gpu/drm/xe drivers/gpu/drm/radeon drivers/net/ethernet \
		drivers/net/wireless sound/soc drivers/staging drivers/scsi \
		drivers/infiniband drivers/media/i2c drivers/media/pci drivers/media/usb \
		drivers/media/dvb-frontends fs/bcachefs fs/xfs fs/btrfs; do
		P="$P :(exclude,glob)$d/**/*.[ch]"
	done
	P="$P :(exclude,glob)Documentation/**/*.rst :(exclude)tools/testing :(exclude)tools/perf"
	git archive HEAD -- . $P | tar -x -C $Q/src
	echo "$head" > $Q/COMMIT
fi
cd $Q/src
make -s LLVM=1 ARCH=arm64 KCONFIG_ALLCONFIG=/work/scripts/vdec/qemu-smoke.config allnoconfig
for s in VIDEO_APPLE_VXD KASAN PROVE_LOCKING SERIAL_AMBA_PL011_CONSOLE DEBUG_FS FW_LOADER PM; do
	grep -q "^CONFIG_$s=y" .config || { echo "qemu-smoke config: CONFIG_$s missing" >&2; exit 1; }
done
make -s -j'"${SMOKE_JOBS:-4}"' LLVM=1 ARCH=arm64 Image
cp arch/arm64/boot/Image /work/build/vdec-qemu/Image
echo "$head" > /work/build/vdec-qemu/COMMIT

dnf -y -q install busybox v4l-utils v4l-utils-devel-tools glibc-static >/dev/null 2>&1
R=$Q/rootfs
rm -rf $R && mkdir -p $R/bin $R/usr/bin $R/lib/firmware/apple
cp /usr/bin/busybox.musl.static $R/bin/busybox
# The tree'"'"'s own uapi (stateless H.264 controls) for the test programs.
H=$Q/uapi
rm -rf $H && make -s ARCH=arm64 headers_install INSTALL_HDR_PATH=$H >/dev/null
clang -O2 -Wall -Wextra -o $R/bin/vdec-smoke /work/scripts/vdec/vdec-smoke.c
clang -O2 -Wall -Wextra -o $R/bin/vxd-fake-mtx /work/scripts/vdec/vxd-fake-mtx.c
clang -O2 -Wall -Wextra -I$H/include -o $R/bin/vdec-h264-test /work/scripts/vdec/vdec-h264-test.c
# Static copies for the device (scripts/omarchy/ssh.sh upload, test plan).
clang -O2 -Wall -Wextra -static -o /work/build/vdec-qemu/vdec-smoke-static /work/scripts/vdec/vdec-smoke.c
clang -O2 -Wall -Wextra -static -I$H/include -o /work/build/vdec-qemu/vdec-h264-test-static /work/scripts/vdec/vdec-h264-test.c
for b in v4l2-ctl v4l2-compliance media-ctl; do
	p=$(command -v $b) || { echo "qemu-smoke: $b not installed" >&2; exit 1; }
	cp "$p" $R/usr/bin/
done
for b in $R/bin/busybox $R/bin/vdec-smoke $R/bin/vxd-fake-mtx $R/bin/vdec-h264-test $R/usr/bin/*; do
	libs=$(ldd "$b" 2>/dev/null | grep -o "/[^ ]*" || true)	# none if static
	for l in $libs; do
		mkdir -p "$R$(dirname "$l")" && cp -L "$l" "$R$l"
	done
done
# GStreamer (scripts/vdec/gst-bundle.sh), FFmpeg + mpv (round 7,
# scripts/vdec/media-bundle.sh) and the streams they decode.
if [ -d /work/build/vdec-r6/gst ] || [ -d /work/build/vdec-r7/media ]; then
	mkdir -p $R/streams
	for s in hp176 wb176 hp176q hp176m fhd1080 clip720; do
		cp /work/build/vdec-r6/streams/$s.h264 $R/streams/
	done
fi
[ -d /work/build/vdec-r6/gst ] && cp -a /work/build/vdec-r6/gst $R/gst
if [ -d /work/build/vdec-r7/media ]; then
	cp -a /work/build/vdec-r7/media $R/media
	# libraries linked by absolute path (media-bundle.sh ABSOLUTE)
	[ -d $R/media/abs ] && cp -a $R/media/abs/. $R/
	mkdir -p $R/media/etc/mpv
	cp /work/scripts/vdec/r7-media/mpv.conf $R/media/etc/mpv/mpv.conf
fi
cp /work/scripts/vdec/qemu-init.sh $R/init
chmod +x $R/init
# Fake DEVA image: right size class, wrong digest (the driver warns).
head -c 65536 /dev/zero > $R/lib/firmware/apple/vxd-deva.bin
(cd $R && find . | cpio -o -H newc --quiet | gzip -1) > /work/build/vdec-qemu/initramfs.gz
echo "qemu-smoke: kernel + initramfs for $head"
'
	# QEMU virt DTB plus the stand-in node.
	"${QEMU[@]}" -smp 2 -machine dumpdtb="$OUT/virt.dtb" >/dev/null
	"$ROOT/scripts/build-kernel.sh" --run "$TREE" "
set -e
cd /work/build/vdec-qemu
dtc -q -I dtb -O dts -o virt.dts virt.dtb
python3 -B -I /work/scripts/vdec/qemu-dtb.py virt.dts vxd.dts $VXD_BASE
dtc -q -I dts -O dtb -o vxd.dtb vxd.dts
rm -f virt.dts
"
}

boot() {
	local name=$1 extra=$2 smp=${3:-1} log="$OUT/boot-$1.log" pid i
	echo "--- qemu-smoke boot '$name' (cmdline extra: '$extra', $smp vCPU)"
	"${QEMU[@]}" -smp "$smp" -kernel "$OUT/Image" -initrd "$OUT/initramfs.gz" \
		-dtb "$OUT/vxd.dtb" \
		-append "console=ttyAMA0 mem=1023M panic=-1 $extra" \
		-device loader,addr=$((VXD_BASE + 0x630)),data=0x03030811,data-len=4 \
		-device loader,addr=$((VXD_BASE + 0x640)),data=0x00070b00,data-len=4 \
		</dev/null >"$log" 2>&1 &
	pid=$!
	for ((i = 0; i < TIMEOUT; i++)); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 1
	done
	if kill -0 "$pid" 2>/dev/null; then
		kill "$pid"
		echo "qemu-smoke: TIMEOUT after ${TIMEOUT}s" >>"$log"
	fi
	wait "$pid" || true
	grep -E '=== VXD-SMOKE|^SMOKE|^FAIL|^fake-mtx|apple-vxd.*(self-test|registered|skipped|failed|FAILED|consumed|PANIC|ready|refused)|Total for|TIMEOUT|rendered:' "$log" || true
	grep -A20 'kernel splats' "$log" | grep -m1 -q '^none' ||
		{ echo "qemu-smoke: '$name' has kernel splats, see $log"; return 1; }
}

# Every expectation of the fake-MTX boot; each miss is printed.
check_fakemtx() {
	local log="$OUT/boot-fakemtx.log" ok=0 pat
	for pat in \
		'fake boot rc=0' \
		'apple-vxd.*command buffers: 8 x 256 KiB' \
		'apple-vxd.*init message consumed after' \
		'apple-vxd.*DEVA firmware ready: mailbox up' \
		'fake-mtx: init message ok: .* seq 0' \
		'fake-mtx: init message ok: .* seq 1' \
		'ping 0x83 x20 ok=20' \
		'ping 0x84 x6 ok=6' \
		'ping 0x80 x1 ok=1' \
		'ping (empty) rc=0' \
		'ping 0x99 rc=1' \
		'stats .*sent 30 .*completions 6 unknown_fences 0 faults 0 contiguity 6 panics 0 unknown 0 malformed 0' \
		'consume_check rc=0' \
		'apple-vxd.*consume_check: stage 1: rd .*(unchanged: ok)' \
		'apple-vxd.*consume_check: stage 2: .*not taken (the reader waits for the doorbell)' \
		'apple-vxd.*consume_check: PASS' \
		'apple-vxd.*decode: picture refused' \
		'FRAME grey64 0 I (display 0): DONE, .* Y\[0..7\] 56 58 44 46 41 4b 45 21' \
		'FRAME ts176 2 P (display 2): DONE' \
		'FRAME ts176s4 1 P (multi-slice) (display 1): DONE' \
		'FRAME ts176cb 2 P (display 2): DONE' \
		'FRAME hp176 2 B (display 1): DONE, .* Y\[0..7\] 56 58 44 46 41 4b 45 21' \
		'FRAME hp176t 3 B (display 2): DONE' \
		'FRAME hp176q 6 B (display 5): DONE' \
		'FRAME hp176m 5 B (display 4): DONE' \
		'FRAME wp176 6 P (display 6): DONE' \
		'FRAME wb176 5 B (display 4): DONE' \
		'FRAME sl176 2 B (multi-slice) (display 1): DONE' \
		'stream hp176q: .*CABAC, 8x8, scaling lists' \
		'stream wb176: .*weighted P' \
		'H264 DONE-MISMATCH (11 streams)' \
		'vdec-h264-test rc=2' \
		'fake-mtx: decode slices 97 pictures 56 bad chunks 0' \
		'apple-vxd.*decode: completion: fence .*, 1 of 4 packets answered, status 0x0, .* -> DONE' \
		'apple-vxd.*decode: completion: fence .*, 1 of 9 packets answered, status 0x0, .* -> DONE' \
		'apple-vxd.*decode: push 0x81 fence .* (not armed)' \
		'apple-vxd.*STEP decode: src [0-9]* va 0x' \
		'apple-vxd.*STEP vb2: output buffer 0 GART-mapped' \
		'SMOKE PASS' \
		'apple-vxd.*FW_DEVA_PANIC: fence' \
		'ping 0x85 x1 ok=1' \
		'ping after panic rc=0' \
		'state .*fw_panicked 1' \
		'fake-mtx: summary boots 1 inits 2 nops 30 completions 62 panics 1 errors 0' \
		'fake-mtx rc=0'; do
		grep -q -- "$pat" "$log" || { echo "qemu-smoke: fakemtx missing: $pat"; ok=1; }
	done
	# Six 0x84 replies, plus the 0x85 ping's fence failed by the panic.
	[ "$(grep -c 'ping: completion for fence' "$log")" = 7 ] &&
		[ "$(grep -c 'ping: completion for fence.*(error)' "$log")" = 1 ] ||
		{ echo "qemu-smoke: fakemtx: expected 6 clean + 1 failed fence completions"; ok=1; }
	grep -q 'stats .*irqs [1-9]' "$log" ||
		{ echo "qemu-smoke: fakemtx: no interrupt was handled"; ok=1; }
	return $ok
}

# GStreamer's v4l2slh264dec on the fake firmware: every frame decoded and
# stamped, the clip played to the end, nothing malformed.
check_gst() {
	local log="$OUT/boot-gst.log" ok=0 pat
	for pat in \
		'fake boot rc=0' \
		'v4l2slh264dec' \
		'GST hp176: rc=0 bytes 266112 frames 7 stamped 7' \
		'GST wb176: rc=0 bytes 266112 frames 7 stamped 7' \
		'GST hp176q: rc=0 bytes 266112 frames 7 stamped 7' \
		'GST hp176m: rc=0 bytes 266112 frames 7 stamped 7' \
		'gst elements present: v4l2slh264dec h264parse checksumsink fakevideosink fpsdisplaysink qtdemux playbin waylandsink' \
		'GST fhd1080: rc=0 bytes 18662400 frames 6 stamped 6' \
		'GST clip720 checksumsink rc=0 frames 300, first: ' \
		'rendered: [0-9]*, dropped: 0' \
		'GST clip720 fpsdisplaysink rc=0' \
		'stats .*completions 634 .*faults 0 contiguity [0-9]* panics 0 unknown 0 malformed 0' \
		'fake-mtx: decode slices 634 pictures 634 bad chunks 0' \
		'fake-mtx rc=0'; do
		grep -q -- "$pat" "$log" || { echo "qemu-smoke: gst missing: $pat"; ok=1; }
	done
	grep -q 'apple-vxd.*decode: picture refused' "$log" &&
		{ echo "qemu-smoke: gst: a picture was refused"; grep 'picture refused' "$log" | head -3; ok=1; }
	return $ok
}

# Round 7: FFmpeg's v4l2request hwaccel and stock mpv on the fake firmware.
# Every frame of every file is stamped (decoded by the "hardware", copied
# back through the dmabuf), the completion counter moves by the frame
# count, mpv takes v4l2request-copy from the system config, and the
# software baseline moves nothing.
check_media() {
	local log="$OUT/boot-media.log" ok=0 pat
	for pat in \
		'fake boot rc=0' \
		'ffmpeg hwaccels: .*v4l2request' \
		'FFMPEG hp176: rc=0 bytes 266112 frames 7 stamped 7 completions +7[[:space:]]*$' \
		'FFMPEG wb176: rc=0 bytes 266112 frames 7 stamped 7 completions +7[[:space:]]*$' \
		'FFMPEG hp176q: rc=0 bytes 266112 frames 7 stamped 7 completions +7[[:space:]]*$' \
		'FFMPEG hp176m: rc=0 bytes 266112 frames 7 stamped 7 completions +7[[:space:]]*$' \
		'FFMPEG fhd1080: rc=0 bytes 18662400 frames 6 stamped 6 completions +6[[:space:]]*$' \
		'FFMPEG clip720 drm_prime rc=0 completions +300[[:space:]]*$' \
		'FFMPEG clip720 framemd5 rc=0 frames 300 completions +300[[:space:]]*$' \
		'MPV clip720 rc=0 completions +300[[:space:]]*$' \
		'Using hardware decoding (v4l2request-copy)' \
		'MPV clip720 --hwdec rc=0 completions +[6-9][0-9] Using hardware decoding (v4l2request-copy)' \
		'MPV clip720 --hwdec=no rc=0 completions +0[[:space:]]*$' \
		'FFMPEG hp176 cached: rc=0 bytes 266112 frames 7 stamped 7 completions +7[[:space:]]*$' \
		'FFMPEG fhd1080 cached: rc=0 bytes 18662400 frames 6 stamped 6 completions +6[[:space:]]*$' \
		'stats .*faults 0 .*panics 0 unknown 0 malformed 0' \
		'fake-mtx rc=0'; do
		grep -q -- "$pat" "$log" || { echo "qemu-smoke: media missing: $pat"; ok=1; }
	done
	grep -q 'apple-vxd.*decode: picture refused' "$log" &&
		{ echo "qemu-smoke: media: a picture was refused"; grep 'picture refused' "$log" | head -3; ok=1; }
	# Reported, not gated: capture_cached forces NON_COHERENT whatever
	# v4l2-compliance asks for (a performance knob, not a conformance claim).
	echo "qemu-smoke: media: compliance with capture_cached=1: $(grep -o 'Total for apple-vxd.*' "$log" | tail -1)"
	return $ok
}

run() {
	local rc=0
	boot default "" || rc=1
	# Two self-tests (probe + rebind) and two smoke passes.
	[ "$(grep -c 'v4l2 self-test ok' "$OUT/boot-default.log")" = 2 ] || rc=1
	[ "$(grep -c '^SMOKE PASS' "$OUT/boot-default.log")" = 2 ] || rc=1
	grep -q 'FRAME grey64 0 I (display 0): ERROR' "$OUT/boot-default.log" || rc=1
	grep -q 'vdec-h264-test (no firmware) rc=1' "$OUT/boot-default.log" || rc=1
	grep -q 'Total for apple-vxd .* Failed: 0' "$OUT/boot-default.log" || rc=1
	boot nov4l2 "apple_vxd.v4l2=0" || rc=1
	grep -q 'registration skipped' "$OUT/boot-nov4l2.log" || rc=1
	boot fwboot "apple_vxd.v4l2=0 vxd_boot_fw=1" || rc=1
	grep -q 'boot trigger rc=1' "$OUT/boot-fwboot.log" || rc=1
	grep -q 'mailbox self-test ok' "$OUT/boot-fwboot.log" || rc=1
	grep -q 'command-buffer self-test ok' "$OUT/boot-fwboot.log" || rc=1
	boot fakemtx "vxd_fake_mtx=1" 2 || rc=1
	check_fakemtx || rc=1
	if [ -d "$ROOT/build/vdec-r6/gst" ]; then
		boot gst "vxd_fake_mtx=1 vxd_gst=1" 2 || rc=1
		check_gst || rc=1
	else
		echo "qemu-smoke: no build/vdec-r6/gst (scripts/vdec/gst-bundle.sh): GStreamer boot skipped"
	fi
	if [ -d "$ROOT/build/vdec-r7/media" ]; then
		TIMEOUT=${MEDIA_TIMEOUT:-900} boot media "vxd_fake_mtx=1 vxd_media=1" 2 || rc=1
		check_media || rc=1
	else
		echo "qemu-smoke: no build/vdec-r7/media (scripts/vdec/media-bundle.sh): FFmpeg/mpv boot skipped"
	fi
	echo "qemu-smoke: $([ $rc = 0 ] && echo PASS || echo FAIL) ($(cat "$OUT/COMMIT"))"
	return $rc
}

case "$MODE" in
build) build ;;
run) run ;;
all) build && run ;;
media) TIMEOUT=${MEDIA_TIMEOUT:-900} boot media "vxd_fake_mtx=1 vxd_media=1" 2 && check_media ;;
*) echo "usage: $0 [build|run|all|media]" >&2; exit 2 ;;
esac
