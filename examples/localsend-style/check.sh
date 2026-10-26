#!/usr/bin/env bash
# Verification gate for LocalSend-style posix-tls integration demo
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

echo "=== Running LocalSend-style Integration Gate ==="

# 1. Build and verify host demo
echo "[1/3] Building host binary..."
bash scripts/build_host.sh

echo "[2/3] Running automated loopback tests and benchmark..."
python3 tests/test_localsend_style.py build/localsend_style_demo

# 3. Cross-compile for QNX ARMv7 and verify symbol isolation
echo "[3/3] Cross-compiling for QNX ARMv7 and verifying symbol isolation..."
bash scripts/build_qnx.sh

ROOT_DIR="$(cd ../.. && pwd)"
CHECK_SYM="${ROOT_DIR}/../posix-tls/scripts/check-symbol-isolation.py"

file build-qnx/localsend_style_demo
if [ ! -f "${CHECK_SYM}" ]; then
    echo "ERROR: required symbol isolation checker not found at ${CHECK_SYM}" >&2
    exit 2
fi
python3 "${CHECK_SYM}" build-qnx/localsend_style_demo

echo "OK: LocalSend-style integration gate passed (all checks green)"
