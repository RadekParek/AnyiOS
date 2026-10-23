#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/tests .local/tests-es2
FLAGS=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp -Ithird_party/pvrtc)
# Every unit is an independent translation/compile, so they are built in parallel;
# HOST_TEST_JOBS caps the concurrency on small machines.
JOBS=${HOST_TEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
PLAN=$(mktemp)
trap 'rm -f "$PLAN"' EXIT
plan() { printf 'c++ %s\n' "$*" >> "$PLAN"; }
plan "${FLAGS[*]}" tests/macho_test.cpp app/src/main/cpp/macho.cpp -o .local/tests/macho-test
plan "${FLAGS[*]}" tests/runtime_test.cpp app/src/main/cpp/macho.cpp \
  app/src/main/cpp/runtime/{memory,loader,launch,dyld,exports,services,frameworks,coregraphics,libc,cxx,fs,stdio,audio,posix,openal,format,heap,objc,uikit,gles,pvrtc}.cpp \
  third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/runtime-test
plan "${FLAGS[*]}" tests/imports_test.cpp app/src/main/cpp/{macho,macho_info}.cpp -o .local/tests/imports-test
plan "${FLAGS[*]}" tests/fat_test.cpp app/src/main/cpp/macho_container.cpp -o .local/tests/fat-test
plan "${FLAGS[*]}" tests/frameworks_test.cpp app/src/main/cpp/runtime/{memory,frameworks}.cpp -o .local/tests/frameworks-test
plan "${FLAGS[*]}" tests/coregraphics_test.cpp app/src/main/cpp/runtime/{memory,frameworks,coregraphics}.cpp -o .local/tests/coregraphics-test
plan "${FLAGS[*]}" tests/dyld_test.cpp app/src/main/cpp/runtime/{memory,dyld,exports}.cpp -o .local/tests/dyld-test
plan "${FLAGS[*]}" tests/exports_test.cpp \
  app/src/main/cpp/runtime/{memory,exports,services,frameworks,coregraphics,dyld,libc,cxx,fs,stdio,audio,posix,openal,format,heap,objc,uikit,gles,pvrtc}.cpp \
  third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/exports-test
plan "${FLAGS[*]}" tests/objc_test.cpp app/src/main/cpp/runtime/{memory,objc,exports,format,libc,heap,gles,pvrtc}.cpp third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/objc-test
plan "${FLAGS[*]}" tests/libc_test.cpp app/src/main/cpp/runtime/{memory,libc,format,heap}.cpp -o .local/tests/libc-test
plan "${FLAGS[*]}" tests/cxx_test.cpp app/src/main/cpp/runtime/{memory,cxx,exports,format,heap}.cpp -o .local/tests/cxx-test
plan "${FLAGS[*]}" tests/uikit_test.cpp app/src/main/cpp/runtime/{memory,objc,exports,format,libc,heap,uikit,gles,pvrtc}.cpp third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/uikit-test
plan "${FLAGS[*]}" tests/gles_test.cpp app/src/main/cpp/runtime/{memory,heap,gles,pvrtc}.cpp third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/gles-test
plan "${FLAGS[*]}" tests/pvrtc_test.cpp app/src/main/cpp/runtime/pvrtc.cpp \
  third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/pvrtc-test
# The GL bridge is exercised against a recording driver: a shared object that answers the
# GLES/EGL entry points and keeps what it was passed. It renders nothing, so the test only
# claims marshalling, resource transitions and the frame flip — never a drawn frame.
plan "${FLAGS[*]} -Itests" tests/gles_stub_driver.cpp -shared -fPIC -o .local/tests/libGLESv2_podnative.so
plan "${FLAGS[*]} -Itests" tests/gles_driver_test.cpp app/src/main/cpp/runtime/{memory,heap,gles,pvrtc}.cpp third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/gles-driver-test
# A second double, in its own directory so both can carry the bundled soname: this one has no
# ES 1.1 entry points at all, which is the shape of ANGLE and of most current mobile drivers.
# It is what makes the fixed-function emulation testable without a GPU.
plan "${FLAGS[*]} -Itests" tests/gles_es2_stub_driver.cpp -shared -fPIC -o .local/tests-es2/libGLESv2_podnative.so
plan "${FLAGS[*]} -Itests" tests/gles_emulation_test.cpp app/src/main/cpp/runtime/{memory,heap,gles,pvrtc}.cpp third_party/pvrtc/PVRTDecompress.cpp -o .local/tests/gles-emulation-test
plan "${FLAGS[*]}" tests/audio_test.cpp app/src/main/cpp/runtime/{memory,audio,openal,format,libc,heap}.cpp -o .local/tests/audio-test
plan "${FLAGS[*]}" tests/fs_test.cpp app/src/main/cpp/runtime/{memory,fs,stdio,audio,libc,format,heap}.cpp -o .local/tests/fs-test
xargs -a "$PLAN" -d '\n' -P "$JOBS" -n 1 -I {} bash -c '{}'
# Development tools are compiled here too, so they cannot rot: the guest runner is the
# entry point the compatibility work is measured with, and the host GL harness is a
# diagnostic double that must keep matching the bridge's expectations.
c++ -std=c++20 -Wall -Wextra -Werror -DPODNATIVE_CPU_IN_APP=1 -Iapp/src/main/cpp \
  -fsyntax-only tools/guest_runner.cpp
c++ -std=c++20 -Wall -Wextra -Werror -Iapp/src/main/cpp -fsyntax-only tools/host_gles_driver.cpp
# Host-visible units must also survive the Xcode/NDK warning set used for the app.
# cpu.cpp is excluded: it needs the pinned Dynarmic headers, which the PC suite owns.
find app/src/main/cpp/runtime -maxdepth 1 -name '*.cpp' ! -name 'cpu.cpp' -print0 |
  xargs -0 -P "$JOBS" -n 1 -I {} c++ -std=c++20 -Wall -Wextra -Werror -DPODNATIVE_CPU_IN_APP=1 \
    -Iapp/src/main/cpp -Ithird_party/pvrtc -fsyntax-only {}
for test in macho runtime imports fat frameworks coregraphics dyld exports libc cxx audio fs objc uikit gles pvrtc; do .local/tests/$test-test; done
LD_LIBRARY_PATH=".local/tests${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  .local/tests/gles-driver-test libGLESv2_podnative.so
LD_LIBRARY_PATH=".local/tests-es2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  .local/tests/gles-emulation-test libGLESv2_podnative.so
python3 -m unittest discover -s tests -p "test_*.py"
python3 -m py_compile scripts/*.py tests/*.py
git diff --check
