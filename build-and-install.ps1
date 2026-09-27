# Build and install X1 AI Island in place.
# Run this script as Administrator so it can restart X1FanService.

$ErrorActionPreference = "Stop"

$projectDir = $PSScriptRoot
$buildScript = Join-Path $projectDir "build.bat"
$exePath = Join-Path $projectDir "X1-AI-Island.exe"
$fanServicePath = Join-Path $projectDir "X1FanService.exe"

function Backup-Old {
    param([string]$Path, [string]$Label)

    if (Test-Path $Path) {
        $backup = "$Path.bak.$(Get-Date -Format 'yyyyMMddHHmmss')"
        Copy-Item $Path $backup -Force
        Write-Host "  Backup: $Label -> $backup" -ForegroundColor Yellow
    }
}

function Stop-AppProcess {
    param([string]$Name)

    $processes = Get-Process $Name -ErrorAction SilentlyContinue
    if ($processes) {
        Write-Host "  Stopping $Name..." -ForegroundColor Yellow
        $processes | Stop-Process -Force
        $processes | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path $buildScript)) {
    throw "Build script not found: $buildScript"
}

Write-Host "`n=== Preparing installation ===" -ForegroundColor Cyan
Stop-AppProcess "X1-AI-Island"
Stop-AppProcess "X1FanService"
Backup-Old $exePath "X1-AI-Island.exe"
Backup-Old $fanServicePath "X1FanService.exe"

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    $buildCommand = "call `"$buildScript`" < nul"
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Visual Studio Build Tools were not found."
    }

    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsInstall) {
        throw "MSVC C++ Build Tools were not found."
    }

    $vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "MSVC environment script not found: $vcvars"
    }
    $buildCommand = "call `"$vcvars`" && call `"$buildScript`" < nul"
}

Write-Host "`n=== Building ===" -ForegroundColor Cyan
Push-Location $projectDir
try {
    & cmd.exe /d /c $buildCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$exe = Get-Item $exePath
$fanService = Get-Item $fanServicePath
Write-Host "`n=== Installing ===" -ForegroundColor Cyan
Write-Host "  X1-AI-Island.exe: $($exe.Length) bytes" -ForegroundColor Green
Write-Host "  X1FanService.exe: $($fanService.Length) bytes" -ForegroundColor Green

Start-Process -FilePath $fanServicePath -WorkingDirectory $projectDir
Start-Process -FilePath $exePath -WorkingDirectory $projectDir

Write-Host "`n=== Complete ===" -ForegroundColor Green
Write-Host "The new build is installed and X1 AI Island has been restarted."
