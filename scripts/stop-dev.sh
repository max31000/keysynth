#!/usr/bin/env bash
# Stop leftover dev processes of THIS checkout on macOS (counterpart of scripts/stop-dev.ps1): the engine / test /
# tool binaries under <repo>/build*, node (Vite, mock) started from <repo>/ui, and whatever of those listens on the
# dev ports 7441/7440/7341/7340. Never touches the installed /Applications/keysynth.app.
#
#   scripts/stop-dev.sh            # stop
#   scripts/stop-dev.sh --dry-run  # only list
set -uo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
main="${repo%%/.claude/worktrees/*}"
dry=0
[[ "${1:-}" == "--dry-run" ]] && dry=1

pids=()
# Dev binaries and node processes whose command line points into this checkout (or its worktrees).
while read -r pid cmd; do
    [[ "$pid" == "$$" ]] && continue
    case "$cmd" in
        *"/Applications/keysynth.app/"*) continue ;;
        "$main"/build*/bin/*keysynth-engine*|"$main"/build*/bin/ks-*|"$main"/.claude/worktrees/*/build*/bin/*) pids+=("$pid") ;;
        *node*"$main"/ui/*|*node*"$main"/.claude/worktrees/*/ui/*) pids+=("$pid") ;;
        *node*mock/mock-engine.ts*|*node*vite/bin/vite.js*)
            cwd="$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')"
            [[ "$cwd" == "$main"* ]] && pids+=("$pid") ;;
    esac
done < <(ps -axo pid=,command=)

if [[ ${#pids[@]} -eq 0 ]]; then
    echo "nothing to stop"
    exit 0
fi
for pid in "${pids[@]}"; do
    echo "$(ps -o pid=,command= -p "$pid" 2>/dev/null | cut -c1-160)"
done
[[ $dry -eq 1 ]] && exit 0
kill -TERM "${pids[@]}" 2>/dev/null
for _ in $(seq 1 12); do
    alive=0
    for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [[ $alive -eq 0 ]] && break
    sleep 0.25
done
for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null; done
echo "stopped ${#pids[@]} process(es)"
