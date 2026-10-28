# Changelog

## 2026-10-28 — Legacy Mach-O ranges, clearer reports and rendering safeguards

### Fixed / changed
- Empty file-backed Mach-O segments and empty symbol, relocation, dyld and
  encryption ranges no longer fail solely because a legacy linker left a stale
  unused offset. Nonempty ranges remain bounds-checked; errors now include the
  range and FAT slice context. Synthetic ARMv6 thin and FAT cases pass, but the
  specific Ninjago IPA/report was not available, so its error is not proven fixed.
- Converter game details now separate completed/packaged work, available providers
  that are not proven game-linked, and remaining problems. Host-reported ELF
  symbols/dependencies are explicitly distinguished from the device's hash and
  header checks. Removed host-command, attach-host-APK, open-installed-app and
  build-preview controls from game details; prior previews, logs and full reports
  remain accessible under More details. Force-convert explanations are shorter.
- Corrected the Native GLES guest float-argument ABI. The Native bridge is ES 1
  only: it now rejects an ES 2/3 context rather than masquerading an ES 1 context
  as ES 2, and no longer uses an ES 2 EGL config as a fixed-function substitute.

### Added / rewired
- Added a bounded Native guest `ASIdentifierManager` and all-zero IDFA object.
  Tracking reports disabled; no Android advertising identifier is accessed. This
  is not an ad service, a complete NSUUID implementation or proof a game linked it.
- AnyiOSEMU's existing ES 2 → native GLES 2 path now forwards explicitly sized
  shader source including embedded NULs instead of silently replacing the entire
  shader with an empty string, and avoids one per-chunk copy. This does not add
  an ES 2 path to the separate Native guest or an EGL renderer to portable C.
- Added a two-region, permission-checked lookup cache for Native guest memory
  callbacks and a bounded C-string scan fast path for AnyiOSEMU. No Dynarmic
  fastmem or floating-point accuracy flags were changed. Local host microbenchmarks
  showed faster repeated reads/scans; this is not a device or frame-rate benchmark.

### Validation / limits
- Synthetic tests cover empty/nonempty ARMv6 ranges, privacy-preserving AdSupport,
  memory remapping/copy/move and GLES ARM argument forwarding with a mock driver.
  No GPU frame, real-game execution or physical device was tested. A focused Rust
  memory harness was run; the engine's full TestApp was not run. CADisplayLink's
  guest Objective-C ABI, full Core Animation, arbitrary callsite rewrites and
  portable-C rendering remain incomplete.

## 2026-10-28 — Safer large imports, issue-focused reports and 2× display profiles

### Fixed / changed
- Relaxed Converter ZIP handling for real highly compressible resources without
  disabling bomb protection. A non-ZIP64 member may now be up to 4,294,967,294
  bytes, and an entire archive shares a 256 MiB allowance for expansion beyond
  250:1. Larger excess, encrypted entries, links, traversal, duplicate names,
  CRC/size mismatches and other unsafe ZIPs still fail. The specific member and
  limit are reported. Free-space preflight now reserves only bytes still to be
  written, rather than charging for the already-stored IPA or previously copied
  data. The bounded static APK packager checks large resource sizes before
  buffering them; its separate limits still apply.
- Converter's game detail and exported compatibility summary emphasize actual
  blockers, unverified dependencies and unresolved API needs. Fully resolved
  entries and completed analysis/port-progress cards are omitted. Missing or
  truncated inventories remain visible, and a catalogued provider is not
  reported as proof of a linked game. The complete `report.json`, analysis
  history remains retained and logs remain accessible; manual analysis and retry are retained.
- Replaced the previous **480 × 320 points, 3:2, 4×** choice with **2×** and
  added a separate **1024 × 768 points, 4:3, 2×** choice. They are mutually
  exclusive and off by default. Older `classic43` settings retain their prior
  3:2 meaning. At content scale 1, their full landscape backing drawables
  request 960 × 640 and 2048 × 1536 pixels respectively. AnyiOSEMU now honors
  scaling in two game-specific fixed-drawable paths and gives an explicit screen
  size precedence over those paths; size multiplication is saturating and
  clamped. Game-owned texture targets remain unscaled, and actual graphics
  allocations depend on content scale and driver limits. Existing standalone
  APKs contain a settings snapshot: rebuild and reinstall to apply a change.

