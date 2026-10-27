#!/usr/bin/env bash
# Build the iOS arm64 SEP ART reader.
# Output: build/ios-sep-art/ios-sep-art (signed with sep.entitlements.plist)
set -eu
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
OUT="$ROOT/build/ios-sep-art"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$OUT"

echo "[*] SDK $SDK"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 \
    -isysroot "$SDK" -O2 -Wall -Wextra -Werror \
    -framework IOKit -framework CoreFoundation \
    ios-sep-art.c -o "$OUT/ios-sep-art"

if ! command -v ldid >/dev/null 2>&1; then
    echo "error: ldid not found (brew install ldid)" >&2
    exit 1
fi
ldid -Ssep.entitlements.plist "$OUT/ios-sep-art"

echo "[*] built:"
printf '    %-20s %8s bytes  sha256=%s\n' ios-sep-art \
    "$(wc -c < "$OUT/ios-sep-art" | tr -d ' ')" \
    "$(shasum -a 256 "$OUT/ios-sep-art" | cut -d' ' -f1)"
echo "[*] entitlements:"
ldid -e "$OUT/ios-sep-art"
