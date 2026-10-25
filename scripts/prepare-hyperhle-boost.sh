#!/usr/bin/env bash
# Fetch the exact Boost release required to build the pinned AnyiOSEMU Dynarmic wrapper.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HLE="$ROOT/third_party/hyperhle"
BOOST_DIR="$HLE/vendor/boost"
VERSION=$(tr -d '\r\n' < "$ROOT/third_party/hyperhle-boost-version.txt")
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Invalid AnyiOSEMU Boost version: $VERSION" >&2
  exit 1
}
[[ -d "$HLE/vendor/dynarmic" ]] || {
  echo "Pinned AnyiOSEMU submodules are missing; run scripts/prepare-hyperhle.sh first" >&2
  exit 1
}
IFS=. read -r major minor patch <<< "$VERSION"
BOOST_NUM=$((major * 100000 + minor * 100 + patch))
BOOST_UNDERSCORE=${VERSION//./_}

check_version() {
  local header=$1
  [[ -f "$header" ]] && grep -Eq "^#define BOOST_VERSION[[:space:]]+$BOOST_NUM$" "$header"
}
if [[ -d "$BOOST_DIR" ]]; then
  if check_version "$BOOST_DIR/boost/version.hpp"; then
    echo "Boost $VERSION is already prepared for AnyiOSEMU"
    exit 0
  fi
  echo "Existing AnyiOSEMU Boost directory has the wrong or incomplete version: $BOOST_DIR" >&2
  exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/anyiose-boost.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
ARCHIVE="$TMP/boost_${BOOST_UNDERSCORE}.tar.gz"
URL="https://archives.boost.io/release/$VERSION/source/boost_${BOOST_UNDERSCORE}.tar.gz"
curl --fail --location --retry 3 --retry-delay 2 --silent --show-error "$URL" -o "$ARCHIVE"
tar -xzf "$ARCHIVE" -C "$TMP"
EXTRACTED="$TMP/boost_${BOOST_UNDERSCORE}"
if ! check_version "$EXTRACTED/boost/version.hpp"; then
  echo "Downloaded Boost archive does not match pinned version $VERSION" >&2
  exit 1
fi
mv "$EXTRACTED" "$BOOST_DIR"
echo "Prepared Boost $VERSION at third_party/hyperhle/vendor/boost"
