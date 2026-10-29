# Chord GitHub configuration

GitHub automation and pull request templates for both applications live in
this root directory. Application sources and platform-specific scripts stay
in `desktop/` and `android/`.

## Workflows

| Workflow | Purpose | Automatic triggers |
| --- | --- | --- |
| [desktop-linux.yml](workflows/desktop-linux.yml) | Install desktop dependencies on Ubuntu 26.04, build with CMake, run Linux tests and upload diagnostics | Relevant pushes, pull requests and `v*` tags |
| [desktop-windows.yml](workflows/desktop-windows.yml) | Run Windows service tests, package the desktop application with Craft, and upload installers and portable archives | `v*` tags |
| [android.yml](workflows/android.yml) | Set up JDK 21, build a debug APK, run lint and unit tests, and upload APKs and reports | Relevant pushes, pull requests and `v*` tags |

All workflows also support **Run workflow** in the Actions tab. Linux and
Android path filters include their own project, their workflow file and
`docs/features/`, so shared feature contracts trigger both checks. Workflow
commands run from their application's directory; artifact paths are relative
to the repository root.

The Linux workflow runs in an Ubuntu 26.04 container on the Ubuntu 24.04
hosted runner. It installs Git before checkout and reuses
`desktop/scripts/install-dependencies.sh` as root. The Android workflow
reuses the original Gradle checks and uses the hosted
runner's Android SDK. After SDK license acceptance, Gradle can download any
missing platform or build-tools packages. Gradle dependencies are cached
using the Android build configuration as the cache key input. The inherited
Android `lint.abortOnError=false` setting remains, so review lint reports.

The Windows workflow was renamed from `windows-packages.yml`; its existing
manual inputs, tag trigger, dependency caches and packaging behavior remain.
It uploads Actions artifacts and does not publish releases.

These workflows cover Chord's Linux desktop tests, Windows packages and
Android debug build. The former desktop GitLab configuration included KDE's
centrally managed formatting/XML/YAML jobs, Qt-next, FreeBSD and macOS signing
jobs. Those KDE-specific jobs are not replicated by these workflows. The
`.kde-ci.yml` dependency metadata is replaced for the supported Linux build
by the existing desktop dependency installer.

## Shared pull request templates

The former Android and desktop GitLab templates differed only in the
desktop AI usage section. The shared GitHub versions retain that section:

- [Bugfix.md](PULL_REQUEST_TEMPLATE/Bugfix.md)
- [Feature.md](PULL_REQUEST_TEMPLATE/Feature.md)

Select one by adding `template=Bugfix.md` or `template=Feature.md` to the
query parameters when opening a pull request. GitHub does not run the old
GitLab `/label` commands; apply any desired labels through GitHub. These
files are blank contributor templates; contributors write their own pull
request descriptions, following [AGENTS.md](../AGENTS.md).

## Original GitLab metadata

Both applications' `.gitlab/` and `.gitlab-ci.yml`, plus the desktop
`.kde-ci.yml`, were archived locally in `.local/legacy-gitlab/` before
removal. The archive contains a SHA-256 manifest. It is ignored and is not
included in fresh checkouts. Original sources remain identifiable through
Chord's Git history and the [Android source record](../docs/upstream/kdeconnect-android.md).

GitHub references: [workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax),
[pull request templates](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/creating-a-pull-request-template-for-your-repository),
[hosted runner images](https://github.com/actions/runner-images), and
[SDK package downloads through Gradle](https://developer.android.com/studio/intro/update#download-with-gradle).
