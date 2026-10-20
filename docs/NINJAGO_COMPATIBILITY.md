# LEGO Ninjago: Spinjitzu Scavenger Hunt

**Status: NOT COMPATIBLE / NOT TESTED.** No game execution, first frame, menu,
gameplay, audio or save result exists. The user's legal copy stays on their device.

Game version/build: **unknown**. Executable SHA-256: **not recorded**.
Architecture: expected historical ARM32 target, **not verified for the user's copy**.
Device: user reports Samsung Galaxy S23 Ultra, ARM64/Adreno/12 GB RAM;
OS build, firmware and Vulkan driver: **not recorded**.

| Stage | Acceptance | Evidence / current status |
|---|---|---|
| A | Recognize AND load executable | Not reached. Thin ARM32 inspection and temporary mapping validation exist; no game tested. |
| B | Guest execution and framework initialization | Not reached; CPU host fixtures pass; no game execution or frameworks. |
| C | Valid guest EAGL graphics context | Not reached; synthetic host EGL is not guest EAGL. |
| D | First visible game frame | Not reached; no captured game output. |
| E | Menus and 3D scenes | Not reached. |
| F | Actual gameplay and input | Not reached. |
| G | Audio, lifecycle and saves | Not reached. |

## Private test procedure

1. First pass the separate textured-cube graphics gate in DEVICE_TESTING.md.
2. Record game CFBundleShortVersionString/CFBundleVersion, exact executable name,
   CPU type/subtype and SHA-256 locally. Do not upload executable/assets to Git/CI.
3. Import your legal IPA or thin executable using the Android document picker.
   Current import is **inspection only** (64 MiB executable / 1 GiB archive caps,
   at most 8 private imports). It validates MH_EXECUTE, ARM32, command ranges,
   segment file ranges and encryption metadata, not loadability or compatibility.
4. Import diagnostics now also attempt temporary fixed-address mapping and report
   `MAPPED_ARM32` or `LOAD_BLOCKED`; mappings are released, not executed. Export diagnostic log. `INSPECTED_ARM32` is not Stage A. Fat, ARM64,
   malformed or encrypted files are explicit limitations; no DRM bypass is supplied.
5. After future loader/CPU integration, capture stage transition, guest PC,
   unresolved symbol/selector/API, first-frame/presentation result and crash log.
   Keep private assets out of shared diagnostics.
6. For each reached stage attach source revision, app build/signature, device/driver,
   game version, test steps and real evidence. Record the last completed stage.

## Black-screen triage (future executable runtime)

Distinguish: no guest execution → framework blocker → no EAGL context → no drawable
→ incomplete framebuffer → shader/texture failure → no draw → no swap → lifecycle
loss. Record errors and first/last call at each boundary. A clear/background color,
Android UI or synthetic cube is never a visible game frame. A black-screen game
must not be called compatible.

## Remaining blockers

Game-validated loading, dyld/relocations/symbol handling, ARM32 execution on Android,
Objective-C/Foundation/UIKit semantics, EAGL/Core Animation, game-specific GLES
coverage/PVRTC, touch controls, audio, timer synchronization and saves. Prioritize
from the user's traces once an executable runtime exists, not guessed API counts.
