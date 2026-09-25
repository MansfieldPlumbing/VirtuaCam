; =============================================================================
; VirtuaCam.iss  --  Inno Setup 6 installer
; =============================================================================
; Build with:  .\build.ps1 -Installer
;
; Installs to Program Files (the camera DLL must be registered machine-wide
; because the Windows Camera Frame Server only reads HKLM), registers
; VirtuaCamSource.dll, and unregisters it on uninstall.  VirtuaCam.exe itself
; runs as a normal user afterwards.
; =============================================================================

#define AppName "VirtuaCam"
#define AppVersion "2.0.0"
#ifndef BinDir
  #define BinDir "..\build\bin\Release"
#endif

[Setup]
AppId={{C7E0A1D4-5B2F-4E83-9A41-2F6D87C0B9E3}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=MansfieldPlumbing
AppPublisherURL=https://github.com/MansfieldPlumbing/VirtuaCam
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir=Output
OutputBaseFilename=VirtuaCam-{#AppVersion}-Setup
SetupIconFile=..\src\App\App.ico
UninstallDisplayIcon={app}\VirtuaCam.exe
CloseApplications=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "autostart"; Description: "Start {#AppName} when I sign in"; Flags: unchecked

[Files]
Source: "{#BinDir}\VirtuaCam.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\VirtuaCamSource.dll";  DestDir: "{app}"; Flags: ignoreversion regserver

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\VirtuaCam.exe"

[Registry]
; Same value the tray menu's "Start with Windows" toggles.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VirtuaCam"; ValueData: """{app}\VirtuaCam.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\VirtuaCam.exe"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser
