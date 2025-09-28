# build.ps1 (Version 10 - Incremental Builds & Registration)
# Supports incremental builds by default. Use -Clean for a full rebuild.
# Supports COM registration via -Register and -Unregister flags.

# --- PARAMETERS ---
param(
    [string]$VcpkgRoot = "c:\vcpkg",
    [string]$BuildConfig = "Release",
    [switch]$Clean,
    [switch]$Register,
    [switch]$Unregister
)

# --- SCRIPT START ---
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Helper Functions ---
function Write-Step { param([string]$Message) Write-Host "`n" -NoNewline; Write-Host "--- [STEP] $Message ---" -ForegroundColor Yellow }
function Write-Success { param([string]$Message) Write-Host "  - SUCCESS:" -ForegroundColor Green -NoNewline; Write-Host " $Message" }
function Exit-WithError {
    param([string]$Message)
    Write-Host "`n"; Write-Host "==================== FATAL BUILD ERROR ====================" -ForegroundColor Red
    Write-Host "  $Message" -ForegroundColor Red
    Write-Host "=========================================================" -ForegroundColor Red
    exit 1
}
function Execute-Process {
    param([string]$File, [array]$Arguments)
    Write-Host "  - Executing: $File $($Arguments -join ' ')"
    & $File $Arguments
    if ($LASTEXITCODE -ne 0) {
        Exit-WithError "An external process ($File) failed with exit code $LASTEXITCODE."
    }
}

# --- Main Script Body ---
Write-Host "============================================================" -ForegroundColor Green
Write-Host " DirectPort Build Script (v10 - Incremental)"
Write-Host "============================================================"

$PSScriptRoot = Get-Location
$BuildDir = Join-Path $PSScriptRoot "build"
$SourceDir = Join-Path $PSScriptRoot "src"

# --- PRE-FLIGHT CHECKS ---
Write-Step "Performing Pre-flight Sanity Checks"
if (-not (Test-Path (Join-Path $SourceDir "CMakeLists.txt"))) { Exit-WithError "'CMakeLists.txt' not found in '$SourceDir'." }
Write-Success "Found 'src/CMakeLists.txt'."
if (-not (Test-Path $VcpkgRoot)) { Exit-WithError "vcpkg directory not found at '$VcpkgRoot'." }
Write-Success "Found vcpkg directory."
$ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $ToolchainFile)) { Exit-WithError "vcpkg toolchain file not found at '$ToolchainFile'." }
Write-Success "Found vcpkg toolchain file."

# --- 1. Clean and Prepare Build Directory (if -Clean is specified) ---
if ($Clean) {
    Write-Step "Cleaning Build Directory (-Clean specified)"
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Write-Success "Created fresh build directory."
} else {
    Write-Step "Incremental Build Mode (use -Clean for a full rebuild)"
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
        Write-Success "Created new build directory."
    } else {
        Write-Success "Build directory already exists."
    }
}

# --- 2. Run CMake to Configure the Project (only if necessary) ---
# CMake is smart enough not to reconfigure if nothing has changed.
Write-Step "Configuring Project with CMake (if necessary)"
$cmake_config_args = @("-S", $SourceDir, "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64", "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile")
Execute-Process "cmake" $cmake_config_args
Write-Success "CMake configuration is up-to-date."

# --- 3. Run CMake to Build the Project ---
Write-Step "Building All Targets ($BuildConfig)"
Write-Host "  - INFO: Starting build. This will be fast if no files have changed." -ForegroundColor Cyan
$cmake_build_args = @("--build", $BuildDir, "--config", $BuildConfig)
Execute-Process "cmake" $cmake_build_args
Write-Success "Project build completed."

# --- 4. Register or Unregister the Virtual Camera DLL (Optional) ---
if ($Unregister) {
    Write-Step "Unregistering Virtual Camera DLL"
    $unregister_args = @("--build", $BuildDir, "--config", $BuildConfig, "--target", "unregister_vcam")
    Execute-Process "cmake" $unregister_args
    Write-Success "Unregistration command sent."
}
if ($Register) {
    Write-Step "Registering Virtual Camera DLL (requires Admin privileges)"
    $register_args = @("--build", $BuildDir, "--config", $BuildConfig, "--target", "register_vcam")
    Execute-Process "cmake" $register_args
    Write-Success "Registration command sent."
}

# --- 5. Copy Artifacts to Project Root ---
Write-Step "Copying Build Artifacts to Project Root"
$ArtifactSourceDir = Join-Path $BuildDir $BuildConfig
if (-not (Test-Path $ArtifactSourceDir)) { Exit-WithError "Build artifact directory not found at '$ArtifactSourceDir'." }

$copiedFiles = 0
Get-ChildItem -Path $ArtifactSourceDir -File | Where-Object { $_.Extension -in ".exe", ".pyd", ".pdb", ".dll" } | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $PSScriptRoot -Force
    Write-Success "Copied Artifact:  $($_.Name)"
    $copiedFiles++
}

if ($copiedFiles -eq 0) {
    Write-Host "  - WARNING: No executables, modules, or DLLs were found in the build output." -ForegroundColor Yellow
}

Write-Host "`n============================================================" -ForegroundColor Green
Write-Host " BUILD SUCCEEDED"
Write-Host "============================================================"