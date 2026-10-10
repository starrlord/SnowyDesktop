; Inno Setup 6 script for the Snowy Desktop installer.
;   iscc /DAppVersion=1.2.3 /DSourceExe=..\build\Release\snowystarr.exe installer\snowystarr.iss
; Output: dist\SnowyDesktop-<version>-setup.exe

#define AppName "Snowy Desktop"
; The original name, still used by the exe, its settings key and its Run value,
; so existing installs upgrade in place and keep their settings.
#define AppKey "snowystarr"
#define AppExe "snowystarr.exe"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\Release\snowystarr.exe"
#endif

[Setup]
AppId={{CDD61CC6-A3E7-4FA2-A170-A9E589F7ECF0}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Joshua Starr
AppPublisherURL=https://github.com/starrlord/snowystarr
AppSupportURL=https://github.com/starrlord/snowystarr/issues
AppUpdatesURL=https://github.com/starrlord/snowystarr/releases
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} setup
; Per-user install by default (no admin prompt); "all users" stays available.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=SnowyDesktop-{#AppVersion}-setup
SetupIconFile=..\res\snowystarr.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start {#AppName} when I sign in to Windows"; GroupDescription: "Options:"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Snow on your desktop"

[Registry]
; Same value the app's own "Start with Windows" switch reads and writes.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppKey}"; ValueData: """{app}\{#AppExe}"""; Tasks: startup

[Run]
Filename: "{app}\{#AppExe}"; Description: "Let it snow now"; Flags: nowait postinstall skipifsilent

[Code]
const
  AppWindowClass = 'snowystarrApp';
  WM_CLOSE = $0010;

{ Ask a running copy to exit (it saves its settings on the way out). }
procedure CloseRunningApp();
var
  Wnd: HWND;
  Tries: Integer;
begin
  Wnd := FindWindowByClassName(AppWindowClass);
  if Wnd = 0 then
    Exit;
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  Tries := 0;
  while (FindWindowByClassName(AppWindowClass) <> 0) and (Tries < 50) do
  begin
    Sleep(100);
    Tries := Tries + 1;
  end;
  Sleep(300);  { let the process finish exiting before files are replaced }
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningApp();
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  { The Start menu entry from before the rename. Removed only once the install
    has succeeded, so an aborted upgrade still leaves a way to start the app. }
  if CurStep = ssPostInstall then
    DeleteFile(ExpandConstant('{autoprograms}\{#AppKey}.lnk'));
end;

function InitializeUninstall(): Boolean;
begin
  CloseRunningApp();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    { Also covers "Start with Windows" enabled from inside the app. }
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppKey}');
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run', '{#AppKey}');
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\{#AppKey}');
  end;
end;
