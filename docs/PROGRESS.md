# Continuation progress — 2026-10-22

## 0.9.2 — startup-blocker sweep

Every item below was a named blocker in a real launch report. Each one is now a code
change with a host test behind it; none of them is a stub that returns success.

1. **`DYLD_ADDRESS: cursor arithmetic overflow`** — the dyld cursor is `uint64_t`
   arithmetic that wraps, exactly like Apple's `address +=`. Link editors encode a
   backward skip as a ten-byte ULEB (the two's complement of the distance), so rejecting
   the wrap refused valid streams. `decodeDyld` now advances modulo 2^64 and validates
   the resulting fixup target against the selected segment (`DYLD_TARGET`/`DYLD_SEGMENT`
   when it really is outside the image).
2. **`DYLD_TRUNCATED: stream is missing DONE`** — dyld's own decoder loops `while (p < end)`
   and never demands a trailing `DONE`. Streams that stop cleanly on an opcode boundary are
   accepted; only an operand that is cut in half is `DYLD_TRUNCATED`.
3. **`DYLD_LIMIT: rebase repeat count`** — repeated rebases/binds are bound by the remaining
   fixup budget rather than by the raw opcode count, and a rebase plus a bind on the same
   slot is a lazy pointer that merges (bind wins) instead of a conflict.
4. **`DYLD_STUB: symbol stub size is not patchable`** — real ARM binaries contain 4-byte
   `ldr pc, [pc, #imm]` stubs (573 of them in the Minecraft build). Those are dispatched
   through the pointer slot they name, with the instruction validated as an
   `ldr pc, [pc, #imm]` before the slot is touched; the 12-byte rewrite path is unchanged.
5. **`HLE_UNIMPLEMENTED: _vsnprintf`** — `vsnprintf`/`_vsnprintf` now format through the
   existing formatter by walking the guest `va_list` with AAPCS rules (8-byte alignment for
   `double`/`int64_t`), writing into the caller's buffer and returning the untruncated length.
6. **`UNIMPLEMENTED _UIApplicationMain`** — a real bootstrap in `runtime/uikit.cpp`: argv is
   parsed into a guest `argc/argv` pair, the delegate class named in the call's fourth argument is
   instantiated through the guest's own class metadata, its launch selector is called, and control returns
   to the caller only at a lifecycle gate the runtime owns. A missing delegate class, an
   unusable `argv` or a delegate without a launch selector are named faults
   (`UIKIT_DELEGATE`, `UIKIT_ARGV`), never silent success.
7. **`FAULT MEMORY_UNMAPPED` at `0x0`** — an access in the first page is now
   `MEMORY_NULL: null pointer dereference` and names the guest PC, so a nil message result
   or a null import is distinguishable from a random unmapped address.
8. **`LOAD_COMMAND_UNSUPPORTED: 0x2b`** — `LC_DYLIB_CODE_SIGN_DRS`, `LC_SEGMENT_SPLIT_INFO`,
   `LC_ENCRYPTION_INFO_64`, `LC_LINKER_OPTION` and `LC_LINKER_OPTIMIZATION_HINT` are parsed
   and range-checked. Encrypted images are still refused by name, and `LC_DYLD_ENVIRONMENT`
   is validated and deliberately ignored so a guest cannot inject `DYLD_*` variables.

Stop reports now also name the guest function: the loader keeps the image's `N_SECT`
symbols (compiler labels such as `ltmp0` are dropped), and `GUEST_BLOCKER` gains
`symbol=<nearest function>`. The crash screen shows "Guest PC in &lt;function&gt;" instead of a
bare hex address.

Display work: games request landscape (`SCREEN_ORIENTATION_LANDSCAPE`, the landscape-left
holding position an iPhone title expects) instead of starting whichever way the phone
happened to lie, and the debug console starts hidden — it is opt-in from Settings.

### Faster pipeline

The same checks now take much less wall clock:

* The PC job restores the pinned dynarmic/Boost/converter source trees (~1 min each run)
  and compiles both suites through a content-addressed ccache, so unchanged objects are
  never rebuilt.
* The Android job reuses those sources, keeps the existing verified ANGLE cache, and runs
  Gradle with the build cache and parallel module execution instead of re-running task
  outputs it already produced.
* `scripts/test-host.sh` builds its fourteen sanitizer binaries and its strict-warning
  sweep in parallel (`HOST_TEST_JOBS` caps it), which took the suite from ~110 s to ~57 s
  here, and both workflows no longer run the same host suite twice.
* Branch pushes no longer trigger a second full run of the same two jobs that their pull
  request already starts.

Evidence: `scripts/test-host.sh` is green end to end (loader/Mach-O fuzz, dyld, export graph,
libc, C++, audio, fs, Objective-C, UIKit bootstrap); both test games the user added to
`tests/` load through the same path the app uses (fat slice selection included): AngryBirds
maps 36,564,992 guest bytes, decodes 230 bindings, and reports **143** required imports still
missing; Minecraft PE maps 37,109,760 guest bytes, decodes 20,585 rebases and 994 bindings,
and reports **391** missing. Both are loader/dyld results on this host — no device, rendering
or gameplay claim, and neither title is dumped, patched or redistributed.


This supersedes the initial BUILD_REPORT.md where noted. The user reports the
previous graphics preview working on their S23 Ultra; no screenshot/log was supplied.
No Android instruction-execution or game-compatibility result is claimed.

## Latest: 0.2.1 startup preview

[Verified APK and CI evidence](STARTUP_APK_REPORT.md): no fixed import-byte/library
entry quota, bounded LC_DYSYMTAB handling and an enabled startup-validation action
with saved/exportable launch blockers. Android lint/build/tests/signing and runtime
host/ARM64 cross-build checks passed. No game code runs in the app yet; the next
execution contracts are recorded in EXECUTION_NEXT.md.

## Previous: library preview

[0.2 APK and verified CI evidence](LIBRARY_APK_REPORT.md): persistent private IPA/folder
library, universal ARM32 selection, metadata parser, modern native UI and original
launcher icon. Android build/lint/tests/signing and runtime host/cross-build checks
passed. New UI/import behavior still needs physical-device review. Game launch
remains unavailable; see [limits](LIBRARY_PREVIEW.md).

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


Runtime run 39493714457 passed both host correctness (including the allocator
permission regression) and the patched Android ARM64 cross-build. It published
`PodNative-arm64-cpu-test` artifact ID 12296603610 (7,398,437 bytes), including the
separate allocator diagnostic. This is still compilation evidence, not device output.

The added diagnostics in Android run 39493714456 identified the next blocker:
**HTTP 503 during depot_tools self-update**, after bootstrap clone succeeded.
The official tool source is now pinned to Chromium DEPS revision
`1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`, and self-update is disabled so a
reproducible checkout is not immediately replaced by an unversioned network fetch.
No ANGLE substitute, alternate driver or unofficial tool mirror is introduced.


Run 39494542779 passed the current host and patched ARM64 compile checks again.
Run 39494542835 passed the pinned source fetch, then reported
`python3_bin_reldir.txt not found`: disabling self-update on a fresh checkout also
requires explicit initialization of depot_tools' pinned Python/CIPD helpers.
`bootstrap-angle-tools.sh` now calls upstream `ensure_bootstrap` before invoking
gclient. This is the upstream initialization path named in the diagnostic; no
homemade Python marker or unverified binary was substituted.


## First successful APK build

Android run 39495542750 **passed** at source
`ed0f08e97c7272ab515daa206bf28cb3d4dadb56`, including real ANGLE compilation,
ABI/dependency closure, Android lint/build, debug signing verification and APK
publication. Artifact ID 12297629398 contains the actual PodNative-debug.apk,
checksums and build provenance. Runtime run 39495542649 also passed host execution
and patched ARM64 diagnostic compilation. Full evidence, download links and exact
limits are in [FIRST_APK_REPORT.md](FIRST_APK_REPORT.md). No device frame, gameplay
or Android CPU execution is claimed. The next required gate is the user's real
S23 Ultra graphics test; it cannot be verified from this unattached sandbox.
