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
