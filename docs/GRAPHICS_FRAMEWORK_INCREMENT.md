# Graphics/framework increment (0.3)

## Actual changes, not gameplay claims

- A single **Android and PC builds** workflow invokes the Android APK build plus
  the reusable runtime pipeline. It publishes a signed Android APK, Linux x86-64
  CPU/W^X diagnostic executables, and Android ARM64 CPU diagnostic executables.
  The Linux artifact is **not a desktop emulator GUI or a playable game build**.
- Original handheld/screen/control launcher icon replaces the letter P. The native
  UI uses violet/teal accents, staggered card entrances and press feedback.
  Android's animation-disable setting is respected. New visuals need device review.
- Game launch artwork is read from loose PNG files named by UILaunchImageFile /
  UILaunchImages or conservative Default/LaunchImage/Splash filename fallbacks.
  Density variants are grouped. Up to three decoded images are normalized privately,
  uploaded as RGBA textures to the existing real embedded ANGLE/Vulkan context and
  presented as aspect-fit quads, in sequence, 900 ms per image after its first swap.
  Then the bounded startup attempt runs. Missing/unsupported artwork is not faked.
- This is **host presentation of actual imported artwork, not guest-rendered frames**.
  A game-created second/third splash drawn procedurally cannot be discovered without
  execution. Compiled asset catalogs and CgBI PNGs remain unsupported. Filenames do
  not reliably distinguish all orientation variants from separate splash content.
- Cancel/background destroys the surface on its owner thread and invalidates the
  completion callback. Artwork is not auto-resumed as the demo cube, and cancellation
  must not kick off startup. No native-GLES fallback is introduced on graphics failure.
- Real CoreFoundation service implementations for immutable bit-vector creation,
  mutable copy, bit access/mutation and retain/release use checked guest memory,
  session-owned opaque handles, MSB-first bit numbering and allocation limits.
  Unknown functions return unresolved; invalid handles/mutations raise errors.
- CoreGraphics affine point/rect transformation, concatenation, inversion, rotation,
  translation and scale have original mathematical implementations and tests.
  Aggregate ARM ABI marshalling for these CG functions is not implemented.

## In-app CPU execution diagnostic

The APK now source-builds and links the exact reviewed Dynarmic pin with the W^X
allocator patch, rather than leaving the engine only in a separate test binary.
**Diagnostics → Run ARM/Thumb CPU self-test** executes two short original fixed
programs inside the app UID, checking actual register and SVC results. This is a
bounded native execution diagnostic, not game execution. Native faults may still
terminate a diagnostic build; successful compilation is not app-UID execution proof.
The same probe is asserted by the host CPU regression suite and cross-compiled for
Android. Engine/dependency license texts and revision are shipped in CPU notices.

No self-test has been run on a physical device here. A phone report is required to
establish Android app-UID behavior. Game startup still stops at missing dyld/guest
ABI/framework services and does not enter an unresolved executable.

## What is still not implemented

The exported game's 20 library dependencies and 539 imports are not satisfied by
these two subsets. No dummy dylibs/no-op framework exports have been installed.
The CF dispatcher is tested with guest register/memory objects but **not wired into
an executing game session**. Dyld binding, CPU-backed game sessions, Objective-C/UIKit bootstrap,
EAGL/guest GLES state/resource semantics, game input/audio and actual gameplay remain
unfinished. The cube and imported-artwork presentation are real ANGLE draws, not
proof that guest glDraw calls run. See EXECUTION_NEXT.md for the dependency order.

## Tests and device checks

Host sanitizer tests cover CF object lifetime/copy isolation/invalid access and CG
composition/inverse/rotated bounds. JVM tests cover launch-image selection, unsafe
paths, density deduplication and presentation constants. CI builds/lints/tests the
Android app and executes the PC CPU diagnostics; Android cross-build is not device
execution. No screenshot, device import/UI test or successful game launch is claimed.

On the device, select an existing imported game and tap Attempt launch. Verify at
most three real images, approximately 0.9 seconds each, correct aspect/orientation,
backend logs, then a saved blocker report. Test Back, pause, lock and rotation on
all three images; returning must not unexpectedly run startup or the cube. Report
missing/corrupt-artwork behavior without sharing proprietary game assets.
