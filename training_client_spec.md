# Training Client Specification (Agent-Side)

## Overview

The Training Client (`training_client.c`) runs on agents compiled with the `TRAINING_MODE` flag. It operates as a lightweight, asynchronous component that:
- Connects to the training server on port 5556
- Collects beacon telemetry from the dispatcher
- Sends telemetry packets at configurable intervals
- Receives feedback and acts on recommendations
- Maintains resilient connection state with automatic reconnection
- Logs all events for audit and analysis

This document specifies exact byte layouts, state machines, and integration points.

---

## Component Architecture

### File Structure
```
src/implant/training_client.c         (NEW - ~500 lines)
src/implant/training_client.h         (NEW - public API)
src/implant/edr_dispatcher.c          (MODIFIED - metrics integration)
src/implant/edr_dispatcher.h          (MODIFIED - metrics callback)
```

### Dependencies
- POSIX sockets (for cross-platform `WSASocket` on Windows, `socket()` on Linux)
- Thread synchronization (mutex, condition variable)
- Standard C library (string.h, stdio.h, time.h)
- No external dependencies (no libcurl, no crypto libraries — TLS handled by OS)

### Integration Points
1. **From edr_dispatcher.c**: Metrics hook callback when beacon succeeds/fails
2. **To RL Server**: TCP socket on localhost:5556 (or configurable IP)
3. **To Log**: File-based logging to `/tmp/training_agent_XXXXXX.log`

---

## Data Structures

### Training Client Handle

```c
typedef struct {
    /* Connection state */
    int socket;                          /* TCP socket fd, -1 if disconnected */
    char server_ip[64];                  /* Training server IP (default "127.0.0.1") */
    uint16_t server_port;                /* Training server port (default 5556) */
    
    /* Agent identity */
    char agent_id[64];                   /* Copied from edr_agent_identity_t */
    
    /* Metrics accumulation */
    uint64_t total_beacons;              /* Cumulative beacon count */
    uint64_t successful_beacons;         /* Count of successful beacons */
    uint64_t failed_beacons;             /* Count of failed/timed-out beacons */
    uint64_t detected_count;             /* Count of detected events (from server feedback) */
    
    /* Current transport state */
    char current_transport[32];          /* "vpn", "https", "dns" */
    uint32_t beacon_interval;            /* Milliseconds between beacons */
    
    /* Last beacon metrics */
    uint32_t last_latency_ms;            /* RTT of last beacon */
    uint32_t last_packet_size;           /* Bytes sent/received in last beacon */
    uint64_t last_beacon_timestamp;      /* Unix timestamp (seconds) of last beacon */
    
    /* Network condition inference */
    char network_condition[32];          /* "normal", "congested", "unstable", "unknown" */
    uint32_t recent_failures;            /* Failures in last 100 beacons */
    
    /* Telemetry tracking */
    uint32_t telemetry_interval;         /* Send telemetry every N beacons (default 10) */
    uint32_t telemetry_counter;          /* Counter toward next telemetry send */
    
    /* Connection state machine */
    enum {
        STATE_DISCONNECTED = 0,
        STATE_CONNECTING = 1,
        STATE_CONNECTED = 2,
        STATE_SENDING = 3,
        STATE_RECEIVING = 4
    } connection_state;
    
    /* Reconnection logic */
    uint64_t last_connect_attempt;       /* Timestamp of last connection attempt */
    uint32_t connect_backoff_ms;         /* Backoff delay (starts at 1000ms, max 60000ms) */
    
    /* Thread synchronization */
    pthread_mutex_t metrics_lock;        /* Protects all metrics fields */
    
    /* Logging */
    FILE *log_file;                      /* File pointer for training log */
    
} training_client_t;
```

### Telemetry Packet Format

**Total size: 256 bytes** (fixed for simplicity)

