#!/usr/bin/env bash
# PersonaRM Linux Quick Build Script
set -e

BUILD_TYPE="${1:-Release}"
RUN_TESTS=false
LAUNCH=false

for arg in "$@"; do
    case $arg in
        --tests) RUN_TESTS=true ;;
        --launch) LAUNCH=true ;;
    esac
done

echo -e "\033[0;36m=== PersonaRM Build ($BUILD_TYPE) ===\033[0m"

# 1. Configure CMake
echo -e "\n\033[0;33m[1/3] Configuring CMake...\033[0m"
if [ "$RUN_TESTS" = true ]; then
    cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux.cmake -DBUILD_TESTS=ON
else
    cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux.cmake
fi

# 2. Build
echo -e "\n\033[0;33m[2/3] Compiling Project...\033[0m"
cmake --build build --config "$BUILD_TYPE" -j"$(nproc)"

# 3. Tests / Launch
if [ "$RUN_TESTS" = true ]; then
    echo -e "\n\033[0;33m[3/3] Running Unit Tests...\033[0m"
    ctest --test-dir build --output-on-failure -C "$BUILD_TYPE"
elif [ "$LAUNCH" = true ]; then
    echo -e "\n\033[0;32m[3/3] Launching PersonaRM...\033[0m"
    ./build/Release/personarm --launch
else
    echo -e "\n\033[0;32m[3/3] Build finished successfully! Executable: ./build/Release/personarm\033[0m"
fi

