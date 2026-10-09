#!/bin/bash
# ==============================================================================
# LocalSend for BlackBerry 10 - Build and Package Script
# Target: BlackBerry 10 OS 10.3.x (ARMv7le), Cascades / Qt 4.8.5
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== Building LocalSend for BlackBerry 10 ==="
echo "Project root: ${ROOT_DIR}"

CONTAINER_CLI=""
if command -v podman >/dev/null 2>&1; then
    CONTAINER_CLI="podman"
elif command -v docker >/dev/null 2>&1; then
    CONTAINER_CLI="docker"
fi

BBNDK_PATH="${HOME}/bbndk"
if [ ! -d "${BBNDK_PATH}" ] && [ -d "/opt/bbndk" ]; then
    BBNDK_PATH="/opt/bbndk"
fi

# Prefer containerized toolchain if available
if [ -n "${CONTAINER_CLI}" ] && [ -d "${BBNDK_PATH}" ]; then
    echo "Using containerized toolchain via ${CONTAINER_CLI} (bb10-toolchain:latest)..."
    if ! ${CONTAINER_CLI} image exists bb10-toolchain:latest 2>/dev/null; then
        echo "Building bb10-toolchain container image..."
        ${CONTAINER_CLI} build -t bb10-toolchain:latest -f "${ROOT_DIR}/docker/bb10-toolchain/Dockerfile" "${ROOT_DIR}/docker/bb10-toolchain"
    fi

    ${CONTAINER_CLI} run --rm \
        -v "${BBNDK_PATH}:/bbndk:z" \
        -v "${ROOT_DIR}:/workspace:z" \
        -w /workspace \
        bb10-toolchain:latest \
        bb10-build /workspace

    echo "=== Build and Packaging Complete ==="
    exit 0
fi

# Fallback: native host toolchain
if [ -z "${QNX_HOST:-}" ] || [ -z "${QNX_TARGET:-}" ]; then
    POSSIBLE_ENV_SCRIPTS=(
        "${HOME}/bbndk/bbndk-env.sh"
        "/opt/bbndk/bbndk-env.sh"
        "${HOME}/Development/bbndk/bbndk-env.sh"
    )
    FOUND_ENV=""
    for s in "${POSSIBLE_ENV_SCRIPTS[@]}"; do
        if [ -f "$s" ]; then
            FOUND_ENV="$s"
            break
        fi
    done

    if [ -n "$FOUND_ENV" ]; then
        echo "Sourcing BlackBerry NDK environment from: ${FOUND_ENV}"
        # shellcheck disable=SC1090
        source "$FOUND_ENV"
    else
        echo "ERROR: Neither container toolchain nor native BB10 NDK found."
        echo "Please install Podman/Docker or set QNX_HOST/QNX_TARGET."
        exit 1
    fi
fi

cd "${ROOT_DIR}"
qmake -spec unsupported/blackberry-armv7le-qcc CONFIG+=cascades10 CONFIG+=release "${ROOT_DIR}/localsend-bb10.pro"
if [ -f "${ROOT_DIR}/scripts/sanitize-makefile.py" ]; then
    python3 "${ROOT_DIR}/scripts/sanitize-makefile.py" Makefile
fi
make clean >/dev/null 2>&1 || true
make -j"$(nproc)"

mkdir -p "${ROOT_DIR}/arm/o.le-v7"
cp -f "${ROOT_DIR}/liblocalsend-bb10.so.1.0.0" "${ROOT_DIR}/arm/o.le-v7/localsend-bb10"

blackberry-nativepackager \
    -package "${ROOT_DIR}/localsend-bb10.bar" \
    -devMode \
    "${ROOT_DIR}/bar-descriptor.xml" \
    -e "${ROOT_DIR}/arm/o.le-v7/localsend-bb10" localsend-bb10 \
    -C "${ROOT_DIR}" assets

echo "=== Packaging Complete ==="
echo "Artifact: ${ROOT_DIR}/localsend-bb10.bar"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${ROOT_DIR}/localsend-bb10.bar"
fi
