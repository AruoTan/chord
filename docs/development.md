# Chord development

Chord is a single Git repository for a customized desktop and Android pair.
Features follow Chord's requirements; upstream projects are reference sources
from which changes can be selected deliberately.

## Layout

```text
chord/
    .git/                    Chord history for both applications
    desktop/                 Desktop C++/Qt project
        CMakeLists.txt       Desktop build entry point
        core/, plugins/      Desktop protocol and plugins
        app/, tests/         Desktop UI and tests
        scripts/             Desktop dependency and Windows packaging scripts
        DEVELOPMENT.md       Desktop build and packaging guide
        build*/              Ignored desktop outputs
    android/                 Android Gradle project and its own build settings
        src/main/            Android application, plugins and resources
        src/test/            Android unit tests
        gradlew              Pinned Gradle wrapper
        build/               Ignored Android outputs
    docs/
        features/            Shared feature designs and protocol changes
        upstream/            Source provenance
    .github/                 Shared Linux, Windows and Android CI and templates
    .devcontainer/           Shared development container entry point
    .local/                  Ignored local backups and tool caches
    chord.code-workspace     Chord, Desktop, Android and Docs views and tasks
```

`desktop/` and `android/` are sibling projects with independent CMake and
Gradle build entry points. Both are ordinary directories tracked by Chord,
with no nested `.git` or submodules. Desktop-only source, licenses, upstream
metadata, documentation and build scripts live under `desktop/`. Root
configuration is shared infrastructure or a required service entry point.
Run Git commands from the root to review changes across both applications.
The existing `origin` still points to the Chord repository. GitHub workflows
and contributor templates are centralized in [`.github/`](../.github/README.md).
The original GitLab metadata has been converted and removed from both apps.

Open `chord.code-workspace` in VS Code, or run:

```sh
code chord.code-workspace
```

The Chord view contains root configuration; Desktop, Android and Docs have
their own views. These views all belong to the same Git repository. Build
outputs and local tool caches are excluded from workspace searches.

## Desktop

Use the [desktop build and Windows packaging guide](../desktop/DEVELOPMENT.md).
From the repository root, the Linux build commands are:

```sh
cmake -S desktop -B desktop/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD/desktop/build/install"
cmake --build desktop/build --parallel 2
dbus-run-session -- xvfb-run -a ctest --test-dir desktop/build --output-on-failure
```

The workspace provides `Desktop: configure`, `Desktop: build` and
`Desktop: test`. The test task builds first. These desktop tasks target the
Linux development container. Windows packages continue to use the existing
GitHub Actions workflow.

The previous root-level build trees were archived under
`.local/legacy-desktop-builds/` during relocation. Their CMake caches contain
the old absolute source paths. Configure a fresh `desktop/build/` tree for
the new layout; the archived trees retain previous binaries for reference.

## Android

Open `android/` as the project directory in Android Studio. Its existing
Gradle configuration remains independent of the desktop CMake build.

The Android CI uses JDK 21, matching the imported upstream CI, Android SDK platform API
37, and its checked-in Gradle wrapper. Install a suitable JDK and Android SDK
before running Android tasks. Set `JAVA_HOME` if necessary and configure the
SDK with `ANDROID_HOME` or an `sdk.dir` entry in `android/local.properties`.
Install platform-tools for `adb`, and review and accept the SDK licenses.
The existing desktop container does not install these Android tools.

From the repository root on Linux:

```sh
GRADLE_USER_HOME="$PWD/.local/gradle" ./android/gradlew -p android --no-daemon assembleDebug
GRADLE_USER_HOME="$PWD/.local/gradle" ./android/gradlew -p android --no-daemon lintDebug testDebugUnitTest
```

On Windows, run `gradlew.bat` from `android/`, or use the workspace Android
tasks. Both workspace Android tasks set `GRADLE_USER_HOME` to `.local/gradle`.
The wrapper downloads its pinned Gradle version on first use; dependency
resolution also needs network access.

Debug APKs are written to `android/build/outputs/apk/debug/`. The imported
debug application ID is `org.kde.kdeconnect_tp.debug`. Select that app when
testing the local build. `Android: build debug APK` assembles it, while
`Android: check` also runs lint and unit tests. The imported build has
`lint.abortOnError=false`, so inspect lint reports as well as the task result.

The root [Android workflow](../.github/workflows/android.yml) runs the same
Gradle checks on relevant pushes and pull requests, `v*` tags, or a manual
run. A successful run uploads a debug APK and lint/test reports to Actions.

Android build outputs, `.gradle/`, `.kotlin/` and `local.properties` are ignored
by `android/.gitignore`. Local SDKs and additional tool caches may be placed
under `.local/`. Release signing keys remain local; the imported
`debug.keystore` is the upstream development key.

## Features across both applications

Use one feature branch for both ends. Record the human-designed behavior and
packet contract in `docs/features/<feature>.md`, following the
[feature workflow](features/README.md). Change both implementations together
when the contract changes, and include the design with the implementation in
the same Chord commit when ready.

`Chord: check both` runs the desktop tests and Android checks in sequence.
After automated checks, test pairing and the feature between a real desktop
and phone. Container networking and device access must allow discovery and
communication; the existing container does not expose host devices or ports.
Record the Chord revision used on both devices and any compatibility checks
against older builds.

## Upstream references and recovery

The [Android source record](upstream/kdeconnect-android.md) identifies the
imported snapshot. Its original Git metadata and a verified full Git bundle
are saved locally under `.local/upstream/`. They are ignored and do not ship
with a fresh Chord checkout. Keep a separate backup if those local history
copies are needed long term; the tracked source record preserves provenance.

To inspect the saved Android history in a separate temporary checkout, run
from the repository root:

```sh
git clone .local/upstream/kdeconnect-android.bundle /tmp/chord-android-upstream
```

Future upstream updates should be reviewed in a separate checkout, applied
selectively to `android/`, and recorded in the source record. `git pull` at
the Chord root operates on Chord's history for both applications.
