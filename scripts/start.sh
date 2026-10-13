#!/usr/bin/env bash
# Start keysynth for playing on macOS: engine with real audio (CoreAudio) + the web UI in the default browser.
# Counterpart of scripts/start.ps1: builds ui/dist when stale, starts build/bin/keysynth-engine (built incrementally
# first), waits for the HTTP port, opens the browser. Ctrl+C stops the engine.
#
#   scripts/start.sh
#   scripts/start.sh --port 7351 --http-port 7350 --no-browser --no-audio
#   scripts/start.sh -- --preset presets/factory/piano/salamander-concert-grand.json   # extra engine args
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
source "$repo/scripts/lib/launch.sh"

port=7341 http_port=7340 no_browser=0 no_audio=0 skip_ui=0 no_build=0 engine_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) port="$2"; shift 2 ;;
        --http-port) http_port="$2"; shift 2 ;;
        --no-browser) no_browser=1; shift ;;
        --no-audio) no_audio=1; shift ;;
        --skip-ui-build) skip_ui=1; shift ;;
        --no-build) no_build=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --) shift; engine_args+=("$@"); break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ $skip_ui -eq 1 ]] || update_ui_dist "$repo"
exe="$(engine_exe "$repo" "$no_build")"
assert_ports_free "$port" "$http_port"

args=(--port "$port" --http-port "$http_port")
[[ $no_audio -eq 1 ]] && args+=(--no-audio --no-midi)
args+=(${engine_args[@]+"${engine_args[@]}"})
echo "starting $exe ${args[*]}"
(cd "$repo" && exec "$exe" "${args[@]}") &
engine=$!
trap 'stop_launched "$engine"' EXIT INT TERM

wait_tcp_port "$http_port" "$engine" 60 ||
    { echo "error: the engine did not open http://127.0.0.1:$http_port (see its output above)" >&2; exit 1; }
url="http://127.0.0.1:$http_port/"
[[ "$port" == 7341 ]] || url+="?engine=127.0.0.1:$port"
echo "keysynth UI: $url   (Ctrl+C to stop)"
[[ $no_browser -eq 1 ]] || open "$url"
wait "$engine" || true
