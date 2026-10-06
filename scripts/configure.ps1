<#
.SYNOPSIS
  Configure the keysynth CMake build.
.DESCRIPTION
  Configures <repo>/<BuildDir> with the Visual Studio generator (multi-config; binaries in <BuildDir>/bin/<Config>/).
  Dependencies are fetched into the shared <repo>/.deps cache so several build dirs / worktrees reuse downloads.
  - When every dependency source already exists in .deps, the configure runs with FETCHCONTENT_FULLY_DISCONNECTED=ON
    (no network, never touches the shared sources). -Online forces a connected configure (e.g. after a pin bump).
  - Configures are serialized with an exclusive lock on <.deps>/.configure.lock, so concurrent configures from
    several worktrees can't wipe each other's sources. The lock is an OS file handle: it is released when this
    script exits, even if it crashes.
  - cmake warnings on stderr are not failures; only the exit code counts.
.EXAMPLE
  scripts/configure.ps1
  scripts/configure.ps1 -BuildDir build-va
  scripts/configure.ps1 -BuildDir build-asan -ExtraArgs '-DKS_RT_CHECKS=OFF'
#>
param(
    [string]$BuildDir = "build",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$DepsDir = "",
    [string[]]$ExtraArgs = @(),
    [switch]$Online,
    [int]$LockTimeoutSeconds = 1800
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $DepsDir) {
    # Worktrees under <main>/.claude/worktrees/<x> share the main checkout's .deps.
    $main = $repo
    $wt = $repo -split '[\\/]\.claude[\\/]worktrees[\\/]'
    if ($wt.Count -gt 1) { $main = $wt[0] }
    $DepsDir = Join-Path $main ".deps"
}
New-Item -ItemType Directory -Force $DepsDir | Out-Null
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$depsCmake = $DepsDir -replace '\\', '/'

# Dependency source dirs FetchContent populates (cmake/Dependencies.cmake). sfizz only when enabled.
$deps = @("juce", "nlohmann_json", "ixwebsocket", "catch2")
$extra = $ExtraArgs -join ' '
if ($extra -notmatch '-DKS_WITH_SFIZZ(:BOOL)?=(OFF|0|FALSE|NO)\b') { $deps += "sfizz" }
$missing = @($deps | Where-Object { -not (Test-Path (Join-Path $DepsDir "$_-src/CMakeLists.txt")) })
$userSetDisconnected = $extra -match 'FETCHCONTENT_FULLY_DISCONNECTED'

$cmakeArgs = @("-S", $repo, "-B", $buildPath, "-G", $Generator, "-A", "x64",
    "-DFETCHCONTENT_BASE_DIR=$depsCmake")
if (-not $userSetDisconnected) {
    if (-not $Online -and $missing.Count -eq 0) {
        $cmakeArgs += "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        Write-Host "All dependency sources present in $DepsDir -> disconnected configure"
    } else {
        # Explicit OFF so a cache entry from an earlier disconnected configure doesn't block the download.
        $cmakeArgs += "-DFETCHCONTENT_FULLY_DISCONNECTED=OFF"
        if ($missing.Count -gt 0) { Write-Host "Missing dependency sources: $($missing -join ', ') -> online configure" }
    }
}
$cmakeArgs += $ExtraArgs

# --- exclusive lock on the shared .deps --------------------------------------------------------------------------
$lockPath = Join-Path $DepsDir ".configure.lock"
$lock = $null
$deadline = (Get-Date).AddSeconds($LockTimeoutSeconds)
$announced = $false
while (-not $lock) {
    try {
        $lock = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    } catch [System.IO.IOException] {
        if ((Get-Date) -gt $deadline) { throw "Timed out waiting for $lockPath (another configure is running)" }
        if (-not $announced) { Write-Host "Waiting for another configure to finish ($lockPath)..."; $announced = $true }
        Start-Sleep -Milliseconds 500
    }
}
try {
    $info = [System.Text.Encoding]::UTF8.GetBytes("pid $PID $buildPath $(Get-Date -Format o)`n")
    $lock.SetLength(0); $lock.Write($info, 0, $info.Length); $lock.Flush()

    Write-Host "cmake $($cmakeArgs -join ' ')"
    # cmake prints warnings (CMake Deprecation/Dev warnings) to stderr. With $ErrorActionPreference = Stop, Windows
    # PowerShell turns redirected native stderr lines into terminating NativeCommandErrors, so relax it for the call
    # and judge success by the exit code only.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if (Test-Path variable:PSNativeCommandUseErrorActionPreference) { $PSNativeCommandUseErrorActionPreference = $false }
    try {
        & cmake @cmakeArgs 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message } else { Write-Host $_ }
        }
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    if ($rc -ne 0) { throw "cmake configure failed (exit code $rc)" }
} finally {
    $lock.Dispose()
}
Write-Host "Configured $buildPath. Build: cmake --build $BuildDir --config Release"
