#!/usr/bin/env bash
# Patch the pinned rust-sdl2 SDL submodule used by Cargo's bundled Android build.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNTIME_ROOT="$ROOT/third_party/anyiose-engine"
PATCH="$ROOT/third_party/anyiose-runtime-patches/SDL-Android-NDK-27.patch"

[[ -f "$PATCH" ]] || { echo "Missing reviewed SDL compatibility patch: $PATCH" >&2; exit 1; }

# Cargo builds sdl2-sys/SDL from its own git checkout, not the runtime submodule's vendor/SDL.
cargo fetch --locked --manifest-path "$RUNTIME_ROOT/Cargo.toml"
METADATA=$(cargo metadata --locked --format-version 1 --manifest-path "$RUNTIME_ROOT/Cargo.toml")
PACKAGE_INFO=$(printf '%s\n' "$METADATA" | python3 -c '
import json
import sys
import tomllib

metadata = json.load(sys.stdin)
with open(sys.argv[1], "rb") as lock_file:
    lock = tomllib.load(lock_file)
locked = [
    package for package in lock["package"]
    if package["name"] == "sdl2-sys" and package["version"] == "0.37.0"
]
packages = [
    package for package in metadata["packages"]
    if package["name"] == "sdl2-sys" and package["version"] == "0.37.0"
]
if len(locked) != 1 or len(packages) != 1:
    raise SystemExit("Expected one locked sdl2-sys 0.37.0 package")
if locked[0].get("source") != packages[0].get("source"):
    raise SystemExit("Cargo metadata does not match the locked sdl2-sys source")
source = packages[0].get("source") or ""
if not source.startswith("git+https://github.com/touchHLE/rust-sdl2?tag=touchHLE-3#"):
    raise SystemExit(f"Unexpected sdl2-sys source: {source}")
print("{}\t{}".format(packages[0]["manifest_path"], source))
' "$RUNTIME_ROOT/Cargo.lock")
IFS=$'\t' read -r S2_MANIFEST S2_SOURCE <<< "$PACKAGE_INFO"
RUST_SDL2=$(cd "$(dirname "$S2_MANIFEST")/.." && pwd)
SDL="$RUST_SDL2/sdl2-sys/SDL"
EXPECTED_RUST_SDL2_REV=${S2_SOURCE##*#}
ACTUAL_RUST_SDL2_REV=$(git -C "$RUST_SDL2" rev-parse HEAD)
[[ "$ACTUAL_RUST_SDL2_REV" == "$EXPECTED_RUST_SDL2_REV" ]] || {
  echo "rust-sdl2 revision mismatch: expected $EXPECTED_RUST_SDL2_REV, found $ACTUAL_RUST_SDL2_REV" >&2
  exit 1
}
EXPECTED_SDL_REV=$(git -C "$RUST_SDL2" ls-tree HEAD sdl2-sys/SDL | awk '{print $3}')
ACTUAL_SDL_REV=$(git -C "$SDL" rev-parse HEAD)
VENDORED_SDL_REV=$(git -C "$RUNTIME_ROOT/vendor/SDL" rev-parse HEAD)
if [[ -z "$EXPECTED_SDL_REV" || "$ACTUAL_SDL_REV" != "$EXPECTED_SDL_REV" || "$ACTUAL_SDL_REV" != "$VENDORED_SDL_REV" ]]; then
  echo "rust-sdl2 SDL submodule does not match the pinned runtime SDL revision" >&2
  exit 1
fi

# Cargo's cached checkout may still contain the previously reviewed SDL patch.
"$ROOT/scripts/apply-reviewed-patch.sh" "$SDL" "$PATCH" \
  b7aa90b2df574c765c5f1fb4b189d3b9c3697940b06ff2123fc3c056c579419b,3d2ebd14a784066e5353997f40fbc48e764ace20d4897c6e9d162e0112100b14 "Cargo rust-sdl2 SDL"
