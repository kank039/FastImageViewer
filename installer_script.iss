; Inno Setup script for Fast Image Viewer v3.
; Build the "release" CMake preset first; paths are relative to this script.
; CI passes the version: iscc /DAppVersion=3.2.0 installer_script.iss

#ifndef AppVersion
  #define AppVersion "3.2"
#endif
#ifndef NumericVersion
  #define NumericVersion "3.2"
#endif

[Setup]
AppName=Fast Image Viewer
AppVersion={#AppVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=KILLSWITCH
AppPublisherURL=https://github.com/killswitch
DefaultDirName={autopf}\FastImageViewer
DefaultGroupName=Fast Image Viewer
Compression=lzma2
SolidCompression=yes
OutputDir=installer
OutputBaseFilename=FastImageViewer_Setup_v{#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=app_icon.ico
UninstallDisplayIcon={app}\FastImageViewer.exe
ChangesAssociations=yes
; Installs for the current user without administrator rights by default;
; "Install for all users" is offered in the setup dialog.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline

[Files]
; v3 is a single statically linked executable: no DLLs to ship.
Source: "build\release\FastImageViewer.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Fast Image Viewer"; Filename: "{app}\FastImageViewer.exe"
Name: "{group}\Uninstall Fast Image Viewer"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Fast Image Viewer"; Filename: "{app}\FastImageViewer.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Registry]
; "Open with" entry and command
Root: HKA; Subkey: "Software\Classes\Applications\FastImageViewer.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Fast Image Viewer"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\FastImageViewer.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\FastImageViewer.exe"" ""%1"""
; ProgID used by Windows "Default apps"
Root: HKA; Subkey: "Software\Classes\FastImageViewer.Image"; ValueType: string; ValueName: ""; ValueData: "Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\FastImageViewer.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\FastImageViewer.exe,0"
Root: HKA; Subkey: "Software\Classes\FastImageViewer.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\FastImageViewer.exe"" ""%1"""

[Code]
const
  Exts = '.jpg .jpeg .jpe .jfif .png .bmp .dib .gif .tif .tiff .ico .jxr .wdp .hdp .dds .webp .heic .heif .hif .avif .jxl';

procedure RegisterExtensions(Root: Integer);
var
  List, Ext: String;
  P: Integer;
begin
  List := Exts + ' ';
  while Length(List) > 0 do
  begin
    P := Pos(' ', List);
    Ext := Copy(List, 1, P - 1);
    Delete(List, 1, P);
    if Ext <> '' then
    begin
      RegWriteStringValue(Root, 'Software\Classes\Applications\FastImageViewer.exe\SupportedTypes', Ext, '');
      RegWriteStringValue(Root, 'Software\Classes\' + Ext + '\OpenWithProgids', 'FastImageViewer.Image', '');
    end;
  end;
end;

procedure UnregisterExtensions(Root: Integer);
var
  List, Ext: String;
  P: Integer;
begin
  List := Exts + ' ';
  while Length(List) > 0 do
  begin
    P := Pos(' ', List);
    Ext := Copy(List, 1, P - 1);
    Delete(List, 1, P);
    if Ext <> '' then
      RegDeleteValue(Root, 'Software\Classes\' + Ext + '\OpenWithProgids', 'FastImageViewer.Image');
  end;
end;

function RootKey: Integer;
begin
  if IsAdminInstallMode then Result := HKLM else Result := HKCU;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then RegisterExtensions(RootKey);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then UnregisterExtensions(RootKey);
end;

[Run]
Filename: "{app}\FastImageViewer.exe"; Description: "{cm:LaunchProgram,Fast Image Viewer}"; Flags: nowait postinstall skipifsilent
