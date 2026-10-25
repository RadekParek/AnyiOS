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
   pinned HyperHLE-Reborn/touchHLE Rust runtime
           /                     \
    ARM32 iOS HLE          SDL touch/window/audio
           |                     |
    guest EAGL/GLES ---- ANGLE EGL/GLES (default)
                           or native GLES (setting)
                 |
          Android display surface
```

The Android app shell, module navigation, library, Converter and visual design remain AnyiOS-owned. `AnyiOSEmuActivity` is the Android host for the pinned Rust/SDL engine; the activity passes the selected game and per-game private data directory, handles result/report transfer, and forwards saved runtime settings. SDL forwards Android touch and lifecycle events to the guest runtime.

The emulator source is a pinned submodule at `third_party/hyperhle`. `scripts/prepare-hyperhle.sh` verifies the source revision, recursively checks out its nested dependencies, and applies the reviewed source-form patch in `third_party/hyperhle-patches/AnyiOSEMU.patch`. Upstream notices and MPL-2.0 attribution are preserved. The Cargo/Android build uses the same Android NDK selected by Gradle plus the exact Boost version in `third_party/hyperhle-boost-version.txt`.

## Responsibilities retained in the AnyiOS shell

- `ModuleActivity` and `MainActivity`: module navigation, existing cards/settings, import/export flows, shared library and Converter handoff.
- `GameLibrary`: private source storage, per-game sandbox and zero-copy IPA alias for AnyiOSEMU's `.ipa` loader, eligibility checks, report persistence and sandbox export.
- `GameInspector`: bounded metadata/executable inspection and explicit ARM architecture reporting. A universal Mach-O that has ARM32 and ARM64 is launched through ARM32; ARM64-only is retained as unsupported.
- `NativeBridge` and the C++ CMake library: import inspection, FAT slice selection, CPU/loader diagnostics and the standalone ANGLE surface test. They do **not** execute the imported game in the active Android path.
- Converter: remains a separate module and continues using the shared original import. No file under `converter/` is changed by the emulator integration.

## Graphics and settings

`scripts/build-angle.sh` builds `libEGL`, `libGLESv1_CM` and `libGLESv2` from the pinned Google ANGLE source. Uniquely named `_podnative` copies serve the existing C++ surface test; identical `*_angle.so` copies in `app/src/main/jniLibs/arm64-v8a` are loaded by AnyiOSEMU's Android SDL driver. CI audits the source-built ELF dependency closure, checks the copies and verifies the packaged libraries.

ANGLE is the default guest GL driver. **Native OpenGL ES** requests the device system driver. AnyiOSEMU records whether ANGLE initialized or a native fallback was selected; a build or log line is not proof of a visible frame. **Force Composition**, frame cap and verbose GLES logging are forwarded on the next runtime launch. The AnyiOS render-resolution setting applies only to the separate surface test.

## CPU/ABI limit and game eligibility

The active Android guest CPU is the A32/ARM32 backend in the pinned AnyiOSEMU engine, running on the host's ARM64 Android ABI. This does not mean that the emulator executes an ARM64 iOS guest. The library retains architecture details and exposes a launch action only when the import contains a supported, unencrypted ARM32 Mach-O. Universal binaries choose ARM32 even if an ARM64 slice is also present. ARM64-only remains unsupported until an actual ARM64 guest implementation is integrated and tested.

The C++ ARM/Thumb/Dynarmic diagnostic targets remain useful for their isolated tests, but are not evidence about the Android AnyiOSEMU game path. Host tests, Cargo/Gradle compilation, retired guest instructions, successful GL calls and exit status do not prove that a game's menu was rendered or that touch input works for that title.

## Private data and diagnostics

Each library entry retains its original `source.bin`. For an IPA launch AnyiOS creates a hard-link alias named `AnyiOSEMU_source.ipa`, avoiding another copy while preserving the Converter's canonical source. AnyiOSEMU writes under that game's `sandbox/AnyiOSEMU` directory; the per-game log and settings summary are saved with the entry and included in sandbox exports. The Android DocumentsProvider resolves only paths under private app storage, refuses symlink components and blocks path/name traversal.

Launch reports identify the source build, pinned engine revision, ARM32 selection, FPS cap, graphics preference, Force Composition and verbose-log setting. The bounded log tail includes the first runtime blocker when available. A forced stop or native process crash may leave only the pending report; saved files and native crash logs are not treated as proof of rendering.

## Verification gates

1. Apply and verify the patch only against the recorded HyperHLE-Reborn source revision; keep nested submodule revisions fixed.
2. Run host parser/FAT regressions and AnyiOS JVM tests. The existing Converter implementation and previously aborted `scripts/test-converter.sh` are out of scope for manual reruns in this integration task.
3. Build the pinned ANGLE source, audit ELF dependencies, compare the AnyiOSEMU driver copies with the `_podnative` outputs and verify APK contents.
4. Complete Android lint, JVM tests and debug/release compilation in CI. Compilation alone is not runtime evidence.
5. Validate a visible game menu, touch response, audio, orientation, graphics backend/fallback and per-game data on a real device before making compatibility or performance claims.

See [`DEVICE_TESTING.md`](DEVICE_TESTING.md), [`ANGLE_INTEGRATION.md`](ANGLE_INTEGRATION.md), and [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
