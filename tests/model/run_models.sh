#!/usr/bin/env bash
# Runs every GenMC model (tests/model/*.c) in its correct form and with each
# ordering site weakened to relaxed, under:
#   rc11        the C/C++ model (RC11). Plain payloads: a missing edge = data race (UB).
#   rc11-atomic RC11 with relaxed-atomic payloads: a missing edge = wrong value.
#   imm         IMM (a hardware-level model covering Arm/Power/x86 behaviour), atomic payloads.
# Writes docs/results/genmc.md. Exit status 1 if any CORRECT model reports an error.
#
#   tests/model/run_models.sh            (needs `genmc` on PATH; see docker/Dockerfile.genmc)
set -uo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
dir="$repo/tests/model"
out="${OUT:-$repo/docs/results/genmc.md}"
relaxed=memory_order_relaxed
status=0

# model : variant name : -D flags   (empty flags = the correct protocol)
variants=(
  "spsc:correct:"
  "spsc:spsc.publish:-DORD_SPSC_PUBLISH=$relaxed"
  "spsc:spsc.consume:-DORD_SPSC_CONSUME=$relaxed"
  "spsc:spsc.release:-DORD_SPSC_RELEASE=$relaxed"
  "spsc:spsc.reclaim:-DORD_SPSC_RECLAIM=$relaxed"
  "broadcast:correct:"
  "broadcast:bcast.intent:-DORD_BCAST_INTENT=$relaxed"
  "broadcast:bcast.publish:-DORD_BCAST_PUBLISH=$relaxed"
  "broadcast:bcast.observe:-DORD_BCAST_OBSERVE=$relaxed"
  "broadcast:bcast.validate:-DORD_BCAST_VALIDATE=$relaxed"
  "seqlock:correct:"
  "seqlock:seqlock.begin:-DORD_SEQLOCK_BEGIN=$relaxed"
  "seqlock:seqlock.end:-DORD_SEQLOCK_END=$relaxed"
  "seqlock:seqlock.read:-DORD_SEQLOCK_READ=$relaxed"
  "seqlock:seqlock.validate:-DORD_SEQLOCK_VALIDATE=$relaxed"
  "mpsc:correct:"
  "mpsc:mpsc.commit:-DORD_MPSC_COMMIT=$relaxed"
  "mpsc:mpsc.consume:-DORD_MPSC_CONSUME=$relaxed"
  "mpsc:mpsc.release:-DORD_MPSC_RELEASE=$relaxed"
  "mpsc:mpsc.reclaim:-DORD_MPSC_RECLAIM=$relaxed"
  "mpsc:mpsc.headcache:-DORD_MPSC_HEADCACHE_LOAD=$relaxed -DORD_MPSC_HEADCACHE_STORE=$relaxed"
  "doorbell:correct:"
  "doorbell:doorbell.notify:-DORD_DOORBELL_NOTIFY=$relaxed"
  "doorbell:doorbell.wait:-DORD_DOORBELL_WAIT=$relaxed"
)

# Runs genmc and prints "<verdict> (<executions>)".
run() {
  local model="$1" file="$2"
  shift 2
  local log
  log="$(timeout "${GENMC_TIMEOUT:-300}" genmc "--$model" -- -I"$dir" "$@" "$file" 2>&1)"
  local rc=$?
  local n
  n="$(grep -oE 'complete executions explored: [0-9]+' <<<"$log" | grep -oE '[0-9]+$' | head -1)"
  if [[ $rc -eq 124 ]]; then
    echo "timeout"
  elif grep -q "No errors were detected" <<<"$log"; then
    echo "ok (${n:-?})"
  elif grep -qiE "race" <<<"$log"; then
    echo "**race**"
  elif grep -qiE "assert|safety violation" <<<"$log"; then
    echo "**assert**"
  else
    echo "error: $(grep -m1 -iE 'error' <<<"$log" | cut -c1-60)"
  fi
}

{
  echo "# GenMC model checking"
  echo
  echo "GenMC $(genmc --version 2>&1 | grep -oE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1), host $(uname -m)."
  echo "\`ok (N)\`: no error in any of the N executions (exhaustive). **race**: a data race on a plain payload"
  echo "(undefined behaviour in C++). **assert**: an execution in which the protocol returns a wrong value."
  echo "\`timeout\`: not finished within ${GENMC_TIMEOUT:-300} s (no verdict)."
  echo
  echo "| model | variant | RC11, plain payload | RC11, atomic payload | IMM, atomic payload |"
  echo "|---|---|---|---|---|"
} >"$out"

for v in "${variants[@]}"; do
  IFS=: read -r model name flags <<<"$v"
  file="$dir/$model.c"
  # shellcheck disable=SC2086
  plain="$(run rc11 "$file" $flags)"
  # shellcheck disable=SC2086
  ratomic="$(run rc11 "$file" -DDATA_ATOMIC $flags)"
  # shellcheck disable=SC2086
  imm="$(run imm "$file" -DDATA_ATOMIC $flags)"
  echo "| $model | $name | $plain | $ratomic | $imm |" | tee -a "$out"
  if [[ "$name" == correct && ( "$plain" != ok* || "$ratomic" != ok* || "$imm" != ok* ) ]]; then
    echo "::error::correct $model model reported an error" >&2
    status=1
  fi
done
exit $status
