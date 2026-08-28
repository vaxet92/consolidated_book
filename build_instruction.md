# Build instructions

## Plain build (works today, no vcpkg needed) - verified working

Nothing in `types`/`md_core` depends on vcpkg packages, so a plain CMake
configure works with no extra setup. Confirmed working end-to-end:

```
cmake -S . -B build -DBUILD_SERVER=OFF -DBUILD_TESTS=OFF
cmake --build build -j
```

This builds `libmd_core.a` (and `types`, which it depends on) and nothing
else. `utils` was removed - it was dead code left over from the deleted
candle_manager, nothing includes it anymore.

## With tests

No test sources exist yet - the old candle/trade parser tests were deleted
along with candle_manager. The first real one (VenueBook) is the next step.
This still configures cleanly, it just has nothing to run yet:

```
cmake -S . -B build -DBUILD_SERVER=OFF -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -R <test>

./build/unit_tests/unit_tests --gtest_filter=VenueBookTest.ZeroQtyDeltaRemovesLevel
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

Neither `VCPKG_ROOT` nor `ninja` is currently set up in this shell - the
plain build above is the one that actually works right now.

export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++


