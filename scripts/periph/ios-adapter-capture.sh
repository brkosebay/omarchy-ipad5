#!/usr/bin/env bash
# iOS-side Lightning camera-adapter capture (ds-usb-host.md steps 1-3).
# Read-only: ioreg snapshots of the Tristar/accessory/USB-host state plus the
# kernel log, recorded on the iPad whenever anything changes. The Mac cable must
# be unplugged for the adapter, so the logger runs detached on the iPad and the
# owner swaps cables at their own pace (no countdown). Afterwards, with the Mac
# cable back in, `pull` fetches the logs.
#
#   IPAD_PW=... scripts/periph/ios-adapter-capture.sh start   # launch logger (up to 30 min)
#   scripts/periph/ios-adapter-capture.sh pull                # after the Mac cable is back
#   scripts/periph/ios-adapter-capture.sh stop                # end it early
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ios() { "$ROOT/scripts/ipad-ssh.sh" "$@"; }
R=/var/mobile/linux-a9/adapter-capture

case "${1:-}" in
start)
	: "${IPAD_PW:?export IPAD_PW in this environment}"
	ios "mkdir -p $R && cat > $R/logger.sh" <<'EOF'
#!/bin/sh
# Runs as root on the iPad. Snapshot on change, at most once per second.
R=/var/mobile/linux-a9/adapter-capture
PATH=/var/jb/sbin:/var/jb/usr/sbin:/var/jb/usr/bin:$PATH
OUT=$R/$(date +%Y%m%dT%H%M%S)
mkdir -p "$OUT"
echo "$OUT" > $R/current
rm -f $R/stop
end=$(( $(date +%s) + 1800 ))
prev=""
n=0
dmesg > "$OUT/dmesg-000-start.txt" 2>&1
while [ "$(date +%s)" -lt "$end" ] && [ ! -e $R/stop ]; do
	ioreg -l -w0 -p IOService -n AppleTriStarBuiltIn > "$OUT/.tristar" 2>&1
	ioreg -w0 -p IOService > "$OUT/.tree" 2>&1
	key=$(grep -E 'IOAccessory|TriStar|USBMode|ConnectType|DigitalID' "$OUT/.tristar" | cksum)$(grep -iE 'USB|Accessory|TriStar|Host' "$OUT/.tree" | cksum)
	if [ "$key" != "$prev" ]; then
		n=$((n + 1)); s=$(printf %03d $n); t=$(date +%H:%M:%S)
		echo "$t snapshot $s" >> "$OUT/events.txt"
		cp "$OUT/.tristar" "$OUT/$s-tristar.txt"
		cp "$OUT/.tree" "$OUT/$s-tree.txt"
		ioreg -l -w0 -p IOService -r -c IOUSBHostDevice > "$OUT/$s-usbhostdevice.txt" 2>&1
		ioreg -l -w0 -p IOService -r -c IOUSBHostInterface > "$OUT/$s-usbhostiface.txt" 2>&1
		ioreg -l -w0 -p IOService -r -c IOPortTransportStateUSB2 > "$OUT/$s-transportusb2.txt" 2>&1
		dmesg > "$OUT/$s-dmesg.txt" 2>&1
		prev="$key"
	fi
	sleep 1
done
dmesg > "$OUT/dmesg-999-end.txt" 2>&1
echo "$(date +%H:%M:%S) logger end" >> "$OUT/events.txt"
EOF
	printf '%s\n' "$IPAD_PW" | ios "sudo -S -p '' sh -c 'nohup sh $R/logger.sh > $R/logger.out 2>&1 < /dev/null &'"
	sleep 2
	ios "cat $R/current; cat \$(cat $R/current)/events.txt"
	;;
pull)
	cur=$(ios "cat $R/current")
	dst="$ROOT/build/periph/ios-adapter-capture/$(basename "$cur")"
	mkdir -p "$dst"
	IPAD_SCP=1 "$ROOT/scripts/ipad-ssh.sh" -r "mobile@127.0.0.1:$cur/" "$dst/.." >/dev/null
	echo "$dst"; cat "$dst/events.txt"
	;;
stop)
	ios "touch $R/stop"; echo stop requested
	;;
*) echo "usage: $0 start|pull|stop" >&2; exit 2 ;;
esac
