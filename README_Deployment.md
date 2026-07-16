# README: VirtuCam Graceful Degradation Deployment

This solution implements a "Try User First, Fallback to Machine" installation strategy for VirtuCam. It addresses the friction legacy applications experience with per-user virtual camera drivers by automatically detecting compatibility issues and escalating privileges only when necessary.

## Components

### 1. `Install-VirtuaCam.ps1` (The Orchestrator)
This is the main entry point. It handles the logic flow:
1.  **Attempt User Install**: Installs to `%LOCALAPPDATA%\Programs\VirtuaCam` without requiring Admin rights.
2.  **Verify Compatibility**: Runs a check to see if legacy apps can see the new camera.
3.  **Graceful Fallback**: If the check fails:
    *   Automatically uninstalls the User version.
    *   Requests UAC elevation (Admin rights).
    *   Installs to `%PROGRAMFILES%\VirtuaCam`.

**Usage:**
```powershell
.\Install-VirtuaCam.ps1
```

**Flags:**
*   `-ForceMachine`: Skips the user install attempt and goes straight to Program Files (requires Admin).

### 2. `Check-LegacyCompatibility.ps1` (The Detector)
A helper script called by the orchestrator. It attempts to instantiate the DirectShow graph or query WMI for the camera device.
*   **Why?** Some legacy apps (e.g., older Flash-based tools, specific C++ DirectShow hosts) cannot enumerate filters registered only in `HKCU` or loaded from non-system paths.
*   **Logic**: If this script returns exit code `1`, the orchestrator triggers the fallback.

### 3. `VirtuaCam_User.iss` (Inno Setup - User)
Configuration for the per-user installer.
*   **Key Setting**: `PrivilegesRequired=lowest` ensures no UAC prompt for the initial attempt.
*   **Path**: Installs to `{localappdata}\Programs`.
*   **Compatibility Trick**: Includes a step to create a Junction/Symlink in `{commonappdata}` (ProgramData) pointing to the user install. This helps some apps that look in standard locations but respect junctions.

### 4. `VirtuaCam_Machine.iss` (Inno Setup - Machine)
Configuration for the system-wide installer.
*   **Key Setting**: `PrivilegesRequired=admin` ensures full system registration.
*   **Path**: Installs to `{pf}\VirtuaCam`.
*   **Registration**: Registers the filter in `HKLM`, making it visible to all legacy applications immediately.

## How the "Graceful Degradation" Works

1.  **User Experience**: The user runs the script. No UAC prompt appears initially. The app installs quietly in their user folder.
2.  **Detection**: The script tests if the camera works.
    *   **Scenario A (Modern Apps)**: Zoom, Teams, OBS usually see the user-installed filter fine. **Result**: Success, script exits. No Admin rights needed.
    *   **Scenario B (Legacy Apps)**: An old app cannot find the camera because it only scans `HKLM`. **Result**: The test fails.
3.  **Recovery**:
    *   The script says: "Legacy compatibility failed. Switching to System Mode."
    *   It silently uninstalls the user version.
    *   Windows shows a UAC prompt (this is unavoidable for Program Files access).
    *   Upon approval, it installs the Machine version.
    *   Legacy apps now see the camera.

## Building the Installers

You need Inno Setup installed to compile the `.iss` files into `.exe` installers.

```bash
# Compile User Installer
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" VirtuaCam_User.iss

# Compile Machine Installer
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" VirtuaCam_Machine.iss
```

*Note: Ensure you have a `bin\` folder with your actual VirtuCam binaries (`register_filter.exe`, DLLs, etc.) before compiling.*

## Customization Notes

*   **Compatibility Check**: Edit `Check-LegacyCompatibility.ps1`. The current version checks WMI PnP entities. If you have a specific CLI tool provided by VirtuCam (e.g., `virtucam-cli.exe --test`), replace the logic there for a more accurate test.
*   **Registration Commands**: Update the `[Run]` sections in the `.iss` files to match the actual command line arguments your driver needs to register itself.
