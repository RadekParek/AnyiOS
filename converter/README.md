> **AnyiOS integration:** This is the owner-authorized source snapshot of RadekParek/RadekiOSconventor at e20c76f33fec87bb311711074c219c3fd32f1911, with shared-library integration changes. Android is built from the repository root; this directory is not a separate Gradle root. See [the integration record](../docs/ANYIOS_INTEGRATION.md). The standalone upstream documentation below describes its original layout. No proprietary IPA fixtures are included. Python CLI usage is from this directory, after preparing its native analyzer and optional host dependencies.

# RadekiOSConventor

An **offline IPA inspection and bounded native-reconstruction workbench** for authorized inputs. It includes an Android importer/analyzer, a C++ Mach-O parser, and Python reconstruction tools.

> **One bounded subset converts for real; everything else stays honestly unbuilt.** When an IPA's whole executable is statically proven to be exactly one closed-integer ARM entry routine (MOV-immediate, MOVK, register MOV, immediate ADD/SUB, RET) with no imports, dependencies, fixups or runtime metadata, both the host CLI and the on-device app convert it end to end into a signed, installable APK (`complete-game-v1`) — on Android this happens automatically during import, no extra button needed: the statically recompiled entry is packaged as `libconverted.so` and runs through JNI when the launcher opens, showing the message recovered from the IPA. The three-instruction `tests/data/hello-test.ipa` displays `hello test succesfull`; `tests/data/simple.ipa` adds a deterministic 128-operation ARM64 routine and also converts through the same path. Outside that proven subset nothing is statically recompiled: the host can still lower the entry routine into a standalone shared object and report compatibility registries, and on the `convert` path it packages those artifacts into a signed, explicitly labelled **experimental shell** APK (`experimental-shell-v1`) that states on screen that no game code is statically recompiled. In the Android app, **Force convert → Native runtime** builds the existing separate `game-runtime-v1` boot-attempt APK containing the original ARM executable, bundle, and guest runtime (including its shared Dynarmic dependency). A separately generated portable-C handoff can also be imported; the on-device builder revalidates the ARM64 ELF, Android dependencies, all translated-function exports, JNI entry point, executable binding, and payload hashes before packaging and selecting it at boot. That translated runner is not connected to Android EGL/GLES, so pixels and gameplay remain unverified. The compatibility guest-CPU runner has no artificial instruction-count or wall-clock cutoff and may stay alive in its render loop; both paths stop only at a real runtime boundary or setup failure. The launcher keeps a black game viewport visible while it runs and leaves diagnostics open if it stops; this is not the complete-game static recompilation contract. A source-free preview shell remains an explicit fallback. The host-only `--diagnostic-probe` flag uses a finite time window so CI can inspect an infinite-looping guest without changing the APK behavior. Device gameplay remains dependent on the implemented framework/input surface and is not claimed merely from a boot trace.

## Force convert runtime choices

- **Native runtime** is the previous diagnostic game-runtime boot attempt, not the strict static single-routine converter. Its guest runner has no Android EGL/GLES renderer and does not establish gameplay.
- **Semi-Emulated** embeds the IPA, AnyiOSEMU, SDL, ANGLE and notices in a signed one-game APK. Its private transparent launcher verifies the IPA off the UI thread without a preparation screen, starts the game automatically at the available display resolution/refresh, and requests maximum-FPS display-link callbacks by default; that experimental promotion can accelerate game logic. Emulator error dialogs are suppressed for this host; Android may still show system UI for early failures or ANRs, and quiet fatal native exits forgo system tombstones. The installed result does not depend on AnyiOS. This is emulation, not native recompilation, and compatibility and FPS require device testing. The emulator currently executes ARM32 slices only; ARM64-only and encrypted/unverified sources are rejected.

### Display profiles and compatibility issues

Semi-Emulated settings optionally select one of **480 × 320 (3:2) at 2×** or
**1024 × 768 (4:3) at 2×**. These sizes are guest points; the requested backing
buffer also depends on the layer content scale, hardware limits and game rendering
path. Texture-backed targets are not automatically enlarged. Rebuild/reinstall a
standalone APK after changing its snapshotted settings. Existing `classic43`
preferences retain their 3:2 meaning.

