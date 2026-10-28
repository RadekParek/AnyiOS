"""Android NDK link, verification, and boot-payload preparation for translated C.

The linker emits an ABI-specific Android shared object containing generated
portable-C code, its guest-state runtime, and the GameBootActivity JNI entry.
The separate payload packer binds the translated memory image and provenance to
that verified library. This is a package-ready input, not an APK or proof of
visible rendering/gameplay; the game-runtime APK builder and launcher consume it
in a later step.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

from ..archive import InputError
from ..elf import inspect as inspect_elf


_SUPPORTED_ABIS = {
    "arm64-v8a": ("aarch64-linux-android", 21),
    "armeabi-v7a": ("armv7a-linux-androideabi", 21),
}
_ABI_ELF_CLASSES = {"arm64-v8a": 64, "armeabi-v7a": 32}
_ALLOWED_ANDROID_NEEDED = {"libc.so", "libm.so", "libdl.so"}
_TRANSLATED_GAME_ENTRY_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"
_TRANSLATED_GAME_PAYLOAD_CONTRACT = "translated-game-payload-v1"
_TRANSLATED_GAME_PAYLOAD_NAME = "translated-game-payload.zip"
_LINK_TIMEOUT_SECONDS = 600


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _inside_directory(root: Path, path: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except (OSError, RuntimeError, ValueError):
        return False


def _atomic_copy(source: Path, destination: Path) -> None:
    """Publish a file without following a pre-existing destination symlink."""
    fd, name = tempfile.mkstemp(prefix="." + destination.name + "-", suffix=".tmp", dir=destination.parent)
    pending = Path(name)
    try:
        with os.fdopen(fd, "wb") as target, source.open("rb") as original:
            shutil.copyfileobj(original, target, 1024 * 1024)
        os.replace(pending, destination)
    finally:
        pending.unlink(missing_ok=True)


def _host_tags() -> tuple[str, ...]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "linux":
        return ("linux-x86_64", "linux-aarch64") if machine in ("aarch64", "arm64") else (
            "linux-x86_64", "linux-aarch64"
        )
    if system == "darwin":
        return ("darwin-arm64", "darwin-x86_64") if machine in ("arm64", "aarch64") else (
            "darwin-x86_64", "darwin-arm64"
        )
    if system == "windows":
        return ("windows-x86_64",)
    return ()


def _version_key(path: Path) -> tuple[int, ...]:
    """Sort NDK directories by numeric version (r27 / 27.0.12077973 / 9.0)."""
    numbers = re.findall(r"\d+", path.name)
    return tuple(int(part) for part in numbers) or (0,)


def _local_properties_roots() -> list[Path]:
    """NDK/SDK locations from Android Studio's ``local.properties`` files."""
    found: list[Path] = []
    candidates = (
        Path.cwd() / "local.properties",
        Path(__file__).resolve().parents[2] / "local.properties",
    )
    for candidate in candidates:
        try:
            lines = candidate.read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for line in lines:
            key, sep, value = line.partition("=")
            if not sep:
                continue
            key, value = key.strip(), value.strip().replace("\\:", ":").replace("\\\\", "\\")
            if key == "ndk.dir" and value:
                found.append(Path(value).expanduser())
            elif key == "sdk.dir" and value:
                found.extend(sorted((Path(value).expanduser() / "ndk").glob("*"),
                                    key=_version_key, reverse=True))
    return found


def _default_sdk_roots() -> list[Path]:
    home = Path.home()
    return [
        home / "Android" / "Sdk",
        home / "Library" / "Android" / "sdk",
        Path("/opt/android-sdk"),
        Path("/usr/lib/android-sdk"),
    ]


def _ndk_roots(environ: dict[str, str] | None = None) -> list[Path]:
    """Candidate NDK roots, most specific first.

    Explicit NDK variables win, then SDK variables, ``local.properties``, and the
    default SDK install locations. The last two are consulted only for the real
    process environment so callers passing ``environ`` get a hermetic search.
    """
    env = os.environ if environ is None else environ
    roots: list[Path] = []
    for key in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME"):
        value = env.get(key)
        if value:
            roots.append(Path(value).expanduser())
    sdk_roots: list[Path] = []
    for key in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        value = env.get(key)
        if value:
            sdk_roots.append(Path(value).expanduser())
    if environ is None:
        roots.extend(_local_properties_roots())
        sdk_roots.extend(_default_sdk_roots())
    for sdk in sdk_roots:
        roots.extend(sorted((sdk / "ndk").glob("*"), key=_version_key, reverse=True))
        roots.append(sdk / "ndk-bundle")
    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        normalized = os.path.normcase(os.path.abspath(root))
        if normalized not in seen:
            seen.add(normalized)
            unique.append(root)
    return unique


