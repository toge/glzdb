#! /bin/sh

set -eu

BUILD_DIR=build
if [ "$#" -ne 0 ] && [ "$1" = "static" ]; then
    BUILD_DIR=build_static
fi

if [ "$(uname -m)" = "aarch64" ]; then
    export CXXFLAGS="-O3 -mcpu=native"
    export CFLAGS="-O3 -mcpu=native"
    : "${VCPKG_TARGET_TRIPLET:=arm64-linux-static}"
else
    export CXXFLAGS="-O3 -march=native"
    export CFLAGS="-O3 -march=native"
    : "${VCPKG_TARGET_TRIPLET:=x64-linux-static}"
fi

VCPKG_ROOT=${VCPKG_ROOT:-"$HOME/vm/vcpkg"}
if [ ! -d "$VCPKG_ROOT" ]; then
    printf '%s\n' "VCPKG_ROOT does not exist: $VCPKG_ROOT" >&2
    exit 1
fi
VCPKG_ROOT=$(CDPATH= cd "$VCPKG_ROOT" && pwd -P)
export VCPKG_ROOT
export VCPKG_OVERLAY_PORTS="$VCPKG_ROOT/ports"
export VCPKG_TARGET_TRIPLET

cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -S .
cmake --build "$BUILD_DIR" --verbose --parallel
