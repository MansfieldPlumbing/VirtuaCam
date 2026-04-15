# =============================================================================
# build.ps1  --  VirtuaCam Build Script
# =============================================================================
#
# PREREQUISITES
#   - PowerShell 7+ (pwsh)
#   - CMake 3.20+ on PATH  (https://cmake.org/download/)
#   - Visual Studio 2022 with "Desktop development with C++" workload
#   - vcpkg with the following packages installed:
#       vcpkg install wil cppwinrt
#     Set $env:VCPKG_ROOT or pass -VcpkgRoot to point at your vcpkg clone.
#     If neither is provided the script probes common install locations.
#
# USAGE
#   .\build.ps1                         # Incremental release build
#   .\build.ps1 -Clean                  # Full rebuild from scratch
#   .\build.ps1 -BuildConfig Debug      # Debug build
#   .\build.ps1 -Register               # Build then register the virtual camera DLL (needs Admin)
#   .\build.ps1 -Unregister             # Unregister the virtual camera DLL (needs Admin)
#   .\build.ps1 -VcpkgRoot C:\tools\vcpkg   # Override vcpkg location
#
# OUTPUTS
#   Binaries are built to .\build\<BuildConfig>\ and then copied to the repo root.
#   To use the virtual camera, run:  .\build.ps1 -Register
# =============================================================================

param(
    [string]$VcpkgRoot   = "",          # Leave blank to auto-detect (see PREREQUISITES above)
    [string]$BuildConfig = "Release",
    [switch]$Clean,
    [switch]$Register,
    [switch]$Unregister
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Helper Functions ---

function Write-Banner  { param([string]$M) Write-Host "`n============================================================" -ForegroundColor Cyan; Write-Host "  $M" -ForegroundColor Cyan; Write-Host "============================================================" -ForegroundColor Cyan }
function Write-Step    { param([string]$M) Write-Host "`n--- [$M] ---" -ForegroundColor Yellow }
function Write-Info    { param([string]$M) Write-Host "  [i] $M" -ForegroundColor Cyan }
function Write-Ok      { param([string]$M) Write-Host "  [+] $M" -ForegroundColor Green }
function Write-Warn    { param([string]$M) Write-Host "  [!] $M" -ForegroundColor Yellow }
function Exit-Fatal {
    param([string]$M)
    Write-Host "`n" -NoNewline
    Write-Host "!!! FATAL !!!" -ForegroundColor Red
    Write-Host $M -ForegroundColor Red
    exit 1
}
function Invoke-Checked {
    param([string]$File, [array]$Args)
    Write-Info "Running: $File $($Args -join ' ')"
    & $File $Args
    if ($LASTEXITCODE -ne 0) {
        Exit-Fatal "$File exited with code $LASTEXITCODE."
    }
}

# --- Banner ---
Write-Banner "VirtuaCam Build Script"
Write-Info "Config : $BuildConfig"
Write-Info "Clean  : $Clean"

# =============================================================================
# [1] Resolve vcpkg
# =============================================================================
Write-Step "1  Resolve vcpkg"

# Auto-detect vcpkg when -VcpkgRoot was not supplied.
# Probed in priority order: env var -> VS-managed install -> common manual installs.
if (-not $VcpkgRoot) {
    $probeOrder = @(
        $env:VCPKG_ROOT,                                          # Standard env var
        (Join-Path $env:LOCALAPPDATA "vcpkg"),                    # VS-managed install
        "C:\vcpkg",                                               # Common manual install
        (Join-Path $env:USERPROFILE "vcpkg")                      # Home-dir manual install
    ) | Where-Object { $_ }   # drop nulls/empty

    foreach ($candidate in $probeOrder) {
        if (Test-Path (Join-Path $candidate "scripts\buildsystems\vcpkg.cmake")) {
            $VcpkgRoot = $candidate
            Write-Info "Auto-detected vcpkg at: $VcpkgRoot"
            break
        }
    }
}

if (-not $VcpkgRoot -or -not (Test-Path $VcpkgRoot)) {
    Exit-Fatal @"
vcpkg not found. Fix one of:
  1. Install vcpkg and set `$env:VCPKG_ROOT
       https://vcpkg.io/en/getting-started
  2. Pass -VcpkgRoot "C:\path\to\vcpkg"

Required packages after installing:
  vcpkg install wil cppwinrt
"@
}

$ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $ToolchainFile)) {
    Exit-Fatal "vcpkg toolchain not found at '$ToolchainFile'. Is your vcpkg installation complete?"
}
Write-Ok "vcpkg: $VcpkgRoot"

# =============================================================================
# [2] Validate source tree
# =============================================================================
Write-Step "2  Validate source tree"

$RepoRoot  = Get-Location
$SourceDir = Join-Path $RepoRoot "src"
$BuildDir  = Join-Path $RepoRoot "build"

if (-not (Test-Path (Join-Path $SourceDir "CMakeLists.txt"))) {
    Exit-Fatal "'src\CMakeLists.txt' not found. Run this script from the repository root."
}
Write-Ok "Source tree looks good."

# =============================================================================
# [3] Prepare build directory
# =============================================================================
Write-Step "3  Prepare build directory"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
    Write-Ok "Removed old build directory."
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Write-Ok "Created build directory."
} else {
    Write-Info "Reusing existing build directory (incremental build)."
}

# =============================================================================
# [4] CMake configure
# =============================================================================
Write-Step "4  CMake configure"
Write-Info "Generator : Visual Studio 17 2022 (x64)"

$configArgs = @(
    "-S", $SourceDir,
    "-B", $BuildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
)
Invoke-Checked "cmake" $configArgs
Write-Ok "Configuration up-to-date."

# =============================================================================
# [5] CMake build
# =============================================================================
Write-Step "5  Build ($BuildConfig)"
Write-Info "Tip: fast if no source files have changed."

Invoke-Checked "cmake" @("--build", $BuildDir, "--config", $BuildConfig)
Write-Ok "Build complete."

# =============================================================================
# [6] COM registration (optional)
# =============================================================================
if ($Unregister) {
    Write-Step "6  Unregister virtual camera DLL"
    Invoke-Checked "cmake" @("--build", $BuildDir, "--config", $BuildConfig, "--target", "unregister_vcam")
    Write-Ok "Unregistered."
}
if ($Register) {
    Write-Step "6  Register virtual camera DLL"
    Write-Warn "This step requires Administrator privileges."
    Invoke-Checked "cmake" @("--build", $BuildDir, "--config", $BuildConfig, "--target", "register_vcam")
    Write-Ok "Registered. VirtuaCam is now visible as a camera device."
}

# =============================================================================
# [7] Copy artifacts to repo root
# =============================================================================
Write-Step "7  Copy artifacts"

$ArtifactDir = Join-Path $BuildDir $BuildConfig
if (-not (Test-Path $ArtifactDir)) {
    Exit-Fatal "Expected artifacts at '$ArtifactDir' but directory not found."
}

$copied = 0
Get-ChildItem -Path $ArtifactDir -File |
    Where-Object { $_.Extension -in ".exe", ".dll", ".pdb", ".pyd" } |
    ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $RepoRoot -Force
        Write-Ok "Copied: $($_.Name)"
        $copied++
    }

if ($copied -eq 0) {
    Write-Warn "No .exe / .dll / .pdb files found in build output."
}

# --- Done ---
Write-Banner "BUILD SUCCEEDED"
