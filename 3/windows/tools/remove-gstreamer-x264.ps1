# Run from an elevated PowerShell after closing GStreamer applications.
# A GStreamer reinstall/repair can restore these files; rerun afterwards.
# This removes x264 only and is not a full runtime license audit.
#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$GStreamerRoot = 'C:\Program Files\gstreamer\1.0\msvc_x86_64'
)

$ErrorActionPreference = 'Stop'
$removalRoot = (Resolve-Path -LiteralPath $GStreamerRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $removalRoot 'bin\gst-inspect-1.0.exe'))) {
    throw 'The target is not a GStreamer installation.'
}
$relativeFiles = @(
    'lib\gstreamer-1.0\gstx264.dll',
    'lib\gstreamer-1.0\libgstx264.a',
    'bin\x264-164.dll',
    'include\x264_config.h',
    'include\x264.h',
    'lib\libx264.a',
    'lib\libx264.dll.a',
    'lib\x264.def',
    'lib\x264.lib',
    'lib\pkgconfig\x264.pc',
    'share\vs\2010\libs\x264.props'
)
foreach ($relativeFile in $relativeFiles) {
    $removalPath = [IO.Path]::GetFullPath((Join-Path $removalRoot $relativeFile))
    if (-not $removalPath.StartsWith($removalRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path outside GStreamer installation: $removalPath"
    }
    if ((Test-Path -LiteralPath $removalPath) -and $PSCmdlet.ShouldProcess($removalPath, 'Remove x264 file')) {
        Remove-Item -LiteralPath $removalPath
        Write-Output "Removed: $removalPath"
    }
}
if (-not $WhatIfPreference) {
    $remainingFiles = @(Get-ChildItem -LiteralPath $removalRoot -Recurse -File -Filter '*x264*')
    if ($remainingFiles.Count -gt 0) {
        throw ('x264 files remain: ' + ($remainingFiles.FullName -join ', '))
    }
    Write-Output 'No x264 files remain in the selected GStreamer installation.'
}
