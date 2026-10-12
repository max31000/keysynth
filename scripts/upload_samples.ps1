<#
.SYNOPSIS
  Upload the sample libraries listed in index.json to the VDS (/var/lib/keysynth/samples/) + index.json.

.DESCRIPTION
  Producer of index.json: scripts/build_sample_index.py (contract: docs/specs/2026-10-12-cloud-api.md §4).
  Only files listed in the index are uploaded. Resume-friendly: per library, the server's file list
  (size + path) is compared with the index and only missing / size-mismatched files are sent, so an
  interrupted run is simply re-run. Transport: rsync if available, otherwise tar streamed over ssh
  (cmd pipe, binary-safe). index.json is uploaded last and swapped in atomically, then ownership is
  fixed (1654:1654 = the API container's `app` user) and the result verified (counts, bytes,
  sha256 of a random sample of files).

.EXAMPLE
  python scripts/build_sample_index.py
  scripts/upload_samples.ps1                      # everything
  scripts/upload_samples.ps1 -Lib salamander      # one library (index.json is uploaded anyway)
  scripts/upload_samples.ps1 -DryRun              # show what would be sent
  scripts/upload_samples.ps1 -VerifyOnly -SpotCheck 50
#>
[CmdletBinding()]
param(
    [string]$Index = (Join-Path $PSScriptRoot '..\build\samples\index.json'),
    [string]$SamplesRoot = (Join-Path $PSScriptRoot '..\assets\samples'),
    [string]$Remote = 'root@mvv42.ru',
    [string]$Key = (Join-Path $HOME '.ssh\mvv42_deploy'),
    [string]$RemoteDir = '/var/lib/keysynth/samples',
    [string]$Owner = '1654:1654',
    [string[]]$Lib = @(),
    [int]$SpotCheck = 20,
    [switch]$DryRun,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$utf8 = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = $utf8   # stdin to native commands: UTF-8 without BOM

$Index = (Resolve-Path $Index).Path
$SamplesRoot = (Resolve-Path $SamplesRoot).Path
if (-not (Test-Path $Key)) { throw "ssh key not found: $Key" }
if ($RemoteDir -notmatch '^/[A-Za-z0-9/_.-]+$') { throw "bad RemoteDir: $RemoteDir" }
$ssh = Join-Path $env:WINDIR 'System32\OpenSSH\ssh.exe'
$scp = Join-Path $env:WINDIR 'System32\OpenSSH\scp.exe'
$tar = Join-Path $env:WINDIR 'System32\tar.exe'
$sshOpts = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=30')
$rsync = Get-Command rsync -ErrorAction SilentlyContinue

function Invoke-Remote([string]$cmd, [string]$stdin = '') {
    $ErrorActionPreference = 'Continue'   # ssh warnings on stderr must not abort; the exit code decides
    if ($stdin) { $out = $stdin | & $ssh @sshOpts $Remote $cmd } else { $out = & $ssh @sshOpts $Remote $cmd }
    if ($LASTEXITCODE -ne 0) { throw "remote command failed ($LASTEXITCODE): $cmd" }
    return $out
}

# Server listing of one library: path -> size.
function Get-RemoteFiles([string]$libId) {
    $map = @{}
    # No double quotes in remote commands: Windows PowerShell 5.1 mangles them in native arguments.
    $lines = Invoke-Remote "if [ -d '$RemoteDir/$libId' ]; then cd '$RemoteDir/$libId' && find . -type f -printf '%s\t%P\n'; fi"
    foreach ($l in @($lines)) {
        if (-not $l) { continue }
        $tab = $l.IndexOf("`t")
        $map[$l.Substring($tab + 1)] = [int64]$l.Substring(0, $tab)
    }
    return $map
}

$idx = Get-Content -Raw -Encoding UTF8 $Index | ConvertFrom-Json
$libs = @($idx.libraries | Where-Object { $Lib.Count -eq 0 -or $Lib -contains $_.id })
if ($Lib.Count -gt 0 -and $libs.Count -ne $Lib.Count) { throw "unknown library id in -Lib $($Lib -join ',')" }
foreach ($l in $libs) { if ($l.id -notmatch '^[a-z0-9][a-z0-9-]*$') { throw "bad library id $($l.id)" } }

$df = Invoke-Remote "mkdir -p '$RemoteDir' && df -B1 --output=avail '$RemoteDir' | tail -1"
$need = ($libs | Measure-Object -Property bytes -Sum).Sum
Write-Host ("server free: {0:N1} GB, libraries selected: {1:N2} GB" -f ([int64]$df / 1e9), ($need / 1e9))

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "ks-upload-$PID"
New-Item -ItemType Directory -Force $tmpDir | Out-Null
try {
    if (-not $VerifyOnly) {
        foreach ($l in $libs) {
            $local = Join-Path $SamplesRoot $l.id
            $remoteFiles = Get-RemoteFiles $l.id
            $todo = @($l.files | Where-Object { -not $remoteFiles.ContainsKey($_.path) -or $remoteFiles[$_.path] -ne [int64]$_.bytes })
            $todoBytes = ($todo | Measure-Object -Property bytes -Sum).Sum
            Write-Host ("{0}: {1}/{2} files to send ({3:N1} MB)" -f $l.id, $todo.Count, $l.files.Count, ($todoBytes / 1e6))
            if ($todo.Count -eq 0 -or $DryRun) { continue }
            $list = Join-Path $tmpDir "$($l.id).list"
            [System.IO.File]::WriteAllText($list, (($todo | ForEach-Object { $_.path }) -join "`n") + "`n", $utf8)
            if ($rsync) {
                & rsync -rt --partial --files-from="$list" -e "ssh -i '$Key' -o BatchMode=yes" "$local/" "${Remote}:$RemoteDir/$($l.id)/"
                if ($LASTEXITCODE -ne 0) { throw "rsync failed for $($l.id)" }
            } else {
                # cmd's pipe is binary-safe (PowerShell 5.1 pipes between native commands are not).
                $remoteCmd = "mkdir -p '$RemoteDir/$($l.id)' && tar -xf - -C '$RemoteDir/$($l.id)' --no-same-owner"
                $line = "`"$tar`" -cf - -C `"$local`" -T `"$list`" | `"$ssh`" -i `"$Key`" -o BatchMode=yes -o ServerAliveInterval=30 $Remote `"$remoteCmd`""
                cmd /c $line
                if ($LASTEXITCODE -ne 0) { throw "tar|ssh failed for $($l.id) (re-run to resume)" }
            }
        }
        if ($DryRun) { Write-Host 'dry run: nothing sent'; return }
        & $scp -i $Key -o BatchMode=yes -q $Index "${Remote}:$RemoteDir/.index.json.tmp"
        if ($LASTEXITCODE -ne 0) { throw 'scp index.json failed' }
        Invoke-Remote ("mv -f '$RemoteDir/.index.json.tmp' '$RemoteDir/index.json' && chown -R $Owner '$RemoteDir' && " +
            "chmod -R u+rwX,go+rX,go-w '$RemoteDir'") | Out-Null
        Write-Host "index.json installed, ownership $Owner"
    }

    # Verify: every indexed file present with the right size; report extras; sha256 spot check.
    $bad = 0
    foreach ($l in $libs) {
        $remoteFiles = Get-RemoteFiles $l.id
        $missing = @($l.files | Where-Object { -not $remoteFiles.ContainsKey($_.path) -or $remoteFiles[$_.path] -ne [int64]$_.bytes })
        $paths = @{}; foreach ($f in $l.files) { $paths[$f.path] = $true }
        $extra = @($remoteFiles.Keys | Where-Object { -not $paths.ContainsKey($_) })
        $sum = ($remoteFiles.Values | Measure-Object -Sum).Sum
        Write-Host ("verify {0}: server {1} files / {2:N1} MB, index {3} / {4:N1} MB, missing-or-wrong-size {5}, extra {6}" -f `
            $l.id, $remoteFiles.Count, ($sum / 1e6), $l.files.Count, ($l.bytes / 1e6), $missing.Count, $extra.Count)
        if ($missing.Count -gt 0) { $bad++ }
    }
    $all = @($libs | ForEach-Object { $lid = $_.id; $_.files | ForEach-Object { [pscustomobject]@{ p = "$lid/$($_.path)"; h = $_.sha256 } } })
    $pick = @($all | Get-Random -Count ([Math]::Min($SpotCheck, $all.Count)))
    $out = Invoke-Remote "cd '$RemoteDir' && tr -d '\r' | sed '1s/^\xEF\xBB\xBF//' | xargs -d '\n' sha256sum" (($pick | ForEach-Object { $_.p }) -join "`n")
    $got = @{}; foreach ($o in @($out)) { if ($o -match '^([0-9a-f]{64})  (.+)$') { $got[$Matches[2]] = $Matches[1] } }
    $mism = @($pick | Where-Object { $got[$_.p] -ne $_.h })
    Write-Host ("sha256 spot check: {0}/{1} match" -f ($pick.Count - $mism.Count), $pick.Count)
    $remoteIdx = Invoke-Remote "sha256sum '$RemoteDir/index.json' 2>/dev/null | cut -c1-64; stat -c '%u:%g' '$RemoteDir' 2>/dev/null"
    $localIdx = (Get-FileHash -Algorithm SHA256 $Index).Hash.ToLower()
    Write-Host ("index.json on server matches local: {0}; owner {1}" -f (@($remoteIdx)[0] -eq $localIdx), @($remoteIdx)[1])
    if ($bad -gt 0 -or $mism.Count -gt 0) { throw "verification failed ($bad libraries incomplete, $($mism.Count) hash mismatches)" }
    Write-Host 'OK'
} finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
