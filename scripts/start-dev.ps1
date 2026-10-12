<#
.SYNOPSIS
  UI development: keysynth-engine --no-audio (null device, no MIDI) or the mock engine + the Vite dev server.
.DESCRIPTION
  Starts <BuildDir>/bin/<Config>/keysynth-engine.exe --no-audio --no-midi --http-port 0 --port <Port> (built
  incrementally first when the build dir is configured; -NoBuild skips that) and the Vite dev server in ui/
  (http://127.0.0.1:5173, run with node directly so Ctrl+C does not hit cmd's "Terminate batch job?" prompt), then
  opens the browser at the dev server with ?engine= pointing at the engine. Fails at once if a port is already
  taken. Ctrl+C stops both. -RealAudio runs the engine with the audio device and MIDI instead.
  -Mock runs ui/mock/mock-engine.ts on its own port (default 7441, never the engine's 7341) instead of the engine.
  Leftovers from an earlier run (or another agent): scripts/stop-dev.ps1.
.EXAMPLE
  scripts/start-dev.ps1
  scripts/start-dev.ps1 -Port 7351 -NoBrowser
  scripts/start-dev.ps1 -Mock
#>
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [int]$Port = 0, # default: 7341 for the engine, 7441 for -Mock
    [string[]]$EngineArgs = @(),
    [switch]$RealAudio,
    [switch]$Mock,
    [switch]$NoBrowser,
    [switch]$NoBuild,
    [int]$StartTimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "lib/launch.ps1")

$repo = Split-Path -Parent $PSScriptRoot
$ui = Join-Path $repo "ui"
$vitePort = 5173 # ui/vite.config.ts (strictPort)
if ($Port -eq 0) { $Port = if ($Mock) { 7441 } else { 7341 } }
if (-not $Mock) { $exe = Get-EngineExe -Repo $repo -BuildDir $BuildDir -Config $Config -NoBuild:$NoBuild }
if (-not (Test-Path (Join-Path $ui "node_modules"))) {
    Write-Host "installing UI dependencies (npm ci)..."
    Invoke-Native -File "npm.cmd" -Arguments @("ci", "--no-audit", "--no-fund") -WorkingDirectory $ui
}
$viteJs = Join-Path $ui "node_modules/vite/bin/vite.js"
$tsxJs = Join-Path $ui "node_modules/tsx/dist/cli.mjs"
if (-not (Test-Path $viteJs)) { throw "$viteJs not found (run npm ci in ui/)" }
$node = (Get-Command node -ErrorAction Stop).Source
Assert-PortsFree -Ports @($Port, $vitePort)

if ($Mock) {
    $what = "mock engine"
    $cmdArgs = @($tsxJs, "mock/mock-engine.ts", "--port", "$Port")
    Write-Host "starting the mock engine on ws://127.0.0.1:$Port"
    $engine = Start-Process -FilePath $node -ArgumentList (Join-ProcessArgs $cmdArgs) -WorkingDirectory $ui `
        -NoNewWindow -PassThru
} else {
    $what = "engine"
    $cmdArgs = @("--port", "$Port", "--http-port", "0")
    if (-not $RealAudio) { $cmdArgs += @("--no-audio", "--no-midi") }
    $cmdArgs += $EngineArgs
    Write-Host "starting $exe $($cmdArgs -join ' ')"
    $engine = Start-Process -FilePath $exe -ArgumentList (Join-ProcessArgs $cmdArgs) -WorkingDirectory $repo `
        -NoNewWindow -PassThru
}
$null = $engine.Handle
$vite = $null
try {
    if (-not (Wait-TcpPort -Port $Port -Process $engine -TimeoutSeconds $StartTimeoutSeconds)) {
        throw "the $what did not open ws://127.0.0.1:$Port (see its output above)"
    }
    $vite = Start-Process -FilePath $node -ArgumentList (Join-ProcessArgs @($viteJs)) -WorkingDirectory $ui `
        -NoNewWindow -PassThru
    $null = $vite.Handle
    if (-not (Wait-TcpPort -Port $vitePort -Process $vite -TimeoutSeconds $StartTimeoutSeconds)) {
        throw "the Vite dev server did not open http://127.0.0.1:$vitePort (see its output above)"
    }
    # Always explicit: the dev server must never silently fall back to whatever answers on 7341.
    $url = "http://127.0.0.1:$vitePort/?engine=127.0.0.1:$Port"
    Write-Host "keysynth dev UI: $url   (Ctrl+C to stop the $what + Vite)"
    if (-not $NoBrowser) { Start-Process $url }
    while (-not $engine.HasExited -and -not $vite.HasExited) { Start-Sleep -Milliseconds 250 }
    if ($engine.HasExited) { Write-Host "$what exited ($($engine.ExitCode))" } else { Write-Host "Vite exited" }
} finally {
    Stop-Launched -Process $vite -Name "Vite"
    Stop-Launched -Process $engine -Name $what
}
