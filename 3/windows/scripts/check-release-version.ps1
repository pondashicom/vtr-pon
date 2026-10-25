param(
    [ValidateSet('Build', 'Package', 'Published')]
    [string]$Stage = 'Build',
    [string]$BuildDir,
    [string]$DistributionDir
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }
if (-not $DistributionDir) { $DistributionDir = Join-Path $root 'dist' }

function Assert-Equal([string]$Label, [string]$Actual, [string]$Expected) {
    if ($Actual -cne $Expected) {
        throw "$Label mismatch: expected '$Expected', got '$Actual'"
    }
}

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file: $Path"
    }
}

function Assert-VersionInfo([string]$Path, [string]$TextVersion, [int[]]$Numbers) {
    $info = (Get-Item -LiteralPath $Path).VersionInfo
    # Inno Setup pads its version-resource text fields with spaces.
    Assert-Equal "$Path ProductVersion" $info.ProductVersion.TrimEnd() $TextVersion
    $actualNumbers = @($info.ProductMajorPart, $info.ProductMinorPart,
                       $info.ProductBuildPart, $info.ProductPrivatePart)
    Assert-Equal "$Path numeric ProductVersion" ($actualNumbers -join '.') ($Numbers -join '.')
}

$source = Join-Path $root 'release/version.json'
Assert-File $source
$config = Get-Content -LiteralPath $source -Raw | ConvertFrom-Json
if (($config.releaseReady -isnot [bool]) -or ($config.version -isnot [string])) {
    throw 'release/version.json needs a string version and boolean releaseReady.'
}
$version = $config.version
$match = [regex]::Match($version, '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(beta|rc)\.([1-9][0-9]*))?$')
if (-not $match.Success) { throw "Unsupported version: $version" }
$numbers = @([int]$match.Groups[1].Value, [int]$match.Groups[2].Value,
             [int]$match.Groups[3].Value)
if (@($numbers | Where-Object { $_ -gt 65535 }).Count) {
    throw 'Windows version components must be <= 65535.'
}
$phase = $match.Groups[4].Value
$sequence = if ($phase) { [int]$match.Groups[5].Value } else { 0 }
if ($sequence -gt 999) { throw 'Prerelease sequence must be 1..999.' }
$revision = if ($phase -eq 'beta') { 1000 + $sequence }
            elseif ($phase -eq 'rc') { 2000 + $sequence }
            else { 3000 }
$numbers += $revision
$windowsVersion = $numbers -join '.'
$tag = "v$version"
$packageName = "VTR-PON-$version-windows-x64.zip"
$installerName = "VTR-PON-$version-windows-x64-setup.exe"
$checksumName = "VTR-PON-$version-SHA256SUMS.txt"
$officialBase = 'https://pondashi.com/vtr-pon/download/files/'

$cache = Join-Path $BuildDir 'CMakeCache.txt'
Assert-File $cache
$cacheMatches = @(Select-String -LiteralPath $cache -Pattern '^VTRPON_APP_VERSION:[^=]+=(.*)$')
if ($cacheMatches.Count -ne 1) { throw 'CMakeCache.txt must contain one VTRPON_APP_VERSION.' }
Assert-Equal 'CMake cache version' $cacheMatches[0].Matches[0].Groups[1].Value $version

$iss = Join-Path $BuildDir 'release/version.iss'
Assert-File $iss
$issText = Get-Content -LiteralPath $iss -Raw
$issTemplate = Get-Content -LiteralPath (Join-Path $root 'release/version.iss.in') -Raw
$expectedIss = $issTemplate.Replace('@VTRPON_APP_VERSION@', $version).
    Replace('@VTRPON_WINDOWS_VERSION@', $windowsVersion).
    Replace('@VTRPON_INSTALLER_BASENAME@', [System.IO.Path]::GetFileNameWithoutExtension($installerName))
Assert-Equal 'generated Inno version include' ($issText -replace "`r`n", "`n") ($expectedIss -replace "`r`n", "`n")

$exe = Join-Path $BuildDir 'vtr-pon.exe'
Assert-File $exe
Assert-VersionInfo $exe $version $numbers
Assert-Equal 'EXE FileVersion' (Get-Item -LiteralPath $exe).VersionInfo.FileVersion $version
if ((Get-Item -LiteralPath $exe).VersionInfo.IsPreRelease -ne [bool]$phase) {
    throw 'EXE prerelease flag does not match the version.'
}

