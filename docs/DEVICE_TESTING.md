# S23 Ultra graphics acceptance

No connected device, Android emulator or captured image was available during
implementation. The user reported the previous 3D test working on their S23 Ultra;
no screenshot/log was supplied. The new library APK is built (see LIBRARY_APK_REPORT.md),
but the new UI/import flow still needs device review. This is a checklist, not a test report.

1. Download a **successfully built** `PodNative-debug.apk` CI artifact. Review
   `BUILD-INFO.txt` and SHA256SUMS. Install with `adb install -r PodNative-debug.apk`.
2. Clear logcat: `adb logcat -c`; launch `org.podnative/.MainActivity`.
3. Diagnostics → Run graphics test. Confirm an animated checker-textured 3D cube,
   perspective and correct self-occlusion, not just a clear color or static UI.
4. Confirm diagnostics identify embedded ANGLE, actual GL version/renderer, Vulkan
   candidate/driver, dimensions and no shader/link/GL/EGL/framebuffer errors.
5. Capture evidence **only if visible output succeeds**:
   `adb exec-out screencap -p > PodNative-S23Ultra.png` and
   `adb logcat -d -s PodNative ANGLE > PodNative-S23Ultra.log`.
   FIRST_PRESENT means swap returned successfully; it is not screenshot evidence.
6. Background/foreground ten times, lock/unlock, rotate portrait/landscape, stop and
   relaunch. Confirm visible scene restores, no stale surface crash and resources
   are released/recreated. Changing resolution applies on the next launch.
7. Run 720/1080/1440 short-edge caps when the display configuration allows. Run
   FHD and QHD display modes; record actual render dimensions, not setting labels.
   Test 30/60/120 caps and actual panel refresh rates. Vsync/backpressure can limit
   frame throughput; do not report requested caps as measured FPS.
8. Collect 5-minute traces and log actual present count/interval, draw+swap duration,
   memory and thermal behavior. Use Perfetto/GPU tools for CPU/GPU split,
   frame-interval distributions and shader compile time. Current in-app metrics
   provide only basic CPU-wall draw+swap and Java heap samples; no GPU/audio or
   guest CPU measurement is implemented. Confirm no memory growth across relaunches.
9. Exercise cancel/invalid import, malformed ZIP, oversized executable, encrypted
   executable, remove imports, log export, settings persistence and process recreation.
10. Return revision, Android build, display mode, GL renderer/version, logs, screenshot,
    observed errors and exact reproduction. Never attach game binaries/assets.

## Current test matrix

| Test | Result |
|---|---|
| Host parser/memory/loader/FAT + 55,000 malformed/mutated inputs | Passed locally with ASan/UBSan |
| Real Dynarmic ARM/Thumb execution on x86-64 host | Passed |
| Android compile / lint | Passed in CI run 39501999659 |
| Source-pinned ANGLE build / dependency audit | Passed in CI run 39495542750 |
| APK produced/published | Yes; see LIBRARY_APK_REPORT.md |
| Embedded Vulkan ANGLE init | Previous 3D test user-reported working; no backend log supplied |
| Textured 3D cube / real screenshot | Previous test user-reported working / no screenshot |
| Surface recreation, pause/resume, rotation | Implemented source paths; device tests pending |
| S23 Ultra FHD / QHD / 60 / 120 FPS | Not measured |
| Game stages A–G | None reached |

## Library acceptance (new preview)

- Import your own IPA and the equivalent extracted `.app` folder. Verify title,
  version, ARM32/universal status, retained size and artwork/fallback. Never upload
  game files to this repository, CI or issue reports.
- Restart/force-stop/reopen: entries, rename and settings must persist. Rotate or
  background during copying; progress must continue without duplicate publication.
- Cancel a large import; previous entries must remain and staging must be removed.
  Provider-blocked reads may delay cancellation. Kill the process during an import,
  reopen, and verify interrupted staging is cleaned and no incomplete entry appears.
- Try ARM64-only, encrypted ARM32, malformed IPA and universal ARM32 binaries.
  Expect explicit status/errors; successful imports must not imply game launch.
- Export an import report and diagnostic log. Remove a private copy and verify the
  original IPA/folder is untouched. Check light/dark/system, large font and rotation.
