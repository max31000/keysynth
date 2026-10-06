<#
.SYNOPSIS
  UI development: keysynth-engine --no-audio (null device, no MIDI) + the Vite dev server with hot reload.
.DESCRIPTION
  Starts <BuildDir>/bin/<Config>/keysynth-engine.exe --no-audio --no-midi --http-port 0 --port <Port> (built
  incrementally first when the build dir is configured; -NoBuild skips that) and the Vite dev server in ui/
  (http://127.0.0.1:5173, run with node directly so Ctrl+C does not hit cmd's "Terminate batch job?" prompt), then
  opens the browser at the dev server with ?engine= pointing at the engine. Fails at once if a port is already
  taken. Ctrl+C stops both. -RealAudio runs the engine with the audio device and MIDI instead.
.EXAMPLE
  scripts/start-dev.ps1
  scripts/start-dev.ps1 -Port 7351 -NoBrowser
#>
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [int]$Port = 7341,
    [string[]]$EngineArgs = @(),
    [switch]$RealAudio,
    [switch]$NoBrowser,
    [switch]$NoBuild,
    [int]$StartTimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "lib/launch.ps1")

$repo = Split-Path -Parent $PSScriptRoot
$ui = Join-Path $repo "ui"
$vitePort = 5173 # ui/vite.config.ts (strictPort)
$exe = Get-EngineExe -Repo $repo -BuildDir $BuildDir -Config $Config -NoBuild:$NoBuild
if (-not (Test-Path (Join-Path $ui "node_modules"))) {
    Write-Host "installing UI dependencies (npm ci)..."
    Invoke-Native -File "npm.cmd" -Arguments @("ci", "--no-audit", "--no-fund") -WorkingDirectory $ui
}
$viteJs = Join-Path $ui "node_modules/vite/bin/vite.js"
if (-not (Test-Path $viteJs)) { throw "$viteJs not found (run npm ci in ui/)" }
$node = (Get-Command node -ErrorAction Stop).Source
Assert-PortsFree -Ports @($Port, $vitePort)

$cmdArgs = @("--port", "$Port", "--http-port", "0")
if (-not $RealAudio) { $cmdArgs += @("--no-audio", "--no-midi") }
$cmdArgs += $EngineArgs
Write-Host "starting $exe $($cmdArgs -join ' ')"
$engine = Start-Process -FilePath $exe -ArgumentList (Join-ProcessArgs $cmdArgs) -WorkingDirectory $repo `
    -NoNewWindow -PassThru
$null = $engine.Handle
$vite = $null
try {
    if (-not (Wait-TcpPort -Port $Port -Process $engine -TimeoutSeconds $StartTimeoutSeconds)) {
        throw "the engine did not open ws://127.0.0.1:$Port (see its output above)"
    }
    $vite = Start-Process -FilePath $node -ArgumentList (Join-ProcessArgs @($viteJs)) -WorkingDirectory $ui `
        -NoNewWindow -PassThru
    $null = $vite.Handle
    if (-not (Wait-TcpPort -Port $vitePort -Process $vite -TimeoutSeconds $StartTimeoutSeconds)) {
        throw "the Vite dev server did not open http://127.0.0.1:$vitePort (see its output above)"
    }
    $url = "http://127.0.0.1:$vitePort/"
    if ($Port -ne 7341) { $url += "?engine=127.0.0.1:$Port" }
    Write-Host "keysynth dev UI: $url   (Ctrl+C to stop engine + Vite)"
    if (-not $NoBrowser) { Start-Process $url }
    while (-not $engine.HasExited -and -not $vite.HasExited) { Start-Sleep -Milliseconds 250 }
    if ($engine.HasExited) { Write-Host "engine exited ($($engine.ExitCode))" } else { Write-Host "Vite exited" }
} finally {
    Stop-Launched -Process $vite -Name "Vite"
    Stop-Launched -Process $engine -Name "engine"
}
