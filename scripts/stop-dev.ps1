<#
.SYNOPSIS
  Stop keysynth dev processes started from this repo or its worktrees: mock engine, Vite dev servers, vitest, dev
  engine builds (keysynth-engine / ks-* executables under the repo).
.DESCRIPTION
  Matches by path, never by name alone:
  - node.exe whose command line contains the repo root (main checkout; worktrees live under it in
    .claude/worktrees) and one of mock-engine, vite, vitest, tsx;
  - executables (keysynth-engine.exe, ks-tests.exe, ks-render.exe, ks-bench.exe) whose image path is under the repo root.
  The installed app (%LOCALAPPDATA%\Programs\keysynth) is never touched unless -IncludeInstalled: it is then asked to
  quit (keysynth-engine.exe --quit, the tray's own quit) and killed only if still running after 5 s.
  Each process is stopped with its child tree (taskkill /T). -WhatIf lists without stopping.
.EXAMPLE
  scripts/stop-dev.ps1
  scripts/stop-dev.ps1 -WhatIf
  scripts/stop-dev.ps1 -IncludeInstalled
#>
param(
    [switch]$IncludeInstalled,
    [switch]$WhatIf
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
# A worktree (<main>/.claude/worktrees/<x>) stops everything of the main checkout too: one repo, one dev stack.
$parts = $repo -split '[\\/]\.claude[\\/]worktrees[\\/]'
$root = [System.IO.Path]::GetFullPath($parts[0]).TrimEnd('\')
$rootPattern = [regex]::Escape($root)
$installedDir = Join-Path $env:LOCALAPPDATA "Programs\keysynth"
$devExes = @("keysynth-engine.exe", "ks-tests.exe", "ks-render.exe", "ks-bench.exe")

function Test-UnderPath([string]$path, [string]$dir) {
    if (-not $path) { return $false }
    $p = [System.IO.Path]::GetFullPath($path)
    return $p.StartsWith($dir.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

$all = Get-CimInstance Win32_Process
# Processes started with a relative script path (`node node_modules/vite/bin/vite.js` from ui/) carry no repo path:
# those count when they listen on a keysynth dev port (Vite 5173/4173, mock 7441/7440, or the engine's 7341/7340
# where a stray mock caused the 2026-10 incident).
$devPorts = @(5173, 4173, 7441, 7440, 7341, 7340)
$listeners = @{}
foreach ($c in (Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue)) {
    if ($devPorts -contains [int]$c.LocalPort) { $listeners[[int]$c.OwningProcess] = $true }
}
$targets = @()
foreach ($p in $all) {
    if ($p.ProcessId -eq $PID) { continue }
    $cmd = [string]$p.CommandLine
    if ($p.Name -ieq "node.exe") {
        $tool = $cmd -match 'mock-engine|[\\/]vite[\\/]|vitest|[\\/]tsx[\\/]|node_modules[\\/]\.bin'
        $relative = $cmd -notmatch '[A-Za-z]:[\\/](?!Program Files)' # no absolute script path (node.exe itself aside)
        if ($tool -and ($cmd -match $rootPattern -or ($relative -and $listeners.ContainsKey([int]$p.ProcessId)))) {
            $targets += [pscustomobject]@{ Id = $p.ProcessId; Kind = "node"; What = $cmd }
        }
    } elseif ($devExes -contains $p.Name.ToLowerInvariant()) {
        if (Test-UnderPath $p.ExecutablePath $root) {
            $targets += [pscustomobject]@{ Id = $p.ProcessId; Kind = "dev-exe"; What = $p.ExecutablePath }
        } elseif ($IncludeInstalled -and (Test-UnderPath $p.ExecutablePath $installedDir)) {
            $targets += [pscustomobject]@{ Id = $p.ProcessId; Kind = "installed"; What = $p.ExecutablePath }
        }
    }
}
# Children of a matched process are stopped with it (taskkill /T); drop them from the list.
$ids = @($targets | ForEach-Object { $_.Id })
$parentOf = @{}
foreach ($p in $all) { $parentOf[[int]$p.ProcessId] = [int]$p.ParentProcessId }
$targets = @($targets | Where-Object { -not ($ids -contains $parentOf[[int]$_.Id]) })

if ($targets.Count -eq 0) {
    Write-Host "no keysynth dev processes running (root $root)"
    exit 0
}
foreach ($t in $targets) {
    Write-Host ("{0} {1,-9} pid {2}: {3}" -f ($(if ($WhatIf) { "would stop" } else { "stopping" })), $t.Kind, $t.Id, $t.What)
    if ($WhatIf) { continue }
    if ($t.Kind -eq "installed") {
        & $t.What --quit | Out-Null
        $proc = Get-Process -Id $t.Id -ErrorAction SilentlyContinue
        if ($proc -and -not $proc.WaitForExit(5000)) { & taskkill.exe /PID $t.Id /T /F | Out-Null }
        continue
    }
    & taskkill.exe /PID $t.Id /T /F 2>$null | Out-Null
}
if (-not $IncludeInstalled -and (Get-CimInstance Win32_Process -Filter "Name='keysynth-engine.exe'" |
        Where-Object { Test-UnderPath $_.ExecutablePath $installedDir })) {
    Write-Host "installed keysynth left running (use -IncludeInstalled to stop it)"
}
