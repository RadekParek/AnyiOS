import argparse
import json
import sys
from pathlib import Path
from .apk import Toolchain, validate_apk, validate_experimental_shell
from .gameruntime import manifest_summary, run_gameboot


def main():
    parser = argparse.ArgumentParser(
        description="Authorized IPA inspection and fail-closed static-recompilation assessment"
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("analyze", "convert"):
        p = sub.add_parser(name)
        p.add_argument("ipa", type=Path)
        p.add_argument("--output", type=Path, required=True, help="new, non-existing job directory")
        p.add_argument(
            "--authorized", action="store_true", help="confirm ownership/permission to convert this IPA"
        )
        p.add_argument(
            "--ghidra-home", type=Path, default=None,
            help="optional local desktop Ghidra installation for bounded diagnostic pseudocode (Linux/macOS)",
        )
        p.add_argument(
            "--android-linker", choices=("clang", "cmake"), default="clang",
            help="host backend for optional portable-C linking (default: NDK clang)",
        )
        p.add_argument(
            "--target-abi",
            choices=("auto", "arm64-v8a", "armeabi-v7a"),
            default="auto",
            help="assessment target ABI; auto prefers ARM64 in a FAT IPA and selects ARMv7 for ARM32-only inputs; no APK is emitted",
        )
    p = sub.add_parser("relink", help="relink existing translated C with the Android NDK on a host")
    p.add_argument("translation_dir", type=Path, help="existing bytecode-translation directory")
    p.add_argument("--authorized", action="store_true", help="confirm permission to process this translation")
    p.add_argument("--backend", choices=("clang", "cmake"), default="cmake")
    p.add_argument("--target-abi", choices=("arm64-v8a", "armeabi-v7a"), default="arm64-v8a")
    p = sub.add_parser(
        "gameboot",
        help="probe the game-runtime boot attempt for one authorized IPA (contract game-runtime-v1)",
    )
    p.add_argument("ipa", type=Path)
    p.add_argument("--output", type=Path, required=True, help="new, non-existing job directory")
    p.add_argument(
        "--authorized", action="store_true", help="confirm ownership/permission to convert this IPA"
    )
    p = sub.add_parser(
        "validate-shell",
        help="validate an honestly labelled experimental shell APK (never a complete-game APK)",
    )
    p.add_argument("apk", type=Path)
    p = sub.add_parser("validate")
    p.add_argument("apk", type=Path)
    p.add_argument("--package", required=True)
    p.add_argument("--entry", required=True)
    p.add_argument(
        "--abi",
        choices=("arm64-v8a", "armeabi-v7a"),
        default=None,
        help="expected native ABI; inferred from conversion provenance when omitted",
    )
    p.add_argument(
        "--converter-app",
        action="store_true",
        help="validate the importer APK rather than a converted program",
    )
    args = parser.parse_args()
    try:
        if args.command == "relink":
            if not args.authorized:
                raise ValueError("authorization confirmation is required before compiling translated code")
            from .game.android_linker import link_generated_translation, prepare_translated_game_runtime_input
            directory = args.translation_dir.resolve(strict=True)
            if not directory.is_dir():
                raise ValueError("translation directory does not exist")
            link = link_generated_translation(directory, args.target_abi, backend=args.backend)
            handoff = None
            if link.get("status") == "VERIFIED_ANDROID_SHARED_LIBRARY":
                handoff = prepare_translated_game_runtime_input(directory, args.target_abi, link)
            # Preserve any old library/handoff on failure; the new report says
            # explicitly whether this run verified and packaged anything.
            report_path = directory / "android-link-report.json"
            pending = directory / ".android-link-report.json.tmp"
            try:
                pending.write_text(json.dumps(link, indent=2, ensure_ascii=True), encoding="utf-8")
                pending.replace(report_path)
            finally:
                pending.unlink(missing_ok=True)
            print(json.dumps({"androidLink": link, "handoff": handoff, "linkReport": str(report_path)}, indent=2))
            return 0 if handoff and handoff.get("readyForGameRuntimeApk") else 2
        if args.command == "gameboot":
            manifest = run_gameboot(args.ipa, args.authorized, args.output)
            summary = manifest_summary(manifest)
            summary["manifest"] = str(args.output / "game-runtime-manifest.json")
            print(json.dumps(summary, indent=2))
            return 0
        if args.command == "validate-shell":
            result = validate_experimental_shell(args.apk, Toolchain.discover())
            print(json.dumps(result, indent=2))
            return 0 if result["status"] == "VALID" else 1
        if args.command == "validate":
            result = validate_apk(
                args.apk,
                Toolchain.discover(),
                args.package,
                args.entry,
                not args.converter_app,
                expected_abi=args.abi,
            )
            print(json.dumps(result, indent=2))
            return 0
        from .pipeline import Pipeline
        result = Pipeline(args.output).run(
            args.ipa,
            args.authorized,
            args.command == "analyze",
            args.target_abi,
            ghidra_home=args.ghidra_home,
            android_linker=args.android_linker,
        )
        print(
            json.dumps(
                {
                    "state": result["state"],
                    "report": str(args.output / "report.json"),
                    "blockers": result.get("blockers", []),
                    "error": result.get("error"),
                },
                indent=2,
            )
        )
        return {"READY": 0, "PARTIAL": 2, "BLOCKED": 3, "FAILED": 1}[result["state"]]
    except Exception as exc:
        print(f"{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
