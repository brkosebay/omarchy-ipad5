#!/usr/bin/env bash
# Read-only upstream snapshots for docs/omarchy/rotation-blank.md.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
dest=${ROTATION_SOURCE_DIR:-$root/build/omarchy/rotation-src}
mkdir -p "$dest"
fetch() {
  local repo=$1 tag=$2 sha=$3 path=$dest/$1 owner=${4:-hyprwm}
  if [[ ! -d $path ]]; then
    git clone --quiet --depth 1 --branch "$tag" "https://github.com/$owner/$repo.git" "$path"
  fi
  [[ $(git -C "$path" rev-parse HEAD) == "$sha" ]] || {
    echo "Unexpected source revision: $path (expected $sha)" >&2
    return 1
  }
  [[ -z $(git -C "$path" status --porcelain) ]] || {
    echo "Source snapshot is modified: $path" >&2
    return 1
  }
  printf '%s %s %s\n' "$repo" "$tag" "$sha"
}
fetch aquamarine v0.15.1 f31c47a1b9d300847d8fc3108ad959448103dfc1
fetch Hyprland v0.56.2 efb50993780079460b0cbed1363e2166a2de1d9f
fetch quickshell v0.3.2 4f508be500dea6e5732cc3d50382a0048b17e7b1 quickshell-mirror
