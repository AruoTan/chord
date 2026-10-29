# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None

. "$PSScriptRoot/initialize-craft-path.ps1"
$env:CRAFT_PYTHON = (Get-Command python -ErrorAction Stop).Source
$env:CraftRoot = Join-Path $env:CRAFT_ROOT 'craft'
# Use Craft's non-interactive interface; craftenv.ps1 clears the console.
Remove-Item Env:KDEROOT, Env:CRAFT_FORCE_RESET -ErrorAction SilentlyContinue
Remove-Item "$env:CRAFT_ROOT/etc/PristineCraftenv.json" -ErrorAction SilentlyContinue
$environmentJson = & $env:CRAFT_PYTHON "$env:CraftRoot/bin/CraftSetupHelper.py" --setup --format=json
if ($LASTEXITCODE) { throw 'Failed to initialize Craft environment' }
$environment = ($environmentJson -join "`n") | ConvertFrom-Json
foreach ($property in $environment.PSObject.Properties) {
    [Environment]::SetEnvironmentVariable($property.Name, [string]$property.Value)
}

function global:craft {
    $craftPython = Get-Command "$env:KDEROOT/dev-utils/bin/python3" -ErrorAction SilentlyContinue
    if ($craftPython) {
        & $craftPython.Source "$env:CraftRoot/bin/craft.py" @args
    } else {
        & $env:CRAFT_PYTHON "$env:CraftRoot/bin/craft.py" @args
    }
}
