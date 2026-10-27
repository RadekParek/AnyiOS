# AnyiOSEMU device acceptance

No device run of the pinned runtime integration has been performed in this workspace. A previously reported AnyiOS ANGLE surface test is not validation of the Android game runtime. This is a test checklist, not a pass report.

## Build and launch

1. Download an APK only after its workflow build succeeds. Check `BUILD-INFO.txt` and `SHA256SUMS`; note the app commit, AnyiOSEMU source pin and ANGLE pin. Install over the existing AnyiOS build when applicable.
2. Launch AnyiOS, open the shared Library and import a legally obtained IPA or extracted `.app` through the existing UI. Originals must remain untouched.
3. Check the architecture label and report. A thin ARM32 app is eligible; a universal ARM64+ARM32 app must state that ARM32 is selected; an ARM64-only app must remain unsupported. Encrypted or malformed imports must not be offered as runnable.
4. Open an eligible game's details and choose **Launch in AnyiOSEMU**. Confirm that the game's own menu or first screen appears—not a picker, boot placeholder, clear-color surface or Android library UI.
5. Tap through the actual game menu. Confirm that the runtime receives touch and the visible state changes. Capture a screenshot only after observing real game output.
6. Open the saved session report. Check engine revision, selected ARM32 source, requested and effective FPS caps, measured presentation FPS if available, ANGLE/native-GLES preference, Force Composition, HUD/Max clocks requests and any initialization/fallback/error lines. A successful return or status 0 is not rendering proof.

## Graphics, input and lifecycle

- Confirm ANGLE is the driver that actually initialized in the default setting. The APK should contain `libEGL_angle.so`, `libGLESv1_CM_angle.so` and `libGLESv2_angle.so` for `arm64-v8a`. If ANGLE is unavailable, verify the log accurately identifies a native GLES fallback rather than claiming ANGLE.
- Retry with Settings ▸ **Native OpenGL ES** enabled, then with **Force Composition** toggled. Each setting should apply on the next AnyiOSEMU launch and be recorded in the report. Do not infer which mode is better without observing the game.
- Compare the main emulator and a standalone APK at their default display settings on both 1080p and higher-resolution screens. Confirm the main game image is not confined to part of the view. Exercise touch targets at the center and edges, multi-touch if the game uses it, Android Back, pause/resume, lock/unlock and orientation changes. Record whether the menu/game returns, freezes, turns blank or exits. Check audio separately.
- Test **Auto** and fixed FPS caps on supported modes. Compare the requested/effective cap with the HUD's measured presentation FPS; the cap is not a promise that the display or game will sustain it.
- With the HUD enabled, verify process CPU, RAM PSS, driver-reported graphics PSS and measured FPS. GPU utilization may correctly show unavailable. Enable **Max clocks** only for an explicit test; Android may ignore or limit the best-effort hint and it cannot force clock rates.
- Run the separate **ANGLE surface test** as a graphics diagnostic. Its cube validates a different surface path and is not a substitute for game rendering.

## Converter and dynamic resolution

- From the Converter's Force convert dialog, confirm **Native runtime** produces the existing diagnostic game-runtime artifact. Confirm **Semi-Emulated** generates a separately named/signed APK with the embedded IPA and runtime, refuses encrypted/ARM64-only inputs, and opens only that game without a picker.
- Install the Semi-Emulated APK and check both first and repeat launch: no preparation or emulator crash/setup-error screen should appear. Try a controlled guest error and a failed payload verification; verify ordinary in-game alerts still work. Test early native failures and ANRs separately because Android may show system UI before handlers run; native quiet exits intentionally lack Android tombstones. Remove AnyiOS, reboot and launch it again. Verify its private saves persist and no host application is required; then repeat after an APK update signed with the same key. Check actual maximum resolution/refresh on a high-refresh device.
- Test classic phone and tablet **Game layout** profiles with fixed-size games in portrait and landscape, including letterbox edges and any native WebView controls. Check proportional sizing and hitboxes. Bundles that reject the selected device family can fall back.
- Verify Dynamic resolution starts Off, that the selected minimum is respected, and that changing the range takes effect on the next game launch. At 90%, 75% and 50% compare the physical game/view bounds against Off in the same scene: lowering buffer resolution must not shrink the displayed image (an explicitly selected classic layout can retain its normal aspect-ratio bars). Check both bundled ANGLE and native GLES, touch targets and WebView overlays, portrait/landscape rotation, background/resume and surface recreation at every scale. Compare measured FPS and image detail with Off, including a CPU-bound scene. A driver may ignore a requested fixed buffer, and the presentation surface alone may not affect offscreen rendering workload; do not claim gains without measurement.
- Test games requesting 60 FPS and >60 FPS separately on a 120-Hz device. Confirm **Maximize FPS** is off by default in the Emulator, and compare game speed and measured FPS when enabled; standalone APKs request boost. It may accelerate callback-driven simulation and cannot override every game-specific cap.

## Storage, icons and reports

- Relaunch the same game and confirm private per-game AnyiOSEMU data persists and remains isolated. Export to a permitted SAF tree and verify that the browsable copy is under `org.anyios`; also test ZIP export. Do not expect the picker to grant storage-root, Downloads, `Android/data` or `Android/obb` access on restricted Android versions.
- Check that Emulator and Converter show the same recovered game artwork while retaining their existing UI tile sizes. Include missing-artwork and generated-icon cases.
- Remove the imported library entry and confirm the original IPA/folder remains unchanged. Reimport if needed; do not upload game binaries or assets to issues/CI.
- Test import cancellation, encrypted/ARM64-only inputs, a universal ARM32+ARM64 input, an invalid archive, crash/forced-stop recovery, diagnostics export and app process recreation.
- If native crash logs are available, save them with the APK revision, device model, Android version, display mode and exact reproduction. Do not include game files.

## Current evidence matrix

| Check | Current status |
|---|---|
| Pinned source and runtime patch | Full tracked patch reverse-check and exact worktree comparison passed; native build awaits CI |
| ARM32/ARM64/ARM64_32 FAT slice detection | Previously passed standalone warning-clean g++ regressions; current Android/JVM tests are not run locally |
| Artwork resolution and shared-icon fallback | Implementation and regression tests updated; tests not run locally |
| SAF folder/ZIP export and Android restrictions | Implementation review only; no device/provider test |
| Converter Force convert choice and APK contracts | Routing, integrity/DRM and native-library closure tests added; focused JVM tests passed with temporary stubs; real APK/device tests pending |
| Repository formatting, script syntax and diff checks | Passed locally |
| Android lint, JVM tests, native/Cargo compilation and APK packaging | Pending this PR's CI |
| AnyiOSEMU initialization on a real Android device | Not tested |
| Visible game menu and touch interaction | Not tested |
| ANGLE/native-GLES fallback, Force Composition, audio and performance settings | Not tested on device |
| GPU utilization and S23 Ultra FPS/thermals | Not measured; GPU utilization is not exposed portably |
| Game compatibility | Not established; must be tested title-by-title |
