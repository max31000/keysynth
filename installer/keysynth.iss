; keysynth Windows installer (Inno Setup 6). Built by scripts/build_installer.ps1 from a staged tree:
;   ISCC /DAppVersion=X.Y.Z /DStageDir=<stage> /DOutputDir=<out> installer\keysynth.iss
; Per-user install, no admin: %LOCALAPPDATA%\Programs\keysynth. User data (%LOCALAPPDATA%\keysynth, AppPaths
; installed layout, docs/specs/2026-10-12-cloud-api.md §6.1) is kept on uninstall unless the user asks.
; Design: docs/specs/2026-10-12-cloud.md §7/§11, ARCHITECTURE §16, docs/TESTING.md "Release / installer".

#ifndef AppVersion
  #error Pass /DAppVersion=X.Y.Z
#endif
#ifndef StageDir
  #error Pass /DStageDir=<staged files>
#endif
#ifndef OutputDir
  #define OutputDir "..\out"
#endif

#define AppName "keysynth"
#define AppExe "keysynth-engine.exe"
; = ks::platform::kInstanceName (engine/src/platform/DesktopIntegration.h)
#define InstanceName "keysynth-engine"

[Setup]
AppId={{6E1D7C2A-4B8F-4F0E-9C3D-6A5B2E8F1D47}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppPublisherURL=https://mvv42.ru/keysynth/
AppSupportURL=https://github.com/max31000/keysynth
AppUpdatesURL=https://github.com/max31000/keysynth/releases/latest
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} setup
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DisableProgramGroupPage=yes
; Fixed dir: [InstallDelete] wipes {app}\ui|presets|share, so never let it point at a foreign folder.
DisableDirPage=yes
DisableReadyPage=yes
UsePreviousAppDir=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=keysynth-setup-{#AppVersion}
SetupIconFile={#StageDir}\keysynth.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
; The engine holds this mutex while running (desktop mode). [Code] asks it to quit first (named event).
AppMutex={#InstanceName}
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
ChangesAssociations=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
en.TaskDesktop=Create a &desktop shortcut
ru.TaskDesktop=Создать значок на &рабочем столе
en.TaskAutostart=Start with &Windows (tray)
ru.TaskAutostart=Автозапуск с &Windows (в трее)
en.TaskGroup=Additional shortcuts:
ru.TaskGroup=Дополнительно:
en.LaunchNow=Launch keysynth
ru.LaunchNow=Запустить keysynth
en.DeleteData=Delete keysynth data (presets, settings, samples, logs) in%n%1?
ru.DeleteData=Удалить данные keysynth (пресеты, настройки, сэмплы, журналы) в%n%1?
en.ProtocolName=URL:keysynth
ru.ProtocolName=URL:keysynth

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked
Name: "autostart"; Description: "{cm:TaskAutostart}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked

[Files]
; App dir (read-only content: exe, faust.dll + share\faust, VC++ runtime app-local, presets, ui\dist, licences).
Source: "{#StageDir}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Example Faust plugins into the user plugins dir (hot reload watches it). Never overwrite the user's edits.
Source: "{#StageDir}\plugins\*"; DestDir: "{localappdata}\keysynth\plugins"; Flags: onlyifdoesntexist uninsneveruninstall recursesubdirs createallsubdirs

[InstallDelete]
; Old read-only content that a new version may have dropped (presets, UI assets with hashed names).
Type: filesandordirs; Name: "{app}\ui"
Type: filesandordirs; Name: "{app}\presets"
Type: filesandordirs; Name: "{app}\share"

[Icons]
; Directly in the Start menu (no folder): one "keysynth" entry, pinnable to Start / taskbar.
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExe}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; keysynth:// protocol (web page "start engine" button), per user.
Root: HKCU; Subkey: "Software\Classes\keysynth"; ValueType: string; ValueName: ""; ValueData: "{cm:ProtocolName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\keysynth"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\keysynth\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"
Root: HKCU; Subkey: "Software\Classes\keysynth\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
; Autostart = the same value the tray's "Autostart" toggle writes (ks::platform::setAutostart).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "keysynth"; ValueData: """{app}\{#AppExe}"" --tray"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchNow}"; Flags: postinstall nowait skipifsilent

[Code]
const
  EVENT_MODIFY_STATE = $0002;
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

function OpenEvent(dwDesiredAccess: DWORD; bInheritHandle: BOOL; lpName: string): THandle;
  external 'OpenEventW@kernel32.dll stdcall';
function SetEvent(hEvent: THandle): BOOL;
  external 'SetEvent@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

{ Ask a running engine to quit cleanly (its "<mutex>-quit" event, ks::platform::SingleInstance) and wait up to 10 s,
  so AppMutex finds nothing and audio devices are released properly. }
procedure StopRunningEngine();
var
  H: THandle;
  I: Integer;
begin
  if not CheckForMutexes('{#InstanceName}') then Exit;
  H := OpenEvent(EVENT_MODIFY_STATE, False, '{#InstanceName}-quit');
  if H = 0 then Exit;
  SetEvent(H);
  CloseHandle(H);
  for I := 1 to 100 do
  begin
    if not CheckForMutexes('{#InstanceName}') then Exit;
    Sleep(100);
  end;
end;

var
  WasRunning: Boolean;

{ Interactive: Inno's AppMutex check asks the user to quit keysynth (tray > Quit); opening or cancelling the wizard
  never kills it. Silent (scripted upgrade): stop it here and start it again (--tray) after the install. }
function InitializeSetup(): Boolean;
begin
  WasRunning := CheckForMutexes('{#InstanceName}');
  if WizardSilent then StopRunningEngine();
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningEngine(); { started again while the wizard was open }
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep = ssPostInstall) and WizardSilent and WasRunning then
    ExecAsOriginalUser(ExpandConstant('{app}\{#AppExe}'), '--tray', ExpandConstant('{app}'), SW_SHOWNORMAL, ewNoWait, Code);
end;

function InitializeUninstall(): Boolean;
begin
  if UninstallSilent then StopRunningEngine();
  Result := True;
end;

function HasParam(const Name: string): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then Result := True;
end;

{ The engine drops its mutex a moment before the process exits and closes its log file, and AV scanners may hold
  fresh files briefly: retry for up to 10 s. }
procedure DeleteDataDir(const Dir: string);
var
  I: Integer;
begin
  StopRunningEngine();
  for I := 1 to 20 do
  begin
    DelTree(Dir, True, True, True);
    if not DirExists(Dir) then
    begin
      Log('deleted user data ' + Dir);
      Exit;
    end;
    Sleep(500);
  end;
  Log('could not delete all of ' + Dir);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Cmd, DataDir: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    StopRunningEngine(); { after the confirmation }
    { The tray toggle may have added the Run value without the installer task: remove it if it points here. }
    if RegQueryStringValue(HKCU, RunKey, 'keysynth', Cmd) and
       (Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Cmd)) > 0) then
      RegDeleteValue(HKCU, RunKey, 'keysynth');
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\keysynth');
    if not DirExists(DataDir) then Exit;
    { Silent uninstall keeps data unless /DELETEDATA is passed. }
    if UninstallSilent then
    begin
      if HasParam('/DELETEDATA') then DeleteDataDir(DataDir);
    end
    else if MsgBox(FmtMessage(CustomMessage('DeleteData'), [DataDir]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DeleteDataDir(DataDir);
  end;
end;
