; Windows installer for the native application (Inno Setup 6). Built by packaging/build_windows.sh
; from the folder windeployqt fills: iscc -DVersion=x.y.z packaging\resindeck.iss
#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppName=ResinDeck
AppVersion={#Version}
AppPublisher=ResinDeck contributors
AppPublisherURL=https://github.com/alexandre-pansan/ResinDeck
DefaultDirName={autopf}\ResinDeck
DefaultGroupName=ResinDeck
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ResinDeck-{#Version}-windows-x64-setup
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\resindeck.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "..\build\windows\ResinDeck\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\ResinDeck"; Filename: "{app}\resindeck.exe"
Name: "{autodesktop}\ResinDeck"; Filename: "{app}\resindeck.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\resindeck.exe"; Description: "{cm:LaunchProgram,ResinDeck}"; Flags: nowait postinstall skipifsilent
