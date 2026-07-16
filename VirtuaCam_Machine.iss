; Inno Setup Script for VirtuCam (Machine Installation)
; Installs to %ProgramFiles%\VirtuaCam
; Requires Administrator Privileges

[Setup]
AppName=VirtuaCam
AppVersion=1.0
AppPublisher=YourName
DefaultDirName={pf}\VirtuaCam
DefaultGroupName=VirtuaCam
DisableProgramGroupPage=yes
OutputBaseFilename=VirtuaCam_Machine_Setup
PrivilegesRequired=admin
Compression=lzma
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "bin\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
; Add your driver binaries, filters, and DLLs here

[Icons]
Name: "{autoprograms}\VirtuaCam"; Filename: "{app}\VirtuaCam.exe"
Name: "{autodesktop}\VirtuaCam"; Filename: "{app}\VirtuaCam.exe"; Tasks: desktopicon

[Run]
; Register the virtual camera filter after installation (System-wide)
; Adjust the command line to match your actual registration tool
Filename: "{app}\register_filter.exe"; Parameters: "--machine --silent"; Flags: waituntilterminated runascurrentuser

[UninstallRun]
; Unregister the filter on uninstall
Filename: "{app}\unregister_filter.exe"; Parameters: "--machine --silent"; Flags: waituntilterminated runascurrentuser

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Optional: Check if a user version is installed and offer to remove it
  if RegKeyExists(HKCU, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VirtuaCam_is1') then
  begin
    if MsgBox('A User-Level installation of VirtuaCam was detected. Would you like to uninstall it now to prevent conflicts?', mbConfirmation, MB_YESNO) = IDYES then
    begin
      // Trigger the uninstall string for the user version
      // This requires querying the registry for the exact UninstallString
      // For simplicity, we just warn here. The PowerShell wrapper handles the actual removal.
      MsgBox('Please run the provided PowerShell wrapper to automatically handle the transition.', mbInformation, MB_OK);
    end;
  end;
end;
