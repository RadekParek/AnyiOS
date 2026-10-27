"""Optional local Ghidra inspection; never executes or decrypts guest code.

Only the explicitly selected desktop installation is launched. Output is diagnostic
pseudocode and has no bearing on conversion or gameplay claims.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile

MAX_INPUT_BYTES = 128 * 1024 * 1024
MAX_REPORT_BYTES = 4 * 1024 * 1024
TIMEOUT_SECONDS = 180
ANALYSIS_SECONDS = 90
MAX_CPUS = 2
MAX_HEAP = "1G"
_RESULT = re.compile(r"/\* ANYIOS_RESULT attempted=(\d+) completed=(\d+) limited=([01]) analysisTimedOut=([01]) \*/\s*\Z")


def inspect(executable: Path, mach: dict, output: Path, ghidra_home: Path | None) -> dict:
    result = {
        "status": "NOT_REQUESTED",
        "attempted": False,
        "completeGameConversion": False,
        "message": "Optional local Ghidra analysis was not requested.",
        "limits": {
            "inputBytes": MAX_INPUT_BYTES,
            "wallSeconds": TIMEOUT_SECONDS,
            "analysisSeconds": ANALYSIS_SECONDS,
            "functions": 32,
            "secondsPerFunction": 3,
            "requestedMaxCpu": MAX_CPUS,
            "requestedJavaHeap": MAX_HEAP,
        },
    }
    if ghidra_home is None:
        return result
    if os.name == "nt":
        result.update(status="UNAVAILABLE", message="The optional Ghidra runner currently requires Linux or macOS.")
        return result
    launcher = Path(ghidra_home).expanduser() / "support" / "analyzeHeadless"
    if not launcher.is_file() or not os.access(launcher, os.X_OK):
        result.update(status="UNAVAILABLE", message="A runnable support/analyzeHeadless was not found in the selected Ghidra installation.")
        return result
    if any(part.get("encrypted") for part in mach.get("slices", [])):
        result.update(status="SKIPPED_ENCRYPTED", message="Encrypted Mach-O images are not decompiled or decrypted.")
        return result
    if executable.stat().st_size > MAX_INPUT_BYTES:
        result.update(status="SKIPPED_TOO_LARGE", message="Executable exceeds the optional Ghidra input-size limit.")
        return result

    output = output.resolve()
    artifact = output / "ghidra-inferred.c"
    scripts = Path(__file__).resolve().parent / "ghidra_scripts"
    with tempfile.TemporaryDirectory(prefix="ghidra-project-", dir=output) as temporary:
        command = [
            str(launcher.resolve()), temporary, "AnyiOS", "-import", str(executable.resolve()),
            "-analysisTimeoutPerFile", str(ANALYSIS_SECONDS), "-max-cpu", str(MAX_CPUS),
            "-scriptPath", str(scripts), "-postScript", "AnyiOSDecompile.java", str(artifact),
            "-deleteProject",
        ]
        env = os.environ.copy()
        env["GHIDRA_HEADLESS_MAXMEM"] = MAX_HEAP
        env["GHIDRA_MAXMEM"] = MAX_HEAP
        result["attempted"] = True
        try:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=env, start_new_session=True,
            )
            try:
                status = process.wait(timeout=TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                # Ghidra launches a JVM and native decompiler: kill the group, not just the shell.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                result.update(status="TIMED_OUT", message="Local Ghidra exceeded the wall-clock limit; partial output was discarded.")
                artifact.unlink(missing_ok=True)
                return result
        except OSError as exc:
            result.update(status="UNAVAILABLE", message=f"Could not launch local Ghidra: {exc}")
            artifact.unlink(missing_ok=True)
            return result
        if status != 0 or not artifact.is_file() or artifact.stat().st_size > MAX_REPORT_BYTES:
            artifact.unlink(missing_ok=True)
            result.update(status="FAILED", message="Ghidra did not finish successfully or produced no bounded report. Check its installation and Java version.")
            return result
        try:
            text = artifact.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            artifact.unlink(missing_ok=True)
            result.update(status="FAILED", message="Ghidra output could not be read as UTF-8.")
            return result
        marker = _RESULT.search(text)
        if not text.startswith("/* ANYIOS_GHIDRA_REPORT_V1\n") or marker is None:
            artifact.unlink(missing_ok=True)
            result.update(status="FAILED", message="Ghidra did not produce a complete AnyiOS report; partial output was discarded.")
            return result
        attempted, completed, limited, analysis_timed_out = map(int, marker.groups())
        if attempted > 32 or completed > attempted:
            artifact.unlink(missing_ok=True)
            result.update(status="FAILED", message="Ghidra produced an invalid function count.")
            return result
        result.update(
            status="EXPORTED_SAMPLE" if completed else "NO_DECOMPILATION",
            functionsAttempted=attempted,
            functionsDecompiled=completed,
            truncated=bool(limited),
            analysisTimedOut=bool(analysis_timed_out),
            path=artifact.name,
            message="Local Ghidra output is inferred pseudocode for a bounded function sample, not original source or a runnable conversion. Check analysisTimedOut and truncated before interpreting it.",
        )
        return result
