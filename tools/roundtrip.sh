#!/bin/bash
# Semantic round-trip check for a real gfortran .mod file:
#
#   original.mod --[fxmod-cli emit-source]--> pass1.f90
#   pass1.f90    --[real gfortran]----------> regenerated.mod
#   regenerated.mod --[fxmod-cli emit-source]--> pass2.f90
#
# A byte-diff of original.mod vs regenerated.mod would be meaningless: the
# two were compiled from different source text (different "created from"
# path, likely different internal symbol numbering), so they will never
# match bit-for-bit even for a perfect translation. Comparing pass1.f90
# against pass2.f90 instead checks the thing that actually matters --
# whether anything was lost going through a real, independent compiler --
# using this library's own reader as the oracle on both ends.
#
# An exact diff sometimes still shows a reordering rather than a real
# difference: a generic that exists in the *original* module only via its
# generic-interfaces section (no symtree entry of its own) gets appended at
# the end of pass1's output by the post-loop sweep (see
# src/gfortran/fortran_emitter.cpp), but once round-tripped through a real
# `interface NAME ... end interface` declaration, gfortran encodes it the
# normal way (its own symtree entry) in the regenerated module -- same
# content, different position. This script always also runs a sorted diff
# to distinguish that from an actual content loss.
#
# Usage:
#   tools/roundtrip.sh <path/to/original.mod> [--best-effort|--strict]
#
# Requires a built fxmod-cli (../build/fxmod-cli relative to this script,
# or set FXMOD_CLI) and a real `gfortran` on PATH.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="${FXMOD_CLI:-$SCRIPT_DIR/../build/fxmod-cli}"
WORK="${FXMOD_ROUNDTRIP_DIR:-/tmp/fxmod-roundtrip}"
mkdir -p "$WORK"

mod="$1"
mode="${2:---best-effort}"
name=$(basename "$mod" .mod)

echo "=== $name ($mod) ==="
"$CLI" emit-source $mode "$mod" -o "$WORK/$name.pass1.f90" 2>"$WORK/$name.pass1.problems" || true
echo "pass 1: emitted $(wc -l < "$WORK/$name.pass1.f90") lines, $(wc -l < "$WORK/$name.pass1.problems") problem(s)"

rm -f "$WORK/$name.mod"
if ! gfortran -fsyntax-only -J"$WORK" "$WORK/$name.pass1.f90" 2>"$WORK/$name.compile.log"; then
  echo "FAIL: gfortran rejected pass-1 source"
  cat "$WORK/$name.compile.log"
  exit 1
fi
echo "pass 1 source compiles with real gfortran: OK"

"$CLI" emit-source $mode "$WORK/$name.mod" -o "$WORK/$name.pass2.f90" 2>"$WORK/$name.pass2.problems" || true
echo "pass 2 (regenerated .mod): emitted $(wc -l < "$WORK/$name.pass2.f90") lines, $(wc -l < "$WORK/$name.pass2.problems") problem(s)"

if diff -q "$WORK/$name.pass1.f90" "$WORK/$name.pass2.f90" >/dev/null; then
  echo "ROUND-TRIP IDENTICAL (exact)"
elif diff -q <(sort "$WORK/$name.pass1.f90") <(sort "$WORK/$name.pass2.f90") >/dev/null; then
  echo "ROUND-TRIP IDENTICAL UP TO REORDERING (see file comment above) -- exact diff:"
  diff "$WORK/$name.pass1.f90" "$WORK/$name.pass2.f90" || true
else
  echo "ROUND-TRIP DIFFERS FOR REAL -- sorted diff:"
  diff <(sort "$WORK/$name.pass1.f90") <(sort "$WORK/$name.pass2.f90") || true
  exit 1
fi
