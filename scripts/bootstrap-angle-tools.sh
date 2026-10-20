#!/usr/bin/env bash
# Official depot_tools, pinned via Chromium DEPS; no unofficial binary/mirror.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=$(cat "$ROOT/third_party/angle/depot-tools-revision.txt")
DEST=${1:-"$ROOT/.local/depot_tools"}
if [[ ! -d "$DEST/.git" ]]; then
  mkdir -p "$DEST"
  # Refuse to take over a nonempty directory supplied by a developer.
  [[ -z $(ls -A "$DEST") ]] || { echo 'depot_tools target is not empty' >&2; exit 1; }
  git init "$DEST"
  git -C "$DEST" remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git
  git -C "$DEST" fetch --depth 1 origin "$REV"
  git -C "$DEST" checkout --detach FETCH_HEAD
fi
[[ $(git -C "$DEST" rev-parse HEAD) == "$REV" ]] || {
  echo 'depot_tools revision mismatch; refusing to overwrite another checkout' >&2
  exit 1
}
printf 'depot_tools source=%s\nlocation=%s\n' "$REV" "$DEST"
echo 'Add this location to PATH and set DEPOT_TOOLS_UPDATE=0.'
