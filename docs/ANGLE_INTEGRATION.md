# Pinned ANGLE integration

## Current source and status

- Upstream: [google/angle](https://github.com/google/angle).
- Source pin: `1ef82576e0e0834b932dfe810064c68a63f615f1`, recorded in `third_party/angle/revision.txt` (2026-10-26).
- License: upstream BSD-style 3-clause license, retained at `third_party/angle/LICENSE`; CI also collects the checked-out dependency notices into the APK.
- This update includes upstream fixes for an EGL termination deadlock, default vertex-attribute state, and Mali dynamic-rendering driver issues, as well as Vulkan dependency changes. It retains the existing ES 1.1/ES 2 and EGL build configuration. A source build or passing packaging checks does not establish Android device or game compatibility; see device acceptance below.

## Why three libraries are built

The source-built arm64 outputs are:

```text
third_party/angle/lib/arm64-v8a/libEGL_podnative.so
third_party/angle/lib/arm64-v8a/libGLESv1_CM_podnative.so
third_party/angle/lib/arm64-v8a/libGLESv2_podnative.so
```

The suffix prevents collisions with the Android system driver. The existing C++ ANGLE surface test imports EGL and GLES2 from these outputs. `scripts/build-angle.sh` also creates byte-for-byte named copies for AnyiOSEMU's SDL loader:

```text
app/src/main/jniLibs/arm64-v8a/libEGL_angle.so
app/src/main/jniLibs/arm64-v8a/libGLESv1_CM_angle.so
app/src/main/jniLibs/arm64-v8a/libGLESv2_angle.so
```

ANGLE's ES 1.1 library is required by the pinned runtime's Android GLES1 path. CI compares each alias with its `_podnative` source, audits the ELF closure, caches both sets of files and verifies their presence in the APK. A missing library or unknown DT_NEEDED dependency is a build failure, not permission to substitute a fake binary or quietly relabel a native driver as ANGLE.

## Reproducible source build

ANGLE uses Chromium's DEPS-pinned build dependencies and a separately pinned depot_tools bootstrap. A Linux build environment needs JDK 17, Python 3, Git, depot_tools, the Android SDK/NDK and CMake/Ninja dependencies, substantial free disk space, and network access to Chromium's dependency hosts.

```sh
scripts/bootstrap-angle-tools.sh
export PATH="$(pwd)/.local/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0
ANGLE_INSTALL_DEPS=1 scripts/build-angle.sh
```

The depot_tools pin is `1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`. `build-angle.sh` refuses an unexpected tool revision, syncs the exact ANGLE pin and its DEPS, generates GN with unused-argument checking, and builds `libEGL`, `libGLESv1_CM` and `libGLESv2`. Outputs, public headers, source revision and conservative third-party license bundle are created in ignored/generated locations. The app NDK is `27.2.12479018`; ANGLE's own DEPS controls its upstream toolchain.

ANGLE's generated libraries and generated notice bundles are not committed to Git. Check the current CI run for the status of this exact pin; source-level updates have not been validated on a device.

## Driver selection and truthful reporting

AnyiOSEMU looks for the three bundled `*_angle.so` libraries before creating its SDL GL context. Bundled ANGLE is the default. If a library cannot load, or SDL reports that a non-ANGLE driver became active, AnyiOSEMU records that outcome and may retry with Android's system GLES driver. The **Native OpenGL ES** setting explicitly requests the system GLES path for the next emulator launch. Neither the preference nor a successful library load proves that the game rendered a visible frame.

The separate AnyiOS ANGLE surface test uses the `_podnative` outputs and its own explicit EGL/Vulkan diagnostics. Its Vulkan preflight and textured-cube tests do not certify the SDL guest window, EAGL semantics, a game's rendered menu or touch input. **Force Composition** is an AnyiOSEMU runtime option, not a workaround that can be declared successful without visual validation.

The app exposes the source revision and the last runtime driver/fallback in its reports. Do not interpret requested FPS, successful swaps, GL-call counts or a clean exit status as measured speed or rendering proof.

## Device acceptance

Follow [`DEVICE_TESTING.md`](DEVICE_TESTING.md). For the updated runtime, verify on a real device that (1) the APK contains all three ANGLE aliases, (2) the log identifies the driver that actually initialized, (3) a known eligible ARM32 or universal ARM32+ARM64 game displays its real menu, (4) touch input reaches the game, and (5) orientation/lifecycle and native-GLES/Force-Composition settings behave as reported. Preserve the log and a screenshot only after seeing actual game output. No such device validation is included in this source change.

## Licensing gate

ANGLE's top-level license is not the license for every compiled dependency. The build conservatively collects upstream license/README notices, but that is not a legal audit. Review the actual pinned source/dependency closure and terms before public production distribution. See [packaged runtime notices](../app/src/main/assets/anyiose-runtime-notices.txt).