```
Byte Offset | Field Name          | Type    | Size | Description
------------+---------------------+---------+------+--------------------------------------------
0-3         | PACKET_TYPE         | uint32  | 4    | 0x01 = telemetry packet
4-7         | PROTOCOL_VERSION    | uint32  | 4    | 0x01000000 (v1.0.0)
8-71        | AGENT_ID            | char[64]| 64   | Null-terminated agent UUID
72-79       | BEACON_COUNT        | uint64  | 8    | Total beacons sent (cumulative)
80-87       | SUCCESS_COUNT       | uint64  | 8    | Successful beacons
88-95       | FAILED_COUNT        | uint64  | 8    | Failed/timeout beacons
96-103      | DETECTED_COUNT      | uint64  | 8    | Detection events
104-135     | CURRENT_TRANSPORT   | char[32]| 32   | Null-terminated: "vpn"|"https"|"dns"
136-139     | BEACON_INTERVAL_MS  | uint32  | 4    | Milliseconds between beacons
140-143     | LAST_LATENCY_MS     | uint32  | 4    | RTT of last beacon (0 if none)
144-147     | LAST_PACKET_SIZE    | uint32  | 4    | Size of last beacon payload
148-155     | TIMESTAMP_SEC       | uint64  | 8    | Unix timestamp (seconds) when sent
156-187     | NETWORK_CONDITION   | char[32]| 32   | "normal"|"congested"|"unstable"|"unknown"
188-191     | RECENT_FAILURES     | uint32  | 4    | Failures in last 100 beacons
192-255     | RESERVED            | uint8[64]| 64   | Padding for future expansion
```

**Key constraints:**
- All multi-byte integers are **network-byte-order (big-endian)**
- Strings are **null-terminated** (do not rely on field size)
- If a field is not applicable, use empty string ("") or 0
- Timestamp should be `time(NULL)` (Unix epoch in seconds)

### Feedback Packet Format

**Total size: 128 bytes** (fixed)

```
Byte Offset | Field Name          | Type    | Size | Description
------------+---------------------+---------+------+--------------------------------------------
0-3         | PACKET_TYPE         | uint32  | 4    | 0x02 = feedback packet
4-7         | PROTOCOL_VERSION    | uint32  | 4    | 0x01000000 (v1.0.0)
8-11        | FEEDBACK_TYPE       | uint32  | 4    | 1=success, 2=detected, 3=traffic_pattern
12-15       | DETECTION_LIKELIHOOD| float   | 4    | 0.0-1.0, confidence in detection risk
16-47       | RECOMMENDED_TRANSPORT| char[32]| 32   | Null-term: "vpn"|"https"|"dns"|"none"
48-51       | RECOMMENDED_INTERVAL| uint32  | 4    | Milliseconds (0 = keep current)
52-55       | CONFIDENCE          | float   | 4    | 0.0-1.0, confidence in recommendation
56-127      | RESERVED            | uint8[72]| 72   | Padding for future expansion
```

**Key constraints:**
- Feedback is **server-to-client only**
- Confidence = 0 means low confidence (agent may ignore)
- Confidence = 1.0 means high confidence (agent should follow)
- Recommended interval of 0 = maintain current interval

### Metrics Update Structure (Internal)

```c
typedef struct {
    /* What changed */
    enum {
        METRIC_BEACON_SUCCESS = 1,
        METRIC_BEACON_FAILURE = 2,
        METRIC_DETECTION_EVENT = 3,
        METRIC_TRANSPORT_CHANGE = 4,
        METRIC_INTERVAL_CHANGE = 5
    } metric_type;
    
    /* When */
    uint64_t timestamp;
    
    /* Context */
    uint32_t latency_ms;                 /* For beacon events */
    uint32_t packet_size;                /* For beacon events */
    char transport[32];                  /* For transport change events */
    uint32_t new_interval;               /* For interval change events */
    
} metrics_update_t;
```

---

## Public API (training_client.h)

### Initialization

