#!/bin/sh
# Host-only cross compilation. Does not deploy or contact a device.
set -eu
cd "$(dirname "$0")/../.."
if [ "$#" -gt 1 ]; then echo "usage: $0 [entitlements.plist]" >&2; exit 2; fi
entitlements=${1:-configs/power/ios-ioreport.plist}
[ -r "$entitlements" ] || { echo "Missing entitlements: $entitlements" >&2; exit 2; }
command -v ldid >/dev/null 2>&1 || { echo 'Install ldid before building an iOS helper.' >&2; exit 2; }
mkdir -p build/power
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 \
    -O2 -Wall -Wextra -Werror -framework CoreFoundation \
    scripts/power/ios-ioreport.c -o build/power/ios-ioreport
ldid -S"$entitlements" build/power/ios-ioreport
ldid -e build/power/ios-ioreport > build/power/ios-ioreport.plist
python3 - "$entitlements" build/power/ios-ioreport.plist <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as f: expected = plistlib.load(f)
with open(sys.argv[2], 'rb') as f: actual = plistlib.load(f)
if actual != expected: raise SystemExit('Signed entitlement readback mismatch')
PY
printf 'Built and signed with %s; readback: build/power/ios-ioreport.plist\n' "$entitlements"
