# Pinned upstream runtime and AnyiOSEMU

**Snapshot: 2026-10-26.** AnyiOSEMU integrates the exact upstream source revision recorded in `third_party/anyiose-runtime-revision.txt`; it is the selected Android **game-execution engine**, not a rejected candidate. This document describes product/integration scope only. It is not an independent compatibility, rendering, security or speed test, and AnyiOS does not inherit upstream game-compatibility claims without testing those games in its own APK/device configuration.

| Area | Pinned upstream runtime | AnyiOS / AnyiOSEMU integration |
|---|---|---|
| Runtime | High-level iOS app emulation with a Rust guest/runtime and Android SDL host. | Uses that pinned runtime to execute eligible imported ARM32 iOS apps inside the AnyiOS Android app. |
| Product UI | Upstream Android host/application picker and its own identity. | Preserves the AnyiOS module chooser, library, import/export, settings and design. The visible runtime product name is AnyiOSEMU. |
| Import/library | Upstream runtime input/application workflow. | Uses the AnyiOS shared private library and retained original IPA or `.app` source. Launching a universal ARM32+ARM64 binary selects ARM32; ARM64-only remains unsupported. |
| Graphics | Upstream Android SDL/EGL/GLES integration. | Builds the pinned Google ANGLE EGL/GLES libraries expected by the runtime, uses ANGLE by default, records driver fallback and exposes an explicit native-GLES option. Rendering in AnyiOS still needs device validation. |
| Data and reports | Runtime-owned paths and logs from the pinned source. | Supplies per-game private data, validates provider paths under app storage, persists bounded session reports and includes source/ANGLE revisions. Folder and ZIP exports are user-triggered SAF copies. |
| Converter | Not part of the guest runtime. | Remains a separate AnyiOS module. Force convert offers a strict Native route when proven or an independent Semi-Emulated boot-attempt APK; the latter is not a complete conversion or gameplay guarantee. |
| Compatibility | Upstream documentation and test claims belong to the upstream build/configuration. | No title is declared playable until the AnyiOSEMU APK displays its real menu and responds to input on a device. A build, host trace or inherited claim is insufficient. |

## Consequence for previous AnyiOS prototype notes

The C++ ARM32/Dynarmic guest runner, custom guest framework/GLES path and ZIP-backed game sandbox described in earlier progress entries are historical prototype work, not the current Android game-launch path. The AnyiOS C++ importer/slice inspection and independent ANGLE surface-test diagnostics remain, but they do not execute games. See [`RUNTIME.md`](RUNTIME.md), [`ARCHITECTURE.md`](ARCHITECTURE.md), and [`DEVICE_TESTING.md`](DEVICE_TESTING.md) for the active path and evidence gates.

Upstream attribution and licenses are retained; rebranding the visible emulator UI does not rename or conceal its source. See [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
