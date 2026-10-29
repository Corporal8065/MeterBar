; MeterBar installer — Inno Setup 6 script (free: https://jrsoftware.org/isdl.php)
; Build via build-release.bat, or manually: ISCC MeterBar.iss
; Produces dist\MeterBar-Setup.exe — the single file Jackson sends to friends.
; The app is built with a STATIC MSVC runtime (/MT), so normally the exe
; needs nothing else. If dist\vc_redist.x64.exe is present at compile time
; (dynamic-link fallback), it is bundled and installed silently instead.

#define MyAppName "MeterBar"
#define MyAppVersion "1.0.0"
#define MyAppExeName "MeterBar.exe"

[Setup]
AppId={{B7E15A42-9C3D-4E58-A1F0-6D2B84C71E09}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=MeterBar-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
; NOTE: the exe is unsigned, so Windows SmartScreen will warn recipients
; on first run ("More info" -> "Run anyway"). That is expected for free
; unsigned software and is explained in README_TEST.md.

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "dist\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
#if FileExists("dist\vc_redist.x64.exe")
Source: "dist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
#if FileExists("dist\vc_redist.x64.exe")
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."; Flags: waituntilterminated
#endif
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
