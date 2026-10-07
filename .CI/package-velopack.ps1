[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z][A-Za-z0-9_.-]*$')]
    [string]$PackageId,

    [Parameter(Mandatory = $true)]
    [string]$PackDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PackageId -in @('MoltoBenne.Moltorino7', 'MoltoBenne.Moltorino7UpdaterTest')) {
    throw 'Use your own package ID. Official Moltorino package identities are reserved.'
}
if ($env:OS -ne 'Windows_NT' -or
    [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This packager requires Windows x64.'
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path -LiteralPath $PackDir -PathType Container)) {
    throw 'PackDir must be a staged runtime directory.'
}
$packPath = (Resolve-Path -LiteralPath $PackDir).Path
$outputPath = [IO.Path]::GetFullPath($OutputDir)
if ($outputPath.Equals($packPath, [StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($packPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The output directory must be outside the staged runtime.'
}
$identityPath = Join-Path $packPath 'moltorino-build-identity.json'
foreach ($file in @('Moltorino7.exe', 'velopack_libc.dll', 'moltorino-build-identity.json', 'licenses\Velopack.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $packPath $file) -PathType Leaf)) {
        throw "Missing $file. Stage a Windows x64 build with MOLTORINO_VELOPACK=ON."
    }
}
$identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
$cmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$version = [regex]::Match($cmake, 'project\s*\(\s*chatterino\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $version -or $identity.publicVersion -cne $version -or
    $identity.platform -cne 'windows' -or $identity.architecture -cne 'win-x64' -or
    $identity.velopackVersion -cne '1.2.0') {
    throw 'The staged build identity does not match this Windows x64 source build.'
}
if (-not $identity.PSObject.Properties['appUserModelId'] -or
    $identity.appUserModelId -cne $PackageId) {
    throw 'Rebuild with MOLTORINO_WINDOWS_APP_ID matching PackageId before staging this fork.'
}

$dotnet = (Get-Command dotnet -ErrorAction Stop).Source
$runtimes = (& $dotnet --list-runtimes) -join "`n"
if ($LASTEXITCODE -ne 0 -or $runtimes -notmatch '(?m)^Microsoft\.NETCore\.App 8\.') {
    throw 'Velopack 1.2.0 requires the .NET 8 runtime.'
}
$toolDir = Join-Path $repoRoot 'releases\_tools\vpk\1.2.0'
New-Item -ItemType Directory -Path $toolDir -Force | Out-Null
$archive = Join-Path $toolDir 'vpk.1.2.0.zip'
$expectedHash = '3e458a676be46d1122e522312db18411f36ea8c70e586f81a676695d43f89dbc'
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    $download = "$archive.download"
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/velopack/velopack/releases/download/1.2.0/vpk.1.2.0.nupkg' -OutFile $download
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw 'The downloaded Velopack CLI does not match the pinned SHA-256.'
    }
    Move-Item -LiteralPath $download -Destination $archive -Force
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'The cached Velopack CLI does not match the pinned SHA-256.'
}
$extractDir = Join-Path $toolDir 'package'
Expand-Archive -LiteralPath $archive -DestinationPath $extractDir -Force
$vpk = Join-Path $extractDir 'tools\net8.0\any\vpk.dll'
if (-not (Test-Path -LiteralPath $vpk -PathType Leaf)) {
    throw 'The pinned archive is missing vpk.dll.'
}

& $dotnet $vpk --skip-updates --yes pack `
    --packId $PackageId --packVersion $version --packDir $packPath `
    --mainExe Moltorino7.exe --packTitle $PackageId --packAuthors $PackageId `
    --channel win-x64-fork --runtime win-x64 --outputDir $outputPath `
    --icon (Join-Path $repoRoot 'resources\icon.ico') --aumid $PackageId `
    --shortcuts StartMenuRoot --noPortable --delta None
if ($LASTEXITCODE -ne 0) {
    throw "Velopack packaging failed with exit code $LASTEXITCODE."
}
Write-Output "Packages created in $outputPath."
