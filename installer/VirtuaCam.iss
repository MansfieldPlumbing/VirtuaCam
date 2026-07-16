; =============================================================================
; VirtuaCam.iss  --  Inno Setup installer definition
; =============================================================================
; Build the project first (.\build.ps1 or via setup menu), then compile this 
; script with the Inno Setup Compiler (https://jrsoftware.org/isinfo.php):
;
;   iscc installer\VirtuaCam.iss
;
; The installer:
;   - copies all binaries to {autopf}\VirtuaCam (or {localappdata} for user install)
;   - registers the virtual camera COM server (regsvr32 DirectPortClient.dll)
;   - unregisters it again on uninstall
;   - offers optional Start Menu / desktop / run-at-startup integration
;   - supports both machine-wide (admin) and per-user installation modes
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
AppSupportURL=https://github.com/MansfieldPlumbing/VirtuaCam/issues
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=VirtuaCam-{#MyAppVersion}-Setup
SetupIconFile=..\src\VirtuaCam\App.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern/dynamic

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart";   Description: "Start {#MyAppName} when Windows starts";   GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\VirtuaCam.exe";                  DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\VirtuaCamProcess.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortClient.dll";           DestDir: "{app}"; Flags: ignoreversion regserver
Source: "{#BuildDir}\DirectPortBroker.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortMFCamera.dll";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortMFGraphicsCapture.dll";DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DirectPortConsumer.dll";         DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";       Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Matches the in-app Settings > "Start with Windows" toggle (same key/value).
; Uses HKCU for per-user installs, HKLM for machine-wide (when admin)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VirtuaCam"; ValueData: """{app}\{#MyAppExeName}"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
; Register the virtual camera COM server (requires the elevation we already have).
; The regserver flag in [Files] handles this, but we add explicit call for clarity.
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\DirectPortClient.dll"""; StatusMsg: "Registering virtual camera..."; Flags: runhidden
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Unregister the virtual camera COM server on uninstall.
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\DirectPortClient.dll"""; RunOnceId: "UnregisterVCam"; Flags: runhidden

[Code]
// =============================================================================
// CODE SECTION - Handle installation mode detection and migration
// =============================================================================

var
  PreviousInstallPath: String;
  IsUpgradeInstall: Boolean;
  InstallModeChoice: Integer;

function InitializeSetup(): Boolean;
var
  UninstallString: String;
  CurrentModeIsUser: Boolean;
  DetectedUserInstall: Boolean;
  DetectedMachineInstall: Boolean;
begin
  Result := True;
  PreviousInstallPath := '';
  IsUpgradeInstall := False;
  InstallModeChoice := 0;
  
  CurrentModeIsUser := not IsAdminLoggedOn;
  
  DetectedUserInstall := RegKeyExists(HKCU, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppName}_is1');
  if DetectedUserInstall then
  begin
    RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppName}_is1', 'UninstallString', UninstallString);
    if Length(UninstallString) > 0 then
      PreviousInstallPath := Copy(UninstallString, 2, Pos('"', Copy(UninstallString, 2, Length(UninstallString))) - 1);
  end;
  
  DetectedMachineInstall := RegKeyExists(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppName}_is1_is1');
  if DetectedMachineInstall then
  begin
    RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppName}_is1_is1', 'UninstallString', UninstallString);
    if Length(UninstallString) > 0 then
      PreviousInstallPath := Copy(UninstallString, 2, Pos('"', Copy(UninstallString, 2, Length(UninstallString))) - 1);
  end;
  
  if (CurrentModeIsUser and DetectedMachineInstall) or (not CurrentModeIsUser and DetectedUserInstall) then
  begin
    if CurrentModeIsUser then
    begin
      if MsgBox('A Machine-Level installation of VirtuaCam was detected at:' + #13#10 + #13#10 + 
                PreviousInstallPath + #13#10 + #13#10 + 
                'Running both simultaneously can cause conflicts.' + #13#10 + #13#10 + 
                'Click YES to uninstall the machine version and continue with user install.' + #13#10 + 
                'Click NO to cancel and keep the machine version.', mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False;
        Exit;
      end;
      ShellExec('', 'cmd.exe', '/c "' + PreviousInstallPath + '\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES', '', SW_HIDE, ewWaitUntilTerminated, InstallModeChoice);
    end
    else
    begin
      if MsgBox('A User-Level installation of VirtuaCam was detected at:' + #13#10 + #13#10 + 
                PreviousInstallPath + #13#10 + #13#10 + 
                'Running both simultaneously can cause conflicts.' + #13#10 + #13#10 + 
                'Click YES to uninstall the user version and continue with machine install.' + #13#10 + 
                'Click NO to cancel and keep the user version.', mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False;
        Exit;
      end;
      ShellExec('', 'cmd.exe', '/c "' + PreviousInstallPath + '\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES', '', SW_HIDE, ewWaitUntilTerminated, InstallModeChoice);
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    Log('Post-install completed for ' + ExpandConstant('{app}'));
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = wpSelectProgramGroup then
    Result := True;
end;
