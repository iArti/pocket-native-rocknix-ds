#!/bin/bash
# runtime.sh: Pocket Native without games, in the container, for a release
# to put over a "Pocket Native" folder a build made: the program, what runs
# it on the handheld and the licenses, with games and titles empty. /src is
# the repository, read only, /work the scratch space and /out where the
# pocket-native folder is put together.
set -euo pipefail
cd /src

echo "== building Pocket Native"
cargo build --release --locked --target aarch64-unknown-linux-gnu -p pocket-native

echo "== putting the pocket-native folder together"
pn=/out/pocket-native
rm -rf "$pn" && mkdir -p "$pn/games" "$pn/titles"
aarch64-linux-gnu-strip -o "$pn/pocket-native" "$CARGO_TARGET_DIR/aarch64-unknown-linux-gnu/release/pocket-native"
cp scripts/device/run.sh scripts/device/power.sh scripts/device/3ds.sh "$pn/"
cp LICENSE CREDITS.md assets/fonts/OFL.txt "$pn/"
python3 tools/third_party.py "$pn/THIRD_PARTY_LICENSES.txt" >/dev/null
chmod +x "$pn/run.sh" "$pn/power.sh" "$pn/3ds.sh" "$pn/pocket-native"
