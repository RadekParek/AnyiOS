#!/usr/bin/env bash
# Initialize the pinned AnyiOSEMU engine source and apply its reviewed source patch.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HLE="$ROOT/third_party/hyperhle"
PIN_FILE="$ROOT/third_party/hyperhle-revision.txt"
PATCH="$ROOT/third_party/hyperhle-patches/AnyiOSEMU.patch"

[[ -f "$PIN_FILE" ]] || { echo "Missing engine revision pin: $PIN_FILE" >&2; exit 1; }
PIN=$(tr -d '\r\n' < "$PIN_FILE")
[[ "$PIN" =~ ^[0-9a-f]{40}$ ]] || {
  echo "Invalid AnyiOSEMU engine revision: $PIN" >&2
  exit 1
}

git -C "$ROOT" submodule update --init --recursive third_party/hyperhle
ACTUAL=$(git -C "$HLE" rev-parse HEAD)
[[ "$ACTUAL" == "$PIN" ]] || {
  echo "AnyiOSEMU engine revision mismatch: expected $PIN, found $ACTUAL" >&2
  exit 1
}
[[ -f "$PATCH" ]] || { echo "Missing reviewed engine patch: $PATCH" >&2; exit 1; }

if git -C "$HLE" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "AnyiOSEMU source patch is already applied to $PIN"
elif git -C "$HLE" apply --check "$PATCH" >/dev/null 2>&1; then
  git -C "$HLE" apply "$PATCH"
  echo "Applied AnyiOSEMU source patch to $PIN"
else
  echo "AnyiOSEMU patch does not apply cleanly to pinned revision $PIN" >&2
  exit 1
fi

ACTUAL_PATCH=$(mktemp "${TMPDIR:-/tmp}/anyiose-patch.XXXXXX")
trap 'rm -f "$ACTUAL_PATCH"' EXIT
# Full object IDs keep this exact patch check stable across Git versions and
# shallow/full clones, whose default abbreviated hashes may differ in length.
git -C "$HLE" diff --binary --full-index HEAD > "$ACTUAL_PATCH"
if ! cmp -s "$PATCH" "$ACTUAL_PATCH"; then
  echo "Pinned engine worktree contains changes outside the reviewed AnyiOSEMU patch" >&2
  exit 1
fi

SUBMODULE_STATUS=$(git -C "$HLE" submodule status --recursive)
if printf '%s\n' "$SUBMODULE_STATUS" | grep -Eq '^[-+U]'; then
  echo "AnyiOSEMU contains missing or mismatched nested submodules:" >&2
  printf '%s\n' "$SUBMODULE_STATUS" >&2
  exit 1
fi
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
  [[ -f "$HLE/$path" ]] || {
    echo "Pinned AnyiOSEMU source is missing a required runtime notice: $path" >&2
    exit 1
  }
done
git -C "$HLE" diff --check
