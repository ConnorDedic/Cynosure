#!/bin/bash
# Build file_http_comm for Windows

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CYNOSURE_ROOT="${SCRIPT_DIR}/../../.."
OUTPUT_DIR="${CYNOSURE_ROOT}/completed/transports"

# Create output directory
mkdir -p "${OUTPUT_DIR}"

echo "[*] Building file_http_comm (Windows x86_64)..."

# Compile with MinGW
x86_64-w64-mingw32-gcc \
    -shared -fPIC \
    -I${SCRIPT_DIR}/../../../src/implant \
    -o "${OUTPUT_DIR}/file_http_comm.dll" \
    ${SCRIPT_DIR}/file_http_comm.c \
    -lws2_32 -lpthread \
    -O2 -Wall -Wextra

if [ -f "${OUTPUT_DIR}/file_http_comm.dll" ]; then
    echo "[+] Build successful: ${OUTPUT_DIR}/file_http_comm.dll"
    ls -lh "${OUTPUT_DIR}/file_http_comm.dll"
else
    echo "[!] Build failed"
    exit 1
fi
