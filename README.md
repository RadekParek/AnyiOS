# AnyiOS

An Android app with the existing **AnyiOS** launcher, shared private game library and **Converter**. The experimental game engine is **AnyiOSEMU**. The app shell and Converter remain separate from the emulator engine; upstream license and attribution are retained.

## What it does

- Preserves the existing AnyiOS module chooser, library screens, settings and teal design.
- Imports iOS IPA files or extracted `.app` bundles into one private library shared with Converter. Original imports remain untouched.
- Runs eligible **ARM32** iPhone OS applications through AnyiOSEMU. A universal bundle containing ARM32 and ARM64 runs its ARM32 slice. ARM64-only apps are explicitly unsupported; this build does not implement ARM64 guest execution.
- Keeps AnyiOSEMU writable data, logs and exports isolated per game. Importing or launching does not imply compatibility.
- Builds all three bundled ANGLE EGL/GLES libraries from the pinned Google ANGLE source. ANGLE is the default emulator driver; using the device's native GLES is an explicit setting, and driver fallback is recorded.
- Keeps the original C++ import inspection and standalone ANGLE surface test as diagnostics; they are not the game execution engine.
- Leaves the Converter pipeline and its upstream source unchanged.

**Compatibility, visible rendering and gameplay require real-device, per-game validation.** A successful compile, clean exit code, instruction count or GL-call log is not proof that a game menu is visible or playable. See [device testing](docs/DEVICE_TESTING.md) and [current progress](docs/PROGRESS.md).

## Runtime and architecture

AnyiOS launches an imported game in the pinned Rust/SDL AnyiOSEMU runtime. The Android activity forwards the selected source, a private per-game data directory, frame cap, ANGLE/native-GLES preference, Force Composition and verbose-GLES preference. Input and window lifecycle are handled by the runtime. Source revisions, selected settings and bounded session logs are recorded in the game's report.

## Game data and Android storage

The Android application/package ID is **`org.podnative`** (not `org.anyios.com`). Imported games and their writable AnyiOSEMU data are stored in internal, app-private storage. A game's runtime data is under `/data/user/0/org.podnative/files/games/<game-id>/sandbox/AnyiOSEMU`; it is not stored in `/Android/data/org.podnative`. Other apps and ordinary file managers normally cannot browse `/data/user/0/...`, and Android 11+ restricts folder-picker access to the storage root, Downloads root, `Android/data`, and `Android/obb`.

To get a usable copy of saves and diagnostics, open that game's details and choose **Export sandbox data**. The system save picker writes a ZIP to a location you choose; runtime logs and per-game Documents/Library data are included, while rebuildable caches are excluded. For folder imports, choose an accessible normal subfolder (for example, under Documents). For an IPA in Downloads, use **Import IPA file(s)** rather than trying to grant the Downloads folder tree. AnyiOS cannot bypass Android's picker restrictions.

See [architecture](docs/ARCHITECTURE.md), [ANGLE integration](docs/ANGLE_INTEGRATION.md), and [third-party notices](THIRD_PARTY_NOTICES.md).

## Build and test

Requirements include JDK 17, Gradle 8.11.1, Android SDK/build-tools 35, NDK `27.2.12479018`, CMake, Rust with the `aarch64-linux-android` target, `cargo-ndk` 3.5.4, and the pinned ANGLE/depot_tools build dependencies. ANGLE's source build needs a Linux environment with Chromium's dependency download hosts and substantial free disk space.

From a recursive checkout:

```sh
git submodule update --init --recursive
scripts/prepare-hyperhle.sh
scripts/prepare-hyperhle-boost.sh
scripts/prepare-cpu.sh
scripts/prepare-converter.sh
scripts/bootstrap-angle-tools.sh
# Add the pinned depot_tools checkout to PATH, then:
ANGLE_INSTALL_DEPS=1 scripts/build-angle.sh
rustup target add aarch64-linux-android
cargo install cargo-ndk@3.5.4
gradle --no-daemon :app:lintDebug :app:testDebugUnitTest :app:assembleDebug
```

The Android workflow runs pinned source preparation, Boost/Rust provisioning, ANGLE build/audit, Android lint/JVM tests, Gradle/Cargo compilation and APK packaging checks. The PC CI suite tests the separate C++ CPU/loader diagnostics. Passing CI confirms the APK builds; it does not prove that any game displays a visible menu or is playable on a device.

## Artifacts and project records

[Android build workflow](https://github.com/RadekParek/AnyiOS/actions/workflows/android.yml) publishes APK and PC diagnostic artifacts after successful jobs. Build provenance and SHA-256 checksums accompany each artifact. Release signing/publication remains subject to the repository's existing signing and license gates.

Historical engineering reports describe the earlier independent C++ game-runtime prototype and its tests. The active Android game path is now AnyiOSEMU; see the dated status in [`docs/PROGRESS.md`](docs/PROGRESS.md) and the [updated integration notes](docs/ANYIOS_INTEGRATION.md).
