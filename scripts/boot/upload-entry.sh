#!/usr/bin/env bash
# Shared immutable entry transfer; usage: upload-entry.sh NAME DIRECTORY [--default]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
NAME=${1:?entry name} STAGE=${2:?directory} DEFAULT=0
[[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'invalid entry name' >&2; exit 2; }
[ "${3:-}" != --default ] || DEFAULT=1
# Hash only present files, never a previous manifest's checksum.
python3 -I - "$STAGE" <<'PY'
import hashlib, pathlib, sys
p = pathlib.Path(sys.argv[1])
files = sorted(f for f in p.iterdir() if f.is_file() and f.name != "SHA256SUMS")
(p / "SHA256SUMS").write_text("".join(
    f"{hashlib.sha256(f.read_bytes()).hexdigest()}  ./{f.name}\n" for f in files))
PY
remote=$("$SSH" 'mktemp -d /tmp/linux-a9-entry.XXXXXXXX')
[[ "$remote" =~ ^/tmp/linux-a9-entry\.[A-Za-z0-9]+$ ]] || { echo 'bad remote temporary path' >&2; exit 1; }
trap '"$SSH" "rm -rf -- $remote" >/dev/null 2>&1 || true' EXIT
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$STAGE" -cf - . |
    "$SSH" "tar -xf - -C $remote"
"$SSH" "sudo -n bash -s -- $remote $NAME $DEFAULT" <<'REMOTE'
set -euo pipefail
src=$1 name=$2 default=$3
base=/boot/linux-a9
mkdir -p "$base/entries" /run/linux-a9
exec 9>/run/linux-a9/install.lock
flock -w 120 9
cd "$src"
sha256sum -c --quiet SHA256SUMS
# Image-only uploads declare compatibility with the running DT and initrd.
current=$(sed -n 's/.*\blinux_a9.entry=\([^ ]*\).*/\1/p' /proc/cmdline)
if [[ "$current" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    for f in dtb initramfs.gz; do
        [ -f "$f" ] || [ ! -f "$base/entries/$current/$f" ] || cp "$base/entries/$current/$f" .
    done
fi
sha256sum ./* | sed '\| ./SHA256SUMS$|d' > /run/linux-a9/entry-sums
mv /run/linux-a9/entry-sums SHA256SUMS
d="$base/entries/$name"
if [ -d "$d" ]; then
    cmp -s SHA256SUMS "$d/SHA256SUMS" || { echo "entry $name exists with different contents; choose a new name" >&2; exit 1; }
    (cd "$d" && sha256sum -c --quiet SHA256SUMS)
else
    new=$(mktemp -d "$base/entries/.new.XXXXXXXX")
    trap 'rm -rf -- "$new"' EXIT
    cp -- * "$new/"
    chmod -R go-w "$new"
    sync
    mv -T "$new" "$d"
    trap - EXIT
fi
sync
echo "staged $d"
if [ "$default" = 1 ]; then /usr/local/bin/linux-a9-kexec set-default "$name"; fi
REMOTE