### Added / rewired
- Added an optional host **Android NDK + CMake** backend for the existing ARM32
  to portable-C translator and a `radek relink` command for its generated output.
  The direct NDK-clang backend remains the default for analysis. Both backends
  require verified Android ELF architecture, dependencies, translated-function
  exports and JNI entry before publishing a library. ABI-specific build caches
  are isolated; a failed relink preserves the last verified library and handoff.
  Converter can copy the host command and import the verified ARM64 handoff;
  ARMv7 libraries can be built but the Android handoff importer is ARM64-only.
  The lifter now accepts classic executables that legitimately lack optional
  stub/pointer/initializer/exception sections while still rejecting inconsistent
  exception metadata. It opens large IPAs without copying the whole archive to
  host memory and retains a separate 512 MiB executable lifting limit.
- The CMake backend links generated C and an existing runtime; it does **not**
  turn arbitrary Mach-O frameworks into Android libraries or implement missing
  framework behavior. The translated runner still lacks an EGL/GLES connection;
  a verified ELF or imported handoff is not proof of a playable game or APK.

### Validation / limits
- Added ZIP, profile, report and portable-C linker regressions. Local host tests
  cover safe expansion, cleanup, settings migration, issue filtering and synthetic
  code generation; a synthetic host CMake build compiles the project, and its
  x86-64 ELF is correctly rejected as an Android ARM library. CI is configured to
  exercise real NDK/CMake ARM64 and ARMv7 links and the ARM64 handoff.
- No affected IPA or physical device was available. The exact previously reported
  ZIP failure, visual scaling, rendering, FPS, and gameplay remain unverified on
  the affected games. Hardware limits and texture-backed render targets can
  prevent a uniform apparent 2× increase. Upstream TestApp was not run locally.

## 2026-10-28 — 4× classic profile, diagnostics and launch polish

### Fixed and rewired
- The existing **480 × 320 points (3:2)** Semi-Emulated profile now requests
  4× rather than 3×. At layer content scale 1, a full landscape drawable is
  requested at 1920 × 1280, or 1280 × 1920 in portrait; content scale and
  hardware limits affect actual allocation. Rebuild and reinstall an existing
  standalone APK for the changed per-game setting to take effect. Memory and
  GPU costs rise; game-owned texture targets remain at their own size.
- AnyiOSEMU now tracks framebuffer scale state per EAGL context. Viewport and
  scissor requests use the same scaling policy, are reapplied when draw
  bindings or color attachments change, and their GL queries return guest-space
  values for tracked requests. Deleted/reused framebuffer and texture
  names no longer carry stale scale classifications. The ES 3 multisample
  renderbuffer path is scaled, layered texture attachments are tracked, and
  blit source/destination coordinates follow their respective targets. Normal
  1× calls retain their previous direct path. Mixed-size multisample resolves,
  pixel readback and game-specific rendering paths still require device testing.
- Converter no longer treats a structurally valid Java `.class` resource as a
  Mach-O image solely because both start with `CAFEBABE`. Other recognized
  Mach-O/FAT files, including malformed ones, still go to the decoder and can
  block with a real error. The details screen now shows the failing image path
  and reason. Artifact-free failed analyses and blocked analyses with embedded
  decode failures can be retried from their shared source; the old report is
  copied into `analysis-history` before retry, and retry does not auto-build.

### Changed and added
- Added breathing room around the Emulator Settings heading, refined the
  shared static glass/gradient palette and motion timing, and used glass for
  Emulator sheets. The Emulator library launch requests an Android scale-up
  transition from the game tile, only when animations are enabled and an anchor
  exists. It falls back safely otherwise. Standalone Semi-Emulated APK launch
  is unchanged and does not use this transition.
- Reduced repeated guest-memory region searches on host writes and switched
  the Native diagnostic runtime's shim-callout address lookup to a hash table;
  timing statistics and permission/dirty-page behavior are retained. This
  affects the Native diagnostic translation/Dynarmic callback path, not the
  separate AnyiOSEMU engine's JIT configuration.

