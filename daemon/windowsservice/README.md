# Windows background service

This fork supports a native service supervisor. It starts at system boot and
runs `kdeconnectd` in the registered user's desktop session after login. It does
not provide device connectivity before login. Locking or disconnecting that
session keeps the worker running; logging out stops it. If the same user has
multiple sessions, one worker is kept in its existing session until logoff.

## Register and remove

Exit any running KDE Connect instance. In a terminal elevated **as the user whose
configuration should be used**, run the CLI from the installed or extracted `bin`
directory:

```powershell
.\kdeconnect-cli.exe --service-register
.\kdeconnect-cli.exe --service-unregister
```

Register starts the service immediately and updates an existing registration.
Unregister stops the service and its owned worker, and removes the registration.
Repeating unregister is harmless. Pairing data and the user installation are
retained. Elevating with another account registers that account instead.
User-state backup and cleanup use the desktop user's permissions. Before dropping
elevation, the helper resolves that account's directories and opens its loaded
`HKEY_USERS\<SID>` hive for reading. Subsequent operations request write or delete
access only on the affected notification subkeys. It does not use
`RegOpenCurrentUser` while impersonating the user-state token. This token is a
restricted copy of the helper's own primary token: administrative groups are
disabled, administrative privileges are removed, and integrity is set to medium.
It is explicitly duplicated at `SecurityImpersonation` level instead of using a
linked UAC token, which may allow identification but fail file access with error 1346. The same
restricted path is used when UAC is disabled, including on CI runners.
Registry permissions are not changed. Configuration failures include the numeric
system error code even when the C++ runtime cannot describe it.
Registry snapshots store names, types and raw value bytes in a file, without
copying security descriptors. Restoring a pre-existing tree removes service-added
data and writes saved values into existing keys, preserving their ACLs. Keys that
must be recreated use normal inherited permissions.

The first registration records existing notification registry keys and service
runtime files. Updating or registering again keeps that original snapshot.
Unregister waits for the worker to exit,
removes service notifications, and restores the recorded state. Items created
by the service are deleted, including its AUMID/COM
registration, its Windows notification preferences, `background-status.json`,
`background.log` and `transfer-notifications.log` (including rotated copies).
The protected service host, service logs and empty service directories are also
removed. Configuration, certificates, paired devices and unrelated files remain.

The snapshot and cleanup progress are stored in
`%LOCALAPPDATA%\kdeconnect\service-state\state.bin`; original runtime files are
backed up alongside it. The v3 binary file records five runtime files:
`background.log`, `background.log.1`, `background-status.json`,
`transfer-notifications.log`, and `transfer-notifications.log.1`. It preserves
UTF-16 registry names, value types, and raw data. It is flushed and atomically replaced, and rejects
truncated, unsupported or oversized snapshots before changing system state.
Snapshots are limited to 64 MiB. This storage adds no Qt or JSON dependency to
the privileged service host.
Only the current state format is supported. Remove a service using the version
that registered it before replacing an older installation. There is no state
migration or shortcut maintenance. An existing service requires a complete
recovery snapshot; registration and removal fail if it is missing, incomplete,
corrupt, or in an unsupported format.
If cleanup fails, the command reports failure and retains recovery information;
repeat `--service-unregister` after resolving the error. A cleanup retry does not
overwrite state already restored successfully. While removal is in progress or
awaiting a retry, the daemon reads the file's startup gate and refuses startup so
D-Bus or notification activation cannot recreate the integration being removed.
Incomplete or unreadable state headers also block startup. Finish removal before
registering or starting again.

The supervisor is named `KDEConnectBackground-<user SID>`. Its small standalone
executable is copied to `%ProgramFiles%\KDE Connect Background\<user SID>` with
an administrator/SYSTEM write ACL and a static C++ runtime. It loads no Qt or KDE
libraries. Only the supervisor runs as LocalSystem; the network daemon uses the
registered user's token, environment, configuration, certificates and plugins.
No account password is stored. Each registration is scoped to a user SID.

Both installer and portable builds contain the service helper. The fork's NSIS
installer does not create a tray login shortcut; register the service explicitly
to enable background startup. Registering a
portable installation changes Windows service state and requires elevation.
Keep its files at a stable local path. Re-run register after moving or updating
the installation, including Scoop updates, to refresh the daemon path and the
protected service-host copy. Unregister before deleting the application.

## Background behavior

The worker uses `--background`: it never launches `kdeconnect-indicator` or
`kdeconnect-app`, including through pairing notifications or settings callbacks.
While a service is registered, D-Bus activation also selects background mode.
Other plugins keep their normal behavior, including desktop notifications and
opening received URLs. Users can still explicitly open a GUI themselves.

On Windows 10/11, incoming Share and Receive transfers use native Windows
notifications. One notification follows each receiving batch, showing the source,
current file, received bytes, entry count, percentage, and speed when available.
Updates are limited to twice per second and do not repeatedly show popups. During
SHA-256 verification the notification says `Verifying SHA-256...`; it reports
completion only after verification succeeds. Missing digests from older senders
remain supported and do not imply verified contents. Failures and cancellations
produce a final notification. Dismissing progress does not cancel the transfer;
progress stays dismissed, but the final result can show a new popup.

