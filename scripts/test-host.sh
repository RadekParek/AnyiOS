#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/tests
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/macho_test.cpp app/src/main/cpp/macho.cpp -o .local/tests/macho-test
.local/tests/macho-test
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/runtime_test.cpp app/src/main/cpp/macho.cpp app/src/main/cpp/runtime/{memory,loader,launch}.cpp \
  -o .local/tests/runtime-test
.local/tests/runtime-test
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/imports_test.cpp app/src/main/cpp/{macho,macho_info}.cpp -o .local/tests/imports-test
.local/tests/imports-test
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/fat_test.cpp app/src/main/cpp/macho_container.cpp -o .local/tests/fat-test
.local/tests/fat-test
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iapp/src/main/cpp \
  tests/frameworks_test.cpp app/src/main/cpp/runtime/{memory,frameworks}.cpp -o .local/tests/frameworks-test
.local/tests/frameworks-test
python3 -m unittest discover -s tests -p "test_*.py"
python3 -m py_compile scripts/*.py tests/*.py
# Whitespace/conflict check is intentionally separate from compile and runtime tests.
git diff --check
