#!/usr/bin/env bash
# Builds games/<name>/templates from the stage code images on a Metal Gear Solid 2
# or 3 disc, so every stage's code is translated without playing to it (see
# docs/templates.md).
# usage: scripts/mgs_templates.sh <game folder under games/>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 1 ] || { echo "usage: scripts/mgs_templates.sh <game folder under games/>" >&2; exit 2; }
game="$ROOT/games/$1"
tools="$ROOT/build/tools"
ninja -C "$tools" mgs_stages eetemplates > /dev/null
images=$(mktemp -d)
trap 'rm -rf "$images"' EXIT
"$tools/mgs_stages" "$game/disc.iso" "$images"
rm -rf "$game/templates"
mkdir -p "$game/templates"
"$tools/eetemplates" "$(cat "$images/load.txt")" "$game/templates" "$images"/*.bin
