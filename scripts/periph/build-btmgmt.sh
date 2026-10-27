#!/usr/bin/env bash
# Build just BlueZ's management client: static AArch64/musl, no daemon/D-Bus.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION=5.85
SOURCE="$ROOT/build/periph/bluez-$VERSION.tar.xz"
EXPECTED=ad028e49254bc4551a13f08fe7904c63d02ba650d77be8ae15bb3b0a0ad94a6f
IMAGE=alpine:3.22
mkdir -p "$ROOT/build/periph"
python3 -B - "$SOURCE" "$EXPECTED" "$VERSION" <<'PY'
import hashlib
from pathlib import Path
import sys
import urllib.request
path, expected, version = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
if not path.exists():
    data = urllib.request.urlopen(f'https://www.kernel.org/pub/linux/bluetooth/bluez-{version}.tar.xz', timeout=60).read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit('BlueZ download checksum mismatch')
    path.write_bytes(data)
if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
    raise SystemExit('BlueZ source checksum mismatch')
PY
docker run --rm --platform linux/arm64 \
  -v "$ROOT/build/periph:/out" \
  -e VERSION="$VERSION" "$IMAGE" sh -eu -c '
apk add --no-cache build-base linux-headers pkgconf glib-dev dbus-dev \
  readline-dev readline-static ncurses-dev ncurses-static xz
cd /out
if [ ! -d "bluez-$VERSION" ]; then tar -xJf "bluez-$VERSION.tar.xz"; fi
cd "bluez-$VERSION"
./configure --disable-systemd --disable-udev --disable-obex --disable-cups \
  --disable-mesh --disable-midi --disable-manpages --disable-monitor \
  --disable-testing --disable-deprecated --enable-client \
  CFLAGS="-Os -ffunction-sections -fdata-sections" \
  LDFLAGS="-static -Wl,--gc-sections" LIBS="-lncursesw"
# Libtool -static only selects static libtool libraries; -all-static is
# needed for the executable and its readline/musl dependencies as well.
rm -f tools/btmgmt tools/.libs/btmgmt
make -j4 tools/btmgmt LDFLAGS="-all-static -Wl,--gc-sections"
strip tools/btmgmt
if readelf -l tools/btmgmt | grep -q INTERP; then
  echo "unexpected dynamic interpreter" >&2; exit 1
fi
if readelf -d tools/btmgmt | grep -q NEEDED; then
  echo "unexpected dynamic dependency" >&2; exit 1
fi
# A pipe gives the event loop pollable stdin; /dev/null is not pollable.
printf "\n" | timeout 5 ./tools/btmgmt --help > /out/btmgmt-help.txt
./tools/btmgmt --version
mkdir -p /out/rootfs/usr/bin /out/rootfs/usr/share/licenses/bluez
cp tools/btmgmt /out/rootfs/usr/bin/btmgmt
chmod 755 /out/rootfs/usr/bin/btmgmt
cp COPYING /out/rootfs/usr/share/licenses/bluez/COPYING
chmod 644 /out/rootfs/usr/share/licenses/bluez/COPYING
apk info -v > /out/btmgmt-build-packages.txt
readelf -h -l -d tools/btmgmt > /out/btmgmt-elf.txt
'
docker image inspect "$IMAGE" --format '{{json .RepoDigests}}' > "$ROOT/build/periph/btmgmt-build-image.json"
python3 -B - "$ROOT" "$VERSION" "$EXPECTED" <<'PY'
import hashlib
import json
from pathlib import Path
import struct
import sys
root, version, source_hash = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
path = root / 'build/periph/rootfs/usr/bin/btmgmt'
binary = path.read_bytes()
if binary[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', binary, 18)[0] != 183:
    raise SystemExit('expected little-endian ELF64 AArch64')
phoff = struct.unpack_from('<Q', binary, 32)[0]
phentsize, phnum = struct.unpack_from('<HH', binary, 54)
for i in range(phnum):
    entry = phoff + i * phentsize
    kind, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', binary, entry)
    if kind == 1 and (align < 16384 or (vaddr - offset) % 16384):
        raise SystemExit('ELF LOAD segment is incompatible with the A9 16K kernel')
report = {'version': version, 'source': f'https://www.kernel.org/pub/linux/bluetooth/bluez-{version}.tar.xz',
          'source_sha256': source_hash, 'binary': str(path.relative_to(root)),
          'size': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
          'architecture': 'AArch64', 'linkage': 'static musl; no PT_INTERP or DT_NEEDED',
          'checks': ['help and version executed in arm64 Linux container', 'readelf program/dynamic headers', 'ELF LOAD alignment compatible with 16K pages'],
          'runtime': 'kernel Bluetooth management socket; bluetoothd and D-Bus not required'}
(root / 'build/periph/btmgmt-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
PY
