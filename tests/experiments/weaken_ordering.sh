#!/usr/bin/env bash
# Memory-ordering bug-injection experiment (docs/CORRECTNESS.md).
#
# For each OrderSite, build the stress tests with that one acquire/release
# weakened to relaxed (-DSHMIPC_WEAKEN_ORDERING=<bit>). Then run the
# matching stress tests repeatedly until they fail or the time budget runs
# out. An unmodified build gets the same budget, as the control.
#
#   tests/experiments/weaken_ordering.sh <label>
#
# Environment:
#   BUDGET=60      seconds of stress per configuration
#   SITES="0 4"    only these site bits (default: all, plus per-channel "all" masks)
#
# Output: docs/results/weaken-<label>.md
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
label="${1:?usage: weaken_ordering.sh <label>}"
budget="${BUDGET:-60}"
os="$(uname -s)"
out="$repo/docs/results/weaken-$label.md"
mkdir -p "$repo/docs/results"

# bit name filter
configs=(
  "none   control           SpscStress.*:BroadcastStress.*:SeqlockStress.*"
  "0      spsc.publish      SpscStress.*"
  "1      spsc.consume      SpscStress.*"
  "2      spsc.release      SpscStress.*"
  "3      spsc.reclaim      SpscStress.*"
  "0-3    spsc.all          SpscStress.*"
  "4      bcast.intent      BroadcastStress.*"
  "5      bcast.publish     BroadcastStress.*"
  "6      bcast.observe     BroadcastStress.*"
  "7      bcast.validate    BroadcastStress.*"
  "4-7    bcast.all         BroadcastStress.*"
  "8      seqlock.begin     SeqlockStress.*"
  "9      seqlock.end       SeqlockStress.*"
  "10     seqlock.read      SeqlockStress.*"
  "11     seqlock.validate  SeqlockStress.*"
  "8-11   seqlock.all       SeqlockStress.*"
)

mask_of() {
  case "$1" in
    none) echo 0 ;;
    *-*) local lo=${1%-*} hi=${1#*-} m=0; for ((b = lo; b <= hi; b++)); do m=$((m | (1 << b))); done; echo $m ;;
    *) echo $((1 << $1)) ;;
  esac
}

# Runs a command with a wall-clock limit (macOS has no `timeout`).
with_limit() { perl -e 'alarm shift; exec @ARGV' "$@"; }

{
  echo "# Ordering bug injection: $label"
  echo
  echo "- Host: $(uname -srm)"
  echo "- Compiler: ${CXX:-default}, release preset (-O3)"
  echo "- Budget: ${budget}s of repeated stress runs per row"
  echo
  echo "| weakened site | mask | runs | first failure after | failure |"
  echo "|---|---:|---:|---:|---|"
} > "$out"

for line in "${configs[@]}"; do
  read -r bit name filter <<< "$line"
  if [[ -n "${SITES:-}" && " $SITES " != *" $bit "* ]]; then continue; fi
  mask=$(mask_of "$bit")
  build="$repo/build/$os${SHMIPC_BUILD_TAG:-}-weaken-$mask"
  cmake --preset release -B "$build" -DSHMIPC_WEAKEN_ORDERING="$mask" -DSHMIPC_BUILD_BENCH=OFF >/dev/null
  cmake --build "$build" --target shmipc_stress_tests >/dev/null
  bin="$build/tests/shmipc_stress_tests"

  start=$(date +%s)
  runs=0
  result="none"
  after="-"
  while (( $(date +%s) - start < budget )); do
    runs=$((runs + 1))
    log="$build/last-run.log"
    if ! with_limit 180 "$bin" --gtest_filter="$filter" --gtest_brief=1 >"$log" 2>&1; then
      after="$(( $(date +%s) - start ))s (run $runs)"
      # First failing test and its first assertion message.
      test_name=$(grep -m1 -oE '\[  FAILED  \] [A-Za-z.]+' "$log" | awk '{print $NF}' || true)
      reason=$(grep -m1 -E 'saw a torn|accepted a torn|Expected equality|catch up|accounting' "$log" | sed 's/|/\\|/g' | cut -c1-80 || true)
      result="${test_name:-hang/crash} ${reason}"
      break
    fi
  done
  echo "| $name | $mask | $runs | $after | $result |" >> "$out"
  echo "$name: runs=$runs after=$after $result" >&2
done

echo >> "$out"
echo "\"none\" in the failure column means no failure was observed within the budget." \
     "That is not a proof of correctness; see CORRECTNESS.md." >> "$out"
cat "$out"
