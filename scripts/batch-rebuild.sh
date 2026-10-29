#!/bin/bash

{
set -euo pipefail

shopt -s nullglob
repo="$(cd "$(dirname "$0")/.." && pwd)"
check="" out="$repo/build" wait=""
games="$out/Pocket Native/pocket-native/games"
staging="$repo/.update/games"
restore() {
    for copy in "$staging"/*.3ds; do
        game="$games/$(basename "$copy" .3ds)/game.3ds"
        if [ -f "$game" ] && [ "$(stat -c %s "$game")" = "$(stat -c %s "$copy")" ]; then
            rm "$copy"
        else
            mkdir -p "$(dirname "$game")" && mv -f "$copy" "$game"
        fi
    done
}
restore
staged=1
trap 'exit 1' HUP INT TERM
built=("$games"/*/game.3ds)
if [ ${#built[@]} -eq 0 ]; then
    echo "== no games in $out to build again, build them as before"
    exit 0
fi
mkdir -p "$staging"
for game in "${built[@]}"; do
    mv "$game" "$staging/$(basename "$(dirname "$game")").3ds"
done
echo "== building ${#built[@]} game$([ ${#built[@]} = 1 ] || echo s) again"
status=0
bash "$repo/scripts/build.sh" "$staging"/*.3ds --out "$out" || status=$?
restore
rmdir "$staging" "$repo/.update" 2>/dev/null || true
[ $status -eq 0 ] && echo "== done: copy \"Pocket Native\" onto the card again, over the old one"
exit $status
}
