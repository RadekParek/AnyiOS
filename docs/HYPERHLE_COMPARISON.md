# Brief comparison: AnyiOS and HyperHLE-Reborn

**Snapshot: 2026-10-24.** This is a source/README comparison, not an independent compatibility or speed test. HyperHLE-Reborn is a community fork of touchHLE; its README describes a high-level iOS emulator that reimplements selected iOS frameworks on the host. Its README also lists multiple titles as tested “Playable” or “Ingame.” That is a stronger *publicly stated game-compatibility record* than AnyiOS currently has: AnyiOS still has no verified visible Angry Birds menu/gameplay result in this worktree.

| Area | HyperHLE-Reborn | AnyiOS in this branch |
|---|---|---|
| Runtime approach | Builds on touchHLE’s high-level iOS-app emulation and host-side framework implementations. | C++ ARM32 runtime using Dynarmic plus explicit iOS service/framework emulation; ANGLE remains the default graphics path. |
| IPA filesystem | Opens an IPA as a ZIP and builds a virtual app-bundle tree. A requested member is decompressed into a shared in-memory cache, which supports seekable reads without extracting the whole bundle to disk. | Indexes ZIP/ZIP64 members and materializes only opened files into a persistent private `sandbox/ZipCache`; CRC is checked. Writes go to `sandbox/BundleOverrides` with deletion markers, never back into the imported IPA. Saves live in `sandbox/Documents`, with an export action. |
| Trade-off | Its in-memory cache can avoid per-member disk writes and make repeated/seeked reads cheap, at the cost of retaining decompressed data in RAM. | Its disk cache uses storage and has first-open write latency, but it survives relaunch and avoids retaining every decompressed resource in RAM. The overlay preserves bundle writes. These trade-offs have not been benchmarked head-to-head. |
| Platform and compatibility evidence | The README emphasizes Android/mobile usability, device profiles, touch, fullscreen/orientation, and ongoing graphics/audio/framework work; it names several games it says have been tested. | This branch focuses on a measured ARM32 runtime, explicit import/error reporting, ANGLE GLES compatibility, and an inspectable per-game data layout. Actual device rendering and gameplay remain unverified. |

## Practical takeaway

HyperHLE-Reborn is presently ahead in the project’s *reported breadth of working games* and its Android/mobile compatibility experience. Its ZIP filesystem is more directly memory-backed; AnyiOS’s new version adds a persistent, write-capable overlay and an exportable save area. The small host measurements in [`PROGRESS.md`](PROGRESS.md) compare AnyiOS lazy versus full resource materialization only; they say nothing about HyperHLE’s speed or Android launch time. No HyperHLE rendering implementation was copied.

Sources: [HyperHLE-Reborn README](https://github.com/chuck6900/HyperHLE-Reborn/blob/trunk/README.md), [its IPA bundle/filesystem implementation](https://github.com/KlugKlugTG/HyperHLE-Reborn/blob/trunk/src/fs/bundle.rs), and [its filesystem nodes](https://github.com/KlugKlugTG/HyperHLE-Reborn/blob/trunk/src/fs.rs).
