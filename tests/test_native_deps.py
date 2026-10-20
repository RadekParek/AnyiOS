"""Dependency auditor unit tests only; mocks are never packaged as ANGLE."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "native_audit",
    Path(__file__).resolve().parent.parent / "scripts/check-native-deps.py",
)
auditor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(auditor)


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        for name in auditor.EXPECTED:
            (self.folder / name).touch()
        self.abi = "Class: ELF64\nMachine: AArch64"
        self.dependencies = ["libnativewindow.so", "libc.so"]

    def tearDown(self):
        self.temp.cleanup()

    def readelf(self, command, text):
        self.assertTrue(text)
        if command[1] == "-h":
            return self.abi
        return "\n".join(
            f"(NEEDED) Shared library: [{name}]" for name in self.dependencies
        )

    def test_android_nativewindow_is_platform_dependency(self):
        result = auditor.audit(self.folder, self.readelf)
        self.assertEqual(set(result), auditor.EXPECTED)

    def test_unknown_driver_not_whitelisted(self):
        self.dependencies.append("libVendorGPU_private.so")
        with self.assertRaisesRegex(ValueError, "Unbundled dependency"):
            auditor.audit(self.folder, self.readelf)

    def test_native_gles_rejected_even_if_bundled(self):
        (self.folder / "libGLESv2.so").touch()
        self.dependencies.append("libGLESv2.so")
        with self.assertRaisesRegex(ValueError, "Forbidden native-GLES substitution"):
            auditor.audit(self.folder, self.readelf)

    def test_wrong_abi_rejected(self):
        self.abi = "Class: ELF64\nMachine: X86-64"
        with self.assertRaisesRegex(ValueError, "Not ARM64"):
            auditor.audit(self.folder, self.readelf)

    def test_missing_outputs_rejected(self):
        for file in self.folder.iterdir():
            file.unlink()
        with self.assertRaisesRegex(ValueError, "Required ANGLE outputs missing"):
            auditor.audit(self.folder, self.readelf)


if __name__ == "__main__":
    unittest.main()
