# AnyiOS integration plan

Requested 2026-10-21. Preserve Android applicationId `org.podnative`, existing `files/games` entries and development signing identity; rename the public app/project/repository AnyiOS. Do not rename the session branch or local checkout root.

## Acceptance and sequence

1. Reproduce the reported `DYLD_ADDRESS` rejection using synthetic opcode streams. Cursor arithmetic is not a memory write: allow terminal stride/skip beyond a segment, reject arithmetic overflow and every out-of-range actual fixup. Preserve transactional application and fail-closed unsupported startup.
2. Import each IPA in a selected SAF directory tree independently, bounded traversal/cycle checks, cancellation and per-file outcomes. Keep extracted app-folder import separate.
3. Integrate the owner's converter at upstream e20c76f33fec87bb311711074c219c3fd32f1911 as a source Android library, with its template builders and native components; never import private/proprietary test games. Original converter repository remains independent. No converter renderer replaces the independent ANGLE emulator.
4. Launch into Emulator / Converter selection. Use converter-style teal cards/palette across modules. Retain conversion disclosures and distinct modules.
5. One canonical `files/games/<id>/source.bin` per IPA. Converter analysis/output lives alongside, never replaces the source. Both lists include all committed entries, including not-analyzed/unsupported sources. Deleting a shared entry requires explicit confirmation and removes it from both. Recovery must not delete emulator entries or canonical source bytes. Serialize jobs across modules.
6. Review the sole fork, Gitsnup/iOStoDroid, against its common ancestor, separating renaming and merge effects from actual functional changes. Do not infer improvements from line count.
7. Run native regression, converter native/Python tests, Android unit tests/lint/build and unified Android/PC CI. Verify artifact contents and source provenance before delivering an APK. No device/gameplay claim without evidence.

## Boundaries

The report is static evidence of 539 imports, not a specification for all framework behavior. Fixing cursor validation does not implement Objective-C, UIKit, EAGL, libc/C++ ABI, audio and every imported API. Preserve explicit runtime blockers rather than returning invented success from unknown imports. The converter's experimental runtime APK is not a proven complete-game conversion. Official Apple dyld format sources remain references, not a Darwin runtime dropped into Android.

## Source/license handling

Converter inclusion is explicitly requested by its owner. Upstream has no root license declaration; do not invent a broad open-source license for its original code. Preserve upstream attribution and third-party notices; record pinned dependency revisions. Review additional runtime dependencies before distribution. Exclude upstream proprietary IPA fixtures, generated binaries, caches and build outputs.

## Implemented integration and validation record

- Owner choices confirmed: preserve `org.podnative`; one catalog with confirmed cross-module deletion; recursive IPA directory search.
- `converter/` includes the pinned source, Android UI/analyzer, all three APK template builders, host CLI and tests. It is an Android library dependency of the existing application, not a second installed app or a launcher pointing to an unavailable package.
- Launcher is `org.podnative.ModuleActivity`; Emulator and Converter are internal activities. Result URIs use the non-exported `${applicationId}.converter.results` provider and continue enforcing output contracts.
- Canonical source is `files/games/game-<UUID>/source.bin`; existing emulator entries require no migration or copy. Converter reports/outputs are separate files alongside it. Entries without converter reports are visible immediately with an explicit Analyze action. Re-analysis reads the existing source without opening it for writing. Recovery never deletes source bytes or an emulator entry for lacking a converter report.
- Shared cross-module job ownership prevents concurrent import/build/remove operations. Module views receive catalog updates. Confirmation text warns that removal affects both modules; generated converter APKs cannot replace the IPA.
- `Import game folder` recursively discovers IPA files and commits each independently. Extraction-folder import is a separate action. Directory depth 32, 30,000 document traversal/cycle checks, free-space checks and per-file failure/cancellation behavior remain explicit. No fixed whole-library size quota is introduced.
- Common converter theme tokens/preferences, module chooser, emulator label, artifact names and application label are AnyiOS. Native/JNI names and signing-cache identity are intentionally stable. Recovered converter artwork can supply emulator covers; existing covers also appear immediately in Converter.
- `DYLD_ADDRESS` terminal-stride regression is covered for rebase/bind single and repeat forms. Actual out-of-segment next fixups and 64-bit cursor arithmetic overflow still fail. Local native regression suite including 65,000 deterministic malformed-input mutations passed. The private Ninjago executable was not available locally, so device confirmation is still required.
- Local converter native analyzer/runtime/API regression tool passed; Python suite: **395 tests, 51 skips**. Separate compatibility database/contract tests: **7 passed**. Skips identify unavailable private-game fixtures or toolchain-dependent tests; they are not runtime/gameplay evidence. Only synthetic fixture generators run in CI.
- Android compilation/lint/JVM/unit integration tests and APK packaging verification run in the unified Android/PC workflow; no APK is declared verified before that workflow succeeds. Additional tests cover recursive SAF import, partial read failure, cycles, per-entry source bytes, shared not-analyzed entries, interrupted report recovery and cross-module job ownership.

