[Setup]
; --- Application Metadata ---
AppName=Fast Image Viewer
AppVersion=2.1
AppPublisher=KILLSWITCH
AppPublisherURL=https://github.com/killswitch
DefaultDirName={autopf}\FastImageViewer
DefaultGroupName=Fast Image Viewer
Compression=lzma2
SolidCompression=yes
OutputDir=D:\dev\FastImageViewer\installer
OutputBaseFilename=FastImageViewer_Setup_v2.0
ArchitecturesInstallIn64BitMode=x64
SetupIconFile=app_icon.ico
UninstallDisplayIcon={app}\app_icon.ico

[Files]
; --- Core Application Files ---
; The main executable
Source: "D:\dev\FastImageViewer\build\FastImageViewer.exe"; DestDir: "{app}"; Flags: ignoreversion
; All the required DLLs (GLFW, WIC, etc.) dynamically linked by vcpkg
Source: "D:\dev\FastImageViewer\build\vcpkg_installed\x64-windows\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "app_icon.ico"; DestDir: "{app}"; Flags: ignoreversion


[Icons]
; --- Shortcuts ---
Name: "{group}\Fast Image Viewer"; Filename: "{app}\FastImageViewer.exe"; IconFilename: "{app}\app_icon.ico"
Name: "{group}\Uninstall Fast Image Viewer"; Filename: "{uninstallexe}"; IconFilename: "{app}\app_icon.ico"
Name: "{autodesktop}\Fast Image Viewer"; Filename: "{app}\FastImageViewer.exe"; IconFilename: "{app}\app_icon.ico"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Registry]
; --- The "Final Boss" File Associations ---
; 1. Register the application in the Windows "Open With" dialog
Root: HKCR; Subkey: "Applications\FastImageViewer.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Fast Image Viewer"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Applications\FastImageViewer.exe\SupportedTypes"; ValueType: string; ValueName: ".jpg"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: "Applications\FastImageViewer.exe\SupportedTypes"; ValueType: string; ValueName: ".jpeg"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: "Applications\FastImageViewer.exe\SupportedTypes"; ValueType: string; ValueName: ".png"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: "Applications\FastImageViewer.exe\SupportedTypes"; ValueType: string; ValueName: ".bmp"; ValueData: ""; Flags: uninsdeletevalue

; 2. Define the command Windows should run when an image is clicked
Root: HKCR; Subkey: "Applications\FastImageViewer.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\FastImageViewer.exe"" ""%1"""; Flags: uninsdeletekey

; 3. Create a Custom Program ID (ProgID) for Windows 10/11 Default Apps menu
Root: HKCR; Subkey: "FastImageViewer.Image"; ValueType: string; ValueName: ""; ValueData: "Fast Image File"; Flags: uninsdeletekey
Root: HKCR; Subkey: "FastImageViewer.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\FastImageViewer.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCR; Subkey: "FastImageViewer.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\FastImageViewer.exe,0"; Flags: uninsdeletekey

; 4. Politely associate our ProgID with the extensions (Allows users to easily set it as default)
Root: HKCR; Subkey: ".jpg\OpenWithProgids"; ValueType: string; ValueName: "FastImageViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".jpeg\OpenWithProgids"; ValueType: string; ValueName: "FastImageViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".png\OpenWithProgids"; ValueType: string; ValueName: "FastImageViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".bmp\OpenWithProgids"; ValueType: string; ValueName: "FastImageViewer.Image"; ValueData: ""; Flags: uninsdeletevalue

[Run]
; --- Launch after install ---
Filename: "{app}\FastImageViewer.exe"; Description: "{cm:LaunchProgram,Fast Image Viewer}"; Flags: nowait postinstall skipifsilent