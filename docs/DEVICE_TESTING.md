# AnyiOSEMU device acceptance

No device run of this HyperHLE-Reborn-to-AnyiOSEMU integration has been performed in the current workspace. A previously reported AnyiOS ANGLE surface test is not validation of the new runtime. This is a test checklist, not a pass report.

## Build and launch

1. Download a **successfully built** APK from the AnyiOS workflow. Check `BUILD-INFO.txt` and `SHA256SUMS`; note the app commit, AnyiOSEMU source pin and ANGLE pin. Install over the existing AnyiOS build when applicable.
2. Launch AnyiOS, open the shared Library and import a legally obtained IPA or extracted `.app` through the existing UI. Originals must remain untouched.
3. Check the architecture label and report. A thin ARM32 app is eligible; a universal ARM32+ARM64 app must state that ARM32 is selected; an ARM64-only app must remain unsupported. Encrypted or malformed imports must not be offered as runnable.
4. Open an eligible game's details and choose **Launch in AnyiOSEMU**. Confirm that the game's own menu or first screen appears—not the emulator's picker, a boot placeholder, a clear-color surface or the Android library UI.
5. Tap through the actual game menu. Confirm the runtime receives touch and that the visible state changes. Capture a screenshot only after observing real game output.
6. Open the saved session report. Check the AnyiOSEMU engine revision, selected ARM32 source, FPS cap, ANGLE/native-GLES preference, Force Composition, verbose-log setting and any initialization/fallback/error lines. A successful return or status 0 is not rendering proof.

## Graphics, input and lifecycle

- Confirm ANGLE is the driver that actually initialized in the default setting. The APK should contain `libEGL_angle.so`, `libGLESv1_CM_angle.so` and `libGLESv2_angle.so` for `arm64-v8a`. If ANGLE is unavailable, verify the log accurately identifies a native GLES fallback rather than claiming ANGLE.
- Retry with Settings ▸ **Native OpenGL ES** enabled, then with **Force Composition** toggled. Each setting should apply on the next AnyiOSEMU launch and be recorded in the report. Do not infer which mode is better without observing the game.
- Exercise touch targets, multi-touch if the game uses it, Android Back, pause/resume, lock/unlock and orientation changes. Record whether the menu/game returns, freezes, turns blank or exits. Check audio separately.
- Try 30/60/120 FPS limits and note actual device/panel behavior; a requested cap is not a measured frame rate. Verbose GLES logging is opt-in and may reduce performance.
- Run the separate **ANGLE surface test** as a graphics diagnostic. Its cube validates a different surface path and is not a substitute for game rendering.

## Storage and reports

- Relaunch the same game and confirm per-game AnyiOSEMU data is isolated and persists. Export its sandbox, inspect the archive for the runtime log/data, and verify rebuildable caches are excluded.
- Remove the imported library entry and confirm the original IPA/folder remains unchanged. Reimport if needed; do not upload game binaries or assets to issues/CI.
- Test import cancellation, encrypted/ARM64-only inputs, a universal ARM32+ARM64 input, an invalid archive, crash/forced-stop recovery, diagnostics export and app process recreation.
- If native crash logs are available, save them with the APK revision, device model, Android version, display mode and exact reproduction. Do not include game files.

## Current evidence matrix

| Check | Current status |
|---|---|
| Pinned AnyiOSEMU source, recursive submodules and reviewed patch | Exact-pin patch applies cleanly; `scripts/prepare-hyperhle.sh` passes locally |
| ARM32/ARM64/ARM64_32 FAT slice detection | `fat_test.cpp` passed with g++ `-Wall -Wextra -Werror`, including 20,000 mutations |
| AnyiOS JVM source tests | Added; not yet run in this workspace |
| Android ARM32/ARM64 universal-bundle labels and slice selection | Implementation updated; Java/Android regression pending |
| ANGLE 9addc298 source build, three-library audit and APK packaging | CI verification pending |
| Android lint, JVM tests and APK build | CI verification pending |
| AnyiOSEMU initialization on a real Android device | Not tested |
| Visible game menu and touch interaction | Not tested |
| ANGLE/native-GLES fallback, Force Composition and audio | Not tested |
| S23 Ultra FPS, thermals, pause/resume and orientation | Not measured |
| Game compatibility | Not established; must be tested title-by-title |
