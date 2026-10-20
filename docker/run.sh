#!/usr/bin/env bash
# Run a command inside the Linux dev container with the repo mounted at /src.
#   docker/run.sh cmake --preset tsan
#   docker/run.sh bash -c 'cmake --build --preset tsan && ctest --preset tsan'
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
image="shmipc-dev"

if ! docker image inspect "$image" >/dev/null 2>&1 || [[ "${REBUILD:-0}" == 1 ]]; then
  docker build -t "$image" "$repo/docker"
fi

tty_flag=()
[[ -t 0 && -t 1 ]] && tty_flag=(-it)

# --shm-size: the default 64MB /dev/shm is too small for benchmark rings.
# --cap-add SYS_NICE: allows sched_setaffinity / SCHED_FIFO experiments.
# --ulimit memlock: allows mlock() of whole rings.
exec docker run --rm ${tty_flag[@]+"${tty_flag[@]}"} \
  --shm-size=1g --cap-add SYS_NICE --ulimit memlock=-1:-1 \
  -v "$repo":/src -w /src "$image" "${@:-bash}"
