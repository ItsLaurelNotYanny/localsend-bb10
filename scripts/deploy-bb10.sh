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

if [ ! -f "${BAR_FILE}" ]; then
    echo "ERROR: BAR package not found at: ${BAR_FILE}"
    echo "Please run scripts/build-bb10.sh first to build the package."
    exit 1
fi

echo "=== Deploying LocalSend to BlackBerry 10 (${DEVICE_IP}) ==="
echo "Package: ${BAR_FILE}"
echo "Method:  ${METHOD}"

if [ "${METHOD}" == "deploy" ]; then
    if ! command -v blackberry-deploy >/dev/null 2>&1; then
        if [ -d "${HOME}/bbndk/host_10_3_1_12/win32/x86/usr/bin" ]; then
            export PATH="${HOME}/bbndk/features/com.qnx.tools.jre.linux.x86_64_1.7.0.51/jre/bin:${HOME}/bbndk/host_10_3_1_12/win32/x86/usr/bin:$PATH"
        fi
    fi

    if ! command -v blackberry-deploy >/dev/null 2>&1; then
        echo "ERROR: blackberry-deploy not found in PATH."
        echo "Please source bbndk-env.sh or use '--method ssh' on rooted devices."
        exit 1
    fi

    DEPLOY_ARGS=(-installApp)
    if [ "${LAUNCH_APP}" = true ]; then
        DEPLOY_ARGS+=(-launchApp)
    fi
    DEPLOY_ARGS+=(-device "${DEVICE_IP}" -package "${BAR_FILE}")
    if [ -n "${DEVICE_PASSWORD}" ]; then
        DEPLOY_ARGS+=(-password "${DEVICE_PASSWORD}")
    fi

    echo "Running blackberry-deploy..."
    blackberry-deploy "${DEPLOY_ARGS[@]}"
    echo "Successfully installed ${BAR_FILE} on ${DEVICE_IP}!"

elif [ "${METHOD}" == "ssh" ]; then
    echo "Deploying via SSH/SCP (Rooted device mode)..."
    REMOTE_TMP="/tmp/localsend-bb10.bar"

    echo "1. Uploading BAR package to device..."
    scp -o StrictHostKeyChecking=no "${BAR_FILE}" "root@${DEVICE_IP}:${REMOTE_TMP}"

    echo "2. Installing BAR on device via bar-installer / sideload..."
    ssh -o StrictHostKeyChecking=no "root@${DEVICE_IP}" "bar-installer ${REMOTE_TMP} || /apps/bin/sideload ${REMOTE_TMP} || echo 'Please install ${REMOTE_TMP} via device file manager or BB10MT.'"

    echo "=== SSH Deployment Finished ==="
else
    echo "ERROR: Unknown method '${METHOD}'. Use 'deploy' or 'ssh'."
    exit 1
fi
