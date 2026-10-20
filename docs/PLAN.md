# First vertical slice

1. Inspect the empty repository, toolchain and upstream documentation. **Done.**
2. Record independent architecture, research and ANGLE delivery strategy. **Done.**
3. Implement Android shell, real-ANGLE backend, textured 3D cube, diagnostics and bounded Mach-O inspection. **Source implemented; Android build/runtime unverified.**
4. Test the portable parser with malformed inputs under sanitizers; add CI for full Android builds. **Host tests passed; CI not run.**
5. Build an APK in an unrestricted Linux Android build environment. **Pending.**
6. Install on the user's S23 Ultra, capture a real rendered scene and lifecycle logs. **Pending; device is with user.**
7. Only after the graphics gate passes: integrate a proven ARM32 CPU engine and implement observed Ninjago dependencies.

No game binaries, inherited renderer, fake ANGLE, placeholder iOS framework functions, speculative FPS claims or fabricated screenshots are part of this slice.
