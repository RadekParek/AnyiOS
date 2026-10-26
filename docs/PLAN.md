# AnyiOSEMU runtime integration plan

## Scope

Preserve the existing AnyiOS Android shell, shared library, import/export screens, settings and design. Keep Converter as a separate module, with the requested Force convert runtime choice and shared artwork behavior. Use the pinned upstream guest runtime under the first-party **AnyiOSEMU** product name, while retaining upstream notices, licensing and dependency provenance. The guest slice remains ARM32; ARM64-only guest execution is not part of this integration. Do not declare a game compatible until a real menu is visible and interactive on an Android device.

## Work sequence

1. **Pin source and architecture.** Keep the runtime source and recursive dependencies at exact revisions recorded by the repository. Maintain the Android host ABI `arm64-v8a` and supported guest slice ARM32.
2. **Preserve app behavior and integrate launch.** Keep library/import/settings/Converter ownership in AnyiOS. Forward a validated selected game, per-game private data path and saved runtime options into the SDL host. Use ARM32 for a universal ARM32+ARM64 binary; keep ARM64-only unavailable.
3. **Maintainability and legal review.** Put upstream source changes into a reproducible exact-pin patch; verify attribution/license files, first-party branding, private storage boundaries and actionable reports.
4. **Build inputs and graphics.** Provision recursive vendor submodules, exact Boost, Rust target/cargo-ndk and pinned ANGLE. Build required EGL/GLES libraries, verify alias copies, audit dependencies and check APK contents.
5. **Automated verification.** Run source-patch checks, C++ FAT/inspection regressions, JVM tests, formatting/lint, Gradle/Cargo builds and CI package checks. Do not run `scripts/test-converter.sh` in this task.
6. **Device acceptance.** On a real device verify actual game menu pixels, touch response, audio, orientation/lifecycle, private per-game saves and SAF exports, display-aware FPS cap/HUD, scheduler-hint disclosure, ANGLE/native-GLES selection and Force Composition.
7. **Report honestly.** No compatibility, performance or rendering claim follows from compilation, host-runner results, instruction counts, GL calls, swaps or exit status alone. Retain old prototype evidence only as explicitly historical.

## Current completion state

The pinned source integration, ARM32 launch path, ANGLE pin and CI setup are in place. This follow-up adds game-data folder/ZIP export, display-aware FPS controls, performance HUD and best-effort scheduler requests, higher-resolution shared artwork, architecture ordering and Converter runtime choices. Repository formatting and script syntax checks pass locally. Full Android lint/build/tests are pending CI; device validation is still required. See [`PROGRESS.md`](PROGRESS.md) and [`DEVICE_TESTING.md`](DEVICE_TESTING.md) for the live evidence state.
