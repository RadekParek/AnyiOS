#!/usr/bin/env python3
"""Expose unchanged, pinned Boost module headers to CMake's FindBoost."""

from pathlib import Path
import os
import shutil
import sys

root, destination = (Path(value).resolve() for value in sys.argv[1:])
count = 0
for include in root.glob("libs/**/include/boost"):
    for source in include.rglob("*"):
        if not source.is_file():
            continue
        target = destination / source.relative_to(include)
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink():
            if target.resolve() != source.resolve():
                raise SystemExit(f"Conflicting Boost header: {target}")
        elif target.exists():
            # Windows runners may lack permission to create symlinks. Copies are
            # acceptable here: these pinned headers are immutable build inputs.
            if (
                os.name != "nt"
                or not target.is_file()
                or target.read_bytes() != source.read_bytes()
            ):
                raise SystemExit(f"Refusing to replace existing file: {target}")
        elif os.name == "nt":
            shutil.copyfile(source, target)
        else:
            target.symlink_to(source)
        count += 1
print(f"Exposed {count} pinned Boost headers (no source modifications)")
