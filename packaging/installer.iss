; Jam Sesh's Windows installer (Inno Setup 6): the app, the Clod Scope VST3 plugin, shortcuts, an optional
; firewall rule for hosting, and an uninstaller in Windows' Apps list. One admin prompt. Running a newer
; installer updates in place (the AppId never changes); settings, presets and recordings are not touched,
; also not by the uninstaller. packaging\make-dist.ps1 compiles it into dist\JamSesh-<version>-Setup.exe:
;   ISCC.exe /DAppVersion=1.2.1 "/DStage=<dist\Jam Sesh 1.2.1>" "/DOutDir=<dist>" packaging\installer.iss
; /CURRENTUSER on the command line installs for this user only, without admin (the plugin then goes to
; %LOCALAPPDATA%\Programs\Common\VST3 and there is no firewall rule). For test builds, /DAppName and
; /DAppGuid give a second, separate product.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef Stage
  #error Pass /DStage=<the staged "Jam Sesh <version>" folder from make-dist.ps1>
#endif
#ifndef OutDir
  #define OutDir "."
#endif
#ifndef AppName
  #define AppName "Jam Sesh"
#endif
#ifndef AppGuid
  #define AppGuid "B644C666-36C3-4172-A82E-8FD87FB5892A"
#endif
#define AppExe "Jam Sesh.exe"
#define Repo "https://github.com/Laumerz84/JamSesh"

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Clod Codes
AppPublisherURL={#Repo}
AppSupportURL={#Repo}
AppUpdatesURL={#Repo}/releases
AppCopyright=Copyright (C) 2026 Clod Codes. Free software under the GNU GPL version 3.
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=JamSesh-{#AppVersion}-Setup
SetupIconFile=..\Source\Resources\JamSesh.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Jam Sesh or a DAW holding the plugin open: Windows' Restart Manager asks to close them
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "plugin"; Description: "Install the Clod Scope plugin for FL Studio and other DAWs (VST3)"
Name: "desktopicon"; Description: "Put a Jam Sesh shortcut on the desktop"
Name: "firewall"; Description: "Let Jam Sesh through the Windows firewall, so friends can join when you host"; Check: IsAdminInstallMode

[InstallDelete]
; an update brings its own source code: the previous version's goes
Type: files; Name: "{app}\Source\*-Source.zip"

[Files]
Source: "{#Stage}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\Licenses\*"; DestDir: "{app}\Licenses"; Flags: ignoreversion
Source: "{#Stage}\*-Source.zip"; DestDir: "{app}\Source"; Flags: ignoreversion
Source: "{#Stage}\Clod Scope.vst3\*"; DestDir: "{autocf}\VST3\Clod Scope.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: plugin

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Play together online with friends"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Play together online with friends"; Tasks: desktopicon

[Run]
; the firewall rule: inbound UDP for the app only (an update replaces it)
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#AppName}"" program=""{app}\{#AppExe}"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#AppName}"" dir=in action=allow program=""{app}\{#AppExe}"" protocol=UDP profile=private,public enable=yes"; Flags: runhidden; Tasks: firewall
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#AppName}"" program=""{app}\{#AppExe}"""; Flags: runhidden; RunOnceId: "FirewallRule"; Check: IsAdminInstallMode
