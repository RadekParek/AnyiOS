# AnyiOS architecture

## Current Android game-launch path

```text
AnyiOS launcher / module chooser
                 |
        shared private game library ---------------- Converter
                 |
    bounded import + ARM32/FAT inspection
                 |
        AnyiOSEmuActivity (Android / SDL)
                 |
      pinned Rust/SDL guest runtime
           /                     \
    ARM32 iOS HLE          SDL touch/window/audio
           |                     |
    guest EAGL/GLES ---- ANGLE EGL/GLES (default)
                           or native GLES (setting)
                 |
          Android display surface
```

The Android app shell, module navigation, library, Converter and visual design remain AnyiOS-owned. `AnyiOSEmuActivity` is the Android host for the pinned Rust/SDL engine; the activity passes the selected game and per-game private data directory, handles result/report transfer, and forwards saved runtime settings. SDL forwards Android touch and lifecycle events to the guest runtime.

The source is pinned as a submodule at `third_party/hyperhle`. `scripts/prepare-anyiose-runtime.sh` verifies its revision and nested dependencies, then applies the reviewed source-form patch in `third_party/anyiose-runtime-patches/AnyiOSEMU.patch`. Upstream notices, licenses and source provenance remain intact; the exact Boost version is recorded in `third_party/anyiose-runtime-boost-version.txt`. These source paths and upstream identifiers are build details, not product branding.

## Responsibilities retained in the AnyiOS shell

- `ModuleActivity` and `MainActivity`: module navigation, existing cards/settings, import/export flows, shared library and Converter handoff.
- `GameLibrary`: private source storage, per-game data, checked/cancellable SAF folder and ZIP exports, launch eligibility, report persistence and icon fallback shared with Converter.
- `GameInspector`: bounded metadata/executable inspection and explicit ARM architecture reporting. A universal Mach-O that has ARM32 and ARM64 is displayed ARM64-first but launched through ARM32; ARM64-only remains unsupported.
- `NativeBridge` and the C++ CMake library: import inspection, FAT slice selection, CPU/loader diagnostics and the standalone ANGLE surface test. They do **not** execute the imported game in the active Android path.
- Converter: remains a separate module. Force convert offers the bounded Native route only when its strict proof passes, or a separate Semi-Emulated boot-attempt APK; neither path promises a general conversion or playable game.

## Graphics and performance controls

`scripts/build-angle.sh` builds `libEGL`, `libGLESv1_CM` and `libGLESv2` from pinned Google ANGLE source. Uniquely named `_podnative` copies serve the existing C++ surface test; identical `*_angle.so` copies in `app/src/main/jniLibs/arm64-v8a` are loaded by the Android runtime. CI audits the source-built ELF dependency closure, checks the copies and verifies packaged libraries.

ANGLE is the default guest GL driver. **Native OpenGL ES** requests the device system driver. AnyiOSEMU records whether ANGLE initialized or a native fallback was selected; a build or log line is not proof of a visible frame. **Force Composition**, verbose GLES logging, display-aware FPS cap, performance HUD and scheduler hints apply on the next runtime launch. The FPS cap request and measured presentation FPS are reported separately. The off-by-default HUD reports process CPU, RAM PSS, driver-reported graphics PSS and presentation FPS; GPU utilization is reported as unavailable when Android does not expose it. **Max clocks** requests best-effort Android hints and high-capacity CPU affinity; it does not force CPU/GPU frequencies. The AnyiOS render-resolution setting applies only to the separate surface test.

## CPU/ABI limit and game eligibility

The active Android guest CPU is the A32/ARM32 backend in the pinned engine, running on the host's ARM64 Android ABI. This does not mean that the emulator executes an ARM64 iOS guest. The library retains architecture details and exposes a launch action only when the import contains a supported, unencrypted ARM32 Mach-O. Universal binaries choose ARM32 even if an ARM64 slice is also present. ARM64-only remains unsupported until an actual ARM64 guest implementation is integrated and tested.

The C++ ARM/Thumb/Dynarmic diagnostic targets remain useful for their isolated tests, but are not evidence about the Android game path. Host tests, Cargo/Gradle compilation, retired guest instructions, successful GL calls and exit status do not prove that a game's menu was rendered or that touch input works for that title.

## Private data and diagnostics

Each library entry retains its original `source.bin`. For an IPA launch AnyiOS prefers a hard-link alias named `AnyiOSEMU_source.ipa`, preserving the Converter's canonical source without another copy. If Android's filesystem forbids hard links, a storage-checked, cancellable copy fallback creates the runtime alias; its rebuildable marker is kept beside the alias. Extracted `.app` imports keep their copied bundle. AnyiOSEMU writes under that game's `sandbox/AnyiOSEMU` directory; per-game logs and settings summaries are saved with the entry.

A game's details can export a browsable copy beneath `org.anyios` in a user-selected SAF tree, or create a ZIP. The folder export includes Documents, bundle overrides, runtime data and an imported extracted bundle when applicable; rebuildable caches are excluded. Android may restrict the selected storage location; AnyiOS does not bypass those restrictions. The Android DocumentsProvider resolves only paths under private app storage, refuses symlink components and blocks path/name traversal.

Launch reports identify the source build, pinned engine revision, ARM32 selection, requested/effective FPS cap, measured presentation FPS when available, graphics preference and scheduler settings. The bounded log tail includes the first runtime blocker when available. A forced stop or native process crash may interrupt logging; saved files and native crash logs are not treated as proof of rendering.

## Verification gates

1. Apply and verify the patch only against the recorded runtime source revision; keep nested submodule revisions fixed.
2. Run host parser/FAT regressions and AnyiOS JVM tests. Do **not** run `scripts/test-converter.sh` in this task; Converter changes are limited to the requested Force convert choice and shared-icon handling.
3. Build pinned ANGLE, audit ELF dependencies, compare the runtime driver copies with the `_podnative` outputs and verify APK contents.
4. Complete Android lint, JVM tests and debug/release compilation in CI. Compilation alone is not runtime evidence.
5. Validate a visible game menu, touch response, audio, orientation/lifecycle, per-game saves, folder/ZIP exports, display cap/HUD and graphics fallback on a real device before making compatibility or performance claims.

See [`DEVICE_TESTING.md`](DEVICE_TESTING.md), [`ANGLE_INTEGRATION.md`](ANGLE_INTEGRATION.md), and [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
