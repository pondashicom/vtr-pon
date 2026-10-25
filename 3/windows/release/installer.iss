; Included from the generated build/release/installer-entry.iss.
; VtrPonPayloadDir, VtrPonOutputDir, agreement files and version.iss are defined there.

[Setup]
AppId={{29208079-2B67-5ACA-B797-FC3650EF3192}
AppName=VTR-PON
AppVersion={#VtrPonVersion}
AppPublisher=Pondashi
AppPublisherURL=https://pondashi.com/vtr-pon/
AppSupportURL=https://pondashi.com/vtr-pon/
DefaultDirName={localappdata}\Programs\VTR-PON3
DefaultGroupName=VTR-PON3
UninstallDisplayName=VTR-PON3
UninstallDisplayIcon={app}\vtr-pon.exe
SetupIconFile={#VtrPonSetupIcon}
LicenseFile={#VtrPonAgreementEn}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
WizardStyle=modern
LanguageDetectionMethod=uilanguage
UsePreviousLanguage=no
ShowLanguageDialog=no
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=no
OutputDir={#VtrPonOutputDir}
OutputBaseFilename={#VtrPonInstallerBaseName}
VersionInfoVersion={#VtrPonWindowsVersion}
VersionInfoProductVersion={#VtrPonWindowsVersion}
VersionInfoTextVersion={#VtrPonVersion}
VersionInfoProductTextVersion={#VtrPonVersion}
Compression=lzma2
SolidCompression=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; LicenseFile: "{#VtrPonAgreementEn}"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"; LicenseFile: "{#VtrPonAgreementJa}"

[Messages]
en.WizardLicense=License and usage notice
ja.WizardLicense=ライセンスと利用状況通知
en.LicenseLabel3=Read the license terms and usage notice. You must agree to both to continue.
ja.LicenseLabel3=ライセンス条件と利用状況通知をお読みください。続行するには両方への同意が必要です。
en.LicenseAccepted=I &agree to the license terms and usage notice
ja.LicenseAccepted=ライセンス条件と利用状況通知に同意する(&A)

[CustomMessages]
en.SilentRequiresConsent=Interactive setup is required to accept the license terms and usage notice.
ja.SilentRequiresConsent=ライセンス条件と利用状況通知への同意が必要なため、画面を表示してインストールしてください。
en.SettingsDirectoryError=Could not create the VTR-PON settings directory.
ja.SettingsDirectoryError=VTR-PONの設定フォルダーを作成できませんでした。
en.SaveActivityNoticeError=Could not save the usage notice choice.
ja.SaveActivityNoticeError=利用状況の通知設定を保存できませんでした。
en.CameraInstallError=Virtual camera registration failed (Windows result %1). Close camera clients and retry setup.
ja.CameraInstallError=仮想カメラの登録に失敗しました（Windowsの終了コード: %1）。カメラを使用しているアプリを閉じて、インストールを再試行してください。
en.CameraUninstallError=Virtual camera removal failed (Windows result %1). Close camera clients and retry uninstallation.
ja.CameraUninstallError=仮想カメラの登録解除に失敗しました（Windowsの終了コード: %1）。カメラを使用しているアプリを閉じて、アンインストールを再試行してください。
en.DesktopShortcut=Add a desktop shortcut
ja.DesktopShortcut=デスクトップにショートカットを追加

; These are installer-owned runtime trees. Clearing them before replacement
; prevents a removed plug-in from surviving an update or downgrade.
[InstallDelete]
Type: filesandordirs; Name: "{app}\gstreamer"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\tls"

[Files]
Source: "{#VtrPonPayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\VTR-PON3\VTR-PON"; Filename: "{app}\vtr-pon.exe"; IconFilename: "{app}\vtr-pon.exe"
Name: "{autoprograms}\VTR-PON3\Licenses and source code"; Filename: "{app}\LICENSE\README.html"

[UninstallDelete]
Type: files; Name: "{userdesktop}\VTR-PON.lnk"

[Code]
var
  CameraRegistrationFailed: Boolean;
  CameraRemovalNeedsRestart: Boolean;
  DesktopShortcutCheckBox: TNewCheckBox;

procedure InitializeWizard;
begin
  DesktopShortcutCheckBox := TNewCheckBox.Create(WizardForm);
  DesktopShortcutCheckBox.Parent := WizardForm.FinishedPage;
  DesktopShortcutCheckBox.Caption := CustomMessage('DesktopShortcut');
  DesktopShortcutCheckBox.Checked := True;
  DesktopShortcutCheckBox.Height := ScaleY(24);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
  begin
    DesktopShortcutCheckBox.Left := WizardForm.FinishedLabel.Left;
    DesktopShortcutCheckBox.Top := WizardForm.FinishedLabel.Top +
      WizardForm.FinishedLabel.Height + ScaleY(16);
    DesktopShortcutCheckBox.Width := WizardForm.FinishedLabel.Width;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpFinished) and DesktopShortcutCheckBox.Checked then
    CreateShellLink(ExpandConstant('{userdesktop}\VTR-PON.lnk'), 'VTR-PON',
      ExpandConstant('{app}\vtr-pon.exe'), '', ExpandConstant('{app}'),
      ExpandConstant('{app}\vtr-pon.exe'), 0, SW_SHOWNORMAL);
end;

function InitializeSetup: Boolean;
begin
  Result := not WizardSilent;
  if not Result then
    MsgBox(CustomMessage('SilentRequiresConsent'), mbError, MB_OK);
end;

procedure SaveActivityNoticeSetting;
var
  SettingsPath: String;
  Value: String;
begin
  SettingsPath := ExpandConstant('{userappdata}\VTR-PON3\user.ini');
  if not ForceDirectories(ExtractFileDir(SettingsPath)) then
    RaiseException(CustomMessage('SettingsDirectoryError'));
  Value := GetIniString('privacy', 'sendActivityNotice', 'true', SettingsPath);
  if not SetIniString('privacy', 'sendActivityNotice', Value, SettingsPath) then
    RaiseException(CustomMessage('SaveActivityNoticeError'));
end;

function CameraCommand(const Action: String): String;
var
  Account: String;
begin
  Account := GetEnv('USERDOMAIN') + '\' + GetEnv('USERNAME');
  Result := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{app}\virtual-camera\install.ps1') + '" -Action ' + Action +
    ' -UserAccount "' + Account + '"';
  if Action = 'Install' then
    Result := Result + ' -SourceDll "' +
      ExpandConstant('{app}\virtual-camera\vtr-pon-virtual-camera.dll') + '"';
end;

procedure RunCameraRegistration(const Action: String);
var
  ResultCode: Integer;
  Started: Boolean;
begin
  ResultCode := -1;
  Log('Virtual camera ' + Action + ' for ' + GetEnv('USERDOMAIN') + '\' + GetEnv('USERNAME'));
  Started := ShellExec('runas', ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      CameraCommand(Action), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('Virtual camera ShellExec started=' + IntToStr(Ord(Started)) +
      ' result=' + IntToStr(ResultCode));
  if Started and (Action = 'Uninstall') and (ResultCode = 3010) then
  begin
    CameraRemovalNeedsRestart := True;
    Log('Virtual camera removal completed; retired DLL cleanup requires restart.');
    Exit;
  end;
  if not Started or (ResultCode <> 0) then
  begin
    CameraRegistrationFailed := True;
    if Action = 'Install' then
      RaiseException(FmtMessage(CustomMessage('CameraInstallError'), [IntToStr(ResultCode)]))
    else
      RaiseException(FmtMessage(CustomMessage('CameraUninstallError'), [IntToStr(ResultCode)]));
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  if CameraRegistrationFailed then Result := 1
  else Result := 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    SaveActivityNoticeSetting;
    RunCameraRegistration('Install');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RunCameraRegistration('Uninstall');
end;

function UninstallNeedRestart: Boolean;
begin
  Result := CameraRemovalNeedsRestart;
end;
