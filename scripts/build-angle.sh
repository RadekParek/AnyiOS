#!/usr/bin/env bash
# Requires Linux, depot_tools and Chromium's download hosts; not a native GLES wrapper.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=$(cat "$ROOT/third_party/angle/revision.txt")
WORK=${ANGLE_WORK_DIR:-"$ROOT/.local/angle-build"}
export DEPOT_TOOLS_UPDATE=0
command -v gclient >/dev/null || { echo 'Install depot_tools first (see docs/ANGLE_INTEGRATION.md)' >&2; exit 1; }
TOOL_REV=$(git -C "$(dirname "$(command -v gclient)")" rev-parse HEAD)
[[ "$TOOL_REV" == "$(cat "$ROOT/third_party/angle/depot-tools-revision.txt")" ]] || {
  echo 'Use the pinned tool checkout from scripts/bootstrap-angle-tools.sh' >&2
  exit 1
}
mkdir -p "$WORK"
cd "$WORK"
if [[ ! -f .gclient ]]; then
  gclient config --name angle https://github.com/google/angle.git
  printf "\ntarget_os = ['android']\n" >> .gclient
fi
gclient sync --no-history --revision "angle@$REV"
cd angle
[[ $(git rev-parse HEAD) == "$REV" ]]
# Upstream Chromium dependencies may require distro packages. CI runs this on a disposable runner.
if [[ ${ANGLE_INSTALL_DEPS:-0} == 1 ]]; then
  sudo ./build/install-build-deps.sh --android --no-prompt
fi
mkdir -p out/PodNative
cat > out/PodNative/args.gn <<'ARGS'
target_os = "android"
target_cpu = "arm64"
arm_control_flow_integrity = "none"
is_component_build = false
is_debug = false
symbol_level = 1
angle_assert_always_on = true
angle_libs_suffix = "_podnative"
angle_enable_vulkan = true
angle_enable_gl = false
angle_enable_null = false
angle_enable_swiftshader = false
angle_enable_vulkan_validation_layers = false
use_remoteexec = false
ARGS
gn gen out/PodNative --fail-on-unused-args
autoninja -C out/PodNative libEGL libGLESv2
DEST="$ROOT/third_party/angle/lib/arm64-v8a"
mkdir -p "$DEST" "$ROOT/third_party/angle/include" "$ROOT/app/src/main/assets/angle-notices"
cp -a include/. "$ROOT/third_party/angle/include/"
# Non-component builds fold ordinary dependencies into the two ANGLE libraries.
# The closure audit below fails if this assumption changes upstream.
cp out/PodNative/libEGL_podnative.so out/PodNative/libGLESv2_podnative.so "$DEST/"
printf '%s\n' "$REV" > "$ROOT/app/src/main/assets/angle-revision.txt"
# Preserve all checkout license texts conservatively, including transitive dependencies.
python3 "$ROOT/scripts/collect-angle-notices.py" "$PWD" "$ROOT/app/src/main/assets/angle-notices"
python3 "$ROOT/scripts/check-native-deps.py" "$DEST"
