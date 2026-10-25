#!/usr/bin/env bash
# Builds the native library and runs every test: the unit tests and, through degust, the
# examples and the programs in tests/programs. Arguments before a `--` go to the test binary,
# such as -v or --run NAME; those after it are the tests' own options, such as --update, which
# rewrites golden files: `./test.sh -v -- --update`.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

binary=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
  binary+=("$1")
  shift
done
[[ $# -gt 0 ]] && shift

"$ROOT/build.sh"
[[ -d packages/degust ]] || jelloc deps.get
JELLO="$(command -v jello)" || { echo "test.sh: jello is not on PATH" >&2; exit 1; }
jelloc test -- ${binary[@]+"${binary[@]}"} -- \
  --compiler "jelloc --path $ROOT/src" --vm "$JELLO" --jdll "$ROOT/jdll" "$@"
