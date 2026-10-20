#!/usr/bin/env bash
# Runs the SPSC benchmark matrix: one JSON file per run, plus summary.md.
#
#   bench/scripts/run_all.sh [label]
#
# Environment:
#   BUILD=dir     build directory (default: build/<OS>-release)
#   CPUS=a,b      the two CPUs to pin to (default 0,1); choose them to pick a
#                 topology: SMT siblings, same L3, across CCX/socket
#   QUICK=1       fewer sizes and iterations (smoke run / CI)
#   TRANSPORTS=.. only these transports (default: shmipc, mutex and every competitor built in)
#
# Measurement hygiene is the caller's job, and is recorded in env.json
# when visible: performance governor, turbo off, isolated cores, nothing
# else running.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${BUILD:-$repo/build/$(uname -s)-release}"
bin="$build/bench/shmipc_bench"
label="${1:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(date +%Y%m%d-%H%M%S)}"
out="$repo/bench/results/$label"
cpus="${CPUS:-0,1}"

if [[ ! -x "$bin" ]]; then
  echo "missing $bin; build the release preset first" >&2
  exit 1
fi
mkdir -p "$out"

if [[ "${QUICK:-0}" == 1 ]]; then
  sizes=(64)
  lat_iters=(--iters=100000 --warmup=10000)
  tput_iters=(--iters=1000000)
else
  sizes=(16 64 256 1024)
  lat_iters=()
  tput_iters=()
fi

# Competitors this binary was built with (probe each with a 2-iteration run).
competitors=()
for c in rigtorp moodycamel boost aeron; do
  "$bin" pingpong --transport="$c" --iters=2 --warmup=0 >/dev/null 2>&1 && competitors+=("$c")
done
# TRANSPORTS="shmipc aeron" restricts the matrix.
read -r -a transports <<< "${TRANSPORTS:-shmipc mutex ${competitors[*]+"${competitors[*]}"}}"

for bench in pingpong oneway throughput; do
  for transport in "${transports[@]}"; do
    for mode in thread process; do
      # Heap-allocated competitor queues are thread-mode only; shmipc, the
      # mutex baseline and Aeron (shared-memory log buffers) also cross processes.
      case "$transport" in shmipc | mutex | aeron) ;; *) [[ "$mode" == process ]] && continue ;; esac
      for size in "${sizes[@]}"; do
        name="$bench-$transport-$mode-$size"
        if [[ "$bench" == throughput ]]; then extra=("${tput_iters[@]+"${tput_iters[@]}"}"); else extra=("${lat_iters[@]+"${lat_iters[@]}"}"); fi
        "$bin" "$bench" --transport="$transport" --mode="$mode" --size="$size" --cpus="$cpus" \
          ${extra[@]+"${extra[@]}"} --out="$out/$name.json"
      done
    done
  done
done

python3 "$repo/bench/scripts/report.py" "$out" | tee "$out/summary.md"
