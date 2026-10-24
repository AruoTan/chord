# SPDX-License-Identifier: CC0-1.0
param([Parameter(Mandatory)][string]$BuildDirectory, [switch]$RequireInteractive)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$build = (Resolve-Path $BuildDirectory).Path
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$serviceName = "KDEConnectBackground-$($identity.User.Value)"
if (Get-CimInstance Win32_Service -Filter "Name='$serviceName'") {
    throw 'Refusing to replace an existing background service during tests'
}
$interactive = [Diagnostics.Process]::GetCurrentProcess().SessionId -gt 0
if ($RequireInteractive -and -not $interactive) {
    throw 'Worker recovery tests require an interactive Windows user session'
}
$fixture = Join-Path $env:TEMP "kdeconnect-service-test-$([guid]::NewGuid())"
New-Item -ItemType Directory $fixture | Out-Null
Copy-Item "$build/Release/kdeconnect-service.exe" $fixture
Copy-Item "$build/Release/service-test-worker.exe" "$fixture/kdeconnectd.exe"
$helper = Join-Path $fixture 'kdeconnect-service.exe'
$hostDirectory = Join-Path (Join-Path $env:ProgramFiles 'KDE Connect Background') $identity.User.Value
$configDirectory = Join-Path $env:LOCALAPPDATA 'kdeconnect'
$stateDirectory = Join-Path $configDirectory 'service-state'
$stateKey = 'HKCU:\Software\KDEConnectBackgroundServiceState'
$notificationKeys = @(
    'HKCU:\Software\Classes\AppUserModelId\org.kde.kdeconnect.transfers',
    'HKCU:\Software\Classes\CLSID\{5FA5D3F1-182B-49FD-A313-C06C5336CC70}',
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Notifications\Settings\org.kde.kdeconnect.transfers'
)
$desktopFiles = @(
    (Join-Path ([Environment]::GetFolderPath('Programs')) 'KDE Connect Transfers.lnk'),
    "$configDirectory/background.log", "$configDirectory/background.log.1",
    "$configDirectory/background-status.json", "$configDirectory/transfer-notifications.log",
    "$configDirectory/transfer-notifications.log.1"
)
foreach ($path in @($notificationKeys) + @($desktopFiles) + @($stateKey, $stateDirectory)) {
    if (Test-Path $path) { throw "Refusing to overwrite existing desktop integration during tests: $path" }
}
$sentinel = Join-Path $configDirectory 'service-test-user-data.keep'
if (Test-Path $sentinel) { throw 'Test user-data sentinel already exists' }
$createdConfigDirectory = -not (Test-Path $configDirectory)
New-Item -ItemType Directory $configDirectory -Force | Out-Null
[IO.File]::WriteAllText($sentinel, 'User configuration and pairing data must be retained')
function Assert($Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    Write-Host "PASS: $Message"
}
function Configure([string]$Operation) {
    & $helper $Operation
    if ($LASTEXITCODE) { throw "Service helper failed: $Operation" }
}
function Wait-Worker([int]$PreviousPid = 0, [int]$TimeoutSeconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $snapshot = $null
        try { $snapshot = Get-Content "$fixture/worker.json" -Raw -ErrorAction Stop | ConvertFrom-Json } catch {}
        if ($snapshot -and $snapshot.pid -ne $PreviousPid -and (Get-Process -Id $snapshot.pid -ErrorAction SilentlyContinue)) {
            return $snapshot
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for a new user background process'
}
function Wait-Supervisor([int]$PreviousPid) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        if ($service -and $service.State -eq 'Running' -and $service.ProcessId -ne $PreviousPid) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'SCM did not recover the interrupted service'
}
function Set-DesktopArtifacts([string]$Value) {
    foreach ($key in $notificationKeys) {
        New-Item $key -Force | Out-Null
        New-ItemProperty $key -Name 'ServiceTestValue' -Value $Value -PropertyType String -Force | Out-Null
    }
    New-Item "$($notificationKeys[0])\Nested" -Force | Out-Null
    $number = if ($Value -eq 'original desktop state') { 42 } else { 99 }
    New-ItemProperty "$($notificationKeys[0])\Nested" -Name 'Number' -Value $number -PropertyType DWord -Force | Out-Null
    foreach ($path in $desktopFiles) {
        if (Test-Path $path) { [IO.File]::SetAttributes($path, [IO.FileAttributes]::Normal) }
        if ($path -eq $desktopFiles[0]) {
            $appId = if ($Value -eq 'original desktop state') { 'org.kde.kdeconnect.test-user' } else { 'org.kde.kdeconnect.transfers' }
            & "$fixture/kdeconnectd.exe" '--create-shortcut' $path $appId
            if ($LASTEXITCODE) { throw 'Failed to create shortcut fixture' }
        } else {
            [IO.File]::WriteAllText($path, $Value)
        }
    }
}
function Assert-DesktopAbsent {
    foreach ($path in @($notificationKeys) + @($desktopFiles) + @($stateKey, $stateDirectory, $hostDirectory)) {
        Assert (-not (Test-Path $path)) "Service-created state removed: $path"
    }
    Assert (Test-Path $sentinel) 'User data is retained.'
}
function Save-ServiceLog {
    if (Test-Path "$hostDirectory/service.log") {
        Get-Content "$hostDirectory/service.log" | Add-Content "$build/service-test.log"
    }
}
try {
    Configure '--register'
    Assert (Test-Path $stateKey) 'Registration records the original desktop state.'
    Set-DesktopArtifacts 'created by service'
    & "$fixture/kdeconnectd.exe" '--remove-legacy-shortcut' $desktopFiles[0]
    Assert ($LASTEXITCODE -eq 0 -and -not (Test-Path $desktopFiles[0])) 'Migration removes the obsolete notification shortcut.'
    Set-DesktopArtifacts 'created by service'
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    Assert ($service.StartMode -eq 'Auto' -and $service.StartName -eq 'LocalSystem') 'Supervisor is an automatic Windows service.'
    Assert ($service.PathName.StartsWith('"' + $hostDirectory, [StringComparison]::OrdinalIgnoreCase)) 'Privileged host uses its protected Program Files copy.'
    $acl = Get-Acl "$hostDirectory/kdeconnect-service.exe"
    $unsafe = @($acl.Access | Where-Object {
        $_.AccessControlType -eq 'Allow' -and
        $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin 'S-1-5-18', 'S-1-5-32-544' -and
        ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write) -ne 0
    })
    Assert ($unsafe.Count -eq 0) 'Unprivileged users cannot modify the service executable.'

    if ($interactive) {
        $first = Wait-Worker
        Assert ($first.sid -eq $identity.User.Value -and $first.session -gt 0 -and $first.background) 'Worker runs as the registered desktop user in background mode.'
        Stop-Process -Id $first.pid -Force
        $second = Wait-Worker -PreviousPid $first.pid
        Assert ($second.pid -ne $first.pid) 'Worker interruption is recovered.'

        Configure '--register'
        $third = Wait-Worker -PreviousPid $second.pid
        Assert ($third.pid -ne $second.pid) 'Repeated registration safely restarts the owned worker.'
        Assert ((Get-Content "$fixture/worker-stopped.txt") -contains [string]$second.pid) 'Replacement requests a graceful worker stop.'

        New-Item -ItemType File "$fixture/pause-heartbeat" | Out-Null
        $fourth = Wait-Worker -PreviousPid $third.pid -TimeoutSeconds 90
        Remove-Item "$fixture/pause-heartbeat"
        Assert ($fourth.pid -ne $third.pid) 'Missing event-loop heartbeat recovers a hung worker.'
        Assert (-not (Get-Process -Id $third.pid -ErrorAction SilentlyContinue)) 'The unresponsive worker is terminated through its owned job.'
    } else {
        Write-Host 'SKIP: user-session and heartbeat tests need an interactive login. Use -RequireInteractive on a desktop runner for full coverage.'
        Configure '--register'
    }

    $supervisor = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    Stop-Process -Id $supervisor.ProcessId -Force
    Wait-Supervisor -PreviousPid $supervisor.ProcessId
    Assert $true 'SCM restarts the crashed supervisor.'
    if ($interactive) {
        $fifth = Wait-Worker -PreviousPid $fourth.pid
        Assert ($fifth.pid -ne $fourth.pid) 'The recovered supervisor starts a new owned worker.'
    }

    Stop-Service -Name $serviceName
    Start-Sleep -Seconds 7
    Assert ((Get-CimInstance Win32_Service -Filter "Name='$serviceName'").State -eq 'Stopped') 'An intentional service stop does not trigger crash recovery.'
    Start-Service -Name $serviceName
    if ($interactive) { $last = Wait-Worker -PreviousPid $fifth.pid }
    Save-ServiceLog
    Configure '--unregister'
    Assert (-not (Get-CimInstance Win32_Service -Filter "Name='$serviceName'")) 'Unregister removes the service.'
    if ($interactive) { Assert (-not (Get-Process -Id $last.pid -ErrorAction SilentlyContinue)) 'Unregister stops the owned worker.' }
    Assert-DesktopAbsent
    Configure '--unregister'
    Assert-DesktopAbsent
    Assert (Test-Path "$fixture/kdeconnectd.exe") 'Repeated unregister preserves the user installation.'

    Set-DesktopArtifacts 'original desktop state'
    & "$fixture/kdeconnectd.exe" '--remove-legacy-shortcut' $desktopFiles[0]
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path $desktopFiles[0])) 'Migration preserves an unrelated shortcut with the same name.'
    [IO.File]::SetAttributes($desktopFiles[0], [IO.FileAttributes]::ReadOnly)
    $originalHashes = @($desktopFiles | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash })
    Configure '--register'
    Set-DesktopArtifacts 'changed by service'
    Configure '--register'
    Set-DesktopArtifacts 'changed by service update'
    Save-ServiceLog
    # Reads are allowed so restore can finish; deleting the backup must fail.
    $lockedBackup = [IO.File]::Open("$stateDirectory/0", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        & $helper '--unregister'
        Assert ($LASTEXITCODE -ne 0) 'A cleanup failure is reported instead of claiming success.'
        Assert (Test-Path $stateKey) 'Failed cleanup retains its recovery metadata.'
        Assert ((Get-ItemPropertyValue $stateKey -Name 'Removing') -eq 1) 'Cleanup keeps the daemon startup gate active until removal finishes.'
        Assert ((Get-ItemPropertyValue $stateKey -Name 'Restored') -eq 1) 'A cleanup retry can skip already-restored desktop state.'
        & $helper '--register'
        Assert ($LASTEXITCODE -ne 0) 'Registration waits for incomplete removal cleanup to finish.'
        Assert (-not (Get-CimInstance Win32_Service -Filter "Name='$serviceName'")) 'A rejected registration does not create a new service.'
    } finally {
        $lockedBackup.Dispose()
    }
    Configure '--unregister'
    foreach ($key in $notificationKeys) {
        Assert ((Get-ItemPropertyValue $key -Name 'ServiceTestValue') -eq 'original desktop state') 'Pre-existing notification registry state is restored.'
    }
    Assert ((Get-ItemPropertyValue "$($notificationKeys[0])\Nested" -Name 'Number') -eq 42) 'Registry subkeys survive restoration.'
    Assert ((Get-Item "$($notificationKeys[0])\Nested").GetValueKind('Number') -eq [Microsoft.Win32.RegistryValueKind]::DWord) 'Registry value types survive restoration.'
    for ($index = 0; $index -lt $desktopFiles.Count; $index++) {
        Assert ((Get-FileHash $desktopFiles[$index] -Algorithm SHA256).Hash -eq $originalHashes[$index]) 'Pre-existing shortcut and log contents are restored.'
    }
    Assert (([IO.File]::GetAttributes($desktopFiles[0]) -band [IO.FileAttributes]::ReadOnly) -ne 0) 'Original file attributes are restored.'
    Configure '--unregister'
    Assert ((Get-ItemPropertyValue $notificationKeys[0] -Name 'ServiceTestValue') -eq 'original desktop state') 'Repeated removal does not delete restored pre-existing state.'
    foreach ($path in $desktopFiles) { Remove-Item $path -Force }
    foreach ($key in $notificationKeys) { Remove-Item $key -Recurse -Force }

    Configure '--register'
    Set-DesktopArtifacts 'legacy service state'
    # Older installed services did not record ownership or original desktop state.
    Remove-Item $stateKey -Recurse -Force
    Remove-Item $stateDirectory -Recurse -Force
    Save-ServiceLog
    Configure '--unregister'
    Assert-DesktopAbsent

    & "$fixture/kdeconnectd.exe" '--create-shortcut' $desktopFiles[0] 'org.kde.kdeconnect.transfers'
    if ($LASTEXITCODE) { throw 'Failed to create legacy shortcut fixture' }
    Configure '--register'
    & "$fixture/kdeconnectd.exe" '--remove-legacy-shortcut' $desktopFiles[0]
    Assert ($LASTEXITCODE -eq 0 -and -not (Test-Path $desktopFiles[0])) 'Migration removes an obsolete shortcut saved before registration.'
    Save-ServiceLog
    Configure '--unregister'
    Assert-DesktopAbsent
} finally {
    Remove-Item "$fixture/pause-heartbeat" -ErrorAction SilentlyContinue
    Save-ServiceLog
    & $helper '--unregister'
    if ($LASTEXITCODE) { Write-Warning 'Service test cleanup failed' }
    Remove-Item $sentinel -ErrorAction SilentlyContinue
    foreach ($path in $desktopFiles) { Remove-Item $path -Force -ErrorAction SilentlyContinue }
    foreach ($key in $notificationKeys) { Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue }
    if ($createdConfigDirectory -and (Test-Path $configDirectory) -and [IO.Directory]::GetFileSystemEntries($configDirectory).Length -eq 0) {
        [IO.Directory]::Delete($configDirectory)
    }
    Remove-Item $fixture -Recurse -Force
}
