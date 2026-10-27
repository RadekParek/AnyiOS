"""Contract tests for the optional Ghidra runner; no proprietary IPA required."""
from pathlib import Path
import os
import tempfile
import unittest
from unittest import mock

from radek.ghidra import inspect, MAX_INPUT_BYTES


class GhidraTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.exe = self.root / "guest"
        self.exe.write_bytes(b"not executable guest data")
        self.out = self.root / "out"
        self.out.mkdir()
        self.mach = {"slices": [{"encrypted": False}]}
        self.home = self.root / "ghidra"
        self.launcher = self.home / "support/analyzeHeadless"
        self.launcher.parent.mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def test_not_requested_and_missing_install_are_truthful(self):
        self.assertEqual(inspect(self.exe, self.mach, self.out, None)["status"], "NOT_REQUESTED")
        report = inspect(self.exe, self.mach, self.out, self.home)
        self.assertEqual(report["status"], "UNAVAILABLE")
        self.assertFalse(report["attempted"])
        self.assertFalse(report["completeGameConversion"])

    @unittest.skipIf(os.name == "nt", "test launcher is a POSIX script")
    def test_encryption_never_launches_ghidra(self):
        self.launcher.write_text("#!/bin/sh\ntouch '$HOME/should-not-run'\nexit 0\n")
        self.launcher.chmod(0o755)
        with mock.patch("radek.ghidra.subprocess.Popen", side_effect=AssertionError("must not launch")):
            report = inspect(self.exe, {"slices": [{"encrypted": True}]}, self.out, self.home)
        self.assertEqual(report["status"], "SKIPPED_ENCRYPTED")
        self.assertFalse(report["attempted"])

    @unittest.skipIf(os.name == "nt", "test launcher is a POSIX script")
    def test_valid_bounded_sample_is_distinct_from_conversion(self):
        # The postScript output is the final argument before -deleteProject.
        self.launcher.write_text(
            "#!/bin/sh\n"
            'while [ "$1" != "-postScript" ]; do shift; done\n'
            'shift; shift; output="$1"\n'
            'printf "/* ANYIOS_GHIDRA_REPORT_V1\\n/* guessed */\\n'
            '/* ANYIOS_RESULT attempted=2 completed=1 limited=1 analysisTimedOut=1 */\\n" > "$output"\n'
        )
        self.launcher.chmod(0o755)
        result = inspect(self.exe, self.mach, self.out, self.home)
        self.assertEqual(result["status"], "EXPORTED_SAMPLE")
        self.assertEqual(result["functionsDecompiled"], 1)
        self.assertTrue(result["analysisTimedOut"])
        self.assertTrue(result["truncated"])
        self.assertFalse(result["completeGameConversion"])
        self.assertTrue((self.out / result["path"]).is_file())
        self.assertFalse(list(self.out.glob("ghidra-project-*")))

    @unittest.skipIf(os.name == "nt", "test launcher is a POSIX script")
    def test_partial_or_failed_output_is_discarded(self):
        self.launcher.write_text(
            "#!/bin/sh\n"
            'while [ "$1" != "-postScript" ]; do shift; done\n'
            'shift; shift; echo "/* incomplete" > "$1"\n'
        )
        self.launcher.chmod(0o755)
        self.assertEqual(inspect(self.exe, self.mach, self.out, self.home)["status"], "FAILED")
        self.assertFalse((self.out / "ghidra-inferred.c").exists())

    @unittest.skipIf(os.name == "nt", "test launcher is a POSIX script")
    def test_large_executable_is_skipped(self):
        self.launcher.write_text("#!/bin/sh\nexit 0\n")
        self.launcher.chmod(0o755)
        with self.exe.open("wb") as handle:
            handle.truncate(MAX_INPUT_BYTES + 1)
        report = inspect(self.exe, self.mach, self.out, self.home)
        self.assertEqual(report["status"], "SKIPPED_TOO_LARGE")
        self.assertFalse(report["attempted"])

    @unittest.skipIf(os.name == "nt", "test launcher is a POSIX script")
    def test_timeout_kills_process_group_and_discards_partial_output(self):
        self.launcher.write_text("#!/bin/sh\nsleep 30\n")
        self.launcher.chmod(0o755)
        with mock.patch("radek.ghidra.TIMEOUT_SECONDS", 0.05):
            report = inspect(self.exe, self.mach, self.out, self.home)
        self.assertEqual(report["status"], "TIMED_OUT")
        self.assertTrue(report["attempted"])
        self.assertFalse((self.out / "ghidra-inferred.c").exists())
        self.assertFalse(list(self.out.glob("ghidra-project-*")))


if __name__ == "__main__":
    unittest.main()
