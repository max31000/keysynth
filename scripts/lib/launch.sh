# Shared helpers for the macOS launchers (scripts/start.sh, scripts/start-dev.sh). Source, don't run.
# Counterpart of scripts/lib/launch.ps1.

# ui/dist missing or older than the UI sources -> npm ci (if needed) + npm run build.
update_ui_dist() {
    local ui="$1/ui"
    if [[ -f "$ui/dist/index.html" ]] &&
        [[ -z "$(find "$ui/src" "$ui/index.html" "$ui/package.json" -newer "$ui/dist/index.html" -print -quit)" ]]; then
        return 0
    fi
    [[ -d "$ui/node_modules" ]] || npm --prefix "$ui" ci --no-audit --no-fund
    npm --prefix "$ui" run build
}

# Path of the console engine (single-config Ninja build: <build>/bin/keysynth-engine), built incrementally first
# when the build dir is configured (unless $2 = 1, no build).
engine_exe() {
    local repo="$1" no_build="${2:-0}" build="${BUILD_DIR:-$1/build}"
    if [[ "$no_build" != 1 && -f "$build/CMakeCache.txt" ]]; then
        cmake --build "$build" --target keysynth-engine >&2 || return 1
    fi
    local exe="$build/bin/keysynth-engine"
    [[ -x "$exe" ]] || { echo "error: $exe not found (scripts/configure.sh, then cmake --build build)" >&2; return 1; }
    echo "$exe"
}

# Fail when any of the ports is taken, naming the owner.
assert_ports_free() {
    local p owner
    for p in "$@"; do
        owner="$(lsof -nP -iTCP:"$p" -sTCP:LISTEN 2>/dev/null | awk 'NR>1 {print $1 " (PID " $2 ")"; exit}' || true)"
        if [[ -n "$owner" ]]; then
            echo "error: port $p is in use by $owner (scripts/stop-dev.sh stops dev leftovers)" >&2
            return 1
        fi
    done
}

# Wait until 127.0.0.1:<port> accepts connections while <pid> lives. $3 = timeout in seconds.
wait_tcp_port() {
    local port="$1" pid="$2" timeout="${3:-60}" i
    for ((i = 0; i < timeout * 4; i++)); do
        kill -0 "$pid" 2>/dev/null || return 1
        nc -z 127.0.0.1 "$port" 2>/dev/null && return 0
        sleep 0.25
    done
    return 1
}

# SIGTERM (the engine quits cleanly), SIGKILL after 3 s.
stop_launched() {
    local pid="$1" i
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null || return 0
    kill -TERM "$pid" 2>/dev/null
    for ((i = 0; i < 12; i++)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.25
    done
    kill -KILL "$pid" 2>/dev/null || true
}
