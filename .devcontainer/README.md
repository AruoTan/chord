# KDE Connect development and Windows packages

The container builds and debugs the Linux version. Windows packages are built
by GitHub Actions on Windows with MSVC 2022 and KDE Craft.

## Development container

The Dockerfile extends the existing local image `aruowis-dev-base:1`, which must
provide Ubuntu 26.04 with the `vscode` user. Keep that image available on the
Docker host. In VS Code, run **Dev Containers: Rebuild Container** after changing
the configuration. This installs CMake, Ninja, GCC, GDB, Qt 6, KDE Frameworks 6,
QML modules and the project's Linux dependencies. libei 1.6.0 is built from a
verified upstream commit because Ubuntu 26.04 provides libei 1.5.

From the repository root:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --build build --parallel 2
cmake --install build
dbus-run-session -- xvfb-run -a ctest --test-dir build --output-on-failure
```

The test command provides a temporary D-Bus session and virtual X display.
Interactive GUI testing requires forwarding a desktop display separately;
network discovery and Bluetooth also depend on container access to the host's
network and hardware. The configuration does not expose host devices or ports.

MinGW is also installed to compile-check the standalone Windows service host;
this does not cross-compile the Qt/KDE application:

```sh
cmake -S daemon/windowsservice -B build-service-cross -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build-service-cross --parallel 2
```

Service registration and recovery require Windows to test. See the
[Windows service instructions](../daemon/windowsservice/README.md) for usage,
session behavior, diagnostics, and updating Scoop installations.

## Windows packages

Add these files to your own GitHub repository. The `Windows packages` workflow
can be started from the Actions tab, and also runs for pushed tags matching
`v*`. Enable Actions in a fork if necessary. It uses the checked-out source
with the current upstream Craft tool and dependency blueprints; it does not
build an unrelated upstream KDE Connect release. No signing credentials are
required.

Windows packages enable `KDECONNECT_BUILD_REDUCED_PLUGIN_SET`. The following
plugins are neither built nor installed:

| Plugin | Source directory |
| --- | --- |
| Find other devices | `findmyphone` |
| Find this device | `findthisdevice` |
| Lock Device | `lockdevice` |
| Multimedia control receiver | `mpriscontrol` |
| Send multimedia control commands | `mprisremote` |
| Pause media during calls | `pausemusic` |
| Ping | `ping` |
| Presenter | `presenter` |
| Send commands to execute | `remotecommands` |
| Send remote mouse input | `remotecontrol` |
| Send remote keyboard input | `remotekeyboard` |
| Remote volume control | `remotesystemvolume` |
| Execute remote commands | `runcommand` |
| Inhibit screensaver | `screensaver-inhibit` |
| Receive volume controls | `systemvolume` |
| Telephony integration | `telephony` |
| Virtual Monitor | `virtualmonitor` |
| Receive remote input | `mousepad` |
| Sync Contacts | `contacts` |
| Drawing tablet | `digitizer` |

Device discovery and pairing remain available. Plugin sources are retained;
the default CMake build still includes the full plugin set. To develop the
reduced build locally, add `-DKDECONNECT_BUILD_REDUCED_PLUGIN_SET=ON` to the
configure command and use a fresh build and installation directory. Installing
over a full build does not remove previously installed plugin files. The
packaging job rejects portable archives containing any excluded plugin DLLs.

The job removes external `make.exe` directories from PATH before Craft setup:
the runner's native MinGW make cannot process MSYS `/c/...` paths. Craft's own
MSYS make remains available. Downloads and dependency binary archives are
cached between runs, including failed runs. Installed Craft tools, dependency
images and libraries are cached separately after dependency installation
succeeds, before compiling the application. A hit skips bootstrap and dependency
installation. The environment cache is keyed by runner image version, Python
version and build configuration; changing application source alone reuses it.
Increase the optional `cache_revision` workflow input to force a fresh snapshot.
Snapshots omit downloads, logs, package outputs and dependency work trees, while
retaining the image directories needed for runtime packaging. Work trees are
excluded during archiving rather than deleted, preserving MSYS read-only files.
Snapshot preparation and cache upload failures do not block application builds;
only successfully prepared snapshots are uploaded. Short-path
junctions are recorded and recreated. Old runner environment snapshots are
removed and the current Python installation is detected again after restore.
Environment initialization uses Craft's Python interface rather than its
interactive PowerShell console script, so it also works without a console
cursor handle on hosted runners. The snapshot is saved before application
compilation, allowing it to be reused even if compilation or packaging fails.
The application uses sccache with GitHub's cache backend and skips compiling
test targets in the packaging job. The Linux test instructions above still
build and run the test suite. Cache hits depend on source, compiler and flags;
the first run still needs to populate the caches.

The desktop job also clears `ANDROID_SDK_ROOT` and `ANDROID_NDK`. The hosted
Windows runner defines both, and CraftBootstrap would otherwise select
`MinSizeRel` even with a Windows ABI. Bootstrap, dependency installation,
application compilation and packaging all use `RelWithDebInfo` to match KDE's
dependency binary cache and Craft's runtime image paths. This configuration
enables optimization; debug symbols are excluded from distributed packages.
MSVC uses embedded debug information (`/Z7`) so sccache can cache compilation
without a shared compiler PDB. Failure logs are collected into one directory
before uploading, because Actions artifacts cannot combine files from different
Windows drives.

Download the `kdeconnect-windows-x64-<commit>` artifact after a successful run:

- `kdeconnect-<version>-<commit>-windows-x64-setup.exe`: unsigned NSIS installer.
- `kdeconnect-<version>-<commit>-windows-x64-portable.7z`: extracted application
  tree including Qt/KDE libraries, plugins and D-Bus runtime dependencies.
- `SHA256SUMS`: SHA256 checksums for both packages.

The job extracts the archive and runs its CLI with Craft removed from PATH.
This is a packaging smoke test; GUI startup, pairing, firewall access and
installation should still be checked on a separate Windows machine.

For Scoop, optionally set `scoop_base_url` when starting the workflow to the
HTTPS directory where you will host the generated `.7z`. The artifact will
also contain `kdeconnect.json` with that URL and the archive's actual SHA256.
Upload the archive to that directory before using the manifest:

```powershell
scoop install .\kdeconnect.json
```

The workflow uploads Actions artifacts only; it does not publish GitHub
releases. Scoop manifests need a stable public download URL, not the
authenticated Actions artifact URL. The archive is install-free; configuration
and device pairing data still use KDE Connect's normal user configuration
directory rather than travelling with the application folder.
