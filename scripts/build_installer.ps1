<#
.SYNOPSIS
  Build the Windows installer: out/keysynth-setup-<ver>.exe + out/keysynth-setup.exe (stable name).
.DESCRIPTION
  1. Version: -Version, else the exact git tag vX.Y.Z on HEAD, else the CMake project() version.
  2. Configures <BuildDir> (scripts/configure.ps1, -DKS_VERSION=<ver>; generator auto-detected from the newest
     Visual Studio unless -Generator) and builds keysynth-app (windows-subsystem keysynth-engine.exe) in Release.
  3. Builds ui/dist (npm ci when node_modules is missing or -CleanUi; vite build --base=/).
  4. Stages out/stage/{app,plugins}: exe, faust.dll + share/faust/*.lib (KS_FAUST_DIR from the CMake cache),
     VC++ runtime DLLs (app-local, faust.dll uses the dynamic CRT), installed.flag, presets/factory + patterns + songs,
     ui/dist, licences; plugins = Faust example plugins (C++ examples need MSVC, not shipped).
  5. Runs ISCC (Inno Setup 6) on installer/keysynth.iss.
  See docs/TESTING.md "Release / installer".
.EXAMPLE
  scripts/build_installer.ps1                       # build-release/ -> out/
  scripts/build_installer.ps1 -BuildDir build-desk -Version 0.2.0
  scripts/build_installer.ps1 -NoBuild -SkipUi      # restage + repackage only
#>
param(
    [string]$BuildDir = "build-release", # own dir: -DKS_VERSION must not stick in the dev build/
    [string]$Version = "",
    [string]$Generator = "auto",
    [switch]$NoBuild,
    [switch]$SkipUi,
    [switch]$CleanUi,
    [string]$Iscc = "",
    [string]$OutDir = "out"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "lib/launch.ps1")
$repo = Split-Path -Parent $PSScriptRoot
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$outPath = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repo $OutDir }

function Get-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio Build Tools (C++ workload)" }
    $j = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    if (-not $j) { throw "no Visual Studio with the C++ x64 tools found" }
    return @($j)[0]
}

# --- version ------------------------------------------------------------------------------------------------------
if (-not $Version) {
    $saved = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $tag = $null
    try { $tag = (& git -C $repo describe --tags --exact-match --match "v[0-9]*" HEAD 2>$null) } catch { $tag = $null } finally { $ErrorActionPreference = $saved }
    if ($tag -and "$tag" -match '^v(\d+\.\d+\.\d+)$') { $Version = $Matches[1] }
}
if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(keysynth VERSION (\d+\.\d+\.\d+)'
    $Version = $m.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must be X.Y.Z (got '$Version')" }
Write-Host "keysynth $Version"

# --- engine -------------------------------------------------------------------------------------------------------
$vs = Get-VsInstall
if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "configure.ps1") -BuildDir $buildPath -Generator $Generator -ExtraArgs @("-DKS_VERSION=$Version")
    Invoke-Native -File "cmake" -Arguments @("--build", $buildPath, "--config", "Release", "--target", "keysynth-app", "--parallel") -WorkingDirectory $repo
}
$exe = Join-Path $buildPath "bin/Release/app/keysynth-engine.exe"
if (-not (Test-Path $exe)) { throw "$exe not found (build keysynth-app)" }
$exeVer = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion
if ($exeVer -ne $Version) { throw "$exe is version '$exeVer', expected $Version (rebuild without -NoBuild)" }

# --- UI -----------------------------------------------------------------------------------------------------------
$ui = Join-Path $repo "ui"
if (-not $SkipUi) {
    if ($CleanUi -or -not (Test-Path (Join-Path $ui "node_modules"))) {
        Invoke-Native -File "npm.cmd" -Arguments @("ci", "--no-audit", "--no-fund") -WorkingDirectory $ui
    }
    # The installed fallback UI is served from http://127.0.0.1:7340/ -> base "/".
    Invoke-Native -File "npm.cmd" -Arguments @("run", "build", "--", "--base=/") -WorkingDirectory $ui
}
if (-not (Test-Path (Join-Path $ui "dist/index.html"))) { throw "ui/dist/index.html missing (drop -SkipUi)" }

# --- stage --------------------------------------------------------------------------------------------------------
$stage = Join-Path $outPath "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$app = Join-Path $stage "app"
New-Item -ItemType Directory -Force $app, (Join-Path $stage "plugins") | Out-Null
function Copy-Tree([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force $To | Out-Null
    Copy-Item -Path (Join-Path $From "*") -Destination $To -Recurse -Force
}

Copy-Item $exe $app
Set-Content -Path (Join-Path $app "installed.flag") -Value "keysynth $Version installed (AppPaths installed mode)" -Encoding ascii

# Faust: faust.dll + the .lib files JIT imports resolve against (stdfaust.lib pulls in the others; ~2.5 MB).
$cache = Get-Content (Join-Path $buildPath "CMakeCache.txt")
$faustLine = $cache | Select-String '^KS_FAUST_DIR:PATH=(.*)$' | Select-Object -First 1
if (-not $faustLine) { throw "KS_FAUST_DIR not in $buildPath/CMakeCache.txt (configure with scripts/configure.ps1 first)" }
$faustDir = $faustLine.Matches[0].Groups[1].Value
$faustDll = Join-Path $faustDir "lib/faust.dll"
if (Test-Path $faustDll) {
    Copy-Item $faustDll $app
    $share = Join-Path $app "share/faust"
    New-Item -ItemType Directory -Force $share | Out-Null
    Copy-Item (Join-Path $faustDir "share/faust/*.lib") $share
    $faustLicense = Join-Path $faustDir "COPYING.txt"
    if (Test-Path $faustLicense) { Copy-Item $faustLicense (Join-Path $app "licenses-faust-COPYING.txt") }
} else {
    Write-Warning "faust.dll not found under $faustDir (scripts/fetch_faust.ps1): the installer has no Faust plugins"
}

# VC++ runtime, app-local (faust.dll imports MSVCP140, VCRUNTIME140, VCRUNTIME140_1; the engine itself is /MT).
$redistRoot = Join-Path $vs.installationPath "VC\Redist\MSVC"
$crt = Get-ChildItem $redistRoot -Directory | Sort-Object { try { [version]$_.Name } catch { [version]"0.0" } } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crt) { throw "VC++ redist CRT dir not found under $redistRoot" }
foreach ($dll in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) { Copy-Item (Join-Path $crt.FullName $dll) $app }
Write-Host "VC++ runtime from $($crt.FullName)"

Copy-Tree (Join-Path $repo "presets/factory") (Join-Path $app "presets/factory")
Copy-Tree (Join-Path $repo "presets/patterns") (Join-Path $app "presets/patterns")
Copy-Tree (Join-Path $repo "presets/songs") (Join-Path $app "presets/songs")
Copy-Tree (Join-Path $ui "dist") (Join-Path $app "ui/dist")
Copy-Item (Join-Path $repo "LICENSE") (Join-Path $app "LICENSE.txt")
Copy-Item (Join-Path $repo "THIRD_PARTY_NOTICES.md") $app
Copy-Item (Join-Path $repo "assets/icon/keysynth.ico") $stage

foreach ($p in Get-ChildItem (Join-Path $repo "plugins") -Directory) {
    if (Get-ChildItem $p.FullName -Filter "*.dsp" -File) { Copy-Tree $p.FullName (Join-Path $stage "plugins/$($p.Name)") }
}

# --- Inno Setup ---------------------------------------------------------------------------------------------------
if (-not $Iscc) {
    $mainRepo = ($repo -split '[\\/]\.claude[\\/]worktrees[\\/]')[0]
    $cands = @((Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"), (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"), (Join-Path $mainRepo ".tools\innosetup\ISCC.exe"))
    $Iscc = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $Iscc) { throw "ISCC.exe (Inno Setup 6) not found: choco install innosetup, or see docs/TESTING.md" }
}
New-Item -ItemType Directory -Force $outPath | Out-Null
Invoke-Native -File $Iscc -Arguments @("/Qp", "/DAppVersion=$Version", "/DStageDir=$stage", "/DOutputDir=$outPath",
    (Join-Path $repo "installer/keysynth.iss")) -WorkingDirectory $repo
$setup = Join-Path $outPath "keysynth-setup-$Version.exe"
if (-not (Test-Path $setup)) { throw "ISCC did not produce $setup" }
Copy-Item $setup (Join-Path $outPath "keysynth-setup.exe") -Force
$stageMb = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("{0} ({1:N1} MB; staged {2:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB), $stageMb)
