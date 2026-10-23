# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None
param([switch]$DependenciesOnly, [switch]$SkipDependencies)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$craftRoot = $env:CRAFT_ROOT
if (-not $craftRoot) { $craftRoot = 'C:\CraftRoot' }
$env:CRAFT_ROOT = $craftRoot
. "$PSScriptRoot/enter-craft-environment.ps1"
Set-Location $source

$output = Join-Path $source 'dist'
New-Item -ItemType Directory -Force $output | Out-Null
$packageOutput = Join-Path $env:TEMP "kdeconnect-packages-$([guid]::NewGuid())"
New-Item -ItemType Directory $packageOutput | Out-Null

$options = @(
    # Craft resolves runtime images by build type; match the dependency snapshot.
    '--ci-mode', '--buildtype', 'RelWithDebInfo',
    '--options', 'kdeconnect-kde.version=master',
    '--options', "kdeconnect-kde.srcDir=$source",
    '--options', 'kdeconnect-kde.packageAppx=False',
    '--options', 'kdeconnect-kde.buildTests=False',
    '--options', 'kdeconnect-kde.args=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DKDECONNECT_BUILD_REDUCED_PLUGIN_SET=ON',
    '--options', '[Packager]PackageType=NullsoftInstallerPackager',
    '--options', '[Packager]PackageDebugSymbols=False',
    '--options', '[CodeSigning]Enabled=False',
    '--options', "[Packager]Destination=$packageOutput"
)

sccache --version
if ($LASTEXITCODE) { throw 'sccache is required for this packaging build' }

