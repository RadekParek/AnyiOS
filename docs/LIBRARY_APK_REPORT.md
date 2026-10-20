# 0.2 library preview APK — 2026-10-20

[Download the signed debug APK archive](https://github.com/RadekParek/PodNative/actions/runs/39501999659/artifacts/12301256622).
Extract it and install `PodNative-debug.apk`. It also contains `SHA256SUMS` and
`BUILD-INFO.txt`. See the [signing/update caveat](LIBRARY_PREVIEW.md#ui-and-installation)
before replacing the older preview: a different certificate may require uninstall,
which deletes private copies/settings, not original source files.

## Verified build evidence

- Source: `430175539b1aea7a42f0342e6163ee844441a222`.
- Android [run 39501999659](https://github.com/RadekParek/PodNative/actions/runs/39501999659):
  **success**, 2m4s. Formatting, host regressions, real cached ANGLE dependency
  audit, Java/native build, Android lint, JVM tests, signing verification and
  artifact publication passed. ANGLE was reused from verified source-build outputs,
  not replaced with a system renderer.
- APK artifact: `12301256622`, archive size **6,937,535 bytes**.
- GitHub **archive** SHA-256 (not the APK digest):
  `34f10c86a9b91782a8f1716206fca3a652f794efa3f364e2f5e17cd73026d6f3`.
  The APK's own checksum is in the archive's `SHA256SUMS`.
- Build diagnostics artifact: `12301816206`.
- Runtime [run 39501999685](https://github.com/RadekParek/PodNative/actions/runs/39501999685):
  **success**. Host sanitizer/real Dynarmic execution tests and Android ARM64 CPU
  harness compilation passed. CPU execution inside the Android app remains pending.
- Tests cover FAT32/FAT64 endian/bounds/overlap/ARM64-only cases and 20,000 mutations,
  in addition to the prior 35,000 parser/loader mutations. JVM metadata tests cover
  XML/binary parsing, negative integers, external DTD isolation, malformed/deep/cyclic
  metadata, allocation amplification, 1,000 binary mutations and safe filename rules.

## What changed

Persistent private IPA/folder library; whole source/resources retained; real
rename/remove/report export; ARM32 slice selection from universal binaries;
original adaptive/themed launcher icon; library-first responsive native UI with
saved system/light/dark appearance and existing graphics settings/diagnostics.
Details and limits: [library preview](LIBRARY_PREVIEW.md).

The user reported that the **previous** graphics APK works perfectly on their
S23 Ultra. No device is connected here, and no screenshot, new library/device test,
new UI visual verification or gameplay result is claimed. Import is not launch:
iOS framework support and runtime integration remain unfinished.
