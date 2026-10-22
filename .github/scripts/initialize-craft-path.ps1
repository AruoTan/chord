# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None

# CraftBootstrap detects Android from these variables even on Windows, then
# overrides BuildType with MinSizeRel, for which the KDE cache has no binaries.
Remove-Item Env:ANDROID_SDK_ROOT, Env:ANDROID_NDK -ErrorAction SilentlyContinue

# Native Windows make cannot consume the /c/... paths used by MSYS Autotools.
# Keep Craft's own tools, but exclude external make installations before setup.
$craftPrefix = [IO.Path]::GetFullPath($env:CRAFT_ROOT).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$env:PATH = (($env:PATH -split ';') | Where-Object {
    $_ -and (
        $_.StartsWith($craftPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Join-Path $_ 'make.exe'))
    )
}) -join ';'
