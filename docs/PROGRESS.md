# Continuation progress — 2026-10-24

## IPA archive filesystem, Objective-C fixup ordering, and game data — host-tested; device validation pending

### Changes in this follow-up

* **IPA launches no longer extract every app-bundle file.** The launcher indexes ZIP/ZIP64 central-directory entries, reads stored/DEFLATE members on demand, CRC-checks each newly materialized member, and keeps that disk cache under `game-<UUID>/sandbox/ZipCache`. Bundle writes go to `BundleOverrides`; deletes use persistent whiteouts, and rename/remove/create behavior is routed through the guest stdio filesystem. The imported IPA is never modified. Extracted `.app` folder imports keep their copied bundle-directory behavior.
* **Objective-C metadata now waits for dyld.** The loader previously read `__objc_classlist` pointer slots before fixups, and class registration could consume pre-fixup `class_data_bits_t`, fall back to the legacy layout, and treat a neighboring pointer word as `instanceSize`. It now records class-list section ranges, applies dyld first, then reads class pointers and registers classes. A regression rebases both the class-list entry and class-data pointer; another reproduces the pre-fixup `AppController` garbage-size error and verifies it disappears after fixups. Both then allocate an `AppController` with the valid 210-byte instance size. Bounds remain enforced; allocation rounds the byte size to the required alignment. This addresses a concrete source of varying garbage sizes, but no fresh device trace has yet confirmed that it was the exact Angry Birds failure.
* **Persistent per-game data is organized and exportable.** `sandbox/Documents` stores saves, `sandbox/ZipCache` is rebuildable, and `sandbox/BundleOverrides` stores IPA app-resource writes. Existing `container/` data is migrated without overwriting conflicting saves. Game details can export the sandbox as a ZIP; for an imported extracted `.app`, the modified private bundle is included. The cache is excluded.
* **Archive regressions cover** stored and compressed reads, lazy cache population, CRC failures, malformed archives, traversal names, copy-on-write, whiteouts, rename, new bundle files, and preservation of the original IPA.

### Measurements (host only; not an Android/gameplay claim)

The local sample `tests/AngryBirds_v1.0_os30.ipa` has 549 ZIP entries; its app contains 268 files totaling 16,602,764 uncompressed bytes, versus a 9,769,211-byte IPA. A seven-trial Linux host microbenchmark with the new C++ filesystem reported a median 1.08 ms to index the archive and 20.23 ms to materialize five representative files (Info.plist, executable, a PVR, PNG, and Lua file; 4,160,684 cached bytes). The same implementation took 109.30 ms median to index and materialize all 268 app files. This indicates lower work/storage for that selected subset on this host, not an old-versus-new Android import or launch benchmark; the actual guest’s resource set is not represented.

Three one-second synthetic Dynarmic probe runs had medians of 1,857 MIPS guest-only and 1,533 MIPS with frequent service calls; observed ranges were 1,433–1,877 and 1,458–1,552 MIPS. These are host throughput probes, not a real-game workload or before/after speedup. Importing the original IPA still copies its bytes, guest CPU execution was not changed in this pass, and no device startup/rendering measurement was available.

### Verification and remaining gaps

* `HOST_TEST_JOBS=2 scripts/test-host.sh`: pass, including archive and rebased Objective-C regressions, sanitizer/strict-warning suites, malformed-input tests, graphics-driver marshalling tests, and Python tests.
* `CPU_BUILD_JOBS=2 scripts/test-cpu.sh`: CTest **11/11** passed, including real CPU/HLE, Objective-C, loader, and throughput-probe checks. The local environment needed temporary CMake and zlib development files.
* `scripts/check-format.sh`: pass with the repository-pinned clang-format 19.1.7 and Ruff 0.8.4.
* Android/JNI compilation and device tests are still pending because this sandbox has no JDK, Gradle, or Android SDK. No fresh `MEMORY_NULL` PC/register trace was available, and visible Angry Birds menu/gameplay is still unverified. A host test, passing APK build, or graphics-driver call count is not rendering proof.
* A brief source-based comparison with HyperHLE-Reborn is in [`HYPERHLE_COMPARISON.md`](HYPERHLE_COMPARISON.md); it separates its broader README-listed game compatibility from the filesystem trade-offs measured here.

## Targeted startup/import compatibility pass — local implementation; device validation pending

This pass wires the newly reported imports and dyld/Objective-C stop conditions through the
runtime instead of filtering names as if that were implementation. It does **not** establish
full iOS compatibility or visible Angry Birds gameplay.

### Implemented and host-tested

* `_glColorMask` is exported and dispatched through the GLES bridge. The recording-driver
  regression checks the call and its arguments; that driver draws nothing.
* C++ ABI gates now include `__ZNSt8ios_base4InitC1Ev`, the old libstdc++ COW-string
  constructors (including `__ZNSsC1EPKcRKSaIcE`), the libc++ `basic_string::__init(PKcm)`
  entry, and GCC SjLj helpers. The stream initializer tracks guard lifetime only: formatted
  iostream operations remain unsupported. String allocation/length budgets remain bounded.
* Dyld fixups distinguish final segment permissions from Mach-O `maxprot`. Loader writes to
  read-only-at-runtime data are accepted only when `maxprot` permits them, then the guest
  permissions are restored. `DYLD_TARGET` now reports the target address, segment, and whether
  it was outside, unaligned, or forbidden by maximum protection. A shared one-million
  emitted-fixup budget covers dyld stream output, external relocation entries and
  indirect-pointer entries (each opcode stream also keeps its own work cap); `DYLD_LIMIT` is
  not bypassed by splitting fixups between sources.
