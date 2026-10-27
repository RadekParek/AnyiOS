# AnyiOS

An Android app with the existing **AnyiOS** launcher, shared private game library and separate **Converter**. **AnyiOSEMU** is the first-party name for the experimental ARM32 iPhone OS runtime. Its source is pinned; upstream licensing and provenance are retained in [packaged notices](app/src/main/assets/anyiose-runtime-notices.txt).

## What it does

- Preserves the existing AnyiOS module chooser, library screens, settings and design.
- Imports iOS IPA files or extracted `.app` bundles into one private library shared with Converter. Original imports remain untouched.
- Runs eligible **ARM32** iPhone OS applications. Universal ARM64+ARM32 bundles use their validated ARM32 slice; ARM64-only apps are unsupported.
- Keeps runtime data private per game, with folder and ZIP export actions. Folder export creates an `org.anyios` directory in a user-selected SAF location when Android permits it.
- Game details show the emulator runtime, best-effort game-engine identification, minimum iOS version, device families and required/prohibited capabilities declared in the IPA or app bundle; an unidentified game engine is shown as unknown, and missing declarations do not prove a feature is unused. **Show latest log** previews the last run's actual per-game log and offers full export.
- Defaults the frame cap to the fastest supported display mode. **Match device Hz** is on by default and promotes 60-FPS display links toward the selected cap; it may speed up game logic and does not guarantee higher FPS. An explicitly saved off setting stays off.
- Manual presentation-resolution choices show landscape pixel dimensions and default to native; **Game layout** is hidden and automatic. Dynamic resolution is off by default and scales the selected presentation buffer without intentionally shrinking the view. Game-owned offscreen buffers may remain full-size, so a speedup is not guaranteed.
- The optional performance HUD shows measured FPS, process CPU normalized over available cores, RAM/graphics PSS and device-wide GPU load when a readable vendor counter exists. Android may restrict these counters, in which case GPU usage is unavailable rather than estimated.
- Game networking is enabled automatically in the main emulator and standalone Semi-Emulated APKs. Games may contact real servers or local devices over Android's active network, including metered mobile data; system restrictions still apply. A connected route is not proof that a server or the Internet is reachable. No Wi-Fi or mobile-data setting is changed.
- On Android devices with a magnetic-field sensor, CoreMotion's magnetometer supports pull-mode readings in microteslas; queue-handler callbacks are not implemented. Hardware, sensor coordinates and real-game behavior need device validation.
- Provides an off-by-default **Max clocks** request for best-effort Android performance hints and high-capacity CPU affinity. Android controls frequencies; AnyiOSEMU cannot force them.
- Builds the bundled ANGLE EGL/GLES libraries from the pinned Google ANGLE source. ANGLE is the default emulator driver; native GLES is an explicit setting, and any fallback is recorded.
- Keeps the original C++ import inspection and standalone ANGLE surface test as diagnostics; they are not the game execution engine.
- Keeps Converter separate. Force convert offers the existing Native diagnostic boot attempt or an independent Semi-Emulated game APK; neither implies general conversion or gameplay success.

**Compatibility, visible rendering and gameplay require real-device, per-game validation.** A successful compile, clean exit code, instruction count or GL-call log is not proof that a game menu is visible or playable. See [device testing](docs/DEVICE_TESTING.md) and [current progress](docs/PROGRESS.md).

## Runtime and architecture

Force convert offers **Native runtime**, which uses the existing diagnostic game-runtime boot attempt, and **Semi-Emulated**, which packages an independent APK with the original IPA and AnyiOSEMU. The latter starts one game without a preparation screen, requests maximum-FPS display-link callbacks by default, and does not require AnyiOS to remain installed; it is not native recompilation and playability is unverified. The Semi-Emulated route requires an analyzed, unencrypted ARM32 slice.

AnyiOS launches an imported game in the pinned Rust/SDL AnyiOSEMU runtime. The Android activity forwards the selected source, private per-game data directory, display-aware frame cap, performance options, ANGLE/native-GLES preference, Force Composition and verbose-GLES preference. Source revisions and settings are recorded in the session report; the latest runtime log is stored separately per game.

## Game data and Android storage

The Android application/package ID remains **`org.podnative`** (not `org.anyios.com`), preserving app upgrades. Imported games and writable runtime data stay in internal app-private storage at `/data/user/0/org.podnative/files/games/<game-id>/sandbox/AnyiOSEMU`.

In a game's details, choose **Export game data to folder…** to copy its data beneath `org.anyios` in a folder selected through Android's document picker, or **Export game data as ZIP…** to save an archive. The folder export is a copy; live saves remain private. The picker may block storage roots, Downloads, `Android/data` and `Android/obb` on recent Android versions. Choose a permitted location; AnyiOS does not bypass those restrictions. Rebuildable caches are excluded.

For a new **Semi-Emulated** force conversion, the APK remains self-contained and
on launch attempts to create `Android/obb/<package>/main.1.<package>.obb` as a
verified mirror of the embedded IPA. The private IPA and saves remain the only
live source; unavailable, full, or read-only OBB storage must not prevent launch.
The Converter can also share that IPA under the expansion filename. The mirror
uses additional storage and verifying it can lengthen startup. The **Native**
diagnostic builder and the narrow bounded converter also attempt to emit a
resource OBB for sharing after building a self-contained APK. Their launchers
create the app-specific directory when available but do not install or load the
sidecar; the APK works without it. Android storage restrictions may prevent
manually placing these files. They are not Google Play delivery and do not
reduce APK size.

See [architecture](docs/ARCHITECTURE.md), [ANGLE integration](docs/ANGLE_INTEGRATION.md), and [third-party notices](app/src/main/assets/anyiose-runtime-notices.txt).

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
