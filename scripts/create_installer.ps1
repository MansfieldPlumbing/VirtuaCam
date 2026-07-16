#Requires -Version 7.5
# scripts/create_installer.ps1
# Compiles the Inno Setup script to create a VirtuaCam installer.
# Called by setup.ps1 - do not run directly.

param(
    [PSCustomObject]$Paths
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot   = $Paths.ProjectRoot
$InstallerDir  = $Paths.InstallerDir
$BuildDir      = $Paths.BuildDir

Write-Host ""
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "  VIRTUACAM INSTALLER CREATOR" -ForegroundColor Cyan
Write-Host "================================================================================"

# ---------------------------------------------------------------------------
# Find Inno Setup Compiler
# ---------------------------------------------------------------------------
$isccPath = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"

if (-not (Test-Path $isccPath)) {
    if (Get-Command ISCC -ErrorAction SilentlyContinue) {
        $isccPath = (Get-Command ISCC).Source
    } else {
        Write-Host ""
        Write-Host "  [!!] Inno Setup Compiler (ISCC.exe) not found." -ForegroundColor Red
        Write-Host "       Install Inno Setup via winget:" -ForegroundColor Yellow
        Write-Host "         winget install JRSoftware.InnoSetup" -ForegroundColor White
        Write-Host "       Or run [3] Install dependencies." -ForegroundColor Yellow
        Write-Host ""
        exit 1
    }
}

Write-Host ""
Write-Host "  Using ISCC: $isccPath" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Check for build outputs
# ---------------------------------------------------------------------------
$requiredFiles = @(
    "VirtuaCam.exe",
    "DirectPortBroker.dll",
    "DirectPortVirtuaCam.dll"
)

$missingFiles = @()
foreach ($file in $requiredFiles) {
    $srcPath = "$ProjectRoot\$file"
    if (-not (Test-Path $srcPath)) {
        # Try build dir
        $buildPath = "$BuildDir\Release\$file"
        if (Test-Path $buildPath) {
            Copy-Item $buildPath -Destination $ProjectRoot -Force
            Write-Host "  + Copied $file from build directory" -ForegroundColor DarkGray
        } else {
            $missingFiles += $file
        }
    }
}

if ($missingFiles.Count -gt 0) {
    Write-Host ""
    Write-Host "  [!!] Missing required files:" -ForegroundColor Red
    foreach ($f in $missingFiles) {
        Write-Host "      - $f" -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "       Run [4] Build first to create these files." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------------------
# Find the .iss script
# ---------------------------------------------------------------------------
$issFile = Get-ChildItem $InstallerDir -Filter "*.iss" -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $issFile) {
    # Try project root
    $issFile = Get-ChildItem $ProjectRoot -Filter "*.iss" -ErrorAction SilentlyContinue | Select-Object -First 1
}

if (-not $issFile) {
    Write-Host ""
    Write-Host "  [!!] No Inno Setup script (.iss) found." -ForegroundColor Red
    Write-Host "       Create one in the 'installer' folder or project root." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

Write-Host "  Using script: $($issFile.FullName)" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Compile the installer
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  Compiling installer..." -ForegroundColor Cyan
Write-Host ""

$outputDir = "$ProjectRoot\output"
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$compileCmd = "& `"$isccPath`" `/o`"$outputDir`" `"$($issFile.FullName)`""
Write-Host "  $compileCmd" -ForegroundColor DarkGray
Write-Host ""

Invoke-Expression $compileCmd

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  [!!] Installer compilation failed." -ForegroundColor Red
    Write-Host "       Check errors above." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# ---------------------------------------------------------------------------
# Show results
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  Installer created:" -ForegroundColor Green

Get-ChildItem $outputDir -Filter "*.exe" | ForEach-Object {
    Write-Host "    + $($_.FullName)" -ForegroundColor Green
}

Write-Host ""
Write-Host "================================================================================" -ForegroundColor Green
Write-Host "  INSTALLER CREATION SUCCESSFUL" -ForegroundColor Green
Write-Host "================================================================================"
Write-Host ""
