#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

LIBRETRO="OFF"
BUILD_TYPE="Debug"
PIC="ON"
ARCH="arm64"
SYSTEM_NAME="macOS"
RUN_BUILD="ON"

# Simple helper to configure, build and run the C++ unit-tests (GoogleTest)
# against the SH4 cached-IR executor.
#
# Usage:
#   ./tests/test.sh [build_dir] [extra cmake args ...]
#
# If no build directory is supplied, a default "build/tests" folder is used.
# Any extra arguments are forwarded directly to the cmake configuration step,
# enabling custom generators, compilers, etc.

set -euo pipefail

# Pick build directory from first argument or use default
BUILD_DIR="${1:-build/tests}"
if [[ $# -gt 0 ]]; then
  shift # remove build dir param so remaining args go to cmake
fi

# Clean the build directory
#rm -rf "${BUILD_DIR}"

# Configure with CMake
export VULKAN_SDK="$HOME/VulkanSDK/1.3.296.0/macOS"
cmake -S "${SCRIPT_DIR}/.." -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/macos_clang_toolchain.cmake" \
       -DLIBRETRO=${LIBRETRO} \
       -DVulkan_INCLUDE_DIR=$HOME/VulkanSDK/1.3.296.0/macOS/include \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DCMAKE_POSITION_INDEPENDENT_CODE=${PIC} \
      -DCMAKE_SYSTEM_NAME=${SYSTEM_NAME} \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DENABLE_SH4_IR=ON \
      -DBUILD_TESTING=ON \
      -DENABLE_OPENMP=OFF \
      -DUSE_JIT=OFF \
      -DENABLE_DYNAREC=OFF \
      -DUSE_BREAKPAD=OFF \
      -DTARGET_NO_NIXPROF=ON \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
       -DUSE_HOST_SDL=OFF \
      "$@"

# Build all (tests are linked to the main target by the root CMakeLists)
echo "Build log in ${BUILD_DIR}/build.log"
cmake --build "${BUILD_DIR}" --target flycast -- VERBOSE=1 -j 12 > "${BUILD_DIR}/build.log" 2>&1 || {
    tail -n100 "${BUILD_DIR}/build.log"
    echo "Build failed. See ${BUILD_DIR}/build.log"
    exit 1
}

# Run unit tests
cd "${BUILD_DIR}"
TEST_LOG_FILE="${SCRIPT_DIR}/../test_log.txt"
rm -f "${TEST_LOG_FILE}"
echo "Test log in ${TEST_LOG_FILE}"
ctest --verbose --output-on-failure -R Sh4InterpreterTest > "${TEST_LOG_FILE}" 2>&1 || {
    tail -n100 "${TEST_LOG_FILE}"
    echo "Tests failed. See ${TEST_LOG_FILE}"
    exit 1
}
