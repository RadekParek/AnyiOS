# Foundation implementation report

Date: 2026-10-20

## Source identity

Repository: `RadekParek/PodNative`.
Branch: `arena/568194d8-podnative`.
Base revision: `dc703553b265b07365ad7d393a042ad5a1a82f6e`.
This implementation is a working-tree change on that revision, not a claimed new
committed/released revision. CI embeds the actual `GITHUB_SHA` in the application and
BUILD-INFO.txt when built. No push, Actions run or GitHub Release was performed.

## Created modules and files

* Root Android build: `settings.gradle`, `build.gradle`, `gradle.properties`,
  `app/build.gradle`, manifest and platform theme resources.
* Native Android UI: `MainActivity.java` (library/inspection, settings, launch/stop,
  fullscreen test, storage picker and export), `Diagnostics.java` (bounded logs),
  `PrototypeView.java` (graphics owner thread/lifecycle/pacing), `NativeBridge.java`.
* Original native code: `graphics.h` (PodNative boundary), `renderer.cpp`
  (upstream ANGLE EGL/GLES backend and synthetic textured cube), `bridge.cpp`
  (JNI), `macho.h/.cpp` (bounded structural inspection), `CMakeLists.txt`.
* Tests: `tests/macho_test.cpp`, sanitizer host runner, format/script lint runner.
* Upstream integration: `third_party/angle/revision.txt` and retained LICENSE;
  source-build, dependency-closure audit and notice-collection scripts.
* CI: debug entry workflow, shared build workflow, tagged-release workflow,
  optional signing, artifact/checksum/source metadata and failure logs.
* Documentation: research, implementation plan, independent architecture, ANGLE
  strategy, Ninjago status, device tests, this report and third-party notices.

No inherited renderer, game binaries/assets, signing keys, fake ANGLE binaries,
placeholder iOS framework APIs or fabricated scene screenshots were created.

## Real ANGLE status

The source integration targets **actual Google ANGLE**, pinned at
`378b820f1c00d511565ab9990ae03de77908b373`, app-embedded with distinct library
names. CMake requires those exact libraries. Runtime uses public EGL platform
selection for Vulkan, preflights baseline Vulkan requirements and reports failures.

**Upstream ANGLE has not been built, packaged, initialized or runtime-verified here.**
The code is an integration implementation, not evidence of a successful integration
run. No system ANGLE assumption or native-GLES substitution is used.

## Tests actually run

| Check | Observed result |
|---|---|
| C++ parser regressions, ASan and UBSan | Passed |
| 25,000 deterministic malformed inputs in same sanitizer test | Passed; no sanitizer report |
| clang-format 19.1.7, C++/Java | Passed |
| Ruff 0.11.13 lint/format, Python scripts | Passed |
| Python byte-compilation | Passed |
| Bash syntax, each script | Passed |
| Three workflow files, PyYAML 6.0.2 syntax parsing | Passed; not GitHub Actions execution/semantic validation |
| Whitespace check | Passed for tracked diff; new source separately formatting-checked |
| `scripts/build-angle.sh` | Explicit prerequisite failure: `Install depot_tools first` |
| Android compile, Android lint, JVM/instrumentation tests | Not run |
| Real ANGLE source build and ELF dependency audit | Not run |
| CI Actions and APK artifact publication | Not run |

Host: Linux x86-64 sandbox. No JDK, Gradle, Android SDK/NDK, Rust compiler, ADB or
connected device was initially available. Python formatting tools were installed
into ignored `.local` tooling. Chromium's required dependency hosts are outside the
sandbox allowlist. These are build-environment limitations, not proof that the
Android code or CI recipe compiles correctly.

## Rendering and device evidence

Textured 3D test code: implemented (ES2 shader compile/link, generated texture,
VBO/IBO cube, perspective rotation, depth testing, framebuffer validation and swap).
Actually rendered: **not verified**.
Device/backend used for rendering tests: **none**.
Samsung S23 Ultra test: **pending user installation of a successful APK**.
Screenshot: **none**. Performance: **not measured**.
Ninjago: no stages A–G achieved; structural inspection is not executable loading.

## APK location

**No APK exists from this work session.** Expected paths only after successful builds:

* Local debug: `app/build/outputs/apk/debug/app-debug.apk`.
* CI debug artifact: `PodNative-debug.apk` (archive also contains checksums/provenance).
* Tagged CI release artifact: `PodNative.apk`; unsigned/non-installable unless
  signing secrets are configured. Public release publication is opt-in and checks
  signing metadata; license/device review remains mandatory before enabling it.

## Known limitations

The requested installable/verified graphics milestone is **not complete**. Android
source/CI need a real compilation pass and any resulting errors diagnosed. No CPU,
loader/mapping/relocations, Objective-C runtime, iOS frameworks, guest GLES/EAGL,
Core Animation, PVRTC compatibility, gameplay, touch overlay, audio or saves exist.
The UI exposes only implemented settings; it does not imply these missing features
work. Imports are private, capped and inspection-only, not a playable game library.

Only Vulkan is implemented; no fallback has been tested. ANGLE's feature negotiation
remains authoritative beyond baseline preflight. A successful swap is not visual
verification. GPU timing, accurate frame-interval distributions, native memory and
audio/CPU guest profiling remain to be added. Synchronous worker teardown can block
on a stalled driver; exercise and harden it during lifecycle testing. Bootstrap
depot_tools is recorded but not pinned; full build hermeticity and transitive
license/distribution review are incomplete. No project source license was imposed
without the owner's decision.

## Next three concrete tasks

1. Execute the new CI in an unrestricted Linux environment; fix real GN/dependency,
   native, Java/lint or packaging failures; audit built dependency licenses/ABI and
   produce an installable, checksummed debug APK. Pin bootstrap tooling after the
   first proven build.
2. Install it on the user's S23 Ultra. Capture actual textured cube output and
   renderer/driver logs; test pause/resume/rotation/lock and FHD/QHD. Diagnose all
   errors and lifecycle stalls before declaring the graphics milestone passed.
3. Review/integrate Dynarmic A32 on AArch64 with synthetic instruction/ABI tests;
   extend Mach-O inspection into a validated loader and start tracing the user's
   Ninjago copy privately. Implement actual observed framework/graphics blockers.
