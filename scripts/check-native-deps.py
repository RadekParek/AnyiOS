#!/usr/bin/env python3
"""Reject unbundled shared dependencies and incorrect ELF ABIs before packaging."""

import pathlib
import re
import subprocess
import sys

# Android platform libraries, not APK-bundled third-party libraries.
# libnativewindow is public since API 26; PodNative's minimum is API 29.
PLATFORM = {
    "libandroid.so",
    "libnativewindow.so",
    "liblog.so",
    "libvulkan.so",
    "libdl.so",
    "libm.so",
    "libc.so",
    "libz.so",
}
EXPECTED = {
    "libEGL_podnative.so",
    "libGLESv1_CM_podnative.so",
    "libGLESv2_podnative.so",
}
NATIVE_GLES = {"libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so"}


def audit(folder, readelf=subprocess.check_output):
    folder = pathlib.Path(folder)
    missing = EXPECTED - {path.name for path in folder.glob("*.so")}
    if missing:
        raise ValueError(
            "Required ANGLE outputs missing: " + ", ".join(sorted(missing))
        )
    dependencies = {}
    errors = []
    for lib in sorted(folder.glob("*.so")):
        header = readelf(["readelf", "-h", str(lib)], text=True)
        if "AArch64" not in header or "ELF64" not in header:
            errors.append(f"Not ARM64: {lib.name}")
        dynamic = readelf(["readelf", "-d", str(lib)], text=True)
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
        dependencies[lib.name] = needed
        for name in needed:
            if name in NATIVE_GLES:
                errors.append(
                    f"Forbidden native-GLES substitution: {lib.name} -> {name}"
                )
            elif name not in PLATFORM and not (folder / name).is_file():
                errors.append(f"Unbundled dependency: {lib.name} -> {name}")
    if errors:
        raise ValueError("\n".join(errors))
    return dependencies


if __name__ == "__main__":
    try:
        result = audit(sys.argv[1])
    except ValueError as error:
        raise SystemExit(str(error)) from error
    for library, needed in result.items():
        print(f"{library}: {', '.join(needed)}")
    print("ANGLE ELF ABI/dependency closure audit passed")