def find_android_clang(environ: dict[str, str] | None = None) -> tuple[Path | None, Path | None, str]:
    """Find the NDK clang driver and sysroot without relying on Gradle/JDK."""
    for root in _ndk_roots(environ):
        prebuilt = root / "toolchains" / "llvm" / "prebuilt"
        for tag in _host_tags():
            toolchain = prebuilt / tag
            clang = toolchain / "bin" / ("clang.exe" if platform.system() == "Windows" else "clang")
            sysroot = toolchain / "sysroot"
            if clang.is_file() and sysroot.is_dir():
                return clang, sysroot, ""
    env = os.environ if environ is None else environ
    configured = any(env.get(key) for key in (
        "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME", "ANDROID_SDK_ROOT", "ANDROID_HOME"
    )) or (environ is None and bool(_local_properties_roots()))
    prefix = "configured NDK/SDK paths have no usable LLVM clang/sysroot" if configured else "no Android NDK was found"
    return None, None, (
        f"{prefix}. Set ANDROID_NDK_HOME, put ndk.dir=<path> in local.properties, "
        "or install an NDK under <SDK>/ndk/<version> (searched ANDROID_SDK_ROOT/ANDROID_HOME and "
        "the default SDK locations)"
    )


def _sha_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _fingerprint_files(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode("utf-8") + b"\0" + _file_sha256(path).encode("ascii") + b"\0")
    return digest.hexdigest()


def _load_link_cache(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _save_link_cache(path: Path, cache: dict) -> None:
    fd, name = tempfile.mkstemp(prefix="." + path.name + "-", suffix=".tmp", dir=path.parent)
    pending = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(cache, indent=2, sort_keys=True))
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def _link_failure(status: str, target_abi: str, reason: str) -> dict:
    return {
        "status": status,
        "attempted": False,
        "ndkLinkVerified": False,
        "targetAbi": target_abi,
        "linkedTranslatedFunctionCount": 0,
        "translatedFunctionCount": 0,
        "androidLinkedTextBytes": 0,
        "executableTextBytes": 0,
        "androidLinkedTextPercent": 0.0,
        "architectureVerified": False,
        "dependenciesVerified": False,
        "exportsVerified": False,
        "translationEntryPointVerified": False,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "allVerificationsPassed": False,
        "missingFunctionSymbols": [],
        "linkedIntoGame": False,
        "apkProduced": False,
        "completeGameConversion": False,
        "reason": reason,
    }


