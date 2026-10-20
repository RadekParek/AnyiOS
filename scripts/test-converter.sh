#!/usr/bin/env bash
# All included synthetic converter tests; never downloads private game fixtures.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT/converter"
python3 tools/make_sample_ipa.py
python3 tools/make_hello_ipa.py
python3 tools/make_simple_ipa.py
python3 tools/build_native.py
python3 -m unittest discover -v
python3 -m unittest discover -s compat-runtime-v1/tests -v
python3 compat-runtime-v1/tools/smoke_db.py validate