* `__Unwind_SjLj_Register` validates the complete 44-byte frame plus a writable link word
  before it reads/writes frame fields. A regression places a 40-byte-readable frame at the
  end of a region and verifies it is rejected.
* Objective-C class registration accepts bounded byte-sized instance sizes (including
  non-pointer-aligned layouts), includes the offending class/size in format errors, and retains
  the 4 MiB cap. Synthetic fixup, 210-byte, 70 KiB, and over-limit cases are covered.
* Session-owned CoreFoundation now implements the reported `_CFArrayCreateMutable` ABI and
  wires CFArray constructors, copies, queries and mutation through `Services`. Default and
  `_kCFTypeArrayCallBacks` ownership are modeled, including Objective-C retain/release;
  immutable mutation, invalid ranges, allocation budgets and rollback are tested. Custom
  allocators/callback descriptors and non-object values with retaining callbacks are refused.
  `CFArrayGetValues` follows ARM32's by-value `CFRange` register ABI.
* `NSRecursiveLock` is a real Objective-C class backed by the same cooperative recursive
  mutex table used by guest pthreads. `alloc`/`new`, `init`, `lock`, `tryLock`, and `unlock`
  use scheduler lock ownership, recursion, blocking and handoff; destroying the object frees
  its mutex storage. Host tests cover recursion, contention, handoff and non-owner unlock.
* Explicit `_exit`/`exit` now produce an exit report naming the routine and status. A guest
  returning to address zero without an exit is separately classified as an unexpected
  entry-point return. The regression resolves `_exit` by name rather than depending on the
  old gate address `0x6f000480`.
* Static import inspection preserves each undefined symbol's Mach-O library ordinal and
  resolves it against the registered runtime export graph before omitting it from the report.
  Duplicate names in different libraries remain distinct; duplicate name/ordinal records do
  not spam the list. Unresolved lines include weak/required status and dependency ordinal/name.
  The UI re-inspects older persisted static reports before showing them; launch output remains
  a separate report. Static inspection is evidence, not proof that a path executes.

### Performance evidence and limits

The dyld planner uses reserved hash lookups for planned fixups and applies validated fixups in
batches. The import-analysis runtime cache avoids rebuilding its service/export graph when the
dependency list is unchanged. Before the ZIP-backed follow-up above, launch extracted and cached
the whole app bundle; that behavior is superseded for new IPA launches. Gradle caching/parallelism
remain configured from earlier work. The one-second synthetic host probe from the previous pass
reported 2,769.9 MIPS guest-only and 2,305.1 MIPS with frequent `memcpy` calls (234 ns/call,
fastmem enabled, no full cache clears). The newer measurements above differ and illustrate host
variation; neither set is a before/after comparison, mobile result, IPA import benchmark, or Angry
Birds speed claim.

The `MEMORY_NULL` report still means an access to an unmapped low guest address; it does not
identify the title's pointer bug. No target binary or fresh device register/PC trace was
available to trace the pointer source. Likewise, no new Android build/device run or visual
frame was available. Host replay, zero GL errors, gate resolution and recording-driver draws
are not proof of a menu or gameplay. No rendering path was copied from HyperHLE-Reborn.

Remaining compatibility limits include standard stream operations, arbitrary C++ allocator
state, custom CFArray callbacks, Objective-C exception unwinding, virtual-base RTTI cases,
and untested target-specific dyld counts/metadata. The one-million fixup ceiling is an explicit
safety bound, not evidence that the target stays below it.

### Verification

* `scripts/check-format.sh`: pass (clang-format 19.1.7 and Ruff formatting/lint checks).
* `HOST_TEST_JOBS=2 scripts/test-host.sh`: pass (host suites, 25,000 malformed Mach-O inputs,
  10,000 memory/loader mutations, 20,000 FAT mutations, driver marshalling/emulation checks,
  and 5 Python tests).
* `CPU_BUILD_JOBS=2 scripts/test-cpu.sh`: CTest 11/11 passed, including CPU correctness,
  runtime loader, C++/Objective-C services and throughput-probe structural checks. The sandbox
  needed a temporary local CMake install because it has no system `cmake`.
* Android APK build was not run: this checkout environment has no Gradle executable/wrapper,
  Java runtime, or Android SDK. Consequently the Android-only JNI bridge and Java stale-report
  UI flow are not compile-verified locally. Converter code and `scripts/test-converter.sh` remain
  untouched.

## 0.9.6 — device-reported C++ exception handled; common API gaps filled

The 2026-10-23 Android report stopped AngryBirds 1.0 at 657,846 instructions when the
bundle-index read threw `io::IOException` and the runtime said no SjLj landing pad caught
it. I reproduced the same point on the host with the repository IPA and a recording ES2
driver (657,888 instructions, 2,767 host calls): the exception now finds its landing pad
and execution continues. The host fixture handles these first-run I/O errors through a
catch-all; the device report's clipped trace does not expose its exact LSDA catch type. The
new RTTI walk also supports typed public-base handlers. The host run's import audit has
**0 required and 0 weak unimplemented imports**. With a three-second budget the guest reaches its own loop, executes
4.42 billion instructions, starts three guest threads, and issues four draws through the
fixed-function-to-ES2 bridge. The driver records calls; this is not evidence of pixels,
main-menu visibility, or a successful device run.

