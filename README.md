# Chord - desktop and Android

Chord develops a customized desktop and Android pair in one Git repository,
based on KDE Connect.

| Directory | Contents |
| --- | --- |
| [`desktop/`](desktop/) | Desktop C++/Qt application, CMake build, tests, licenses and packaging scripts |
| [`android/`](android/) | Android Kotlin/Java application, Gradle build, tests and licenses |
| [`docs/`](docs/) | Shared development workflow, feature designs and upstream source records |
| [`.devcontainer/`](.devcontainer/) | Shared development container configuration |
| [`.github/`](.github/README.md) | Shared GitHub Actions workflows and pull request templates |

Open [`chord.code-workspace`](chord.code-workspace) for Chord, Desktop, Android
and Docs views with build and check tasks. Both applications are ordinary
directories managed by the root `.git`, with no nested working repositories
or submodules.

See the [development guide](docs/development.md),
[desktop build guide](desktop/DEVELOPMENT.md) and
[shared feature workflow](docs/features/README.md).
The Android source import is recorded in
[its source record](docs/upstream/kdeconnect-android.md).

Each application's original license files are retained in
[`desktop/LICENSES/`](desktop/LICENSES/) and
[`android/LICENSES/`](android/LICENSES/). See the
[contribution instructions](CONTRIBUTING.md) before making changes.
