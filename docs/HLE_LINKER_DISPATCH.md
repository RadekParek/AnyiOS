# HLE linker / guest-call increment

2026-10-21. Target the latest private Spinjitzu Scavenger Hunt report: 472 static imports, 1,235 binding sites, weak coalescing and legacy dispatch blockers. Do not commit the report or game files.

Implement an original, session-owned export graph (two-level ordinals, explicit aliases, deterministic weak selection), graph-aware transactional fixups, and checked ARM SVC call gates for genuinely implemented APIs. Bind the existing CF bit-vector implementation and bounded C memory/string operations. Audit production launch against the actual registry instead of claiming every import is supported. Exercise ARM and Thumb calls through the pinned CPU in CI, including forged traps and invalid return addresses.

HyperHLE-Reborn 5b65054c55cc33c6095f8c4b771430da5e63fbf4: MPL-2.0; reviewed src/dyld.rs module documentation and HostDylib interface as architectural research only. Its general separation of library names/aliases and function/data/class exports is useful. No implementation or renderer copied, linked or vendored. Keep independent ANGLE renderer unchanged. Official Apple format references from the earlier dyld increment still apply.

This is not implementation of every UIKit, Objective-C, C++ exception, EAGL, audio or networking API. Guest dependency loading, export tries/re-exports, class bootstrap, callbacks, variadic/hard-float ABI and application lifecycle remain separate requirements. Never bind missing APIs to success-returning placeholders. No game compatibility claim without actual device evidence.
