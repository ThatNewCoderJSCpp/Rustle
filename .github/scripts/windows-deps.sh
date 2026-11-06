#!/usr/bin/env bash
set -euo pipefail

target="$1"
prefix="$2"
work="${3:-$(pwd)/_deps-src}"

zlib_tag=v1.3.2
jpeg_tag=3.1.4.1
vulkan_tag=v1.4.365

mkdir -p "$work" "$prefix"

fetch() {
    local url="$1" tag="$2" dir="$3"
    if [ ! -d "$dir" ]; then
        for attempt in 1 2 3; do
            git clone --quiet --depth 1 --branch "$tag" "$url" "$dir" && return 0
            rm -rf "$dir"
            sleep $((attempt * 10))
        done
        echo "couldn't clone $url at $tag" >&2
        exit 1
    fi
}

fetch https://github.com/madler/zlib.git "$zlib_tag" "$work/zlib"
fetch https://github.com/libjpeg-turbo/libjpeg-turbo.git "$jpeg_tag" "$work/libjpeg-turbo"
fetch https://github.com/KhronosGroup/Vulkan-Headers.git "$vulkan_tag" "$work/Vulkan-Headers"

toolchain="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../cmake" && pwd)/llvm-mingw.cmake"

cross=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    -DLLVM_MINGW_TARGET="$target"
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

cmake -S "$work/zlib" -B "$work/build-zlib-$target" "${cross[@]}" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_MINIZIP=OFF
cmake --build "$work/build-zlib-$target"
cmake --install "$work/build-zlib-$target"

cmake -S "$work/libjpeg-turbo" -B "$work/build-jpeg-$target" "${cross[@]}" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
cmake --build "$work/build-jpeg-$target"
cmake --install "$work/build-jpeg-$target"

cmake -S "$work/Vulkan-Headers" -B "$work/build-vulkan-$target" "${cross[@]}"
cmake --install "$work/build-vulkan-$target"
