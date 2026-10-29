# Contributing to Chord

Read [AGENTS.md](AGENTS.md) before using AI assistance. The existing
requirements for human design, understanding and review apply to both
applications.

Keep desktop changes in `desktop/`, Android changes in `android/`, and
shared feature designs in `docs/features/`. Use one branch for a feature
that changes both applications, following the
[shared feature workflow](docs/features/README.md). The
[development guide](docs/development.md) provides build and test commands.

Both applications use the root [GitHub configuration](.github/README.md)
for CI and shared contributor templates.

The root `.clang-format` provides shared C++ formatting settings;
`desktop/.clang-format` inherits them. Keep both files in version control:
existing KDE pre-commit hooks require the root file even when the C++
project is built from `desktop/`.

The original [desktop contribution instructions](desktop/CONTRIBUTING.md)
and [Android project information](android/README.md) describe KDE Connect's
upstream conventions and contribution requirements.
