# Research and component decisions

Research date: 2026-10-20. Repository baseline: `dc703553b265b07365ad7d393a042ad5a1a82f6e`.
Inspected repository: only a README, clean working tree. Sandbox: Linux x86-64,
Python and g++ available; no JDK, Gradle, Android SDK/NDK, Rust compiler or ADB.
Outbound downloads are restricted to GitHub, npm and PyPI hosts. Chromium's gclient,
CIPD and Google toolchain downloads cannot be completed here. The user has a legal
copy of the game and an S23 Ultra with ADB, but neither is connected to this sandbox.

## References actually inspected

* [ANGLE source](https://github.com/google/angle/tree/378b820f1c00d511565ab9990ae03de77908b373),
  pinned at `378b820f1c00d511565ab9990ae03de77908b373`:
  `LICENSE`, `DEPS`, `BUILD.gn`, `gni/angle.gni`, `doc/DevSetup.md`,
  `doc/DevSetupAndroid.md`, `doc/ES1Status.md`, `doc/ExtensionSupport.md`,
  `include/EGL/eglext_angle.h`, `extensions/EGL_ANGLE_platform_angle_vulkan.txt`,
  `src/libANGLE/renderer/vulkan/vk_renderer.cpp`.
* [HyperHLE-Fork](https://github.com/KlugKlugTG/HyperHLE-Fork/tree/5b65054c55cc33c6095f8c4b771430da5e63fbf4),
  revision `5b65054c55cc33c6095f8c4b771430da5e63fbf4`: README, Cargo manifest and MPL-2.0
  license. Manifest identifies Dynarmic, SDL, audio/media, Mach-O and PVRTC components.
  These are research leads, not proof of suitability. The manifest's allocator and
  AAC failure comments reinforce the need for pinned dependencies, bounds checks and
  structured failure handling; they have not been independently reproduced here.
  No renderer, framework implementation or code from this project is copied.
* [Dynarmic](https://github.com/lioncash/dynarmic/tree/a41c380246d3d9f9874f0f792d234dc0cc17c180),
  candidate revision `a41c380246d3d9f9874f0f792d234dc0cc17c180`: repository metadata
  reports 0BSD; README describes A32 guests, AArch64 hosts, Android, callbacks,
  instrumentable execution and a C++20 implementation. Dependency licenses and
  actual Android build need review **before integration**.
* [Unicorn](https://github.com/unicorn-engine/unicorn): repository metadata reports
  GPL-2.0. Potential deterministic execution/reference engine; exact licensing,
  performance and AArch64 Android support require a deeper version-specific review.
  Not selected or included. No custom dynamic recompiler is planned.

## Decisions

**Graphics:** independent PodNative backend and Android Surface lifecycle, with real
ANGLE providing EGL/GLES translation. Use public EGL and GLES entry points only;
no internal ANGLE renderer objects. Embed uniquely suffixed ANGLE shared objects.
Vulkan only in this first experiment, fail explicitly rather than silently choose
an untested driver. See ANGLE_INTEGRATION.md for source/build/distribution details.

**GLES coverage:** prototype exercises GLES2. The target game's GLES version,
extensions and PVRTC usage are unknown without executable/API traces. Upstream's
ES1Status says GLES1 is frontend emulation over GLES3-capable backends and lists
missing smooth lines, two-sided lighting and matrix palettes. This is not complete
iOS GLES1 compatibility. PVRTC must be probed and, if required, independently
licensed decoding tested against reference vectors. No unsupported operation is
silently dropped; no guest GLES entry points exist yet.

**CPU:** favor Dynarmic for a later measured integration, not a new JIT. Validate
A32/Thumb/VFP behavior, AAPCS register/stack bridging, W^X-compliant code allocation,
Android AArch64 execution, exception callbacks and dependency licenses first.
CPU execution remains independent of graphics.

**Loader:** small original C++ inspector, not an executable loader. Recognizes thin
little-endian ARM32 MH_EXECUTE; validates command bounds, segment file ranges and
section counts, detects encryption and rejects unsupported formats. Does not map
memory, resolve relocations, process dyld bindings or claim Stage A completion.
The APK's importer stores private copies, bounds file sizes, does not extract ZIP
paths, and rejects ambiguity and unsupported binaries. It does not bypass DRM.

**UI:** Android platform widgets, restrained dark/slate palette, rounded panels,
teal accent and large touch targets rather than a counterfeit Apple interface.
No web UI or runtime in the Activity. Settings actually feed prototype resolution
and frame cap. Unimplemented guest features are labeled, not presented as working
switches. No third-party UI dependencies.

## Not established by research

No verified vendor-neutral prebuilt ANGLE Maven/AAR distribution was selected.
ANGLE's separately installable driver APK is not an ordinary-user embedded runtime.
No game requirements, current maintenance guarantees, device compatibility,
performance results or successful Android build have been established by reading
source alone. Transitive ANGLE license review is a distribution gate, not something
the top-level BSD license resolves automatically.
