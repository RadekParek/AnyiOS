"""CMake + NDK link tests; real Android builds are never simulated as success."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from dataclasses import replace
from pathlib import Path
from unittest import mock

from radek.game import lsda, macho
from radek.game.android_linker import (
    link_generated_translation,
    prepare_translated_game_runtime_input,
)
from .fixtures import classic_game_macho_without_optional_sections, ipa

ROOT = Path(__file__).resolve().parents[1]
NDK = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
REQUIRE_REAL = os.environ.get("ANYIOS_REQUIRE_REAL_NDK_CMAKE") == "1"


class OptionalSectionsTests(unittest.TestCase):
    def test_optional_sections_can_be_absent_but_required_text_stays_strict(self):
        image = macho.parse(classic_game_macho_without_optional_sections())
        self.assertEqual(macho.stub_map(image), [])
        self.assertEqual(macho.non_lazy_map(image), [])
        self.assertEqual(lsda.parse_all(image), ({}, [], []))
        with self.assertRaises(macho.MachOError):
            image.section_named("__DATA", "__required_missing")
        image.symbols[0].name = "GCC_except_table_missing"
        with self.assertRaisesRegex(macho.MachOError, "exception table symbols"):
            lsda.parse_all(image)
        stub = replace(
            image.sections[0],
            segment="__TEXT",
            name="__symbol_stub4",
            size=12,
            reserved2=12,
        )
        image.sections.append(stub)
        with self.assertRaisesRegex(macho.MachOError, "__la_symbol_ptr"):
            macho.stub_map(image)

    def test_codegen_lifts_classic_fixture_without_optional_sections(self):
        try:
            from radek.game.codegen import generate
        except ImportError:
            self.skipTest("capstone is not installed")
        with tempfile.TemporaryDirectory(prefix="anyios reconstruction ") as directory:
            root = Path(directory)
            source = ipa(
                root / "sample.ipa",
                executable=classic_game_macho_without_optional_sections(),
            )
            result = generate(str(source), str(root / "translated"))
            self.assertEqual(result["functions"], 1)
            self.assertEqual(result["functionFailures"], 0)
            self.assertEqual(result["translatedUniqueTextBytes"], 8)
            self.assertFalse(result["linkedIntoGame"])
            self.assertFalse(result["apkProduced"])
            self.assertTrue((root / "translated/rt_mem.bin").is_file())

    def test_lifter_bounds_plist_before_decompression(self):
        try:
            from radek.game.codegen import load_ipa
        except ImportError:
            self.skipTest("capstone is not installed")

        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "large-plist.ipa"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("Payload/Game.app/Info.plist", b"X" * (8 * 1024 * 1024 + 1))
            with self.assertRaisesRegex(ValueError, "Info.plist exceeds"):
                load_ipa(str(archive))

    def test_cli_requires_permission_and_leaves_files_alone(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sentinel = root / "sentinel"
            sentinel.write_text("unchanged", encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-m", "radek", "relink", str(root)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("authorization", completed.stderr)
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["sentinel"])

    def test_missing_toolchain_and_cmake_are_explicit_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("game_all.c", "rt_gen.c", "rt_gen.h"):
                (root / name).write_text("/* fixture */", encoding="utf-8")
            (root / "rt_report.json").write_text("{}", encoding="utf-8")
            compiler = root / "ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
            compiler.parent.mkdir(parents=True)
            compiler.write_text("fixture", encoding="utf-8")
            sysroot = compiler.parent.parent / "sysroot"
            sysroot.mkdir()
            with mock.patch(
                "radek.game.android_linker.find_android_clang",
                return_value=(compiler, sysroot, ""),
            ):
                with mock.patch(
                    "radek.game.android_linker.shutil.which", return_value=None
                ):
                    missing = link_generated_translation(root, backend="cmake")
                    self.assertEqual(missing["status"], "BLOCKED_NO_CMAKE")
                    self.assertFalse(missing["apkProduced"])
                with mock.patch(
                    "radek.game.android_linker.shutil.which",
                    return_value=sys.executable,
                ):
                    missing = link_generated_translation(root, backend="cmake")
                    self.assertEqual(
                        missing["status"], "BLOCKED_NO_NDK_CMAKE_TOOLCHAIN"
                    )
                    self.assertFalse(missing["allVerificationsPassed"])

    def test_failed_cmake_steps_and_rejected_elf_never_replace_previous_artifacts(self):
        with tempfile.TemporaryDirectory(prefix="anyios failure ") as directory:
            root = Path(directory)
            for name in ("game_all.c", "rt_gen.c", "rt_gen.h"):
                (root / name).write_text("/* fixture */", encoding="utf-8")
            (root / "rt_report.json").write_text("{}", encoding="utf-8")
            compiler = root / "ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
            compiler.parent.mkdir(parents=True)
            compiler.write_text("fixture", encoding="utf-8")
            sysroot = compiler.parent.parent / "sysroot"
            sysroot.mkdir()
            toolchain = root / "ndk/build/cmake/android.toolchain.cmake"
            toolchain.parent.mkdir(parents=True)
            toolchain.write_text("# fake; no tool runs", encoding="utf-8")
            old_library = root / "android-link/arm64-v8a/libtranslated_game.so"
            old_library.parent.mkdir(parents=True)
            old_library.write_bytes(b"previous verified library")
            old_handoff = root / "translated-game-runtime-input.zip"
            old_handoff.write_bytes(b"previous verified handoff")

            def run_failure(fail_at):
                calls = []

                def fake_run(command, **kwargs):
                    calls.append(command)
                    if len(calls) == fail_at:
                        return subprocess.CompletedProcess(
                            command, 9, "simulated failure"
                        )
                    return subprocess.CompletedProcess(command, 0, "configured")

                return fake_run

            with (
                mock.patch(
                    "radek.game.android_linker.find_android_clang",
                    return_value=(compiler, sysroot, ""),
                ),
                mock.patch(
                    "radek.game.android_linker.shutil.which",
                    return_value=sys.executable,
                ),
            ):
                for fail_at in (1, 2):
                    with (
                        self.subTest(fail_at=fail_at),
                        mock.patch(
                            "radek.game.android_linker.subprocess.run",
                            side_effect=run_failure(fail_at),
                        ),
                    ):
                        failed = link_generated_translation(root, backend="cmake")
                        self.assertEqual(
                            failed["status"], "BLOCKED_ANDROID_LINK_FAILED"
                        )
                        self.assertEqual(failed["linkerExitCode"], 9)
                        self.assertIn("simulated failure", failed["linkerOutputTail"])
                        self.assertFalse(failed["allVerificationsPassed"])
                        self.assertEqual(
                            old_library.read_bytes(), b"previous verified library"
                        )
                        self.assertEqual(
                            old_handoff.read_bytes(), b"previous verified handoff"
                        )

                commands = []

                def produces_invalid_elf(command, **kwargs):
                    commands.append(command)
                    if len(commands) == 1:
                        output = next(
                            x.split("=", 1)[1]
                            for x in command
                            if x.startswith("-DANYIOS_OUTPUT_DIR=")
                        )
                        (Path(output) / "libtranslated_game.so").write_bytes(
                            b"not an ELF"
                        )
                    return subprocess.CompletedProcess(command, 0, "completed")

                with mock.patch(
                    "radek.game.android_linker.subprocess.run",
                    side_effect=produces_invalid_elf,
                ):
                    rejected = link_generated_translation(root, backend="cmake")
                self.assertEqual(rejected["status"], "BLOCKED_ANDROID_ELF_VALIDATION")
                self.assertFalse(rejected["allVerificationsPassed"])
                self.assertTrue((root / rejected["unverifiedArtifact"]).is_file())
                self.assertEqual(old_library.read_bytes(), b"previous verified library")
                self.assertEqual(old_handoff.read_bytes(), b"previous verified handoff")


class RealNdkCmakeTests(unittest.TestCase):
    def test_real_cmake_relinks_verifies_and_builds_handoff(self):
        if not NDK or not Path(NDK).is_dir() or not shutil.which("cmake"):
            if REQUIRE_REAL:
                self.fail("Android CI must provide a real NDK and CMake")
            self.skipTest(
                "set ANDROID_NDK_HOME and install CMake to run the real NDK test"
            )
        try:
            from radek.game.codegen import generate
        except ImportError:
            if REQUIRE_REAL:
                self.fail("Android CI must install Capstone for the real NDK test")
            self.skipTest("capstone is not installed")

        # Deliberately include spaces to exercise CMake argument/path handling.
        with tempfile.TemporaryDirectory(prefix="anyios real cmake ") as directory:
            root = Path(directory)
            source = ipa(
                root / "sample.ipa",
                executable=classic_game_macho_without_optional_sections(),
            )
            translation = root / "translated game"
            generate(str(source), str(translation))
            first = link_generated_translation(
                translation, "arm64-v8a", backend="cmake"
            )
            self.assertEqual(first["status"], "VERIFIED_ANDROID_SHARED_LIBRARY", first)
            self.assertEqual(first["linkBackend"], "CMAKE_ANDROID_NDK")
            self.assertTrue(first["ndkLinkVerified"])
            self.assertTrue(first["translationEntryPointVerified"])
            self.assertFalse(first["linkedIntoGame"])
            self.assertFalse(first["apkProduced"])
            self.assertEqual(first["translatedFunctionCount"], 1)
            self.assertTrue((translation / first["artifact"]).is_file())

            again = link_generated_translation(
                translation, "arm64-v8a", backend="cmake"
            )
            self.assertEqual(again["status"], "VERIFIED_ANDROID_SHARED_LIBRARY", again)
            self.assertEqual(first["cmakeBuildDirectory"], again["cmakeBuildDirectory"])
            self.assertEqual(first["artifactSha256"], again["artifactSha256"])

            game = translation / "game_all.c"
            game.write_text(
                game.read_text(encoding="utf-8")
                + "\nint anyios_relink_marker(void) { return 123; }\n",
                encoding="utf-8",
            )
            os.utime(game, None)
            changed = link_generated_translation(
                translation, "arm64-v8a", backend="cmake"
            )
            self.assertEqual(
                changed["status"], "VERIFIED_ANDROID_SHARED_LIBRARY", changed
            )
            self.assertNotEqual(first["artifactSha256"], changed["artifactSha256"])
            handoff = prepare_translated_game_runtime_input(
                translation, "arm64-v8a", changed
            )
            self.assertEqual(handoff["status"], "READY_FOR_GAME_RUNTIME_APK", handoff)
            with zipfile.ZipFile(
                translation / handoff["runtimeInputBundle"]
            ) as archive:
                manifest = json.loads(archive.read("manifest.json"))
                self.assertEqual(manifest["targetAbi"], "arm64-v8a")
                self.assertEqual(
                    manifest["files"]["libtranslated_game.so"]["sha256"],
                    changed["artifactSha256"],
                )
            self.assertFalse(handoff["gamePlayable"])
            old_bundle = (translation / handoff["runtimeInputBundle"]).read_bytes()
            armv7 = link_generated_translation(
                translation, "armeabi-v7a", backend="cmake"
            )
            self.assertEqual(armv7["status"], "VERIFIED_ANDROID_SHARED_LIBRARY", armv7)
            self.assertTrue(armv7["architectureVerified"])
            self.assertNotEqual(armv7["artifact"], changed["artifact"])
            v7_handoff = prepare_translated_game_runtime_input(
                translation, "armeabi-v7a", armv7
            )
            self.assertFalse(v7_handoff["readyForGameRuntimeApk"])
            self.assertIn("arm64-v8a", v7_handoff["reason"])
            self.assertEqual(
                (translation / handoff["runtimeInputBundle"]).read_bytes(), old_bundle
            )

            report = translation / "rt_report.json"
            valid_report = report.read_text(encoding="utf-8")
            verified_library = (translation / changed["artifact"]).read_bytes()
            broken = json.loads(valid_report)
            broken["translatedFunctionSymbols"] = ["missing_from_elf"]
            report.write_text(json.dumps(broken), encoding="utf-8")
            blocked = link_generated_translation(
                translation, "arm64-v8a", backend="cmake"
            )
            self.assertEqual(
                blocked["status"], "BLOCKED_TRANSLATED_SYMBOL_VERIFICATION"
            )
            self.assertFalse(blocked["allVerificationsPassed"])
            self.assertFalse(blocked["linkedIntoGame"])
            # A failed relink does not replace the last verified library.
            self.assertEqual(
                (translation / changed["artifact"]).read_bytes(), verified_library
            )
            report.write_text(valid_report, encoding="utf-8")
            cli = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "radek",
                    "relink",
                    str(translation),
                    "--authorized",
                    "--backend",
                    "cmake",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=600,
                check=False,
            )
            self.assertEqual(cli.returncode, 0, cli.stderr + cli.stdout)
            cli_report = json.loads(cli.stdout)
            self.assertEqual(
                cli_report["androidLink"]["linkBackend"], "CMAKE_ANDROID_NDK"
            )
            self.assertEqual(
                cli_report["handoff"]["status"], "READY_FOR_GAME_RUNTIME_APK"
            )


class LargeResourceSafetyTests(unittest.TestCase):
    def test_bounded_packager_rejects_large_files_before_reading_them(self):
        from radek.archive import InputError
        from radek.gamepack import MAX_RESOURCE_FILE_BYTES, _resource_inventory

        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory)
            resource = app / "large-resource"
            with resource.open("wb") as output:
                output.truncate(MAX_RESOURCE_FILE_BYTES + 1)
            with mock.patch.object(
                Path, "read_bytes", side_effect=AssertionError("must not allocate")
            ):
                with self.assertRaisesRegex(InputError, "resource exceeds"):
                    _resource_inventory(app, "Game")


if __name__ == "__main__":
    unittest.main()
