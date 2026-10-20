# Linux development image. Builds natively for the host architecture
# (arm64 on Apple silicon, x86-64 elsewhere).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-14 clang-18 lld-18 libclang-rt-18-dev \
      cmake ninja-build git ca-certificates curl \
      python3 python3-pip gdb libboost-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Aeron (optional benchmark competitor) needs CMake >= 3.30; Ubuntu 24.04 ships 3.28.
RUN pip3 install --no-cache-dir --break-system-packages cmake==3.31.6

ENV CC=clang-18 CXX=clang++-18
WORKDIR /src
