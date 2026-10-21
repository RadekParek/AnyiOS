# Converter fork review — 2026-10-21

## Repositories and pinned evidence

- Owner upstream: [RadekParek/RadekiOSconventor](https://github.com/RadekParek/RadekiOSconventor), reviewed current `main` **e20c76f33fec87bb311711074c219c3fd32f1911** (the source integrated into AnyiOS).
- Sole public fork returned by GitHub: [Gitsnup/iOStoDroid](https://github.com/Gitsnup/iOStoDroid), reviewed head **fc08f802fb3a8f0b884bcb1ceb14672baef4c595**.
- Common ancestor: **f04756d4159627906075ba8166311336fae2412c**.
- GitHub comparison at review time: **3 fork-only commits, 72 upstream-only commits**. Counts can change after this review.

Fork-only commits:

1. `2dc35cc0c63205fe8c9fa2e920f3840fce178e78` — “Rebrand RadekiOSConventor to iOStoDroid”.
2. `7a1421f2d9bf78d4ca1a5aa9117364770669c6ee` — temporary preservation of the upstream CI filename because the fork token lacked workflow-renaming scope.
3. `fc08f802fb3a8f0b884bcb1ceb14672baef4c595` — upstream merge with conflict resolutions.

## Method and findings

Downloaded the common ancestor and fork source trees, compared corresponding files after normalizing project/package/prefix renames, then manually inspected remaining differences. Comparing only current upstream against the older fork would incorrectly credit older upstream features to the fork.

- 60 files were byte-identical and 183 additional corresponding files differed only by the normalized branding substitutions.
- Remaining code change: the bootstrap stack frame grew from **32 to 48 bytes**. The renamed `iostodroid-gameboot` string is longer than `radek-gameboot`; 16 bytes of argv/env header plus the new NUL-terminated string no longer fit the old 32-byte frame. This is a necessary rebranding accommodation, not an independent runtime/API upgrade. AnyiOS preserves the short internal guest-bootstrap name and does not need this adjustment.
- Tests changed string lengths/file positions from the 5-character original brand to the 10-character new brand. A shim-table local identifier was renamed. Neither adds semantics.
- The changed synthetic `simple.ipa` contains the same executable; its Info.plist bundle identifier changed from `dev.radek.simpleipa` to `dev.iostodroid.simpleipa`. No fixture binary was copied into AnyiOS.
- The remaining workflow change is its display title; one build-cache comment was shortened.

## Recommendation

**No independent functional, conversion, renderer, performance or security improvement was identified to port.** Do not replace current upstream with this fork: that would also move behind 72 upstream commits. AnyiOS therefore integrates the owner's newer upstream source, not fork code. This is a source-diff finding, not a claim that either application has been device/gameplay validated.

The fork is independently published; this review does not infer the author's intent or make a licensing judgment. Upstream has no general root source-license declaration, and AnyiOS inclusion was explicitly requested by the owner. Existing third-party licenses still apply.
