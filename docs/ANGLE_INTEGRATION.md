# Actual upstream ANGLE integration

## Current build evidence

Run [39495542750](https://github.com/RadekParek/PodNative/actions/runs/39495542750)
successfully compiled and audited real ANGLE and published the signed debug APK.
See FIRST_APK_REPORT.md for the exact source/artifact. Device initialization and
rendering remain unverified. Earlier failure descriptions below document the path
to that successful build, not current compilation blockers.

## Status and provenance

Pinned source: `google/angle` revision `378b820f1c00d511565ab9990ae03de77908b373`.
Top-level license: BSD-style 3-clause, retained in `third_party/angle/LICENSE`.
No upstream libraries or vendored renderer were present initially. This change
adds **source-build and integration code**, not verified ANGLE binaries. The second GitHub Actions run successfully compiled actual upstream ANGLE; its
post-build audit then stopped on the previously unrecognized Android platform
`libnativewindow.so` dependency. The audit now recognizes that public API-26+
library (PodNative minimum: API 29), with regression tests that continue to reject
unknown vendor libraries and any native-GLES substitution. EGL initialization and
rendered frames remain unverified. See PROGRESS.md for current run evidence.

## Selected strategy: app-embedded, arm64 shared libraries

Build upstream `libEGL` and `libGLESv2` with `angle_libs_suffix="_podnative"`,
`is_component_build=false`, Vulkan enabled and GL/null/SwiftShader disabled.
GN's suffix mechanism is present in the pinned `gni/angle.gni` and `BUILD.gn`.
Build `libEGL_podnative.so` and `libGLESv2_podnative.so` into ignored
`third_party/angle/lib/arm64-v8a`. AGP packages the linked CMake IMPORTED targets
into the app ABI directory. They are not also copied to jniLibs, which would
risk duplicate packaging.
Link PodNative to these exact imports, never to Android's `libEGL`/`libGLESv2`.
NDK EGL/GLES declarations come from the same ANGLE source checkout. A missing
binary causes CMake to fail with instructions rather than build a fake substitute.

Unique library names avoid confusing Android's system drivers with our build.
The build audits ELF64/AArch64 and DT_NEEDED closure. New unbundled dependencies
cause an explicit failure; investigate and license them rather than weakening the
check. The audit currently expects static ordinary dependencies; this expectation
was checked successfully in the completed upstream build. App packaging uses AGP/NDK's
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
2. Run `scripts/bootstrap-angle-tools.sh`; add `.local/depot_tools` to PATH.
   This fetches official depot_tools at the recorded pin. `build-angle.sh` disables
   depot_tools self-update so the checked-out source is actually the source used.
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
speculatively with the app NDK. depot_tools is now pinned at `1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`,
identified in Chromium `0f14e57ef0419795c73bf553e5c16c43259c07da` DEPS.
Run 39493714456 diagnosed an HTTP 503 specifically during depot_tools' automatic
self-update after a successful clone. The pinned bootstrap plus
`DEPOT_TOOLS_UPDATE=0` removes that unversioned update fetch, not dependency checks.
Full offline/CIPD toolchain hermeticity remains unfinished.
ANGLE and full Android application compilation, lint, packaging and debug signing
verification passed in CI run 39495542750. Build outputs are cached by upstream revision and
build/audit script hashes, then audited again on restore.

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

## Driver order and the OpenGL ES 1.1 gap

`scripts/build-angle.sh` builds `libEGL` and `libGLESv2` only; upstream dropped
`libGLESv1_CM`, so an `EGL_CONTEXT_CLIENT_VERSION 1` context cannot be created
through the bundled driver. Titles built for iOS 3.x-5.x are ES 1.1 titles: they
import `glVertexPointer`, `glColorPointer`, `glTexCoordPointer`, `glNormalPointer`,
`glEnableClientState`, `glMatrixMode`, `glLoadMatrixf`, `glColor4f`,
`glClientActiveTexture`, `glTexEnvi`, `glLightfv` and `glMaterialfv`, and an ES 2.0
driver exports none of those names. Against bundled ANGLE such a title used to stop
at its first fixed-function call, with the import's trap naming the function.

The GL bridge resolves drivers as *families*: an EGL library together with the GLES
library that belongs to it. An EGL from one driver and a GLES from another produce a
display the two disagree about, which is a black screen with no error anywhere. The
order is

1. bundled ANGLE, `libEGL_podnative.so` + `libGLESv2_podnative.so` — the default;
2. the device driver, `libEGL.so` + `libGLESv2.so` + `libGLESv1_CM.so`;
3. the device driver by soname, `libEGL.so` + `libGLESv2.so.2`.

Settings ▸ **Native OpenGL ES** reverses that order, for a title that needs
something the bundled driver does not provide. It is off by default, it is applied
before the driver is resolved, and it is never silent: `GLES_STATE` in the launch
report names the libraries that answered and which order was preferred. EGL and
ES 1.1 are optional inside a family, because a driver that ships both APIs in one
library answers the EGL names from the GLES handle.

A family whose GLES library has `glDrawArrays` but no `glVertexPointer` has no
ES 1.1 frontend, and the bridge then serves those entry points itself instead of
leaving them trapped. It holds the state an ES 2.0 driver has no place for — matrix
mode, projection/modelview/texture matrices, current colour, client-array
descriptions, texture-environment mode, client texture unit — and draws through a
generated ES 2.0 program, asking EGL for an ES 2.0 context even though the guest
asked for ES 1.1. `GL_FIXED` positions are widened to floats, a byte colour array is
passed normalised so 255 still means 1.0, a client array in guest memory is copied
into a stream buffer, and an array the guest already put in a buffer object is passed
through as an offset with nothing copied. Programs are cached per variant — colour
array on or off, texture coordinates from an array or from the current value,
`GL_REPLACE` or not — so a frame does not relink. A draw copies as far as it will be
read: to `first + count`, or to the largest index for an indexed draw.

What the emulation does *not* do is reported too, because a title that looks wrong on
a device nobody can attach a debugger to is otherwise undiagnosable: lighting is
recorded and counted but not shaded, the `GL_DECAL`, `GL_BLEND` and `GL_ADD` texture
environments are approximated as modulate and counted, and use of a client texture
unit beyond the emulated one is counted. A generated shader that fails to compile is
refused rather than drawn with, and the driver's own info log goes into the report.

A driver that does have ES 1.1 keeps forwarding every call untouched and none of this
state is used; `tests/gles_driver_test.cpp` covers that side against
`tests/gles_stub_driver.cpp`. The emulation side is covered by
`tests/gles_emulation_test.cpp` against `tests/gles_es2_stub_driver.cpp`, an ES 2.0
-only recording double: the context version asked for, the generated shader pair and
attribute bindings, the model-view-projection product, the vertex and index copies,
fixed-point widening, the program cache and the named refusal. That double records
rather than renders, so this is evidence of translation, not of a drawn frame.

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