```c
/*
 * training_client_create
 *   Allocate a new training client.
 *   Returns NULL on allocation failure.
 *   Does NOT connect to server (that happens in _start).
 */
training_client_t *training_client_create(
    const char *agent_id,          /* Copy from edr_agent_identity_t.agent_id */
    const char *server_ip,         /* Training server IP, NULL = "127.0.0.1" */
    uint16_t server_port           /* Training server port, 0 = 5556 */
);

/*
 * training_client_start
 *   Spawn background thread that manages connection and telemetry.
 *   This thread automatically reconnects on failure.
 *   Does NOT block; spawns worker thread.
 */
edr_status_t training_client_start(training_client_t *client);

/*
 * training_client_stop
 *   Signal background thread to shut down gracefully.
 *   Flushes any pending telemetry before closing.
 *   Blocks until thread exits (max 5 second timeout).
 */
edr_status_t training_client_stop(training_client_t *client);

/*
 * training_client_destroy
 *   Free all resources.
 *   Must call training_client_stop() first.
 */
void training_client_destroy(training_client_t *client);
```

### Metrics Reporting

```c
/*
 * training_client_report_beacon
 *   Called by edr_dispatcher when a beacon is sent.
 *   Thread-safe; can be called from any thread.
 */
edr_status_t training_client_report_beacon(
    training_client_t *client,
    int success,                   /* 1 = success, 0 = failure */
    uint32_t latency_ms,           /* Response time in ms */
    uint32_t packet_size,          /* Bytes in beacon payload */
    const char *transport          /* "vpn", "https", "dns" */
);

/*
 * training_client_report_detection
 *   Called when server sends detection feedback.
 *   Thread-safe.
 */
edr_status_t training_client_report_detection(
    training_client_t *client,
    int detected,                  /* 1 = detected, 0 = not detected */
    double confidence              /* 0.0-1.0 */
);

/*
 * training_client_set_transport
 *   Update current transport in metrics.
 *   Called when beacon transport changes.
 *   Thread-safe.
 */
edr_status_t training_client_set_transport(
    training_client_t *client,
    const char *transport          /* "vpn", "https", "dns" */
);

/*
 * training_client_set_interval
 *   Update beacon interval.
 *   Called when evasion logic changes beacon timing.
 *   Thread-safe.
 */
edr_status_t training_client_set_interval(
    training_client_t *client,
    uint32_t interval_ms           /* Milliseconds between beacons */
);
```

### Feedback Reading (Optional — for adaptive behavior)

```c
/*
 * training_client_get_feedback
 *   Non-blocking: retrieve last feedback from server (if any).
 *   Returns NULL if no feedback since last call.
 *   Caller must NOT free returned pointer; valid until next call.
 */
const struct {
    char recommended_transport[32];
    uint32_t recommended_interval;
    float confidence;
} *training_client_get_feedback(training_client_t *client);

/*
 * training_client_has_feedback
 *   Quick check: returns 1 if new feedback is available, 0 otherwise.
 *   Non-blocking.
 */
int training_client_has_feedback(training_client_t *client);
```

---

## Connection State Machine

### States and Transitions

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  DISCONNECTED ──┐                                           │
│      │          │                                           │
│      │ (manual) │ (error)                                   │
│      │          │                                           │
│      └─────────┴──► CONNECTING ──error──┐                   │
│                          │               │                  │
│                   (connected)   (backoff timeout)           │
│                          │               │                  │
│                          ▼               │                  │
│                    CONNECTED ◄───────────┘                  │
│                          │ │                                │
│                    send  │ │ receive                        │
│                          ▼ ▼                                │
│                        SENDING ──timeout──┐                │
│                        RECEIVING        (error)            │
│                          │ │               │                │
│                    ─────┘ └─────────────────┘               │
│                    │                                       │
│                    └──► DISCONNECTED (manual stop)         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### State Behaviors

**DISCONNECTED:**
- Initial state
- No socket open
- Waiting for start command or automatic reconnect

**CONNECTING:**
- Attempting TCP connection to server_ip:server_port
- Timeout: 10 seconds
- On failure: transition to DISCONNECTED, apply exponential backoff
- On success: transition to CONNECTED

**CONNECTED:**
- Socket open and ready
- Waiting for telemetry interval or feedback data
- Listening on socket with 5-second select() timeout

