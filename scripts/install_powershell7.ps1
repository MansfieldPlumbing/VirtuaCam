# install_powershell7.ps1
# Installs PowerShell 7 from GitHub with informed consent.
# Called by setup.ps1 - do not run directly.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "=============================================" -ForegroundColor Green
Write-Host "      PowerShell 7 Deployment              " -ForegroundColor Green
Write-Host "=============================================" -ForegroundColor Green
Write-Host ""

# Initialize TLS 1.2 for secure downloads
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

try {
    Write-Host "  Fetching latest release information from GitHub..." -ForegroundColor Cyan
    $apiResult = Invoke-WebRequest -Uri "https://api.github.com/repos/PowerShell/PowerShell/releases/latest" -UseBasicParsing | ConvertFrom-Json
    Write-Host "  Latest version: $($apiResult.tag_name)" -ForegroundColor DarkGray
    Write-Host ""
} catch {
    Write-Host ""
    Write-Host "  ERROR: Failed to fetch release information from GitHub." -ForegroundColor Red
    Write-Host "  Please check your internet connection and try again." -ForegroundColor Yellow
    Write-Host "  Or download manually: https://github.com/PowerShell/PowerShell/releases/latest" -ForegroundColor White
    Write-Host ""
    pause
    exit 1
}

# MENU 1: Package Type Selection
Write-Host "Select installation format:" -ForegroundColor White
Write-Host "  1) MSI Installer (System-wide, requires Admin)" -ForegroundColor DarkGray
Write-Host "  2) Portable ZIP (Isolated, no Admin required)" -ForegroundColor DarkGray
Write-Host ""
$typeChoice = Read-Host "  Enter choice [1-2]"

$asset = $null
$downloadPath = $null

if ($typeChoice -eq "1") {
    $asset = $apiResult.assets | Where-Object { $_.name -like "*win-x64.msi" } | Select-Object -First 1
    if (-not $asset) {
        Write-Host ""
        Write-Host "  ERROR: Could not find MSI asset for win-x64." -ForegroundColor Red
        exit 1
    }
    $downloadPath = Join-Path $env:TEMP $asset.name
} else {
    $asset = $apiResult.assets | Where-Object { $_.name -like "*win-x64.zip" } | Select-Object -First 1
    if (-not $asset) {
        Write-Host ""
        Write-Host "  ERROR: Could not find ZIP asset for win-x64." -ForegroundColor Red
        exit 1
    }
    $downloadPath = Join-Path $env:TEMP $asset.name

    # MENU 2: Extraction Target
    Write-Host ""
    Write-Host "Select target extraction directory:" -ForegroundColor White
    Write-Host "  1) C:\bin\pwsh" -ForegroundColor DarkGray
    Write-Host "  2) Local AppData ($env:LOCALAPPDATA\Pwsh)" -ForegroundColor DarkGray
    Write-Host "  3) Custom Path" -ForegroundColor DarkGray
    Write-Host ""
    $dirChoice = Read-Host "  Enter choice [1-3]"

    switch ($dirChoice) {
        "1" { $extractDir = "C:\bin\pwsh" }
        "2" { $extractDir = "$env:LOCALAPPDATA\Pwsh" }
        "3" { 
            $customPath = Read-Host "  Enter absolute path (e.g., D:\Tools\pwsh)"
            $extractDir = $customPath.Trim()
        }
        Default { 
            Write-Host "  Invalid choice. Defaulting to C:\bin\pwsh" -ForegroundColor Yellow
            $extractDir = "C:\bin\pwsh" 
        }
    }
}

# PROCESS: Download
Write-Host ""
Write-Host "  Downloading $($asset.name)..." -ForegroundColor Cyan
Write-Host "  Source: $($asset.browser_download_url)" -ForegroundColor DarkGray
Write-Host "  Destination: $downloadPath" -ForegroundColor DarkGray
Write-Host ""

try {
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $downloadPath -UseBasicParsing
    Write-Host "  Download complete." -ForegroundColor Green
} catch {
    Write-Host ""
    Write-Host "  ERROR: Download failed." -ForegroundColor Red
    Write-Host "  $_" -ForegroundColor DarkYellow
    Write-Host ""
    pause
    exit 1
}

# PROCESS: Deployment
if ($typeChoice -eq "1") {
    # MSI Installation
    Write-Host ""
    Write-Host "  Launching MSI installer (silent with progress bar)..." -ForegroundColor Cyan
    
    try {
        Start-Process msiexec.exe -ArgumentList "/i `"$downloadPath`" /passive /norestart" -Wait
        Remove-Item $downloadPath -Force -ErrorAction SilentlyContinue
        $binDir = "C:\Program Files\PowerShell\7"
        Write-Host "  Installation complete." -ForegroundColor Green
    } catch {
        Write-Host ""
        Write-Host "  ERROR: MSI installation failed." -ForegroundColor Red
        Write-Host "  $_" -ForegroundColor DarkYellow
        Write-Host ""
        pause
        exit 1
    }
} else {
    # ZIP Extraction
    Write-Host ""
    Write-Host "  Extracting files to $extractDir..." -ForegroundColor Cyan
    
    try {
        if (!(Test-Path $extractDir)) {
            New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
        }
        Expand-Archive -Path $downloadPath -DestinationPath $extractDir -Force
        Remove-Item $downloadPath -Force -ErrorAction SilentlyContinue
        $binDir = $extractDir
        Write-Host "  Extraction complete." -ForegroundColor Green
    } catch {
        Write-Host ""
        Write-Host "  ERROR: Extraction failed." -ForegroundColor Red
        Write-Host "  $_" -ForegroundColor DarkYellow
        Write-Host ""
        pause
        exit 1
    }
}

# MENU 3: Environment Path Update
Write-Host ""
Write-Host "Do you want to add this PowerShell instance to your User PATH?" -ForegroundColor Yellow
Write-Host "  This allows you to run 'pwsh' from any terminal." -ForegroundColor DarkGray
Write-Host ""
$pathChoice = Read-Host "  [Y]es / [N]o"

if ($pathChoice -match "y|yes") {
    # Fetch current user-level PATH env variable
    $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    
    # Check if the directory is already registered
    if ($currentPath -notlike "*$binDir*") {
        # Append clean separation semi-colon if path isn't empty
        $newPath = if ([string]::IsNullOrEmpty($currentPath)) { $binDir } else { "$currentPath;$binDir" }
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host ""
        Write-Host "  Success! Added '$binDir' to User PATH." -ForegroundColor Green
        Write-Host "  Note: You will need to restart your terminal window to see the changes." -ForegroundColor Gray
    } else {
        Write-Host ""
        Write-Host "  Directory is already present in your PATH." -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=============================================" -ForegroundColor Green
Write-Host "  Deployment complete!" -ForegroundColor Green
Write-Host "=============================================" -ForegroundColor Green
Write-Host ""
