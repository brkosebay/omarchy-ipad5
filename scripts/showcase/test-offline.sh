#!/usr/bin/env bash
# Host-only checks of the showcase payload and tooling; no device, no Docker.
#   scripts/showcase/test-offline.sh [--package]
# --package rebuilds build/showcase first (scripts/showcase/package.py).
# Lints with shellcheck when it is on PATH (or SHELLCHECK=/path/to/shellcheck).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
[ "${1:-}" = --package ] && python3 -B scripts/showcase/package.py
bash -n scripts/showcase/bring-up.sh
echo "PASS bash -n bring-up.sh"
sc=${SHELLCHECK:-$(command -v shellcheck || true)}
if [ -n "$sc" ]; then
	"$sc" -x scripts/showcase/bring-up.sh scripts/showcase/test-offline.sh
	echo "PASS shellcheck"
else
	echo "SKIP shellcheck (not installed)"
fi
python3 -B -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' scripts/showcase/device/a9-oc-hold.py
for test in test-package test-oc-hold test-fastfetch test-bring-up; do
	python3 -B "scripts/showcase/tests/$test.py" 2>&1 | tail -n 3
	echo "PASS $test"
done
