# File Transfer Transport Modules — Build Guide

## Directory Structure

```
~/Cynosure/
├── modules/Comm/
│   ├── FileBeacon/
│   │   ├── file_beacon_comm.c      (Source code)
│   │   ├── Cargo.toml              (Rust stub)
│   │   └── build.sh                (Build script)
│   └── FileHTTP/
│       ├── file_http_comm.c        (Source code)
│       ├── Cargo.toml              (Rust stub)
│       └── build.sh                (Build script)
├── build_file_transports.sh        (Master build script)
└── completed/transports/           (Output directory)
    ├── file_beacon_comm.dll        (Compiled beacon transport)
    └── file_http_comm.dll          (Compiled HTTP transport)
```

## Building

### Option 1: Build Both (Recommended)
```bash
cd ~/Cynosure
bash build_file_transports.sh
```

**Output:**
```
[1/2] Building File Beacon transport...
[+] Build successful: ~/Cynosure/completed/transports/file_beacon_comm.dll

[2/2] Building File HTTP transport...
[+] Build successful: ~/Cynosure/completed/transports/file_http_comm.dll
```

### Option 2: Build Individual Modules
```bash
# File Beacon only
cd ~/Cynosure/modules/Comm/FileBeacon
bash build.sh

# File HTTP only
cd ~/Cynosure/modules/Comm/FileHTTP
bash build.sh
```

### Option 3: Manual Compilation
```bash
# File Beacon
x86_64-w64-mingw32-gcc -shared -fPIC \
    -I~/Cynosure/modules/include \
    -I~/Cynosure/src/implant \
    -o ~/Cynosure/completed/transports/file_beacon_comm.dll \
    ~/Cynosure/modules/Comm/FileBeacon/file_beacon_comm.c \
    -lws2_32 -lpthread -O2

# File HTTP
x86_64-w64-mingw32-gcc -shared -fPIC \
    -I~/Cynosure/modules/include \
    -I~/Cynosure/src/implant \
    -o ~/Cynosure/completed/transports/file_http_comm.dll \
    ~/Cynosure/modules/Comm/FileHTTP/file_http_comm.c \
    -lws2_32 -lpthread -O2
```

## Verifying Build

```bash
ls -lh ~/Cynosure/completed/transports/

# Should show:
# -rwxr-xr-x file_beacon_comm.dll
# -rwxr-xr-x file_http_comm.dll
```

## Loading in Implant

The implant's dispatcher loads these modules at startup:

```c
// In edr_dispatcher initialization
edr_dispatcher_load_plugin(d, "file_beacon_comm.dll");
edr_dispatcher_load_plugin(d, "file_http_comm.dll");

// Both now available in capability routing table
// Dispatcher can switch between them based on:
// - File size
// - Network conditions
// - ML model recommendation
```

## Module Details

### file_beacon_comm.dll
- **Transport**: Beacon protocol (chunked + base64)
- **Port**: None (uses existing beacon)
- **Size**: ~50KB
- **Best for**: Small files, stealth, firewall-restricted
- **Interface**: `edr_iface_comm_t`

### file_http_comm.dll
- **Transport**: HTTP server (direct transfer)
- **Port**: 8888 (configurable)
- **Size**: ~60KB
- **Best for**: Large files, fast transfers
- **Interface**: `edr_iface_comm_t`

## TUI Integration

When user initiates file transfer in TUI:

1. TUI sends `file-send /local /remote` or `file-recv /remote` command
2. Dispatcher receives command
3. **ML model consulted** → selects optimal transport
4. Dispatcher switches to selected module
5. File transfer proceeds through chosen transport

## Testing

```bash
# 1. Ensure both DLLs exist
ls ~/Cynosure/completed/transports/*.dll

# 2. Copy to agent directory (or set plugin_dir in dispatcher config)
cp ~/Cynosure/completed/transports/*.dll /path/to/agent/

# 3. In TUI:
# - Press [c] (command menu)
# - Select "upload" or "download"
# - Follow prompts to transfer file

# 4. Check dispatcher logs:
# [FILE_OP] agent-id: file-send /local /remote
# [TRANSPORT] Selected: file_beacon_comm (or file_http_comm)
```

## Troubleshooting

**Build fails with "x86_64-w64-mingw32-gcc: not found"**
- Install MinGW: `apt-get install mingw-w64`

**DLLs not loading in dispatcher**
- Check plugin directory is correct
- Verify `edr_plugin_entry` symbol is exported
- Check system event log for load errors

**File transfer hanging**
- Check network connectivity
- Verify ports (8888 for HTTP, beacon port for beacon)
- Check firewall rules

## Next Steps

1. **Test with actual implants** in your lab
2. **Monitor which transport is selected** based on file size/conditions
3. **Tune ML model** to learn optimal selection strategy
4. **Add resume capability** for interrupted transfers
5. **Implement compression** for faster transfers

---

For more details, see: `FILE_TRANSFER_MODULES.md`
