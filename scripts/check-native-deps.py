#!/usr/bin/env python3
"""Reject unbundled shared dependencies and incorrect ELF ABIs before packaging."""

import pathlib
import re
import subprocess
import sys

folder = pathlib.Path(sys.argv[1])
allowed = {
    "libandroid.so",
    "liblog.so",
    "libvulkan.so",
    "libdl.so",
    "libm.so",
    "libc.so",
    "libz.so",
}
libs = list(folder.glob("*.so"))
if not libs:
    raise SystemExit("No real ANGLE libraries supplied")
for lib in libs:
    header = subprocess.check_output(["readelf", "-h", str(lib)], text=True)
    if "AArch64" not in header or "ELF64" not in header:
        raise SystemExit(f"Not ARM64: {lib}")
    dynamic = subprocess.check_output(["readelf", "-d", str(lib)], text=True)
    for needed in re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic):
        if needed not in allowed and not (folder / needed).is_file():
            raise SystemExit(f"Unbundled dependency: {lib.name} -> {needed}")
print("ANGLE ELF ABI/dependency closure audit passed")
