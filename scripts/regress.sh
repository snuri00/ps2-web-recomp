#!/bin/bash
set -e
here=$(cd "$(dirname "$0")/.." && pwd)

usage() {
  cat <<'EOF'
usage: scripts/regress.sh record <game> [--frames n] [--press frame:button]... [--load-state file]
       scripts/regress.sh [<game>...]

record replays a headless run of games/<game> natively and keeps its cycle
count and the digests of its frames in games/<game>/regress.txt. Without
record, each named game (every game with a recording when none is named) runs
again the same way and is compared with its recording, so a change to the
runtime shows up as a difference. After a change to the translator, run
port.sh <game> <image> --no-explore --native-only first.

A recording holds for the machine it was made on: the frames come from its GPU.
EOF
}

elf_of() {
  local dir=$1 boot name elf
  elf=$(ls "$dir/patched/"* 2>/dev/null | head -1)
  [ -n "$elf" ] || elf="$dir/core.elf"
  if [ ! -f "$elf" ]; then
    boot=$(sed -n 's/^BOOT2 *= *//p' "$dir/disc/SYSTEM.CNF" | tr -d '\r')
    name=${boot#*\\}
    elf="$dir/disc/${name%;*}"
  fi
  echo "$elf"
}

replay() {
  local game=$1 dir="$here/games/$1" run="$here/build/native-$1/ps2run" out patches=()
  shift
  out=$(mktemp -d)
  ninja -C "$here/build/native-$game" ps2run > "$out/build.log" 2>&1 || { tail -20 "$out/build.log" >&2; rm -rf "$out"; return 1; }
  [ -f "$dir/patches.txt" ] && patches=(--patches patches.txt)
  mkdir "$out/frames"
  (cd "$dir" && nice "$run" "$(elf_of "$dir")" --disc disc.iso --memcard "$out/card.ps2" "${patches[@]}" "$@" --frames-dir "$out/frames" > "$out/run.log" 2>&1) || true
  sed -n 's/^\[ee\] stopped after \([0-9]*\) frames, \([0-9]*\) cycles: \(.*\)$/stopped \1 \2 \3/p' "$out/run.log"
  (cd "$out/frames" && for frame in *.ppm; do [ -f "$frame" ] && echo "$frame $(sha256sum < "$frame" | cut -d' ' -f1)"; done)
  rm -rf "$out"
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then usage; exit 0; fi

if [ "$1" = record ]; then
  [ $# -ge 2 ] || { usage; exit 2; }
  game=$2
  shift 2
  frames=3000 args=() presses=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --frames) frames=$2; shift ;;
      --press) args+=(--press "$2"); presses=1; shift ;;
      --load-state) args+=(--load-state "$(realpath "$2")"); shift ;;
      *) usage; exit 2 ;;
    esac
    shift
  done
  if [ "$presses" = 0 ]; then
    args+=(--press 300:start)
    for ((f = 600; f < frames; f += 300)); do args+=(--press "$f:cross"); done
  fi
  args=(--frames "$frames" --frame-every $((frames / 6 > 0 ? frames / 6 : 1)) "${args[@]}")
  recording="$here/games/$game/regress.txt"
  result=$(replay "$game" "${args[@]}")
  [ -n "$result" ] || { echo "$game: the run left no result" >&2; exit 1; }
  { printf 'run'; printf ' %q' "${args[@]}"; printf '\n%s\n' "$result"; } > "$recording"
  echo "$game: recorded $(echo "$result" | head -1 | cut -d' ' -f2-), $(($(echo "$result" | wc -l) - 1)) frames"
  exit 0
fi

games=("$@")
if [ ${#games[@]} -eq 0 ]; then
  for recording in "$here"/games/*/regress.txt; do
    [ -f "$recording" ] && games+=("$(basename "$(dirname "$recording")")")
  done
fi
[ ${#games[@]} -gt 0 ] || { echo "no recordings, make one with: scripts/regress.sh record <game>" >&2; exit 2; }

failed=0
for game in "${games[@]}"; do
  recording="$here/games/$game/regress.txt"
  [ -f "$recording" ] || { echo "$game: no recording"; failed=1; continue; }
  eval "args=($(head -1 "$recording" | cut -d' ' -f2-))"
  if changes=$(diff <(tail -n +2 "$recording") <(replay "$game" "${args[@]}")); then
    echo "$game: same"
  else
    echo "$game: differs"
    echo "$changes" | sed 's/^/  /'
    failed=1
  fi
done
exit $failed
