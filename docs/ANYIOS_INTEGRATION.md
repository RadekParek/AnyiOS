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
- Architecture research of HyperHLE-Reborn (MPL-2.0) covered its general host-dylib export/interface separation only. No source or renderer was copied; the independent ANGLE renderer is unchanged.

### Remaining runtime work

This increment fixes the decoder rejection class and establishes export resolution, coalescing and checked call-gate dispatch, but does not implement all imported APIs. Production Emulator still blocks while required exports lack implementations, legacy/symbol stubs have no execution path, and Darwin/Objective-C/UIKit/EAGL process bootstrap is absent alongside dependency loading, re-exports/general export tries, variadic/hard-float ABI details and application lifecycle. **A successful APK build or linker test is not game execution or compatibility evidence.** Converter's independent experimental runtime and API implementations remain in its own pipeline; they are not silently presented as a complete implementation of the independent ANGLE emulator. Compiled asset catalogs, procedural launch scenes and complete game rendering remain unproven here. A separately installed converter's old private Android sandbox is not automatically readable; reimport its original IPAs into the new shared catalog.
