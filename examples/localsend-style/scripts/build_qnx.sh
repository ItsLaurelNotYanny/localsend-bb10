#!/usr/bin/env bash
# Cross-compile LocalSend-style demo for QNX ARMv7 using posix-tls QNX SDK
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

ROOT_DIR="$(cd ../.. && pwd)"
POSIX_TLS_DIR="${ROOT_DIR}/../posix-tls"

if [ ! -d "${POSIX_TLS_DIR}/build/sdk-qnx" ]; then
    echo "ERROR: posix-tls QNX SDK not found at ${POSIX_TLS_DIR}/build/sdk-qnx" >&2
    exit 1
fi

CONTAINER_IMAGE="${BB10_NATIVE_IMAGE:-bb10-native-dev:local}"
BUILD_DIR="build-qnx"
mkdir -p "${BUILD_DIR}"

docker run --rm --network none --user "$(id -u):$(id -g)" \
    -v "${ROOT_DIR}:/workspace" \
    -v "${POSIX_TLS_DIR}/build/sdk-qnx:/sdk-qnx:ro" \
    -w /workspace/examples/localsend-style \
    "${CONTAINER_IMAGE}" bash -c '
        cmake -S . -B build-qnx \
            -DCMAKE_TOOLCHAIN_FILE=/opt/bb10-toolchain.cmake \
            -DPOSIX_TLS_SDK_DIR=/sdk-qnx &&
        cmake --build build-qnx --parallel "${BUILD_JOBS:-4}"
    '

echo "OK: QNX ARMv7 demo built at ${BUILD_DIR}/localsend_style_demo"
