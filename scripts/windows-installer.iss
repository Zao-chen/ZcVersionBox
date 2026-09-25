#define MyAppName "ZcVersionBox"
#define MyAppExeName "ZcVersionBox.exe"

[Setup]
AppId={{6CA0B4A8-7B63-4D88-9AD0-CDF11A5D4F20}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=ZcVersionBox-{#PackageTag}-setup
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\elawidgettools.dll"
Type: files; Name: "{app}\ZcWidgetTools.dll"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Code]
const
  GitInstallerUrl = 'https://github.com/git-for-windows/git/releases/latest/download/Git-64-bit.exe';
  GitDownloadPage = 'https://git-scm.com/download/win';

function URLDownloadToFile(Caller: Integer; URL: string; FileName: string; Reserved: Integer; StatusCB: Integer): Integer;
  external 'URLDownloadToFileA@urlmon.dll stdcall';

function GitIsInstalled: Boolean;
begin
  Result := FileExists('C:\Program Files\Git\cmd\git.exe') or
            FileExists('C:\Program Files (x86)\Git\cmd\git.exe');
end;

procedure InstallGit;
var
  ResultCode, DownloadCode: Integer;
  InstallerPath: string;
begin
  MsgBox('Git is not installed. ZcVersionBox requires Git for version control.', mbInformation, MB_OK);
  InstallerPath := ExpandConstant('{tmp}\GitInstaller.exe');
  DownloadCode := URLDownloadToFile(0, GitInstallerUrl, InstallerPath, 0, 0);
  if (DownloadCode <> 0) or (not FileExists(InstallerPath)) then
  begin
    MsgBox('Failed to download Git installer automatically. The Git download page will be opened for manual installation.', mbError, MB_OK);
    ShellExec('open', GitDownloadPage, '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
    exit;
  end;
  if ShellExec('open', InstallerPath, '/VERYSILENT /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if ResultCode = 0 then
      MsgBox('Git installed successfully. Please restart the application.', mbInformation, MB_OK)
    else
      MsgBox('Git installation failed with exit code ' + IntToStr(ResultCode) + '. Please install Git manually from https://git-scm.com', mbError, MB_OK);
  end
  else
    MsgBox('Failed to launch Git installer. Please install Git manually from https://git-scm.com', mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    if not GitIsInstalled then InstallGit;
end;

[Run]
Filename: "{app}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ Runtime..."; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
