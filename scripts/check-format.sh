#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
clang-format --dry-run --Werror app/src/main/cpp/*.{cpp,h} app/src/main/cpp/runtime/*.{cpp,h} tests/*.cpp app/src/main/java/org/podnative/*.java
ruff check scripts/*.py tests/*.py
ruff format --check scripts/*.py tests/*.py
