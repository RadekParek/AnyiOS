# Research and integration decision

**Updated: 2026-10-25.** Repository state is the current AnyiOS branch. The active decision supersedes the original independent-runtime plan: keep the AnyiOS Android shell, library and Converter, but replace its Android game backend with pinned HyperHLE-Reborn/touchHLE source branded in the app as **AnyiOSEMU**. Preserve upstream notices and MPL-2.0 attribution. The accepted guest ABI remains ARM32; ARM64-only execution is not part of this integration.

## Current pins and external components

- **HyperHLE-Reborn**: `https://github.com/KlugKlugTG/HyperHLE-Reborn`, source pin `3f7325ddc29fa5b625b17487ec82eb864209b729` in `third_party/hyperhle-revision.txt`. The repository uses recursive SDL, Dynarmic, libxml2, OpenAL Soft and stb submodules. The integration is maintained as `third_party/hyperhle-patches/AnyiOSEMU.patch`, applied only to that source commit.
- **Boost**: 1.81.0, required by the pinned Rust Dynarmic Android build; downloaded by `scripts/prepare-hyperhle-boost.sh` into ignored build storage.
- **Android runtime**: NDK `27.2.12479018`, Rust `aarch64-linux-android`, and cargo-ndk 3.5.4. The Android host is arm64; the engine's supported imported-app slice is ARM32.
- **ANGLE**: `https://github.com/google/angle`, source pin `9addc2983b90152f628cfdaea194983b46b254bd`. The engine expects EGL, GLES1.1 and GLES2 shared objects. Source build, ELF audit and APK packaging at this new pin still require CI validation.
- **depot_tools**: `1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`, pinned by ANGLE's bootstrap script.

The HyperHLE-Reborn source inspected locally establishes its options, Android SDL host, argument-driven game launch, ANGLE library names, Rust/NDK requirements, nested source dependencies and MPL-2.0 license. Source inspection is not a successful build, APK, device run or compatibility result.

## Decisions retained from the original AnyiOS architecture

- Preserve AnyiOS-owned navigation, design, shared private library, import/export behavior, settings and Converter. The emulator engine is the backend, not a replacement app shell.
- Rebrand visible emulator UI/log/product strings as AnyiOSEMU while retaining source attribution and upstream legal labels/notices.
- Pass a validated selected app and per-game private data path into the engine; preserve originals and keep saves/logs isolated per game.
- Report ARM32, ARM64 and ARM64_32 separately. Run a universal ARM32+ARM64 app via its ARM32 slice; reject ARM64-only launches for now.
- Keep ANGLE as the default bundled graphics driver where available; keep native GLES explicit, record fallback truthfully, and expose Force Composition. A C++ surface test does not validate the SDL game's graphics path.
- Keep bounded, actionable diagnostics without repetitive report spam; do not add fake-success stubs to make unsupported game APIs look implemented.
- Validate a visible menu and input on a real Android device before claiming compatibility or speed.

## Earlier research and superseded assumptions

The original research compared a separate C++ Dynarmic guest with Unicorn and a new ANGLE-backed renderer. That independent guest/framework implementation is no longer the active Android path. The C++ Mach-O inspection, FAT regression coverage and standalone surface diagnostics remain, but are not the game engine. Earlier C++ host-runner measurements, Angry Birds frames and ANGLE builds apply only to those previous prototype revisions and must not be attributed to AnyiOSEMU.

An earlier candidate named HyperHLE-Fork at `5b65054c55cc33c6095f8c4b771430da5e63fbf4` was only a research reference; it is **not** the selected runtime source. The selected repository and exact source commit are the `HyperHLE-Reborn` pin above.

## Build environment and validation limits

The local sandbox has no installed Rust compiler, Cargo, Gradle, Android SDK or JDK. Chromium/ANGLE also needs dependency hosts that are not available from this sandbox. Source scripts and CI must therefore provision the build; current native/Android code must pass CI before it is considered compiled. A passing source checkout/patch check cannot substantiate packaging or device execution.

The technical/legal license review remains a distribution gate: retain the engine's MPL-2.0 notice and all source/dependency notices, preserve ANGLE/dependency license files, and audit the exact nested build closure before production distribution. Do not redistribute proprietary iOS apps/assets in source, CI or reports.