if (-not $SkipDependencies) {
    craft --ci-mode --buildtype RelWithDebInfo nsis
    if ($LASTEXITCODE) { throw 'Failed to install NSIS' }
    craft @options --install-deps kdeconnect-kde
    if ($LASTEXITCODE) { throw 'Failed to install build and runtime dependencies' }
}
if ($DependenciesOnly) { return }
python "$PSScriptRoot/configure-windows-blueprint.py"
if ($LASTEXITCODE) { throw 'Failed to configure native service packaging in the Craft blueprint' }
Write-Host "[$(Get-Date -Format o)] Building the checked-out sources"
# Explicit stages and no binary cache ensure this checkout is compiled.
craft @options --no-cache --unpack --compile --install --qmerge kdeconnect-kde
if ($LASTEXITCODE) { throw 'Failed to build the checked-out KDE Connect sources' }
Write-Host "[$(Get-Date -Format o)] Creating the installer and portable archive"
# NullsoftInstallerPackager also generates a dependency-complete portable .7z.
craft @options --package kdeconnect-kde
if ($LASTEXITCODE) { throw 'Failed to package KDE Connect' }
Write-Host "[$(Get-Date -Format o)] Packaging complete; collecting artifacts"

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
    Write-Host "[$(Get-Date -Format o)] Extracting the portable archive for validation"
    & 7z x "$output/$portableName" "-o$probe" -y
    if ($LASTEXITCODE) { throw 'Portable archive cannot be extracted' }
    Write-Host "[$(Get-Date -Format o)] Checking portable executables and plugins"
    foreach ($executable in 'kdeconnect-app.exe', 'kdeconnect-indicator.exe', 'kdeconnectd.exe', 'kdeconnect-cli.exe', 'kdeconnect-service.exe', 'dbus-daemon.exe') {
        if (-not (Test-Path "$probe/bin/$executable")) {
            throw "Portable archive is missing $executable"
        }
    }
    $excludedPlugins = @(
        'findmyphone', 'findthisdevice', 'lockdevice', 'mpriscontrol', 'mprisremote',
        'pausemusic', 'ping', 'presenter', 'remotecommands', 'remotecontrol',
        'remotekeyboard', 'remotesystemvolume', 'runcommand', 'screensaver_inhibit',
        'systemvolume', 'telephony', 'virtualmonitor', 'mousepad', 'contacts', 'digitizer'
    ) | ForEach-Object { "kdeconnect_$_" }
    $unexpectedPlugins = @(Get-ChildItem $probe -Recurse -File -Filter 'kdeconnect_*.dll' |
        Where-Object { $_.BaseName -in $excludedPlugins })
    if ($unexpectedPlugins.Count) {
        throw "Portable archive contains excluded plugins: $($unexpectedPlugins.Name -join ', ')"
    }
    $translationFiles = @(Get-ChildItem $probe -Recurse -File | Where-Object {
        $relative = [IO.Path]::GetRelativePath($probe, $_.FullName).Replace('\', '/')
        $englishLocale = $relative -match '(^|/)locale/en(?:_[A-Za-z0-9]+)*/'
        $englishQtCatalog = $_.Extension -eq '.qm' -and $_.BaseName -match '(^|_)en(?:_[A-Za-z0-9]+)*$'
        ($relative -match '(^|/)locale/' -and -not $englishLocale) -or
            ($_.Extension -in '.mo', '.qm' -and -not ($englishLocale -or $englishQtCatalog))
    })
    if ($translationFiles.Count) {
        throw "Portable archive contains non-English translation resources: $($translationFiles.FullName -join ', ')"
    }
    # Do not allow DLLs from the build environment to conceal missing dependencies.
    $probeVariables = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
        'QML2_IMPORT_PATH', 'QML_IMPORT_PATH', 'QT_DATA_DIRS', 'XDG_DATA_DIRS', 'DBUS_SESSION_BUS_ADDRESS')
    $previousEnvironment = @{}
    try {
        foreach ($name in $probeVariables) {
            $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $null)
        }
        $env:PATH = "$probe/bin;$env:SystemRoot/System32;$env:SystemRoot"
        # Exercise D-Bus cleanup without starting a daemon or contacting the user's session.
        $env:DBUS_SESSION_BUS_ADDRESS = 'kdeconnect-test-invalid-transport:'
        foreach ($check in 'version', 'help', 'my-id', 'pair', 'list-available') {
            Write-Host "[$(Get-Date -Format o)] Checking portable CLI --$check (30-second timeout)"
            $probeStdout = Join-Path $env:TEMP "kdeconnect-portable-probe.$check.stdout.log"
            $probeStderr = Join-Path $env:TEMP "kdeconnect-portable-probe.$check.stderr.log"
            $probeProcess = Start-Process -FilePath "$probe/bin/kdeconnect-cli.exe" -ArgumentList "--$check" `
                -NoNewWindow -PassThru -RedirectStandardOutput $probeStdout -RedirectStandardError $probeStderr
            try {
                if (-not $probeProcess.WaitForExit(30000)) {
                    $probeProcess.Kill($true)
                    $null = $probeProcess.WaitForExit(5000)
                    throw 'Portable CLI startup timed out after 30 seconds; see kdeconnect-portable-probe logs'
                }
                $expectedExitCode = if ($check -in 'pair', 'list-available') { 1 } else { 0 }
                if ($probeProcess.ExitCode -ne $expectedExitCode) {
                    throw "Portable CLI --$check returned $($probeProcess.ExitCode); expected $expectedExitCode"
                }
                $stderrText = [IO.File]::ReadAllText($probeStderr)
                if ($stderrText.Contains('QEventDispatcherWin32::wakeUp')) {
                    throw "Portable CLI --$check reported an invalid event-dispatcher window during shutdown"
                }
                if ($check -eq 'pair' -and -not $stderrText.Contains('No device specified')) {
                    throw 'Portable CLI did not exercise the missing-device return path'
                }
                if ($check -eq 'list-available' -and -not $stderrText.Contains('error: ')) {
                    throw 'Portable CLI did not exercise the D-Bus error exit path'
                }
                if ($check -eq 'help') {
                    $helpText = Get-Content $probeStdout -Raw
                    foreach ($option in '--service-register', '--service-unregister') {
                        if (-not $helpText.Contains($option)) { throw "Portable CLI is missing $option" }
                    }
                }
            } finally {
                foreach ($log in $probeStdout, $probeStderr) {
                    if (Test-Path $log) { Get-Content $log | Write-Host }
                }
                $probeProcess.Dispose()
            }
        }
    } finally {
        foreach ($name in $probeVariables) {
            [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name])
        }
    }
} finally {
    Write-Host "[$(Get-Date -Format o)] Removing temporary package validation files"
    Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $packageOutput -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "[$(Get-Date -Format o)] Writing package checksums"
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
