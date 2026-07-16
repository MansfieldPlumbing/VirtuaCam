; Inno Setup Script for VirtuCam (User Installation)
; Installs to %LOCALAPPDATA%\Programs\VirtuaCam

[Setup]
AppName=VirtuaCam
AppVersion=1.0
AppPublisher=YourName
DefaultDirName={localappdata}\Programs\VirtuaCam
DisableProgramGroupPage=yes
OutputBaseFilename=VirtuaCam_User_Setup
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
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
; Register the virtual camera filter after installation
; Adjust the command line to match your actual registration tool
Filename: "{app}\register_filter.exe"; Parameters: "--user --silent"; Flags: waituntilterminated
Filename: "{cmd}"; Parameters: "/C ""mklink /D ""{commonappdata}\VirtuaCam"" ""{app}"""""; Flags: runhidden; Check: not IsAdminLoggedOn; 
; Optional: Create a junction in ProgramData if legacy apps look there but can't see LocalAppData

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\VirtuaCam"

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Optional: Check if a machine version is installed and warn the user
  if RegKeyExists(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VirtuaCam_is1') then
  begin
    MsgBox('A Machine-Level installation of VirtuaCam was detected. It is recommended to uninstall it before proceeding with the User-Level installation.', mbInformation, MB_OK);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Additional post-install logic can go here
    // e.g., flushing the device manager cache
  end;
end;
