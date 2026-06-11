; =============================================================================
; VirtuaCam.iss  --  Inno Setup installer definition
; =============================================================================
; Build the project first (.\build.ps1), then compile this script with the
; Inno Setup Compiler (https://jrsoftware.org/isinfo.php):
;
;   iscc installer\VirtuaCam.iss
;
; The installer:
;   - copies all binaries to {autopf}\VirtuaCam
;   - registers the virtual camera COM server (regsvr32 DirectPortClient.dll)
;   - unregisters it again on uninstall
;   - offers optional Start Menu / desktop / run-at-startup integration
;
; NOTE: VirtuaCam.exe currently requires elevation at runtime, so the
; run-at-startup task will trigger a UAC prompt at logon.  Once the planned
; Windows-service split lands (docs/WINDOWS_SERVICE.md), the tray app becomes
; non-elevated and this caveat goes away.
; =============================================================================

#define MyAppName "VirtuaCam"
#define MyAppVersion "1.0.0"
#define MyAppExeName "VirtuaCam.exe"
#define BuildDir "..\build\Release"

[Setup]
AppId={{C7E0A1D4-5B2F-4E83-9A41-2F6D87C0B9E3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=MansfieldPlumbing
AppPublisherURL=https://github.com/MansfieldPlumbing/VirtuaCam
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=VirtuaCam-{#MyAppVersion}-Setup
SetupIconFile=..\src\VirtuaCam\App.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart";   Description: "Start {#MyAppName} when Windows starts";   GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\VirtuaCam.exe";                  DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\VirtuaCamProcess.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortClient.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortBroker.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortMFCamera.dll";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortMFGraphicsCapture.dll";DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortConsumer.dll";         DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";       Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Matches the in-app Settings > "Start with Windows" toggle (same key/value).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VirtuaCam"; ValueData: """{app}\{#MyAppExeName}"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
; Register the virtual camera COM server (requires the elevation we already have).
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\DirectPortClient.dll"""; StatusMsg: "Registering virtual camera..."; Flags: runhidden
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\DirectPortClient.dll"""; RunOnceId: "UnregisterVCam"; Flags: runhidden
