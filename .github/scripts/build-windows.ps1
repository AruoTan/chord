# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$craftRoot = $env:CRAFT_ROOT
if (-not $craftRoot) { $craftRoot = 'C:\CraftRoot' }
$env:CRAFT_ROOT = $craftRoot
. "$PSScriptRoot/initialize-craft-path.ps1"
. "$craftRoot/craft/craftenv.ps1"
Set-Location $source

$output = Join-Path $source 'dist'
New-Item -ItemType Directory -Force $output | Out-Null
$packageOutput = Join-Path $env:TEMP "kdeconnect-packages-$([guid]::NewGuid())"
New-Item -ItemType Directory $packageOutput | Out-Null

$options = @(
    '--ci-mode', '--buildtype', 'Release',
    '--options', 'kdeconnect-kde.version=master',
    '--options', "kdeconnect-kde.srcDir=$source",
    '--options', 'kdeconnect-kde.packageAppx=False',
    '--options', 'kdeconnect-kde.buildTests=False',
    '--options', 'kdeconnect-kde.args=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
    '--options', '[Packager]PackageType=NullsoftInstallerPackager',
    '--options', '[Packager]PackageDebugSymbols=False',
    '--options', '[CodeSigning]Enabled=False',
    '--options', "[Packager]Destination=$packageOutput"
)

sccache --version
if ($LASTEXITCODE) { throw 'sccache is required for this packaging build' }

craft --ci-mode nsis
if ($LASTEXITCODE) { throw 'Failed to install NSIS' }
craft @options --install-deps kdeconnect-kde
if ($LASTEXITCODE) { throw 'Failed to install build and runtime dependencies' }
# Explicit stages and no binary cache ensure this checkout is compiled.
craft @options --no-cache --unpack --compile --install --qmerge kdeconnect-kde
if ($LASTEXITCODE) { throw 'Failed to build the checked-out KDE Connect sources' }
# NullsoftInstallerPackager also generates a dependency-complete portable .7z.
craft @options --package kdeconnect-kde
if ($LASTEXITCODE) { throw 'Failed to package KDE Connect' }

$installers = @(Get-ChildItem $packageOutput -File -Filter '*.exe')
$archives = @(Get-ChildItem $packageOutput -File -Filter '*.7z' |
    Where-Object { $_.Name -notmatch '(debug|symbols)' })
if ($installers.Count -ne 1 -or $archives.Count -ne 1) {
    throw 'Expected exactly one installer and one portable archive'
}

$versionParts = foreach ($part in 'MAJOR', 'MINOR', 'MICRO') {
    $match = Select-String -Path "$source/CMakeLists.txt" -Pattern "set\s*\(RELEASE_SERVICE_VERSION_$part\s+`"(\d+)`"\)"
    if (-not $match) { throw "Missing version component $part" }
    $match.Matches[0].Groups[1].Value
}
$version = $versionParts -join '.'
$commit = git -C $source rev-parse --short=12 HEAD
if ($LASTEXITCODE) { throw 'Failed to identify source commit' }
$baseName = "kdeconnect-$version-$commit-windows-x64"
$portableName = "$baseName-portable.7z"
Copy-Item $installers[0].FullName "$output/$baseName-setup.exe"
Copy-Item $archives[0].FullName "$output/$portableName"

$probe = Join-Path $env:TEMP "kdeconnect-probe-$([guid]::NewGuid())"
try {
    & 7z x "$output/$portableName" "-o$probe" -y | Out-Null
    if ($LASTEXITCODE) { throw 'Portable archive cannot be extracted' }
    foreach ($executable in 'kdeconnect-app.exe', 'kdeconnect-indicator.exe', 'kdeconnectd.exe', 'kdeconnect-cli.exe', 'dbus-daemon.exe') {
        if (-not (Test-Path "$probe/bin/$executable")) {
            throw "Portable archive is missing $executable"
        }
    }
    # Do not allow DLLs from the build environment to conceal missing dependencies.
    $probeVariables = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
        'QML2_IMPORT_PATH', 'QML_IMPORT_PATH', 'QT_DATA_DIRS', 'XDG_DATA_DIRS')
    $previousEnvironment = @{}
    try {
        foreach ($name in $probeVariables) {
            $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $null)
        }
        $env:PATH = "$probe/bin;$env:SystemRoot/System32;$env:SystemRoot"
        & "$probe/bin/kdeconnect-cli.exe" --version
        if ($LASTEXITCODE) { throw 'Portable CLI failed to start outside Craft' }
    } finally {
        foreach ($name in $probeVariables) {
            [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name])
        }
    }
} finally {
    Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $packageOutput -Recurse -Force -ErrorAction SilentlyContinue
}

Get-ChildItem $output -File | Where-Object { $_.Extension -in '.exe', '.7z' } |
    ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.Name)" } |
    Set-Content "$output/SHA256SUMS" -Encoding utf8NoBOM

if ($env:SCOOP_BASE_URL) {
    $downloadBase = [uri]$env:SCOOP_BASE_URL
    if (-not $downloadBase.IsAbsoluteUri -or $downloadBase.Scheme -ne 'https') {
        throw 'SCOOP_BASE_URL must be an absolute HTTPS download directory'
    }
    $manifest = [ordered]@{
        version = "$version-$commit"
        description = 'Communications and data transfer between devices over local networks'
        homepage = 'https://kdeconnect.kde.org/'
        license = 'GPL-3.0-or-later'
        architecture = @{
            '64bit' = @{
                url = "$($env:SCOOP_BASE_URL.TrimEnd('/'))/$portableName"
                hash = (Get-FileHash "$output/$portableName" -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
        bin = 'bin\kdeconnect-cli.exe'
        shortcuts = ,@('bin\kdeconnect-app.exe', 'KDE Connect')
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content "$output/kdeconnect.json" -Encoding utf8NoBOM
}
