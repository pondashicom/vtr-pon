param(
    [string]$CameraDll = (Join-Path $PSScriptRoot '../build/vtr-pon-virtual-camera.dll'),
    [string]$ResultPath,
    [switch]$TestNativeRestartScheduling
)
$ErrorActionPreference = 'Stop'
$registrationScript = Join-Path $PSScriptRoot '../apps/desktop/virtual_camera/install.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($registrationScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
# Import the production helpers without running registration or touching HKLM.
$helpers = $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)
. ([scriptblock]::Create(($helpers | ForEach-Object { $_.Extent.Text }) -join "`n"))
Initialize-CameraFileOperations
$nativeScheduler = (Get-Command Schedule-CameraDllRemoval).ScriptBlock
if ($TestNativeRestartScheduling -and -not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Native restart-scheduling test requires an elevated PowerShell.'
}
Add-Type -IgnoreWarnings -WarningAction SilentlyContinue -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VtrPonCameraUninstallTest {
    [DllImport("kernel32.dll", EntryPoint="LoadLibraryExW", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
$script:scheduled = @()
$script:failScheduling = $false
function Schedule-CameraDllRemoval([string]$Path) {
    # Scheduling is the only mock: tests never write PendingFileRenameOperations.
    if ($script:failScheduling) { throw 'Injected restart-scheduling failure' }
    $script:scheduled += $Path
}
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$testRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('vtr-pon-camera-uninstall-' + [Guid]::NewGuid().ToString('N'))))
$null = New-Item -ItemType Directory -Path $testRoot
$results = @()
$module = [IntPtr]::Zero
$held = $null
try {
    $path = Join-Path $testRoot 'camera.dll'
    Copy-Item -LiteralPath $CameraDll -Destination $path
    Assert (-not (Remove-CameraDll $path)) 'Idle removal requested a restart'
    Assert (-not (Test-Path -LiteralPath $path)) 'Idle DLL was not removed'
    Assert ($script:scheduled.Count -eq 0) 'Idle deletion was scheduled'
    $results += 'Idle DLL is removed immediately without restart'

    Copy-Item -LiteralPath $CameraDll -Destination $path
    $module = [VtrPonCameraUninstallTest]::LoadLibraryEx($path, [IntPtr]::Zero, 1)
    Assert ($module -ne [IntPtr]::Zero) 'Could not map the test DLL as an image'
    Assert (Remove-CameraDll $path) 'Mapped DLL did not request a restart'
    Assert (-not (Test-Path -LiteralPath $path)) 'Mapped DLL kept its original name'
    Assert ($script:scheduled.Count -eq 1) 'Expected one retired DLL to be scheduled'
    $retired = $script:scheduled[0]
    Assert ((Split-Path $retired) -eq $testRoot) 'Retired DLL escaped the test folder'
    Assert ($retired -ne $path -and (Test-Path -LiteralPath $retired)) 'Wrong cleanup path'
    # Reinstall before restart. Delayed cleanup must target only the old image.
    Copy-Item -LiteralPath $CameraDll -Destination $path
    $replacementHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    [VtrPonCameraUninstallTest]::FreeLibrary($module) | Out-Null
    $module = [IntPtr]::Zero
    Remove-Item -LiteralPath $retired
    Assert ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $replacementHash) 'Cleanup removed the reinstalled DLL'
    Remove-Item -LiteralPath $path
    $results += 'A mapped DLL is retired; reinstall before restart survives old-DLL cleanup'

    Copy-Item -LiteralPath $CameraDll -Destination $path
    $module = [VtrPonCameraUninstallTest]::LoadLibraryEx($path, [IntPtr]::Zero, 1)
    Assert ($module -ne [IntPtr]::Zero) 'Could not map the rollback test DLL'
    $script:failScheduling = $true
    $failed = $false
    try { Remove-CameraDll $path | Out-Null } catch {
        $failed = $_.ToString().Contains('Injected restart-scheduling failure')
    }
    $script:failScheduling = $false
    Assert $failed 'Scheduling failure was not reported'
    Assert (Test-Path -LiteralPath $path) 'Scheduling failure did not restore the original name'
    Assert (@(Get-ChildItem -LiteralPath $testRoot -File).Count -eq 1) 'Rollback left a retired DLL'
    [VtrPonCameraUninstallTest]::FreeLibrary($module) | Out-Null
    $module = [IntPtr]::Zero
    Remove-Item -LiteralPath $path
    $results += 'Scheduling failure restores the original DLL name and remains an error'

    $frame = Join-Path $testRoot 'frames-v2.bin'
    [IO.File]::WriteAllBytes($frame, [byte[]](1, 2, 3, 4))
    $held = [IO.File]::Open($frame, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $failed = $false
    try { Wait-CameraFilesIdle @($frame) -Attempts 1 } catch {
        $failed = $_.ToString().Contains('Camera file is still in use:')
    }
    Assert $failed 'An active frame-file reader was not rejected'
    $held.Dispose()
    $held = $null
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($frame)) -eq 'AQIDBA==') 'Busy frame data was modified'
    Remove-Item -LiteralPath $frame
    $results += 'Active frame-file use is rejected without changing frame data'

    Copy-Item -LiteralPath $CameraDll -Destination $path
    $held = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $scheduledBefore = $script:scheduled.Count
    $failed = $false
    try { Remove-CameraDll $path | Out-Null } catch { $failed = $true }
    Assert $failed 'A file that cannot be renamed was not rejected'
    Assert ((Test-Path -LiteralPath $path) -and $script:scheduled.Count -eq $scheduledBefore) 'Unrenameable DLL was changed or scheduled'
    $held.Dispose()
    $held = $null
    Remove-Item -LiteralPath $path
    $results += 'A separate file handle that prevents retirement remains an error'

    Assert (-not (Remove-CameraDll $path)) 'An already removed DLL requested restart'
    $results += 'Repeated cleanup of an absent DLL succeeds without restart'

    if ($TestNativeRestartScheduling) {
        # Exercise the real API and inspect its actual source/empty-target pair.
        # Never edit existing queue entries. The unique fixture entry is harmless
        # after this test removes its own files and is consumed on next restart.
        $session = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager')
        try {
            $before = @($session.GetValue('PendingFileRenameOperations', @(), [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames))
            Set-Item -LiteralPath Function:Schedule-CameraDllRemoval -Value $nativeScheduler
            Copy-Item -LiteralPath $CameraDll -Destination $path
            $module = [VtrPonCameraUninstallTest]::LoadLibraryEx($path, [IntPtr]::Zero, 1)
            Assert ($module -ne [IntPtr]::Zero) 'Could not map native-scheduler fixture'
            Assert (Remove-CameraDll $path) 'Native scheduling did not request restart'
            $retiredFiles = @(Get-ChildItem -LiteralPath $testRoot -File -Filter '*.remove-*')
            Assert ($retiredFiles.Count -eq 1 -and -not (Test-Path -LiteralPath $path)) 'Native scheduling did not retire the DLL'
            $retired = $retiredFiles[0].FullName
            $pending = @($session.GetValue('PendingFileRenameOperations', @(), [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames))
            $index = [Array]::IndexOf($pending, ('\??\' + $retired))
            # Current Windows 11 may prefix a native delete source with *1.
            if ($index -lt 0) { $index = [Array]::IndexOf($pending, ('*1\??\' + $retired)) }
            Assert ($index -ge 0 -and $index % 2 -eq 0 -and $index + 1 -lt $pending.Count -and $pending[$index + 1] -ceq '') 'No native source/empty-target delete pair was recorded'
            $others = @()
            for ($i=0; $i -lt $pending.Count; $i++) {
                if ($i -eq $index) { $i++; continue }
                $others += $pending[$i]
            }
            Assert (($others -join "`0") -ceq ($before -join "`0")) 'Existing pending operations changed'
            Copy-Item -LiteralPath $CameraDll -Destination $path
            $newHash = (Get-FileHash -LiteralPath $path).Hash
            [VtrPonCameraUninstallTest]::FreeLibrary($module) | Out-Null
            $module = [IntPtr]::Zero
            Remove-Item -LiteralPath $retired
            Assert ((Get-FileHash -LiteralPath $path).Hash -eq $newHash) 'Native pending deletion targets the new DLL'
            Remove-Item -LiteralPath $path
            $results += 'Real Windows restart-deletion API records the old DLL only; existing operations and reinstall are preserved'
        } finally { $session.Dispose() }
    }
} finally {
    if ($held) { $held.Dispose() }
    if ($module -ne [IntPtr]::Zero) { [VtrPonCameraUninstallTest]::FreeLibrary($module) | Out-Null }
    # The test folder contains only direct test files; never recursively delete.
    foreach ($file in Get-ChildItem -LiteralPath $testRoot -File) { Remove-Item -LiteralPath $file.FullName }
    Remove-Item -LiteralPath $testRoot
}
$report = [ordered]@{passed=$results.Count;tests=$results;nativeImageMapping=$true;restartSchedulingMocked=$true;nativeRestartSchedulingTested=[bool]$TestNativeRestartScheduling;registrationAndServicesChanged=$false}
if ($ResultPath) { $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResultPath -Encoding UTF8 }
$results | ForEach-Object { Write-Output "PASS: $_" }
