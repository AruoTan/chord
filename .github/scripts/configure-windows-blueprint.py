# SPDX-License-Identifier: CC0-1.0

"""Adapt Craft's executable allowlist and disable tray autostart for this fork."""

import ast
import os
from pathlib import Path
import sys


def configure(path):
    source = path.read_text(encoding="utf-8")
    marker = "# KDE Connect native background service packaging\n"
    original_filter = "kdeconnect-cli|kdeconnectd|"
    service_filter = "kdeconnect-cli|kdeconnect-service|kdeconnectd|"
    original_startup = r'CreateShortCut "$SMPROGRAMS\\Startup\\@{productname}.lnk" "$INSTDIR\\bin\\@{appname}.exe"'
    disabled_startup = '; Tray autostart is disabled; use --service-register.'
    if service_filter not in source:
        if source.count(original_filter) != 1:
            raise RuntimeError("Craft's executable allowlist changed; review the KDE Connect blueprint")
        source = source.replace(original_filter, service_filter)
    if original_startup in source:
        if source.count(original_startup) != 1:
            raise RuntimeError("Craft's startup shortcut changed; review the KDE Connect blueprint")
        source = source.replace(original_startup, disabled_startup)
    elif not source.startswith(marker) or source.count(disabled_startup) != 1:
        raise RuntimeError("Craft's startup shortcut is missing; review the KDE Connect blueprint")
    if not source.startswith(marker):
        source = marker + source
    ast.parse(source)
    temporary = path.with_suffix(".py.service.tmp")
    temporary.write_text(source, encoding="utf-8", newline="\n")
    temporary.replace(path)


if __name__ == "__main__":
    sys.path.insert(0, str(Path(os.environ["CraftRoot"]) / "bin"))
    from Blueprints.CraftPackageObject import CraftPackageObject

    package = CraftPackageObject.get("kde/applications/kdeconnect-kde")
    if package is None or not package.source:
        raise RuntimeError("KDE Connect Craft blueprint was not found")
    blueprint = Path(package.source)
    configure(blueprint)
    print(f"Configured native background service packaging: {blueprint}")
