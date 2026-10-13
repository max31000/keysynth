#!/usr/bin/env bash
# Configure the keysynth CMake build on macOS (counterpart of scripts/configure.ps1).
#
# Single-config Ninja build in <repo>/<build-dir> (binaries in <build-dir>/bin/). Dependencies are fetched into the
# shared <repo>/.deps cache (worktrees under .claude/worktrees reuse the main checkout's). Requires cmake + ninja
# (`brew install cmake ninja`) and the Xcode command line tools.
#
#   scripts/configure.sh                       # build/, Release
#   scripts/configure.sh build-va              # another build dir
#   scripts/configure.sh build-dbg -DCMAKE_BUILD_TYPE=Debug -DKS_RT_CHECKS=OFF
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build}"
[[ $# -gt 0 ]] && shift
[[ "$build" = /* ]] || build="$repo/$build"

main="${repo%%/.claude/worktrees/*}"
deps="${KS_DEPS_DIR:-$main/.deps}"
mkdir -p "$deps"

# Serialize configures that share .deps (a concurrent first configure would clone into the same source dirs).
lock="$deps/.configure.lock.d"
for _ in $(seq 1 1800); do
    mkdir "$lock" 2>/dev/null && break
    sleep 1
done
trap 'rmdir "$lock" 2>/dev/null || true' EXIT

generator=()
command -v ninja >/dev/null && generator=(-G Ninja)

cmake -S "$repo" -B "$build" ${generator[@]+"${generator[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_BASE_DIR="$deps" \
    "$@"
