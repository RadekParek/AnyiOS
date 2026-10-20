# 0.2.1 startup preview — 2026-10-20

[Download the signed debug APK archive](https://github.com/RadekParek/PodNative/actions/runs/39504099819/artifacts/12301983317).
Extract and install `PodNative-debug.apk`; APK checksum and build provenance are
inside `SHA256SUMS` and `BUILD-INFO.txt`. Existing development signing cache was
available to the build. See LIBRARY_PREVIEW.md for debug-key/cache caveats; never
uninstall without retaining originals/anything you need from private storage.

## Changes

- No fixed 2 GiB import quota, including selected folders and full IPA copies.
  Streaming copies use checked 64-bit accounting. Library entry quota removed too.
  Actual free storage, the low-space reserve and provider/filesystem limits apply.
  File-count/depth, safe paths, executable/metadata allocation and malformed-input
  checks remain; this is not unlimited RAM or acceptance of arbitrary executable sizes.
- LC_DYSYMTAB now has bounded structural validation and is marked as requiring dyld.
  It is not ignored, and indirect binding/relocations are not claimed implemented.
- Enabled experimental startup attempts reopen the stored source and invoke the
  native loader off the UI thread. Latest launch report persists with the entry,
  is available in game details, and is included in report exports/Diagnostics.
  Interrupted launch scratch files use the existing private staging cleanup path.
- Import reports captured before this update remain historical. Use **Latest launch
  report** to inspect the current build's fresh startup result, not the cached import
  report that originally mentioned unsupported command 0xb.

## Verification

Source: `e3fbe9b9ec0d4cf46acc8c396dc180aac55ff2c0`.

Android [run 39504099819](https://github.com/RadekParek/PodNative/actions/runs/39504099819):
**success**, build job 2m3s. Formatting, sanitizer regressions, real cached ANGLE
input audit, Android lint, JVM tests, Java/native compilation, signing verification
and artifact publication passed. No new source ANGLE rebuild was needed.

Artifact `12301983317`, **archive** size 6,942,704 bytes; GitHub **archive** SHA-256:
`cb250d8b6d4b26bfb1c2e2accc4b5b77ca353a04fda4ef44510daf4379a7f596`.
This is not the APK digest; the latter is inside the archive.
Diagnostics artifact: `12302888025`.

Runtime [run 39504099775](https://github.com/RadekParek/PodNative/actions/runs/39504099775):
**success** for host CPU/sanitizer tests and Android ARM64 CPU harness cross-build.
The CPU harness is not guest execution inside this app.

New regressions exercise symbol partitions, all six LC_DYSYMTAB table bounds,
indirect-symbol indices/flags and truthful launch-blocker outcomes. JVM tests cover
accounting beyond 2/8 GiB and overflow rejection; **an actual multi-GiB device import
has not been tested here**. Existing 55,000 native malformed/mutated inputs pass.

## Remaining execution gap

This APK performs **startup validation, not guest code execution**. Dynamic linking,
CPU integration in the app, iOS frameworks, guest graphics semantics, audio and
input are unfinished. Games are not playable yet; see EXECUTION_NEXT.md. No physical
device/UI/import/launch test or screenshot is claimed for this build.
