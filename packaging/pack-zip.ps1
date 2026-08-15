#Requires -Version 5.1
param(
    [string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path (Join-Path $root 'build_x64\rundir') $Configuration
$outDir = Join-Path $PSScriptRoot 'Output'

# Read version from buildspec.json
$buildspec = Get-Content (Join-Path $root 'buildspec.json') -Raw | ConvertFrom-Json
$version = $buildspec.version

if (-not (Test-Path $buildDir)) {
    throw "Build directory not found: $buildDir"
}

$dlls = @(
    'fullblur-filter.dll',
    'onnxruntime.dll',
    'onnxruntime_providers_shared.dll',
    'DirectML.dll'
)

foreach ($dll in $dlls) {
    $p = Join-Path $buildDir $dll
    if (-not (Test-Path $p)) {
        throw "Missing required file: $p"
    }
}

$tempBase = Join-Path $env:TEMP ('fullblur-filter-' + [Guid]::NewGuid())
$stage = Join-Path $tempBase ('fullblur-filter-' + $version + '-windows-x64')
New-Item -ItemType Directory -Path "$stage\obs-plugins\64bit" -Force | Out-Null
New-Item -ItemType Directory -Path "$stage\data\obs-plugins\fullblur-filter\locale" -Force | Out-Null
New-Item -ItemType Directory -Path "$stage\THIRD_PARTY_LICENSES" -Force | Out-Null

# Copy plugin binaries
foreach ($dll in $dlls) {
    Copy-Item (Join-Path $buildDir $dll) "$stage\obs-plugins\64bit\" -Force
}

# Copy locale only (do not copy stale/empty models folder)
$localeSrc = Join-Path $buildDir 'fullblur-filter\locale'
if (Test-Path $localeSrc) {
    Copy-Item "$localeSrc\*" "$stage\data\obs-plugins\fullblur-filter\locale\" -Recurse -Force
}

# Copy third-party licenses
$tpSrc = Join-Path $root 'THIRD_PARTY_LICENSES'
Copy-Item "$tpSrc\*" "$stage\THIRD_PARTY_LICENSES\" -Recurse -Force

# Copy packaging READMEs
Copy-Item (Join-Path $PSScriptRoot 'README.txt') "$stage\" -Force
Copy-Item (Join-Path $PSScriptRoot 'README_RU.txt') "$stage\" -Force

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$zipName = "fullblur-filter-$version-windows-x64.zip"
$zipPath = Join-Path $outDir $zipName

if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zipPath -CompressionLevel Optimal

# Compute SHA-256
$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash

Remove-Item $tempBase -Recurse -Force

Write-Host "Created: $zipPath"
Write-Host "SHA-256: $hash"
