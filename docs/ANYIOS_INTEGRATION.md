# AnyiOSEMU integration notes

## Scope

AnyiOS remains the Android app shell: its module chooser, library/import/export flow, settings and design are preserved, as is the separate Converter. The Android game-launch path uses the pinned Rust/SDL guest runtime under the first-party product name **AnyiOSEMU**. This is a replacement of the game-execution backend, not a replacement of the AnyiOS product UI or Converter. The Converter's conversion backend remains separate; its requested Force convert choices and shared-icon handling are part of the current UI work.

Upstream legal notices, MPL-2.0 attribution and source provenance are intentionally retained in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) and the packaged runtime notices. The application-facing name is AnyiOSEMU. This integration does not claim that all games run.

## Reproducible engine source

- `third_party/anyiose-runtime-revision.txt` records the exact upstream source commit.
- `third_party/hyperhle` is the source submodule, including its pinned SDL, Dynarmic, libxml2, OpenAL Soft and stb submodules.
- `third_party/anyiose-runtime-patches/AnyiOSEMU.patch` is the reviewed AnyiOS integration/rebranding patch. It is applied only at the recorded source revision.
- `scripts/prepare-anyiose-runtime.sh` initializes/checks source and nested revisions, applies or verifies the patch, and checks required notices.
- `third_party/anyiose-runtime-boost-version.txt` and `scripts/prepare-anyiose-runtime-boost.sh` provide the exact header dependency expected by the Rust Dynarmic wrapper.
- `app/build.gradle` builds the Android host and Rust runtime for `arm64-v8a`; the guest CPU is ARM32/A32. Rust and Android source remain in their respective dependency trees.

The paths above are retained as technical build/provenance identifiers. Generated Boost, Cargo, NDK and ANGLE outputs remain ignored build assets; only source pins, patches, scripts and notices are versioned. The integration must not vendor a second, silently drifting copy of upstream engine code.

## Import-to-launch flow

1. AnyiOS retains selected IPA files or extracted `.app` folders in its shared private library. Inspection parses bounded executable metadata, reports ARM64 before ARM32 for universal bundles, and keeps the original file untouched.
2. Launch is enabled for supported, unencrypted ARM32 executables. If a universal executable contains ARM32 and ARM64 slices, the runtime receives the ARM32 launch path. ARM64-only executables stay unsupported.
3. IPA imports retain the canonical `source.bin`. AnyiOS prefers an `AnyiOSEMU_source.ipa` hard link for the runtime loader and falls back to a cancellable, free-space-checked copy on filesystems that reject links. Extracted `.app` imports pass a validated executable/app path.
4. `AnyiOSEmuActivity` hosts the SDL Android activity and forwards the selected game, canonical per-game private data path, display-aware frame cap, scheduler/HUD options, device-family choice, ANGLE/native-GLES setting, Force Composition and verbose-GLES option through runtime arguments. Invalid launch arguments show usage rather than silently opening a standalone picker.
5. The patched Android entry point consumes those arguments, constructs the guest environment and loads the selected ARM32 app. The AnyiOS importer and host C++ diagnostics do not run the game.
6. Per-game data stays under private app storage. Runtime logs and settings are included in the bounded session report. Game details can export a browsable copy under `org.anyios` to a permitted SAF tree or save a ZIP; Android storage restrictions are respected.

The activity must only expose private storage to the runtime, refuse traversal/symlink escapes and avoid deriving a file path directly from an untrusted title. No unrestricted Android storage access is claimed.

## Runtime controls and driver behavior

- Auto FPS cap requests the fastest supported refresh mode at the current resolution. The requested and effective caps are reported separately from measured presentation FPS.
- The performance HUD is off by default and reports process CPU, RAM PSS, driver-reported graphics PSS and measured presentation FPS. GPU utilization is shown as unavailable if the platform does not expose it.
- Max clocks is off by default. It requests supported Android performance hints and high-capacity CPU affinity; the OS controls frequencies and may limit or ignore the request.
- ANGLE is the bundled default and native GLES is an explicit preference. The runtime records whether the requested driver initialized or whether it fell back.
- Force Composition and verbose GLES logs are opt-in runtime arguments.
- Device-family selection defaults to automatic. The emulated OS/device identity and UI label use AnyiOSEMU branding; guest-side identity can affect compatibility, so do not change the profile casually.
- Runtime log volume is bounded before persistence; repetitive reporting must not overwhelm the UI or overwrite the first useful failure.

The independent C++ ANGLE surface test is not the SDL guest window. Neither a GL call count, instruction count nor process exit proves visible game rendering.

## Validation status and release gate

Formatting, script syntax, source-patch checks and repository diff checks have passed locally. The current sandbox lacks Java, Gradle and the Android SDK, so the Android/Cargo build and JVM tests have not been run here. This PR's CI must pass before merging. No Android device test has been performed; the Android user experience still requires a real ARM32 game menu, touch response and settings/fallback behavior to be checked on-device. See [`DEVICE_TESTING.md`](DEVICE_TESTING.md), [`ARCHITECTURE.md`](ARCHITECTURE.md) and [`ANGLE_INTEGRATION.md`](ANGLE_INTEGRATION.md).

The native Converter test script is intentionally not run in this task. Retain original imports; do not upload game IPAs or extracted app assets to the repository, CI, issue reports or test artifacts.
