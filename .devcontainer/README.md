# Chord development container

This directory is the shared container entry point for the Chord repository.
The Dockerfile uses the local `aruowis-dev-base:latest` image and installs
Linux desktop dependencies through
[`desktop/scripts/install-dependencies.sh`](../desktop/scripts/install-dependencies.sh).
Its build context is `desktop/scripts/`.

Run **Dev Containers: Rebuild Container** in VS Code after changing this
configuration. Open [`chord.code-workspace`](../chord.code-workspace) for
Chord, Desktop, Android and Docs views and build tasks.

The [desktop guide](../desktop/DEVELOPMENT.md) covers CMake, Linux tests and
Windows packaging. The [shared development guide](../docs/development.md)
covers both applications. JDK and Android SDK installation is separate from
the current desktop dependency setup.
