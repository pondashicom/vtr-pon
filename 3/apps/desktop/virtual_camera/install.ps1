param(
    [ValidateSet('Install','Uninstall')][string]$Action = 'Install',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../../build'),
    [string]$SourceDll,
    [string]$UserAccount,
    [string]$UserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
)
$ErrorActionPreference = 'Stop'

function Initialize-CameraFileOperations {
    if ('VtrPonCameraFileOperations' -as [type]) { return }
    Add-Type -IgnoreWarnings -WarningAction SilentlyContinue -TypeDefinition @'
using System.Runtime.InteropServices;
public static class VtrPonCameraFileOperations {
    [DllImport("kernel32.dll", EntryPoint="DeleteFileW", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern bool DeleteFile(string path);
    [DllImport("kernel32.dll", EntryPoint="MoveFileExW", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern bool MoveFileEx(string source, string destination, uint flags);
}
'@
}

function Schedule-CameraDllRemoval([string]$Path) {
    if (-not [VtrPonCameraFileOperations]::MoveFileEx($Path, $null, 4)) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw [ComponentModel.Win32Exception]::new($code, "Could not schedule camera DLL removal: $Path")
    }
}

function Remove-CameraDll([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    Initialize-CameraFileOperations
    if ([VtrPonCameraFileOperations]::DeleteFile($Path)) { return $false }
    $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($code -in 2, 3) { return $false }
    if ($code -notin 5, 32, 33) {
        throw [ComponentModel.Win32Exception]::new($code, "Could not remove camera DLL: $Path")
    }

    # A mapped image cannot be deleted even after its last camera client closes.
    # Retire its name before scheduling deletion so reinstalling before reboot
    # cannot cause the newly installed DLL to be deleted at that reboot.
    $retiredPath = $Path + '.remove-' + [Guid]::NewGuid().ToString('N')
    if (-not [VtrPonCameraFileOperations]::MoveFileEx($Path, $retiredPath, 0)) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw [ComponentModel.Win32Exception]::new($code, "Could not retire camera DLL: $Path")
    }
    try {
        if ([VtrPonCameraFileOperations]::DeleteFile($retiredPath)) { return $false }
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($code -in 2, 3) { return $false }
        if ($code -notin 5, 32, 33) {
            throw [ComponentModel.Win32Exception]::new($code, "Could not remove retired camera DLL: $retiredPath")
        }
        Schedule-CameraDllRemoval $retiredPath
    } catch {
        $failure = $_
        if (-not [VtrPonCameraFileOperations]::MoveFileEx($retiredPath, $Path, 0)) {
            throw "Camera DLL removal failed and its name could not be restored: $retiredPath. $failure"
        }
        throw $failure
    }
    return $true
}

function Wait-CameraFilesIdle([string[]]$Paths, [int]$Attempts = 30) {
    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $busy = $null
        for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
            try {
                $idle = [IO.File]::Open($path, [IO.FileMode]::Open,
                    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $idle.Dispose()
                $busy = $null
                break
            } catch [IO.IOException] {
                $busy = $_
                if ($attempt + 1 -lt $Attempts) { Start-Sleep -Seconds 1 }
            }
        }
        if ($busy) { throw "Camera file is still in use: $path. $busy" }
    }
}

if (-not [Environment]::Is64BitProcess) {
    # Inno Setup 6 is a 32-bit process. Relaunch under native PowerShell before
    # using Program Files or the 64-bit HKLM COM registry view.
    $native = Join-Path $env:WINDIR 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                   '-File', $PSCommandPath, '-Action', $Action, '-UserSid', $UserSid)
    if ($SourceDll) { $arguments += @('-SourceDll', $SourceDll) }
    if ($UserAccount) { $arguments += @('-UserAccount', $UserAccount) }
    if ($BuildDirectory) { $arguments += @('-BuildDirectory', $BuildDirectory) }
    & $native @arguments
    exit $LASTEXITCODE
}
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights are required for COM DLL registration. Run this script in an elevated PowerShell.'
}
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
if ($UserAccount) {
    $UserSid = ([Security.Principal.NTAccount]::new($UserAccount)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
}
$null = [Security.Principal.SecurityIdentifier]::new($UserSid)
$classId = '{78686B3A-6797-4B6C-A225-81270522174F}'
$registry = "HKLM:\Software\Classes\CLSID\$classId"
$binaryDirectory = Join-Path $env:ProgramFiles 'VTR-PON\VirtualCameraPOC'
$dataDirectory = Join-Path $env:ProgramData 'VTR-PON\VirtualCameraPOC'
$installedDll = Join-Path $binaryDirectory 'vtr-pon-virtual-camera.dll'
$dataFile = Join-Path $dataDirectory 'frames-v2.bin'
$legacyDataFile = Join-Path $dataDirectory 'frames.bin'
if (Get-Process vtr-pon -ErrorAction SilentlyContinue) {
    throw 'Close VTR-PON before changing the virtual camera.'
}
$dllNeedsCopy = $false
if ($Action -eq 'Install') {
    if (-not $SourceDll) {
        $SourceDll = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'vtr-pon-virtual-camera.dll'
    }
    if (-not (Test-Path -LiteralPath $SourceDll -PathType Leaf)) { throw "Camera DLL is missing: $SourceDll" }
    $dllNeedsCopy = -not (Test-Path -LiteralPath $installedDll) -or
        (Get-FileHash -LiteralPath $SourceDll -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash
}
# Check ownership before changing any files, including retiring a mapped DLL.
if (Test-Path -LiteralPath $registry) {
    $registeredDll = (Get-Item -LiteralPath "$registry\InprocServer32").GetValue('')
    if ($registeredDll -ne $installedDll) {
        throw "Virtual camera registration belongs to another DLL: $registeredDll"
    }
}
# Keep the active frame-data guard. Only DLL-only retention during removal can
# use delayed cleanup; replacing a different DLL still requires exclusive access.
$pathsToCheck = @($dataFile, $legacyDataFile)
if ($Action -eq 'Install' -and $dllNeedsCopy) {
    $pathsToCheck = @($installedDll) + $pathsToCheck
}
Wait-CameraFilesIdle $pathsToCheck
# Literal allowlist only; never recursively remove a computed directory.
if ($Action -eq 'Uninstall') {
    $needsRestart = Remove-CameraDll $installedDll
    if (Test-Path -LiteralPath $registry) {
        Remove-Item -LiteralPath "$registry\InprocServer32"
        Remove-Item -LiteralPath $registry
    }
    if (Test-Path -LiteralPath $dataFile) { Remove-Item -LiteralPath $dataFile }
    if (Test-Path -LiteralPath $legacyDataFile) { Remove-Item -LiteralPath $legacyDataFile }
    if ($needsRestart) {
        Write-Output 'Virtual camera registration removed. Retired DLL will be deleted after Windows restarts.'
        exit 3010
    }
    Write-Output 'Virtual camera COM registration and files removed. Empty directories retained.'
    exit 0
}
if ((Test-Path -LiteralPath $dataFile) -and
    (Get-Item -LiteralPath $dataFile).Length -ne 9331456) {
    throw 'Existing camera frame mapping has an unexpected size. Close camera clients before updating.'
}
New-Item -ItemType Directory -Path $binaryDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
# Binaries are writable only by administrators/System. Frame data is shared only
# with the installing user and Local Service (the Windows Camera Frame Server).
$binaryAcl = [Security.AccessControl.DirectorySecurity]::new()
$binaryAcl.SetSecurityDescriptorSddlForm('D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;LS)')
Set-Acl -LiteralPath $binaryDirectory -AclObject $binaryAcl
$dataAcl = [Security.AccessControl.DirectorySecurity]::new()
$dataAcl.SetSecurityDescriptorSddlForm("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1301bf;;;$UserSid)")
Set-Acl -LiteralPath $dataDirectory -AclObject $dataAcl
if ($dllNeedsCopy) {
    Copy-Item -LiteralPath $SourceDll -Destination $installedDll -Force
}
# sizeof(Shared): 64-byte header + 3 x (24 metadata + NV12 + alignment).
# Keep in sync with protocol.h static_assert; refuse to truncate an in-use file.
if (-not (Test-Path -LiteralPath $dataFile)) {
    $stream = [IO.File]::Open($dataFile,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try { $stream.SetLength(9331456) } finally { $stream.Dispose() }
}
New-Item -Path "$registry\InprocServer32" -Force | Out-Null
Set-Item -LiteralPath "$registry\InprocServer32" -Value $installedDll
New-ItemProperty -LiteralPath "$registry\InprocServer32" -Name ThreadingModel -Value Both -PropertyType String -Force | Out-Null
Write-Output "Installed camera DLL: $installedDll"
Write-Output "Frame mapping: $dataFile (user $UserSid + Local Service)"
Write-Output 'Start VTR-PON normally. The camera is session-scoped and disappears when VTR-PON exits.'
