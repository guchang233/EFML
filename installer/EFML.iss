; EFML installer. Derived from Better-Endfield's installer script (AGPL-3.0).
; Built by .github/workflows/build.yml:  ISCC /DStageDir=... /DOutputDir=... /DAppVersion=...
#ifndef StageDir
  #error StageDir must be supplied
#endif
#ifndef OutputDir
  #error OutputDir must be supplied
#endif
#ifndef AppVersion
  #error AppVersion must be supplied
#endif

#define AppName "EFML"

[Setup]
AppId={{3B6F1C52-8E2D-4C7A-9A61-5D0E7F4B2C19}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=EFML
AppPublisherURL=https://github.com/guchang233/EFML
AppSupportURL=https://github.com/guchang233/EFML/issues
AppUpdatesURL=https://github.com/guchang233/EFML/releases
DefaultDirName={localappdata}\Programs\EFML
DefaultGroupName=EFML
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=EFML-{#AppVersion}-Setup
SetupIconFile=..\manager\EFML.Manager\Assets\efml.ico
UninstallDisplayIcon={app}\EFML.exe
InfoBeforeFile=DISCLAIMER.zh-CN.txt
LicenseFile=..\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=EFML Installer
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}.0

[Languages]
Name: "chinesesimp"; MessagesFile: "Languages\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimp.LaunchProgram=启动 EFML
en.LaunchProgram=Launch EFML

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\EFML"; Filename: "{app}\EFML.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\EFML"; Filename: "{app}\EFML.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\EFML.exe"; Description: "{cm:LaunchProgram}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Removes the xinput1_4.dll proxy EFML put in the game folder. Modules and logs in
; %LOCALAPPDATA%\EFML are kept.
Filename: "{app}\EFML.exe"; Parameters: "--uninstall-game-files-silent"; WorkingDir: "{app}"; RunOnceId: "RemoveGameDirectoryFiles"; Flags: runhidden waituntilterminated skipifdoesntexist
