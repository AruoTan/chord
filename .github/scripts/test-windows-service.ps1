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
try {
    Configure '--register'
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
    Configure '--unregister'
    Assert (-not (Get-CimInstance Win32_Service -Filter "Name='$serviceName'")) 'Unregister removes the service.'
    if ($interactive) { Assert (-not (Get-Process -Id $last.pid -ErrorAction SilentlyContinue)) 'Unregister stops the owned worker.' }
    Configure '--unregister'
    Assert (Test-Path "$fixture/kdeconnectd.exe") 'Repeated unregister preserves the user installation.'
} finally {
    Remove-Item "$fixture/pause-heartbeat" -ErrorAction SilentlyContinue
    if (Test-Path "$hostDirectory/service.log") {
        Copy-Item "$hostDirectory/service.log" "$build/service-test.log" -Force
    }
    & $helper '--unregister'
    if ($LASTEXITCODE) { Write-Warning 'Service test cleanup failed' }
    Remove-Item $fixture -Recurse -Force
}
