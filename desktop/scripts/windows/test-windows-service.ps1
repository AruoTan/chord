# SPDX-License-Identifier: CC0-1.0
param([Parameter(Mandatory)][string]$BuildDirectory, [switch]$RequireInteractive)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
$build = (Resolve-Path $BuildDirectory).Path
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Service tests require an elevated terminal'
}
$serviceName = "KDEConnectBackground-$($identity.User.Value)"
if (Get-CimInstance Win32_Service -Filter "Name='$serviceName'") {
    throw 'Refusing to replace an existing background service during tests'
}
$interactive = [Diagnostics.Process]::GetCurrentProcess().SessionId -gt 0
if ($RequireInteractive -and -not $interactive) {
    throw 'Worker recovery tests require an interactive Windows user session'
}
$fixture = Join-Path $env:TEMP "kdeconnect-service-test-$([guid]::NewGuid())"
$helper = Join-Path $fixture 'kdeconnect-service.exe'
$hostDirectory = Join-Path (Join-Path $env:ProgramFiles 'KDE Connect Background') $identity.User.Value
$configDirectory = Join-Path $env:LOCALAPPDATA 'kdeconnect'
$stateDirectory = Join-Path $configDirectory 'service-state'
$stateFile = Join-Path $stateDirectory 'state.bin'
$notificationKeys = @(
    'HKCU:\Software\Classes\AppUserModelId\org.kde.kdeconnect.transfers',
    'HKCU:\Software\Classes\CLSID\{5FA5D3F1-182B-49FD-A313-C06C5336CC70}',
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Notifications\Settings\org.kde.kdeconnect.transfers'
)
$desktopFiles = @(
    "$configDirectory/background.log", "$configDirectory/background.log.1",
    "$configDirectory/background-status.json", "$configDirectory/transfer-notifications.log",
    "$configDirectory/transfer-notifications.log.1"
)
foreach ($path in @($notificationKeys) + @($desktopFiles) + @($stateDirectory, $hostDirectory)) {
    if (Test-Path $path) { throw "Refusing to overwrite existing desktop integration during tests: $path" }
}
$sentinel = Join-Path $configDirectory 'service-test-user-data.keep'
if (Test-Path $sentinel) { throw 'Test user-data sentinel already exists' }
New-Item -ItemType Directory $fixture | Out-Null
Copy-Item "$build/Release/kdeconnect-service.exe" $fixture
Copy-Item "$build/Release/service-test-worker.exe" "$fixture/kdeconnectd.exe"
$createdConfigDirectory = -not (Test-Path $configDirectory)
New-Item -ItemType Directory $configDirectory -Force | Out-Null
[IO.File]::WriteAllText($sentinel, 'User configuration and pairing data must be retained')
function Assert($Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    Write-Host "PASS: $Message"
}
function Configure([string]$Operation) {
    Write-Host "Service helper: $Operation"
    $output = & $helper $Operation 2>&1
    $exitCode = $LASTEXITCODE
    $details = ($output | Out-String).Trim()
    if ($details) { Write-Host $details }
    if ($exitCode) {
        if (-not $details) { $details = 'The helper produced no output.' }
        $hexCode = '{0:X8}' -f ([long]$exitCode -band 0xffffffffL)
        throw "Service helper failed: $Operation (exit code $exitCode, 0x$hexCode)`n$details"
    }
}
function Get-StateFlags {
    $bytes = [IO.File]::ReadAllBytes($stateFile)
    if ($bytes.Length -lt 48 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -ne 'KDCSTATE' -or [BitConverter]::ToUInt32($bytes, 8) -ne 3) {
        throw 'Invalid service state file header'
    }
    return [BitConverter]::ToUInt32($bytes, 12)
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
        [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($key.Substring('HKCU:\'.Length)).Dispose()
        New-ItemProperty $key -Name 'ServiceTestValue' -Value $Value -PropertyType String -Force | Out-Null
    }
    $dataKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($notificationKeys[0].Substring('HKCU:\'.Length))
    try {
        $dataKey.SetValue('', $Value, [Microsoft.Win32.RegistryValueKind]::String)
        $dataKey.SetValue('Expand', "%TEMP%\$Value", [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $dataKey.SetValue('Multiple', [string[]]@($Value, 'second entry'), [Microsoft.Win32.RegistryValueKind]::MultiString)
        [byte[]]$bytes = if ($Value -eq 'original desktop state') { 0, 1, 127, 255 } else { 42 }
        $dataKey.SetValue('Bytes', $bytes, [Microsoft.Win32.RegistryValueKind]::Binary)
        $large = if ($Value -eq 'original desktop state') { [long]4294967297 } else { [long]99 }
        $dataKey.SetValue('Large', $large, [Microsoft.Win32.RegistryValueKind]::QWord)
    } finally {
        $dataKey.Dispose()
    }
    [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($notificationKeys[0].Substring('HKCU:\'.Length) + '\Nested').Dispose()
    $number = if ($Value -eq 'original desktop state') { 42 } else { 99 }
    New-ItemProperty "$($notificationKeys[0])\Nested" -Name 'Number' -Value $number -PropertyType DWord -Force | Out-Null
    foreach ($path in $desktopFiles) {
        if (Test-Path $path) { [IO.File]::SetAttributes($path, [IO.FileAttributes]::Normal) }
        [IO.File]::WriteAllText($path, $Value)
    }
}
function Assert-DesktopAbsent {
    foreach ($path in @($notificationKeys) + @($desktopFiles) + @($stateDirectory, $hostDirectory)) {
        Assert (-not (Test-Path $path)) "Service-created state removed: $path"
    }
    Assert (Test-Path $sentinel) 'User data is retained.'
}
function Save-ServiceLog {
    if (Test-Path "$hostDirectory/service.log") {
        Get-Content "$hostDirectory/service.log" | Add-Content "$build/service-test.log"
    }
}
$testFailure = $null
try {
    Configure '--register'
    Assert ((Get-StateFlags) -band 1) 'Registration records a complete file-based desktop snapshot.'
    $stateOwner = (Get-Acl $stateFile).GetOwner([Security.Principal.SecurityIdentifier])
    Assert ($stateOwner.Value -eq $identity.User.Value) 'The restricted user owns the recovery snapshot.'
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
    }

    $supervisor = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    Stop-Process -Id $supervisor.ProcessId -Force
    Wait-Supervisor -PreviousPid $supervisor.ProcessId
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

    Configure '--register'
    $validSnapshot = [IO.File]::ReadAllBytes($stateFile)
    foreach ($invalidState in 'corrupt', 'unsupported', 'missing', 'incomplete') {
        try {
            switch ($invalidState) {
                'corrupt' { [IO.File]::WriteAllBytes($stateFile, [byte[]]@(0, 1, 2)) }
                'unsupported' {
                    $bytes = [byte[]]$validSnapshot.Clone()
                    [Array]::Copy([BitConverter]::GetBytes([uint32]2), 0, $bytes, 8, 4)
                    [IO.File]::WriteAllBytes($stateFile, $bytes)
                }
                'missing' { Remove-Item $stateFile }
                'incomplete' {
                    $bytes = [byte[]]$validSnapshot.Clone()
                    [Array]::Copy([BitConverter]::GetBytes([uint32]0), 0, $bytes, 12, 4)
                    [IO.File]::WriteAllBytes($stateFile, $bytes)
                }
            }
            foreach ($operation in '--register', '--unregister') {
                $failure = & $helper $operation 2>&1 | Out-String
                Write-Host $failure.Trim()
                Assert ($LASTEXITCODE -ne 0) "A $invalidState snapshot blocks $operation."
                Assert ((Get-CimInstance Win32_Service -Filter "Name='$serviceName'").State -eq 'Running') 'Invalid state does not stop or delete the service.'
            }
        } finally {
            [IO.File]::WriteAllBytes($stateFile, $validSnapshot)
        }
    }
    Configure '--unregister'
    Assert-DesktopAbsent

    Set-DesktopArtifacts 'original desktop state'
    [IO.File]::SetAttributes($desktopFiles[0], [IO.FileAttributes]::ReadOnly)
    $originalHashes = @($desktopFiles | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash })
    $nestedKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
        $notificationKeys[0].Substring('HKCU:\'.Length) + '\Nested',
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Security.AccessControl.RegistryRights]::FullControl)
    try {
        $acl = $nestedKey.GetAccessControl()
        $acl.SetAccessRuleProtection($true, $true)
        $nestedKey.SetAccessControl($acl)
    } finally {
        $nestedKey.Dispose()
    }
    Configure '--register'
    $originalSnapshotHash = (Get-FileHash $stateFile -Algorithm SHA256).Hash
    Set-DesktopArtifacts 'changed by service'
    New-ItemProperty $notificationKeys[0] -Name 'ServiceOnly' -Value 'remove me' -PropertyType String | Out-Null
    New-ItemProperty "$($notificationKeys[0])\Nested" -Name 'ServiceOnly' -Value 'remove me' -PropertyType String | Out-Null
    [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($notificationKeys[0].Substring('HKCU:\'.Length) + '\ServiceOnly\Leaf').Dispose()
    Configure '--register'
    Assert ((Get-FileHash $stateFile -Algorithm SHA256).Hash -eq $originalSnapshotHash) 'Repeated registration preserves the original file snapshot.'
    Set-DesktopArtifacts 'changed by service update'
    Save-ServiceLog
    # Reads are allowed so restore can finish; deleting the backup must fail.
    $lockedBackup = [IO.File]::Open("$stateDirectory/0", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        & $helper '--unregister'
        Assert ($LASTEXITCODE -ne 0) 'A cleanup failure is reported instead of claiming success.'
        Assert (Test-Path $stateFile) 'Failed cleanup retains its recovery metadata.'
        Assert ((Get-StateFlags) -band 4) 'Cleanup keeps the daemon startup gate active until removal finishes.'
        Assert ((Get-StateFlags) -band 8) 'A cleanup retry can skip already-restored desktop state.'
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
    $nestedKey = Get-Item "$($notificationKeys[0])\Nested"
    try {
        Assert ($nestedKey.GetValueKind('Number') -eq [Microsoft.Win32.RegistryValueKind]::DWord) 'Registry value types survive restoration.'
        Assert ($nestedKey.GetAccessControl().AreAccessRulesProtected) 'Restoration preserves the original subkey ACL protection.'
        Assert ($null -eq $nestedKey.GetValue('ServiceOnly')) 'Restoration removes service-created values from an existing subkey.'
    } finally {
        $nestedKey.Dispose()
    }
    $dataKey = Get-Item $notificationKeys[0]
    try {
        Assert ($dataKey.GetValue('') -eq 'original desktop state') 'The unnamed registry value is restored.'
        Assert ($dataKey.GetValue('Expand', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -eq '%TEMP%\original desktop state') 'Expandable registry strings retain their raw contents.'
        Assert (($dataKey.GetValue('Multiple') -join '|') -eq 'original desktop state|second entry') 'Multi-string registry values are restored.'
        Assert (($dataKey.GetValue('Bytes') -join ',') -eq '0,1,127,255') 'Binary registry values are restored.'
        Assert ($dataKey.GetValue('Large') -eq [long]4294967297) '64-bit registry values are restored without truncation.'
        foreach ($entry in @{ Expand='ExpandString'; Multiple='MultiString'; Bytes='Binary'; Large='QWord' }.GetEnumerator()) {
            Assert ($dataKey.GetValueKind($entry.Key).ToString() -eq $entry.Value) "Registry type restored: $($entry.Key)"
        }
        Assert ($null -eq $dataKey.GetValue('ServiceOnly')) 'Restoration removes service-created root values.'
    } finally {
        $dataKey.Dispose()
    }
    Assert (-not (Test-Path "$($notificationKeys[0])\ServiceOnly")) 'Restoration removes service-created subkey trees.'
    for ($index = 0; $index -lt $desktopFiles.Count; $index++) {
        Assert ((Get-FileHash $desktopFiles[$index] -Algorithm SHA256).Hash -eq $originalHashes[$index]) 'Pre-existing runtime file contents are restored.'
    }
    Assert (([IO.File]::GetAttributes($desktopFiles[0]) -band [IO.FileAttributes]::ReadOnly) -ne 0) 'Original file attributes are restored.'
    Configure '--unregister'
    Assert ((Get-ItemPropertyValue $notificationKeys[0] -Name 'ServiceTestValue') -eq 'original desktop state') 'Repeated removal does not delete restored pre-existing state.'
    foreach ($path in $desktopFiles) { Remove-Item $path -Force }
    foreach ($key in $notificationKeys) { Remove-Item $key -Recurse -Force }
    Assert-DesktopAbsent
} catch {
    $testFailure = $_
    throw
} finally {
    try {
        Remove-Item "$fixture/pause-heartbeat" -ErrorAction SilentlyContinue
        try { Save-ServiceLog } catch { Write-Warning "Failed to save service diagnostics: $_" }
        Configure '--unregister'
        Remove-Item $sentinel -ErrorAction SilentlyContinue
        foreach ($path in $desktopFiles) { Remove-Item $path -Force -ErrorAction SilentlyContinue }
        foreach ($key in $notificationKeys) { Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue }
        if ($createdConfigDirectory -and (Test-Path $configDirectory) -and [IO.Directory]::GetFileSystemEntries($configDirectory).Length -eq 0) {
            [IO.Directory]::Delete($configDirectory)
        }
        Remove-Item $fixture -Recurse -Force
    } catch {
        if ($null -eq $testFailure) { throw }
        Write-Warning "Service test cleanup also failed: $_"
    }
}