The device reports from 07:04–07:06 named three more direct stops. The UIApplication badge
getter is now stateful, `NSStringFromClass`/`NSStringFromSelector` return real guest NSString
objects, and nil-delegate startup has a conservative fallback only when the image defines
exactly one `*AppDelegate` class implementing a documented launch selector. That fallback
is reported honestly: it does not decode the main NIB's outlets/actions. The hundreds of
other imports in those different applications are not all implemented or validated; this
increment adds the concrete geometry, transform, CFData, encoding, absolute-time and
Security-random APIs described below rather than mapping unknown imports to success.

* **`tools/guest_runner` — the app's entry point on a desktop.** It takes a `.app` bundle
  (or a bare Mach-O), mounts a container directory, and prints the same launch report the
  app's crash screen shows. `PODNATIVE_CPU_IN_APP` is defined for that one target so the
  real guest path is linked in. It ships in the PC artifact, so any report can be
  reproduced without a phone.
* **`tools/host_gles_driver.cpp` — a GLES 1.1/EGL diagnostic double.** The host has no GL
  driver, so the guest used to stop at the first `gl*` call. This harness tracks real GL
  state (matrix stacks with real multiply/push/pop, enables, texture and buffer objects,
  framebuffer/renderbuffer objects), answers `glGet*` with real values, and *checks that
  every client array it is handed is readable host memory* — which is what turns a
  marshalling regression into a failure instead of silent garbage. It rasterises nothing:
  the frame it returns is a test pattern, and no run is ever described as rendering.
* **`sprintf` and `vsprintf` never wrote their buffer.** Both were treated as
  capacity-zero `snprintf`s, so every formatted string produced by a title came out empty.
  The guest proved it: its log line read `Created Throwable( "" )` before the fix and
  `Created Throwable( "Failed to open /var/mobile/data/bundleIndex.idx" )` after it.
  `vprintf`/`vfprintf` are implemented too, over the same guest `va_list` cursor.
* **The bundle is writable, like the iOS 3 directory it stands for.** This title writes
  `data/bundleIndex.idx` next to its resources on first run and treats a failure as fatal;
  a read-only mount turned a normal first launch into an error. At the time of this historical
  note, the host directory was a private extraction of the IPA; the ZIP-backed follow-up above
  now routes those writes to a private overlay while keeping the path sandbox in place, so
  nothing outside the mounts is reachable (`..`, symlinks and absolute escapes are refused).
* **The stop report names what happened.** `HOST_CALLS` used to repeat `CPU_SVC` twelve
  times; it now shows the last 24 *services* the guest called (a ring, so the tail is the
  part that survives). A C++ throw reports the thrown type by reading the Itanium
  `type_info` name, plus the depth of the guest's SjLj frame chain.
