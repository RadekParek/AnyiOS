# Actual upstream ANGLE integration

## Status and provenance

Pinned source: `google/angle` revision `378b820f1c00d511565ab9990ae03de77908b373`.
Top-level license: BSD-style 3-clause, retained in `third_party/angle/LICENSE`.
No upstream libraries or vendored renderer were present initially. This change
adds **source-build and integration code**, not verified ANGLE binaries. No Android
compilation, EGL initialization or rendered frame has occurred in this sandbox.

## Selected strategy: app-embedded, arm64 shared libraries

Build upstream `libEGL` and `libGLESv2` with `angle_libs_suffix="_podnative"`,
`is_component_build=false`, Vulkan enabled and GL/null/SwiftShader disabled.
GN's suffix mechanism is present in the pinned `gni/angle.gni` and `BUILD.gn`.
Package `libEGL_podnative.so` and `libGLESv2_podnative.so` in the app ABI directory.
Link PodNative to these exact imports, never to Android's `libEGL`/`libGLESv2`.
NDK EGL/GLES declarations come from the same ANGLE source checkout. A missing
binary causes CMake to fail with instructions rather than build a fake substitute.

Unique library names avoid confusing Android's system drivers with our build.
The build audits ELF64/AArch64 and DT_NEEDED closure. New unbundled dependencies
cause an explicit failure; investigate and license them rather than weakening the
check. The audit currently expects static ordinary dependencies; this expectation
still needs confirmation by a real upstream build. App packaging uses AGP/NDK's
normal shared-library handling. Validate 16 KiB page-size alignment on a real APK
and supported Android versions before release distribution.

This route does not require root, developer driver settings or a system-wide switch.
There is **no assumption** that the device's system ANGLE is installed or selectable.
Upstream documents a separate `AngleLibraries.apk` driver method, primarily for
debuggable/root environments; that is not the selected distribution path. No
unverified prebuilt AAR or binary mirror is used.

## Source build

Run on a Linux workstation/CI with unrestricted access to Chromium dependency
hosts, sufficient free disk space (plan tens of GB) and RAM:

1. Install Python3, git, JDK17, Gradle 8.11.1, Android SDK platform/build-tools 35,
   app NDK `27.2.12479018` and CMake 3.22.1. Set `ANDROID_HOME`.
2. Follow upstream `doc/DevSetup.md` to install depot_tools and add it to PATH.
3. Run `ANGLE_INSTALL_DEPS=1 scripts/build-angle.sh` on a disposable Ubuntu runner;
   omit the environment option if system build dependencies are already installed.
4. The script creates a gclient workspace in `.local/angle-build`, adds
   `target_os=['android']`, syncs **the pinned ANGLE revision and its DEPS**, generates
   GN args and runs `autoninja ... libEGL libGLESv2`.
5. It copies actual outputs and headers to ignored build-input directories, creates
   an ANGLE source revision asset and conservative license-text bundle, and audits
   ELF dependencies. `gn --fail-on-unused-args` prevents silently ignored config.
6. Run `gradle --no-daemon lintDebug testDebugUnitTest assembleDebug`.
   APK (if built): `app/build/outputs/apk/debug/app-debug.apk`.

ANGLE has its own DEPS-pinned Android toolchain (the inspected DEPS currently
specifies NDK `30.0.16248370`), independent of the app's NDK. Do not replace it
speculatively with the app NDK. CI records depot_tools' revision because bootstrap
is currently fetched at HEAD; full tool-bootstrap hermeticity is still unfinished.
No claim is made that this unexecuted CI recipe already builds successfully.

The sandbox cannot perform gclient/CIPD downloads or Android builds due to absent
tools and host restrictions. CI is the next compilation gate. Any GN, SDK, ABI,
linker or dependency failure must be diagnosed against this pin.

## API and backend selection

PodNative uses public `eglGetPlatformDisplayEXT` after checking
`EGL_ANGLE_platform_angle` and `EGL_ANGLE_platform_angle_vulkan`, with
`EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE`. No private ANGLE structs are imported.
These ANGLE extensions are version-pinned and checked at runtime; the Vulkan
extension document is marked Draft, not a promise of eternal ABI stability.

The pinned Vulkan implementation requires **Vulkan 1.1 instance and device**
(`vk_renderer.cpp` explicitly rejects lower versions) and prefers core 1.1 plus
extensions. PodNative preflight checks:

* Instance enumeration, `VK_KHR_surface`, `VK_KHR_android_surface`.
* Creation of a Vulkan 1.1 instance and enumeration of physical devices.
* A >=1.1 candidate with a graphics queue and `VK_KHR_swapchain`.
* Feature diagnostics for ETC2 and anisotropy (reported, not mandatory for this cube).

These are a baseline, **not ANGLE's entire dynamic feature matrix**. ANGLE itself
performs device/feature negotiation during initialization/context/drawable creation.
Success is only accepted after those steps. The preflight candidate is not asserted
to be the exact ANGLE-selected physical device; GL_RENDERER is authoritative. Future
support requires backend feature tests and presentation validation on each target.

The prototype requests GLES2, RGBA8, depth24, window surface and vsync. It reports
EGL vendor/version, GL version/renderer and draw/initialization failures. No validated
fallback exists. Vulkan failure stops rendering and tells the user to export logs;
no native-GLES code is mislabeled ANGLE. Future GL-backed ANGLE must be separately
built, tested and clearly reported before it can be a fallback.

## GLES support is not iOS emulation

Upstream DevSetup documents GLES3.1/EGL1.5; current extension documentation also
lists GLES3.2 capabilities. These statements do not certify every backend/device.
Only an actual created context's version/extensions establish usable functionality.
The prototype exercises ES2 shaders, texture upload/sampling, VBO/IBO draw calls,
depth state and a window framebuffer. No game GLES1/2 coverage claim is made.

ANGLE does not implement EAGL, CAEAGLLayer, UIKit, guest object identities, guest
error/state semantics, guest share groups or iOS timing. See ARCHITECTURE.md.
Compressed textures (especially PVRTC), context sharing, offscreen FBOs, guest
shaders, depth/stencil variants and Core Animation composition remain untested.
Unsupported guest APIs will need semantic implementations or structured rejection,
not no-op stubs.

## Licensing and distribution gate

Retain upstream copyright, disclaimer and license; no upstream changes are made.
ANGLE includes separately licensed dependencies such as shader/Vulkan support
libraries. The build conservatively collects checkout LICENSE/COPYING/NOTICE and
README.chromium texts into the APK with a source-path index. This preserves notices
but **is not a legal audit**. Before public distribution, inspect the exact built
closure, all applicable dependency terms and modifications, and update
THIRD_PARTY_NOTICES.md with the resolved versions. Release publication is opt-in;
unsigned builds are marked non-installable. Do not enable public publication before
license and device verification gates pass.

## Device acceptance

See DEVICE_TESTING.md. Capture actual output only after a successful run; no scene
screenshot is included here. Surface/pause/resume/rotation code is source-only until
those tests run. A successful Gradle build or FIRST_PRESENT log alone is not visual
proof of a textured 3D scene.
