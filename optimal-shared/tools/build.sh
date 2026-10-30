#!/usr/bin/env bash
# build.sh — compile the C++ host tool tools/gen-imports (native twin of the .py).
#   ./tools/build.sh                 # -> tools/gen-imports (host binary)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXX="${CXX:-c++}"
"$CXX" -std=c++20 -O2 -o "$HERE/gen-imports" "$HERE/gen-imports.cpp"
echo "built -> $HERE/gen-imports"