### Remote rename limitation

`gh repo rename AnyiOS --yes` was refused with **HTTP 403: Resource not accessible by integration**. The app and local Gradle project are named AnyiOS, but GitHub's repository name is unchanged. The owner can use **repository Settings → General → Repository name → AnyiOS**. GitHub normally redirects the old URL; once changed, update the local origin URL as appropriate. Do not rename the Arena checkout root or session branch.

- HLE linker increment: session-owned two-level export graph (image/alias/dependency ordinals plus flat/weak lookup), checked strong-definition coalescing, native-thumb export parsing, and transactional graph-aware fixup application. Production launch now reports required-implementation coverage and unresolved examples instead of a generic symbol-resolution message.
- Twelve checked ARM SVC call gates implement bounded C memory/string operations plus the existing CoreFoundation bit-vector subset. Dispatch authenticates the gate PC, trap number, ARM mode and guest return address before invoking host code; unknown imports and forged traps remain hard failures. Real Dynarmic ARM and Thumb guest calls through these gates are included in the CPU correctness test; sanitizer service tests cover overlap, budgets, invalid returns and forged traps.
- Guest C runtime increment: owned guest heap, bounded string/number/math/atomic services and checked dispatch, raising implemented imports from 9 to 48 of the 438 unique names in the latest device report. Host sanitizer tests cover allocator invariants, parsing, the float/double call boundary and explicit non-implementation of `qsort`/`printf`/UIKit entry points.
- Architecture research of HyperHLE-Reborn (MPL-2.0) covered its general host-dylib export/interface separation only. No source or renderer was copied; the independent ANGLE renderer is unchanged.

### Execution, Objective-C and installer increment (2026-10-21)

- `runGuest` applies resolved dyld fixups, seeds an argc/argv/envp stack, and executes the
  real CPU in bounded slices (2M instructions / 2.5 s), dispatching host services and
  reporting instructions retired, final PC, host calls and the exact stopping reason.
- Required imports with no implementation bind to faulting gates instead of aborting launch,
  so a blocked title now reports the first missing API instead of refusing to start. Weak
  imports still bind to null.
- Objective-C runtime core: guest class objects, selectors as interned guest strings,
  `objc_msgSend`/`objc_getClass`/`sel_registerName`/retain/release/autorelease dispatch,
  NSObject lifecycle and identity, NSString length/UTF8String/hash/isEqualToString:, and
  NSAutoreleasePool push/drain. Unknown classes, selectors and receivers fail hard.
- Installer warning mitigation: the published artifact is now a non-debuggable release
  (`AnyiOS.apk`) signed with the cached preview certificate so it still upgrades an existing
  install. The converter keeps `REQUEST_INSTALL_PACKAGES` because installing generated APKs
  requires it; that permission is the remaining scanner-facing surface.

### ABI and CoreGraphics increment (2026-10-21)

- Call-boundary correction: iOS ARMv7 uses the **base** AAPCS variant. Floating-point
  and aggregate arguments travel in the core registers `r0-r3` and then the stack, and
  results come back in `r0-r3`; the VFP file is for arithmetic only. Apple's ARMv7 ABI
  reference lists only VFP register preservation and prologue shape as deviations from
  the ARMv6 rules, so the earlier implementation that read `s0`/`d0` for arguments was
  wrong. `runtime/abi.h` now owns that marshal: a word cursor over `r0-r3` then `sp`,
  even-register alignment for `double`, by-value aggregates of up to four words, and
  hidden-result pointers plus stack arguments for larger aggregates such as the
  six-float `CGAffineTransform`. `sin`/`cos`/`pow`/`fmod`/`strtod` and the `…f` variants
  were moved onto it.
