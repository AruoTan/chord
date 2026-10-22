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

## Windows packages

Add these files to your own GitHub repository. The `Windows packages` workflow
can be started from the Actions tab, and also runs for pushed tags matching
`v*`. Enable Actions in a fork if necessary. It uses the checked-out source
with the current upstream Craft tool and dependency blueprints; it does not
build an unrelated upstream KDE Connect release. No signing credentials are
required.

The job removes external `make.exe` directories from PATH before Craft setup:
the runner's native MinGW make cannot process MSYS `/c/...` paths. Craft's own
MSYS make remains available. Downloads and dependency binary archives are
cached between runs, including failed runs. Craft installations and their
short-path directory junctions are rebuilt rather than restored from cache.
The application uses sccache with GitHub's cache backend and skips compiling
test targets in the packaging job. The Linux test instructions above still
build and run the test suite. Cache hits depend on source, compiler and flags;
the first run still needs to populate the caches.

The desktop job also clears `ANDROID_SDK_ROOT` and `ANDROID_NDK`. The hosted
Windows runner defines both, and CraftBootstrap would otherwise select
`MinSizeRel` even with a Windows ABI. Bootstrap must retain `RelWithDebInfo`
to use KDE's dependency binary cache; the application itself builds in
`Release`. Failure logs are collected into one directory before uploading,
because Actions artifacts cannot combine files from different Windows drives.

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