def verify_android_translation(
    elf_data: bytes,
    translation_report: dict,
    target_abi: str,
    *,
    artifact_name: str = "libtranslated_game.so",
) -> dict:
    """Verify the Android ELF and the complete generated-function export set."""
    if target_abi not in _SUPPORTED_ABIS:
        return _link_failure("BLOCKED_UNSUPPORTED_ANDROID_ABI", target_abi, "unsupported Android target ABI")
    try:
        elf = inspect_elf(elf_data)
    except (InputError, ValueError, struct.error) as exc:
        return _link_failure("BLOCKED_ANDROID_ELF_VALIDATION", target_abi, str(exc))

    if (
        elf.get("architecture") != target_abi
        or elf.get("elfClass") != _ABI_ELF_CLASSES[target_abi]
    ):
        return _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            f"ELF architecture/class is {elf.get('architecture')!r}/{elf.get('elfClass')!r}, "
            f"expected {target_abi!r}/{_ABI_ELF_CLASSES[target_abi]}",
        )
    dynamic_dependencies = elf.get("needed", [])
    unexpected_needed = sorted(set(dynamic_dependencies) - _ALLOWED_ANDROID_NEEDED)
    if unexpected_needed:
        return _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            "ELF depends on a non-Android or unreviewed library: " + ", ".join(unexpected_needed),
        )

    symbols = translation_report.get("translatedFunctionSymbols")
    if not isinstance(symbols, list) or not symbols or any(
        not isinstance(symbol, str) or not symbol for symbol in symbols
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C report does not contain a non-empty translatedFunctionSymbols list",
        )
    if len(symbols) != len(set(symbols)):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C report contains duplicate translated function symbols",
        )
    reported_functions = translation_report.get("functions")
    function_failures = translation_report.get("functionFailures", 0)
    if (
        isinstance(reported_functions, bool)
        or not isinstance(reported_functions, int)
        or reported_functions != len(symbols)
        or function_failures != 0
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C function count, translated symbol manifest, or function-failure count is inconsistent",
        )

    exports = elf.get("exports", {})
    missing = [symbol for symbol in symbols if symbol not in exports]
    malformed = [
        symbol
        for symbol in symbols
        if symbol in exports
        and (exports[symbol].get("type") != 2 or int(exports[symbol].get("size", 0)) <= 0)
    ]
    if missing or malformed:
        reasons = []
        if missing:
            reasons.append(f"{len(missing)} translated function symbol(s) are missing")
        if malformed:
            reasons.append(f"{len(malformed)} translated symbol(s) are not sized ELF functions")
        result = _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_VERIFICATION",
            target_abi,
            "; ".join(reasons),
        )
        result["missingFunctionSymbols"] = missing[:128]
        result["malformedFunctionSymbols"] = malformed[:128]
        result["translatedFunctionCount"] = len(symbols)
        return result

    entry_point_symbol = translation_report.get(
        "translationEntryPointSymbol", _TRANSLATED_GAME_ENTRY_SYMBOL
    )
    if not isinstance(entry_point_symbol, str) or not entry_point_symbol:
        return _link_failure(
            "BLOCKED_TRANSLATION_ENTRYPOINT_VERIFICATION",
            target_abi,
            "portable-C report does not name its Android JNI entry point",
        )
    entry_point = exports.get(entry_point_symbol)
    if (
        entry_point is None
        or entry_point.get("type") != 2
        or int(entry_point.get("size", 0)) <= 0
    ):
        result = _link_failure(
            "BLOCKED_TRANSLATION_ENTRYPOINT_VERIFICATION",
            target_abi,
            "Android shared library does not export a sized GameBootActivity JNI entry point",
        )
        result["translationEntryPointVerified"] = False
        result["entryPointSymbol"] = entry_point_symbol
        result["translatedFunctionCount"] = len(symbols)
        return result

    executable_text_bytes = int(translation_report.get("executableTextBytes", 0) or 0)
    translated_text_bytes = int(
        translation_report.get(
            "translatedUniqueTextBytes",
            translation_report.get("translatedFunctionBytes", 0),
        )
        or 0
    )
    if (
        executable_text_bytes <= 0
        or translated_text_bytes <= 0
        or translated_text_bytes > executable_text_bytes
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_COVERAGE_VERIFICATION",
            target_abi,
            "translated unique instruction bytes are empty or exceed executable __text",
        )

    return {
        "status": "VERIFIED_ANDROID_SHARED_LIBRARY",
        "attempted": True,
        "targetAbi": target_abi,
        "artifact": artifact_name,
        "artifactSizeBytes": len(elf_data),
        "artifactSha256": hashlib.sha256(elf_data).hexdigest(),
        "elfArchitecture": elf["architecture"],
        "elfClass": elf["elfClass"],
        "dynamicDependencies": dynamic_dependencies,
        "dynamicUndefinedSymbols": elf.get("undefinedSymbols", []),
        "architectureVerified": True,
        "dependenciesVerified": True,
        "translatedFunctionCount": len(symbols),
        "linkedTranslatedFunctionCount": len(symbols),
        "translatedUniqueTextBytes": translated_text_bytes,
        "androidLinkedTextBytes": translated_text_bytes,
        "executableTextBytes": executable_text_bytes,
        "androidLinkedTextPercent": round(100.0 * translated_text_bytes / executable_text_bytes, 6),
        "exportsVerified": True,
        "translationEntryPointVerified": True,
        "entryPointSymbol": entry_point_symbol,
        "allVerificationsPassed": True,
        "missingFunctionSymbols": [],
        "malformedFunctionSymbols": [],
        "linkedIntoGame": False,
        "apkProduced": False,
        "completeGameConversion": False,
        "apiRelinking": {
            "status": "GUEST_IMPORTS_STILL_DISPATCHED_BY_GENERATED_SHIMS",
            "staticGuestImportRewriteCount": 0,
            "note": (
                "This link contains the portable instruction translator and its state runtime. "
                "Mach-O API calls remain generated guest-shim dispatches; no per-IPA API "
                "callsite was statically rewritten to an Android API."
            ),
        },
        "note": (
            "A standalone Android shared object was linked and every generated translated-function "
            "symbol was verified. It is not an APK, is not linked into the GameBootActivity guest "
            "runtime, and is not evidence of a playable conversion."
        ),
    }


