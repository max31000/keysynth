#!/usr/bin/env bash
# Build keysynth.app and the release disk image (macOS arm64) — counterpart of scripts/build_installer.ps1.
#
#   out/keysynth-macos-<X.Y.Z>.dmg   versioned
#   out/keysynth-macos.dmg           fixed name (GitHub "latest/download" link, ui/src/platform/version.ts)
#
# The app is ad-hoc signed (no Apple Developer ID): the first start of a downloaded copy needs System Settings >
# Privacy & Security > "Open Anyway" (docs/TESTING.md "macOS release"). Requirements: scripts/configure.sh deps,
# scripts/fetch_faust.sh (else the app ships without Faust plugins), node + npm.
#
#   scripts/build_app_macos.sh                     # version from the vX.Y.Z tag on HEAD, else CMakeLists project()
#   scripts/build_app_macos.sh --version 1.2.3
#   scripts/build_app_macos.sh --no-build --skip-ui   # repackage an existing build
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
version="" build_dir="$repo/build-release" out="$repo/out" no_build=0 skip_ui=0 clean_ui=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --no-build) no_build=1; shift ;;
        --skip-ui) skip_ui=1; shift ;;
        --clean-ui) clean_ui=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- version ------------------------------------------------------------------------------------------------------
if [[ -z "$version" ]]; then
    tag="$(git -C "$repo" describe --tags --exact-match --match 'v[0-9]*' HEAD 2>/dev/null || true)"
    [[ "$tag" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]] && version="${BASH_REMATCH[1]}"
fi
if [[ -z "$version" ]]; then
    version="$(sed -nE 's/^project\(keysynth VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$repo/CMakeLists.txt")"
fi
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be X.Y.Z (got '$version')" >&2; exit 2; }
echo "keysynth $version"

# --- engine -------------------------------------------------------------------------------------------------------
if [[ $no_build -eq 0 ]]; then
    "$repo/scripts/configure.sh" "$build_dir" -DKS_VERSION="$version" -DKS_WITH_SFIZZ=ON -DKS_WITH_FAUST=ON
    cmake --build "$build_dir" --target keysynth-app --parallel
fi
exe="$build_dir/bin/Release/app/keysynth-engine"
[[ -x "$exe" ]] || { echo "$exe not found (build keysynth-app)" >&2; exit 1; }
got="$("$exe" --version 2>/dev/null || true)"
[[ "$got" == *"$version"* ]] || { echo "$exe reports '$got', expected $version (rebuild without --no-build)" >&2; exit 1; }

# --- UI -----------------------------------------------------------------------------------------------------------
ui="$repo/ui"
if [[ $skip_ui -eq 0 ]]; then
    if [[ $clean_ui -eq 1 || ! -d "$ui/node_modules" ]]; then npm --prefix "$ui" ci --no-audit --no-fund; fi
    # The installed fallback UI is served from http://127.0.0.1:7340/ -> base "/".
    npm --prefix "$ui" run build -- --base=/
fi
[[ -f "$ui/dist/index.html" ]] || { echo "ui/dist/index.html missing (drop --skip-ui)" >&2; exit 1; }

# --- stage keysynth.app -------------------------------------------------------------------------------------------
stage="$out/stage"
rm -rf "$stage"
app="$stage/keysynth.app"
contents="$app/Contents"
res="$contents/Resources"
mkdir -p "$contents/MacOS" "$res"

cp "$exe" "$contents/MacOS/keysynth-engine"
sed "s/@VERSION@/$version/g" "$repo/installer/macos/Info.plist.in" > "$contents/Info.plist"
plutil -lint "$contents/Info.plist" >/dev/null
printf 'APPL????' > "$contents/PkgInfo"
echo "keysynth $version installed (AppPaths installed mode)" > "$res/installed.flag"

# Icon: keysynth.icns from the generated PNGs (scripts/make_icon.py).
iconset="$out/keysynth.iconset"
rm -rf "$iconset" && mkdir -p "$iconset"
icon="$repo/assets/icon"
for n in 16 32 128 256 512; do
    cp "$icon/keysynth-$n.png" "$iconset/icon_${n}x${n}.png"
done
cp "$icon/keysynth-32.png" "$iconset/icon_16x16@2x.png"
cp "$icon/keysynth-64.png" "$iconset/icon_32x32@2x.png"
cp "$icon/keysynth-256.png" "$iconset/icon_128x128@2x.png"
cp "$icon/keysynth-512.png" "$iconset/icon_256x256@2x.png"
iconutil -c icns "$iconset" -o "$res/keysynth.icns"
rm -rf "$iconset"

# Faust: libfaust + the .lib files JIT imports resolve against (stdfaust.lib pulls in the others).
faust_dir="$(sed -nE 's/^KS_FAUST_DIR:PATH=(.*)$/\1/p' "$build_dir/CMakeCache.txt")"
if [[ -f "$faust_dir/lib/libfaust.2.dylib" ]]; then
    mkdir -p "$res/lib" "$res/share/faust"
    real="$(cd "$faust_dir/lib" && readlink libfaust.2.dylib || echo libfaust.2.dylib)"
    cp "$faust_dir/lib/$real" "$res/lib/libfaust.2.dylib"
    cp "$faust_dir"/share/faust/*.lib "$res/share/faust/"
    [[ -f "$faust_dir/share/faust/COPYING.txt" ]] && cp "$faust_dir/share/faust/COPYING.txt" "$res/licenses-faust-COPYING.txt" || true
else
    echo "warning: libfaust.2.dylib not found under '$faust_dir' (scripts/fetch_faust.sh): no Faust plugins" >&2
fi

copy_tree() { mkdir -p "$2" && cp -R "$1"/. "$2"/; }
copy_tree "$repo/presets/factory" "$res/presets/factory"
copy_tree "$repo/presets/patterns" "$res/presets/patterns"
copy_tree "$repo/presets/songs" "$res/presets/songs"
copy_tree "$ui/dist" "$res/ui/dist"
cp "$repo/LICENSE" "$res/LICENSE.txt"
cp "$repo/THIRD_PARTY_NOTICES.md" "$res/"
# Example plugins (Faust only, as on Windows): copied to the data dir on the first start (app/main.cpp).
for p in "$repo"/plugins/*/; do
    if compgen -G "$p*.dsp" >/dev/null; then copy_tree "$p" "$res/plugins/$(basename "$p")"; fi
done

# --- sign (ad-hoc) + disk image -------------------------------------------------------------------------------------
# Inside-out: the dylib first, then the bundle. No hardened runtime (it would need a Developer ID and the JIT /
# library-validation entitlements), so libfaust's JIT and user .dylib plugins load as in a dev build.
[[ -f "$res/lib/libfaust.2.dylib" ]] && codesign --force --sign - "$res/lib/libfaust.2.dylib"
codesign --force --sign - --identifier ru.mvv42.keysynth "$app"
codesign --verify --strict "$app"

ln -s /Applications "$stage/Applications"
mkdir -p "$out"
dmg="$out/keysynth-macos-$version.dmg"
rm -f "$dmg"
hdiutil create -quiet -volname "keysynth $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg"
cp "$dmg" "$out/keysynth-macos.dmg"
echo "built $dmg ($(du -h "$dmg" | cut -f1)) and $out/keysynth-macos.dmg"
