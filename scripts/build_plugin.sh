#!/usr/bin/env bash
# Build a C++ keysynth plugin (C ABI, sdk/include/keysynth/plugin_abi.h) on macOS — counterpart of
# scripts/build_plugin.ps1, same layout and rules (docs/PLUGINS.md).
#
# Compiles every .cpp in <plugins>/<name>/ with clang++ into <plugins>/.build/<name>-<hash>.dylib, where <hash>
# covers the sources, local headers, the ABI header and the flags. The library is written under a temporary name
# and renamed, so a running engine never loads a half-written file; the engine watches .build/ and hot-reloads the
# newest <name>-*.dylib. Exit codes: 0 ok (or up to date), 1 compile/link error, 2 bad arguments, 3 no compiler.
#
#   scripts/build_plugin.sh cpp_bitcrusher
#   scripts/build_plugin.sh my_fx --config Debug
#   scripts/build_plugin.sh my_fx --plugins-dir /tmp/plugins      # tests: build outside the repo
set -uo pipefail

usage() { echo "usage: build_plugin.sh <name> [--plugins-dir DIR] [--out-dir DIR] [--config Release|Debug] [--flag F]..." >&2; exit 2; }

[[ $# -ge 1 ]] || usage
name="$1"; shift
plugins_dir="" out_dir="" config="Release" extra=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --plugins-dir) plugins_dir="${2:-}"; shift 2 ;;
        --out-dir) out_dir="${2:-}"; shift 2 ;;
        --config) config="${2:-}"; shift 2 ;;
        --flag) extra+=("${2:-}"); shift 2 ;;
        *) usage ;;
    esac
done
[[ "$name" =~ ^[a-z][a-z0-9_]{0,63}$ ]] || { echo "error: invalid plugin name '$name' (expected [a-z][a-z0-9_]*)" >&2; exit 2; }
[[ "$config" == "Release" || "$config" == "Debug" ]] || usage

repo="$(cd "$(dirname "$0")/.." && pwd)"
[[ -n "$plugins_dir" ]] || plugins_dir="$repo/plugins"
mkdir -p "$plugins_dir" && plugins_dir="$(cd "$plugins_dir" && pwd)"
[[ -n "$out_dir" ]] || out_dir="$plugins_dir/.build"
src_dir="$plugins_dir/$name"
[[ -f "$src_dir/$name.cpp" ]] || { echo "error: $src_dir/$name.cpp not found"; exit 2; }
sdk_include="$repo/sdk/include"
abi_header="$sdk_include/keysynth/plugin_abi.h"

cxx="${CXX:-clang++}"
command -v "$cxx" >/dev/null || { echo "error: $cxx not found (install the Xcode command line tools: xcode-select --install)"; exit 3; }

# --- content hash (sources + local headers + ABI header + flags) ---------------------------------------------
# .cpp files at the top level are compiled; headers anywhere below the plugin dir are hashed.
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$src_dir" -maxdepth 1 -type f -name '*.cpp' | LC_ALL=C sort)
headers=()
while IFS= read -r f; do headers+=("$f"); done < <(find "$src_dir" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inl' \) | LC_ALL=C sort)
hash="$(
    {
        for f in "${sources[@]}" ${headers[@]+"${headers[@]}"}; do # bash 3.2: empty arrays need the + guard
            printf '%s\n' "${f#"$src_dir"}"
            cat "$f"
        done
        printf '%s\n' "plugin_abi.h"
        cat "$abi_header"
        printf '%s' "$config|${extra[*]-}"
    } | shasum -a 256 | cut -c1-12
)"
mkdir -p "$out_dir" && out_dir="$(cd "$out_dir" && pwd)"
lib="$out_dir/$name-$hash.dylib"

if [[ -f "$lib" ]]; then
    # Same sources as an existing build (e.g. an undo): make it the newest so the engine switches to it.
    touch "$lib"
    echo "up to date: $lib"
    exit 0
fi

# --- compile + link ----------------------------------------------------------------------------------------------
obj_dir="$out_dir/obj/$name"
mkdir -p "$obj_dir"
tmp_lib="$obj_dir/$name-$hash.dylib"
if [[ "$config" == "Release" ]]; then opt=(-O2 -DNDEBUG); else opt=(-O0 -g); fi
start=$(date +%s)
if ! "$cxx" -std=c++20 -dynamiclib -fPIC -fvisibility=hidden -Wall -Wextra -arch arm64 -mmacosx-version-min=13.0 \
        "${opt[@]}" ${extra[@]+"${extra[@]}"} -I"$sdk_include" -I"$src_dir" "${sources[@]}" -o "$tmp_lib" 2>&1; then
    echo "error: build of $name failed"
    exit 1
fi
mv -f "$tmp_lib" "$lib"

# Keep the 3 newest builds of this plugin (the engine loads a temp copy, so older files are never in use).
ls -t "$out_dir/$name"-*.dylib 2>/dev/null | tail -n +4 | while IFS= read -r old; do rm -f "$old"; done

echo "built $lib in $(( $(date +%s) - start )) s"
exit 0
