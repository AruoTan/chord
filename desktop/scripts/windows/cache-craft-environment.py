# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None
"""Save dependency environments without runner credentials; restore junctions."""

import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def manage_cache(action, root, archive):
    root = root.resolve()
    tar = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/usr/bin/tar.exe"
    if not tar.is_file():
        tar = Path(shutil.which("tar") or "tar")
    if action == "restore":
        root.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(tar), "--force-local", "-xf", archive.as_posix(), "-C", root.as_posix()], check=True)
    settings_path = root / "etc/CraftSettings.ini"
    settings = configparser.ConfigParser(interpolation=None)
    settings.optionxform = str
    if not settings.read(settings_path):
        raise RuntimeError("Craft settings missing from dependency environment")
    if settings["General"]["ABI"] != "windows-cl-msvc2022-x86_64":
        raise RuntimeError("Cached Craft ABI does not match Windows x64 MSVC 2022")
    if settings["Compile"]["BuildType"] != "RelWithDebInfo":
        raise RuntimeError("Unexpected dependency environment build type")
    short_root = Path(settings["ShortPath"]["JunctionDir"])
    manifest_path = root / "etc/environment-junctions.json"

    # PristineCraftenv contains temporary paths and GitHub runtime credentials.
    for name in ("PristineCraftenv.json", "cache.pickle"):
        (root / "etc" / name).unlink(missing_ok=True)

    if action == "save":
        junctions = {}
        if short_root.exists():
            for entry in short_root.iterdir():
                if entry.is_junction():
                    target = entry.resolve()
                    if target.is_relative_to(root):
                        junctions[entry.name] = target.relative_to(root).as_posix()
        manifest_path.write_text(json.dumps(junctions), encoding="utf-8")
        # Exclude work trees without deleting MSYS files with read-only ACLs.
        work_excludes = []
        for directory, children, _ in os.walk(root / "build"):
            if "work" in children:
                relative_work = (Path(directory) / "work").relative_to(root).as_posix()
                work_excludes.append(f"--exclude=./{relative_work}")
                children.remove("work")
        archive.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            str(tar), "--force-local", "--use-compress-program=zstd -T0 -1",
            "-cf", archive.as_posix(), "-C", root.as_posix(),
            "--exclude=./download", "--exclude=./tmp", "--exclude=./logs",
            "--exclude=./msys/var/cache/pacman/pkg", *work_excludes, ".",
        ], check=True)
    else:
        junctions = json.loads(manifest_path.read_text(encoding="utf-8"))
        short_root.mkdir(parents=True, exist_ok=True)
        for name, relative_target in junctions.items():
            link = short_root / name
            target = (root / relative_target).resolve()
            if link.parent != short_root or not target.is_relative_to(root):
                raise RuntimeError("Invalid path in cached junction manifest")
            target.mkdir(parents=True, exist_ok=True)
            if link.is_junction() and link.resolve() == target:
                continue
            subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(link), str(target)], check=True)
        # setup-python may use a different installation path on a new runner.
        settings["Paths"]["Python"] = str(Path(sys.executable).parent)
        with settings_path.open("w", encoding="utf-8") as output:
            settings.write(output)
        archive.unlink()
    print(f"Craft environment {action}: {len(junctions)} junctions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("save", "restore"))
    args = parser.parse_args()
    manage_cache(args.action, Path(os.environ["CRAFT_ROOT"]), Path(os.environ["CRAFT_SNAPSHOT_FILE"]))
