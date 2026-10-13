#!/usr/bin/env bash
# UI development on macOS: keysynth-engine --no-audio (or the mock engine) + the Vite dev server.
# Counterpart of scripts/start-dev.ps1. Ctrl+C stops both; leftovers: scripts/stop-dev.sh.
#
#   scripts/start-dev.sh
#   scripts/start-dev.sh --mock             # ui/mock/mock-engine.ts on 7441 (never the engine's 7341)
#   scripts/start-dev.sh --real-audio --port 7351 --no-browser
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
source "$repo/scripts/lib/launch.sh"
ui="$repo/ui"
vite_port=5173 # ui/vite.config.ts (strictPort)

port=0 mock=0 real_audio=0 no_browser=0 no_build=0 engine_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) port="$2"; shift 2 ;;
        --mock) mock=1; shift ;;
        --real-audio) real_audio=1; shift ;;
        --no-browser) no_browser=1; shift ;;
        --no-build) no_build=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --) shift; engine_args+=("$@"); break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$port" != 0 ]] || { [[ $mock -eq 1 ]] && port=7441 || port=7341; }

[[ $mock -eq 1 ]] || exe="$(engine_exe "$repo" "$no_build")"
[[ -d "$ui/node_modules" ]] || { echo "installing UI dependencies (npm ci)..."; npm --prefix "$ui" ci --no-audit --no-fund; }
assert_ports_free "$port" "$vite_port"

engine="" vite=""
trap 'stop_launched "$vite"; stop_launched "$engine"' EXIT INT TERM
if [[ $mock -eq 1 ]]; then
    what="mock engine"
    echo "starting the mock engine on ws://127.0.0.1:$port"
    (cd "$ui" && exec node node_modules/tsx/dist/cli.mjs mock/mock-engine.ts --port "$port") &
else
    what="engine"
    args=(--port "$port" --http-port 0)
    [[ $real_audio -eq 1 ]] || args+=(--no-audio --no-midi)
    args+=(${engine_args[@]+"${engine_args[@]}"})
    echo "starting $exe ${args[*]}"
    (cd "$repo" && exec "$exe" "${args[@]}") &
fi
engine=$!
wait_tcp_port "$port" "$engine" 60 || { echo "error: the $what did not open ws://127.0.0.1:$port" >&2; exit 1; }

(cd "$ui" && exec node node_modules/vite/bin/vite.js) &
vite=$!
wait_tcp_port "$vite_port" "$vite" 60 || { echo "error: Vite did not open http://127.0.0.1:$vite_port" >&2; exit 1; }
# Always explicit: the dev server must never silently fall back to whatever answers on 7341.
url="http://127.0.0.1:$vite_port/?engine=127.0.0.1:$port"
echo "keysynth dev UI: $url   (Ctrl+C to stop the $what + Vite)"
[[ $no_browser -eq 1 ]] || open "$url"
while kill -0 "$engine" 2>/dev/null && kill -0 "$vite" 2>/dev/null; do sleep 0.25; done