**SENDING:**
- Serialized telemetry packet queued to socket
- Max 5 retries on partial send
- On send_all complete: transition back to CONNECTED
- On error: close socket, go to DISCONNECTED

**RECEIVING:**
- Blocking recv() for feedback packet (max 128 bytes)
- Timeout: 5 seconds
- On complete packet: parse and cache, return to CONNECTED
- On partial: accumulate in buffer
- On error: close socket, go to DISCONNECTED

---

## Telemetry Transmission Logic

### Trigger Conditions

Send telemetry when **any** of these conditions is met:

1. **Beacon Count Threshold**: `beacon_counter >= telemetry_interval` (default 10)
2. **Detection Event**: Immediately when server sends detection feedback
3. **Transport Change**: Immediately when transport switches
4. **Interval Change**: Immediately when beacon interval changes
5. **Time Threshold**: Every 60 seconds, send at least one telemetry (even if no beacons)

### Transmission Process

```c
/*
 * Internal worker thread function (pseudocode)
 */
void *training_client_worker_thread(void *arg) {
    training_client_t *client = (training_client_t *)arg;
    
    while (client->running) {
        
        /* Check if reconnection is needed */
        if (client->connection_state == STATE_DISCONNECTED) {
            if (time(NULL) - client->last_connect_attempt > client->connect_backoff_ms / 1000) {
                client->connection_state = STATE_CONNECTING;
                if (tcp_connect(client) == 0) {
                    client->connection_state = STATE_CONNECTED;
                    client->connect_backoff_ms = 1000;  /* Reset backoff */
                } else {
                    client->connection_state = STATE_DISCONNECTED;
                    client->connect_backoff_ms = MIN(client->connect_backoff_ms * 2, 60000);
                    client->last_connect_attempt = time(NULL);
                }
            }
            sleep(1);
            continue;
        }
        
        /* Check if telemetry should be sent */
        pthread_mutex_lock(&client->metrics_lock);
        int should_send = (
            client->telemetry_counter >= client->telemetry_interval ||
            (time(NULL) - client->last_telemetry_send > 60)
        );
        pthread_mutex_unlock(&client->metrics_lock);
        
        if (should_send && client->connection_state == STATE_CONNECTED) {
            /* Serialize telemetry packet */
            uint8_t packet[256];
            int len = serialize_telemetry_packet(client, packet);
            
            /* Send with retries */
            client->connection_state = STATE_SENDING;
            if (send_all(client->socket, packet, len, 5000) == len) {
                /* Success: update metrics */
                pthread_mutex_lock(&client->metrics_lock);
                client->telemetry_counter = 0;
                client->last_telemetry_send = time(NULL);
                pthread_mutex_unlock(&client->metrics_lock);
                client->connection_state = STATE_CONNECTED;
            } else {
                /* Failure: close and reconnect */
                close(client->socket);
                client->socket = -1;
                client->connection_state = STATE_DISCONNECTED;
            }
        }
        
        /* Listen for feedback */
        if (client->connection_state == STATE_CONNECTED) {
            client->connection_state = STATE_RECEIVING;
            uint8_t feedback_buf[128];
            int bytes = recv_with_timeout(client->socket, feedback_buf, 128, 5000);
            
            if (bytes == 128) {
                /* Parse feedback packet */
                parse_feedback_packet(client, feedback_buf);
                client->connection_state = STATE_CONNECTED;
            } else if (bytes == 0) {
                /* Server closed connection */
                close(client->socket);
                client->socket = -1;
                client->connection_state = STATE_DISCONNECTED;
            } else if (bytes < 0) {
                /* Timeout or error */
                client->connection_state = STATE_CONNECTED;  /* Keep trying */
            }
        }
        
        sleep(1);  /* Avoid busy-waiting */
    }
    
    return NULL;
}
```

### Serialize/Deserialize Functions

```c
/*
 * serialize_telemetry_packet
 *   Packs all metrics into fixed 256-byte format (big-endian).
 *   Returns 256 (always).
 */
int serialize_telemetry_packet(training_client_t *client, uint8_t *out);

/*
 * parse_feedback_packet
 *   Unpacks 128-byte feedback packet.
 *   Stores recommendations in client->pending_feedback.
 *   Returns 0 on success, -1 on parse error.
 */
int parse_feedback_packet(training_client_t *client, const uint8_t *buf);
```

