#!/usr/bin/env bash
# Additional upstream converter CPU, isolated from the emulator's reviewed engine.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
src="$ROOT/.local/converter-dynarmic"
pin=$(cat "$ROOT/third_party/converter-runtime/dynarmic-revision.txt")
if [[ ! -d "$src/.git" ]]; then
  git init "$src"
  git -C "$src" remote add origin https://github.com/suyu-emu/dynarmic.git
  git -C "$src" fetch --depth 1 origin "$pin"
  git -C "$src" checkout --detach FETCH_HEAD
fi
[[ $(git -C "$src" rev-parse HEAD) == "$pin" ]] || { echo 'Converter CPU revision mismatch'; exit 1; }
cmp "$src/LICENSE.txt" "$ROOT/third_party/converter-runtime/dynarmic-LICENSE.txt"
git -C "$src" submodule update --init --depth 1 \
  externals/mcl externals/fmt externals/xbyak externals/oaknut externals/robin-map \
  externals/zydis externals/zycore
# Upstream packaging defect: remove only its known forwarding placeholder.
stub="$src/externals/mcl/include/boost/variant.hpp"
if [[ -f "$stub" ]] && grep -Eq 'cpm|C:/Users|CPM' "$stub"; then rm "$stub"; fi
notices="$ROOT/converter/app/src/main/assets/converter-notices"
mkdir -p "$notices"
cp "$src/LICENSE.txt" "$notices/dynarmic.txt"
cp "$ROOT/third_party/converter-runtime/dynarmic-revision.txt" "$notices/revision.txt"
for dependency in mcl fmt xbyak oaknut robin-map zydis zycore; do
  directory="$src/externals/$dependency"
  git -C "$src" rev-parse "HEAD:externals/$dependency" > "$notices/$dependency-revision.txt"
  found=0
  while IFS= read -r -d '' text; do
    cp "$text" "$notices/$dependency-$(basename "$text").txt"
    found=1
  done < <(find "$directory" -maxdepth 2 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' -o -iname 'copyright*' \) -print0)
  [[ $found == 1 ]] || { echo "Missing license for $dependency"; exit 1; }
done
printf '%s\n' "Converter Dynarmic prepared at $pin"
if [[ -n ${GITHUB_ENV:-} ]]; then printf 'RADEK_DYNARMIC_SOURCE_DIR=%s\n' "$src" >> "$GITHUB_ENV"; fi