- CoreGraphics subset: `CGAffineTransformMakeTranslation`, `CGAffineTransformTranslate`,
  `CGAffineTransformScale`, `CGRectApplyAffineTransform`, `CGRectEqualToRect`,
  `CGRectGetMidX`/`MidY`, `CGRectInset`, real colour-space handles with reference counts,
  and `CACurrentMediaTime` from a monotonic clock. `_CGAffineTransformIdentity` and
  `_CGRectNull` were already emitted as guest data and stay byte-identical.
- Proof: `tests/coregraphics_test.cpp` covers the marshalling directly, and the CPU
  correctness test now drives the real Dynarmic engine through an ARM guest that loads
  float bit patterns into `r0-r3`, calls the gate, and stores the result — including a
  hidden-pointer call, so the convention is exercised by executed instructions rather
  than asserted by a host helper.
- Coverage at the end of this increment was 72 of 438 required imports; the later
  increments below take it to **130 of 438**. This is API-surface progress; it is not
  evidence that any title launches or renders.

### Launch-blocker increment: dyld, stubs, msgSend and the launch UI (2026-10-21, sixth pass)

This pass answers the two failures in the latest on-device report
(`LAUNCH_BLOCKED: GUEST_SETUP: DYLD_TARGET: conflicting duplicate fixup`,
`DYLD_ADDRESS: invalid segment or cursor arithmetic overflow`) and the 350 undispatched
legacy stubs, then widens the runtime surface around `objc_msgSend`.

- **Duplicate fixups are merged, not rejected.** A link editor may list the same pointer in
  both the lazy and the plain binding stream; writing the same value twice is idempotent.
  `DyldPlan::applyWithGraph` now merges entries with identical target/addend (keeping the
  eager one) and still fails `DYLD_TARGET` when the targets genuinely differ.
- **Cursor strides are arithmetic, not validation.** `advance()` used to throw
  `DYLD_ADDRESS` whenever a stream advanced before selecting a segment, which rejected
  valid streams from this binary. Only real 64-bit overflow fails now; a fixup emitted with
  no segment is a precise `DYLD_SEGMENT`, and an out-of-range target stays `DYLD_TARGET`.
- **Legacy symbol stubs are dispatched.** The 350 `S_SYMBOL_STUBS` entries are decoded with
  their indirect symbol and ordinal, then rewritten before guest execution to
  `ldr r12, [pc] / bx r12 / .word target` — the resolved gate when the API exists, a named
  trap gate otherwise. Unresolved weak stubs bind to null, matching dyld. `Memory::patch`
  rewrites the executable segment without granting guest write permission.
- **`objc_msgSend` family.** Messaging nil returns 0 (real Objective-C semantics, no longer a
  guest fault); `objc_msgSendSuper2` reads `struct objc_super`; `objc_msgSend_stret` shifts
  the hidden result pointer for the scalar-returning classes and refuses struct returns it
  cannot model. Added `objc_getProperty`, `objc_setProperty`, `objc_copyStruct`,
  `objc_sync_enter/exit` (balanced bookkeeping, mismatch is an error),
  `objc_enumerationMutation`, `objc_autoreleasePoolPush/Pop`, `objc_retainBlock`,
  `objc_lookUpClass`, `sel_getName`, `NSClassFromString`, `NSSelectorFromString`, `NSLog`.
  Runtime entries are registered under `libobjc.A.dylib`, Foundation's under Foundation.
- **Bundle and asset paths.** `+[NSBundle mainBundle]`, `bundlePath`, `resourcePath`,
  `pathForResource:ofType:` (nil unless the file really exists in the mount),
  `NSSearchPathForDirectoriesInDomains`, `NSHomeDirectory`, plus
  `CFBundleGetMainBundle`, `CFBundleCopyResourcesDirectoryURL`, `CFBundleCopyBundleURL`,
  `CFURLGetFileSystemRepresentation` and `CFURLCopyFileSystemPath`.
