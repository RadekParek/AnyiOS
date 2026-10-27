"""Exercise the actual Android SDL timestamp conversion with a fake sensor queue."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "third_party/anyiose-engine/vendor/SDL/src/sensor/android/SDL_androidsensor.c"
)
HARNESS = ROOT / "tests/sdl_sensor_timestamp_test.c"


class SdlSensorTimestampTests(unittest.TestCase):
    def test_android_nanoseconds_are_exposed_to_sdl_as_microseconds(self):
        if not SOURCE.is_file():
            self.skipTest("pinned SDL is not initialized in the host-only suite")
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a host C compiler is required")
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("static void\nSDL_ANDROID_SensorUpdate(")
        end = source.index("\n}\n", start) + len("\n}\n")
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / "sdl_sensor_fragment.h").write_text(
                source[start:end], encoding="utf-8"
            )
            executable = folder / "sdl-sensor-test"
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
