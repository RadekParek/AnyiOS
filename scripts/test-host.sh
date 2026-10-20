#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/tests
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/macho_test.cpp app/src/main/cpp/macho.cpp -o .local/tests/macho-test
.local/tests/macho-test
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/runtime_test.cpp app/src/main/cpp/macho.cpp app/src/main/cpp/runtime/{memory,loader}.cpp \
  -o .local/tests/runtime-test
.local/tests/runtime-test
python3 -m py_compile scripts/*.py
# Whitespace/conflict check is intentionally separate from compile and runtime tests.
git diff --check
