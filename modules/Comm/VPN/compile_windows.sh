#!/bin/bash
# Direct Windows DLL compilation using MinGW
# This bypasses the Rust layer and compiles C directly

OUTPUT_DIR="/home/branis/Cynosure/completed/Comm/VPN"
mkdir -p "$OUTPUT_DIR"

echo "[*] Compiling VPN module for Windows x86-64..."
x86_64-w64-mingw32-gcc \
    -shared \
    -fPIC \
    -O2 \
    -o "$OUTPUT_DIR/cynosure_vpn_comm.dll" \
    -I /home/branis/Cynosure/src/implant \
    /home/branis/Cynosure/modules/Comm/VPN/vpn_comm.c \
    -lws2_32 \
    -ladvapi32 \
    -Wl,--export-all-symbols

echo "[+] Compilation complete!"
ls -lh "$OUTPUT_DIR/cynosure_vpn_comm.dll"
strings "$OUTPUT_DIR/cynosure_vpn_comm.dll" | grep edr_plugin_entry || echo "[!] Symbol not found - checking..."
