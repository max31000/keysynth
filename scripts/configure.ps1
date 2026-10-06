<#
.SYNOPSIS
  Configure the keysynth CMake build.
.DESCRIPTION
  Configures <repo>/<BuildDir> with the Visual Studio generator (multi-config; binaries in <BuildDir>/bin/<Config>/).
  Dependencies are fetched into the shared <repo>/.deps cache so several build dirs / worktrees reuse downloads.
.EXAMPLE
  scripts/configure.ps1
  scripts/configure.ps1 -BuildDir build-va
  scripts/configure.ps1 -BuildDir build-asan -ExtraArgs '-DKS_RT_CHECKS=OFF'
#>
param(
    [string]$BuildDir = "build",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$DepsDir = "",
    [string[]]$ExtraArgs = @()
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

$cmakeArgs = @("-S", $repo, "-B", $buildPath, "-G", $Generator, "-A", "x64",
    "-DFETCHCONTENT_BASE_DIR=$depsCmake") + $ExtraArgs
Write-Host "cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
Write-Host "Configured $buildPath. Build: cmake --build $BuildDir --config Release"