def _link_generated_with_cmake(
    destination: Path,
    target_abi: str,
    clang: Path,
    translation_report: dict,
    environ: dict[str, str] | None,
    timeout: int,
) -> dict:
    """Configure/build the same translated runner with the real NDK CMake toolchain.

    The build is isolated per NDK/CMake/ABI fingerprint and only a freshly
    verified output is atomically published. No shell or generated game code is
    executed by this step; the compiler consumes the authorized C inputs.
    """
    env = dict(os.environ)
    if environ is not None:
        env.update(environ)
    requested_cmake = env.get("ANYIOS_CMAKE", "cmake")
    cmake = shutil.which(requested_cmake, path=env.get("PATH"))
    if not cmake:
        return _link_failure("BLOCKED_NO_CMAKE", target_abi,
                             "CMake was not found; install CMake 3.22+ or set ANYIOS_CMAKE")
    toolchain = next((parent / "build/cmake/android.toolchain.cmake" for parent in clang.parents
                      if (parent / "build/cmake/android.toolchain.cmake").is_file()), None)
    if toolchain is None:
        return _link_failure("BLOCKED_NO_NDK_CMAKE_TOOLCHAIN", target_abi,
                             "The selected Android NDK has no build/cmake/android.toolchain.cmake")
    project = Path(__file__).with_name("cmake") / "CMakeLists.txt"
    if not project.is_file():
        return _link_failure("BLOCKED_CMAKE_PROJECT_MISSING", target_abi,
                             "The reviewed AnyiOS translated-game CMake project is missing")
    destination = destination.resolve()
    runtime = Path(__file__).with_name("rt").resolve()
    toolchain = toolchain.resolve()
    project = project.resolve()
    clang = clang.resolve()
    cmake = str(Path(cmake).resolve())
    link_dir = destination / "android-link" / target_abi
    if not _inside_directory(destination, link_dir):
        return _link_failure("BLOCKED_ANDROID_OUTPUT_PATH", target_abi, "link output directory escapes the translation directory")
    link_dir.mkdir(parents=True, exist_ok=True)
    # A CMake cache must never be silently reused with another NDK, compiler,
    # CMake binary, project, or ABI. CMake handles source/header incremental
    # rebuilds within this isolated directory.
    fingerprint = _sha_text("\0".join((
        str(toolchain), _file_sha256(toolchain), str(clang), str(clang.stat().st_size),
        str(clang.stat().st_mtime_ns), cmake, str(Path(cmake).stat().st_mtime_ns),
        _file_sha256(project), target_abi,
    )))[:20]
    build_dir = link_dir / ("cmake-build-" + fingerprint)
    output_dir = build_dir / "output"
    if not _inside_directory(destination, output_dir):
        return _link_failure("BLOCKED_ANDROID_OUTPUT_PATH", target_abi, "CMake build directory escapes the translation directory")
    # Do not let a pre-existing cache redirect generated objects or outputs
    # outside this job. CMake's reviewed project does not create symlinks.
    if build_dir.is_dir() and any(path.is_symlink() for path in build_dir.rglob("*")):
        return _link_failure("BLOCKED_ANDROID_OUTPUT_PATH", target_abi, "CMake build directory contains a symlink")
    output_dir.mkdir(parents=True, exist_ok=True)
    _, api_level = _SUPPORTED_ABIS[target_abi]
    commands = [
        [cmake, "-S", str(project.parent), "-B", str(build_dir),
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={target_abi}",
         f"-DANDROID_PLATFORM=android-{api_level}", "-DANDROID_STL=none",
         "-DCMAKE_BUILD_TYPE=Release", f"-DANYIOS_TRANSLATION_DIR={destination}",
         f"-DANYIOS_RUNTIME_DIR={runtime}", f"-DANYIOS_OUTPUT_DIR={output_dir}"],
        [cmake, "--build", str(build_dir), "--config", "Release", "--target", "translated_game"],
    ]
    output = ""
    for index, command in enumerate(commands):
        try:
            completed = subprocess.run(command, cwd=destination, env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            failure = _link_failure("BLOCKED_ANDROID_LINK_FAILED", target_abi,
                                    f"Android NDK CMake {'configuration' if index == 0 else 'build'} failed: {exc}")
            failure["attempted"] = True
            return failure
        output += (completed.stdout or "")
        if completed.returncode != 0:
            failure = _link_failure("BLOCKED_ANDROID_LINK_FAILED", target_abi,
                                    f"Android NDK CMake {'configuration' if index == 0 else 'build'} failed")
            failure.update(attempted=True, linkerExitCode=completed.returncode, linkerOutputTail=output[-12000:])
            return failure
    built = output_dir / "libtranslated_game.so"
    if not built.is_file():
        failure = _link_failure("BLOCKED_ANDROID_LINK_FAILED", target_abi,
                                "CMake succeeded but did not produce libtranslated_game.so")
        failure.update(attempted=True, linkerOutputTail=output[-12000:])
        return failure
    try:
        verified = verify_android_translation(built.read_bytes(), translation_report, target_abi)
    except (OSError, ValueError, TypeError) as exc:
        verified = _link_failure("BLOCKED_ANDROID_ELF_VALIDATION", target_abi,
                                 f"Android CMake output could not be verified: {exc}")
    verified["attempted"] = True
    if verified["status"] != "VERIFIED_ANDROID_SHARED_LIBRARY":
        rejected = link_dir / "libtranslated_game.unverified.so"
        try:
            _atomic_copy(built, rejected)
            verified["unverifiedArtifact"] = str(rejected.relative_to(destination))
        except OSError as exc:
            verified["diagnosticArtifactError"] = str(exc)
        verified["linkerOutputTail"] = output[-12000:]
        return verified
    artifact = link_dir / "libtranslated_game.so"
    try:
        _atomic_copy(built, artifact)
    except OSError as exc:
        failure = _link_failure("BLOCKED_ANDROID_ARTIFACT_WRITE", target_abi,
                                f"Verified Android library could not be published: {exc}")
        failure.update(attempted=True, linkerOutputTail=output[-4000:])
        return failure
    verified.update(
        artifact=str(artifact.relative_to(destination)), linker=str(clang),
        ndkLinkVerified=True, linkBackend="CMAKE_ANDROID_NDK", cmake=cmake,
        ndkCmakeToolchain=str(toolchain), cmakeBuildDirectory=str(build_dir.relative_to(destination)),
        linkerOutputTail=output[-4000:],
    )
    return verified


def link_generated_translation(
    output_dir: str | Path,
    target_abi: str = "arm64-v8a",
    *,
    environ: dict[str, str] | None = None,
    timeout: int = _LINK_TIMEOUT_SECONDS,
    backend: str = "clang",
) -> dict:
    """Link generated portable C with NDK clang or CMake and verify the result."""
    destination = Path(output_dir).resolve()
    if backend not in ("clang", "cmake"):
        return _link_failure("BLOCKED_UNSUPPORTED_LINK_BACKEND", target_abi, "choose clang or cmake")
    if target_abi == "auto":
        target_abi = "arm64-v8a"
    if target_abi not in _SUPPORTED_ABIS:
        return _link_failure(
            "BLOCKED_UNSUPPORTED_ANDROID_ABI",
            target_abi,
            "whole-game Android linking supports arm64-v8a and armeabi-v7a targets",
        )

    required = (destination / "game_all.c", destination / "rt_gen.c", destination / "rt_gen.h")
    missing_files = [path.name for path in required if not path.is_file()]
    runtime_root = Path(__file__).with_name("rt")
    runtime_source = runtime_root / "rt_core.c"
    entry_source = runtime_root / "rt_entry.c"
    android_jni_source = runtime_root / "rt_android_jni.c"
    filesystem_source = runtime_root / "rt_fs.c"
    filesystem_header = runtime_root / "rt_fs.h"
    entry_header = runtime_root / "rt_entry.h"
    missing_files.extend(
        str(path.relative_to(runtime_root.parent))
        for path in (entry_source, android_jni_source, filesystem_source, filesystem_header, entry_header)
        if not path.is_file()
    )
    report_path = destination / "rt_report.json"
    if not runtime_source.is_file():
        missing_files.append("radek/game/rt/rt_core.c")
    if not report_path.is_file():
        missing_files.append("rt_report.json")
    if missing_files:
        return _link_failure(
            "BLOCKED_TRANSLATION_ARTIFACTS_MISSING",
            target_abi,
            "required portable-C inputs are missing: " + ", ".join(missing_files),
        )

    clang, sysroot, reason = find_android_clang(environ)
    if clang is None or sysroot is None:
        failure = _link_failure("BLOCKED_NO_ANDROID_NDK", target_abi, reason)
        failure["ndkLinkVerified"] = False
        return failure

    try:
        translation_report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _link_failure(
            "BLOCKED_TRANSLATION_REPORT_INVALID",
            target_abi,
            f"portable-C translation report cannot be read: {exc}",
        )

    if backend == "cmake":
        return _link_generated_with_cmake(destination, target_abi, clang, translation_report, environ, timeout)

    triple, api_level = _SUPPORTED_ABIS[target_abi]
    target = f"{triple}{api_level}"
    link_dir = destination / "android-link" / target_abi
    if not _inside_directory(destination, link_dir):
        return _link_failure("BLOCKED_ANDROID_OUTPUT_PATH", target_abi, "link output directory escapes the translation directory")
    link_dir.mkdir(parents=True, exist_ok=True)
    artifact = link_dir / "libtranslated_game.so"
    temporary_artifact = link_dir / "libtranslated_game.so.tmp"
    if temporary_artifact.exists() or temporary_artifact.is_symlink():
        temporary_artifact.unlink()
    flags = [
        f"--target={target}",
        f"--sysroot={sysroot}",
        "-std=c11",
        "-O2",
        "-fPIC",
        "-fno-strict-aliasing",
        "-fvisibility=default",
    ]
    env_arg = None if environ is None else {**os.environ, **environ}
    include_dirs = [destination, runtime_source.parent]
    units = [
        destination / "game_all.c",
        destination / "rt_gen.c",
        runtime_source,
        filesystem_source,
        entry_source,
        android_jni_source,
    ]
    # Incremental relink: each translation unit is compiled to its own object and
    # reused while its source, the headers, the toolchain, and the flags are
    # unchanged. The shared object is then relinked from the object set.
    cache_path = link_dir / "link-cache.json"
    cache = _load_link_cache(cache_path)
    headers_fingerprint = _fingerprint_files(
        sorted({*destination.glob("*.h"), *runtime_root.glob("*.h")}, key=lambda p: p.name)
    )
    toolchain_fingerprint = _sha_text(
        f"{clang}|{clang.stat().st_size}|{int(clang.stat().st_mtime)}|{target}"
    )
    object_dir = link_dir / "objects"
    if not _inside_directory(destination, object_dir):
        return _link_failure("BLOCKED_ANDROID_OUTPUT_PATH", target_abi, "object directory escapes the translation directory")
    object_dir.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []
    compiled_units: list[str] = []
    reused_units: list[str] = []
    for unit in units:
        obj = object_dir / f"{unit.stem}.o"
        key = _sha_text("\0".join(
            [_file_sha256(unit), headers_fingerprint, toolchain_fingerprint, " ".join(flags)]
        ))
        entry = cache.get(obj.name) if isinstance(cache.get(obj.name), dict) else {}
        if (
            entry.get("key") == key
            and obj.is_file()
            and not obj.is_symlink()
            and entry.get("objectSha256") == _file_sha256(obj)
        ):
            reused_units.append(obj.name)
            objects.append(obj)
            continue
        cache.pop(obj.name, None)
        if obj.exists() or obj.is_symlink():
            obj.unlink()
        compile_command = [
            str(clang), *flags, *[f"-I{d}" for d in include_dirs], "-c", str(unit), "-o", str(obj),
        ]
        try:
            compiled = subprocess.run(
                compile_command,
                cwd=destination,
                env=env_arg,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            if obj.exists():
                obj.unlink()
            return _link_failure(
                "BLOCKED_ANDROID_LINK_FAILED",
                target_abi,
                f"Android NDK clang could not compile {unit.name}: {exc}",
            )
        if compiled.returncode != 0 or not obj.is_file():
            if obj.exists():
                obj.unlink()
            failure = _link_failure(
                "BLOCKED_ANDROID_LINK_FAILED",
                target_abi,
                f"Android NDK clang failed to compile {unit.name}",
            )
            failure["linkerExitCode"] = compiled.returncode
            failure["linkerOutputTail"] = (compiled.stdout or "")[-12000:]
            _save_link_cache(cache_path, cache)
            return failure
        cache[obj.name] = {"key": key, "objectSha256": _file_sha256(obj)}
        compiled_units.append(obj.name)
        objects.append(obj)
    _save_link_cache(cache_path, cache)

    command = [
        str(clang),
        *flags,
        "-shared",
        "-Wl,--no-undefined",
        "-Wl,--build-id=none",
        "-Wl,-z,max-page-size=16384",
        "-Wl,-soname,libtranslated_game.so",
        *[str(obj) for obj in objects],
        "-lm",
        "-o",
        str(temporary_artifact),
    ]
    try:
        completed = subprocess.run(
            command,
            cwd=destination,
            env=env_arg,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        if temporary_artifact.exists():
            temporary_artifact.unlink()
        return _link_failure(
            "BLOCKED_ANDROID_LINK_FAILED",
            target_abi,
            f"Android NDK clang could not link portable C: {exc}",
        )
    if completed.returncode != 0 or not temporary_artifact.is_file():
        if temporary_artifact.exists():
            temporary_artifact.unlink()
        failure = _link_failure(
            "BLOCKED_ANDROID_LINK_FAILED",
            target_abi,
            "Android NDK clang failed to link portable C",
        )
        failure["linkerExitCode"] = completed.returncode
        failure["linkerOutputTail"] = (completed.stdout or "")[-12000:]
        return failure

    try:
        elf_data = temporary_artifact.read_bytes()
        verified = verify_android_translation(
            elf_data,
            translation_report,
            target_abi,
            artifact_name=artifact.name,
        )
    except OSError as exc:
        verified = _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            f"Android linker output could not be read: {exc}",
        )
    if verified["status"] != "VERIFIED_ANDROID_SHARED_LIBRARY":
        # Retain a rejected ELF as a diagnostic artifact, but never count it.
        rejected = link_dir / "libtranslated_game.unverified.so"
        temporary_artifact.replace(rejected)
        verified["unverifiedArtifact"] = str(rejected.relative_to(destination))
        verified["linkerOutputTail"] = (completed.stdout or "")[-12000:]
        return verified

    temporary_artifact.replace(artifact)
    verified["artifact"] = str(artifact.relative_to(destination))
    verified["commandTarget"] = target
    verified["linker"] = str(clang)
    verified["ndkLinkVerified"] = True
    verified["linkBackend"] = "DIRECT_NDK_CLANG"
    verified["incremental"] = {
        "compiledUnits": compiled_units,
        "reusedUnits": reused_units,
        "objectDirectory": str(object_dir.relative_to(destination)),
    }
    verified["linkerOutputTail"] = (completed.stdout or "")[-4000:]
    return verified


def prepare_translated_game_runtime_input(
    output_dir: str | Path,
    target_abi: str,
    android_link_report: dict,
) -> dict:
    """Package the translated memory image and verified library for APK import.

    The returned ZIP is an input to ``GameRuntimeApkBuilder``; it is not itself
    an APK, and it does not change the standalone link's linkedIntoGame flag.
    """
    destination = Path(output_dir).resolve()
    failure = {
        "status": "BLOCKED_TRANSLATED_GAME_PACKAGE_INPUT",
        "readyForGameRuntimeApk": False,
        "linkedIntoGame": False,
        "apkProduced": False,
    }
    if target_abi not in _SUPPORTED_ABIS:
        return {**failure, "reason": "translated game package input uses an unsupported Android ABI"}
    if target_abi != "arm64-v8a":
        return {**failure, "reason": (
            "The Android game-runtime importer currently accepts arm64-v8a handoffs only; "
            "an ARMv7 ELF can still be verified as a standalone library."
        )}
    required_flags = (
        android_link_report.get("status") == "VERIFIED_ANDROID_SHARED_LIBRARY",
        android_link_report.get("targetAbi") == target_abi,
        android_link_report.get("architectureVerified") is True,
        android_link_report.get("dependenciesVerified") is True,
        android_link_report.get("exportsVerified") is True,
        android_link_report.get("translationEntryPointVerified") is True,
        android_link_report.get("allVerificationsPassed") is True,
        android_link_report.get("ndkLinkVerified") is True,
    )
    if not all(required_flags):
        return {**failure, "reason": "Android architecture, dependency, export, entry-point, and link checks must pass first"}

    report_path = destination / "rt_report.json"
    memory_path = destination / "rt_mem.bin"
    if not report_path.is_file() or not memory_path.is_file():
        return {**failure, "reason": "rt_report.json and rt_mem.bin are required for the translated boot payload"}
    try:
        translation = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {**failure, "reason": f"portable-C translation report is invalid: {exc}"}
    source_hash = translation.get("sourceExecutableSha256")
    if not isinstance(source_hash, str) or len(source_hash) != 64 or any(c not in "0123456789abcdef" for c in source_hash):
        return {**failure, "reason": "portable-C translation report does not bind a source executable SHA-256"}
    if translation.get("translationEntryPointSymbol") != _TRANSLATED_GAME_ENTRY_SYMBOL:
        return {**failure, "reason": "portable-C report does not name the GameBootActivity translation entry point"}
    functions = translation.get("functions")
    symbols = translation.get("translatedFunctionSymbols")
    if (
        isinstance(functions, bool)
        or not isinstance(functions, int)
        or functions <= 0
        or translation.get("functionFailures") != 0
        or not isinstance(symbols, list)
        or len(symbols) != functions
        or any(not isinstance(symbol, str) or not symbol for symbol in symbols)
        or len(set(symbols)) != len(symbols)
    ):
        return {**failure, "reason": "portable-C translated-function manifest is incomplete or inconsistent"}
    if android_link_report.get("linkedTranslatedFunctionCount") != functions:
        return {**failure, "reason": "verified Android function count does not match the translation report"}

    artifact_value = android_link_report.get("artifact")
    if not isinstance(artifact_value, str) or not artifact_value:
        return {**failure, "reason": "verified Android shared-library path is missing"}
    artifact_path = (destination / artifact_value).resolve()
    try:
        artifact_path.relative_to(destination)
    except ValueError:
        return {**failure, "reason": "verified Android shared-library path escapes the translation directory"}
    if not artifact_path.is_file():
        return {**failure, "reason": "verified Android shared library is missing"}
    library_sha = _file_sha256(artifact_path)
    if library_sha != android_link_report.get("artifactSha256"):
        return {**failure, "reason": "Android shared-library hash no longer matches its verified link report"}
    if android_link_report.get("entryPointSymbol") != _TRANSLATED_GAME_ENTRY_SYMBOL:
        return {**failure, "reason": "verified Android link report has the wrong JNI entry point"}

    memory_sha = _file_sha256(memory_path)
    translation_report_sha = _file_sha256(report_path)
    manifest = {
        "contract": _TRANSLATED_GAME_PAYLOAD_CONTRACT,
        "schemaVersion": 1,
        "targetAbi": target_abi,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "sourceExecutableSha256": source_hash,
        "translationReportSha256": translation_report_sha,
        "androidLibrarySha256": library_sha,
        "translatedFunctionCount": functions,
        "translatedUniqueTextBytes": translation.get(
            "translatedUniqueTextBytes", translation.get("translatedFunctionBytes", 0)
        ),
        "executableTextBytes": translation.get("executableTextBytes", 0),
        "memoryImage": {
            "name": "rt_mem.bin",
            "sizeBytes": memory_path.stat().st_size,
            "sha256": memory_sha,
        },
    }
    payload_path = destination / _TRANSLATED_GAME_PAYLOAD_NAME
    temporary_payload: Path | None = None
    input_path = destination / "translated-game-runtime-input.zip"
    temporary_input: Path | None = None
    link_bytes = json.dumps(android_link_report, sort_keys=True, indent=2).encode("utf-8")
    bundle_manifest = {
        "contract": "translated-game-runtime-input-v1",
        "schemaVersion": 1,
        "targetAbi": target_abi,
        "sourceExecutableSha256": source_hash,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "files": {
            "libtranslated_game.so": {
                "sizeBytes": artifact_path.stat().st_size,
                "sha256": library_sha,
            },
            _TRANSLATED_GAME_PAYLOAD_NAME: {
                "sizeBytes": 0,
                "sha256": "",
            },
            "android-link-report.json": {
                "sizeBytes": len(link_bytes),
                "sha256": hashlib.sha256(link_bytes).hexdigest(),
            },
            "rt_report.json": {
                "sizeBytes": report_path.stat().st_size,
                "sha256": translation_report_sha,
            },
        },
        "linkVerification": {
            "architectureVerified": True,
            "dependenciesVerified": True,
            "exportsVerified": True,
            "translationEntryPointVerified": True,
            "allVerificationsPassed": True,
            "status": "VERIFIED_ANDROID_SHARED_LIBRARY",
        },
    }
    try:
        payload_fd, payload_name = tempfile.mkstemp(prefix=".translated-payload-", suffix=".tmp", dir=destination)
        os.close(payload_fd)
        temporary_payload = Path(payload_name)
        input_fd, input_name = tempfile.mkstemp(prefix=".translated-input-", suffix=".tmp", dir=destination)
        os.close(input_fd)
        temporary_input = Path(input_name)
        with zipfile.ZipFile(temporary_payload, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            archive.writestr("manifest.json", json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            archive.write(memory_path, "rt_mem.bin")
        payload_sha = _file_sha256(temporary_payload)
        bundle_manifest["files"][_TRANSLATED_GAME_PAYLOAD_NAME] = {
            "sizeBytes": temporary_payload.stat().st_size,
            "sha256": payload_sha,
        }
        with zipfile.ZipFile(temporary_input, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            archive.writestr("manifest.json", json.dumps(bundle_manifest, sort_keys=True, separators=(",", ":")))
            archive.write(artifact_path, "libtranslated_game.so")
            archive.write(temporary_payload, _TRANSLATED_GAME_PAYLOAD_NAME)
            archive.writestr("android-link-report.json", link_bytes)
            archive.write(report_path, "rt_report.json")
        # Read each produced archive to catch truncated central directories before
        # reporting a package-ready handoff.
        for candidate in (temporary_payload, temporary_input):
            with zipfile.ZipFile(candidate, "r") as archive:
                if archive.testzip() is not None:
                    raise OSError("translated game runtime ZIP failed its CRC check")
        os.replace(temporary_payload, payload_path)
        os.replace(temporary_input, input_path)
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        if temporary_payload is not None:
            temporary_payload.unlink(missing_ok=True)
        if temporary_input is not None:
            temporary_input.unlink(missing_ok=True)
        # Do not remove the last verified handoff when a new attempt fails.
        return {**failure, "reason": f"translated game-runtime package input could not be written: {exc}"}

    return {
        "status": "READY_FOR_GAME_RUNTIME_APK",
        "readyForGameRuntimeApk": True,
        "contract": "translated-game-runtime-input-v1",
        "payloadContract": _TRANSLATED_GAME_PAYLOAD_CONTRACT,
        "targetAbi": target_abi,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "sourceExecutableSha256": source_hash,
        "translatedGamePayload": payload_path.name,
        "translatedGamePayloadSha256": payload_sha,
        "translatedGamePayloadBytes": payload_path.stat().st_size,
        "runtimeInputBundle": input_path.name,
        "runtimeInputBundleSha256": _file_sha256(input_path),
        "runtimeInputBundleBytes": input_path.stat().st_size,
        "linkedIntoGame": False,
        "apkProduced": False,
        "gamePlayable": False,
        "note": (
            "Verified host input bundle for the game-runtime APK builder. This is not itself an APK, "
            "is not yet packaged by the device app, and does not provide an EGL renderer or gameplay evidence."
        ),
    }


__all__ = [
    "find_android_clang",
    "link_generated_translation",
    "prepare_translated_game_runtime_input",
    "verify_android_translation",
]
