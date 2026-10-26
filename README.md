# AnyiOS

An Android app with the existing **AnyiOS** launcher, shared private game library and separate **Converter**. **AnyiOSEMU** is the first-party name for the experimental ARM32 iPhone OS runtime. Its source is pinned; upstream licensing and provenance are retained in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## What it does

- Preserves the existing AnyiOS module chooser, library screens, settings and design.
- Imports iOS IPA files or extracted `.app` bundles into one private library shared with Converter. Original imports remain untouched.
- Runs eligible **ARM32** iPhone OS applications. Universal ARM64+ARM32 bundles use their validated ARM32 slice; ARM64-only apps are unsupported.
- Keeps runtime data private per game, with folder and ZIP export actions. Folder export creates an `org.anyios` directory in a user-selected SAF location when Android permits it.
- Defaults the emulator frame cap to the fastest supported display mode at the current resolution. An optional performance HUD reports process CPU, RAM PSS, driver-reported graphics PSS and measured presentation FPS; GPU utilization is not reported where Android does not expose it.
- Provides an off-by-default **Max clocks** request for best-effort Android performance hints and high-capacity CPU affinity. Android controls frequencies; AnyiOSEMU cannot force them.
- Builds the bundled ANGLE EGL/GLES libraries from the pinned Google ANGLE source. ANGLE is the default emulator driver; native GLES is an explicit setting, and any fallback is recorded.
- Keeps the original C++ import inspection and standalone ANGLE surface test as diagnostics; they are not the game execution engine.
- Keeps Converter separate. Its Force convert flow offers a narrowly proven Native runtime or a separate Semi-Emulated boot-attempt APK; neither implies general conversion or gameplay success.

**Compatibility, visible rendering and gameplay require real-device, per-game validation.** A successful compile, clean exit code, instruction count or GL-call log is not proof that a game menu is visible or playable. See [device testing](docs/DEVICE_TESTING.md) and [current progress](docs/PROGRESS.md).

## Runtime and architecture

AnyiOS launches an imported game in the pinned Rust/SDL AnyiOSEMU runtime. The Android activity forwards the selected source, private per-game data directory, display-aware frame cap, performance options, ANGLE/native-GLES preference, Force Composition and verbose-GLES preference. Source revisions, selected settings and bounded session logs are recorded in the game's report.

The runtime source and nested dependencies are pinned in the `third_party/hyperhle` submodule. The reviewed AnyiOS integration patch is `third_party/anyiose-runtime-patches/AnyiOSEMU.patch`, applied only to the revision recorded in `third_party/anyiose-runtime-revision.txt`. These paths and upstream identifiers are build/provenance details, not user-facing product names; required attribution is preserved in the third-party notices.

## Game data and Android storage

The Android application/package ID remains **`org.podnative`** (not `org.anyios.com`), preserving app upgrades. Imported games and writable runtime data stay in internal app-private storage at `/data/user/0/org.podnative/files/games/<game-id>/sandbox/AnyiOSEMU`.

In a game's details, choose **Export game data to folder…** to copy its data beneath `org.anyios` in a folder selected through Android's document picker, or **Export game data as ZIP…** to save an archive. The folder export is a copy; live saves remain private. The picker may block storage roots, Downloads, `Android/data` and `Android/obb` on recent Android versions. Choose a permitted location; AnyiOS does not bypass those restrictions. Rebuildable caches are excluded.

See [architecture](docs/ARCHITECTURE.md), [ANGLE integration](docs/ANGLE_INTEGRATION.md), and [third-party notices](THIRD_PARTY_NOTICES.md).

## Build and test

Requirements include JDK 17, Gradle 8.11.1, Android SDK/build-tools 35, NDK `27.2.12479018`, CMake, Rust with the `aarch64-linux-android` target, `cargo-ndk` 3.5.4, and the pinned ANGLE/depot_tools build dependencies. ANGLE's source build needs a Linux environment with Chromium's dependency download hosts and substantial free disk space.

From a recursive checkout:

```sh
git submodule update --init --recursive
scripts/prepare-anyiose-runtime.sh
scripts/prepare-anyiose-runtime-boost.sh
scripts/prepare-cpu.sh
scripts/prepare-converter.sh
scripts/bootstrap-angle-tools.sh
# Add the pinned depot_tools checkout to PATH, then:
ANGLE_INSTALL_DEPS=1 scripts/build-angle.sh
rustup target add aarch64-linux-android
cargo install cargo-ndk@3.5.4
gradle --no-daemon :app:lintDebug :app:testDebugUnitTest :app:assembleDebug
```

The Android workflow prepares pinned sources, Boost/Rust/ANGLE, runs lint and JVM tests, compiles the Android/Cargo code and audits APK contents. Passing CI confirms the package builds; it does not prove that any game displays a visible menu or is playable on a device.

## Artifacts and project records

[Android build workflow](https://github.com/RadekParek/AnyiOS/actions/workflows/android.yml) publishes APK and PC diagnostic artifacts after successful jobs. Build provenance and SHA-256 checksums accompany each artifact. Release signing/publication remains subject to the repository's existing signing and license gates.

Historical engineering reports describe the earlier independent C++ game-runtime prototype and its tests. The active Android game path is AnyiOSEMU; see the dated status in [`docs/PROGRESS.md`](docs/PROGRESS.md) and the [integration notes](docs/ANYIOS_INTEGRATION.md).
