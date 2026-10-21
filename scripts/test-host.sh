#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/tests
FLAGS=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp)
build() { c++ "${FLAGS[@]}" "$@" ; }
build tests/macho_test.cpp app/src/main/cpp/macho.cpp -o .local/tests/macho-test
build tests/runtime_test.cpp app/src/main/cpp/macho.cpp \
  app/src/main/cpp/runtime/{memory,loader,launch,dyld,exports,services,frameworks,libc,heap,objc}.cpp \
  -o .local/tests/runtime-test
build tests/imports_test.cpp app/src/main/cpp/{macho,macho_info}.cpp -o .local/tests/imports-test
build tests/fat_test.cpp app/src/main/cpp/macho_container.cpp -o .local/tests/fat-test
build tests/frameworks_test.cpp app/src/main/cpp/runtime/{memory,frameworks}.cpp -o .local/tests/frameworks-test
build tests/dyld_test.cpp app/src/main/cpp/runtime/{memory,dyld,exports}.cpp -o .local/tests/dyld-test
build tests/exports_test.cpp \
  app/src/main/cpp/runtime/{memory,exports,services,frameworks,dyld,libc,heap,objc}.cpp \
  -o .local/tests/exports-test
build tests/objc_test.cpp app/src/main/cpp/runtime/{memory,objc,exports}.cpp -o .local/tests/objc-test
build tests/libc_test.cpp app/src/main/cpp/runtime/{memory,libc,heap}.cpp -o .local/tests/libc-test
for test in macho runtime imports fat frameworks dyld exports libc objc; do .local/tests/$test-test; done
python3 -m unittest discover -s tests -p "test_*.py"
python3 -m py_compile scripts/*.py tests/*.py
git diff --check