- **Audio session state.** `AudioSessionInitialize`, `SetActive`, `SetProperty`,
  `GetProperty`/`GetPropertySize` implement the real AudioToolbox state machine: the
  category and other modelled properties round-trip, unknown property IDs return
  `kAudioSessionUnsupportedPropertyError`, wrong sizes return
  `kAudioSessionBadPropertySizeError`, and the route is a real `"Speaker"` string. No audio is
  rendered; playback APIs remain traps.
- **Timing and identity.** `mach_absolute_time` (monotonic, 1/1 timebase),
  `mach_timebase_info`, `sysctlbyname` for the keys a title actually queries
  (`hw.machine` = `iPod5,1`, `kern.osversion`, …) with `-1` for unknown keys, and
  `strtok_r` with real in-place tokenising.
- **QuartzCore.** `CATransform3DMakeRotation`/`CATransform3DScale` compute the real
  column-major 4x4 matrix with a normalised axis.
- **Coverage** moved from 130 to **156 of 438** required imports (167 service gates, 33
  runtime entries, 17 class objects). The remaining 282 are dominated by UIKit view and
  lifecycle classes, OpenGL ES entry points, EAGL, CoreGraphics image/bitmap contexts and
  AVFoundation media, none of which are stubbed.
- **Full-screen, rotation-aware launch.** `MainActivity` now declares
  `configChanges="orientation|screenSize|…"`, so turning the device resizes the ANGLE
  surface in place instead of destroying the emulator. The boot view is a full-bleed
  `SurfaceView` with floating console chrome that fades after four seconds and returns on
  tap; system bars are hidden with transient-swipe behaviour, the display keeps the screen
  awake, and both are restored when the launch stops. `onConfigurationChanged` and
  `onWindowFocusChanged` re-assert immersive mode on OEMs that restore the bars.

#### What this does not do

No title reaches gameplay, a rendered frame or an FPS number in this increment, and nothing
here is evidence that it will. A launch now proceeds past dyld and stub binding and stops at
the first *actually called* unimplemented API, which the crash screen names — that is the
honest state. UIKit views, EAGL contexts, `gl*` and CoreGraphics bitmap contexts are the next
vertical slices.

### Guest sandbox and stdio increment (2026-10-21, fifth pass)

A game that cannot open its own assets never reaches anything. The runtime previously had
**no file system at all** — `fopen` was an unimplemented import, so asset loading could only
ever be a trap. `runtime/fs.{h,cpp}` and `runtime/stdio.{h,cpp}` add a real, mounted guest
file system:

- **Explicit mounts only.** The launcher mounts the extracted `.app` at `/var/mobile`
  (read-only) and a per-game `container/` at `/var/mobile/Documents` (read-write). Nothing
  else in the host file system is reachable: unmounted paths are refused with the reason.
- **Path validation before any host call.** Guest paths must be absolute, UTF-8-safe,
  shorter than 4 KiB, and free of `.`/`..` components; mounts match on whole path
  components with longest-prefix wins, so the writable Documents mount beats the read-only
  bundle above it. A component-wise `symlink_status` walk refuses paths that resolve through
  a symbolic link, instead of letting an extractor or the user escape the sandbox.
- **Guest `FILE*` values are checked handles.** Each `fopen` allocates an 8-byte object in
  the guest address space holding a magic word and a token; the host `FILE*` stays in a
  host-side table. A forged, stale or closed `FILE*` raises `STDIO_BAD_FILE` rather than
  reaching a host pointer. `stdin`/`stdout`/`stderr` are real streams and are exported as
  the data symbols `___stdinp`, `___stdoutp`, `___stderrp`; writes to them go to the same
  log sink as `printf`.
- **Implemented stdio**: `fopen`, `fclose`, `fflush`, `fprintf`, `fread`, `fwrite`, `fseek`,
  `ftell`, `rewind`, `feof`, `ferror`, `clearerr`, `fileno`, `fgetc`/`getc`, `fputc`/`putc`,
  `fgets`, `fputs`, `setvbuf`, `remove`, `rename`, plus `getenv` over the launcher's
  environment (`HOME=/var/mobile`, `TMPDIR=/var/mobile/Documents`). `fopen` reports the
  exact refusal ("mount /var/mobile is read-only") instead of failing silently, and returns
  null exactly as the C library would.
