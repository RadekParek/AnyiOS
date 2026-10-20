#!/usr/bin/env python3
"""Conservative notice bundle; not a substitute for a distribution license audit."""

import hashlib
import pathlib
import sys

root, dest = map(pathlib.Path, sys.argv[1:])
dest.mkdir(parents=True, exist_ok=True)
index = []
for path in sorted(root.rglob("*")):
    if not path.is_file() or path.is_symlink():
        continue
    rel = path.relative_to(root)
    if any(p in {".git", "out", "node_modules"} for p in rel.parts):
        continue
    if not (
        path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE"))
        or path.name == "README.chromium"
    ):
        continue
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    (dest / (digest + ".txt")).write_bytes(data)
    index.append(f"{rel}\t{digest}.txt")
(dest / "INDEX.txt").write_text("\n".join(index) + "\n")
if not index:
    raise SystemExit("No ANGLE dependency notices found")
