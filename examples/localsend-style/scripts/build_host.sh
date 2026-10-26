#!/usr/bin/env bash
# Build LocalSend-style demo on host system using posix-tls host SDK
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

BUILD_DIR="build"
mkdir -p "${BUILD_DIR}"

cmake -S . -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS:-4}"

echo "OK: Host demo built at ${BUILD_DIR}/localsend_style_demo"
