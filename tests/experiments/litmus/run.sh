#!/usr/bin/env bash
# Which memory model does this environment actually run? (docs/LESSONS.md #5)
#   MP (message passing): the weird outcome is forbidden by TSO, allowed on Arm.
#   SB (store buffering): the weird outcome is allowed by both TSO and Arm.
# MP > 0 means weakly ordered. MP == 0 with SB > 0 means TSO.
set -euo pipefail
dir="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cxx="${CXX:-c++}"
for t in mp sb; do
  "$cxx" -std=c++20 -O2 -pthread "$dir/$t.cpp" -o "$tmp/$t"
  printf '%s: ' "$t"
  "$tmp/$t" "${ROUNDS:-2000000}"
done
