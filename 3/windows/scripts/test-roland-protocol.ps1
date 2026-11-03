param(
    [string]$QtPrefix = "",
    [string]$BuildDirectory = "",
    [string]$Configuration = "RelWithDebInfo",
    [string]$GStreamerBin = "",
    [switch]$BuildApplication
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ($env:OS -ne "Windows_NT") { throw "Native WinMM verification requires Windows." }
if (-not [Environment]::Is64BitProcess) { throw "Use 64-bit PowerShell with the x64 Qt/GStreamer runtimes." }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "Run from the Visual Studio x64 developer PowerShell (cl.exe must be available)."
}
$vtrponSourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $vtrponSourceRoot "build/roland-protocol-windows" }
if (-not $QtPrefix) {
    $vtrponCache = Join-Path $vtrponSourceRoot "build/CMakeCache.txt"
    if (Test-Path $vtrponCache) {
        foreach ($vtrponLine in Get-Content $vtrponCache) {
            if ($vtrponLine -match '^Qt6_DIR:[^=]+=(.+)$') {
                $QtPrefix = [IO.Path]::GetFullPath((Join-Path $Matches[1] "../../.."))
                break
            }
        }
    }
}
if (-not $QtPrefix -or -not (Test-Path (Join-Path $QtPrefix "lib/cmake/Qt6/Qt6Config.cmake"))) {
    throw "Specify -QtPrefix with the Qt MSVC x64 installation root."
}
function Invoke-NativeBuild {
    param([string]$Tool, [string[]]$ToolArguments)
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE" }
}
if (-not ("VtrponRolandDllProbe" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VtrponRolandDllProbe {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryW(string name);
    [DllImport("kernel32", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
}
function Test-NativeDll {
    param([string]$Name)
    $vtrponModule = [VtrponRolandDllProbe]::LoadLibraryW($Name)
    if ($vtrponModule -eq [IntPtr]::Zero) {
        throw "Cannot load $Name with this process PATH (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
    }
    [void][VtrponRolandDllProbe]::FreeLibrary($vtrponModule)
}
if ($BuildApplication -and -not $GStreamerBin) {
    if ($env:GSTREAMER_1_0_ROOT_MSVC_X86_64) {
        $GStreamerBin = Join-Path $env:GSTREAMER_1_0_ROOT_MSVC_X86_64 "bin"
    }
}
if ($BuildApplication -and (-not $GStreamerBin -or -not (Test-Path $GStreamerBin))) {
    throw "Specify -GStreamerBin for the normal application build/tests."
}
$vtrponPreviousPath = $env:PATH
try {
    $env:PATH = (Join-Path $QtPrefix "bin") + ";" + $env:PATH
    if ($GStreamerBin) { $env:PATH = $GStreamerBin + ";" + $env:PATH }
    Test-NativeDll "Qt6Core.dll"
    Test-NativeDll "Qt6Network.dll"
    if ($BuildApplication) { Test-NativeDll "libgstreamer-1.0-0.dll"; Test-NativeDll "libgstapp-1.0-0.dll" }
    Invoke-NativeBuild -Tool cmake -ToolArguments @(
        "-S", (Join-Path $vtrponSourceRoot "tests/roland-protocol"),
        "-B", $BuildDirectory, "-G", "Ninja",
        "-DQt6_DIR=$(Join-Path $QtPrefix 'lib/cmake/Qt6')",
        "-DCMAKE_BUILD_TYPE=$Configuration")
    Invoke-NativeBuild -Tool cmake -ToolArguments @("--build", $BuildDirectory, "--config", $Configuration)
    Invoke-NativeBuild -Tool ctest -ToolArguments @(
        "--test-dir", $BuildDirectory, "-C", $Configuration, "--output-on-failure",
        "--no-tests=error", "--output-junit", (Join-Path $BuildDirectory "results.xml"))
    if ($BuildApplication) {
        $vtrponMainBuild = Join-Path $vtrponSourceRoot "build"
        Invoke-NativeBuild -Tool cmake -ToolArguments @("-S", $vtrponSourceRoot, "-B", $vtrponMainBuild, "-DBUILD_TESTING=ON")
        Invoke-NativeBuild -Tool cmake -ToolArguments @(
            "--build", $vtrponMainBuild, "--config", $Configuration, "--target", "vtr-pon",
            "program-playout-transition-test", "switcher-report-mail-test", "language-catalog-test")
        Invoke-NativeBuild -Tool ctest -ToolArguments @(
            "--test-dir", $vtrponMainBuild, "-C", $Configuration,
            "-R", "^(switcher-control|switcher-report-mail-(en|ja)|language-catalog)$",
            "--output-on-failure", "--no-tests=error",
            "--output-junit", (Join-Path $BuildDirectory "application-results.xml"))
    }
} finally {
    $env:PATH = $vtrponPreviousPath
}
