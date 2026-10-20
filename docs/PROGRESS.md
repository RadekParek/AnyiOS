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
   The second run passed SDK setup, formatting and host tests and **successfully
   compiled actual upstream ANGLE**: 1,636 build steps in 7m44.87s. Its subsequent
   dependency audit rejected `libnativewindow.so`. The pinned upstream BUILD.gn
   explicitly identifies this as the public API-26+ ANativeWindow library, so the
   corrected audit permits it on our API-29 minimum while tests reject private
   vendor dependencies and native-GLES substitution. No APK was produced by this run.
4. Added guest memory, fixed-address Mach-O mapping validation and temporary mapping
   diagnostics in the app import flow. Parser, memory and loader tests include
   25,000 malformed inputs plus 10,000 mutated executables under ASan/UBSan.
5. Read and retained CPU/dependency licenses, pinned Dynarmic and Boost, and added an
   independently designed adapter using actual Dynarmic. Built the upstream engine
   and **executed** ARM/Thumb fixtures successfully on this x86-64 host.
6. First ARM64 CPU cross-build failed to locate prepared Boost headers because NDK
   root-path rules exclude host-stored headers. The fix sets only the explicitly
   checked pinned Boost include directory, without disabling NDK cross-search rules.
   Host runtime tests also passed on GitHub Actions.
7. Added bounded static Mach-O import evidence (required/weak symbols, dylibs and
   fixed-function/programmatic GLES import hints) to the import report. This helps
   prioritize real dependencies without uploading a game. It is not an execution
   trace, complete dyld analysis or PVRTC-format detection.
8. Added CPU regression coverage for services, permissions, faults, bounded execution
   and host-written/self-modified instruction cache invalidation. Added separate
   host-execution and Android ARM64 cross-compilation CI jobs.

See RUNTIME.md for exact behavior, reproducible commands, limitations and the future
ADB executable test. CPU engine and dependency source pins are under
`third_party/dynarmic/`; no source/vendor trees or generated binaries are committed.

## Evidence boundaries

* Actual host CPU tests: passed locally and in GitHub Actions, including real ARM
  and Thumb execution via Dynarmic.
* Parser/memory/loader ASan/UBSan regression/mutation tests: passed.
* Formatting, Python lint, shell syntax, YAML syntax: passed locally.
* Device: none attached; user retains the S23 Ultra and game privately.
* Guest CPU inside Android app: not yet linked.
* Actual upstream ANGLE compilation: passed in run 39490248638; post-build audit
  failed on the now-corrected platform-library classification.
* ANGLE runtime, textured cube screenshot, gameplay: not verified.
* APK existence: only assert after a completed CI build publishes one; expected names
  in earlier documents are not evidence that an APK exists.
* Ninjago stages A–G: no game-stage evidence. Host fixtures are not the game.

The runtime is still a foundation, **not a finished high-level emulator**. Dynamic
linking/relocations, iOS ABI, Objective-C/framework semantics and guest graphics APIs
remain blockers. Do not implement arbitrary successful framework stubs to hide them.

## CI evidence links

* Initial SDK-action failure: https://github.com/RadekParek/PodNative/actions/runs/39489735013
* Successful upstream ANGLE compile / subsequent audit failure: https://github.com/RadekParek/PodNative/actions/runs/39490248638
* Host CPU success / first ARM64 cross-configuration failure: https://github.com/RadekParek/PodNative/actions/runs/39491738356

Full log download redirects are blocked by this sandbox's host allowlist; failing
build tails are additionally emitted as API-readable annotations. An attempted
cancellation of the duplicate known-failing Android run was denied by GitHub token
permissions; no permission or credential workaround was attempted. Future Android
runs are serialized, and successful ANGLE outputs will be cached before app
compilation to avoid repeating a long source build for unrelated app fixes.

## Further observed results

Runtime run https://github.com/RadekParek/PodNative/actions/runs/39492740731 passed
both host execution and ARM64 Android cross-compilation at revision
`9e380159e4d2c09c12d59c2c7be72d8cda5ec77f`. GitHub published the initial diagnostic
artifact (ID 12296712326, 7,154,331 bytes); no device execution occurred. Subsequent
review found that the pinned Oaknut Linux/Android allocator's RWX/MAP_FAILED handling
needs hardening before APK integration. The follow-up patch and host permission
regressions are documented under third_party/dynarmic/; the old artifact predates them.

Android run 39492740897 stopped at depot_tools bootstrap (exit 128) before compiling
ANGLE. Its uncaptured clone output prevents an exact root-cause assertion. The next
workflow captures that output and emits the failure tail through the API; no
unverified download mirror or silent substitute has been added.
