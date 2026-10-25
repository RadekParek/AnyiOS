# HyperHLE-Reborn and AnyiOSEMU

**Snapshot: 2026-10-25.** AnyiOSEMU integrates the exact HyperHLE-Reborn source revision recorded in `third_party/hyperhle-revision.txt`; this is the selected Android **game-execution engine**, not a rejected candidate. This document describes product/integration scope only. It is not an independent compatibility, rendering, security or speed test, and AnyiOS does not inherit upstream game-compatibility claims without testing those games in its own APK/device configuration.

| Area | HyperHLE-Reborn upstream | AnyiOS / AnyiOSEMU integration |
|---|---|---|
| Runtime | touchHLE-based high-level iOS app emulation with a Rust guest/runtime and Android SDL host. | Uses that pinned runtime to execute eligible imported ARM32 iOS apps inside the AnyiOS Android app. |
| Product UI | Upstream Android host/application picker and its own branding. | Preserves the AnyiOS module chooser, library, import/export, settings and design. The visible runtime product name is AnyiOSEMU. |
| Import/library | Upstream runtime input/application workflow. | Uses the AnyiOS shared private library and retained original IPA or `.app` source. Launching a universal ARM32+ARM64 binary selects ARM32; ARM64-only remains unsupported. |
| Graphics | Upstream Android SDL/EGL/GLES integration. | Builds the pinned Google ANGLE EGL/GLES libraries expected by the runtime, uses ANGLE by default, records any driver fallback and exposes an explicit native-GLES option. Rendering in AnyiOS still needs device validation. |
| Data and reports | Runtime-owned paths and logs from the pinned source. | Supplies per-game private data, validates the provider paths under app storage, persists a bounded session report and includes the engine/ANGLE revisions. |
| Converter | Not part of upstream. | Remains a separate AnyiOS module, unchanged by this engine integration. |
| Compatibility | Upstream README lists its own titles/statuses, which reflect upstream builds and tests. | No title is declared playable until the AnyiOSEMU APK displays its real menu and responds to input on a device. A build, host trace or inherited upstream claim is insufficient. |

## Consequence for previous AnyiOS prototype notes

The C++ ARM32/Dynarmic guest runner, custom guest framework/GLES path and ZIP-backed game sandbox described in earlier progress entries are historical prototype work, not the current Android game-launch path. The AnyiOS C++ importer/slice inspection and independent ANGLE surface-test diagnostics remain, but they do not execute games. See [`RUNTIME.md`](RUNTIME.md), [`ARCHITECTURE.md`](ARCHITECTURE.md), and [`DEVICE_TESTING.md`](DEVICE_TESTING.md) for the active path and evidence gates.

The integration retains upstream attribution and licenses; rebranding the visible emulator UI does not rename or conceal its source. See [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
