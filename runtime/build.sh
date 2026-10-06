#!/usr/bin/env bash
# URORuntime Build Script — 复用 M0 验证过的构建链
# 前置: export ANDROID_NDK=/workspace/android-sdk/ndk/26.3.11579264
#       binfmt: mount binfmt_misc + 注册 qemu-x86_64 (见 docs/M0_构建链验证_v1.md §2)
set -e
BUILD_TYPE=${BUILD_TYPE:-Release}
ANDROID_PLATFORM=${ANDROID_PLATFORM:-android-29}
[ -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ] || { echo "ANDROID_NDK invalid"; exit 1; }
rm -rf build && mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -GNinja .. >/dev/null
cmake --build . -j"$(nproc)"
mkdir -p ../output && cp bin/URORuntime ../output/
file ../output/URORuntime
md5sum ../output/URORuntime