- **Launch report** now carries a `GUEST_STORAGE:` line describing the mounts and the open
  file count, so the crash screen can distinguish "no bundle mounted" from "asset missing".
- **Boundary, stated plainly**: `.app` imports get a persistent bundle and therefore real
  file access. An `.ipa` is inspected from its archive and is **not** persistently extracted,
  so its launch has no mount and every file call is refused by name. Extracting IPAs into the
  sandbox is the next vertical slice, together with `CFBundle`/`NSBundle` path APIs so a title
  can ask where its bundle *is*.
- **Proof**: `tests/fs_test.cpp` builds a temporary bundle/container pair, then asserts:
  read-only resolution, write refusal with reason, longest-prefix container write, traversal
  and symlink refusal, `fread`/`fgets`/`fseek`/`feof` semantics against real bytes, a guest
  write that is verified on the host afterwards, formatted `fprintf` to stdout, `remove`, and
  rejection of stale `FILE*` handles. A leaked temporary tree is removed at the end.

### C++ ABI and compiler-runtime increment (2026-10-21, fourth pass)

The 472-import report links `libstdc++.6.dylib` (ordinal 16) and `libgcc_s.1.dylib`
(ordinal 17) directly, so a C++ title stops on `__Znwm`/`___divsi3` long before any
graphics work. `runtime/cxx.{h,cpp}` now implements that surface for real and reports
the rest honestly:

- **Allocator entry points**: `operator new`/`new[]` allocate from the guest heap with
  8-byte alignment and fail with a named `CXX_ALLOC` error instead of returning null;
  `operator delete`/`delete[]` release through the same heap and tolerate null.
- **Itanium C++ ABI**: `__cxa_allocate_exception`/`__cxa_free_exception` use guest
  memory, `__cxa_atexit`/`atexit` record handlers for the guest exit path,
  `__cxa_guard_acquire`/`release`/`abort` drive the real 8-byte function-local-static
  guard word (including the recursive-initialization fault), and
  `__cxa_pure_virtual`/`__cxa_bad_typeid`/`__cxa_call_unexpected`/`__cxa_rethrow`/
  `__cxa_begin_catch`/`__cxa_end_catch` raise `CXX_UNWIND`. Throwing
  (`__cxa_throw`), `std::terminate`, `__assert_rtn` and `__stack_chk_fail` each become a
  distinct guest failure. **Exception unwinding is deliberately not faked**: SjLj
  registration/resume and the two personality routines refuse with `CXX_UNWIND` until a
  real unwinder exists, because pretending to catch would corrupt the guest stack.
- **Compiler helpers from libgcc**: `__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`,
  `__divdi3`/`__udivdi3`/`__moddi3`/`__umoddi3` (64-bit results back in `r0:r1`,
  `INT64_MIN / -1` and `INT32_MIN / -1` saturate instead of trapping), and
  `__floatundidf`/`__floatdidf`/`__fixdfdi`/`__fixunsdfdi` with NaN and range clamping.
  Division by zero returns 0, matching `__aeabi_idiv0` on a device that has no host
  trap, and is reported once through the log sink.
- **Fortified libsystem_c entry points**: `__memcpy_chk`, `__memmove_chk`, `__memset_chk`,
  `__strcpy_chk`, `__strcat_chk`, `__strncpy_chk` and `__sprintf_chk` check the
  compiler-recorded object size and raise `LIBC_FORTIFY` when the request overflows it.
  They are not thin aliases: that size check is the entire point of the `_chk` variant.
- **Library attribution**: every gate is registered under the library that exports it on
  iOS — `libSystem.B`, `CoreFoundation`, `CoreGraphics`, `libstdc++.6` or `libgcc_s.1` —
  because `ExportGraph::resolve` checks two-level ordinals strictly. A helper registered
  under the wrong image would still fail to bind, which is the honest outcome; the export
  test now asserts both the positive and the negative case.
- **Proof**: `tests/cxx_test.cpp` exercises the allocator, all twelve integer helpers,
  both float helpers, guard acquisition/release/abort, exit-handler registration and its
  budget, every fortified copy (success and `LIBC_FORTIFY`), `__sprintf_chk` with its fifth
  argument taken from the guest stack, and each refusal path. The suite runs under
  ASan/UBSan in `scripts/test-host.sh` and in CI.