Converter details and exports separate completed/packaged work, available but
unlinked providers, and outstanding issues. The complete `report.json` and logs
remain accessible under **More details**, and previous analyses are retained.
Export availability and host-reported ELF checks are not independently observed
game linkage or evidence of gameplay.
ZIP imports still reject unsafe paths, links, invalid checksums, ZIP64 and bombs;
the archive-wide allowance for expansion above 250:1 is 256 MiB, with a maximum
non-ZIP64 member size of 4,294,967,294 bytes. Truly larger or more compressible
inputs can still be rejected with a member-specific reason.

### Android expansion files

A new Semi-Emulated result remains a self-contained APK. Its launcher verifies a
private IPA, then best-effort creates `Android/obb/<package>/main.1.<package>.obb`
as an identical, verified IPA ZIP mirror; execution and saves use private data,
not the mutable OBB. The Converter can share the retained IPA under the exact
OBB filename without a second converter-side copy. First launch uses additional
storage and launches verify the mirror, so startup can take longer. If OBB
storage is missing, read-only, full or restricted, the private game still runs.

The Native diagnostic builder and the narrow `complete-game-v1` builders also
attempt to produce a shareable resource ZIP OBB after validating the APK. A
failure to create the extra copy leaves the self-contained APK usable. The
bundled resources remain inside the APK, and the launchers create the app-specific
OBB directory when available but do not install or load the sidecar. Desktop
CLI output includes the sidecar alongside the APK when creation succeeds. These
optional files are not Google Play expansion delivery and do not shrink the
APK. Android's picker may prohibit `Android/obb`; no bypass is attempted.
Existing conversions remain installable without an OBB.

### Optional host NDK/CMake relinking

From the `converter` directory, install the host Android NDK (for example NDK
27.2.12479018), CMake 3.22+ and the optional Capstone package used for ARM
translation. Set `ANDROID_NDK_HOME` to the installed NDK. The ordinary host
analysis keeps using direct NDK clang unless `--android-linker cmake` is selected:

```sh
python3 -m radek analyze /path/to/authorized.ipa --authorized --output workspace/analysis-result --android-linker cmake
# If a portable-C translation was generated, repeat the host link without re-analysis:
python3 -m radek relink workspace/analysis-result/bytecode-translation --authorized --backend cmake --target-abi arm64-v8a
```

Only relink translations generated by this tool that you trust; CMake and the
compiler process source and build-cache files on your computer. The report
records failures and ELF verification; relink exits successfully only
when an ARM64 `translated-game-runtime-input.zip` was prepared for import. ARMv7
can be verified as a standalone ELF, but its handoff is not accepted by the
current ARM64-only Android importer. Copy the host-generated ZIP into Converter
and select **More details → Import translated runtime handoff**. Before packaging,
the device checks hashes, report consistency and the ELF header; symbol/dependency
inspection is host-reported, not independently repeated on the device. This workflow relinks *generated C*, not
arbitrary Mach-O or proprietary frameworks, and does not provide an EGL/GLES
renderer or establish gameplay.

## Offline reconstruction

Authorized imports are analyzed before compatibility is assessed:

- Mach-O images (main executable and embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv6/ARMv7/Thumb/Thumb-2),
  register/constant/reference tracking and reconstructed pseudocode listings.
- Objective-C classes, categories, protocols, ivars, properties, selectors and message-send
  targets; Swift type/field metadata and symbol demangling.
- Static API-call attribution: unique direct import callsites ranked across recovered functions,
  plus a separate direct-call graph rooted at the selected entry. Indirect/dyld transfers,
  Objective-C message dispatch, callbacks and loader initializers are not guessed; rankings are
  limited to the decoded fraction of `__text` and are never treated as runtime execution evidence.

