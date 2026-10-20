# From startup validation to game execution

The supplied report establishes a universal ARMv7 executable and a real loader
blocker, not a runtime trace or compatibility result. Its metadata names a
particular Ninjago app/version; do not assume it is the originally proposed title.
The report itself and proprietary files are not checked into this repository.

## This increment

- Remove fixed import-byte/library-entry quotas without removing bounds against
  hostile metadata, native allocation, cyclic trees or unsafe paths.
- Validate LC_DYSYMTAB instead of ignoring it or reporting it as unknown.
- Provide an enabled, honest startup attempt: reload bytes from private storage,
  run native mapping validation off the UI thread, preserve the first failure and
  keep the imported game intact. No call into unresolved guest addresses.

## Still required for playable games

1. Complete bounded Mach-O/dyld metadata handling, then implement/test rebasing,
   binding, lazy/weak imports and relocation semantics. Accepting a load command
   is not implementing its runtime effect. Missing required services must stop.
2. Integrate the pinned, already host-tested CPU engine into an Android app-UID
   test harness; prove W^X, exception handling and ARM/Thumb transitions there.
   An ADB-shell binary or successful cross-build is insufficient evidence.
3. Define guest Darwin ABI, allocator/libc/thread/TLS/filesystem services and
   bundle-relative resource lookup. Keep imported resources private/read-only
   until an explicit guest writable-data model exists.
4. Implement Objective-C dispatch/lifetime and the actual Foundation/UIKit
   bootstrap path needed by observed execution, not hundreds of successful no-ops.
5. Implement guest EAGL/Core Animation and graphics state/resource semantics over
   the existing genuine embedded ANGLE backend. Static GLES/compressed-texture
   imports do not prove actual texture formats or render paths.
6. Add correct input/audio/timing/lifecycle semantics and device trace evidence.
   Optional camera/media imports do not justify fabricated required-service success.

Each stage needs synthetic regression fixtures and physical-device validation.
Current APK startup attempts deliberately stop before CPU execution; no gameplay,
FPS, framework compatibility or successful launch is claimed.
