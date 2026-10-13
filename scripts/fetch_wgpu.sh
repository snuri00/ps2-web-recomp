#!/bin/sh
set -e
version=${1:-v29.0.1.1}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/wgpu-native"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)   platform=linux-x86_64 ;;
  Linux-aarch64)  platform=linux-aarch64 ;;
  Darwin-arm64)   platform=macos-aarch64 ;;
  Darwin-x86_64)  platform=macos-x86_64 ;;
  *) echo "plateforme non gérée: $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

mkdir -p "$target"
curl -fL -o "$target/wgpu.zip" "https://github.com/gfx-rs/wgpu-native/releases/download/$version/wgpu-$platform-release.zip"
unzip -o -q "$target/wgpu.zip" -d "$target"
rm "$target/wgpu.zip"
echo "wgpu-native $version ($platform) in $target"
