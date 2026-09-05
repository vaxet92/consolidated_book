# Build instructions

## vcpkg is required

`md_core` and `types` themselves depend on nothing from vcpkg - that is what
keeps the domain logic testable without a toolchain - but the top-level build
always configures `md_provider`, `aggregator` and `client`, which need
Boost.Beast/Asio, OpenSSL, gRPC, Protobuf and simdjson. Set vcpkg up first
(see below).

```
cmake -S . -B build
cmake --build build -j
```

> **`-DBUILD_SERVER=OFF` does nothing.** The option is declared but gates
> nothing: `md_provider`, `aggregator` and `client` are added unconditionally,
> and the only `if (BUILD_SERVER)` guard is commented out for a `server/`
> directory that does not exist. This document previously claimed the flag gave
> a vcpkg-free build of `md_core` alone; it does not. Either delete the option
> or make it gate those three subdirectories - it should not stay as a flag
> that reads like it does something.
>
> To build just the domain library without the network stack, name the target:
> `cmake --build build --target md_core`.

## With tests

260 tests across 24 suites, all passing, in about 3 seconds. They cover the
books (both implementations, compared against each other), the merge, band
math, the three venue parsers, sequence continuity and dedup, the SPSC queue,
venue and instrument registries, config parsing, and the gRPC service over a
real in-process server:

```
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -R <test>

./build/tests/unit_tests/unit_tests --gtest_filter=MapOrderBookTest.ZeroQtyDeltaRemovesLevel
./build/tests/unit_tests/unit_tests --gtest_filter='FlatOrderBookTest.*'
```

## With the server

`-DBUILD_SERVER=ON` also configures `md_provider`, which needs Boost.Beast/
Asio and OpenSSL via vcpkg (see below) - it will fail to configure without
that set up.

```
cmake -S . -B build -DBUILD_TESTS=OFF -DBUILD_SERVER=ON
cmake --build build -j
```

## vcpkg-based build (presets) - not yet set up on this machine

`CMakePresets.json` defines `vcpkg-arm64` / `vcpkg-arm64-debug`. These
require, before they will configure at all:

1. **vcpkg installed**, with `VCPKG_ROOT` pointing at the checkout:
   ```
   git clone https://github.com/microsoft/vcpkg ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh
   export VCPKG_ROOT=~/vcpkg   # add to your shell profile to persist
   ```
2. **ninja installed** (the presets hardcode the Ninja generator):
   ```
   brew install ninja
   ```

Once both are in place:

```
cmake --preset vcpkg-arm64
cmake --build build -j

# or the debug variant
cmake --preset vcpkg-arm64-debug
cmake --build build_debug -j
```

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j


Neither `VCPKG_ROOT` nor `ninja` is currently set up in this shell - the
plain build above is the one that actually works right now.

export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++

clang++ -std=c++20 -fsyntax-only -I. -Imd_core md_core/consolidated_book.cpp 


./build/aggregator/aggregator_app

./build/client/client_app --bbo
./build/client/client_app --notional_band=100K,1M
./build/client/client_app --price_band=1000
./build/client/client_app --bbo --volume_bands --price_bands    # all three, one subscription
