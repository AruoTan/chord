# KDE Connect Android source record

| Field | Imported value |
| --- | --- |
| Repository | https://invent.kde.org/network/kdeconnect-android.git |
| Branch | `master` |
| Commit | `353361ed55ab5ccb444c9d00a74b9bab97eff43e` |
| Commit date (UTC) | `2026-10-24T02:19:37Z` |
| Chord import date | `2026-10-29` |
| Destination | `android/` |
| Imported tracked files | 678 |
| Application version | `1.37.0 Beta 3` (`13692`) |

The import preserves the tracked upstream source, licenses, Gradle wrapper,
build configuration and file executable modes. Android development after
this import is tracked by Chord's root `.git`. No Android submodule or nested
working repository is used.

Before removing the nested Git directory, a complete bundle of the original
clone's refs was created and verified. The original Git metadata was moved
to `.local/upstream/kdeconnect-android.git`; the bundle is
`.local/upstream/kdeconnect-android.bundle`. Both are local ignored backups.
The bundle SHA-256 at import is:

```text
08963e738ce447c4ca164194abfb23fb5bdeb2c8d41f403c9a7d6c43a34f4539
```

Future selected upstream updates should record their source commits here.

## Chord CI conversion

The imported `.gitlab-ci.yml` checks were migrated to the root
[Android GitHub workflow](../../.github/workflows/android.yml). Android and
desktop contributor templates are shared under
[`.github/PULL_REQUEST_TEMPLATE/`](../../.github/PULL_REQUEST_TEMPLATE/).
The original Android `.gitlab/` and `.gitlab-ci.yml` were backed up in
`.local/legacy-gitlab/android/` and removed from `android/`. Application
sources, Gradle settings and licenses remain as imported.
