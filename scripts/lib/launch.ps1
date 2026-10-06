# Helpers shared by scripts/start.ps1 and scripts/start-dev.ps1 (dot-sourced).

function Invoke-Native {
    # Runs a native command; stderr output (npm/cmake warnings) is not an error, the exit code is.
    param([string]$File, [string[]]$Arguments, [string]$WorkingDirectory)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $WorkingDirectory
    try {
        & $File @Arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message } else { Write-Host $_ }
        }
        $rc = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $saved
    }
    if ($rc -ne 0) { throw "$File $($Arguments -join ' ') failed (exit code $rc)" }
}

function Get-NewestWriteTime {
    param([string[]]$Paths)
    $newest = [datetime]::MinValue
    foreach ($p in $Paths) {
        if (-not (Test-Path $p)) { continue }
        $items = if ((Get-Item $p).PSIsContainer) { Get-ChildItem $p -Recurse -File } else { @(Get-Item $p) }
        foreach ($i in $items) { if ($i.LastWriteTimeUtc -gt $newest) { $newest = $i.LastWriteTimeUtc } }
    }
    return $newest
}

function Update-UiDist {
    # Builds ui/dist when it is missing or older than the UI sources (npm ci first if node_modules is missing).
    param([string]$Repo)
    $ui = Join-Path $Repo "ui"
    $distIndex = Join-Path $ui "dist/index.html"
    $sources = @("src", "public", "index.html", "package.json", "package-lock.json", "vite.config.ts", "tsconfig.json") |
        ForEach-Object { Join-Path $ui $_ }
    $srcTime = Get-NewestWriteTime $sources
    if ((Test-Path $distIndex) -and (Get-Item $distIndex).LastWriteTimeUtc -ge $srcTime) {
        Write-Host "ui/dist is up to date"
        return
    }
    if (-not (Test-Path (Join-Path $ui "node_modules"))) {
        Write-Host "installing UI dependencies (npm ci)..."
        Invoke-Native -File "npm.cmd" -Arguments @("ci", "--no-audit", "--no-fund") -WorkingDirectory $ui
    }
    Write-Host "building ui/dist (npm run build)..."
    Invoke-Native -File "npm.cmd" -Arguments @("run", "build") -WorkingDirectory $ui
}

function Get-EngineExe {
    # <BuildDir>/bin/<Config>/keysynth-engine.exe; builds the target when the build dir is configured but the exe is
    # missing.
    param([string]$Repo, [string]$BuildDir, [string]$Config)
    $buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Repo $BuildDir }
    $exe = Join-Path $buildPath "bin/$Config/keysynth-engine.exe"
    if (Test-Path $exe) { return $exe }
    if (-not (Test-Path (Join-Path $buildPath "CMakeCache.txt"))) {
        throw "$exe not found and $buildPath is not configured: run scripts/configure.ps1 -BuildDir $BuildDir first"
    }
    Write-Host "building keysynth-engine ($Config)..."
    Invoke-Native -File "cmake" -Arguments @("--build", $buildPath, "--config", $Config, "--target", "keysynth-engine",
        "--parallel") -WorkingDirectory $Repo
    if (-not (Test-Path $exe)) { throw "build finished but $exe is missing" }
    return $exe
}

function Join-ProcessArgs {
    # Start-Process joins -ArgumentList with spaces: quote arguments that contain spaces.
    param([string[]]$Arguments)
    return ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
}

function Wait-TcpPort {
    # True once 127.0.0.1:<Port> accepts a connection; false on timeout or when $Process exits first.
    param([int]$Port, [System.Diagnostics.Process]$Process, [int]$TimeoutSeconds = 60)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($Process -and $Process.HasExited) { return $false }
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $task = $client.ConnectAsync("127.0.0.1", $Port)
            if ($task.Wait(250) -and $client.Connected) { return $true }
        } catch {
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Stop-Launched {
    # Gives a process that shares this console (and so got the Ctrl+C) up to 3 s to quit, then kills its tree.
    param([System.Diagnostics.Process]$Process, [string]$Name)
    if (-not $Process -or $Process.HasExited) { return }
    if (-not $Process.WaitForExit(3000)) {
        Write-Host "stopping $Name (pid $($Process.Id))"
        & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-Null
    }
}
