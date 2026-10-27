import hashlib
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from radek.gamepack import publish_resource_expansion
from radek.pipeline import Pipeline


class ExpansionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = "dev.radek.converted.p" + "a" * 20

    def test_resource_only_zip_uses_manifest_package_and_preserves_contents(self):
        resources = {"Info.plist": b"plist", "media/picture.bin": b"\0\x01\x02"}
        result = publish_resource_expansion(self.root, self.package, resources)
        obb = self.root / result["name"]
        self.assertEqual(result["name"], f"main.1.{self.package}.obb")
        self.assertEqual(result["sha256"], hashlib.sha256(obb.read_bytes()).hexdigest())
        self.assertEqual(result["bytes"], obb.stat().st_size)
        self.assertFalse(result["required"])
        self.assertTrue(result["alsoEmbeddedInApk"])
        self.assertEqual(result["installPath"], f"Android/obb/{self.package}/{result['name']}")
        with zipfile.ZipFile(obb) as archive:
            self.assertEqual(set(archive.namelist()), set(resources))
            for name, contents in resources.items():
                self.assertEqual(archive.read(name), contents)
                self.assertEqual(archive.getinfo(name).compress_type, zipfile.ZIP_STORED)

    def test_invalid_path_does_not_replace_previous_or_leave_pending_files(self):
        previous = self.root / f"main.1.{self.package}.obb"
        previous.write_bytes(b"previous")
        with self.assertRaises(ValueError):
            publish_resource_expansion(self.root, self.package, {"../escape": b"bad"})
        self.assertEqual(previous.read_bytes(), b"previous")
        self.assertEqual(list(self.root.glob("*.pending")), [])
        with self.assertRaises(ValueError):
            publish_resource_expansion(self.root, "../bad", {"ok": b"x"})

    def test_disk_headroom_is_checked_before_touching_old_expansion(self):
        previous = self.root / f"main.1.{self.package}.obb"
        previous.write_bytes(b"previous")
        with patch("radek.gamepack.require_free_space", side_effect=ValueError("disk full")):
            with self.assertRaises(ValueError):
                publish_resource_expansion(self.root, self.package, {"resource": b"data"})
        self.assertEqual(previous.read_bytes(), b"previous")
        self.assertEqual(list(self.root.glob("*.pending")), [])

    def test_unavailable_expansion_does_not_invalidate_verified_apk(self):
        pipeline = Pipeline(self.root / "job")
        pipeline.report["state"] = "ANALYZING"
        pipeline.report["application"] = {"sha256": "a" * 64, "name": "fixture"}
        pipeline.report["nativeCodeArtifact"] = {}
        pipeline.report["staticRecompilationAssessment"] = {}
        pipeline.report["portProgress"] = {}
        program = SimpleNamespace(
            target_abi="arm64-v8a", machine_code=b"abcd", output_architecture="arm64", source_size=4
        )
        assessment = {"resourceInventory": [], "resourcePayloads": {}, "launchMessage": "hello"}
        with (
            patch("radek.pipeline.Toolchain.discover", return_value=object()),
            patch("radek.pipeline.build_complete_game", return_value={"sha256": "b" * 64, "sizeBytes": 10}),
            patch("radek.pipeline.validate_complete_game", return_value={"status": "PASSED"}),
            patch("radek.pipeline.publish_resource_expansion", side_effect=OSError("disk full")),
        ):
            report = pipeline._attempt_complete_conversion(self.root, Path("test.ipa"), program, {}, assessment)
        self.assertEqual(report["state"], "READY")
        self.assertEqual(report["completeConversion"]["status"], "COMPLETE")
        self.assertEqual(report["completeConversion"]["expansionObbStatus"]["status"], "NOT_CREATED")
        self.assertIn("disk full", report["completeConversion"]["expansionObbStatus"]["message"])

    def test_report_failure_after_optional_copy_keeps_persisted_ready_report(self):
        pipeline = Pipeline(self.root / "job")
        pipeline.report.update({
            "state": "ANALYZING",
            "application": {"sha256": "a" * 64, "name": "fixture"},
            "nativeCodeArtifact": {},
            "staticRecompilationAssessment": {},
            "portProgress": {},
        })
        original_save = pipeline.save

        def fail_only_after_expansion(force=False):
            if "expansionObb" in pipeline.report.get("completeConversion", {}):
                raise OSError("disk full")
            return original_save(force)

        program = SimpleNamespace(
            target_abi="arm64-v8a", machine_code=b"abcd", output_architecture="arm64", source_size=4
        )
        assessment = {"resourceInventory": [], "resourcePayloads": {}, "launchMessage": "hello"}
        with (
            patch("radek.pipeline.Toolchain.discover", return_value=object()),
            patch("radek.pipeline.build_complete_game", return_value={"sha256": "b" * 64, "sizeBytes": 10}),
            patch("radek.pipeline.validate_complete_game", return_value={"status": "PASSED"}),
            patch("radek.pipeline.publish_resource_expansion", return_value={"name": "optional.obb"}),
            patch.object(pipeline, "save", side_effect=fail_only_after_expansion),
        ):
            report = pipeline._attempt_complete_conversion(self.root, Path("test.ipa"), program, {}, assessment)
        persisted = json.loads((self.root / "job/report.json").read_text())
        self.assertEqual(report["state"], "READY")
        self.assertEqual(persisted["state"], "READY")
        self.assertEqual(persisted["completeConversion"]["status"], "COMPLETE")
        self.assertNotIn("expansionObb", persisted["completeConversion"])


if __name__ == "__main__":
    unittest.main()
