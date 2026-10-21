# AnyiOS

One Android app with **Emulator** and **Converter** modules and a shared private source library.
The emulator is the independent former PodNative ARM32/ANGLE project; the converter
integrates the owner's RadekiOSconventor source at a recorded revision.
**Real-game compatibility remains unfinished. A successful build or generated
experimental APK is not proof that Ninjago (or another game) runs.**

- Launch into an Emulator / Converter chooser with a common teal design and shared appearance settings.
- Import IPAs in either module; both use `files/games/<id>/source.bin`, not duplicate IPA copies.
- Import a folder of IPAs recursively as separate entries; extracted .app import remains separate.
- Converter analysis and generated APKs are separate from the immutable original source.
- Confirmed removal deletes the shared entry from both modules. Existing emulator data and Android package id `org.podnative` are preserved. A separately installed converter's private storage cannot be read automatically; reimport its original IPAs.

[Integration, tests and boundaries](docs/ANYIOS_INTEGRATION.md) ·
[Converter fork review](docs/CONVERTER_FORK_REVIEW.md).
The remote GitHub repository rename was denied by the integration's admin permissions;
its URL remains `RadekParek/PodNative` until the owner renames it in repository settings.
Technical JNI namespaces, signing/cache names and historical engineering records intentionally retain their original names.

[Download current Android and PC diagnostic artifacts](https://github.com/RadekParek/PodNative/actions/workflows/android.yml)
(choose a successful run; extract GitHub's archive, then install `AnyiOS-debug.apk`).
The PC artifact is a Linux CPU diagnostic build, not a desktop emulator GUI.
[Current implementation boundaries](docs/GRAPHICS_FRAMEWORK_INCREMENT.md) ·
[Linker/dispatch increment](docs/DYLD_AND_IMPORT_INCREMENT.md).
[Historical 0.2.1 build evidence](docs/STARTUP_APK_REPORT.md).
[Historical first APK report](docs/FIRST_APK_REPORT.md).

## First slice

* Native Android library/inspection flow, settings, diagnostics and log export.
* PodNative-owned graphics backend; source-pinned upstream ANGLE embedded Vulkan
  integration, with explicit failure and no disguised native-GLES fallback.
* Shader-based animated textured cube with depth testing and reusable resources.
* Bounded ARM32 Mach-O inspection and temporary validated guest-memory mapping.
* Actual Dynarmic CPU adapter with host-tested ARM/Thumb execution and an in-app CPU self-test; device execution and game-session integration remain pending.
* HLE linker core: session-owned two-level export graph, deterministic strong/weak
  coalescing, transactional graph-aware fixups, and authenticated ARM `SVC` call
  gates; launch reports required-import coverage instead of claiming resolution.
* GitHub Actions debug/tagged-release build recipes with artifacts and provenance.

No inherited TouchHLE/HyperHLE renderer. No proprietary game files. No universal
compatibility or performance claims. Ninjago stages A–G are all unverified.

## Build / test

Host: `scripts/test-host.sh` (g++/Python required).
Real CPU engine tests: `scripts/test-cpu.sh` (CMake/C++20/git required).
See [runtime contracts and device test](docs/RUNTIME.md).

Android: follow [ANGLE integration](docs/ANGLE_INTEGRATION.md), then use Gradle
8.11.1/JDK17: run `scripts/prepare-cpu.sh`, `scripts/prepare-converter.sh` and the ANGLE preparation first, then
`gradle :app:lintDebug :converter:lintDebug :app:testDebugUnitTest :converter:testDebugUnitTest :app:assembleDebug`.
Converter host checks: install test-only `capstone==5.0.7` and `unicorn==2.1.4`, then run `scripts/test-converter.sh`.
Proprietary game-dependent upstream tests are explicitly skipped unless a locally supplied authorized fixture exists; CI never fetches one.
Real upstream ANGLE libraries are mandatory build inputs; no stub mode exists.
See [device tests](docs/DEVICE_TESTING.md) before claiming rendering success.

CI debug artifacts are published only after a successful workflow and are named `AnyiOS-debug.apk`.
Tagged builds produce `AnyiOS.apk`, SHA256SUMS and BUILD-INFO.txt. Without
signing secrets the release output is explicitly unsigned and not installable.
Public GitHub Release publication additionally requires the repository variable
`PODNATIVE_PUBLISH_RELEASE=true` after license/device review and configured signing.

## Engineering records

[Current progress](docs/PROGRESS.md) · [Runtime](docs/RUNTIME.md) · [Plan](docs/PLAN.md) · [Research](docs/RESEARCH.md) ·
[Architecture](docs/ARCHITECTURE.md) · [ANGLE](docs/ANGLE_INTEGRATION.md) ·
[Ninjago status](docs/NINJAGO_COMPATIBILITY.md) ·
[Third-party notices](THIRD_PARTY_NOTICES.md) · [Build report](docs/BUILD_REPORT.md)

### 0.2 library preview

This preview adds persistent private IPA/folder imports, ARM32 selection in
universal Mach-O containers, a library-first light/dark UI and an original launcher
icon. Imported does **not** mean runnable. See [library limits, workflow and update
signing caveats](docs/LIBRARY_PREVIEW.md). The previous graphics preview was
reported working by the user on their S23 Ultra; this does not validate gameplay
or the new UI.

### Startup preview follow-up

This APK removes the fixed 2 GiB import quota and library entry cap, validates
`LC_DYSYMTAB`, and enables a real native-loader startup attempt with persistent,
exportable blocker reports. It does not execute guest code yet. Resource/parser
safety bounds and low-space checks remain; see [current behavior](docs/LIBRARY_PREVIEW.md).

### 0.3 graphics/framework foundation

Unified Android/PC diagnostic CI, a new handheld launcher mark, animated native
UI, actual imported launch-artwork presentation through ANGLE (max 3 × 900 ms),
CoreFoundation bit-vector/lifetime services, CoreGraphics affine math and an
app-linked ARM/Thumb CPU self-test. These are **not playable-game compatibility**;
[implementation boundaries and device checklist](docs/GRAPHICS_FRAMEWORK_INCREMENT.md).
Local Android builds now also require `scripts/prepare-cpu.sh` before Gradle.

### 0.4 linker/import preview

ARM32 pointer rebase/bind and legacy indirect-symbol decoding, a tested transactional
fixup applier, retained ARM64/broken imports with reasons, legacy CgBI artwork
conversion, and a green pocket-player visual identity. Required exports and guest
bootstrap remain unresolved: **this is not a playable Ninjago build**.
[Scope, official Apple format references and tests](docs/DYLD_AND_IMPORT_INCREMENT.md).
