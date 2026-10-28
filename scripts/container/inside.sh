#!/bin/bash
# inside.sh <rom> [aes_keys.txt]: the build, in the container. /src is the
# repository, read only, /work the scratch space kept between builds, and
# /out where "Pocket Native" is put together.
set -euo pipefail

# the open-source system font from Azahar's archives, generated from them
# at build time rather than shipped
FONT_URL=https://raw.githubusercontent.com/azahar-emu/azahar/c9e1e946a0b5d30a58ae14dee83c1f203a49622d/externals/open_source_archives/include/shared_font.app.romfs.h

rom="$1"
keys="${2:-}"
cd /src

case "${rom,,}" in
    *.cia)
        [ -n "$keys" ] || { echo "a .cia needs your console's aes_keys.txt, give it with --keys"; exit 1; }
        echo "== decrypting the .cia"
        python3 tools/cia2cci.py "$rom" /work/game.3ds "$keys"
        game=/work/game.3ds
        ;;
    *.zcci)
        echo "== decompressing the .zcci (Z3DS)"
        rm -f /work/game.3ds
        tail -c +161 "$rom" | zstd -d -c > /work/game.3ds
        game=/work/game.3ds
        ;;
    *) game="$rom" ;;
esac
id=$(python3 tools/title_id.py "$game")
name=$(sed -n '/^name = "/{s/^name = "\(.*\)"$/\1/p;q}' "titles/$id/profile.toml" 2>/dev/null || true)
echo "== $id ${name:-(no profile yet)}"

echo "== building the recompiler and Pocket Native"
cargo build --release --locked -p recomp3ds --bin 3dsrecomp
cargo build --release --locked --target aarch64-unknown-linux-gnu -p pocket-native

echo "== recompiling the game for the handheld"
overrides=()
[ -d "titles/$id/overrides" ] && overrides=(--overrides "titles/$id/overrides")
RECOMP_CFLAGS=-DRECOMP_FASTMEM CC=aarch64-linux-gnu-gcc-8 "$CARGO_TARGET_DIR/release/3dsrecomp" build "$game" "/work/recomp/$id" "${overrides[@]}"

if [ ! -f /work/shared_font.bin ]; then
    echo "== making the system font"
    curl -sSfL "$FONT_URL" -o /work/shared_font.app.romfs.h
    python3 tools/make_shared_font.py /work/shared_font.app.romfs.h /work/shared_font.bin
fi

echo "== putting Pocket Native together"
top="/out/Pocket Native"
pn="$top/pocket-native"
mkdir -p "$pn/games/$id/sysdata"
aarch64-linux-gnu-strip -o "$pn/pocket-native" "$CARGO_TARGET_DIR/aarch64-unknown-linux-gnu/release/pocket-native"
rm -rf "$pn/titles" && cp -r titles "$pn/titles"
cp scripts/device/run.sh scripts/device/power.sh scripts/device/3ds.sh "$pn/"
cp LICENSE CREDITS.md assets/fonts/OFL.txt "$pn/"
python3 tools/third_party.py "$pn/THIRD_PARTY_LICENSES.txt" >/dev/null
aarch64-linux-gnu-strip -o "$pn/games/$id/title.so" "/work/recomp/$id/$id.so"
cp /work/shared_font.bin "$pn/games/$id/sysdata/shared_font.bin"
cp "$game" "$pn/games/$id/game.3ds"
# FAT cards and Windows take no colons and a few other characters in a name
launcher="$top/$(printf %s "${name:-$id}" | sed 's/: / - /g; s/[\\/:*?"<>|]//g').sh"
printf '#!/bin/bash\nexec "$(dirname "$0")/pocket-native/run.sh" %s\n' "$id" > "$launcher"
chmod +x "$launcher" "$pn/run.sh" "$pn/power.sh" "$pn/3ds.sh" "$pn/pocket-native"
echo "== done: copy \"Pocket Native\" into roms/ports on the handheld's card"
