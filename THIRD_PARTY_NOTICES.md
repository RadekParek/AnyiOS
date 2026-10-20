# Third-party provenance and license status

PodNative graphics/loader/UI source is newly written here; no TouchHLE/HyperHLE
renderer or framework code is incorporated. Research is not component reuse.

## ANGLE — integration target, not yet built in this environment

* Origin: https://github.com/google/angle
* Exact revision: `378b820f1c00d511565ab9990ae03de77908b373`.
* License: upstream BSD-style 3-clause; full copyright, conditions and disclaimer
  retained verbatim at `third_party/angle/LICENSE`.
* Intended use: build and embed upstream EGL/GLES libraries and use upstream API
  headers. Top-level terms permit source/binary redistribution subject to notices
  and no endorsement. No upstream source modifications.
* Built artifacts: not yet generated, so no binary dependency manifest is claimed.
* Dependencies: exact versions are pinned by that ANGLE revision's DEPS. Build
  script collects license and README.chromium texts with an index into APK assets.
  **All compiled dependency terms and attribution must still be reviewed against
  the actual built/link closure before public distribution.** Do not infer full
  dependency permission from ANGLE's top-level license alone.
* App links to Android platform APIs/libraries; no vendor GPU driver is distributed.

## Research only, not redistributed or linked

HyperHLE-Fork `5b65054c55cc33c6095f8c4b771430da5e63fbf4` (MPL-2.0):
README/manifest/license research; no code copied.
Dynarmic `a41c380246d3d9f9874f0f792d234dc0cc17c180` (metadata: 0BSD):
CPU candidate only. Dependency/license and technical integration review pending.
Unicorn (metadata: GPL-2.0): alternative candidate, not selected or included.

## Build/test tooling, not app runtime code

Android Gradle Plugin 8.9.2, Gradle 8.11.1, Android SDK/NDK/CMake, JDK17,
Chromium depot_tools/GN/Ninja and GitHub Actions are build tools, fetched by CI.
Formatting: clang-format Python distribution 19.1.7 (LLVM tooling), Ruff 0.11.13;
installed only into ignored local/tool environments or CI, not packaged in APKs.
Keep upstream terms when redistributing tools; this repository does not vendor them.

No proprietary game executable, asset, Apple framework implementation, signing key
or credential is included. A standalone PodNative source license has not yet been
chosen by the repository owner; third-party licenses do not license this project's
original code automatically.