### Validation and limitations
- Added framebuffer-state, Java-class/Mach-O recognition, retry-policy/history
  and guest-memory dirty-page regressions. Host Debug CTest passed 17/17 both
  without Dynarmic and with the pinned Dynarmic A32 backend required by its
  CPU tests. A local x86-64 synthetic benchmark
  of 600,000 operations across 1,024 regions/callouts measured median host
  writes of 86.1 ms before vs 50.5 ms after, and callout dispatch of 62.8 ms
  before vs 30.4 ms after (seven samples each, GCC 12 `-O3`). A separate
  pinned-Dynarmic Debug A32 workload ran 100,000 guest calls to one shim,
  writing across 1,024 regions with 1,024 callouts registered. Across 21
  alternating-order samples including JIT creation, the median was 301.4 ms
  before vs 290.2 ms after (`-O3` host helpers). These are synthetic diagnostic
  runtime results, not game FPS, primary AnyiOSEMU or Android results.
- No affected IPA/report/log was provided, so the Java-class collision is a
  plausible fix for one cause, not a confirmed diagnosis of the reported
  embedded-image error. Real image decoding failures still need their reported
  path and reason. No physical device or affected game has validated graphics,
  transitions, game speed or performance. Upstream iOS TestApp requires its
  separate historical SDK and was not run locally.

## 2026-10-28 — Manual Converter analysis and shared library refresh

### Removed / rewired
- Removed the automatic Converter analysis that ran after Emulator imports,
  along with its automatic-only progress/error notices and heap preflight.
  Emulator still inspects the source's normal launch metadata. No existing
  reports, game sources, or generated outputs are deleted. Open an imported
  game in Converter and choose **Analyze shared source** to analyze it; IPA,
  folder and executable analysis remain available. Choosing an IPA directly
  in Converter still starts its explicitly requested import/analysis, including
  its existing narrowly proven single-routine auto-build behavior.
- The 256 MiB native analyzer executable-size guard remains in place before
  manual analysis allocates the executable. Manual analysis is still subject
  to device memory; its prior behavior has not been made more restrictive.

### Added / changed
- Converter now presents the same shared games as adaptive artwork tiles,
  ordered by their shared library names. Tapping a tile opens that game's own
  details, existing reports and build actions. Unanalyzed games show their
  source metadata and an explicit analysis action. The grid loads 60 entries
  at a time, preserves scroll when expanded or refreshed, and retains an open
  shared-source detail across activity recreation. Artwork is decoded off the
  UI thread at a bounded size, with a 16 MiB cache and stale queued views
  skipped rather than retaining old screens.
- Both modules have a visible **All modules** button on every module screen;
  Android Back from either library also returns to the Emulator/Converter
  picker. Navigation clears intervening module activities without cancelling
  the shared background job or touching game files. An older Converter activity
  cannot disconnect the current activity's job-progress listener when destroyed.
- Refreshed the picker and both modules with a common palette, translucent
  glass-inspired gradients/highlights, outline-vector fallback art instead of
  decorative glyphs, and responsive grid sizing for narrow screens and large
  text. Short page and press transitions respect Android's animation settings;
  cards use static gradients rather than expensive live backdrop blur.
- Bundled the Manrope variable font (Google Fonts repository, commit
  `b31870aff700ab7a1d74fa0c6887d95beb9e0037`, font SHA-256
  `3ae11c49db0455a3cc33e37d380f20fdb8c7f8b41dc07625c177e3d87a9d6ae6`).
  The font family selects its regular and bold weight axes explicitly. Its SIL
  Open Font License is shipped in `assets/typography/manrope-ofl.txt` and
  exposed in the app's notices; Android's system font is a fallback.

### Validation and limitations
- Added import/report-preservation, manual-analysis, responsive grid, tile,
  pagination, activity-recreation and picker-navigation regressions. Local
  Kotlin and Java compilation used Android 35 stubs and stubbed APK-signing
  dependencies; local pure-JVM size/guard and artwork sizing tests (4) and
  shared-catalog tests with a fake Android context (7) pass. Full Android/host
  CI is tracked with the pull request. A physical device has not been used to
  verify rendering, typography, transitions, navigation stacks or game play;
  smoothness and exact glass appearance therefore remain unverified. The
  actual Ninjago IPA remains unavailable and its failure is not confirmed fixed.

## 2026-10-28 — Shared analysis, display profile, package identity and polish

