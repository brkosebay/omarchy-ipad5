#!/usr/bin/env bash
# Install, remove or inspect the Mac LaunchAgent that runs
# scripts/boot/autoboot.sh in the background (docs/boot/warm-reboot.md).
#
#   scripts/boot/install-autoboot.sh            install and start
#   scripts/boot/install-autoboot.sh --remove   stop and remove
#   scripts/boot/install-autoboot.sh --status   launchctl state + last log lines
#
# Pause without removing: touch build/boot/autoboot.disabled
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL=com.linux-a9.autoboot
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

case "${1:-}" in
--remove)
	launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
	rm -f "$PLIST"
	echo "removed $LABEL"
	;;
--status)
	launchctl print "$DOMAIN/$LABEL" 2>/dev/null | grep -E "state =|pid =|last exit" || echo "$LABEL not loaded"
	tail -5 "$ROOT/build/boot/autoboot.log" 2>/dev/null || true
	;;
"")
	mkdir -p "$(dirname "$PLIST")" "$ROOT/build/boot"
	cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/bash</string>
		<string>$ROOT/scripts/boot/autoboot.sh</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
	</dict>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
	<key>ThrottleInterval</key><integer>10</integer>
	<key>StandardOutPath</key><string>$ROOT/build/boot/autoboot.out</string>
	<key>StandardErrorPath</key><string>$ROOT/build/boot/autoboot.out</string>
</dict>
</plist>
EOF
	plutil -lint "$PLIST" >/dev/null
	launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
	launchctl bootstrap "$DOMAIN" "$PLIST"
	echo "installed $PLIST; log: build/boot/autoboot.log"
	;;
*) sed -n '2,9p' "$0"; exit 2 ;;
esac
