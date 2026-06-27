#!/bin/bash
# Start the RL Beacon Agent Service

set -e

echo "[*] Starting RL Beacon Agent Service..."
echo ""

# Check if Python is available
if ! command -v python3 &> /dev/null; then
    echo "[!] Python 3 is not installed"
    exit 1
fi

# Check if requirements are installed
echo "[*] Checking Python dependencies..."
python3 -c "import torch; import aiohttp; import numpy" 2>/dev/null || {
    echo "[!] Missing dependencies. Installing..."
    pip3 install -r requirements-rl.txt
}

# Start the service
cd src
python3 rl_beacon_service.py
