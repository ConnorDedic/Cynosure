# File Transfer Modules — Beacon vs HTTP

Two pluggable file transfer implementations that follow the same `edr_iface_comm` abstract interface. The dispatcher (or ML model) can choose which method to use based on network conditions, latency, and connectivity.

## Module A: File Beacon (`file_beacon_comm.dll`)

**Transport**: Beacon protocol (integrated with existing beacon channel)

### How It Works
1. **File chunking**: Reads file in 4KB chunks
2. **Encoding**: Base64 encodes each chunk
3. **Transmission**: Sends via standard JSON beacon messages
4. **Reassembly**: Implant chunks are reassembled on disk

### Advantages
- ✅ Uses existing beacon channel (no new ports)
- ✅ Works through proxies/firewalls (same as beacon)
- ✅ Encrypted if beacon is encrypted
- ✅ Low overhead (integrated with heartbeat)

### Disadvantages
- ❌ Slower than HTTP (chunked + base64 overhead)
- ❌ Beacon interval delays transfers
- ❌ Base64 inflates file size ~33%

### Message Format
```json
{
  "file": "C:\\target\\path\\file.exe",
  "chunk": "BASE64_ENCODED_DATA",
  "offset": 4096,
  "size": 4096
}
```

### Best For
- **Stealth transfers** (no new network activity)
- **Firewall-heavy environments** (uses beacon path)
- **Small files** (< 50MB)

## Module B: File HTTP (`file_http_comm.dll`)

**Transport**: Temporary HTTP server on agent

### How It Works
1. **Server startup**: Agent starts HTTP server on port 8888 (configurable)
2. **Upload**: TUI POSTs file to `/upload` endpoint
3. **Download**: TUI GETs file from `/download` endpoint
4. **Direct transfer**: No chunking, direct file copy

### Advantages
- ✅ Fast (direct HTTP, no chunking)
- ✅ No beacon interval delays
- ✅ Standard HTTP (resumable, range requests)
- ✅ Large files efficient (streaming)

### Disadvantages
- ❌ Opens new listening port (8888)
- ❌ Requires agent to expose HTTP server
- ❌ Firewall rules may block
- ❌ Not stealthy (new port, new network signature)

### HTTP Endpoints
```
POST /upload           - Agent receives file from TUI
GET /download          - TUI downloads file from agent
PUT /file              - Resumable uploads
GET /file?range=X-Y    - Range requests for large files
```

### Best For
- **Speed-critical transfers** (large files, high bandwidth)
- **Low-latency environments**
- **Stealth not required** (DMZ, lab, internal network)
- **Large files** (> 50MB)

## Dispatcher/ML Selection Strategy

The dispatcher can choose based on:

```c
edr_status_t select_file_transport(
    const char *filename,
    uint64_t filesize,
    network_conditions_t *net,
    char *out_module_name
) {
    // Small files + latency-sensitive → beacon
    if (filesize < 50*1024*1024 && net->latency_ms > 500) {
        strcpy(out_module_name, "file_beacon");
        return EDR_OK;
    }

    // Large files + good bandwidth → HTTP
    if (filesize > 50*1024*1024 && net->bandwidth_mbps > 10) {
        strcpy(out_module_name, "file_http");
        return EDR_OK;
    }

    // Default to beacon (safer)
    strcpy(out_module_name, "file_beacon");
    return EDR_OK;
}
```

## ML Model Integration

The RL beacon agent can learn which transport minimizes transfer time:

```python
# State includes: file_size, network_latency, bandwidth_available
state = [
    file_size / 1e9,          # Normalized MB
    network_latency / 1000.0, # Normalized ms
    bandwidth_available,      # Mbps
]

# Actions: 0=file_beacon, 1=file_http
action = agent.select_action(state)
selected_transport = ["file_beacon", "file_http"][action]

# Reward: -(transfer_time_seconds)
# Agent learns: fast transfers = high reward
reward = -transfer_time
```

## Loading Both Modules

```c
// In dispatcher initialization
edr_dispatcher_load_plugin(dispatcher, "output/file_beacon_comm.dll");
edr_dispatcher_load_plugin(dispatcher, "output/file_http_comm.dll");

// Both now available for routing
edr_dispatcher_list_comm_modules(dispatcher, modules, cap);
// Output: [file_beacon, file_http, https, vpn, dns, ...]
```

## TUI Routing

When user initiates file transfer:

```rust
// 1. Send request to dispatcher
dispatcher.dispatch(FileTransferRequest {
    local_path,
    remote_path,
    preferred_module: None,  // Let dispatcher choose
});

// 2. Dispatcher consults ML model or heuristics
let selected = select_file_transport(file_size, network_conditions);

// 3. Dispatcher routes through selected module
edr_dispatcher_switch_comm_module(dispatcher, selected);

// 4. File transfer proceeds with chosen transport
```

## Build Instructions

```bash
# Build File Beacon module
cd modules/Comm/FileBeacon
bash build.sh
# Output: output/file_beacon_comm.dll

# Build File HTTP module  
cd modules/Comm/FileHTTP
bash build.sh
# Output: output/file_http_comm.dll
```

## Testing

```bash
# Load both modules
./cynosure_tui

# In TUI, when selecting file transfer:
# - Small test file (<10MB) → beacon
# - Large test file (>100MB) → HTTP
# - Observe which module is selected in dispatcher logs
```

## Implementation Notes

1. **Both modules implement `edr_iface_comm_t`** — same interface as HTTPS/DNS/VPN
2. **Pluggable** — dispatcher dynamically loads/unloads
3. **Stateful** — maintain transfer progress per session
4. **Timeouts** — abort transfers after 5 minutes idle
5. **Error handling** — return appropriate EDR_ERR codes

## Future Enhancements

- [ ] Resume partial transfers (HTTP range requests)
- [ ] Compression before transfer (gzip, brotli)
- [ ] Parallel uploads (split file into chunks, upload concurrently)
- [ ] Streaming transfer (process file as it arrives, no disk buffering)
- [ ] Integrity checking (MD5/SHA256 verification)
