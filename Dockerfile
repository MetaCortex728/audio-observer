# Development image for audio-observer.
# Core toolchain only: no audio libraries, since the core never opens a device.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        g++ \
        gcc \
        git \
        mingw-w64 \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
