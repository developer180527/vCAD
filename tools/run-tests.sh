#!/usr/bin/env bash
# One command to run everything. The pre-commit gate and what CI runs.
#
# Tiers, cheapest first, so a failure surfaces as early as possible:
#   1. layering    — structural rule, milliseconds
#   2. C++ unit    — kernel/naming/units internals (Catch2)
#   3. Rust accept — document, recompute, ABI (the boundary plugins will use)
#   4. Rust props  — invariants over generated inputs
#
# Usage:  tools/run-tests.sh [--asan] [--quick]
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD="${CAD_BUILD_DIR:-$ROOT/build}"
QUICK=0
ASAN=0
for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=1 ;;
    --asan)  ASAN=1; BUILD="$ROOT/build-asan" ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

step "Layering"
python3 tools/check_layering.py .

step "Build (${BUILD##*/})"
if [ ! -d "$BUILD" ]; then
  echo "No build directory at $BUILD. Configure it first — see README.md." >&2
  exit 1
fi
cmake --build "$BUILD" -j

step "C++ unit and acceptance tests"
if [ "$ASAN" = 1 ]; then
  ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "$BUILD" --output-on-failure
else
  ctest --test-dir "$BUILD" --output-on-failure
fi

step "Rust acceptance tests (through the C ABI)"
export CAD_BUILD_DIR="$BUILD"
cd tests-rs
cargo test --test m2_recompute

if [ "$QUICK" = 0 ]; then
  step "Rust property tests"
  cargo test --test prop_invariants
else
  echo "(skipped by --quick)"
fi

printf '\n\033[1;32mAll tiers passed.\033[0m\n'
