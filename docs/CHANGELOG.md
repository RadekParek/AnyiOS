# Changelog

## 2026-10-28 — Compatibility and per-game conversion settings

### Added
- Per-game **Convert settings** for new Semi-Emulated APKs: an optional 4:3
  1024 × 768-point landscape profile with 2× rendering (768 × 1024 in
  portrait), offline mode, a performance request, and startup rotation. All
  overrides default off. Rebuild and reinstall an APK after changing them;
  Native diagnostic and other APK types do not use these settings.
- Offline packages omit Android's Internet permission and turn off guest
  networking. The performance request uses Android's sustained-performance
  mode when supported and existing CPU hints/affinity; Android still controls
  clocks and there is no guarantee of higher FPS or maximum CPU/GPU clocks.
- ARMv5TEJ (`CPU_SUBTYPE_ARM_V5TEJ`, `0x7`) is an explicitly lower-priority
  ARM32 fallback in the loaders, analyzers and converter. This does not provide
  Jazelle Java bytecode execution; a supported ARMv6/v7 slice remains preferred.
- The two Ninjago titles are labeled “Big Blue Bubble engine (reported)” for
  their exact bundle identifiers. Big Blue Bubble's role as developer is
  documented; the engine identification is user-reported, not verified.

### Changed and rewired
- Converted standalone APKs carry their settings as signed metadata. Their
  existing default remains the device-sized screen, networking on, normal
  startup rotation and fastest requested display mode. The main app continues
  to default **Match device Hz** on, while respecting an explicit saved choice.
- Normalized harmless IPA member/icon spellings without stripping real spaces;
  rejected icon metadata no longer aborts import. Traversal, duplicate/case
  collisions, links, ZIP bombs and containment checks remain in place. The
  emulator's IPA reader recognizes the same safe spelling variations and avoids
  pulling in a similarly named sibling bundle.
- dyld now reports actual image slides and stable image-name pointers. `dlopen`,
  `dlsym`, `dlclose` and `dlerror` track preloaded guest/host libraries, scoped
  handles and per-thread errors, without exposing private guest symbols. Runtime
  loading of arbitrary new images, host data exports and complete dependency
  ordering are still unsupported; C++ ABI interception remains in scope.
- The Core Animation scheduler keeps deadlines when a forced frame arrives
  early and skips missed deadlines without catch-up bursts. Native GLES1 keeps
  synthetic GL errors per context until queried. These are correctness fixes,
  not measured device-performance claims.
- Repeated selector and NIB messages are bounded per session, and the main
  app no longer duplicates game metadata already shown in details. Low-address
  and nil-isa diagnostics include context without treating invalid objects as
  valid or claiming a proven cause. `sel_getName(NULL)` returns the documented
  stable string.

### Removed
- The session-badge setting and launch overlay were removed. Old preferences
  and stale launch extras cannot show the badge.

### Validation and limitations
- Host checks exercise the real loader/scheduling code with mocks, FAT/Mach-O
  and native runtime tests, and ZIP/manifest/options regressions; they do not
  prove device rendering, actual frame rates, audio playback, clock speeds or
  successful gameplay. Terraria's reported audio assertion was not reproduced:
  its IPA was not supplied, and no root cause was inferred from the log alone.
- AnyiOSEMU TestApp requires its separate LLVM 12 and common-3.0 SDK: **TestApp
  not run** in this environment. See PR checks for Android/Linux/Windows builds
  and tests; physical-device validation remains outstanding.
