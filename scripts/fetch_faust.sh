#!/usr/bin/env bash
# Fetch Faust (libfaust with the LLVM backend) for macOS arm64 into .tools/faust — counterpart of
# scripts/fetch_faust.ps1, same version and layout: bin/, include/faust/, lib/libfaust*.dylib, share/faust/.
# The official release dmg is mounted read-only and copied (static .a libraries skipped); nothing is installed.
# The download is cached in .deps/downloads. Worktrees use the main checkout's .tools/faust.
#
#   scripts/fetch_faust.sh           # no-op when the expected version is already there
#   scripts/fetch_faust.sh --force   # re-extract
set -euo pipefail

version="2.88.0"
repo="$(cd "$(dirname "$0")/.." && pwd)"
main="${repo%%/.claude/worktrees/*}"
dest="$main/.tools/faust"
dmg="Faust-$version-arm64.dmg"
url="https://github.com/grame-cncm/faust/releases/download/$version/$dmg"
downloads="$main/.deps/downloads"

if [[ "${1:-}" != "--force" && -x "$dest/bin/faust" ]] && "$dest/bin/faust" --version 2>/dev/null | grep -q "$version"; then
    echo "Faust $version already present: $dest"
    exit 0
fi

mkdir -p "$downloads"
if [[ ! -f "$downloads/$dmg" ]]; then
    echo "downloading $url"
    curl -fL --retry 3 -C - -o "$downloads/$dmg.part" "$url"
    mv "$downloads/$dmg.part" "$downloads/$dmg"
fi

mnt="$(mktemp -d "${TMPDIR:-/tmp}/faust-dmg.XXXXXX")"
cleanup() { hdiutil detach -quiet "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true; }
trap cleanup EXIT
hdiutil attach -nobrowse -readonly -noverify -mountpoint "$mnt" "$downloads/$dmg" >/dev/null
src="$mnt/Faust-$version"
[[ -f "$src/lib/libfaust.2.dylib" && -f "$src/include/faust/gui/CInterface.h" ]] ||
    { echo "error: unexpected dmg layout (no lib/libfaust.2.dylib or include/faust/gui/CInterface.h)"; exit 1; }

rm -rf "$dest"
mkdir -p "$dest"
# -a keeps the dylib version symlinks. Not copied: static libraries (~330 MB) and the other bin/ tools (~350 MB);
# keysynth only loads lib/libfaust.2.dylib at runtime and the compiler is kept for `faust --version` / manual use.
mkdir -p "$dest/bin"
cp "$src/bin/faust" "$dest/bin/"
rsync -a --exclude '*.a' "$src/include" "$src/lib" "$src/share" "$dest/"
xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true

"$dest/bin/faust" --version | head -1
echo "Faust $version -> $dest"
