# PodNative architecture

Update: portable guest memory, mapping validation and the real Dynarmic CPU
adapter are now implemented/tested on the host; see RUNTIME.md for current contracts.
Guest frameworks and guest graphics remain unimplemented.

```text
Android Activity / SAF importer / settings / diagnostics
              |                   |
        PrototypeView       bounded Mach-O inspector
              |                   |
    GraphicsBackend         future guest memory / loader / CPU
              |                   |
     embedded ANGLE     future guest GLES + EAGL compatibility
              |                   |
      Vulkan / surface <--- PodNative-owned graphics boundary
```

## Implemented ownership

* `MainActivity`: native navigation, private import, persistent settings and log
  export. It contains no guest CPU loop. Only the synthetic test can launch.
* `GameInspector`: archive enumeration and bounded bytes; no ZIP extraction,
  proprietary assets in source/CI, executable mappings or DRM circumvention.
* `macho`: portable original parser with no Android or graphics dependencies.
* `PrototypeView`: a single graphics owner thread for attach/draw/teardown,
  monotonic-time animation, cap pacing, basic draw+swap measurements. Frames are
  counted only after successful draw and swap. Resources are reused every frame.
* `GraphicsBackend`: PodNative interface for surface attachment, frame execution
  and diagnostics. It deliberately does not invent a duplicate rendering engine.
* `AngleBackend`: owns EGL context/surface/program/texture/buffers, validates
  shader/link/framebuffer/draw/presentation errors, releases resources on teardown.
  Unique ANGLE shared libraries provide GL implementation; Vulkan is preflighted.

All contexts/resources are surface-lifetime in this prototype. Pause releases the
backend; resume and rotation recreate the synthetic scene. No claim is made that
this already preserves future guest objects. Teardown currently joins the owner
thread synchronously to prevent use-after-destroy; a stalled driver could block the
UI/ANR. Device testing and bounded render-worker lifecycle handling are required
before treating this as production-ready. No GPU calls occur on the UI thread.

## Guest-facing design (not implemented)

1. **Guest API/state:** decode ABI arguments from guest memory, validate ranges,
   maintain guest object IDs, error flags and per-context state. Do not expose host
   pointers or GL names to guest code. Unsupported enum/API returns structured
   capability failure instead of dropping an operation.
2. **iOS compatibility:** EAGL context identity and thread binding; share-group
   ownership; CAEAGLLayer-backed drawable allocation, renderbuffer storage,
   presentRenderbuffer behavior; detach versus destroy; UIKit/CA orientation,
   scaling and composition. Shared-resource tests must establish lifetime semantics.
3. **PodNative graphics model:** typed context/drawable/share-group handles;
   generation-checked textures, buffers, shaders/programs, framebuffer attachments;
   surface epochs and explicit presentation result. Keep state/program translation
   at the guest boundary; send supported GLES to ANGLE without a second pipeline
   compiler. Add narrow resource interfaces only as guest semantics require them.
4. **ANGLE integration:** public EGL/GLES implementation and backend capabilities.
   Must report missing formats/extensions and context loss explicitly. Logical
   guest resources must be restorable or guest-visible loss handled correctly.
5. **Android presentation:** surface epochs, input coordinate transform, pacing,
   orientation and lifecycle. Pause guest timing independently of graphics; do not
   accelerate simulation by raising presentation FPS.

## CPU and framework roadmap

Integrate reviewed Dynarmic A32/AArch64 behind execution/memory/host-call interfaces.
Use synthetic A32 and Thumb fixtures before game code. Add validated virtual memory,
Mach-O segments, entry point, relocations and symbols. Enforce bounds, executable
permissions and W^X; unsupported instructions/symbols carry guest PC and context.
Guest ABI bridging and Objective-C messaging precede UIKit startup. The subsequent continuation integrates and host-tests Dynarmic in a separate CPU
test target; Android linking and execution are not yet verified.

Implement observed Foundation/CoreFoundation, ObjC classes/selectors, UIKit,
EAGL/Core Animation, file access, timers, audio and input only after trace evidence.
Missing framework calls are blockers, not placeholder successes. Game-specific
requirements and priorities are currently unknown.
