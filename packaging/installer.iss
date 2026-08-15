#define AppName "FullBlur Filter"
#define AppVersion "0.9.0"
#define AppPublisher "Sergei Kulishov"
#define AppURL "https://example.com"
#define AppId "{{fae952af-9996-4b98-b10d-bf66f00e8a57}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} beta
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
DefaultDirName={code:GetObsPath}
DisableDirPage=no
DirExistsWarning=no
AppendDefaultDirName=no
PrivilegesRequired=admin
UsedUserAreasWarning=no
OutputBaseFilename=fullblur-filter-0.9.0-setup
OutputDir=Output
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\obs-plugins\64bit\fullblur-filter.dll
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoCompany={#AppPublisher}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "..\build_x64\rundir\RelWithDebInfo\fullblur-filter.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\build_x64\rundir\RelWithDebInfo\onnxruntime.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\build_x64\rundir\RelWithDebInfo\onnxruntime_providers_shared.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\build_x64\rundir\RelWithDebInfo\DirectML.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\build_x64\rundir\RelWithDebInfo\fullblur-filter\locale\*"; DestDir: "{app}\data\obs-plugins\fullblur-filter\locale"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\THIRD_PARTY_LICENSES\*"; DestDir: "{app}\data\obs-plugins\fullblur-filter\THIRD_PARTY_LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "README.txt"; DestDir: "{app}\data\obs-plugins\fullblur-filter"; Flags: ignoreversion
Source: "README_RU.txt"; DestDir: "{app}\data\obs-plugins\fullblur-filter"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\fullblur-filter"; Check: RemoveSettings

[Code]
var
  DeleteSettings: Boolean;

function GetObsPath(Param: string): string;
var
  Path: string;
begin
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Path) then
    Result := Path
  else
    Result := 'C:\Program Files\obs-studio';
end;

function IsObsRunning(): Boolean;
var
  ResCode: Integer;
begin
  Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -Command "exit [int]($null -ne (Get-Process obs64 -ErrorAction SilentlyContinue))"', '', SW_HIDE, ewWaitUntilTerminated, ResCode);
  Result := ResCode = 1;
end;

function InitializeSetup(): Boolean;
var
  Version: string;
  MajorStr: string;
  Major: Integer;
  DotPos: Integer;
begin
  Result := true;
  while IsObsRunning() do
  begin
    if MsgBox('OBS Studio is running. Please close it before continuing.', mbCriticalError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := false;
      Exit;
    end;
  end;

  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'DisplayVersion', Version) then
  begin
    DotPos := Pos('.', Version);
    if DotPos > 0 then
      MajorStr := Copy(Version, 1, DotPos - 1)
    else
      MajorStr := Version;
    Major := StrToIntDef(MajorStr, 0);
    if (Major > 0) and (Major < 32) then
      MsgBox('Warning: this build of FullBlur Filter is intended for OBS Studio 32.x. Detected version: ' + Version + '. Installation continues, but the plugin may not load.', mbInformation, MB_OK);
  end;
end;

function RemoveSettings(): Boolean;
begin
  Result := DeleteSettings;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    DeleteSettings := (MsgBox('Delete FullBlur Filter settings and downloaded models?' + #13#10 + 'This will remove %APPDATA%\fullblur-filter.', mbConfirmation, MB_YESNO) = IDYES);
end;
