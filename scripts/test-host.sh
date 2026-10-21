#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/tests
FLAGS=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp)
build() { c++ "${FLAGS[@]}" "$@" ; }
build tests/macho_test.cpp app/src/main/cpp/macho.cpp -o .local/tests/macho-test
build tests/runtime_test.cpp app/src/main/cpp/macho.cpp \
  app/src/main/cpp/runtime/{memory,loader,launch,dyld,exports,services,frameworks,coregraphics,libc,cxx,fs,stdio,audio,format,heap,objc}.cpp \
  -o .local/tests/runtime-test
build tests/imports_test.cpp app/src/main/cpp/{macho,macho_info}.cpp -o .local/tests/imports-test
build tests/fat_test.cpp app/src/main/cpp/macho_container.cpp -o .local/tests/fat-test
build tests/frameworks_test.cpp app/src/main/cpp/runtime/{memory,frameworks}.cpp -o .local/tests/frameworks-test
build tests/coregraphics_test.cpp app/src/main/cpp/runtime/{memory,frameworks,coregraphics}.cpp -o .local/tests/coregraphics-test
build tests/dyld_test.cpp app/src/main/cpp/runtime/{memory,dyld,exports}.cpp -o .local/tests/dyld-test
build tests/exports_test.cpp \
  app/src/main/cpp/runtime/{memory,exports,services,frameworks,coregraphics,dyld,libc,cxx,fs,stdio,audio,format,heap,objc}.cpp \
  -o .local/tests/exports-test
# Host-visible units must also survive the Xcode/NDK warning set used for the app.
for unit in app/src/main/cpp/runtime/*.cpp; do
  case "$unit" in *cpu.cpp) continue ;; esac  # needs the pinned Dynarmic headers.
  c++ -std=c++20 -Wall -Wextra -Werror -DPODNATIVE_CPU_IN_APP=1 -Iapp/src/main/cpp \
    -fsyntax-only "$unit"
done
build tests/objc_test.cpp app/src/main/cpp/runtime/{memory,objc,exports,format,libc,heap}.cpp -o .local/tests/objc-test
build tests/libc_test.cpp app/src/main/cpp/runtime/{memory,libc,format,heap}.cpp -o .local/tests/libc-test
build tests/cxx_test.cpp app/src/main/cpp/runtime/{memory,cxx,format,heap}.cpp -o .local/tests/cxx-test
build tests/audio_test.cpp app/src/main/cpp/runtime/{memory,audio,format,libc,heap}.cpp -o .local/tests/audio-test
build tests/fs_test.cpp app/src/main/cpp/runtime/{memory,fs,stdio,audio,libc,format,heap}.cpp -o .local/tests/fs-test
for test in macho runtime imports fat frameworks coregraphics dyld exports libc cxx audio fs objc; do .local/tests/$test-test; done
python3 -m unittest discover -s tests -p "test_*.py"
python3 -m py_compile scripts/*.py tests/*.py
git diff --check