The daemon registers its notification identity directly in the current user's
registry, without creating a Start menu shortcut. Its notification COM activator is under
`HKCU\Software\Classes\CLSID\{5FA5D3F1-182B-49FD-A313-C06C5336CC70}`.
It also registers `DisplayName` and `CustomActivator` under
`HKCU\Software\Classes\AppUserModelId\org.kde.kdeconnect.transfers` and sets
the daemon's process AUMID to the same identity.
No elevation or MSIX installation is required, including for portable/Scoop
builds. Restart the daemon after moving its files to refresh the paths. Clicking
a notification does not open the app or indicator; if necessary, Windows starts
only the daemon in background mode. Windows notification settings and Do Not
Disturb control whether popups appear. Notification failures are logged and do
not stop file reception. Removing a portable installation does not remove this
per-user registration automatically. For service use, run `--service-unregister`
before removing the installation so service-created integration is cleaned up.
For manual use without a service, stop KDE Connect, then delete both registry
keys to remove its notification registration. Starting KDE
Connect manually again will register the integration it needs.

Notification initialization failures are retried during active transfers, with
a five-second delay between failed API attempts. A blocked initial notification
is retried if Windows notifications are enabled before the transfer finishes.
The daemon respects disabled notification settings; it does not change them.
For unpackaged desktop apps, `ToastNotifier.Setting()` can fail with
`0x80070490` even though toast delivery is available. That query failure is logged
once and cached until the notifier is reinitialized, avoiding repeated exceptions
on progress updates. It does not block `Show` or `Update`; Windows still applies
notification preferences when processing those calls. Other API errors retain
the retry delay.
Registration, notification settings, Show requests, and failures with HRESULTs
are written to `transfer-notifications.log` in the user configuration directory.
The log rotates at about 1 MiB. `Show accepted` records API acceptance, which
does not guarantee a popup when Do Not Disturb is active. After updating, restart
the background service or daemon before testing a new transfer. On the receiver:

```powershell
Get-Content "$env:LOCALAPPDATA\kdeconnect\transfer-notifications.log" -Encoding UTF8 -Tail 40
```

First pairing remains manual. For example, list devices and request pairing
with a known device ID, then confirm the request on the phone:

```powershell
.\kdeconnect-cli.exe --list-devices
.\kdeconnect-cli.exe --device <device-id> --pair
```

Every 10 seconds the worker reloads `[General] customDevices` and compares each
configured IP to the addresses of paired, reachable devices. Connected addresses
are excluded from its retries. Other addresses receive targeted KDE Connect UDP
discovery requests with delays of 10, 20, 40, 80 and then 120 seconds. A connection
or network change resets the corresponding retry delay. Address removal and
changes take effect without restarting. The normal LAN backend still performs
its own discovery and keepalive traffic.

ICMP Ping is not required: a firewall dropping ICMP must not prevent a valid KDE
Connect connection. The monitor uses internal device state and protocol messages;
it does not launch CLI subprocesses, parse localized output or auto-accept pairs.
The LAN backend's disabled state and `KDECONNECT_DISABLE_UDP_BROADCAST` are honored.

The worker sends a heartbeat every two seconds only after a successful async
D-Bus round trip. A missing heartbeat for 60 seconds of system awake time or a
process exit triggers recovery. Sleep and hibernation do not count as hangs.
It first requests a graceful stop, waits up to 10 seconds, and then
terminates only its owned job if necessary. Restart delays increase from five
seconds to at most two minutes, and reset after five minutes of stable running.
Phone/network outages do not trigger worker restarts. Windows SCM separately
restarts a crashed supervisor after 5, 15 and then 60 seconds. An intentional
service stop stays stopped. Closing a supervisor's job also cleans up its owned
worker tree; unrelated applications and pre-existing D-Bus processes are not
terminated.

## Diagnostics and validation

- User configuration directory: `background-status.json` is an atomic snapshot;
  check its `updated` timestamp. `background.log` rotates at about 1 MiB.
- Protected host directory: `service.log` records session starts and recovery,
  also rotating at about 1 MiB.
- `Get-Service "KDEConnectBackground-*"` reports supervisor state, which does
  not imply a phone is connected or even that the user is logged in.

The Windows workflow first builds this directory without Qt and runs
`servicestatetest.cpp` to check typed registry data, UTF-16 names, format validation,
truncated files and the startup gate header. The same format tests can run on
Linux using the two `servicestate*.cpp` sources.
It then exercises real SCM registration, token/session selection, protected host installation, process
interruption, heartbeat timeout, SCM crash recovery, deliberate stop, repeat
registration, and repeat removal using `testworker.cpp`. The fixture never loads
user pairing data. It also verifies desktop integration cleanup, restoration of
pre-existing files and registry values, preservation across repeated registration,
unnamed values, expandable strings, multi-strings, binary and 64-bit data,
file snapshot preservation and user ownership, existing ACL preservation during
restoration, removal of service-created values and nested keys, invalid or missing
state refusal, and recovery after a cleanup failure. Registry handles
are explicitly closed before removal or re-registration. The workflow uploads
both the supervisor log and a transcript of all test output, including failures.
Failed helper invocations include their merged output and decimal/hexadecimal
exit code directly in the test exception.
Cleanup failures are reported separately without replacing the original test
failure, and failure to save a log does not prevent service shutdown.
Native notification history cleanup is part of the MSVC package; the MinGW
container build checks the standalone host without the Windows SDK notification implementation.
On a runner without an interactive session, user-token and
worker recovery checks are explicitly skipped; SCM and installation checks
still run. To require all checks on an interactive Windows machine, run
`.github/scripts/test-windows-service.ps1 -BuildDirectory build-service -RequireInteractive`
after building this directory with MSVC in Release mode.
Packaging then builds the real daemon and checks that the
helper and CLI service options are present. Reboot, actual device pairing and
desktop clipboard behavior should also be tested on Windows with the generated
package.
