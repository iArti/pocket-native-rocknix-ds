#!/bin/bash
# release.sh [--out dir]: the pocket-native folder without games, built in
# the kit's container, zipped as pocket-native-<version>.zip in the output
# directory, build/ by default, for a release.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$(realpath -m "$2")"; shift 2 ;;
        -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
        *) echo "usage: scripts/release.sh [--out dir]"; exit 2 ;;
    esac
done
engine=$(command -v podman || command -v docker) || { echo "the build needs podman or docker"; exit 1; }
image=pocket-native-build:$(sha256sum "$repo/scripts/container/Containerfile" | cut -c1-12)
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    echo "== making the build container, once"
    "$engine" build -f "$repo/scripts/container/Containerfile" -t "$image" "$repo/scripts/container"
fi
stage="$out/.release"
mkdir -p "$out/.work" "$stage"
user=(--user "$(id -u):$(id -g)")
[ "$(basename "$engine")" = podman ] && user=(--userns=keep-id)
"$engine" run --rm "${user[@]}" -v "$repo:/src:ro" -v "$out/.work:/work" -v "$stage:/out" \
    -e CARGO_HOME=/work/cargo -e CARGO_TARGET_DIR=/work/target -e HOME=/work \
    "$image" bash /src/scripts/container/runtime.sh

version=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$repo/Cargo.toml" | head -1)
zip="$out/pocket-native-$version.zip"
# zipped with the files' modes, and the empty folders kept
python3 - "$stage" "$zip" <<'PY'
import os, sys, zipfile
stage, path = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
    for top, folders, files in os.walk(os.path.join(stage, "pocket-native")):
        folders.sort()
        name = os.path.relpath(top, stage) + "/"
        entry = zipfile.ZipInfo(name)
        entry.external_attr = (0o40755 << 16) | 0x10
        archive.writestr(entry, "")
        for file in sorted(files):
            archive.write(os.path.join(top, file), os.path.relpath(os.path.join(top, file), stage))
PY
echo "== done: $zip"
