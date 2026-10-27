"""Exercise the real Java/JNI route bridge against a synthetic Android host.

This does not replace Android/device or guest TestApp validation.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "third_party/anyiose-engine"
HARNESS = ROOT / "tests/network_bridge"
JAVA = ENGINE / "android/app/src/main/java/org/touchhle/android"


class AndroidNetworkBridgeTests(unittest.TestCase):
    def test_jni_monitor_and_lifecycle(self):
        if not sys.platform.startswith("linux"):
            self.skipTest("host JNI bridge harness currently targets Linux")
        if not (JAVA / "NetworkRouteMonitor.java").is_file():
            self.skipTest("pinned engine is not initialized")
        if not all(shutil.which(tool) for tool in ("rustc", "javac", "java")):
            self.skipTest("rustc, javac and java are needed for this host-only harness")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            library = temp / "libnetwork_test.so"
            subprocess.run(
                [
                    "rustc",
                    "--edition=2021",
                    "-Aexplicit_builtin_cfgs_in_flags",
                    "--cfg",
                    'target_os="android"',
                    "--crate-type",
                    "cdylib",
                    str(HARNESS / "jni_test.rs"),
                    "-o",
                    str(library),
                ],
                check=True,
            )
            sources = sorted(str(p) for p in HARNESS.rglob("*.java"))
            sources += [
                str(JAVA / "NetworkRouteMonitor.java"),
                str(JAVA / "NetworkRouteTracker.java"),
            ]
            subprocess.run(["javac", "-d", str(temp), *sources], check=True)
            result = subprocess.run(
                [
                    "java",
                    "-cp",
                    str(temp),
                    "org.anyiose.android.NetworkMonitorHarness",
                    str(library),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("PASS JNI network monitor", result.stdout)


if __name__ == "__main__":
    unittest.main()