Results are written as `reconstruction.json` and `reconstruction.md` beside `report.json`. Every
run also records a **conversion ceiling** (`report.json` → `conversionCeiling`, plus a
“Conversion ceiling” section in `reconstruction.md`): an ordered gate ledger over target ABI,
protection, container, slice format, loader metadata, the entry-routine proof, reachable-code
static recompilation, API linking, resources, lifecycle and APK packaging. The first gate that cannot pass
is named with the fail-closed prover's own statement as evidence, and every later gate is
`NOT_REACHED` — never estimated. For a real game this typically reports
`ENTRY_ROUTINE`/`API_LINKING` with the concrete first obstacle (for example an instruction outside
the proven subset, or *import symbols declared and not linked*), which is the honest answer to
“how far can this be converted”. The ledger carries `countsAsConversionProgress: false`: it is an
assessment, not statically recompiled code, and it never counts symbol triage or stub handlers as progress.
Analysis also writes an `ioscompat/` directory containing the generated compatibility-registry
source (`libioscompat.cpp`), copied compatibility headers and a `registry.json` classification of
every observed Darwin import as `verified` (tested implementation) or `stubbed-unimplemented`
(explicit resolution handler). Stubs record invocations and return a documented safe default;
they are resolution targets for a future linker, not API implementations, and the report counts
them separately from verified shims. `compatRegistry.symbolResolution` states this split
explicitly (verified / stubbed-unimplemented / unresolved, `linkedIntoGame: 0`,
`resolutionIsNotImplementation: true`), so a 100% resolution figure is never read as 100%
implementation, static recompilation or gameplay coverage.
When (and only when) the entry routine passes the closed-integer proof, the host writes:

- `recompiled-entry.bin`: lowered ARM64/ARMv7 function bytes;
- `librecompiled-entry.so`: a minimal, relocation-free Android ET_DYN library exporting
  `radek_recompiled_entry`, statically checked for ABI/class, zero `DT_NEEDED` dependencies and
  undefined symbols, and the export's type/size/hash;
- `recompiled-entry.c`: a portable C rendering of the same proof-carrying integer operations,
  executable in host tests to compare return-value semantics;
- `leaf-experiment.ll`: supplementary textual LLVM IR for the same proven leaf.

On the `convert` path only, when those artifacts exist, the host additionally assembles,
zipaligns and signs `experimental-shell.apk` using the Android toolchain (aapt2/javac/d8/
zipalign/apksigner) under the `experimental-shell-v1` contract. The shell launcher displays the
disclosure that no game code is statically recompiled, and its metadata repeats it; the validator rejects a
shell that drops the disclosure or claims game code. The shell never satisfies
`complete-game-v1`, and its build status is reported separately from `conversionProgress`, which
stays 0 / `NOT_BUILT` unless the IPA passed the bounded complete-conversion gate and a signed
`complete-game-v1` APK was actually built and statically validated.

These files contain one isolated function only. The shared object has no game JNI entry, game
resources, lifecycle, or callsites into the original game. `portProgress.percent` is nonzero only
after a standalone Android ELF or verified APK contains the statically recompiled code; it measures
source instruction bytes represented by that Android artifact, not whole-app or playability coverage.
A standalone `.so` is not linked into the game boot path or packaged as a game APK. The report's
`androidLink` field records the target architecture/dependencies and exported-function verification
behind that figure.

For an input the bounded prover refuses, the host CLI also runs a bounded **static-recompilation
plan** (`radek/plan.py`, needs optional `capstone`): it lifts discovered functions with the same
fail-closed emitter the differential test proves. Its host-only coverage is reported separately in
`staticRecompilationPlan` and `hostStaticRecompilationProgress`; it never increments `portProgress`.
Whole-game portable-C coverage is reported as unique emitted instruction bytes in
`bytecodeTranslation`, separately from the host plan. When an Android NDK is available, the host may
link a standalone translation library; Android-linked progress becomes positive only when the
artifact exists and its ABI/ELF class, dynamic-dependency policy, all translated-function exports,
JNI entry point, and linker-to-translation function/byte counts verify. The host can then prepare a
hash-bound `translated-game-runtime-input.zip`; this is still only input for the Android builder,
not an APK or game-runtime link. After the app revalidates it and successfully packages the library
and payload into `game-runtime-v1`, the report marks translated code as linked into that boot path.
That runner is not connected to EGL/GLES; APK inclusion/linkage is not execution, pixel, gameplay, or
playability evidence. `conversionProgress` remains `NOT_BUILT`. Reconstruction is an engineering
artifact, not original source; uncertain instructions and control flow are marked. The generated
portable-C Android library is not executed by the host; any bounded guest-boot probe is a separate
diagnostic path.
Objective-C `__objc_msgrefs` selector references are followed through their `__objc_selrefs`
pointer slots when statically readable.