if ($Stage -ne 'Build') {
    if (-not $config.releaseReady) {
        throw 'releaseReady is false; this version is a development placeholder.'
    }
    $dirty = & git -C $root status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Release source must be a clean Git worktree.' }

    $package = Join-Path $DistributionDir $packageName
    $installer = Join-Path $DistributionDir $installerName
    Assert-File $package
    Assert-File $installer
    Assert-VersionInfo $installer $version $numbers

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $entries = @($archive.Entries | Where-Object {
            $_.FullName -match '(^|/)vtr-pon\.exe$'
        })
        if ($entries.Count -ne 1) { throw 'Package must contain exactly one vtr-pon.exe.' }
        $stream = $entries[0].Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { $insideHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose() }
        }
        finally { $stream.Dispose() }
    }
    finally { $archive.Dispose() }
    Assert-Equal 'packaged EXE SHA-256' $insideHash (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()

    $sourceRecordPath = Join-Path $DistributionDir "VTR-PON-$version-windows-x64/LICENSE/release-source.json"
    Assert-File $sourceRecordPath
    $sourceRecord = Get-Content -LiteralPath $sourceRecordPath -Raw | ConvertFrom-Json
    Assert-Equal 'source record version' $sourceRecord.version $version
    Assert-Equal 'source record commit' $sourceRecord.projectCommit (& git -C $root rev-parse HEAD)
    & py -3 (Join-Path $root 'scripts/verify-release-licenses.py') --stage (Join-Path $DistributionDir "VTR-PON-$version-windows-x64") --require-source-record
    if ($LASTEXITCODE -ne 0) { throw 'Source preparation record or license verification failed.' }
    & py -3 (Join-Path $root 'scripts/verify-windows-package.py') --distribution-dir $DistributionDir
    if ($LASTEXITCODE -ne 0) { throw 'Payload and ZIP verification failed.' }
    $authenticatedEvidencePath = Join-Path $DistributionDir "VTR-PON-$version-source-access-authenticated.json"
    Assert-File $authenticatedEvidencePath
    $authenticatedEvidence = Get-Content -LiteralPath $authenticatedEvidencePath -Raw | ConvertFrom-Json
    Assert-Equal 'source preparation evidence schema' $authenticatedEvidence.schemaVersion '1'
    Assert-Equal 'source preparation access mode' $authenticatedEvidence.mode 'authenticated'
    Assert-Equal 'source preparation result' $authenticatedEvidence.result 'retrieved-and-checked'
    Assert-Equal 'source preparation version' $authenticatedEvidence.version $version
    Assert-Equal 'source preparation commit' $authenticatedEvidence.projectCommit $sourceRecord.projectCommit
    Assert-Equal 'third-party source URL' $authenticatedEvidence.thirdPartySourceAccessUrl $sourceRecord.thirdPartySourceUrl
    Assert-Equal 'third-party source SHA-256' $authenticatedEvidence.thirdPartySourceSha256 $sourceRecord.thirdPartySourceSha256
    Assert-Equal 'authenticated project source URL' $authenticatedEvidence.projectSourceAccessUrl "https://api.github.com/repos/pondashicom/vtr-pon/zipball/$($sourceRecord.projectCommit)"
    Assert-Equal 'authenticated build instructions URL' $authenticatedEvidence.buildInstructionsAccessUrl "https://api.github.com/repos/pondashicom/vtr-pon/contents/3/windows/BUILDING.md?ref=$($sourceRecord.projectCommit)"
    if ($authenticatedEvidence.projectSourceSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'Source preparation evidence lacks the downloaded project ZIP SHA-256.'
    }
    $checkedAt = [datetimeoffset]::MinValue
    if (-not [datetimeoffset]::TryParse($authenticatedEvidence.checkedAtUtc, [ref]$checkedAt)) {
        throw 'Source preparation evidence has no valid check time.'
    }
}

if ($Stage -eq 'Published') {
    $objectType = & git -C $root cat-file -t "refs/tags/$tag" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Missing Git tag: $tag" }
    Assert-Equal 'Git tag type' $objectType 'tag'
    $tagCommit = & git -C $root rev-list -n 1 "refs/tags/$tag"
    $headCommit = & git -C $root rev-parse HEAD
    Assert-Equal 'Git tag commit' $tagCommit $headCommit
    $remoteTag = & git -C $root ls-remote --tags origin "refs/tags/$tag^{}"
    if ($LASTEXITCODE -ne 0 -or -not $remoteTag) { throw "Missing remote annotated tag: $tag" }
    Assert-Equal 'remote Git tag commit' (($remoteTag -split '\s+')[0]) $headCommit

    $releaseJson = & gh release view $tag --json tagName,name,isDraft,isPrerelease,body
    if ($LASTEXITCODE -ne 0) { throw "Cannot read GitHub Release $tag" }
    $release = $releaseJson | ConvertFrom-Json
    Assert-Equal 'GitHub Release tag' $release.tagName $tag
    Assert-Equal 'GitHub Release title' $release.name "VTR-PON $tag"
    if ($release.isDraft) { throw 'GitHub Release is still a draft.' }
    if ($release.isPrerelease -ne [bool]$phase) {
        throw 'GitHub Release prerelease flag does not match the version.'
    }
    foreach ($name in @($packageName, $installerName, $checksumName)) {
        if (-not $release.body.Contains("$officialBase$name")) {
            throw "GitHub Release does not link to the official file $name"
        }
    }
    $checksum = Join-Path $DistributionDir $checksumName
    Assert-File $checksum
    $expected = @{}
    foreach ($name in @($packageName, $installerName)) {
        $expected[$name] = (Get-FileHash -LiteralPath (Join-Path $DistributionDir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if (-not ((Get-Content -LiteralPath $checksum) -ccontains "$($expected[$name])  $name")) {
            throw "Local checksum list does not match $name"
        }
    }
    $downloadDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $downloadDir | Out-Null
    try {
        foreach ($name in @($packageName, $installerName, $checksumName)) {
            $download = Join-Path $downloadDir $name
            Invoke-WebRequest -Uri "$officialBase$name" -OutFile $download -UseBasicParsing
            if ($name -eq $checksumName) {
                Assert-Equal 'official checksum list' (Get-Content -LiteralPath $download -Raw) (Get-Content -LiteralPath $checksum -Raw)
            } else {
                Assert-Equal "official $name SHA-256" (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() $expected[$name]
            }
        }
    }
    finally { Remove-Item -LiteralPath $downloadDir -Recurse -Force }
    & py -3 (Join-Path $root 'scripts/check-release-source-access.py') --record $sourceRecordPath --mode anonymous --evidence (Join-Path $DistributionDir "VTR-PON-$version-source-access-anonymous.json")
    if ($LASTEXITCODE -ne 0) { throw 'Anonymous source retrieval failed.' }
}

Write-Host "Version check passed: $version ($windowsVersion), $tag, stage $Stage"
