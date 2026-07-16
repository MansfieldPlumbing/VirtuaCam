#Requires -Version 7.5
# scripts/build_virtuacam.ps1
# Builds VirtuaCam using CMake and vcpkg.
# Called by setup.ps1 - do not run directly.

param(
    [PSCustomObject]$Paths
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = $Paths.ProjectRoot
$BuildDir    = $Paths.BuildDir
$SrcDir      = $Paths.SrcDir
$CmakeExe    = $Paths.CmakeExe
$VcpkgRoot   = $Paths.VcpkgRoot
$ConfigFile  = $Paths.ConfigFile

Write-Host ""
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "  VIRTUACAM BUILD" -ForegroundColor Cyan
Write-Host "================================================================================"

# ---------------------------------------------------------------------------
# Load config to get vcvars path
# ---------------------------------------------------------------------------
$vcvars = $null
if (Test-Path $ConfigFile) {
    Get-Content $ConfigFile | ForEach-Object {
        if ($_ -match "^vcvars\s*=\s*(.+)$") { $vcvars = $Matches[1].Trim() }
    }
}

# ---------------------------------------------------------------------------
# Check for build tools
# ---------------------------------------------------------------------------
if (-not $vcvars -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "  [!!] Visual Studio C++ environment not found." -ForegroundColor Red
    Write-Host "       Run [2] Preflight checks first to detect your VS installation." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------------------
# Find or use provided CMake
# ---------------------------------------------------------------------------
$cmakeToUse = $CmakeExe
if (-not $cmakeToUse -or -not (Test-Path $cmakeToUse)) {
    if (Get-Command cmake -ErrorAction SilentlyContinue) {
        $cmakeToUse = (Get-Command cmake).Source
    } else {
        Write-Host ""
        Write-Host "  [!!] CMake not found." -ForegroundColor Red
        Write-Host "       Install CMake via winget or run [2] Preflight to detect it." -ForegroundColor Yellow
        Write-Host ""
        exit 1
    }
}

Write-Host ""
Write-Host "  Using CMake: $cmakeToUse" -ForegroundColor DarkGray
if ($vcvars) {
    Write-Host "  Using VC vars: $vcvars" -ForegroundColor DarkGray
} else {
    Write-Host "  Using active developer environment (cl.exe in PATH)" -ForegroundColor DarkGray
}
Write-Host ""

# ---------------------------------------------------------------------------
# Create build directory
# ---------------------------------------------------------------------------
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    Write-Host "  + Created build directory: $BuildDir" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Build command
# ---------------------------------------------------------------------------
$vcpkgArg = ""
if ($VcpkgRoot -and (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    $vcpkgArg = "-DCMAKE_TOOLCHAIN_FILE=`"$VcpkgRoot\scripts\buildsystems\vcpkg.cmake`""
    Write-Host "  + Using vcpkg toolchain: $VcpkgRoot" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "  Configuring with CMake..." -ForegroundColor Cyan

$configCmd = "& `"$cmakeToUse`" -S `"$SrcDir`" -B `"$BuildDir`" -DCMAKE_BUILD_TYPE=Release $vcpkgArg"
Write-Host "  $configCmd" -ForegroundColor DarkGray
Write-Host ""

Invoke-Expression $configCmd

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  [!!] CMake configuration failed." -ForegroundColor Red
    Write-Host "       Check errors above." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "  Building..." -ForegroundColor Cyan
Write-Host ""

$buildCmd = "& `"$cmakeToUse`" --build `"$BuildDir`" --config Release"
Invoke-Expression $buildCmd

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  [!!] Build failed." -ForegroundColor Red
    Write-Host "       Check errors above." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------------------
# Copy outputs to project root
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  Copying build outputs..." -ForegroundColor Cyan

$exePath = "$BuildDir\Release\VirtuaCam.exe"
if (Test-Path $exePath) {
    Copy-Item $exePath -Destination $ProjectRoot -Force
    Write-Host "  + VirtuaCam.exe" -ForegroundColor Green
} else {
    # Try without Release subdir (single-config generators)
    $exePath = "$BuildDir\VirtuaCam.exe"
    if (Test-Path $exePath) {
        Copy-Item $exePath -Destination $ProjectRoot -Force
        Write-Host "  + VirtuaCam.exe" -ForegroundColor Green
    }
}

$brokerPath = "$BuildDir\Release\DirectPortBroker.dll"
if (Test-Path $brokerPath) {
    Copy-Item $brokerPath -Destination $ProjectRoot -Force
    Write-Host "  + DirectPortBroker.dll" -ForegroundColor Green
} else {
    $brokerPath = "$BuildDir\DirectPortBroker.dll"
    if (Test-Path $brokerPath) {
        Copy-Item $brokerPath -Destination $ProjectRoot -Force
        Write-Host "  + DirectPortBroker.dll" -ForegroundColor Green
    }
}

$camPath = "$BuildDir\Release\DirectPortVirtuaCam.dll"
if (Test-Path $camPath) {
    Copy-Item $camPath -Destination $ProjectRoot -Force
    Write-Host "  + DirectPortVirtuaCam.dll" -ForegroundColor Green
} else {
    $camPath = "$BuildDir\DirectPortVirtuaCam.dll"
    if (Test-Path $camPath) {
        Copy-Item $camPath -Destination $ProjectRoot -Force
        Write-Host "  + DirectPortVirtuaCam.dll" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "================================================================================" -ForegroundColor Green
Write-Host "  BUILD SUCCESSFUL" -ForegroundColor Green
Write-Host "================================================================================"
Write-Host ""
Write-Host "  Run the application:" -ForegroundColor Cyan
Write-Host "    .\VirtuaCam.exe" -ForegroundColor White
Write-Host ""
