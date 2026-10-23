#!/usr/bin/env bash
# Fetch proven CPU components at exact revisions; never modifies the PodNative git branch.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/.local/deps"
mkdir -p "$DEPS"
fetch_pin() {
  local url=$1 dest=$2 revision=$3
  if [[ ! -d "$dest/.git" ]]; then
    git init "$dest"
    # Git for Windows may otherwise check out CRLF files, which makes the
    # reviewed LF patch fail to apply even when the pinned source is identical.
    git -C "$dest" config core.autocrlf false
    git -C "$dest" config core.eol lf
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch --depth 1 origin "$revision"
    git -C "$dest" checkout --detach FETCH_HEAD
  fi
  [[ $(git -C "$dest" rev-parse HEAD) == "$revision" ]] || {
    echo "Dependency revision mismatch in $dest; refusing to overwrite existing sources" >&2
    exit 1
  }
}
fetch_pin https://github.com/lioncash/dynarmic.git "$DEPS/dynarmic" "$(cat "$ROOT/third_party/dynarmic/revision.txt")"
fetch_pin https://github.com/boostorg/boost.git "$DEPS/boost" "$(cat "$ROOT/third_party/dynarmic/boost-revision.txt")"
PATCH="$ROOT/third_party/dynarmic/patches/oaknut-wx.patch"
# Some Windows Git configurations still materialize this pinned file as CRLF.
# Ignore whitespace differences only for this reviewed patch against the exact pin.
PATCH_FLAGS=(--ignore-space-change --ignore-whitespace)
if git -C "$DEPS/dynarmic" apply "${PATCH_FLAGS[@]}" --check "$PATCH" 2>/dev/null; then
  git -C "$DEPS/dynarmic" apply "${PATCH_FLAGS[@]}" "$PATCH"
elif git -C "$DEPS/dynarmic" apply "${PATCH_FLAGS[@]}" --reverse --check "$PATCH" 2>/dev/null; then
  echo 'Reviewed Oaknut W^X patch already applied'
else
  echo 'CPU dependency differs from the reviewed patch; refusing to overwrite it' >&2
  git -C "$DEPS/dynarmic" apply "${PATCH_FLAGS[@]}" --check "$PATCH" || true
  git -C "$DEPS/dynarmic" apply "${PATCH_FLAGS[@]}" --reverse --check "$PATCH" || true
  exit 1
fi
cd "$DEPS/boost"
git submodule update --init --depth 1 tools/boostdep libs/icl libs/variant
python3 tools/boostdep/depinst/depinst.py --exclude test --git_args '--depth 1' icl
python3 tools/boostdep/depinst/depinst.py --exclude test --git_args '--depth 1' variant
python3 "$ROOT/scripts/link-boost-headers.py" "$DEPS/boost" "$ROOT/.local/boost-headers/boost"
