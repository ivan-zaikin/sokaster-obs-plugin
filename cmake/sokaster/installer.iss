; Windows installer for sokaster (Inno Setup 6).
;
; Built by .github/scripts/Package-Windows.ps1, which passes:
;   PluginVersion  — version from buildspec.json, e.g. 0.1.1 or 0.2.0-beta1
;   NumericVersion — the same without the pre-release suffix, for VERSIONINFO
;   SourceDir      — the installed plugin folder, release/<config>/sokaster
;   OutputDir, OutputBaseName
;
; Puts the plugin where OBS looks for it on every supported version — see
; cmake/sokaster/plugin-layout.cmake for the two layouts inside that folder.

#define PluginName "sokaster"
#define MinObsVersion "31.1.1"
#define MinObsMajor 31
#define MinObsMinor 1
#define MinObsPatch 1

[Setup]
AppId={{6B0E3C1A-5F2D-4C87-9A41-3D7E52B9C0F4}
AppName={#PluginName}
AppVersion={#PluginVersion}
AppVerName={#PluginName} {#PluginVersion}
AppPublisher={#PluginName}
AppPublisherURL=https://sokaster.ru
AppSupportURL=https://github.com/ivan-zaikin/sokaster-obs-plugin/issues
AppUpdatesURL=https://github.com/ivan-zaikin/sokaster-obs-plugin/releases
VersionInfoVersion={#NumericVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginName}
DisableDirPage=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; OBS holding the DLL is handled by the running-OBS check below, never by
; closing OBS behind the streamer's back.
CloseApplications=no
UninstallFilesDir={app}\uninstall
UninstallDisplayName={#PluginName} for OBS Studio
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Messages]
english.FinishedLabel=sokaster is installed. Start OBS Studio, open Docks → sokaster and sign in.
russian.FinishedLabel=sokaster установлен. Запустите OBS Studio, откройте «Док-панели» → sokaster и войдите.

[CustomMessages]
english.ObsRunning=OBS Studio is running. Close it and press Retry.
russian.ObsRunning=OBS Studio запущен. Закройте его и нажмите «Повтор».
english.ObsNotFound=OBS Studio was not found on this computer. The plugin will be installed anyway and will load once OBS Studio {#MinObsVersion} or newer is installed.%n%nContinue?
russian.ObsNotFound=OBS Studio на этом компьютере не найден. Плагин всё равно будет установлен и заработает, когда появится OBS Studio {#MinObsVersion} или новее.%n%nПродолжить?
english.ObsTooOld=sokaster needs OBS Studio {#MinObsVersion} or newer, this computer has %1. Update OBS Studio and run the installer again.
russian.ObsTooOld=sokaster нужен OBS Studio {#MinObsVersion} или новее, а установлен %1. Обновите OBS Studio и запустите установщик снова.

[InstallDelete]
; Whatever an earlier version or a hand-unzipped archive left behind, so no
; stale DLL or locale outlives the update.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"
Type: files; Name: "{app}\*.dll"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
function IsObsRunning: Boolean;
var
  Locator, Service, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Processes.Count > 0;
  except
    { No WMI — let the file copy itself report a locked DLL. }
  end;
end;

function WaitForObsToClose: Boolean;
begin
  Result := True;
  while IsObsRunning do
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

{ obs64.exe of the regular installer; empty for a portable copy or no OBS. }
function FindObsExecutable: String;
var
  ObsDir: String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', ObsDir) then
    if not RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', ObsDir) then
      Exit;
  if FileExists(AddBackslash(ObsDir) + 'bin\64bit\obs64.exe') then
    Result := AddBackslash(ObsDir) + 'bin\64bit\obs64.exe';
end;

function CheckObsVersion: Boolean;
var
  ObsExe: String;
  Installed, Required: Int64;
  InstalledText: String;
begin
  Result := True;
  ObsExe := FindObsExecutable;
  if ObsExe = '' then
  begin
    Result := SuppressibleMsgBox(CustomMessage('ObsNotFound'), mbConfirmation, MB_YESNO, IDYES) = IDYES;
    Exit;
  end;

  if not GetPackedVersion(ObsExe, Installed) then
    Exit;
  Required := PackVersionComponents({#MinObsMajor}, {#MinObsMinor}, {#MinObsPatch}, 0);
  if ComparePackedVersion(Installed, Required) < 0 then
  begin
    GetVersionNumbersString(ObsExe, InstalledText);
    SuppressibleMsgBox(FmtMessage(CustomMessage('ObsTooOld'), [InstalledText]), mbCriticalError, MB_OK, IDOK);
    Result := False;
  end;
end;

function InitializeSetup: Boolean;
begin
  Result := CheckObsVersion and WaitForObsToClose;
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitForObsToClose;
end;
