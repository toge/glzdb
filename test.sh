#!/bin/sh
set -eu

BUILD_DIR=build
if [ "$#" -ne 0 ] && [ "$1" = "static" ]; then
    BUILD_DIR=build_static
fi

cd "$BUILD_DIR"
ctest -V
