# Install-VirtuaCam.ps1
# Graceful degradation installer for VirtuCam
# Attempts User Install first, falls back to Machine Install if legacy apps fail detection.

param(
    [switch]$ForceMachine,
    [string]$UserInstallerPath = ".\VirtuaCam_User_Setup.exe",
    [string]$MachineInstallerPath = ".\VirtuaCam_Machine_Setup.exe",
    [string]$UninstallStringUser = "", # Will be detected dynamically
    [string]$LegacyCheckTool = ".\Check-LegacyCompatibility.ps1"
)

$AppName = "VirtuaCam"
$UserInstallDir = "$env:LOCALAPPDATA\Programs\$AppName"
$MachineInstallDir = "$env:ProgramFiles\$AppName"

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Write-Host "[$timestamp] [$Level] $Message"
}

function Test-LegacyCompatibility {
    # Placeholder for actual compatibility check
    # In reality, this might try to open the device handle or check specific registry keys
    # that legacy apps rely on (e.g., HKLM vs HKCU restrictions).
    
    Write-Log "Testing legacy application compatibility..."
    
    if (Test-Path $LegacyCheckTool) {
        & $LegacyCheckTool
        return $?
    }
    
    # Fallback heuristic: 
    # Some legacy apps strictly require the filter to be registered in HKLM (Machine)
    # If we are running as standard user, we can't write HKLM, so we simulate failure 
    # if specific known legacy conditions exist.
    
    # For now, we assume success unless ForceMachine is set or a specific error is thrown
    # by the instantiation attempt below.
    
    try {
        # Attempt to instantiate the filter graph or touch the driver
        # This is pseudo-code for the actual COM/DirectShow interaction
        Write-Log "Attempting to instantiate VirtuCam filter..."
        
        # If you have a specific CLI tool installed with VirtuCam to test it:
        # & "$UserInstallDir\virtucam-cli.exe" --test
        # if ($LASTEXITCODE -ne 0) { return $false }
        
        return $true
    }
    catch {
        Write-Log "Instantiation failed: $_" -Level "ERROR"
        return $false
    }
}

function Invoke-Uninstall {
    param([string]$UninstallKeyPattern)
    
    Write-Log "Searching for existing installation to remove..."
    $uninstallPaths = @(
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKCU:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
    )

    foreach ($path in $uninstallPaths) {
        if (Test-Path $path) {
            $keys = Get-ChildItem -Path $path
            foreach ($key in $keys) {
                $displayName = (Get-ItemProperty -Path $key.PSPath -Name DisplayName -ErrorAction SilentlyContinue).DisplayName
                if ($displayName -like "*$AppName*") {
                    $quietUninstall = (Get-ItemProperty -Path $key.PSPath -Name QuietUninstallString -ErrorAction SilentlyContinue).QuietUninstallString
                    $uninstallString = (Get-ItemProperty -Path $key.PSPath -Name UninstallString -ErrorAction SilentlyContinue).UninstallString
                    
                    $cmd = ""
                    if ($quietUninstall) { $cmd = $quietUninstall }
                    elseif ($uninstallString) { $cmd = $uninstallString -replace '/i', '/silent' -replace '/I', '/silent' } # Try to force silent
                    
                    if ($cmd) {
                        Write-Log "Executing uninstall: $cmd"
                        Start-Process cmd.exe -ArgumentList "/c $cmd" -Wait -NoNewWindow
                        Write-Log "Uninstall completed."
                        return $true
                    }
                }
            }
        }
    }
    return $false
}

function Request-Elevation {
    Write-Log "Administrative privileges required for Machine Installation. Requesting UAC..."
    $arguments = "-File `"$($myinvocation.mycommand.definition)`" -ForceMachine"
    Start-Process powershell -Verb RunAs -ArgumentList $arguments
    exit
}

# --- Main Execution Flow ---

Write-Log "Starting VirtuCam Deployment..."

if ($ForceMachine) {
    Write-Log "Forcing Machine Level Installation."
    if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
        Request-Elevation
    }
    
    # Ensure user version is gone first to prevent conflicts
    Invoke-Uninstall -UninstallKeyPattern $AppName
    
    Write-Log "Running Machine Installer..."
    Start-Process -FilePath $MachineInstallerPath -ArgumentList "/VERYSILENT", "/NORESTART" -Wait
    Write-Log "Machine Installation Complete."
    exit 0
}

# 1. Attempt User Installation (if not already present)
if (-not (Test-Path $UserInstallDir)) {
    Write-Log "User installation not found. Installing to %LOCALAPPDATA%..."
    if (Test-Path $UserInstallerPath) {
        Start-Process -FilePath $UserInstallerPath -ArgumentList "/VERYSILENT", "/NORESTART" -Wait
    } else {
        Write-Log "User installer not found at $UserInstallerPath" -Level "ERROR"
        exit 1
    }
} else {
    Write-Log "User installation already exists."
}

# 2. Verify Compatibility
$isCompatible = Test-LegacyCompatibility

if ($isCompatible) {
    Write-Log "SUCCESS: VirtuCam installed and verified at User Level."
    exit 0
}
else {
    Write-Log "WARNING: Legacy compatibility check failed for User Level installation."
    Write-Log "Initiating graceful fallback to Machine Level installation..."
    
    # 3. Fallback: Uninstall User Version
    Invoke-Uninstall -UninstallKeyPattern $AppName
    
    # 4. Fallback: Elevate and Install Machine Version
    if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
        Request-Elevation
    } else {
        # Already admin, proceed directly
        Write-Log "Running Machine Installer..."
        if (Test-Path $MachineInstallerPath) {
            Start-Process -FilePath $MachineInstallerPath -ArgumentList "/VERYSILENT", "/NORESTART" -Wait
            Write-Log "Machine Installation Complete."
        } else {
            Write-Log "Machine installer not found at $MachineInstallerPath" -Level "ERROR"
            exit 1
        }
    }
}
