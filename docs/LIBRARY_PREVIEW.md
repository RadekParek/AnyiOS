# Library and startup preview (0.2.1)

The user reported that the previous APK's 3D test works perfectly on their
Galaxy S23 Ultra. This is user-reported graphics evidence, not gameplay evidence
or a device test of this new library/UI.

## Imports

- Add an IPA using Android's document picker, or select an extracted game folder
  (the `.app` bundle itself or a containing directory with one identifiable bundle).
- The full IPA or selected folder is copied into app-private storage. Resources
  and assets are retained; original files are never changed. Imports survive app
  restarts. Rename and remove affect only the library/private copy.
- Mach-O universal FAT32/FAT64 containers, either byte order, are inspected for an
  ARM32 slice. Thin ARM32 is also accepted. ARM64-only binaries are rejected.
  This refers to Mach-O containers, not the storage volume's filesystem.
- A bounded XML/binary Info.plist reader supplies names, versions, executable
  names and optional loose PNG artwork. Compiled asset catalogs and Apple's
  optimized CgBI PNGs may have no preview; a monogram is used instead.
- Encrypted ARM32 binaries can be retained, but are explicitly marked encrypted.
  There is no decryption, protection bypass or compatibility claim.
- **Attempt launch · experimental** reopens the retained executable, selects its
  ARM32 slice and invokes the native loader. The first blocker is saved with the
  entry, available under Latest launch report, included in report export and logged
  in Diagnostics. This is startup validation, **not guest code execution**: CPU
  integration and required iOS frameworks/audio/input remain unfinished. The real
  ANGLE/Vulkan graphics test remains independently available under Diagnostics.

## Bounds and failure behavior

There is **no fixed total-byte import quota**, including for folders above 2/4 GiB,
and no fixed library entry count. Copying is streamed with checked 64-bit byte
accounting; available private storage and Android/provider/filesystem limits apply.
The 64 MiB storage reserve remains a low-space safeguard, not a per-game size cap.
Parser/resource safety limits remain: 30,000 tree documents/ZIP entries,
32 tree levels, 512 MiB executable container, 64 MiB selected ARM32 slice,
1 MiB metadata, 64 FAT records; folder bundle discovery is bounded to depth 4.
64 MiB free-space reserve is checked during copying. Duplicate/unsafe file names,
ambiguous bundles and invalid slice ranges fail the import. Invalid metadata is
reported; when possible, executable-magic discovery supplies a fallback.
Stored native reports are limited to 128,000 characters with an explicit note.
New data is staged and published only after inspection. Previous entries remain
on failure; interrupted staging is removed on the next process start.

Imports run off the UI thread and survive Activity recreation, **not process
termination**. Cancellation is cooperative; a blocked provider read cannot be
immediately interrupted. Use locally available files for large imports. Android
uninstall/clear-storage deletes app-private library copies; keep originals.
No proprietary games or artwork belong in this repository or CI.

## UI and installation

Library cards, details/import sheets, saved system/light/dark appearance, render
resolution and frame-limit settings, diagnostics/report export, and original
adaptive/themed launcher artwork are included. New UI needs physical-device
review; no screenshots or visual-test claims are made.

CI APKs are development builds, not stable signed releases. The original preview
used an ephemeral CI debug certificate. Android may refuse an in-place update
with a different signing certificate: back up anything needed before uninstalling
an older preview, as uninstall deletes private copies/settings. Original source
files are untouched. CI now reuses a debug keystore **while its cache exists**;
cache eviction can still change it. Durable signed updates require the existing
release-signing secrets configuration. No private key is committed or published.

## Loader follow-up from the supplied report

The user’s report confirms ARMv7 selection from a universal binary and exposes
`LOAD_COMMAND_UNSUPPORTED: 0xb`. The loader now validates `LC_DYSYMTAB`: exact
command length, duplicates, LC_SYMTAB presence, non-overlapping bounded symbol
partitions, six linkedit table ranges and indirect-symbol indices/special flags.
It marks the image as requiring dynamic linking; **this does not implement binding**.
No proprietary binary, symbol dump or supplied report has been copied into Git.
