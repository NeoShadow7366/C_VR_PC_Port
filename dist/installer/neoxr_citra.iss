; SPDX-FileCopyrightText: 2026 CitraVR / NeoXR Citra authors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; NeoXR Citra + NeoXR Citra VR  —  Inno Setup script.
;
; Bundles:
;   - citra-qt.exe + Qt DLLs (flat 3DS emulator)        [from build\bin\Release\]
;   - citra_vr.exe + citra_vr_launcher.exe (VR build)   [from build-vr\bin\Release\]
;   - dist\steam_art\out\{qt,vr}\*                      (Steam grid art)
;   - dist\steam_integration\*.ps1 + .psm1              (shortcut writer)
;
; Per-user install to %LOCALAPPDATA%\Programs\NeoXRCitra\ — no admin needed.
;
; Build command:
;   ISCC.exe /DBUILD_QT="<path>\build\bin\Release"
;            /DBUILD_VR="<path>\build-vr\bin\Release"
;            /DREPO_ROOT="<path>"
;            "dist\installer\neoxr_citra.iss"
;
; Defines you may override from the command line with /D:
;   APP_VERSION      Defaults to "0.1.0"
;   BUILD_QT         Required.  Directory holding citra-qt.exe + DLLs.
;   BUILD_VR         Required.  Directory holding citra_vr.exe.
;   REPO_ROOT        Required.  Repo root for dist\ assets.

#ifndef APP_VERSION
  #define APP_VERSION "0.1.0"
#endif
#ifndef REPO_ROOT
  #error "REPO_ROOT must be defined (ISCC /DREPO_ROOT=<path>)"
#endif
#ifndef BUILD_QT
  #error "BUILD_QT must be defined (ISCC /DBUILD_QT=<dir with citra-qt.exe>)"
#endif
#ifndef BUILD_VR
  #error "BUILD_VR must be defined (ISCC /DBUILD_VR=<dir with citra_vr.exe>)"
#endif

#define MyAppPublisher "NeoXR Citra Project"
#define MyAppQtName    "NeoXR Citra"
#define MyAppVrName    "NeoXR Citra VR"
#define MyAppURL       "https://github.com/NeoShadow7366/C_VR_PC_Port"
#define ArtRoot        REPO_ROOT + "\dist\steam_art\out"
#define IntegRoot      REPO_ROOT + "\dist\steam_integration"
#define MyAppId        "{{B7E4F2A1-9CD3-4F8E-B3A7-7F8B2C5D9E10}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppQtName}
AppVersion={#APP_VERSION}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\NeoXRCitra
DefaultGroupName={#MyAppQtName}
DisableProgramGroupPage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#REPO_ROOT}\dist\installer\out
OutputBaseFilename=NeoXRCitra-Setup-{#APP_VERSION}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
SetupIconFile={#ArtRoot}\vr\icon.ico
UninstallDisplayIcon={app}\citra-qt.exe
ShowLanguageDialog=auto

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon_qt"; Description: "Create Desktop shortcut for {#MyAppQtName}";    GroupDescription: "Additional shortcuts:"
Name: "desktopicon_vr"; Description: "Create Desktop shortcut for {#MyAppVrName}";    GroupDescription: "Additional shortcuts:"
Name: "steam_qt";       Description: "Add '{#MyAppQtName}' to Steam library";         GroupDescription: "Steam library integration:"; Check: SteamDetected
Name: "steam_vr";       Description: "Add '{#MyAppVrName}' to Steam library (auto-launches SteamVR)"; GroupDescription: "Steam library integration:"; Check: SteamDetected

[Files]
; --- VR build payload ---
Source: "{#BUILD_VR}\citra_vr.exe";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#BUILD_VR}\citra_vr_launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BUILD_VR}\*.dll";                 DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist recursesubdirs

; --- Qt build payload ---
Source: "{#BUILD_QT}\citra-qt.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BUILD_QT}\*.dll";        DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist recursesubdirs
Source: "{#BUILD_QT}\platforms\*";  DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#BUILD_QT}\styles\*";     DestDir: "{app}\styles";    Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#BUILD_QT}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#BUILD_QT}\iconengines\*";  DestDir: "{app}\iconengines";  Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

; --- Steam integration scripts (used post-install + on uninstall) ---
Source: "{#IntegRoot}\*.ps1"; DestDir: "{app}\steam_integration"; Flags: ignoreversion
Source: "{#IntegRoot}\*.psm1"; DestDir: "{app}\steam_integration"; Flags: ignoreversion

; --- Steam grid art (shipped so uninstaller knows what to clean; also re-deployable) ---
Source: "{#ArtRoot}\qt\*"; DestDir: "{app}\steam_art\qt"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#ArtRoot}\vr\*"; DestDir: "{app}\steam_art\vr"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Repo-root legal ---
Source: "{#REPO_ROOT}\license.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#REPO_ROOT}\NOTICE";      DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppQtName}";     Filename: "{app}\citra-qt.exe";             WorkingDir: "{app}"; IconFilename: "{app}\steam_art\qt\icon.ico"
Name: "{group}\{#MyAppVrName}";     Filename: "{app}\citra_vr_launcher.exe";    WorkingDir: "{app}"; IconFilename: "{app}\steam_art\vr\icon.ico"
Name: "{group}\Uninstall {#MyAppQtName}"; Filename: "{uninstallexe}"

Name: "{userdesktop}\{#MyAppQtName}"; Filename: "{app}\citra-qt.exe";           WorkingDir: "{app}"; IconFilename: "{app}\steam_art\qt\icon.ico"; Tasks: desktopicon_qt
Name: "{userdesktop}\{#MyAppVrName}"; Filename: "{app}\citra_vr_launcher.exe";  WorkingDir: "{app}"; IconFilename: "{app}\steam_art\vr\icon.ico"; Tasks: desktopicon_vr

[Run]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\steam_integration\Add-SteamShortcuts.ps1"" -InstallDir ""{app}"" -ArtRoot ""{app}\steam_art"" -QtOnly"; \
  Flags: runhidden waituntilterminated; \
  Tasks: steam_qt; \
  StatusMsg: "Adding {#MyAppQtName} to Steam library..."

Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\steam_integration\Add-SteamShortcuts.ps1"" -InstallDir ""{app}"" -ArtRoot ""{app}\steam_art"" -VrOnly"; \
  Flags: runhidden waituntilterminated; \
  Tasks: steam_vr; \
  StatusMsg: "Adding {#MyAppVrName} to Steam library..."

Filename: "{app}\citra-qt.exe"; \
  Description: "Launch {#MyAppQtName} now"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
; Always remove shortcuts on uninstall (script no-ops if Steam absent / entry missing).
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\steam_integration\Remove-SteamShortcuts.ps1"""; \
  Flags: runhidden waituntilterminated; \
  RunOnceId: "RemoveSteamShortcuts"

[Code]
function SteamDetected(): Boolean;
var Path: String;
begin
  Result := False;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Path) and (Path <> '') then begin
    Result := True; exit;
  end;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', Path) and (Path <> '') then begin
    Result := True; exit;
  end;
  if RegQueryStringValue(HKLM, 'SOFTWARE\Valve\Steam', 'InstallPath', Path) and (Path <> '') then begin
    Result := True; exit;
  end;
end;