### ARMv6, launch acceptance and rendering increment (2026-10-21, second pass)

- **ARMv6 titles are no longer refused.** Header validation accepted only `CPU_SUBTYPE_ARM_V7`
  (9) and rejected the newer subtypes outright, so a 2009-era armv6 build never reached guest
  code. Subtypes 6, 9, 11 and 0 are accepted and reported; anything else is a named
  `MACHO_CPU_SUBTYPE` failure instead of an opaque one.
- **Instruction semantics follow the image.** `LoadedImage` now carries `cpuSubtype`/`IsaArch`,
  and the CPU adapter selects Dynarmic `v6K` for armv6 and `v7` otherwise. iOS armv6 hardware is
  ARM11, i.e. ARMv6K, which is the superset those binaries were built against; Dynarmic has no
  plain-v6 selector. The selector is passed to the translator as an architecture hint, so the
  load-time mapping is asserted in the loader tests and the CPU test asserts that an
  armv6-declared image still executes to its supervisor call.
- **PIE executables are accepted.** Mapping already happens at the un-slid vmaddrs and every
  rebase/bind is applied transactionally before the first instruction, so slide 0 is exactly what
  the fixup stream describes. The blanket `LOAD_PIE_UNSUPPORTED` rejection blocked most 2010-era
  iOS builds for no remaining reason.
- **Launcher icon** is now a green anodised-metal device mark: layered linear gradients for the
  rim, face plate, glass screen, metal play glyph and click wheel, plus a translucent specular
  band. The monochrome layer is a matching silhouette for themed icons.
- **ANGLE backend selection is explicit.** The renderer requests the Vulkan backend first and, if
  the device or driver refuses, falls back to ANGLE's OpenGL ES backend. The fallback is recorded
  with the reason and shown as "backend in use"; the `ANGLE_IDENTITY_MISMATCH` guard still rejects
  any renderer that does not identify as ANGLE.

### Repository rename is still blocked, and the "harmful app" prompt is not an app bug

The repository is now **`RadekParek/AnyiOS`** (the old `PodNative` URL redirects). The integration
token itself cannot rename repositories — `gh repo rename` returned `HTTP 403: Resource not
accessible by integration` — so this was completed from the owner's account.

The installer's "may be harmful" dialog is Play Protect / the OEM scanner judging a **sideloaded,
self-signed APK that is not distributed through Play**. It is a server-side reputation verdict, so
no app-side change can switch it off. What the project can and does control:

- the published artifact is **non-debuggable** and the CI job fails if a debuggable APK would ship;
- the published artifact **must verify with APK Signature Scheme v2**, which removes the
  "invalid or unknown signer" family of installer warnings;
- the signing certificate SHA-256 is shown in **Settings**, so the installed app can be compared
  with the fingerprint recorded at build time;
- `SHA256SUMS` is published with every artifact.

`REQUEST_INSTALL_PACKAGES` stays declared because the converter installs the APKs it generates;
removing it would break that flow. The honest installation paths are: verify the checksum and
fingerprint, then dismiss the dialog with "install anyway"; or install over `adb` from a trusted
host; or distribute through Google Play.

### Remaining runtime work

The decoder rejection, export resolution, coalescing, checked call-gate dispatch, guest heap,
C runtime, base-ABI call boundary, CoreGraphics geometry, bounded guest execution and
Objective-C core are implemented, but not all imported
APIs are. Production Emulator still blocks while required exports lack implementations, legacy/symbol stubs have no execution path, and Darwin/Objective-C/UIKit/EAGL process bootstrap is absent alongside dependency loading, re-exports/general export tries, variadic arguments and application lifecycle. **A successful APK build or linker test is not game execution or compatibility evidence.** Converter's independent experimental runtime and API implementations remain in its own pipeline; they are not silently presented as a complete implementation of the independent ANGLE emulator. Compiled asset catalogs, procedural launch scenes and complete game rendering remain unproven here. A separately installed converter's old private Android sandbox is not automatically readable; reimport its original IPAs into the new shared catalog.