---

## Error Handling & Resilience

### Connection Failures

| Scenario | Action | Backoff |
|----------|--------|---------|
| Initial connect fails | Retry every N seconds | 1s→2s→4s→...→60s |
| Send timeout | Close, reconnect | Same backoff |
| Recv timeout | Stay connected, retry | None (keep-alive) |
| Server closes | Reconnect immediately | Reset to 1s |
| Thread crash | Auto-restart (if watchdog enabled) | N/A |

### Metrics Loss

- **Telemetry sent but server doesn't ACK**: Metrics are reset; that data is lost (acceptable for training)
- **Metrics buffer overflow**: Circular buffer; oldest entries overwritten (max 1000 entries)
- **Disconnect during send**: Partial data lost; next telemetry will have updated cumulative counts

### Logging

All events logged to `/tmp/training_agent_<PID>.log`:

```
[2024-06-27 14:23:45.123] INFO: Training client started (agent=abc123def)
[2024-06-27 14:23:46.234] INFO: Connected to training server (127.0.0.1:5556)
[2024-06-27 14:23:50.456] DEBUG: Beacon success (latency=450ms, transport=dns)
[2024-06-27 14:24:00.567] INFO: Telemetry sent (beacons=10, success=9, detected=0)
[2024-06-27 14:24:00.678] DEBUG: Feedback received (transport=https, confidence=0.65)
[2024-06-27 14:25:00.789] WARN: Connection timeout, reconnecting...
[2024-06-27 14:25:02.890] ERROR: Failed to connect (backoff=2000ms)
```

### Log Rotation

- Max file size: 10 MB
- Keep last 5 rotated files
- Format: `/tmp/training_agent_<PID>.log.0`, `.log.1`, etc.

---

## Integration with edr_dispatcher.c

### Hook Points

**In `edr_dispatcher_start()`:**
```c
/* After dispatcher init, before main loop */
if (getenv("TRAINING_MODE") != NULL) {
    training_client_t *training = training_client_create(
        dispatcher->identity.agent_id,
        getenv("TRAINING_SERVER_IP") ?: "127.0.0.1",
        atoi(getenv("TRAINING_SERVER_PORT") ?: "5556")
    );
    training_client_start(training);
    dispatcher->training_client = training;  /* Store handle */
}
```

**In beacon success path (e.g., after `send_beacon()` completes):**
```c
if (dispatcher->training_client != NULL) {
    training_client_report_beacon(
        dispatcher->training_client,
        1,  /* success */
        elapsed_ms,
        payload_size,
        current_transport  /* "vpn", "https", "dns" */
    );
}
```

**In beacon failure path (e.g., after timeout or send error):**
```c
if (dispatcher->training_client != NULL) {
    training_client_report_beacon(
        dispatcher->training_client,
        0,  /* failure */
        elapsed_ms,
        0,  /* no payload sent */
        current_transport
    );
}
```

**In `edr_dispatcher_stop()`:**
```c
if (dispatcher->training_client != NULL) {
    training_client_stop(dispatcher->training_client);
    training_client_destroy(dispatcher->training_client);
}
```

---

## Testing Approach

### Unit Tests

```bash
# Test telemetry serialization
gcc -o test_telemetry test_telemetry.c training_client.c -lpthread
./test_telemetry

# Test connection state machine
gcc -o test_state test_state_machine.c training_client.c -lpthread
./test_state

# Test metrics accumulation
gcc -o test_metrics test_metrics.c training_client.c -lpthread
./test_metrics
```

### Integration Tests

1. **Manual**: Run training server on localhost:5556, agent connects
   ```bash
   python3 src/training_server.py &
   ./implant training_mode=1
   # Check /tmp/training_agent_*.log for telemetry events
   ```

