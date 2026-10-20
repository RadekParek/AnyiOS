# First published debug APK — build evidence

2026-10-20. **Android compilation milestone passed. Device-rendering and game
compatibility milestones have NOT passed.** This supersedes the initial build
report's statements that no APK exists.

## Exact source and artifacts

* Build source: `ed0f08e97c7272ab515daa206bf28cb3d4dadb56`.
* Branch: `arena/568194d8-podnative`.
* Android build: https://github.com/RadekParek/PodNative/actions/runs/39495542750
* Download artifact: https://github.com/RadekParek/PodNative/actions/runs/39495542750/artifacts/12297629398
* Artifact name: `PodNative-debug.apk`. GitHub downloads an archive containing the
  actual APK, `SHA256SUMS`, and `BUILD-INFO.txt`. Extract it before installation.
* Published archive size: 6,896,820 bytes. GitHub archive SHA-256:
  `77959e57eb85fb6ca68e5d067c5cbd3143aa31f7213023c1d7a287095ad5cf00`.
  This is **not the APK file checksum**; use the enclosed SHA256SUMS for the APK.
* The APK is debug-signed. `apksigner verify --print-certs` passed in CI.
* Packaging checks confirmed `lib/arm64-v8a/libEGL_podnative.so` and
  `lib/arm64-v8a/libGLESv2_podnative.so` are in the APK.
* No release APK or public GitHub Release was produced. Debug artifacts are subject
  to GitHub retention/access policies.

## What was actually built and tested

The Android workflow completed successfully in 13m26s:

* Android SDK/NDK and JDK/Gradle setup.
* C++/Java formatting, Python lint/format and shell syntax.
* Sanitized parser/memory/loader/import-evidence tests: 25,000 malformed inputs,
  10,000 mutated executables, targeted regressions and 5 ELF-auditor unit tests.
* **Real upstream ANGLE** source compilation, actual ELF64/AArch64 dependency
  closure audit, pinned-source check and caching of verified outputs.
* Android lint, applicable Gradle unit-test task, native/Java compilation and
  debug APK assembly. No Android instrumentation/UI or device tests ran.
* Debug signing verification, ANGLE packaging checks, checksum/provenance creation,
  artifact publication and build/test report publication.

The separate runtime workflow also passed at the same source revision:
https://github.com/RadekParek/PodNative/actions/runs/39495542649

It executed real Dynarmic ARM/Thumb fixtures on x86-64 and checked the patched
allocator's real Linux memory permissions. It cross-compiled the instruction and
allocator diagnostics for Android ARM64; **it did not run them on Android**.
The published diagnostic archive is:
https://github.com/RadekParek/PodNative/actions/runs/39495542649/artifacts/12297881468
This is an executable test bundle, not a second APK. See RUNTIME.md for ADB steps.

## Architecture delivered

* Android platform UI: library/inspection, SAF import, actionable diagnostics,
  persistent prototype resolution/frame-cap settings, fullscreen test launch/stop,
  private storage and log export.
* Independent PodNative graphics boundary and original textured-cube scene:
  EGL context/drawable, shader/link checks, texture sampling, VBO/IBO, perspective,
  depth, framebuffer validation, draw/swap errors and surface-lifetime resources.
* Actual embedded ANGLE Vulkan integration, not a native-GLES wrapper or inherited
  renderer. Vulkan preflight and explicit initialization failure; no untested fallback.
* Portable checked guest memory, bounded Mach-O inspection/mapping validation,
  static import evidence and a separate host-tested Dynarmic CPU adapter.
* CI, source pins, retained licenses, documented allocator modification and regressions.

ANGLE revision: `378b820f1c00d511565ab9990ae03de77908b373`.
Official depot_tools pin: `1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`.
CPU source pin: `a41c380246d3d9f9874f0f792d234dc0cc17c180` with the documented
Oaknut allocator patch. The CPU adapter is not yet linked into the APK.

## Runtime evidence — explicitly absent

* Rendering device: **none attached**; intended device is the user's S23 Ultra.
* Selected runtime renderer/backend: **not measured**. The compiled integration
  requests embedded Vulkan-backed ANGLE; only a device run can establish success.
* Textured cube actually rendered: **unverified**. Screenshot: **none**.
* Pause/resume, rotation and surface recreation: implemented source paths, **untested
  on a device**. Synchronous teardown/driver stalls remain a risk.
* FPS, GPU/guest CPU/audio performance: **not measured**.
* Ninjago version/architecture: **not observed**. Stages A–G: **none demonstrated**.

## Next acceptance gate and remaining work

1. Install the extracted APK on the S23 Ultra:
   `adb install -r PodNative-debug.apk`.
   Open **Library → Launch 3D graphics test**. Require a rotating textured 3D cube
   with depth/self-occlusion. Capture a real screenshot and export diagnostic logs.
   Follow DEVICE_TESTING.md for lifecycle and FHD/QHD checks.
2. Run the current ARM64 CPU/allocator diagnostics, then validate the engine inside
   an app-UID harness before linking it into guest application execution. An ADB
   shell pass alone does not establish app-UID/SELinux compatibility.
3. Only after the graphics gate passes: retained guest sessions, iOS ABI and dynamic
   loading, Objective-C/framework semantics and guest EAGL/Core Animation. Prioritize
   game imports/traces, never large sets of successful no-op APIs.

This is an installable **foundation prototype**, not a finished high-level emulator.
No game execution, usable guest controls, audio, saves, complete relocation/binding,
PIE/TLS compatibility, full GLES1/2 guest layer or iOS framework compatibility exists.
License texts and source provenance are retained; full transitive distribution/legal
review and a project source-license decision remain release gates. Device support
and performance must be demonstrated, not inferred from a successful build.
