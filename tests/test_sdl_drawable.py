"""Exercise the actual Android SDL drawable-size function with a fake EGL driver.

The real GPU, Android lifecycle, and touch alignment still require device tests.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "third_party/anyiose-engine/vendor/SDL/src/video/android/SDL_androidgl.c"
)
HARNESS = ROOT / "tests/sdl_drawable_test.c"


class SdlDrawableTests(unittest.TestCase):
    def test_loaded_egl_dimensions_and_lifecycle_fallbacks(self):
        if not SOURCE.is_file():
            self.skipTest("pinned SDL is not initialized in the host-only suite")
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a host C compiler is required")
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("void\nAndroid_GLES_GetDrawableSize(")
        end = source.index("\n}\n", start) + len("\n}\n")
        fragment = source[start:end]
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / "sdl_drawable_fragment.h").write_text(fragment, encoding="utf-8")
            executable = folder / "sdl-drawable-test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(folder),
                    str(HARNESS),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
