# syntax=docker/dockerfile:1
#
# Multi-stage: one image serves both the aggregator and the clients. Compose
# supplies the command per service, so there is deliberately no CMD here.

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

# ninja + pkg-config for the build; git/curl/zip/tar for vcpkg's fetching;
# ca-certificates so vcpkg can clone over HTTPS. ccache persists compiled
# object files across builds (see the build step below).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config \
        ca-certificates linux-libc-dev ccache \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src

# Dependencies FIRST, from the manifest alone. gRPC, Boost, OpenSSL and
# Protobuf all compile from source, so this layer is the expensive one
# (~30-60 min cold). Keeping it above the source copy means editing a .cpp
# reuses it instead of rebuilding gRPC.
COPY vcpkg.json ./

# The cache mount is what makes a vcpkg.json change survivable. vcpkg stores
# built packages in ~/.cache/vcpkg/archives; without the mount that lives
# inside the layer, so invalidating this layer rebuilds gRPC, Boost and
# OpenSSL from source - another 30-60 min for a one-line manifest change.
# With it, only the newly added package builds.
#
# --clean-after-build discards build trees (large) but keeps the binary cache
# (the part worth persisting).
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    ${VCPKG_ROOT}/vcpkg install --clean-after-build

# Source second - changes here reuse the dependency layer above.
COPY CMakeLists.txt ./
COPY types/ types/
COPY config/ config/
COPY logger/ logger/
COPY md_core/ md_core/
COPY md_provider/ md_provider/
COPY md_proto/ md_proto/
COPY aggregator/ aggregator/
COPY client/ client/
COPY tests/ tests/

# VCPKG_MANIFEST_INSTALL=OFF: the dependency layer above already ran
# `vcpkg install` into /src/vcpkg_installed. Without this flag the vcpkg
# toolchain runs `install` AGAIN at CMake configure time. That rerun is not
# guaranteed to hit the binary cache, so it can rebuild gRPC/Boost from source
# - 15+ min - on every single source edit. OFF makes CMake trust the tree that
# is already there; VCPKG_INSTALLED_DIR points it at that tree.
#
# The two cache mounts stay as a safety net (a stray manifest change still
# lands in the persistent binary cache instead of the layer). CCACHE_DIR is a
# cache mount too: after this, fixing one .cpp recompiles one file, not all 52.
ENV CCACHE_DIR=/root/.cache/ccache
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build build -j

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime

# ca-certificates is REQUIRED, not optional: the aggregator opens TLS
# connections to the exchanges, and without a CA bundle every handshake fails
# in a way that looks like a network problem rather than a missing package.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/aggregator/aggregator_app ./
COPY --from=builder /src/build/client/client_app ./

# aggregator_app reads this by a FIXED name (config.h::kConfigFileName) from
# its working directory - there is no --config= flag to point elsewhere, so
# it must land at exactly /app/server_config.json. Copied from the build
# context (not the builder stage): it is data, not something compiled.
COPY server_config.json ./

# Venue endpoints - hosts, ports and WS/REST paths per (venue, market). Read by
# the same fixed-name rule as server_config.json, so it must land at exactly
# /app/venues_config.json.
#
# KEY: this file is REQUIRED, not optional. A missing one is a startup error
# rather than a fall-back to built-in defaults, because there are no built-in
# defaults any more - types/venue.h no longer carries the host constants. If
# this COPY is ever dropped, the container fails immediately with
# "venues_config: could not open ..." instead of connecting somewhere stale.
COPY venues_config.json ./

EXPOSE 50051