* **The SjLj chain is the guest's own.** `__Unwind_SjLj_Register` now writes the previous
  frame into the frame context in guest memory (the linked list libgcc's unwinder walks),
  and `__Unwind_SjLj_Unregister` unlinks from the head or splices an inner frame out.
* **GL entry points are resolved from the driver that owns them.** EGL and GLESv2 are
  `dlopen`ed as a pair (ANGLE's `libEGL_podnative.so`/`libGLESv2_podnative.so` first on
  Android) and symbols are looked up in that handle before the process-global table.
  Android has a system `libEGL` in memory, and mixing it with ANGLE's GLESv2 yields a
  display the other library knows nothing about.

### SjLj exception matching and remaining limits

`CxxRuntime` now exports the four Itanium RTTI vtable symbols as **guest data identities**
(not executable gates) and walks `__si_class_type_info` and public, non-virtual
`__vmi_class_type_info` bases. A caught base subobject receives its adjusted guest pointer;
`___dynamic_cast` uses the same bounded non-virtual walk and the object vtable's
`offset-to-top`/dynamic-type words. A virtual-base adjustment is still refused as a failed
match/cast because it needs the complete ABI vtable search. Guest exception destructors are
also not called: invoking guest code from inside a service is still outside this runtime's
contract, and verbose reports say when that destructor is skipped.

The host replay at the device report's instruction boundary logs three `N2io11IOExceptionE`
throws. Each now finds its landing pad (the fixture's call sites use catch-all filters 1,
17 and 11), resumes through the cleanup frame, and reaches the game code instead of
`CXX_UNCAUGHT`. Separate synthetic RTTI tests exercise a thrown derived type caught as a
public base and verify the base-subobject pointer adjustment. AngryBirds then logs the
expected first-run missing `bundleIndex.idx`, `highscores.lua` and `settings.lua` files and
enters its own thread/frame loop. The repository fixture and recording ES2 driver are the
only artifacts used; no device pixels were captured.

### Other directly reported imports

`-[UIApplication applicationIconBadgeNumber]` now reads the modeled UIApplication value and
its setter stores it. Foundation's `NSStringFromClass` and `NSStringFromSelector` create guest
NSString objects instead of trapping. When `UIApplicationMain` has a nil delegate, UIKit now
uses a launch class only if exactly one image class ending in `AppDelegate` implements one of
the documented launch selectors; ambiguous or absent candidates still report the NIB gap.
This is a narrow inference, **not** NIB deserialization: IBOutlet/action connections and the
rest of the archived object graph remain unimplemented.

More concrete API coverage was added from the supplied missing-import lists: affine rotation,
CGRect containment/offset/dimensions/origin, CATransform3D rotation/translation composition,
CFAbsoluteTime, CFData create/length/guest-byte-pointer, common CF-to-NSString encodings,
Security random bytes, and common Foundation/CoreFoundation/Security/QuartzCore/UIKit string
constants. No success-returning placeholders were added for imports whose semantics are not
implemented (notably keychain CRUD, Objective-C exceptions, networking, rendering contexts,
and the large C++ string/iostream surface in the other applications).

### Verification and honest boundary

* `scripts/test-host.sh`: pass, including the new exception hierarchy/base-pointer tests,
  data/geometry tests, Objective-C helper/property tests, Security random bytes and the
  one-candidate NIB fallback test.
* `scripts/test-cpu.sh`: CTest 6/6.
* `scripts/check-format.sh`: clang-format 19.1.7 and script lint pass.
* AngryBirds host replay at 657,846 requested instructions: 657,888 executed, no unimplemented
  import trap, no required/weak unresolved imports, and three caught `io::IOException`s.
* Three-second AngryBirds host probe: `BUDGET`, 4.42B instructions, 4 recorded draws through
  ES1-to-ES2 emulation, 3 guest threads. The recording driver renders nothing; this does not
  establish gameplay or menu visibility.
* The merged Android build d24e920 is the build that produced the user's reports. **The new
  APK has not yet been built or re-tested on their device.** That remains the next evidence
  gate; converter code and converter tests remain out of scope.

## 0.9.4 — the OpenGL ES boundary is a real driver call, not a stub

AngryBirds' 51 GL imports used to stop the run at `_glFrontFace`. They now reach a real
driver: `runtime/gles.{h,cpp}` is a call gate from guest ARM code to the GLES/EGL entry
points, and the EAGL objects route into it.

* **51 entry points, marshalled as ARM code actually calls them.** iOS ARM code uses the VFP
  variant of AAPCS: integers and pointers in `r0-r3` then the stack, `float` arguments in
  `s0-s15`, numbered independently. The bridge reads both kinds separately, so a
  `glClearColor` cannot clear to a colour taken out of a pointer register. Argument
  evaluation is sequenced through locals, because `f(m.f32(), m.f32(), ...)` consumes the
  float registers in an unspecified order and does reverse `glClearColor` (the recording-driver
  test caught exactly that).
* **Client arrays are copied when the draw happens.** AngryBirds draws with
  `glVertexPointer`/`glColorPointer`/`glNormalPointer`/`glTexCoordPointer` + `glDrawArrays`,
  and a guest pointer is not host memory. Each enabled array is copied out of guest memory at
  draw time (stride and component size respected), the entry point is re-issued with the host
  copy, and no copy is made for an array that lives in a bound buffer object.
* **Pixel uploads are sized, not guessed.** `glTexImage2D`/`glTexSubImage2D` copy
  `w*h*texelBytes(format, type)` bytes out of guest memory, `glCompressedTexImage2D` copies its
  PVRTC size, `glLoadMatrixf`/`glLightfv`/`glMaterialfv` copy their arrays, and `glGetIntegerv`
  writes back only into memory the guest can write.
* **ES 1.1 gets an ES 1.1 context.** `-[EAGLContext initWithAPI:]` carries the guest's choice
  into EGL; a 2.0 context rejects the fixed-function calls this title lives on, so this is the
  difference between a black screen and a frame. `-renderbufferStorage:fromDrawable:` creates
  the colour renderbuffer and framebuffer from the drawable's real geometry and reports a
  driver failure by name, and `-presentRenderbuffer:` reads the drawable back (top row first)
  for the host frame.
* **No driver means no success.** With nothing loaded, `GLES` reports itself unavailable, every
  import keeps its `HLE_UNIMPLEMENTED` trap naming the exact symbol, and the EAGL methods throw
  `EAGL_NO_DRIVER` instead of returning a drawable that does not exist. `describe()` puts the
  backend, entry-point count and call/draw counters into the launch report line.

Verified by CI run [39745421769](https://github.com/RadekParek/AnyiOS/actions/runs/39745421769): PC
host suites green, Android `assembleDebug` green, artifact `AnyiOS.apk` (10,354,572 bytes) built
from this commit. What CI proves is that the bridge compiles for `arm64-v8a` against the real
ANGLE libraries and that the host units pass; it does not prove a frame reaches the screen.

Evidence on this host (`scripts/test-host.sh`): a recording GLES/EGL driver
(`tests/gles_stub_driver.cpp`) is `dlopen`ed exactly as a real driver is, and the unit test
verifies the register split, the draw-time client-array copy, texel-size uploads, the
renderbuffer/framebuffer transitions, the ES-version switch (context destroyed, not leaked) and
the bottom-left → top-row-first flip. The recording driver rasterises nothing, so **no frame,
FPS or gameplay claim is made**, and the ANGLE-on-device path still has to be exercised on the
S23 before anything is claimed for it.

## 0.9.3 — AngryBirds runs its own code into GL

The user's AngryBirds IPA now boots through the real launch path instead of stopping at the
first dozen instructions. Each step below was a genuine defect, not a stub:

* **Every guest was cut off at 200,000 instructions.** The run loop treated the end of a
  200,000-instruction CPU slice as the end of the run, so a title could never get past its
  startup. It now continues until the caller's total budget or the wall clock stops it.
* **Classic external relocations were ignored.** This binary predates `LC_DYLD_INFO`: all 348
  of its Objective-C binds (`_OBJC_CLASS_$_UIView`, `_OBJC_METACLASS_$_NSObject`,
  `__objc_empty_cache`, `__objc_empty_vtable`, the class refs) live in `LC_DYSYMTAB`'s
  `extreloff/nextrel` table. Without them every superclass and class-ref slot stayed zero, so
  an image-defined class had no superclass at all. `loadMachO` decodes the table into ordinary
  fixups (4-byte absolute pointers only; anything else is `DYLD_RELOCATION`), and the whole
  table is range-checked like every other linkedit table.
* **A superclass is a bind, not a rebase.** Guest classes are registered before dyld runs, so
  their `superclass` field was read as zero once and cached. It is now read from guest memory
  when the chain is walked, so `[MyEAGLView -initWithFrame:]` and its siblings are found.
* **Class symbols belong to the framework that exports them.** Every class was defined under
  Foundation, so a UIKit-ordinal bind (and the CoreFoundation-ordinal bind of `NSDictionary`)
  resolved to the unimplemented-import trap instead. Class and metaclass symbols are now
  defined in the owning framework, the toll-free bridged Foundation classes in CoreFoundation
  as well, and `__objc_empty_cache`/`__objc_empty_vtable` in libobjc.
* **Method lookup missed the metaclass and the host superclass.** `+layerClass` and friends
  arrive with a metaclass address that is not in `__objc_classlist`; lookups now map it back to
  its class. Methods inherited from a modelled framework class (`-layer`,
  `-setMultipleTouchEnabled:`, `-initWithFrame:`) now reach the host implementation.
* **Struct returns.** `objc_msgSend_stret` now writes the four returned words through the
  caller's buffer instead of refusing, which is what `[UIScreen bounds]` needs. A `CGRect`
  argument is accepted either as a pointer or as four words in `r2-r5`.
* View state is kept for instances whose class the image defines, layer objects follow the
  image's own `+layerClass` override, `UIAccelerometer`/`EAGLContext`/`CAEAGLLayer` are real
  objects with real properties, and `+dictionaryWithObjectsAndKeys:` builds a real dictionary.

Result on this host: AngryBirds runs **340,304 instructions** of its own ARMv6 code (from 86
before this round), through `UIApplicationMain`, the `AppController` delegate, window and
`MyEAGLView` creation and EAGL context setup, and stops at `_glFrontFace` — the first OpenGL ES
call. That is the honest boundary: the host probe links no GL, and the next step is the
EAGL/ANGLE bridge that forwards these calls to the real ANGLE libraries the app already ships.
`-[EAGLContext renderbufferStorage:fromDrawable:]` and `-presentRenderbuffer:` refuse by name
rather than pretending a drawable exists. No gameplay, rendering or device claim is made.


## 0.9.2 — startup-blocker sweep

Every item below was a named blocker in a real launch report. Each one is now a code
change with a host test behind it; none of them is a stub that returns success.

1. **`DYLD_ADDRESS: cursor arithmetic overflow`** — the dyld cursor is `uint64_t`
   arithmetic that wraps, exactly like Apple's `address +=`. Link editors encode a
   backward skip as a ten-byte ULEB (the two's complement of the distance), so rejecting
   the wrap refused valid streams. `decodeDyld` now advances modulo 2^64 and validates
   the resulting fixup target against the selected segment (`DYLD_TARGET`/`DYLD_SEGMENT`
   when it really is outside the image).
2. **`DYLD_TRUNCATED: stream is missing DONE`** — dyld's own decoder loops `while (p < end)`
   and never demands a trailing `DONE`. Streams that stop cleanly on an opcode boundary are
   accepted; only an operand that is cut in half is `DYLD_TRUNCATED`.
3. **`DYLD_LIMIT: rebase repeat count`** — repeated rebases/binds are bound by the remaining
   fixup budget rather than by the raw opcode count, and a rebase plus a bind on the same
   slot is a lazy pointer that merges (bind wins) instead of a conflict.
4. **`DYLD_STUB: symbol stub size is not patchable`** — real ARM binaries contain 4-byte
   `ldr pc, [pc, #imm]` stubs (573 of them in the Minecraft build). Those are dispatched
   through the pointer slot they name, with the instruction validated as an
   `ldr pc, [pc, #imm]` before the slot is touched; the 12-byte rewrite path is unchanged.
5. **`HLE_UNIMPLEMENTED: _vsnprintf`** — `vsnprintf`/`_vsnprintf` now format through the
   existing formatter by walking the guest `va_list` with AAPCS rules (8-byte alignment for
   `double`/`int64_t`), writing into the caller's buffer and returning the untruncated length.
6. **`UNIMPLEMENTED _UIApplicationMain`** — a real bootstrap in `runtime/uikit.cpp`: argv is
   parsed into a guest `argc/argv` pair, the delegate class named in the call's fourth argument is
   instantiated through the guest's own class metadata, its launch selector is called, and control returns
   to the caller only at a lifecycle gate the runtime owns. A missing delegate class, an
   unusable `argv` or a delegate without a launch selector are named faults
   (`UIKIT_DELEGATE`, `UIKIT_ARGV`), never silent success.
7. **`FAULT MEMORY_UNMAPPED` at `0x0`** — an access in the first page is now
   `MEMORY_NULL: null pointer dereference` and names the guest PC, so a nil message result
   or a null import is distinguishable from a random unmapped address.
8. **`LOAD_COMMAND_UNSUPPORTED: 0x2b`** — `LC_DYLIB_CODE_SIGN_DRS`, `LC_SEGMENT_SPLIT_INFO`,
   `LC_ENCRYPTION_INFO_64`, `LC_LINKER_OPTION` and `LC_LINKER_OPTIMIZATION_HINT` are parsed
   and range-checked. Encrypted images are still refused by name, and `LC_DYLD_ENVIRONMENT`
   is validated and deliberately ignored so a guest cannot inject `DYLD_*` variables.

Stop reports now also name the guest function: the loader keeps the image's `N_SECT`
symbols (compiler labels such as `ltmp0` are dropped), and `GUEST_BLOCKER` gains
`symbol=<nearest function>`. The crash screen shows "Guest PC in &lt;function&gt;" instead of a
bare hex address.

Display work: games request landscape (`SCREEN_ORIENTATION_LANDSCAPE`, the landscape-left
holding position an iPhone title expects) instead of starting whichever way the phone
happened to lie, and the debug console starts hidden — it is opt-in from Settings.

### Faster pipeline

The same checks now take much less wall clock:

* The PC job restores the pinned dynarmic/Boost/converter source trees (~1 min each run)
  and compiles both suites through a content-addressed ccache, so unchanged objects are
  never rebuilt.
* The Android job reuses those sources, keeps the existing verified ANGLE cache, and runs
  Gradle with the build cache and parallel module execution instead of re-running task
  outputs it already produced.
* `scripts/test-host.sh` builds its fourteen sanitizer binaries and its strict-warning
  sweep in parallel (`HOST_TEST_JOBS` caps it), which took the suite from ~110 s to ~57 s
  here, and both workflows no longer run the same host suite twice.
* Branch pushes no longer trigger a second full run of the same two jobs that their pull
  request already starts.

Evidence: `scripts/test-host.sh` is green end to end (loader/Mach-O fuzz, dyld, export graph,
libc, C++, audio, fs, Objective-C, UIKit bootstrap); both test games the user added to
`tests/` load through the same path the app uses (fat slice selection included): AngryBirds
maps 36,564,992 guest bytes, decodes 230 bindings, and reports **143** required imports still
missing; Minecraft PE maps 37,109,760 guest bytes, decodes 20,585 rebases and 994 bindings,
and reports **391** missing. Both are loader/dyld results on this host — no device, rendering
or gameplay claim, and neither title is dumped, patched or redistributed.


This supersedes the initial BUILD_REPORT.md where noted. The user reports the
previous graphics preview working on their S23 Ultra; no screenshot/log was supplied.
No Android instruction-execution or game-compatibility result is claimed.

## Latest: 0.2.1 startup preview

[Verified APK and CI evidence](STARTUP_APK_REPORT.md): no fixed import-byte/library
entry quota, bounded LC_DYSYMTAB handling and an enabled startup-validation action
with saved/exportable launch blockers. Android lint/build/tests/signing and runtime
host/ARM64 cross-build checks passed. No game code runs in the app yet; the next
execution contracts are recorded in EXECUTION_NEXT.md.

## Previous: library preview

[0.2 APK and verified CI evidence](LIBRARY_APK_REPORT.md): persistent private IPA/folder
library, universal ARM32 selection, metadata parser, modern native UI and original
launcher icon. Android build/lint/tests/signing and runtime host/cross-build checks
passed. New UI/import behavior still needs physical-device review. Game launch
remains unavailable; see [limits](LIBRARY_PREVIEW.md).

## Engineering changes

1. Committed and pushed the original foundation as `7827f026e79a49e09936f942d720743b010732a3`
   on `arena/568194d8-podnative` so GitHub Actions could actually execute.
2. First Android run failed in `android-actions/setup-android`. Its complete log
   download host is inaccessible from this sandbox; the job/step result was confirmed
   via the GitHub API. Do not infer an exact root cause from that limited evidence.
3. Replaced that opaque setup step with the runner's explicitly checked preinstalled
   SDK toolchain, added persistent SDK/build diagnostics and API-visible failure tails.
   The second run passed SDK setup, formatting and host tests and **successfully
   compiled actual upstream ANGLE**: 1,636 build steps in 7m44.87s. Its subsequent
   dependency audit rejected `libnativewindow.so`. The pinned upstream BUILD.gn
   explicitly identifies this as the public API-26+ ANativeWindow library, so the
   corrected audit permits it on our API-29 minimum while tests reject private
   vendor dependencies and native-GLES substitution. No APK was produced by this run.
4. Added guest memory, fixed-address Mach-O mapping validation and temporary mapping
   diagnostics in the app import flow. Parser, memory and loader tests include
   25,000 malformed inputs plus 10,000 mutated executables under ASan/UBSan.
5. Read and retained CPU/dependency licenses, pinned Dynarmic and Boost, and added an
   independently designed adapter using actual Dynarmic. Built the upstream engine
   and **executed** ARM/Thumb fixtures successfully on this x86-64 host.
6. First ARM64 CPU cross-build failed to locate prepared Boost headers because NDK
   root-path rules exclude host-stored headers. The fix sets only the explicitly
   checked pinned Boost include directory, without disabling NDK cross-search rules.
   Host runtime tests also passed on GitHub Actions.
7. Added bounded static Mach-O import evidence (required/weak symbols, dylibs and
   fixed-function/programmatic GLES import hints) to the import report. This helps
   prioritize real dependencies without uploading a game. It is not an execution
   trace, complete dyld analysis or PVRTC-format detection.
8. Added CPU regression coverage for services, permissions, faults, bounded execution
   and host-written/self-modified instruction cache invalidation. Added separate
   host-execution and Android ARM64 cross-compilation CI jobs.

See RUNTIME.md for exact behavior, reproducible commands, limitations and the future
ADB executable test. CPU engine and dependency source pins are under
`third_party/dynarmic/`; no source/vendor trees or generated binaries are committed.

## Evidence boundaries

* Actual host CPU tests: passed locally and in GitHub Actions, including real ARM
  and Thumb execution via Dynarmic.
* Parser/memory/loader ASan/UBSan regression/mutation tests: passed.
* Formatting, Python lint, shell syntax, YAML syntax: passed locally.
* Device: none attached; user retains the S23 Ultra and game privately.
* Guest CPU inside Android app: not yet linked.
* Actual upstream ANGLE compilation: passed in run 39490248638; post-build audit
  failed on the now-corrected platform-library classification.
* ANGLE runtime, textured cube screenshot, gameplay: not verified.
* APK existence: only assert after a completed CI build publishes one; expected names
  in earlier documents are not evidence that an APK exists.
* Ninjago stages A–G: no game-stage evidence. Host fixtures are not the game.

The runtime is still a foundation, **not a finished high-level emulator**. Dynamic
linking/relocations, iOS ABI, Objective-C/framework semantics and guest graphics APIs
remain blockers. Do not implement arbitrary successful framework stubs to hide them.

## CI evidence links

* Initial SDK-action failure: https://github.com/RadekParek/PodNative/actions/runs/39489735013
* Successful upstream ANGLE compile / subsequent audit failure: https://github.com/RadekParek/PodNative/actions/runs/39490248638
* Host CPU success / first ARM64 cross-configuration failure: https://github.com/RadekParek/PodNative/actions/runs/39491738356

Full log download redirects are blocked by this sandbox's host allowlist; failing
build tails are additionally emitted as API-readable annotations. An attempted
cancellation of the duplicate known-failing Android run was denied by GitHub token
permissions; no permission or credential workaround was attempted. Future Android
runs are serialized, and successful ANGLE outputs will be cached before app
compilation to avoid repeating a long source build for unrelated app fixes.

## Further observed results

Runtime run https://github.com/RadekParek/PodNative/actions/runs/39492740731 passed
both host execution and ARM64 Android cross-compilation at revision
`9e380159e4d2c09c12d59c2c7be72d8cda5ec77f`. GitHub published the initial diagnostic
artifact (ID 12296712326, 7,154,331 bytes); no device execution occurred. Subsequent
review found that the pinned Oaknut Linux/Android allocator's RWX/MAP_FAILED handling
needs hardening before APK integration. The follow-up patch and host permission
regressions are documented under third_party/dynarmic/; the old artifact predates them.

Android run 39492740897 stopped at depot_tools bootstrap (exit 128) before compiling
ANGLE. Its uncaptured clone output prevents an exact root-cause assertion. The next
workflow captures that output and emits the failure tail through the API; no
unverified download mirror or silent substitute has been added.


Runtime run 39493714457 passed both host correctness (including the allocator
permission regression) and the patched Android ARM64 cross-build. It published
`PodNative-arm64-cpu-test` artifact ID 12296603610 (7,398,437 bytes), including the
separate allocator diagnostic. This is still compilation evidence, not device output.

The added diagnostics in Android run 39493714456 identified the next blocker:
**HTTP 503 during depot_tools self-update**, after bootstrap clone succeeded.
The official tool source is now pinned to Chromium DEPS revision
`1eb46f8b23b4c6b3caa2be8e05999b6d1f4d0253`, and self-update is disabled so a
reproducible checkout is not immediately replaced by an unversioned network fetch.
No ANGLE substitute, alternate driver or unofficial tool mirror is introduced.


Run 39494542779 passed the current host and patched ARM64 compile checks again.
Run 39494542835 passed the pinned source fetch, then reported
`python3_bin_reldir.txt not found`: disabling self-update on a fresh checkout also
requires explicit initialization of depot_tools' pinned Python/CIPD helpers.
`bootstrap-angle-tools.sh` now calls upstream `ensure_bootstrap` before invoking
gclient. This is the upstream initialization path named in the diagnostic; no
homemade Python marker or unverified binary was substituted.


## First successful APK build

Android run 39495542750 **passed** at source
`ed0f08e97c7272ab515daa206bf28cb3d4dadb56`, including real ANGLE compilation,
ABI/dependency closure, Android lint/build, debug signing verification and APK
publication. Artifact ID 12297629398 contains the actual PodNative-debug.apk,
checksums and build provenance. Runtime run 39495542649 also passed host execution
and patched ARM64 diagnostic compilation. Full evidence, download links and exact
limits are in [FIRST_APK_REPORT.md](FIRST_APK_REPORT.md). No device frame, gameplay
or Android CPU execution is claimed. The next required gate is the user's real
S23 Ultra graphics test; it cannot be verified from this unattached sandbox.

## Angry Birds 1.0 (os30, armv6): start-up reaches its own game loop

Measured with `tools/guest_runner` on the host CPU engine. This is guest execution
evidence only: the host GL harness is a recording double that renders nothing, so
nothing here is a claim of a drawn frame, gameplay or frame rate.

The title's own structure explains what it needs from the runtime. `AppController`
implements `applicationDidFinishLaunching:`, `applicationDidBecomeActive:`,
`mainloop`, `update` and `present`; its `-applicationDidBecomeActive:` creates an
`NSThread` whose selector is `mainloop`, and `mainloop` hands `update` to the main
thread with `performSelectorOnMainThread:withObject:waitUntilDone:`. Each of those
steps was a named blocker, and each is now implemented rather than stubbed:

* The launch sequence delivers `-applicationDidBecomeActive:` after
  `-applicationDidFinishLaunching:`, as UIKit does. Without it the title never
  started its loop thread and exited from `UIApplicationMain`.
* `NSThread` is real: `-initWithTarget:selector:object:` plus `-start` (and
  `+detachNewThreadSelector:…`) create a guest thread through the scheduler, which
  enters the target's selector on that thread's own stack; `-cancel`,
  `-isCancelled`, `-isExecuting`, `-isFinished` report the thread's real state.
* The main-thread hop is real: the selector is queued, the run loop dispatches it on
  the main thread, and a `waitUntilDone:YES` caller stays blocked until it has run.
  The main thread parks in the run loop and is woken by the queue.
* `sched_yield` and the sleep family block the *guest* thread instead of the host:
  the scheduler switches to another thread and is idle (not spinning) when only
  sleepers remain.

Faults that C++ cannot report are now reported as guest faults: host code inside
translated code has no unwind tables, so a SIGSEGV/SIGBUS is printed with the guest
registers and the 256-instruction PC ring before the default action.

Measured on the current tree (host, recording GL double):

* start-up completes: the delegate launches, the application becomes active, the
  loop thread is created and `-[AppController mainloop]` runs;
* the run loop dispatches `update` on the main thread;
* a 15-minute run reached 1.46G instructions with no unimplemented-import trap and
  no fault until the guest heap could not satisfy a single 47 MB `operator new`.
  That was a real limit of the runtime, not of the title: the heap window is now
  512 MiB and it is committed to the guest address space in 8 MiB steps on demand,
  so the host only backs the pages the title actually touches. The report prints
  used/mapped/reserved bytes and the live allocation count, which is what turns the
  next exhaustion into a diagnosis instead of a mystery;
* GL: the driver is loaded, the EAGL drawable is created, the title's own GL state
  calls execute and draw calls are actually issued — the previous version handed
  guest addresses straight to the host driver, which is what produced the host
  SIGSEGV at 200M instructions. Client arrays are now copied into host storage
  whenever no buffer object is bound, and the count of draws stays the honest
  number the bridge observed.

A guest pointer can no longer reach the driver: the bridge copies client arrays
into host storage and passes offsets only when a buffer object is bound, which is
the only case in which the driver would dereference one.

The main-thread queue is bounded (8192 pending selectors): a title that queues
faster than the run loop drains is a runtime bug worth naming, not a memory leak to
absorb silently.

The Settings screen has a **Verbose logging** switch, off by default. When it is
on, the runtime logs the guest's service calls, its Objective-C call graph, every
GL entry point with its arguments, and — after each GL call — the driver's own
`glGetError`, so a GL error is traced where it happens instead of being swallowed.

Still open: no displayed frame (the host harness renders nothing, and no display
link or touch source exists), no texture upload path for compressed atlases if the
title asks for one, and the four Itanium RTTI vtables
(`__ZTVN10__cxxabiv1*`) are still unbound, so `dynamic_cast`/`typeid` are reported
as missing rather than approximated.

## The Android build compiles under the NDK's clang

The release job builds `podnative` with the NDK toolchain and `-Wall -Wextra -Werror`. The
host suites compile the same sources with GCC, and GCC does not report three warnings
clang does, so a green host run said nothing about the Android binary:

* `cxx.cpp` kept a `UnwindContinue` constant that no code used. Clang reports an unused
  `constexpr` at namespace scope (`-Wunused-const-variable`); GCC does not diagnose
  unused constants in C++ at all.
* `stdio.cpp` captured `input` in the `sscanf` push-back lambda, which only rewinds the
  cursor. Clang reports unused lambda captures (`-Wunused-lambda-capture`).
* `posix.h` held a `GuestHeap&` the scheduler never read; the thread stacks live in their
  own guest window. Clang reports unused private fields (`-Wunused-private-field`).

The fix is a compile of every runtime translation unit with an actual clang front-end
(`zig c++`, LLVM-based) under the same flags plus those three warnings promoted to errors,
which is what the host loop now checks before pushing: `runtime/*` and the top-level
Mach-O sources build clean that way. `bridge.cpp` and `renderer.cpp` need the NDK and ANGLE
headers, so they stay covered by the CI job only.

While removing the dead OpenAL constant, the pause path was found to be a stop in
disguise: `-alSourcePause` reported `AL_STOPPED` and a later `-alSourcePlay` restarted the
buffer from the beginning. It is a real state now — `AL_INITIAL` before the first play,
`AL_PAUSED` while paused, `AL_PLAYING` again on resume with the elapsed offset kept, and
`AL_STOPPED` only after a stop. `tests/audio_test.cpp` covers the four transitions.
