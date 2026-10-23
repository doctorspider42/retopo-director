; The Windows installer, for Inno Setup 7.
;
;   iscc /DAppVersion=0.1.0 /DBuildNumber=41 /DStageDir=..\dist\stage packaging\installer.iss
;
; StageDir holds what the zip holds: retopo-director.exe, profiles\, tools\.
; CI builds this on every push to main and attaches it to the release; the
; application's update check downloads it from there and runs it with
; /SILENT /RELAUNCH=1.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildNumber
  #define BuildNumber "0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName    "PS2 Retopo Director"
#define AppExe     "retopo-director.exe"
#define AppUrl     "https://github.com/doctorspider42/retopo-director"

[Setup]
; Never change the AppId: it is how an update finds the copy it replaces.
AppId={{6F3B2C1E-8D47-4A5B-9C21-7E0D5A4B3F92}
AppName={#AppName}
AppVersion={#AppVersion}.{#BuildNumber}
AppVerName={#AppName} {#AppVersion} (build {#BuildNumber})
AppPublisher=doctorspider42
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}.{#BuildNumber}
; Per user, into %LOCALAPPDATA%\Programs: no administrator prompt, so an
; update can install silently, and the work folder next to the executable
; stays writable.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\LICENSE
SetupIconFile=app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=PS2-Retopo-Director-{#AppVersion}-build.{#BuildNumber}-setup
; The running copy has to go before its files can be replaced.
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\profiles\*"; DestDir: "{app}\profiles"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; The finish page's checkbox, for a person running the installer.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; An update started from inside the application opens the new build itself.
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: ShouldRelaunch

[Code]
function ShouldRelaunch: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;
