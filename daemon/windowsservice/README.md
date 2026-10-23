# Windows background service

This fork supports a native service supervisor. It starts at system boot and
runs `kdeconnectd` in the registered user's desktop session after login. It does
not provide device connectivity before login. Locking or disconnecting that
session keeps the worker running; logging out stops it. If the same user has
multiple sessions, one worker is kept in its existing session until logoff.

## Register and remove

Stop and uninstall the old PowerShell monitor first, and exit any existing KDE
Connect tray instance. Disable any old tray login shortcut as well. In a terminal
elevated **as the user whose configuration should be used**, run the CLI from
the installed or extracted `bin` directory:

```powershell
.\kdeconnect-cli.exe --service-register
.\kdeconnect-cli.exe --service-unregister
```

Register starts the service immediately and updates an existing registration.
Unregister stops the service and its owned worker, and removes the registration.
Repeating unregister is harmless. Pairing data and the user installation are
retained. Elevating with another account registers that account instead.

The supervisor is named `KDEConnectBackground-<user SID>`. Its small standalone
executable is copied to `%ProgramFiles%\KDE Connect Background\<user SID>` with
an administrator/SYSTEM write ACL and a static C++ runtime. It loads no Qt or KDE
libraries. Only the supervisor runs as LocalSystem; the network daemon uses the
registered user's token, environment, configuration, certificates and plugins.
No account password is stored. Each registration is scoped to a user SID.

Both installer and portable builds contain the service helper. The fork's NSIS
installer removes the legacy tray login shortcut instead of creating it;
register the service explicitly to enable background startup. Registering a
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

The Windows workflow first builds this directory without Qt and exercises real
SCM registration, token/session selection, protected host installation, process
interruption, heartbeat timeout, SCM crash recovery, deliberate stop, repeat
registration, and repeat removal using `testworker.cpp`. The fixture never loads
user pairing data. On a runner without an interactive session, user-token and
worker recovery checks are explicitly skipped; SCM and installation checks
still run. To require all checks on an interactive Windows machine, run
`.github/scripts/test-windows-service.ps1 -BuildDirectory build-service -RequireInteractive`
after building this directory with MSVC in Release mode.
Packaging then builds the real daemon and checks that the
helper and CLI service options are present. Reboot, actual device pairing and
desktop clipboard behavior should also be tested on Windows with the generated
package.