### Added
- Emulator imports are analyzed in the Converter under the existing shared job
  lease, using the original private source. IPA, extracted app folder and
  executable-only analysis are supported; only IPAs currently support APK
  packaging. Cancelling analysis keeps the already-imported game. The emulator
  does not automatically build an APK when adding a game.
- Per-game Semi-Emulated settings now offer **480 × 320 points at 3×** (3:2),
  with 320 × 480 in portrait and a requested 1440 × 960 / 960 × 1440 render
  size. Startup rotation offers −90°, +90° and 180° plus the default.
- Actionable compatibility needs can be viewed or exported, with resolved
  providers omitted and incomplete/unverified evidence identified.

### Changed and rewired
- New game APKs use the exact iOS bundle identifier as their Android package
  when it is a valid Android ID; Android's OBB folder and filename then match
  that package. Invalid identifiers use a signer-bound generated fallback.
  Existing generated APKs keep their prior identity and remain shareable.
- Runtime variants using the same ID cannot be installed side-by-side and can
  conflict with an existing Android app or signing key. Installation warnings
  recommend backing up existing data. The standalone IPA remains self-contained;
  its optional OBB mirror never overwrites a different existing OBB.
- Native and bounded optional OBBs are stored separately in the private
  library, even when their Android filenames coincide. Share URIs select the
  correct runtime and validate source, package and digest. Folder and bare
  executable reports no longer offer IPA-only packaging; builders also reject
  these source types while accepting legacy IPA reports. On-device analysis
  now applies the native analyzer's existing 256 MiB executable limit before
  allocating instead of attempting to read up to 1 GiB first.
- Converter ZIP extraction and executable validation preserve legitimate
  colon-containing filenames and give specific Info.plist diagnostics. Path
  traversal, links, normalized collisions, expansion and containment checks
  remain enforced. The Ninjago IPA was not supplied, so its exact failing name
  and end-to-end result are not verified.
- Force-convert progress stays on the selected game's details and preserves
  scroll position. The module picker, Emulator library and Converter have a
  refreshed, shared game-library visual language.

### Removed / replaced
- The previous 1024 × 768 / 2× profile and new portrait/landscape selector were
  replaced by the 3:2 profile and degree choices. Legacy saved portrait and
  landscape choices remain readable; the old profile opt-in migrates to the
  new profile. Default behavior stays unchanged.

### Validation and limitations
- Local standalone storage/options tests (12), storage/ZIP tests (11),
  artifact/settings contracts (13), API mapper (21), source-policy (2),
  analysis-memory (2),
  binary-plist/ZIP safety tests (11 selected), and shared-catalog tests (6)
  pass. The last used a fake Android context rather than Robolectric; several
  contract tests used JSON-java and a stub runner. Converter Kotlin and main
  app Java compiled against an Android 35 jar with stubbed build dependencies;
  formatting checks pass. CI and physical-device validation are tracked with
  the pull request. The real
  Ninjago IPA and actual device rendering, touch rotation, performance and OBB
  provisioning were not tested here. **TestApp not run** (requires its separate
  LLVM 12 and common-3.0 SDK). Optional native/bounded OBB exports are not
  loaded by their launchers; Android may restrict manual OBB placement.

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
  emulator's IPA reader recognizes safe spelling variations, preserves valid
  colon-containing guest resource names and excludes similarly named siblings.
- dyld now reports actual image slides and stable image-name pointers. `dlopen`,
  `dlsym`, `dlclose` and `dlerror` track preloaded guest/host libraries, scoped
  handles and per-thread errors, without exposing local or private-external
  guest symbols. Runtime loading of arbitrary new images, host data exports and complete dependency
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
  and native runtime tests, and ZIP/manifest/options regressions. Android CI
  also tests permission removal against an AAPT-compiled standalone template;
  this does not prove signed-APK enforcement or device rendering, actual frame
  rates, audio playback, clock speeds or successful gameplay. Terraria's reported audio assertion was not reproduced:
  its IPA was not supplied, and no root cause was inferred from the log alone.
- AnyiOSEMU TestApp requires its separate LLVM 12 and common-3.0 SDK: **TestApp
  not run** in this environment. See PR checks for Android/Linux/Windows builds
  and tests; physical-device validation remains outstanding.
