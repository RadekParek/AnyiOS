# Continuation progress — 2026-10-20

This supersedes the initial BUILD_REPORT.md where noted. No rendered-frame,
Android instruction-execution or game-compatibility result is claimed.

## Engineering changes

1. Committed and pushed the original foundation as `7827f026e79a49e09936f942d720743b010732a3`
   on `arena/568194d8-podnative` so GitHub Actions could actually execute.
2. First Android run failed in `android-actions/setup-android`. Its complete log
   download host is inaccessible from this sandbox; the job/step result was confirmed
   via the GitHub API. Do not infer an exact root cause from that limited evidence.
3. Replaced that opaque setup step with the runner's explicitly checked preinstalled
   SDK toolchain, added persistent SDK/build diagnostics and API-visible failure tails.
   The second run passed SDK setup, formatting and host tests and reached upstream
   ANGLE's actual source build. Current final status must be checked on GitHub.
4. Added guest memory, fixed-address Mach-O mapping validation and temporary mapping
   diagnostics in the app import flow. Parser, memory and loader tests include
   25,000 malformed inputs plus 10,000 mutated executables under ASan/UBSan.
5. Read and retained CPU/dependency licenses, pinned Dynarmic and Boost, and added an
   independently designed adapter using actual Dynarmic. Built the upstream engine
   and **executed** ARM/Thumb fixtures successfully on this x86-64 host.
6. Added CPU regression coverage for services, permissions, faults, bounded execution
   and host-written/self-modified instruction cache invalidation. Added separate
   host-execution and Android ARM64 cross-compilation CI jobs.

See RUNTIME.md for exact behavior, reproducible commands, limitations and the future
ADB executable test. CPU engine and dependency source pins are under
`third_party/dynarmic/`; no source/vendor trees or generated binaries are committed.

## Evidence boundaries

* Actual host CPU tests: passed, including real ARM and Thumb execution via Dynarmic.
* Parser/memory/loader ASan/UBSan regression/mutation tests: passed.
* Formatting, Python lint, shell syntax, YAML syntax: passed locally.
* Device: none attached; user retains the S23 Ultra and game privately.
* Guest CPU inside Android app: not yet linked.
* ANGLE runtime, textured cube screenshot, gameplay: not verified.
* APK existence: only assert after a completed CI build publishes one; expected names
  in earlier documents are not evidence that an APK exists.
* Ninjago stages A–G: no game-stage evidence. Host fixtures are not the game.

The runtime is still a foundation, **not a finished high-level emulator**. Dynamic
linking/relocations, iOS ABI, Objective-C/framework semantics and guest graphics APIs
remain blockers. Do not implement arbitrary successful framework stubs to hide them.
