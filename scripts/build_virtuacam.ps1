#Requires -Version 7.5
# scripts/build_virtuacam.ps1
# CMake-based build for VirtuaCam.
# Called by setup.ps1 with a $Paths object.
# Can also be run standalone — will use config.ini / auto-detect.

param(
    [PSCustomObject]$Paths = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# STANDALONE MODE
# ---------------------------------------------------------------------------
if (-not $Paths) {
    $ProjectRoot = (Resolve-Path "$PSScriptRoot\..").Path
    $ConfigFile  = "$ProjectRoot\config.ini"
    $VcpkgRoot = $null
    $CmakeExe  = 'cmake'

    if (Test-Path $ConfigFile) {
        Get-Content $ConfigFile | ForEach-Object {
            if ($_ -match "^vcpkg_root=(.+)$")   { $VcpkgRoot = $Matches[1].Trim() }
            if ($_ -match "^cmake_exe=(.+)$")   { $CmakeExe  = $Matches[1].Trim() }
        }
    }

    # Auto-detect vcpkg if not in config
    if (-not $VcpkgRoot) {
        $probePaths = @(
            $env:VCPKG_ROOT,
            (Join-Path $env:LOCALAPPDATA "vcpkg"),
            "C:\vcpkg",
            (Join-Path $env:USERPROFILE "vcpkg")
        ) | Where-Object { $_ }

        foreach ($candidate in $probePaths) {
            $toolchain = Join-Path $candidate "scripts\buildsystems\vcpkg.cmake"
            if (Test-Path $toolchain) {
                $VcpkgRoot = $candidate
                break
            }
        }
    }

    $Paths = [PSCustomObject]@{
        ProjectRoot = $ProjectRoot
        ConfigFile  = $ConfigFile
        VcpkgRoot   = $VcpkgRoot
        CmakeExe    = $CmakeExe
        SrcDir      = "$ProjectRoot\src"
        BuildDir    = "$ProjectRoot\build"
    }
}

$ProjectRoot = $Paths.ProjectRoot
$VcpkgRoot   = $Paths.VcpkgRoot
$CmakeExe    = $Paths.CmakeExe
$SrcDir      = $Paths.SrcDir
$BuildDir    = $Paths.BuildDir

Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "  VIRTUACAM  -  Build Script" -ForegroundColor Cyan
Write-Host "════════════════════════════════════════════════════════════"

# ---------------------------------------------------------------------------
# [1] VALIDATE VCPKG
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [1/5] Validating vcpkg..." -ForegroundColor Yellow

if (-not $VcpkgRoot -or -not (Test-Path $VcpkgRoot)) {
    Write-Host ""
    Write-Host "  ❌ vcpkg not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "  Set VCPKG_ROOT environment variable or install vcpkg:" -ForegroundColor DarkYellow
    Write-Host "    git clone https://github.com/microsoft/vcpkg.git" -ForegroundColor White
    Write-Host "    cd vcpkg && .\bootstrap-vcpkg.bat" -ForegroundColor White
    Write-Host ""
    exit 1
}

$ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $ToolchainFile)) {
    Write-Host ""
    Write-Host "  ❌ vcpkg toolchain not found at '$ToolchainFile'." -ForegroundColor Red
    Write-Host "  Is your vcpkg installation complete?" -ForegroundColor DarkYellow
    exit 1
}

Write-Host "  + vcpkg: $VcpkgRoot" -ForegroundColor Green

# ---------------------------------------------------------------------------
# [2] PREPARE BUILD DIRECTORY
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [2/5] Preparing build directory..." -ForegroundColor Yellow

if (-not (Test-Path $SrcDir)) {
    Write-Host ""
    Write-Host "  ❌ Source directory not found: $SrcDir" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Write-Host "  + Created build directory." -ForegroundColor Green
} else {
    Write-Host "  + Using existing build directory." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# [3] CMAKE CONFIGURE
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [3/5] Configuring with CMake..." -ForegroundColor Yellow

$configArgs = @(
    "-S", $SrcDir,
    "-B", $BuildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
)

Write-Host "  Running: $CmakeExe $($configArgs -join ' ')" -ForegroundColor DarkGray
& $CmakeExe $configArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  ❌ CMake configuration failed." -ForegroundColor Red
    exit 1
}
Write-Host "  + Configuration complete." -ForegroundColor Green

# ---------------------------------------------------------------------------
# [4] CMAKE BUILD
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [4/5] Building (Release)..." -ForegroundColor Yellow

$buildArgs = @("--build", $BuildDir, "--config", "Release")
Write-Host "  Running: $CmakeExe $($buildArgs -join ' ')" -ForegroundColor DarkGray
& $CmakeExe $buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  ❌ Build failed." -ForegroundColor Red
    exit 1
}
Write-Host "  + Build complete." -ForegroundColor Green

# ---------------------------------------------------------------------------
# [5] COPY ARTIFACTS TO REPO ROOT
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [5/5] Copying artifacts..." -ForegroundColor Yellow

$ArtifactDir = Join-Path $BuildDir "Release"
if (-not (Test-Path $ArtifactDir)) {
    Write-Host ""
    Write-Host "  ❌ Expected artifacts at '$ArtifactDir' but directory not found." -ForegroundColor Red
    exit 1
}

$copied = 0
Get-ChildItem -Path $ArtifactDir -File |
    Where-Object { $_.Extension -in ".exe", ".dll", ".pdb", ".pyd" } |
    ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $ProjectRoot -Force
        Write-Host "  + $($_.Name)" -ForegroundColor DarkGray
        $copied++
    }

if ($copied -eq 0) {
    Write-Host "  ⚠  No .exe / .dll / .pdb files found in build output." -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------------------
# SUMMARY
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Green
Write-Host "  BUILD SUCCEEDED" -ForegroundColor Green
Write-Host "════════════════════════════════════════════════════════════"
Write-Host ""
Write-Host "  To register the virtual camera (requires Admin):" -ForegroundColor Cyan
Write-Host "    .\build.ps1 -Register" -ForegroundColor White
Write-Host ""
Write-Host "  To create an installer:" -ForegroundColor Cyan
Write-Host "    Run [5] Create installer from the setup menu" -ForegroundColor White
Write-Host ""
