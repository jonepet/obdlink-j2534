FROM debian:trixie-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        g++ \
        libgtest-dev \
        g++-mingw-w64-i686-posix \
        g++-mingw-w64-x86-64-posix \
        binutils-mingw-w64-i686 \
        binutils-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
