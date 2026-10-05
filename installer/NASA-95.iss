; NASA-95 Windows 설치 파일(Inno Setup 6). scripts\package.ps1 이 만든 dist\NASA-95\ 를 묶는다.
;   ISCC /DAppVersion=0.1.0 /DAppDir=..\dist\NASA-95 /DOutDir=..\dist installer\NASA-95.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppDir
  #define AppDir "..\dist\NASA-95"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{7C0E7D2E-3C8A-4C8B-9B57-A95A95000001}
AppName=NASA-95
AppVersion={#AppVersion}
AppVerName=NASA-95 {#AppVersion}
AppPublisher=NASA-95 project
AppPublisherURL=https://github.com/kwonbyeongchun/Nasa-95
AppSupportURL=https://github.com/kwonbyeongchun/Nasa-95/issues
DefaultDirName={autopf}\NASA-95
DefaultGroupName=NASA-95
UninstallDisplayIcon={app}\nasa95.ico
OutputDir={#OutDir}
OutputBaseFilename=NASA-95-{#AppVersion}-setup
SetupIconFile=nasa95.ico
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
#ifexist "compiler:Languages\Korean.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "*.nasa95 프로젝트 파일을 NASA-95 로 연다"; GroupDescription: "파일 연결:"

[Files]
Source: "{#AppDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "nasa95.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\NASA-95"; Filename: "{app}\python\pythonw.exe"; Parameters: "-m nasa95.ui"; WorkingDir: "{app}"; IconFilename: "{app}\nasa95.ico"
Name: "{group}\NASA-95 (콘솔)"; Filename: "{app}\python\python.exe"; Parameters: "-m nasa95.ui"; WorkingDir: "{app}"; IconFilename: "{app}\nasa95.ico"
Name: "{group}\{cm:UninstallProgram,NASA-95}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\NASA-95"; Filename: "{app}\python\pythonw.exe"; Parameters: "-m nasa95.ui"; WorkingDir: "{app}"; IconFilename: "{app}\nasa95.ico"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.nasa95"; ValueType: string; ValueName: ""; ValueData: "NASA95.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\NASA95.Project"; ValueType: string; ValueName: ""; ValueData: "NASA-95 프로젝트"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\NASA95.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\nasa95.ico"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\NASA95.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\python\pythonw.exe"" -m nasa95.ui --project ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\python\pythonw.exe"; Parameters: "-m nasa95.ui"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,NASA-95}"; Flags: nowait postinstall skipifsilent
