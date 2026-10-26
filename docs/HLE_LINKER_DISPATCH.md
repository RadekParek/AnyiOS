# Historical C++ HLE linker / guest-call prototype

> **Historical only.** The C++ guest framework and call-gate path below belonged to the
> previous AnyiOS game-runtime prototype. The current Android game engine is the pinned
> Rust/SDL guest runtime presented as AnyiOSEMU; these C++ coverage counts and host results
> are not current-engine compatibility evidence. See [`RUNTIME.md`](RUNTIME.md) and
> [`ANYIOS_INTEGRATION.md`](ANYIOS_INTEGRATION.md).

## Original HLE linker / guest-call increment

2026-10-21. Target the latest private Spinjitzu Scavenger Hunt report: 472 static imports, 1,235 binding sites, weak coalescing and legacy dispatch blockers. Do not commit the report or game files.

Implement an original, session-owned export graph (two-level ordinals, explicit aliases, deterministic weak selection), graph-aware transactional fixups, and checked ARM SVC call gates for genuinely implemented APIs. Bind the existing CF bit-vector implementation and bounded C memory/string operations. Audit production launch against the actual registry instead of claiming every import is supported. Exercise ARM and Thumb calls through the pinned CPU in CI, including forged traps and invalid return addresses.

An earlier upstream-runtime candidate at `5b65054c55cc33c6095f8c4b771430da5e63fbf4` was reviewed for architecture. It is not the selected engine pin. At the time of this historical prototype, no implementation or renderer was copied; the later AnyiOSEMU integration deliberately replaced this Android game path with a separately pinned Rust/SDL guest runtime. Official Apple format references from the earlier dyld increment still apply.

Second increment (same day): guest heap and the C runtime subset described in
`DYLD_AND_IMPORT_INCREMENT.md`. Coverage moved from 9 to 72 of 438 required imports.

Third and fourth increments (same day): varargs formatting, the Objective-C collections
and dispatch layer, then the C++ ABI and compiler-runtime layer in `runtime/cxx.cpp`
(libstdc++.6 and libgcc_s.1, both bound under their real image names). Coverage is now
**156 of 438** required imports: 128 service names, 10 Objective-C runtime entries and 15
class/metaclass entries. Guest file I/O and stdio are implemented over explicit sandbox
mounts (`runtime/fs.cpp`, `runtime/stdio.cpp`); classic `S_SYMBOL_STUBS` sections are now
decoded and redirected to their resolved import (or a named trap) before execution, duplicate
binding-stream entries with the same target are merged, and the `objc_msgSend` family,
Foundation bundle paths, AudioSession state, `CATransform3D` and the mach time/sysctl helpers
are implemented. `qsort`, UIKit lifecycle, EAGL rendering,
audio and exception unwinding deliberately remain unimplemented and are refused by name.

This is not implementation of every UIKit, Objective-C, C++ exception, EAGL, audio or networking API. Guest dependency loading, export tries/re-exports, class bootstrap, callbacks, variadic/hard-float ABI and application lifecycle remain separate requirements. Never bind missing APIs to success-returning placeholders. No game compatibility claim without actual device evidence.

Sixth increment (same day): the two on-device blockers were fixed in the decoder itself.
Stream cursor strides no longer raise `DYLD_ADDRESS` (only real 64-bit overflow does), an
entry repeated across the lazy and plain binding streams is merged when the target matches,
and the 350 undispatched legacy stubs are now bound by rewriting each stub to
`ldr r12, [pc] / bx r12 / .word target`.

Fifth increment (2026-10-22): the dyld cursor is now unconditional uint64 wrap-around
arithmetic, matching Apple's decoder, with the lifetime check kept in the fixup emitter; a
rebase and a bind on the same slot merge with the bind winning; streams no longer need a
trailing `DONE`. Four-byte `ldr pc, [pc, #imm]` stubs are dispatched through the pointer slot
they name (the instruction is validated first) instead of being rejected as unpatchable.
`LoadedImage::symbols` keeps the image's `N_SECT` functions for stop-report symbolication, so
`GUEST_BLOCKER` carries `symbol=<nearest function>` and the crash screen names the guest
function rather than only its PC.
