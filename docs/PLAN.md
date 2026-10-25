# AnyiOSEMU engine-replacement plan

## Scope

Keep the existing AnyiOS Android shell, shared library/import screens, settings and design, and leave the Converter unchanged. Replace the Android game-execution backend with pinned HyperHLE-Reborn/touchHLE source, branded visibly as **AnyiOSEMU**. Preserve upstream legal attribution. Upgrade the pinned ANGLE EGL/GLES libraries used by the engine. Do not declare a game compatible until a real menu is visible and interactive on an Android device.

## Work sequence

1. **Pin source and architecture.** Register the HyperHLE-Reborn submodule at an exact commit with its recursive dependencies; record engine and Boost pins. Keep the Android host ABI `arm64-v8a` and the supported guest slice ARM32.
2. **Preserve app behavior and integrate launch.** Keep library/import/settings/Converter ownership in AnyiOS. Forward a validated selected game, per-game private data path and saved runtime options into the SDL host. Use ARM32 for a universal ARM32+ARM64 binary; keep ARM64-only unavailable.
3. **Maintainability and legal review.** Put all upstream source changes into a reproducible exact-pin patch; verify attribution/license files, branded UI strings, private storage boundaries and actionable reports.
4. **Build inputs and graphics.** Provision recursive vendor submodules, Boost 1.81.0, Rust target/cargo-ndk and pinned ANGLE. Build the required EGL, GLES1.1 and GLES2 libraries, verify alias copies, audit dependencies and check APK contents.
5. **Automated verification.** Run source-patch checks, C++ FAT/inspection regressions, JVM tests, formatting/lint, Gradle/Cargo builds and CI package checks. Do not edit `converter/` or rerun the previously aborted `scripts/test-converter.sh`.
6. **Device acceptance.** On a real device verify actual game menu pixels, touch response, audio, orientation/lifecycle, private per-game saves, ANGLE/native-GLES selection and Force Composition. Capture reports/logs only for observed outcomes.
7. **Report honestly.** No compatibility, performance or rendering claim follows from compilation, host-runner results, instruction counts, GL calls, swaps or exit status alone. Update docs to the current backend; retain old prototype evidence only as explicitly historical.

## Current completion state

The submodule, exact-pin source patch/preparation script, runtime launch path, ARM-slice reporting, ANGLE pin and CI setup are in place. Patch preparation passes locally, but full lint/build/package checks and device validation are not complete; refer to [`PROGRESS.md`](PROGRESS.md) and [`DEVICE_TESTING.md`](DEVICE_TESTING.md) for the live evidence state.
