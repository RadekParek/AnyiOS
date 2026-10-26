# AnyiOSEMU integration notes

## Scope

AnyiOS remains the Android app shell: the existing AnyiOS module chooser, library/import/export flow, settings and design remain in place, as does the separate Converter. The Android game engine has been switched to AnyiOSEMU, an integration of the pinned HyperHLE-Reborn/touchHLE source. This is a replacement of the game-execution backend, not a replacement of the AnyiOS product UI or Converter.

The visible runtime branding is **AnyiOSEMU**. Upstream legal notices, MPL-2.0 attribution and source provenance are intentionally retained. This integration does not claim that all games run.

## Reproducible engine source

- `third_party/hyperhle-revision.txt` records the exact HyperHLE-Reborn Git commit.
- `third_party/hyperhle` is the source submodule, including its pinned SDL, Dynarmic, libxml2, OpenAL Soft and stb submodules.
- `third_party/hyperhle-patches/AnyiOSEMU.patch` is the reviewed AnyiOS integration/rebranding patch. It is applied only at the recorded source revision.
- `scripts/prepare-hyperhle.sh` initializes/checks the source and nested revisions, applies or verifies the patch, and checks required notices.
- `third_party/hyperhle-boost-version.txt` and `scripts/prepare-hyperhle-boost.sh` provide the exact header dependency expected by the Rust Dynarmic wrapper.
- `app/build.gradle` builds the Android host and the Rust runtime for `arm64-v8a`; the guest CPU is ARM32/A32. Rust and Android source belong to their respective submodules.

Generated Boost, Cargo, NDK and ANGLE outputs remain ignored build assets; only source pins, patches, scripts and notices are versioned. The integration must not vendor a second, silently drifting copy of upstream engine code.

## Import-to-launch flow

1. AnyiOS retains one or more selected IPA files, a folder containing IPAs, or an extracted `.app` folder in its shared private library. Inspection parses bounded executable metadata, reports ARM32/ARM64/ARM64_32 separately and keeps the original file untouched.
2. Launch is enabled for supported, unencrypted ARM32 executables. If a universal executable contains ARM32 and ARM64 slices, inspection reports both and the engine receives the ARM32 launch path. ARM64-only executables stay unsupported.
3. IPA imports retain the canonical `source.bin`. AnyiOS prefers an `AnyiOSEMU_source.ipa` hard link for the runtime loader and falls back to a cancellable, free-space-checked copy on filesystems that reject links. Extracted `.app` imports pass a validated executable/app path.
4. `AnyiOSEmuActivity` extends the upstream SDL Android activity and forwards the selected game, a canonical per-game private data path, frame cap, device-family choice, ANGLE/native-GLES setting, Force Composition and verbose-GLES option through the runtime arguments. Invalid or missing launch arguments show actionable usage rather than silently opening the standalone picker.
5. AnyiOSEMU's patched Android entry point consumes those arguments, constructs the guest environment and loads the selected ARM32 app. The AnyiOS importer, host C++ diagnostics and Converter do not run the game.
6. The per-game data root and last selected path are persisted under `anyiose_runtime`; the DocumentsProvider uses that saved value if its transient activity state is unavailable and validates canonical containment under private app storage. Runtime logs and the engine pin are included in the bounded session report/export.

The activity must only expose private storage, refuse traversal/symlink escapes and avoid deriving a file path directly from an untrusted title. The source patch and current provider logic still require Android compilation and device review.

## Runtime controls and driver behavior

- FPS is clamped to the supported settings range and sent as an argument.
- ANGLE is the bundled default and native GLES is an explicit preference. The runtime records whether the requested driver initialized or whether it fell back.
- Force Composition and verbose GLES logs are opt-in runtime arguments.
- Device-family selection defaults to automatic. The emulated OS/device identity and UI label use AnyiOSEMU branding; app compatibility may depend on guest-side device identity, so do not change the guest profile casually.
- Runtime log volume is bounded before persistence; repetitive reporting must not overwhelm the UI or overwrite the first useful failure.

The independent C++ ANGLE surface test is not the SDL guest window. Neither a GL call count, instruction count nor process exit proves visible game rendering.

## Validation status and release gate

The exact source patch applies to an isolated worktree at the pinned commit, and `scripts/prepare-hyperhle.sh` passes against the local recursive checkout. CI preparation for Boost/Rust/ANGLE and Android packaging is configured but has not yet run; Cargo/Gradle build, APK package audit and device tests must pass before the integration is considered release-ready. The Android user experience requires a real ARM32 game menu to appear, touch to work, and the chosen graphics setting/fallback to match the report. See [`DEVICE_TESTING.md`](DEVICE_TESTING.md), [`ARCHITECTURE.md`](ARCHITECTURE.md) and [`ANGLE_INTEGRATION.md`](ANGLE_INTEGRATION.md).

Retain original import files; do not upload game IPAs or extracted app assets to the repository, CI, issue reports or test artifacts. No file under `converter/` is part of this backend change.