## APK output policy

- CI builds **only the RadekiOSConventor importer/analyzer APK** (`RadekiOSConventor-debug.apk`),
  plus the hello-test and longer simple-test bounded conversions used to exercise the complete-game pipeline.
- Importing an IPA runs analysis and — when the executable passes the bounded conversion proof —
  automatically finishes the conversion into a signed, installable APK; no separate action is
  needed for proven inputs. Everything outside the proven subset creates nothing on import; the red
  **Force convert to game APK** action builds a separately named, signed boot-attempt APK. It
  packages the selected ARM executable and bundle together with `libcompat_runtime_v1.so` and its
  required `statically linked dynarmic` dependency, then keeps a diagnostic log open when guest execution stops
  at an unimplemented call. This is not a playable conversion and does not count as static
  recompilation or complete-game progress. A source-free preview shell remains an explicit fallback
  and contains no iOS executable or game code.
- A host APK can be attached only if its metadata declares the `complete-game-v1` contract and
  passes source-identity, complete reachable-code/API/resource, ABI, packaging and provenance
  checks. Preview-shell APK metadata and provider paths are separate; a preview shell can never
  satisfy the host APK contract. The host CLI **is** a producer for that contract for the proven
  bounded subset only (see `radek/gamepack.py`); IPAs outside the subset stay `NOT_BUILT`.
- The `experimental-shell-v1` APK is a third, distinct category: a signed, honestly labelled
  inspection shell for the isolated statically recompiled artifacts and compatibility-registry source. It is
  produced only on the `convert` path, is validated with `python3 -m radek validate-shell`, and can
  never be attached as a complete-game host APK.
- The original IPA archive itself is retained only in private analysis storage until the library
  entry is deleted and is never embedded whole into an APK. Bounded conversions package the
  bundle's static resource files verbatim under `assets/bundle/` with a hashed inventory. The
  game-runtime boot attempt packages the selected main executable and bundle resources, plus the
  ARM64 guest runtime and its shared dependencies; a source-free preview shell copies only the app
  name and a recovered icon. If no original icon is available, a generated/fallback icon is used
  and reported accurately.

The host's ARM assessment prefers `arm64-v8a` when an IPA contains both ARM32 and ARM64. A
supported ARM32-only input is assessed for `armeabi-v7a`. These ABI choices describe analysis and
future conversion targeting; they do not imply that an APK was generated.

## Android API status

The Android mapper reports same-named NDK symbols and semantic rewrite targets (for example,
`UIView` → `android.view.View`) as **candidates only**. Two coverage figures are reported separately
and are never interchangeable. *Reviewed Android mapping coverage* counts every observed import the
classifier assigned exactly one reviewed mapping kind to (same-name NDK/system export, compiler-runtime
candidate, `libcompat_runtime_v1.so` guest-adapter catalog entry, compiled `libioscompat.so`
implementation export, or reviewed semantic target) and therefore reaches 100% for a fully triaged
IPA. The *same-name NDK candidate subset* is the strict count of symbols whose exact name exists in
the reviewed Android catalog; it is divided by all distinct imports and excludes Darwin spellings
and guest-runtime adapters. The provider-catalog count is another, separate measure: in the checked-in
Angry Birds v1.0 fixture, `181/254` are strict same-name candidates and `73/254` have non-same-name
guest-runtime adapter catalog entries. Those 73 are not static NDK links, per-image fixup results, or
proof of full API semantics. The eight compiler-runtime/unwind candidates are reported on their own
axis, and all eight happen to have guest-adapter catalog entries in this fixture; that is not a static
NDK compiler-rt/libunwind link or proof those imports were bound for a loaded image. At runtime, the
game report's `runtimeLinking` block records actual import-slot bind/relocation
results. Device `dlsym` export verification is shown both as a fraction of same-name candidates and
of all imports (167/264 is 63%, not 65%); it is evidence for the current device/API only. A name
resolving at runtime does not prove Darwin/Android ABI compatibility or link the imported code.
Dependency grades are deliberately distinct: `provided` means a reviewed Android system ABI is a
possible target; `compatibility` is reserved for bounded, tested implementation subsets;
`candidate` means only a semantic/API target exists; `no-execution-path-yet` means no Android
provider or adapter is identified. Each dependency edge reports imports associated by Mach-O dylib
ordinal, with association completeness and separate counts for device-verified exports,
host-tested implementation bodies, stub-only handlers, and imports with none of those facts. These
counts always keep callsites linked, runtime calls observed, and recompiled bytes linked at zero.
Foundation, UIKit, CoreGraphics, audio/media, networking, and other framework mappings without ABI
adapters are candidates only. CoreFoundation is a limited C subset; QuartzCore has only the C time /
frame-callback slice, not Apple Objective-C classes or game integration.

