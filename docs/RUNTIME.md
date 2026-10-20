# Runtime foundation: memory, Mach-O and actual ARM32 execution

## What now works in host tests

PodNative's original guest-memory and loader code is independent of graphics and
Android UI. A new original adapter uses **actual upstream Dynarmic 6.7.0**; it is
not a homemade recompiler and does not wrap HyperHLE's CPU wrapper.

On Linux x86-64, the adapter has actually executed original ARM and Thumb fixtures.
Tests check arithmetic, register results, loads/stores, SVC traps, bounded execution,
undefined instructions, instruction/data permissions, and a faulted-state reset
requirement. A regression modifies guest executable bytes between slices and verifies
that cached translated instructions are invalidated before execution continues.
This is host correctness infrastructure, not desktop product support or evidence of
AArch64 Android execution. No proprietary fixtures are used.

## Memory contract

* Explicit ARM32 address range with checked 64-bit range arithmetic.
* 4 KiB **guest** pages (not an assumption about Android host page sizes).
* Nonoverlapping region maps with read/write/execute metadata, zero fill and budget.
* Execute-only checks for instruction fetch; read/write checks for data operations.
* A write spanning multiple regions is fully prevalidated before any mutation.
* Little-endian scalar reads/writes; no raw backing pointers escape to the UI.
* Backing bytes are ordinary host data, never host-executable guest mappings.
* Successful maps/writes advance a memory revision. CPU callbacks invalidate code
  ranges; host modifications between slices invalidate cached translated code.
* Single CPU owner thread. Thread safety, guest shared memory and atomic/exclusive
  semantics require additional work. Exclusive-store callbacks explicitly fault.

## Loader contract

`loadMachO` first runs bounded structural inspection, then validates fixed-address,
non-PIE ARM32 Mach-O segments, sections, protections, file/virtual ranges, entrypoint,
metadata and dynamic-link information. It maps initialized bytes and zero-filled
segment tails, leaves guard segments unmapped and allocates a separate guarded
1 MiB guest stack. Guard ranges are included in stack-overlap validation even when
they require no backing allocation. Entry conversion supports file-relative LC_MAIN
and the historical LC_UNIXTHREAD ARM_THREAD_STATE register layout.

PIE, TLS, section relocations, unknown commands and unsupported initial stack states
are explicit errors. Dynamic dependencies and symbols are not resolved. Symbol/dyld
metadata is bounded and marks execution as blocked, not silently successful.
No library loader, Objective-C runtime, argc/argv/environment initialization,
Foundation/UIKit, EAGL or high-level framework implementation exists yet.

The Android inspection bridge now attempts a **temporary, capped (64 MiB) mapping
validation** after structural inspection. Mapping results or `LOAD_BLOCKED` reasons
are logged. Memory is released afterward. This is not a retained game session and
there is still no game-launch button that pretends to work.

## Static import evidence

The importer can also analyze a bounded Mach-O symbol table and dylib load commands.
It reports required versus weak undefined symbols (a required duplicate overrides
weak), ignores defined/debug/local symbols, and identifies a few fixed-function,
programmable and compressed-texture API imports. It validates offsets, terminators,
counts and total stored-name budgets. Names are escaped for diagnostics. This is
not a dynamic trace or a complete dyld binding parser; missing symbols do not prove
API absence, and compressed texture imports do not establish PVRTC usage.

## CPU adapter contract

`Cpu` owns a Dynarmic A32 JIT, configured for v7, safe optimizations, callback-based
checked memory, memory-access halt checking and a bounded 16 MiB code cache. The upstream build enables its no-execute support; a reviewed opt-in Oaknut
allocator patch is additionally required because the original Linux/Android path
still uses RWX and fails to recognize MAP_FAILED. The patch allocates RW then
switches RW/RX with checked errors and no RWX fallback. Host kernel-permission
tests pass; actual Android policy behavior remains unverified. The initial execution path uses
Dynarmic `Step()` for bounded correctness slices; it is not a fast-path benchmark.
No iOS syscall is emulated as a successful no-op. SVC returns a service-trap result
requiring an actual guest ABI handler. Interpreter fallbacks, architectural
exceptions, exclusive operations and memory faults return structured errors.

Every stop includes attempted instruction count and guest PC; faults also preserve
the fault address and code. Attempted instructions are not a cycle/FPS measure.
After a fault, callers must reset state before continuing. Core execution contains
no reference to EGL, GLES, Vulkan or Activity classes.

Known gaps: AArch64 device validation, VFP/NEON conformance, atomics, multithreading,
TLS/coprocessors, fast block-mode timing, iOS ABI/framework dispatch and production
cancellation. The adapter is **not yet linked into the APK**. Header/source provenance
and dependency licenses are recorded under `third_party/dynarmic/`.

## Repeatable host validation

With git, Python3, CMake >=3.22, a C++20 compiler and make:

```sh
scripts/test-host.sh       # ASan/UBSan parser, memory and loader regressions + mutation tests
scripts/test-cpu.sh        # fetch exact reviewed source pins; build real Dynarmic; CTest
```

Only ignored `.local` directories hold third-party sources, symlinked unmodified
Boost headers and generated objects. `prepare-cpu.sh` refuses to overwrite dependency
checkouts whose revision differs from the required pin. Boost's own boostdep tool
resolves ICL/Variant headers using pinned superproject gitlinks, not floating HEADs.

`Runtime correctness` CI separates actual host execution from Android ARM64
cross-compilation. Android compilation success alone must never be recorded as
instruction execution on the S23 Ultra.

## Android ARM64 device test, when the current CI artifact exists

Run 39492740731 successfully published the initial cross-compiled CPU diagnostic
at source `9e380159e4d2c09c12d59c2c7be72d8cda5ec77f`; it does **not** include the
subsequent allocator patch. Do not treat that earlier artifact as W^X-validated.
Use a later successful build containing `PodNative-arm64-wx-test` as well as the
instruction test. These executable diagnostics are **not APKs**. Extract the artifact and verify its SHA256SUMS and BUILD-INFO.txt before running:

```sh
adb push PodNative-arm64-wx-test /data/local/tmp/PodNative-arm64-wx-test
adb shell chmod 700 /data/local/tmp/PodNative-arm64-wx-test
adb shell /data/local/tmp/PodNative-arm64-wx-test
adb push PodNative-arm64-cpu-test /data/local/tmp/PodNative-arm64-cpu-test
adb shell chmod 700 /data/local/tmp/PodNative-arm64-cpu-test
adb shell /data/local/tmp/PodNative-arm64-cpu-test
```

A real success must print the test pass line and return exit code zero. Attach the
actual output, source revision, Android build and device model. Failure is a CPU
integration blocker; it does not justify bypassing memory or JIT permission checks.

An ADB-shell executable runs under a different UID/policy than the installed app.
A passing shell test is useful evidence but does not certify execution inside the
APK. App-UID tests and the graphics scene remain required before game execution.
