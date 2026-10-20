# PodNative

An independent Android-native foundation for researching ARM32 iOS game compatibility.
**Not yet an emulator capable of running games. A signed debug APK has been built
and published; no rendered frame has been verified on a device.**

[Download the first debug build](https://github.com/RadekParek/PodNative/actions/runs/39495542750/artifacts/12297629398)
(extract GitHub's archive, then install `PodNative-debug.apk`).
[Build evidence and exact source revision](docs/FIRST_APK_REPORT.md).

## First slice

* Native Android library/inspection flow, settings, diagnostics and log export.
* PodNative-owned graphics backend; source-pinned upstream ANGLE embedded Vulkan
  integration, with explicit failure and no disguised native-GLES fallback.
* Shader-based animated textured cube with depth testing and reusable resources.
* Bounded ARM32 Mach-O inspection and temporary validated guest-memory mapping.
* Actual Dynarmic CPU adapter with host-tested ARM/Thumb execution; Android CPU integration remains pending.
* GitHub Actions debug/tagged-release build recipes with artifacts and provenance.

No inherited TouchHLE/HyperHLE renderer. No proprietary game files. No universal
compatibility or performance claims. Ninjago stages A–G are all unverified.

## Build / test

Host: `scripts/test-host.sh` (g++/Python required).
Real CPU engine tests: `scripts/test-cpu.sh` (CMake/C++20/git required).
See [runtime contracts and device test](docs/RUNTIME.md).

Android: follow [ANGLE integration](docs/ANGLE_INTEGRATION.md), then use Gradle
8.11.1/JDK17: `gradle lintDebug testDebugUnitTest assembleDebug`.
Real upstream ANGLE libraries are mandatory build inputs; no stub mode exists.
See [device tests](docs/DEVICE_TESTING.md) before claiming rendering success.

CI debug artifacts are published only after a successful workflow and are named `PodNative-debug.apk`.
Tagged builds produce `PodNative.apk`, SHA256SUMS and BUILD-INFO.txt. Without
signing secrets the release output is explicitly unsigned and not installable.
Public GitHub Release publication additionally requires the repository variable
`PODNATIVE_PUBLISH_RELEASE=true` after license/device review and configured signing.

## Engineering records

[Current progress](docs/PROGRESS.md) · [Runtime](docs/RUNTIME.md) · [Plan](docs/PLAN.md) · [Research](docs/RESEARCH.md) ·
[Architecture](docs/ARCHITECTURE.md) · [ANGLE](docs/ANGLE_INTEGRATION.md) ·
[Ninjago status](docs/NINJAGO_COMPATIBILITY.md) ·
[Third-party notices](THIRD_PARTY_NOTICES.md) · [Build report](docs/BUILD_REPORT.md)
