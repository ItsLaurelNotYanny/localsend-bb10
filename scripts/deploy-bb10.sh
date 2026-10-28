#!/bin/bash
# ==============================================================================
# LocalSend for BlackBerry 10 - Deployment & Sideload Script
# Supports:
# 1. Official NDK blackberry-deploy tool (Development Mode via Wi-Fi/USB)
# 2. Direct SSH / SCP sideload for pre-rooted devices (Oleksandr BB10 OS v3)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BAR_FILE="${ROOT_DIR}/localsend-bb10.bar"

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --device <IP>        Target BlackBerry 10 device IP address (Required)"
    echo "  --password <PASS>    Device password for blackberry-deploy (Optional for SSH)"
    echo "  --method <METHOD>    Deployment method: 'deploy' (default) or 'ssh'"
    echo "  --bar <PATH>         Path to BAR package (default: ${BAR_FILE})"
    echo "  --launch             Launch app on device after installation"
    echo ""
    echo "Examples:"
    echo "  $0 --device 192.168.1.150 --password mypassword"
    echo "  $0 --device 192.168.1.150 --password mypassword --launch"
    echo "  $0 --device 192.168.1.150 --method ssh"
    exit 1
}

DEVICE_IP=""
DEVICE_PASSWORD=""
METHOD="deploy"
LAUNCH_APP=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)
            DEVICE_IP="$2"
            shift 2
            ;;
        --password)
            DEVICE_PASSWORD="$2"
            shift 2
            ;;
        --method)
            METHOD="$2"
            shift 2
            ;;
        --bar)
            BAR_FILE="$2"
            shift 2
            ;;
        --launch)
            LAUNCH_APP=true
            shift 1
            ;;
        *)
            usage
            ;;
    esac
done

if [ -z "${DEVICE_IP}" ]; then
    echo "ERROR: --device <IP> is required."
    usage
fi

if [ -z "${BAR_FILE}" ] || [ ! -f "${BAR_FILE}" ]; then
    VERSION="$(grep '<versionNumber>' "${ROOT_DIR}/bar-descriptor.xml" 2>/dev/null | sed -e 's/.*<versionNumber>\(.*\)<\/versionNumber>.*/\1/' -e 's/[[:space:]]//g' || true)"
    BUILD="$(grep '<buildId>' "${ROOT_DIR}/bar-descriptor.xml" 2>/dev/null | sed -e 's/.*<buildId>\(.*\)<\/buildId>.*/\1/' -e 's/[[:space:]]//g' || true)"
    if [ -n "${VERSION}" ] && [ -n "${BUILD}" ] && [ -f "${ROOT_DIR}/localsend-bb10-v${VERSION}-build${BUILD}.bar" ]; then
        BAR_FILE="${ROOT_DIR}/localsend-bb10-v${VERSION}-build${BUILD}.bar"
    elif [ -f "${ROOT_DIR}/localsend-bb10.bar" ]; then
        BAR_FILE="${ROOT_DIR}/localsend-bb10.bar"
    else
        echo "ERROR: BAR package not found at: ${BAR_FILE}"
        echo "Please run scripts/build-bb10.sh first to build the package."
        exit 1
    fi
fi

echo "=== Deploying LocalSend to BlackBerry 10 (${DEVICE_IP}) ==="
echo "Package: ${BAR_FILE}"
echo "Method:  ${METHOD}"

if [ "${METHOD}" == "deploy" ]; then
    USE_CONTAINER=false
    if ! command -v blackberry-deploy >/dev/null 2>&1; then
        if [ -d "${HOME}/bbndk/host_10_3_1_12/win32/x86/usr/bin" ]; then
            export PATH="${HOME}/bbndk/features/com.qnx.tools.jre.linux.x86_64_1.7.0.51/jre/bin:${HOME}/bbndk/host_10_3_1_12/win32/x86/usr/bin:$PATH"
        fi
    fi

    if ! command -v blackberry-deploy >/dev/null 2>&1; then
        if command -v docker >/dev/null 2>&1 && docker image inspect bb10-native-dev:local >/dev/null 2>&1; then
            USE_CONTAINER=true
        else
            echo "ERROR: blackberry-deploy not found in PATH and container bb10-native-dev:local not available."
            echo "Please source bbndk-env.sh or install Docker."
            exit 1
        fi
    fi

    DEPLOY_ARGS=(-installApp)
    if [ "${LAUNCH_APP}" = true ]; then
        DEPLOY_ARGS+=(-launchApp)
    fi
    DEPLOY_ARGS+=(-device "${DEVICE_IP}")
    if [ -n "${DEVICE_PASSWORD}" ]; then
        DEPLOY_ARGS+=(-password "${DEVICE_PASSWORD}")
    fi

    echo "Running blackberry-deploy..."
    if [ "${USE_CONTAINER}" = true ]; then
        BAR_ABS="$(cd "$(dirname "${BAR_FILE}")" && pwd)/$(basename "${BAR_FILE}")"
        BAR_DIR="$(dirname "${BAR_ABS}")"
        BAR_NAME="$(basename "${BAR_ABS}")"
        docker run --rm --net host \
            -v "${BAR_DIR}:/pkg:ro" \
            bb10-native-dev:local \
            blackberry-deploy "${DEPLOY_ARGS[@]}" -package "/pkg/${BAR_NAME}"
    else
        blackberry-deploy "${DEPLOY_ARGS[@]}" -package "${BAR_FILE}"
    fi
    echo "Successfully installed ${BAR_FILE} on ${DEVICE_IP}!"

elif [ "${METHOD}" == "ssh" ]; then
    if [ "${LAUNCH_APP}" = true ]; then
        echo "ERROR: --launch is not supported in SSH deployment mode (use blackberry-deploy for launching)." >&2
        exit 1
    fi

    echo "Deploying via SSH/SCP (Rooted device mode)..."
    REMOTE_TMP="/tmp/localsend-bb10.bar"

    echo "1. Uploading BAR package to device..."
    if ! scp -o StrictHostKeyChecking=no "${BAR_FILE}" "root@${DEVICE_IP}:${REMOTE_TMP}"; then
        echo "ERROR: Failed to upload BAR package to device via SCP." >&2
        exit 1
    fi

    echo "2. Installing BAR on device via bar-installer / sideload..."
    if ! ssh -o StrictHostKeyChecking=no "root@${DEVICE_IP}" "bar-installer ${REMOTE_TMP} 2>/dev/null || /apps/bin/sideload ${REMOTE_TMP} 2>/dev/null"; then
        echo "ERROR: Automatic installation failed on device (neither bar-installer nor sideload succeeded)." >&2
        echo "The package remains uploaded at ${REMOTE_TMP} for manual installation via device file manager or BB10MT." >&2
        exit 1
    fi

    echo "=== SSH Deployment Finished (Installation Succeeded) ==="
else
    echo "ERROR: Unknown method '${METHOD}'. Use 'deploy' or 'ssh'."
    exit 1
fi
