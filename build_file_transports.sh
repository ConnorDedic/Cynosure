#!/bin/bash
# Build all file transfer transport modules

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Building File Transfer Transport Modules                  ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Build File Beacon
echo "[1/2] Building File Beacon transport..."
cd "${SCRIPT_DIR}/modules/Comm/FileBeacon"
bash build.sh
echo ""

# Build File HTTP
echo "[2/2] Building File HTTP transport..."
cd "${SCRIPT_DIR}/modules/Comm/FileHTTP"
bash build.sh
echo ""

# Summary
echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Build Complete - Transports ready in ~/Cynosure/completed/transports"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

ls -lh "${SCRIPT_DIR}/completed/transports/"*.dll 2>/dev/null || true
