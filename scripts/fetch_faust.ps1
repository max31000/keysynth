<#
.SYNOPSIS
  Fetch Faust 2.88.0 for Windows into .tools/faust WITHOUT running the NSIS installer.

.DESCRIPTION
  Downloads Faust-<ver>-win64.exe into .deps/downloads and unpacks it with 7-Zip (NSIS installers are
  7z-readable). Falls back to py7zr if 7-Zip is not installed. Idempotent: skips if
  .tools/faust/bin/faust.exe already reports the expected version. Use -Force to re-extract.
  See docs/research/FAUST_SETUP.md.
#>
param(
    [string]$Version = "2.88.0",
    [switch]$Force
)
$ErrorActionPreference = "Stop"
# Native tools (curl, 7z) write progress to stderr; in Windows PowerShell 5.1 that becomes a terminating
# error under "Stop", so native calls run through this helper and are checked via $LASTEXITCODE.
function Invoke-Native([scriptblock]$Block) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $Block 2>&1 | ForEach-Object { "$_" } | Out-Host } finally { $ErrorActionPreference = $old }
}
$Repo = Split-Path -Parent $PSScriptRoot
$Downloads = Join-Path $Repo ".deps\downloads"
$Dest = Join-Path $Repo ".tools\faust"
$Exe = "Faust-$Version-win64.exe"
$Url = "https://github.com/grame-cncm/faust/releases/download/$Version/$Exe"
$Installer = Join-Path $Downloads $Exe
$FaustExe = Join-Path $Dest "bin\faust.exe"

if (-not $Force -and (Test-Path $FaustExe)) {
    $v = & $FaustExe --version 2>$null | Select-Object -First 1
    if ($v -match [regex]::Escape($Version)) { Write-Host "Faust $Version already present: $FaustExe"; exit 0 }
}

New-Item -ItemType Directory -Force $Downloads | Out-Null
if (-not (Test-Path $Installer)) {
    Write-Host "Downloading $Url"
    $part = "$Installer.part"
    # curl.exe (ships with Windows 10+) supports resume (-C -) and shows progress.
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        Invoke-Native { & $curl.Source -L --fail --retry 5 -sS -C - -o $part $Url }
        if ($LASTEXITCODE -ne 0) { throw "curl failed ($LASTEXITCODE)" }
    } else {
        $ProgressPreference = "SilentlyContinue"
        Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing
    }
    Move-Item -Force $part $Installer
}
Write-Host ("Installer: {0} ({1:N0} bytes)" -f $Installer, (Get-Item $Installer).Length)

if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force $Dest | Out-Null

$SevenZip = @("C:\Program Files\7-Zip\7z.exe", "C:\Program Files (x86)\7-Zip\7z.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $SevenZip) { $c = Get-Command 7z.exe -ErrorAction SilentlyContinue; if ($c) { $SevenZip = $c.Source } }

if ($SevenZip) {
    Write-Host "Extracting with $SevenZip"
    Invoke-Native { & $SevenZip x $Installer "-o$Dest" -y -bso0 -bsp0 }
    if ($LASTEXITCODE -ne 0) { throw "7z extraction failed ($LASTEXITCODE)" }
} else {
    Write-Host "7-Zip not found; trying py7zr"
    Invoke-Native { & python -m pip install --quiet py7zr }
    Invoke-Native { & python -c "import py7zr,sys; py7zr.SevenZipFile(sys.argv[1]).extractall(sys.argv[2])" $Installer $Dest }
    if ($LASTEXITCODE -ne 0) { throw "py7zr could not extract the installer; install 7-Zip (do NOT run the installer)" }
}

# NSIS leaves its plugin dir and the uninstaller behind; they are not part of the distribution.
foreach ($junk in @('$PLUGINSDIR', 'Uninstall.exe')) {
    $p = Join-Path $Dest $junk
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}

if (-not (Test-Path $FaustExe)) { throw "bin\faust.exe not found after extraction; inspect $Dest" }
& $FaustExe --version