2. **Automated**: Mock training server that replies with predefined feedback
   ```bash
   gcc -o test_integration test_integration.c training_client.c -lpthread
   ./test_integration  # Spawns mock server, runs agent, validates flow
   ```

3. **Stress Test**: 100 concurrent beacons, verify no race conditions
   ```bash
   ./test_integration --threads=100 --duration=60s
   ```

---

## Platform-Specific Notes

### Windows (MinGW cross-compile)

```c
#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define close(fd) closesocket(fd)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif
```

### macOS

- Uses same POSIX API as Linux
- Log file location: `/tmp/training_agent_<PID>.log` (same)
- No special handling needed

### Linux

- Standard glibc, no special handling
- Works with systemd if implant runs as service

---

## Performance Characteristics

| Metric | Target | Tolerance |
|--------|--------|-----------|
| Telemetry send latency | <100ms | ±50ms |
| Memory overhead | <5MB | ±1MB |
| CPU usage (idle) | <1% | ±0.5% |
| Connection establish | <2s | ±1s |
| Reconnect backoff max | 60s | ±10s |

---

## Security Considerations

### What is NOT protected

- **No encryption**: Telemetry sent over plain TCP (use firewall/VPN for transport security)
- **No authentication**: Server accepts any agent_id (trust the network boundary)
- **No integrity check**: No HMAC or signing (plaintext in trusted environment)

### What IS protected

- **Metrics isolation**: Each agent's metrics protected by mutex (no cross-agent leakage)
- **Buffer overflow prevention**: Fixed-size packets (256 bytes telemetry, 128 bytes feedback)
- **Connection state validation**: No command injection via feedback (only structured fields)

### Recommendations

- Run training server on private/VPN network only
- Use firewall rules to restrict port 5556 to approved agents
- Monitor `/tmp/training_agent_*.log` for suspicious patterns
- Disable training mode in production builds

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-06-27 | Initial specification |

---

## Appendix: Example Telemetry Packet (Hex Dump)

```
Offset  0 1 2 3  4 5 6 7  8 9 A B  C D E F
------  -----  -----  -----  -----
0000    01 00 00 00  01 00 00 00  61 62 63 31  32 33 64 65
        ^PACKET_TYPE  ^PROTO_VER  ^---- AGENT_ID ----
0010    66 2d 34 35  36 37 38 39  00 00 00 00  00 00 00 00
        ^---- AGENT_ID (continued) ----
0020    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
0030    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
        ^padding to byte 72
0040    00 00 00 00  00 00 00 96  00 00 00 00  00 00 00 78
        ^BEACON_COUNT=150 ^SUCCESS_COUNT=120
0050    00 00 00 00  00 00 00 05  64 6e 73 00  00 00 00 00
        ^FAILED_COUNT ^DETECTED ^TRANSPORT="dns"
0060    00 00 00 00  00 00 00 1e  00 00 01 c4  00 00 02 00
        ^^^^^^^^^^^^^^^^^^^^^^^^^^ ^INTERVAL^LATENCY  ^PKTSIZE
0070    00 00 00 00  66 6b 6b 6b  6e 6f 72 6d  61 6c 00 00
        ^TIMESTAMP  ^NETWORK_COND="normal"
0080    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
0090    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
...
00FF    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
        ^padding to byte 256
```

---

## Appendix: Example Feedback Packet (Hex Dump)

```
Offset  0 1 2 3  4 5 6 7  8 9 A B  C D E F
------  -----  -----  -----  -----
0000    02 00 00 00  01 00 00 00  3f a2 8f 5c  68 74 74 70
        ^PACKET_TYPE  ^PROTO_VER  ^LIKELIHOOD  ^TRANSPORT="http"
0010    73 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
        ^TRANSPORT (continued)
0020    00 00 00 00  00 00 00 00  00 00 00 00  00 01 0e 00
        ^^^^^^^^^^^^^^TRANSPORT padding  ^RECOMMENDED_INTERVAL
0030    3f 52 f0 e5  00 00 00 00  00 00 00 00  00 00 00 00
        ^CONFIDENCE  ^RESERVED padding
...
0070    00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
```

