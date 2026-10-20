# Dyld and retained-import increment (0.4)

The supplied device report demonstrates ARM32 mapping (4 regions, 4,399,104 guest
bytes) and a dyld startup blocker. It does **not** demonstrate game CPU execution.
The uploaded report and proprietary assets are not committed here.

## Official source review

Format references reviewed:
- Apple's dyld repository at `fd8d0c4d52320ebf64db34f3cb280310d905c5ae`:
  https://github.com/apple-oss-distributions/dyld/tree/fd8d0c4d52320ebf64db34f3cb280310d905c5ae
  (`mach_o/BindOpcodes.cpp`, `RebaseOpcodes.cpp`, and APPLE_LICENSE).
- Official Mach-O loader format constants in XNU at
  `f6217f891ac0bb64f3d375211650a4c1ff8ca1ea`:
  https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/EXTERNAL_HEADERS/mach-o/loader.h

Apple's referenced implementations carry APSL 2.0 notices and depend on Darwin.
No Apple implementation or dylib was copied or linked. PodNative's new decoder is
original code implementing the format, not a drop-in port of Apple's dyld.

## Actual linker progress

- Bounded ARM32 pointer rebase and regular/weak/lazy bind opcode decoding, checked
  ULEB/SLEB, ordinals/addends, segment offsets, repeat/work/name-allocation budgets.
- Legacy LC_DYSYMTAB non-lazy/lazy pointer sections now produce symbol/ordinal
  fixups from the indirect table and bounded nlist/string records, even without
  LC_DYLD_INFO. Local/absolute slots are distinguished; section ranges, ordinals
  and duplicate/conflicting stream bindings are checked. ARM stub counts are
  reported but stub execution is not initialized.
- Loader now parses LC_DYLD_INFO streams and includes target symbol/ordinal/lazy
  information in its report (up to 120 displayed binding entries). Static import
  display expands from 60 to 1024 names per category, allowing this game's 539
  names to be reported within the existing overall report-size bound.
- A tested fixup applier resolves through an explicit callback and validates all
  targets/values before writing. Required unresolved symbols leave memory unchanged;
  missing weak imports may become null. Weak-definition coalescing plans explicitly
  refuse application until dependency-graph semantics exist. Segment addresses passed to the decoder are
  mapped addresses; the applier's slide adjusts pointer contents only.
- **Production startup does not apply this plan or enter guest code.** Guest service
  trampolines, framework exports, dependency loading, legacy stub execution,
  export tries, weak coalescing, threaded/chained fixups, text fixups, PIE and Darwin
  process/Objective-C/UIKit/EAGL bootstrapping remain unfinished. A decoded plan is
  not a resolved process. Unsupported/malformed streams fail explicitly.

## Imports and artwork

After a safe, complete private copy, inspection failures are retained as Invalid
or Unsupported entries. ARM64 entries preserve their available metadata/artwork
and explain why this ARM32 runtime cannot execute them. Missing executables and
corrupt IPAs retain their original private source. Clicking the entry shows the
reason; reports include it. Unsafe/provider/copy/low-storage failures still abort
rather than publishing an incomplete or unsafe tree.

Existing entries can use **Refresh details & artwork** to update metadata/diagnostics
and retry covers without recopying the IPA/folder or losing custom titles. Preparing
a launch also retries its cover. This avoids reimporting a multi-GiB game merely to
repair an old missing icon.

Artwork discovery happens before executable validation, so a blocked binary no
longer suppresses available loose launch images/icons. Legacy iOS CgBI PNG artwork
gets original bounded raw-deflate/BGRA/unpremultiplication/filter conversion for
noninterlaced RGB8/RGBA8 images. Up to eight bounded candidates may be read,
but only three successfully decoded images are presented; a corrupt candidate
does not suppress later candidates. Unsupported formats, compiled asset catalogs and images
not stored as loose launch PNGs remain unsupported; no splash content is invented.
ARMv6 alone is not a reason to suppress artwork. Existing max-three / 900 ms
ANGLE presentation remains. No on-device rendering success is claimed here.

## Visual changes

Green/mint palette and gradients replace violet, with existing animation-disable
aware card/press effects. A generic pocket-player screen/click-wheel mark replaces
the handheld-gamepad mark; it contains no Apple logo or proprietary game artwork.

## Evidence / remaining gate

Native sanitizer tests exercise real synthetic binding/rebase application,
transactional failures, lazy reset, weak null and 10,000 dyld mutations. JVM tests
cover retained ARM64/malformed imports, metadata/artwork preservation and CgBI
conversion. Android build/lint/JVM and CPU/PC CI must pass before APK delivery.
Physical-device CPU execution, artwork rendering and Ninjago gameplay require
separate evidence. This increment is not a complete implementation of all 539
symbols / 20 libraries from the report and does not claim game compatibility.