`libioscompat.so` contains host-tested
C/time/POSIX compatibility functions and a limited CoreFoundation C object/collection/run-loop
subset, alongside the four Darwin time APIs (`_CFAbsoluteTimeGetCurrent`, `_CACurrentMediaTime`,
`_mach_absolute_time`, and `_mach_timebase_info`). Host source generation selects implementations
only when reconstructed call paths reach supported imports. `dlopen`/`dlsym` checks can verify
runtime exports, but do not rewrite IPA callsites or prove the imported caller ABI. The bounded
on-device APK packages `libioscompat.so` through `DT_NEEDED`; its accepted zero-import executable
still claims no per-callsite API replacement.

A native C frame-callback service is driven by Android `Choreographer` in the bounded launcher's
lifecycle. It is not the Objective-C `CADisplayLink` class/selector ABI, and no game callsite uses
it yet. The `libgcc_s.1.dylib` mapping is not a drop-in `libgcc_s.so` alias: some ARM32 arithmetic
helpers and unwind boundaries have guest-runtime adapters, while static NDK compiler-rt/libunwind
linking and full SjLj/personality/landing-pad semantics remain separate and unclaimed. GNU
`libstdc++.6.dylib` is also only a candidate: LLVM
`libc++_shared.so` is not a drop-in GNU C++ ABI substitute, and low-level symbol overlap is not
proof of compatible C++ objects, exceptions, or linkage.

`libioscompat.so` also carries a dynamic symbol-resolution registry: individually verified
implementation entries plus a pool of stub trampolines. Symbols that would otherwise stay unmapped
can be registered at runtime (`radek_compat_register_stub` / `NativeBridge.compatRegisterStub`) and
then resolve to an explicit stub handler instead of nothing. The on-device triage reports those as
`compat stub handler(s) registered (unimplemented)` — a resolution category that is never counted
toward verified implementations or generated API implementations. The host likewise generates a per-IPA
registry source in which every observed import is classified exactly as `verified` or
`stubbed-unimplemented`. Symbol-triage percentages describe categorization, not static recompilation
coverage. Foundation has no Foundation/Objective-C ABI adapter; UIKit, CoreGraphics, audio, input,
game lifecycle and general resource APIs remain candidate targets or unsupported when required.
CoreFoundation support is only the tested C object/collection/run-loop subset above; the full
CoreFoundation ABI remains unsupported.

## Build and test

See [Build instructions](docs/BUILD.md), [support matrix](docs/SUPPORT.md),
[architecture](docs/ARCHITECTURE.md), and [security](docs/SECURITY.md).

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

Java 17, Python 3.10+, C++17, Android platform 35, build-tools 35.0.0, NDK 27.2.12479018,
CMake 3.22.1; the Gradle wrapper is included. No Python packages are required for the
analyzer, tests or conversions; the optional `capstone==5.0.7` package
(`pip install 'capstone==5.0.7'`) enables the host static-recompilation plan (`staticRecompilationPlan`) and the game-path
disassembly tests, which otherwise report `UNAVAILABLE` or skip.

