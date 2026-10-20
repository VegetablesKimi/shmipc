#!/usr/bin/env bash
# Run a command inside the Linux dev container with the repo mounted at /src.
#   docker/run.sh cmake --preset tsan
#   docker/run.sh bash -c 'cmake --build --preset tsan && ctest --preset tsan'
#   PLATFORM=linux/amd64 docker/run.sh ...   x86-64 under emulation (Rosetta/QEMU)
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
platform="${PLATFORM:-}"
image="shmipc-dev"
platform_flag=()
build_tag=""
if [[ -n "$platform" ]]; then
  arch="${platform#*/}"
  image="shmipc-dev-$arch"
  platform_flag=(--platform "$platform")
  build_tag="-$arch"  # separate build trees: build/Linux-amd64-<preset>
fi

if ! docker image inspect "$image" >/dev/null 2>&1 || [[ "${REBUILD:-0}" == 1 ]]; then
  docker build ${platform_flag[@]+"${platform_flag[@]}"} -t "$image" "$repo/docker"
fi

tty_flag=()
[[ -t 0 && -t 1 ]] && tty_flag=(-it)

# --shm-size: the default 64MB /dev/shm is too small for benchmark rings.
# --cap-add SYS_NICE: allows sched_setaffinity / SCHED_FIFO experiments.
# --ulimit memlock: allows mlock() of whole rings.
exec docker run --rm ${tty_flag[@]+"${tty_flag[@]}"} ${platform_flag[@]+"${platform_flag[@]}"} \
  -e SHMIPC_BUILD_TAG="$build_tag" \
  --shm-size=1g --cap-add SYS_NICE --ulimit memlock=-1:-1 \
  -v "$repo":/src -w /src "$image" "${@:-bash}"
