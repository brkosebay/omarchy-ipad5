#!/bin/sh
# linux-a9 video decoder round 7: put the VXD to work for everyday apps on
# the iPad. Runs ON the iPad as root (sudo from the login user), from the
# directory staged by scripts/vdec/stage-r7-media.sh (copied over SSH: the
# iPad has no internet).
#
#   sudo ./install-media.sh            install (idempotent)
#   sudo ./install-media.sh --revert   back to Omarchy's stock state
#   sudo ./install-media.sh --status   what is installed now
#
# Install:
#  1. ffmpeg 2:9.0.2-2.1 (Arch Linux ARM's 9.0.2-2 + the V4L2 Request API
#     hwaccels, ABI-checked drop-in) with pacman -U, after sha256 checks
#  2. GStreamer's base/good/bad plugins (v4l2slh264dec, playbin, waylandsink,
#     kmssink) from gst/, the image's own versions, if staged
#  3. mpv: the marked block of mpv.conf into ~/.config/mpv/mpv.conf of the
#     login user (hwdec=v4l2request-copy for H.264). Not /etc/mpv/mpv.conf:
#     that file belongs to omarchy-settings and is not in its backup list,
#     so its next upgrade would put hwdec=auto-safe back silently
#  4. /etc/environment.d/60-apple-vxd-gstreamer.conf (v4l2slh264dec on top;
#     no package owns it)
#  5. the login user into "video" (SSH sessions; the seat session already
#     gets /dev/video0 and /dev/media0 through systemd's uaccess rule)
# Revert puts back the stock ffmpeg (rollback/), removes the mpv block (and
# the file if nothing else is in it) and the environment file; the
# GStreamer packages stay (they are stock).
set -eu
cd "$(dirname "$0")"
USER_NAME="${SUDO_USER:-linux}"
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
MPV="$USER_HOME/.config/mpv/mpv.conf"
BEGIN='# >>> linux-a9 apple-vxd'
END='# <<< linux-a9 apple-vxd'
ENVD=/etc/environment.d/60-apple-vxd-gstreamer.conf

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }

mpv_unblock() { # remove our marked block from $MPV
	[ -f "$MPV" ] || return 0
	sed -i "\|^$BEGIN|,\|^$END|d" "$MPV"
	grep -q '[^[:space:]]' "$MPV" || rm -f "$MPV"
}

status() {
	echo "ffmpeg: $(pacman -Q ffmpeg)"
	echo "hwaccels: $(ffmpeg -hide_banner -hwaccels | tail -n +2 | tr '\n' ' ')"
	echo "gstreamer: $(pacman -Q gstreamer gst-plugins-bad 2>&1 | tr '\n' ' ')"
	echo "mpv: $MPV: $(grep -E '^hwdec' "$MPV" 2>/dev/null | tr '\n' ' '); /etc/mpv/mpv.conf: $(grep -E '^hwdec' /etc/mpv/mpv.conf 2>/dev/null | tr '\n' ' ')"
	echo "environment.d: $(grep -v '^#' $ENVD 2>/dev/null | tr '\n' ' ')"
	echo "groups of $USER_NAME: $(id -nG "$USER_NAME")"
	ls -l /dev/video* /dev/media* 2>/dev/null || echo "no /dev/video* (kernel without apple-vxd?)"
}

case "${1:-}" in
--status)
	status
	exit 0
	;;
--revert)
	stock=$(ls rollback/ffmpeg-2:9.0.2-2-aarch64.pkg.tar.* 2>/dev/null | grep -v '\.sig$' | head -1 || true)
	if [ -n "$stock" ]; then
		(cd rollback && sha256sum -c --quiet SHA256SUMS)
		pacman -U --noconfirm "$stock"
	else
		echo "no rollback/ package: ffmpeg left as it is"
	fi
	mpv_unblock
	rm -f $ENVD
	status
	exit 0
	;;
"") ;;
*)
	echo "usage: $0 [--revert|--status]" >&2
	exit 2
	;;
esac

sha256sum -c --quiet SHA256SUMS
pkg=$(ls pkgs/ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.* | grep -v '\.sig$')
pacman -U --noconfirm --needed "$pkg"
if [ -f gst/list.txt ]; then
	(cd gst && sha256sum -c --quiet SHA256SUMS && pacman -U --noconfirm --needed $(cat list.txt))
else
	echo "no gst/ staged: GStreamer packages not installed (round 6 option B bundle still works)"
fi
if [ -f "$MPV" ] && sed "\|^$BEGIN|,\|^$END|d" "$MPV" | grep -qE '^[[:space:]]*hwdec='; then
	echo "$MPV already sets hwdec outside our block: left alone"
else
	mpv_unblock
	install -d -o "$USER_NAME" -g "$USER_NAME" "$(dirname "$MPV")"
	cat mpv.conf >> "$MPV"
	chown "$USER_NAME:$USER_NAME" "$MPV"
fi
install -D -m 644 60-apple-vxd-gstreamer.conf $ENVD
id -nG "$USER_NAME" | grep -qw video || usermod -aG video "$USER_NAME"
status
echo "install-media: done. The session picks up environment.d and the video group at the next login."
