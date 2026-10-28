#!/usr/bin/env bash
# Initialize pinned AnyiOSEMU engine sources and apply reviewed compatibility patches.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNTIME_ROOT="$ROOT/third_party/anyiose-engine"
PIN_FILE="$ROOT/third_party/anyiose-runtime-revision.txt"
PATCH="$ROOT/third_party/anyiose-runtime-patches/AnyiOSEMU.patch"
SDL="$RUNTIME_ROOT/vendor/SDL"
SDL_PATCH="$ROOT/third_party/anyiose-runtime-patches/SDL-Android-NDK-27.patch"

[[ -f "$PIN_FILE" ]] || { echo "Missing engine revision pin: $PIN_FILE" >&2; exit 1; }
PIN=$(tr -d '\r\n' < "$PIN_FILE")
[[ "$PIN" =~ ^[0-9a-f]{40}$ ]] || {
  echo "Invalid AnyiOSEMU engine revision: $PIN" >&2
  exit 1
}

git -C "$ROOT" submodule update --init --recursive third_party/anyiose-engine
ACTUAL=$(git -C "$RUNTIME_ROOT" rev-parse HEAD)
[[ "$ACTUAL" == "$PIN" ]] || {
  echo "AnyiOSEMU engine revision mismatch: expected $PIN, found $ACTUAL" >&2
  exit 1
}
[[ -f "$PATCH" ]] || { echo "Missing reviewed engine patch: $PATCH" >&2; exit 1; }

# Recognize only exact previously reviewed engine patches.
"$ROOT/scripts/apply-reviewed-patch.sh" "$RUNTIME_ROOT" "$PATCH" \
  c0db49133c87ba9b7ba643b2a18d9c0e688792bd588a323301a77ab548971ae7,e6d5e28137a64151125324d6e929eaab9a54ac4b3018217247a2477b514155c3,bd12e23b8bb0cf5bc00c786302dee711342c824458dbea1a396b2c5792dabcc1,6dd3b0162626382c5d0f18eb4609ec27c114230ed5e6d0c36a8f94ee6e4b26ef,bb1ce4643029e8e22a0abf84a66d22ec064d8556f7f145f6401060be95d40927 "AnyiOSEMU engine"

SUBMODULE_STATUS=$(git -C "$RUNTIME_ROOT" submodule status --recursive)
if printf '%s\n' "$SUBMODULE_STATUS" | grep -Eq '^[-+U]'; then
  echo "AnyiOSEMU contains missing or mismatched nested submodules:" >&2
  printf '%s\n' "$SUBMODULE_STATUS" >&2
  exit 1
fi
[[ -f "$SDL_PATCH" ]] || { echo "Missing reviewed SDL compatibility patch: $SDL_PATCH" >&2; exit 1; }

"$ROOT/scripts/apply-reviewed-patch.sh" "$SDL" "$SDL_PATCH" \
  b7aa90b2df574c765c5f1fb4b189d3b9c3697940b06ff2123fc3c056c579419b,3d2ebd14a784066e5353997f40fbc48e764ace20d4897c6e9d162e0112100b14 "SDL Android"
required_files=(
  LICENSE Cargo.lock
  vendor/SDL/LICENSE.txt
  vendor/dynarmic/LICENSE.txt vendor/dynarmic/README.md
  vendor/openal-soft/COPYING vendor/libxml2/Copyright vendor/stb/LICENSE
  vendor/PVRTDecompress/LICENSE.md vendor/symphonia-codec-aac/LICENSE
  touchHLE_dylibs/README.md touchHLE_dylibs/COPYING.libgcc
  touchHLE_dylibs/COPYING.libsqlite3 touchHLE_dylibs/COPYING.libstdcxx
  touchHLE_dylibs/COPYING.libz
  touchHLE_fonts/README.md touchHLE_fonts/LICENSE.liberation
  touchHLE_fonts/LICENSE.noto
)
for path in "${required_files[@]}"; do
  [[ -f "$RUNTIME_ROOT/$path" ]] || {
    echo "Pinned AnyiOSEMU source is missing a required runtime notice: $path" >&2
    exit 1
  }
done
git -C "$RUNTIME_ROOT" diff --check --ignore-submodules=all
git -C "$SDL" diff --check
