# Third-party provenance and license status

PodNative graphics/loader/UI source is newly written here; no TouchHLE/HyperHLE
renderer or framework code is incorporated. Research is not component reuse.

## ANGLE — source-built and packaged in the debug APK

* Origin: https://github.com/google/angle
* Exact revision: `378b820f1c00d511565ab9990ae03de77908b373`.
* License: upstream BSD-style 3-clause; full copyright, conditions and disclaimer
  retained verbatim at `third_party/angle/LICENSE`.
* Intended use: build and embed upstream EGL/GLES libraries and use upstream API
  headers. Top-level terms permit source/binary redistribution subject to notices
  and no endorsement. No upstream source modifications.
* Actual upstream build and ELF dependency closure passed in CI run 39495542750;
  the debug APK packages the uniquely suffixed EGL/GLES libraries. Runtime
  initialization/device rendering remains unverified.
* Dependencies: exact versions are pinned by that ANGLE revision's DEPS. Build
  script collects license and README.chromium texts with an index into APK assets.
  **All compiled dependency terms and attribution must still be reviewed against
  the actual built/link closure before public distribution.** Do not infer full
  dependency permission from ANGLE's top-level license alone.
* App links to Android platform APIs/libraries; no vendor GPU driver is distributed.

## Dynarmic — CPU correctness integration

* Origin: https://github.com/lioncash/dynarmic
* Exact revision: `a41c380246d3d9f9874f0f792d234dc0cc17c180` (6.7.0).
* Root license: 0BSD, read and preserved in `third_party/dynarmic/LICENSE.txt`.
* Original PodNative adapter; no inherited HyperHLE CPU wrapper. A reviewed Oaknut
  allocator patch adds opt-in checked W^X transitions and MAP_FAILED handling;
  provenance, modifications and retained MIT terms are documented in DEPENDENCIES.md.
* CPU and dependency licenses were read before build. All included components have
  permissive terms allowing the intended use with retained notices. Exact dependency
  tree hashes and host applicability: `third_party/dynarmic/DEPENDENCIES.md`.
* fmt, mcl, oaknut, robin-map, Zycore and Zydis: MIT; xbyak: BSD-3-Clause;
  Boost headers: BSL-1.0. Exact texts retained under `third_party/dynarmic/notices/`.
* Boost 1.88.0 pin: `199ef13d6034c85232431130142159af3adfce22`, with modules
  fixed by its gitlinks. These are build/test source checkouts, not vendored code.
* Used for actual ARM/Thumb host correctness tests. Android compilation and device
  execution are separate gates. Not yet linked into the application or used for games.

## Research only, not redistributed or linked

HyperHLE-Fork `5b65054c55cc33c6095f8c4b771430da5e63fbf4` (MPL-2.0):
README/manifest/license research; no code copied.
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

## JVM test dependencies (not shipped in the APK)

JUnit 4.13.2 (junit-team/junit4, tag r4.13.2), Eclipse Public License 1.0;
license retained in `third_party/licenses/junit/LICENSE-junit.txt`.
Its transitive Hamcrest Core 1.3 dependency is BSD-3-Clause; both are used
only by Gradle local unit tests. Neither is a runtime implementation/dependency.
