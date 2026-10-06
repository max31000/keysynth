<#
.SYNOPSIS
  Start keysynth for playing: engine with real audio (ASIO) + the web UI in the default browser.
.DESCRIPTION
  1. Builds ui/dist (npm ci if needed, npm run build) when it is missing or older than the UI sources.
  2. Starts <BuildDir>/bin/<Config>/keysynth-engine.exe (building it first if the build dir is configured but the
     exe is missing), which serves ui/dist on http://127.0.0.1:<HttpPort> and the protocol on ws://127.0.0.1:<Port>.
  3. Waits until the HTTP port accepts connections, then opens the browser.
  Ctrl+C stops the engine (it shares this console and quits cleanly; it is killed if it does not exit in 3 s).
.EXAMPLE
  scripts/start.ps1
  scripts/start.ps1 -EngineArgs '--preset','presets/factory/piano/salamander-concert-grand.json'
  scripts/start.ps1 -Port 7351 -HttpPort 7350 -NoBrowser -NoAudio
#>
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [int]$Port = 7341,
    [int]$HttpPort = 7340,
    [string[]]$EngineArgs = @(),
    [switch]$NoBrowser,
    [switch]$NoAudio,
    [switch]$SkipUiBuild,
    [int]$StartTimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "lib/launch.ps1")

$repo = Split-Path -Parent $PSScriptRoot
if (-not $SkipUiBuild) { Update-UiDist -Repo $repo }
$exe = Get-EngineExe -Repo $repo -BuildDir $BuildDir -Config $Config

$cmdArgs = @("--port", "$Port", "--http-port", "$HttpPort")
if ($NoAudio) { $cmdArgs += @("--no-audio", "--no-midi") } # smoke tests / no interface attached
$cmdArgs += $EngineArgs
Write-Host "starting $exe $($cmdArgs -join ' ')"
$engine = Start-Process -FilePath $exe -ArgumentList (Join-ProcessArgs $cmdArgs) -WorkingDirectory $repo `
    -NoNewWindow -PassThru
$null = $engine.Handle # keeps ExitCode readable after exit
try {
    if (-not (Wait-TcpPort -Port $HttpPort -Process $engine -TimeoutSeconds $StartTimeoutSeconds)) {
        throw "the engine did not open http://127.0.0.1:$HttpPort (see its output above; is the port in use?)"
    }
    $url = "http://127.0.0.1:$HttpPort/"
    if ($Port -ne 7341) { $url += "?engine=127.0.0.1:$Port" }
    Write-Host "keysynth UI: $url   (Ctrl+C to stop)"
    if (-not $NoBrowser) { Start-Process $url }
    while (-not $engine.WaitForExit(250)) { }
    Write-Host "engine exited ($($engine.ExitCode))"
} finally {
    Stop-Launched -Process $engine -Name "engine"
}
