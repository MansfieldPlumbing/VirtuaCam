#Requires -Version 7.5
# scripts/create_installer.ps1
# Creates the VirtuaCam installer using Inno Setup Compiler (iscc).
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
    $BuildDir    = "$ProjectRoot\build"
    $InstallerScript = "$ProjectRoot\installer\VirtuaCam.iss"

    $Paths = [PSCustomObject]@{
        ProjectRoot      = $ProjectRoot
        ConfigFile       = $ConfigFile
        BuildDir         = $BuildDir
        InstallerScript  = $InstallerScript
        InstallerOutput  = "$ProjectRoot\installer\Output"
    }
}

$ProjectRoot     = $Paths.ProjectRoot
$BuildDir        = $Paths.BuildDir
$InstallerScript = $Paths.InstallerScript
$InstallerOutput = $Paths.InstallerOutput

Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "  VIRTUACAM  -  Create Installer" -ForegroundColor Cyan
Write-Host "════════════════════════════════════════════════════════════"

# ---------------------------------------------------------------------------
# [1] VALIDATE BUILD ARTIFACTS
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [1/4] Checking build artifacts..." -ForegroundColor Yellow

$ArtifactDir = Join-Path $BuildDir "Release"
if (-not (Test-Path $ArtifactDir)) {
    Write-Host ""
    Write-Host "  ❌ Build directory not found: $ArtifactDir" -ForegroundColor Red
    Write-Host "  Run [4] Build first." -ForegroundColor DarkYellow
    exit 1
}

$RequiredFiles = @(
    "VirtuaCam.exe",
    "VirtuaCamProcess.exe",
    "DirectPortClient.dll",
    "DirectPortBroker.dll",
    "DirectPortMFCamera.dll",
    "DirectPortMFGraphicsCapture.dll",
    "DirectPortConsumer.dll"
)

$missingFiles = @()
foreach ($file in $RequiredFiles) {
    $filePath = Join-Path $ArtifactDir $file
    if (-not (Test-Path $filePath)) {
        $missingFiles += $file
    } else {
        Write-Host "  + $file" -ForegroundColor Green
    }
}

if ($missingFiles.Count -gt 0) {
    Write-Host ""
    Write-Host "  ❌ Missing required files:" -ForegroundColor Red
    foreach ($f in $missingFiles) {
        Write-Host "     - $f" -ForegroundColor DarkYellow
    }
    Write-Host ""
    Write-Host "  Ensure the build completed successfully." -ForegroundColor DarkYellow
    exit 1
}

# ---------------------------------------------------------------------------
# [2] VALIDATE INNO SETUP
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [2/4] Checking Inno Setup..." -ForegroundColor Yellow

$isccCmd = Get-Command iscc -ErrorAction SilentlyContinue
if (-not $isccCmd) {
    # Try common install locations
    $commonPaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup\ISCC.exe"
    )
    
    foreach ($path in $commonPaths) {
        if (Test-Path $path) {
            $isccCmd = $path
            break
        }
    }
}

if (-not $isccCmd) {
    Write-Host ""
    Write-Host "  ❌ Inno Setup Compiler (iscc) not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "  Install Inno Setup:" -ForegroundColor DarkYellow
    Write-Host "    winget install JRSoftware.InnoSetup" -ForegroundColor White
    Write-Host "    https://jrsoftware.org/isinfo.php" -ForegroundColor Cyan
    Write-Host ""
    exit 1
}

Write-Host "  + ISCC: $isccCmd" -ForegroundColor Green

# ---------------------------------------------------------------------------
# [3] PREPARE INSTALLER DIRECTORY
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [3/4] Preparing installer..." -ForegroundColor Yellow

# Clean output directory
if (Test-Path $InstallerOutput) {
    Remove-Item $InstallerOutput -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $InstallerOutput | Out-Null
Write-Host "  + Output directory ready." -ForegroundColor DarkGray

# Verify installer script exists
if (-not (Test-Path $InstallerScript)) {
    Write-Host ""
    Write-Host "  ❌ Installer script not found: $InstallerScript" -ForegroundColor Red
    exit 1
}
Write-Host "  + Script: $InstallerScript" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# [4] COMPILE INSTALLER
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  [4/4] Compiling installer..." -ForegroundColor Yellow

Write-Host "  Running: iscc `"$InstallerScript`"" -ForegroundColor DarkGray
Write-Host ""

& $isccCmd $InstallerScript
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  ❌ Inno Setup compilation failed." -ForegroundColor Red
    exit 1
}

# Find the generated installer
$generatedInstaller = Get-ChildItem $InstallerOutput -Filter "*.exe" | Select-Object -First 1
if ($generatedInstaller) {
    Write-Host ""
    Write-Host "  + Installer created: $($generatedInstaller.FullName)" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "  ⚠  Installer executable not found in output directory." -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------------------
# SUMMARY
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Green
Write-Host "  INSTALLER CREATED SUCCESSFULLY" -ForegroundColor Green
Write-Host "════════════════════════════════════════════════════════════"
Write-Host ""
Write-Host "  Output location:" -ForegroundColor Cyan
Write-Host "    $InstallerOutput" -ForegroundColor White
Write-Host ""
Write-Host "  The installer includes:" -ForegroundColor DarkGray
Write-Host "    - All VirtuaCam binaries" -ForegroundColor DarkGray
Write-Host "    - COM registration for DirectPortClient.dll" -ForegroundColor DarkGray
Write-Host "    - Optional desktop shortcut and startup entry" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  To distribute:" -ForegroundColor Cyan
Write-Host "    Share the .exe file from the Output folder." -ForegroundColor White
Write-Host ""
