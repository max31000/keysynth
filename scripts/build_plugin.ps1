<#
.SYNOPSIS
  Build a C++ keysynth plugin DLL (C ABI, sdk/include/keysynth/plugin_abi.h).
.DESCRIPTION
  Compiles every .cpp in plugins/<Name>/ with MSVC (vcvars64 of the newest Visual Studio / Build Tools, found via
  vswhere) into plugins/.build/<Name>-<hash>.dll, where <hash> covers the sources, local headers and the ABI header.
  The DLL is written under a temporary name and renamed, so a running engine never loads a half-written file; the
  engine watches plugins/.build/ and hot-reloads the newest <Name>-*.dll (docs/PLUGINS.md).
  Static CRT (/MT): only the C ABI crosses the DLL boundary.
  Exit codes: 0 ok (or up to date), 1 compile/link error, 2 bad arguments, 3 MSVC not found.
.EXAMPLE
  scripts/build_plugin.ps1 cpp_bitcrusher
  scripts/build_plugin.ps1 my_fx -Config Debug
  scripts/build_plugin.ps1 my_fx -PluginsDir C:\tmp\plugins     # tests: build outside the repo
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Name,
    [string]$PluginsDir = "",
    [string]$OutDir = "",
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [string[]]$ExtraFlags = @()
)
$ErrorActionPreference = "Stop"
if ($Name -notmatch '^[a-z][a-z0-9_]{0,63}$') { Write-Error "invalid plugin name '$Name' (expected [a-z][a-z0-9_]*)"; exit 2 }

$repo = Split-Path -Parent $PSScriptRoot
if (-not $PluginsDir) { $PluginsDir = Join-Path $repo "plugins" }
$PluginsDir = [System.IO.Path]::GetFullPath($PluginsDir)
if (-not $OutDir) { $OutDir = Join-Path $PluginsDir ".build" }
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$srcDir = Join-Path $PluginsDir $Name
$main = Join-Path $srcDir "$Name.cpp"
if (-not (Test-Path -LiteralPath $main -PathType Leaf)) { Write-Host "error: $main not found"; exit 2 }
$sdkInclude = Join-Path $repo "sdk\include"
$abiHeader = Join-Path $sdkInclude "keysynth\plugin_abi.h"

# --- content hash (sources + local headers + ABI header + flags) ---------------------------------------------
# .cpp files at the top level are compiled; headers anywhere below the plugin dir are hashed.
$files = @(Get-ChildItem -LiteralPath $srcDir -File | Where-Object { $_.Extension -eq ".cpp" } | Sort-Object Name)
$headers = @(Get-ChildItem -LiteralPath $srcDir -File -Recurse | Where-Object { $_.Extension -in ".h", ".hpp", ".inl" } |
             Sort-Object FullName)
$sha = [System.Security.Cryptography.SHA256]::Create()
$ms = New-Object System.IO.MemoryStream
foreach ($f in $files + $headers + @(Get-Item -LiteralPath $abiHeader)) {
    $rel = if ($f.FullName.StartsWith($srcDir)) { $f.FullName.Substring($srcDir.Length) } else { $f.Name }
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($rel + "`n")
    $ms.Write($nameBytes, 0, $nameBytes.Length)
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $ms.Write($bytes, 0, $bytes.Length)
}
$flagBytes = [System.Text.Encoding]::UTF8.GetBytes("$Config|" + ($ExtraFlags -join " "))
$ms.Write($flagBytes, 0, $flagBytes.Length)
$hash = (($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 12)
$dll = Join-Path $OutDir "$Name-$hash.dll"

New-Item -ItemType Directory -Force $OutDir | Out-Null
if (Test-Path -LiteralPath $dll) {
    # Same sources as an existing build (e.g. an undo): make it the newest so the engine switches to it.
    (Get-Item -LiteralPath $dll).LastWriteTime = Get-Date
    Write-Host "up to date: $dll"
    exit 0
}

# --- MSVC environment ------------------------------------------------------------------------------------------
$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $inst = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ($inst) { $vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars -or -not (Test-Path -LiteralPath $vcvars)) {
    foreach ($c in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools",
                     "${env:ProgramFiles}\Microsoft Visual Studio\18\Community",
                     "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
                     "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community")) {
        $p = Join-Path $c "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path -LiteralPath $p) { $vcvars = $p; break }
    }
}
if (-not $vcvars -or -not (Test-Path -LiteralPath $vcvars)) { Write-Host "error: MSVC (vcvars64.bat) not found"; exit 3 }

# --- compile + link ----------------------------------------------------------------------------------------------
$objDir = Join-Path $OutDir "obj\$Name"
New-Item -ItemType Directory -Force $objDir | Out-Null
$tmpDll = Join-Path $objDir "$Name-$hash.dll"
$opt = if ($Config -eq "Release") { @("/O2", "/DNDEBUG", "/MT") } else { @("/Od", "/Zi", "/MTd") }
# All compiler/linker arguments go through a response file, so paths and -ExtraFlags are never parsed by cmd.exe
# (no injection through & | % ^). Directory arguments end in "\\" so the closing quote is not escaped.
function Quote([string]$v) { '"' + $v.Replace('"', '') + '"' }
$clArgs = @("/nologo", "/LD", "/std:c++20", "/EHsc", "/W4", "/permissive-", "/utf-8", "/fp:precise") + $opt +
    $ExtraFlags + @("/I" + (Quote $sdkInclude), "/I" + (Quote $srcDir)) + @($files | ForEach-Object { Quote $_.FullName }) +
    @("/Fo" + (Quote "$objDir\\"), "/Fd" + (Quote "$objDir\\"), "/Fe" + (Quote $tmpDll),
      "/link", "/NOLOGO", "/NOIMPLIB", "/NOEXP", "/INCREMENTAL:NO")
$rsp = Join-Path $objDir "cl.rsp"
[System.IO.File]::WriteAllLines($rsp, [string[]]$clArgs)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$out = cmd /c "call `"$vcvars`" >nul 2>&1 && cl @`"$rsp`" 2>&1"
$code = $LASTEXITCODE
$sw.Stop()
if ($code -ne 0 -or -not (Test-Path -LiteralPath $tmpDll)) {
    $out | Where-Object { $_ -and $_ -notmatch '^\s*$' } | ForEach-Object { Write-Host $_ }
    Write-Host "error: build of $Name failed (cl exit $code)"
    exit 1
}
Move-Item -LiteralPath $tmpDll -Destination $dll -Force

# Keep the 3 newest builds of this plugin (the engine loads a temp copy, so older files are never locked).
Get-ChildItem -LiteralPath $OutDir -Filter "$Name-*.dll" | Sort-Object LastWriteTime -Descending |
    Select-Object -Skip 3 | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }

Write-Host ("built {0} in {1:N1} s" -f $dll, $sw.Elapsed.TotalSeconds)
exit 0
