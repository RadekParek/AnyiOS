#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v cmake >/dev/null
scripts/prepare-cpu.sh
cmake -S tests -B .local/cpu-tests \
  -DDYNARMIC_ROOT="$PWD/.local/deps/dynarmic" \
  -DBOOST_ROOT="$PWD/.local/boost-headers" -DBoost_NO_BOOST_CMAKE=ON \
  -DCMAKE_POLICY_DEFAULT_CMP0167=OLD -DCMAKE_POLICY_DEFAULT_CMP0144=NEW \
  -DCMAKE_EXE_LINKER_FLAGS="${CMAKE_EXE_LINKER_FLAGS:-}" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .local/cpu-tests --parallel "${CPU_BUILD_JOBS:-2}"
ctest --test-dir .local/cpu-tests --output-on-failure
