# AnyiOSEMU integration notes

## Scope

AnyiOS remains the Android app shell: its module chooser, library/import/export flow, settings and design are preserved, as is the separate Converter. The Android game-launch path uses the pinned Rust/SDL guest runtime under the first-party product name **AnyiOSEMU**. This is a replacement of the game-execution backend, not a replacement of the AnyiOS product UI or Converter. Converter keeps its existing native diagnostic backend and can also package an independent one-game AnyiOSEMU APK.

Upstream legal notices, MPL-2.0 attribution and source provenance are intentionally retained in [packaged runtime notices](../app/src/main/assets/anyiose-runtime-notices.txt). The application-facing name is AnyiOSEMU. This integration does not claim that all games run.

## Reproducible engine source

- `third_party/anyiose-runtime-revision.txt` records the exact upstream source commit.
- `third_party/anyiose-engine` is the source submodule, including its pinned SDL, Dynarmic, libxml2, OpenAL Soft and stb submodules.
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
- The manual presentation-resolution setting is hidden and the base presentation surface always uses the native view size, matching the standalone host. Any previously saved manual presentation or legacy graphics-test resolution is ignored, including after upgrades. Optional dynamic resolution can still reduce the presentation buffer below this native base without changing guest logical dimensions or game-owned offscreen buffers.
- Dynamic resolution is off by default. Its selectable minimum scale controls the Android SDL presentation surface based on measured swaps, with hysteresis and restoration if FPS does not improve. It does not currently resize all guest EAGL/offscreen buffers, so it may not improve performance. SDL queries the active EGL drawable for its viewport and falls back to its window size when the surface is unavailable; WebView bounds use the same drawable dimensions, and touch normalization uses the view dimensions. These alignment paths require physical-device validation, including when a driver does not honor a requested buffer size.
- The CADisplayLink implementation honors explicit higher-FPS requests up to the effective display cap. The off-by-default **Maximize FPS** setting can promote 60-FPS display links to that cap; callback-driven games may run too fast. Other FPS requests stay unchanged.
- The performance HUD is off by default and reports process CPU, RAM PSS, driver-reported graphics PSS and measured presentation FPS. GPU utilization is shown as unavailable if the platform does not expose it.
- Max clocks is off by default. It requests supported Android performance hints and high-capacity CPU affinity; the OS controls frequencies and may limit or ignore the request.
- ANGLE is the bundled default and native GLES is an explicit preference. The runtime records whether the requested driver initialized or whether it fell back.
- Force Composition and verbose GLES logs are opt-in runtime arguments.
- Game layout defaults to automatic. Classic 320×480, 320×568 and 768×1024 iOS-point profiles can be selected for games with fixed-size layouts; the viewport preserves aspect ratio and input uses the same viewport. Bars may remain and unsupported device families fall back according to the bundle. Guest-side identity can affect compatibility.
- Runtime log volume is bounded before persistence; repetitive reporting must not overwhelm the UI or overwrite the first useful failure.

The independent C++ ANGLE surface test is not the SDL guest window. Neither a GL call count, instruction count nor process exit proves visible game rendering.

## Converter runtime choices

**Native runtime** runs the existing `game-runtime-v1` boot-attempt builder (previously the Semi-Emulated choice), not the strict single-routine converter. **Semi-Emulated** builds a distinct `anyiose-single-game-v1` APK with the retained IPA, AnyiOSEMU, SDL, ANGLE dependency closure and notices. Its transparent launcher verifies and copies that one IPA off the UI thread into private storage, without a preparation or setup-error screen, and starts it directly using the current display mode/maximum available refresh. It enables experimental 60-FPS display-link promotion by default, which can accelerate game logic. Emulator error and SDL startup dialogs are disabled for this host, but ordinary in-game dialogs are not. The standalone Java exception handler and native fatal-signal policy attempt to close quietly and retain log markers; fatal native exits do not generate Android tombstones. Android can still display its own UI for ANRs, crashes before the handlers are installed, or other system-level failures. No AnyiOS installation, library picker or app data is needed after installation. The Semi-Emulated builder rejects unverified/encrypted ARM32 source; ARM64-only is unsupported by AnyiOSEMU. The existing Native diagnostic builder retains its previous source eligibility. Generated metadata is source/signer/hash-bound and explicitly does not claim gameplay verification.

## Validation status and release gate

Formatting and a full pinned-patch reverse/applicability check passed locally. Java and Kotlin sources and focused JVM controller, source-validation and library-closure tests were checked with temporary Android/signing stubs. These checks do not validate Gradle, signing, native/Cargo compilation or APK packaging; those remain CI gates. No Android device or performance test has been performed. See [`DEVICE_TESTING.md`](DEVICE_TESTING.md) and [`ANGLE_INTEGRATION.md`](ANGLE_INTEGRATION.md). Retain original imports and do not upload game IPAs or extracted app assets to the repository, CI, issues or test artifacts.
