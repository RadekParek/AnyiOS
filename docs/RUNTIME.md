# Android game runtime and native diagnostics

## Active Android game path

AnyiOS launches eligible iOS apps in the pinned **AnyiOSEMU** ARM32 guest runtime. `AnyiOSEmuActivity` hosts the Rust/SDL engine and passes the selected app/IPA, a validated private per-game data directory and saved runtime controls. The separate C++ loader and CPU diagnostic below are not used to execute Android game launches.

The active Android ABI is `arm64-v8a`, but the iOS guest slice is ARM32/A32. A universal ARM32+ARM64 Mach-O is launched using ARM32. ARM64-only execution is not implemented. Do not interpret the host ABI or the existence of an ARM64 slice as guest support.

The pinned guest runtime supplies the instruction engine and iOS compatibility layer. Importing a game, starting a process, successfully returning from the JIT or observing guest instructions does not demonstrate framework compatibility or visible rendering. A game's real menu, touch interaction, audio and lifecycle need separate device validation; see [`DEVICE_TESTING.md`](DEVICE_TESTING.md).

## Import inspection and slice reporting

The retained AnyiOS C++ inspection bridge performs bounded archive/Mach-O metadata parsing. It distinguishes ARM32 (`CPU_TYPE_ARM`), ARM64 (`0x0100000c`) and ARM64_32 (`0x0200000c`) instead of conflating the 64-bit CPU families. `GameInspector` reports available slices ARM64-first and identifies the ARM32 slice for universal bundles. The import inspector refuses to claim ARM64-only eligibility.

The root `tests/fat_test.cpp` exercises thin and FAT slice metadata and selection. `AnyiOSEmuSourceTest.java` tests source selection in nested `.app` folders and rejects executable metadata that escapes the imported directory. These tests verify inspection/source handling, not any game's execution or rendering.

## Separate C++ CPU/loader diagnostics

AnyiOS still has a standalone C++ guest-memory/Mach-O/CPU diagnostic branch using its pinned upstream Dynarmic source. It is retained for isolated regressions and historical host/Android diagnostic artifacts; **it is not linked into or called as the AnyiOSEMU game engine**. Its results must not be used to claim game compatibility or ARM64 iOS execution.

Historical host tests covered fixed-address ARM32 Mach-O parsing, bounded memory, ARM/Thumb fixtures, callbacks/SVC fault reporting, permissions, cache invalidation and mutated inputs under ASan/UBSan. They were written without proprietary fixtures. Passing these host tests establishes only the isolated native diagnostic behavior.

The diagnostic implementation's memory contract includes checked 32-bit guest ranges, 4 KiB guest-page metadata, non-overlapping regions, prevalidated cross-region writes and no executable host mapping for guest bytes. Unsupported instruction/ABI cases return structured faults rather than simulated successful no-ops. Those constraints apply to that retained C++ diagnostic path; they do not replace the Rust runtime's own memory/safety behavior.

## Reproducible source and build inputs

- AnyiOSEMU runtime source pin: `third_party/anyiose-runtime-revision.txt`.
- Reviewed runtime integration/rebranding changes: `third_party/anyiose-runtime-patches/AnyiOSEMU.patch`.
- Rust Android target: `aarch64-linux-android`; Cargo/NDK wrapper: `cargo-ndk` 3.5.4.
- Dynarmic's exact Boost headers: `third_party/anyiose-runtime-boost-version.txt`, prepared by `scripts/prepare-anyiose-runtime-boost.sh`.
- App NDK: `27.2.12479018`.
- ANGLE pin: `third_party/angle/revision.txt`; ANGLE's toolchain and DEPS are pinned separately.

The source patch must apply cleanly to the exact engine pin and recursively pinned submodules. Do not edit vendored engine code without regenerating the reviewed patch. Generated dependencies, libraries and notices stay outside Git unless a source license notice requires inclusion.

## Verification plan and current evidence

Host parser/FAT regressions and repository format/script checks can run independently. The Android integration requires Android lint, JVM tests, Cargo/NDK build, APK package audit and device validation. This sandbox has no Java, Gradle or Android SDK, so the Android build and unit tests have not run locally; PR CI is the next build verification gate. No game compatibility, visible rendering or measured-speed claim is made here.

Do not run `scripts/test-converter.sh` in this task. Converter changes here are limited to the requested Force convert choice and shared-icon handling; the conversion backend is not being treated as validated by those UI changes. Tests of the separate C++ diagnostics, a clean Rust/Gradle build, APK contents, GL-call counts, successful swaps, or process exit status do not establish that an iOS game's first menu is visible.
