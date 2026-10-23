#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v cmake >/dev/null
read -r _ _ CMAKE_VERSION < <(cmake --version)
CMAKE_MAJOR_VERSION=${CMAKE_VERSION%%.*}
CMAKE_POLICY_ARGS=()
if [[ "$CMAKE_MAJOR_VERSION" =~ ^[0-9]+$ ]] && (( CMAKE_MAJOR_VERSION >= 4 )); then
  # The pinned robin-map dependency still declares compatibility with CMake 3.0.
  CMAKE_POLICY_ARGS+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
fi
scripts/prepare-cpu.sh
cmake -S tests -B .local/cpu-tests \
  -DDYNARMIC_ROOT="$PWD/.local/deps/dynarmic" \
  -DBOOST_ROOT="$PWD/.local/boost-headers" -DBoost_NO_BOOST_CMAKE=ON \
  -DCMAKE_POLICY_DEFAULT_CMP0167=OLD -DCMAKE_POLICY_DEFAULT_CMP0144=NEW \
  "${CMAKE_POLICY_ARGS[@]}" \
  -DCMAKE_EXE_LINKER_FLAGS="${CMAKE_EXE_LINKER_FLAGS:-}" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .local/cpu-tests --parallel "${CPU_BUILD_JOBS:-2}"
ctest --test-dir .local/cpu-tests --output-on-failure
