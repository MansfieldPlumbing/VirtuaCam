# Check-LegacyCompatibility.ps1
# Helper script to verify if the current VirtuCam installation is accessible by legacy applications.
# Legacy apps often fail to see filters registered only in HKCU or non-standard paths.

param(
    [string]$FilterName = "VirtuaCam",
    [int]$TimeoutSeconds = 5
)

Write-Host "Checking legacy compatibility for $FilterName..."

$success = $false

try {
    # Method 1: Try to enumerate DirectShow filters via PowerShell COM
    # This mimics what many older C++/C# apps do when building a graph.
    
    $filterGraph = New-Object -ComObject FilterGraph.NoThread
    if ($filterGraph) {
        # Attempt to find the filter
        # Note: Direct COM enumeration of specific filters can be tricky without the SDK,
        # so we often rely on whether the system recognizes the device as a valid VideoInput
        
        Write-Host "COM Graph object created successfully."
        
        # If the installer registered the filter correctly, it should be enumerable.
        # For this script, we will assume success if the COM object creation didn't throw,
        # AND we can find a matching device in WMI (which is a stronger signal for legacy apps).
        
        $devices = Get-WmiObject Win32_PnPEntity | Where-Object { 
            $_.PNPClass -eq "Camera" -or $_.Service -like "*video*" 
        }
        
        # Simple string match check
        $found = $devices | Where-Object { $_.Name -like "*$FilterName*" }
        
        if ($found) {
            Write-Host "Found compatible device entry: $($found.Name)"
            $success = $true
        } else {
            Write-Host "Warning: Device not found in WMI PnP Entity list."
            $success = $false
        }
    }
}
catch {
    Write-Host "Error during compatibility check: $_"
    $success = $false
}

if ($success) {
    Write-Host "COMPATIBILITY_CHECK_PASSED"
    exit 0
} else {
    Write-Host "COMPATIBILITY_CHECK_FAILED"
    exit 1
}