A small synthetic sample IPA (`tests/data/sample-leaf.ipa`, regenerable with
`python3 tools/make_sample_ipa.py`) is committed for end-to-end checks: its entry routine lies
entirely inside the proven subset, so analysis reaches PARTIAL with nonzero statically recompiled-byte
coverage and a complete compatibility registry. Run it through both paths:

```sh
python3 -m radek analyze tests/data/sample-leaf.ipa --authorized --output .local/analysis
python3 -m radek convert tests/data/sample-leaf.ipa --authorized --output .local/conversion
# .local/conversion holds librecompiled-entry.so, ioscompat/ registry source, and — when an
# Android toolchain is installed — experimental-shell.apk. State is BLOCKED: no game APK.
```

A longer end-to-end regression fixture is also committed. It contains a deterministic 128-operation
ARM64 entry (over eighty times the hello fixture's code bytes), with no imports or unresolved
runtime dependencies, and is inside the same narrow static recompilation subset:

```sh
python3 tools/make_simple_ipa.py  # deterministically regenerates tests/data/simple.ipa
python3 -m radek convert tests/data/simple.ipa --authorized --output .local/simple-conversion
# With an Android SDK/NDK toolchain, this reaches READY and writes simple.apk.
```

For other synthetic Mach-O inputs, `tools/make_fixture.py` produces variants:

```sh
python3 tools/make_fixture.py --arch arm64 --output .local/fixture.ipa
python3 -m radek analyze .local/fixture.ipa --authorized --output .local/analysis
# Inspect .local/analysis/librecompiled-entry.so and report.json.
# No game APK is produced.
```

To inspect an authorized IPA:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

### Optional local Ghidra analysis (desktop CLI)

The existing offline reconstruction (Mach-O metadata, disassembly and call/CFG
analysis) runs without Ghidra. On Linux/macOS, you may additionally point the
Python CLI at a **locally installed** Ghidra distribution and its compatible JDK:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis \
  --ghidra-home /path/to/ghidra
```

This opt-in runs `support/analyzeHeadless` and the bundled `AnyiOSDecompile.java`
post-script locally. It never uploads or executes the IPA, and it skips encrypted
Mach-O images rather than decrypting them. The job's `report.json` records
availability, errors, timeout/truncation and `ghidra-inferred.c` if produced.
That file is a **bounded pseudocode sample**, not original source or a runnable
conversion; it does not change the converter's validation gates. Limits include
128 MiB input, 32 functions, 3 seconds per function, a 90-second auto-analysis
budget, 180-second wall timeout and 4 MiB maximum report. The Ghidra launcher
is asked to use at most 2 analysis CPUs and a 1 GiB Java heap; these are Ghidra
settings, not OS-wide limits on all native processes or disk usage. The temporary
project is removed, but the selected Ghidra installation may retain its own
local settings/logs. Treat the job output as private game material.

Ghidra is not bundled with the Android converter; stock desktop Ghidra is not
an Android library. Windows and Android runs report the opt-in as unavailable.
The regular Android importer continues its on-device Mach-O/dependency analysis.

A new output directory is required. Reports and logs persist; temporary extraction workspaces are
removed. `analyze` returns `PARTIAL` after a successful inspection and may write the isolated code
artifacts above. On `convert`, inputs outside the bounded complete-game subset remain `BLOCKED` and
may produce only a separately labelled experimental shell. Inputs inside the subset proceed through
native linking, launcher/APK packaging and static validation: with an Android toolchain they reach
`READY` and emit a signed `complete-game-v1` APK; without one they remain blocked with an explicit
missing-toolchain report. Device execution and gameplay are not claimed as tested.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow** and select the branch containing
this implementation. Pushes and pull requests also run CI. After a successful run, download the
**RadekiOSConventor-debug.apk** importer artifact. CI runs native, Python and Android importer tests,
validates the importer APK, converts `tests/data/hello-test.ipa` end to end with the host CLI, and
validates the resulting bounded complete-game APK (`complete-game-v1`) alongside the honestly
blocked sample leaf.

Only inspect or convert IPAs you own or are authorized to process. Encrypted/FairPlay-protected
images are blocked; no protection bypass is provided.
