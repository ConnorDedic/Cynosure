# Deploy File Transfer - Quick Start

## Step 1: Copy New Implant to Agent

The file transfer handlers are now implemented in the dispatcher. You need to deploy the **new implant.exe**:

```powershell
# On Windows agent (DESKTOP-M919A7K)
# Download the new implant from your C2 server
# Or copy it directly to the agent directory:

C:\> copy \\[TUI-SERVER]\implant.exe C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\implant.exe

# Kill the old one if running
C:\> taskkill /IM implant3.exe /F

# Start the new implant
C:\> C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\implant.exe
```

## Step 2: Verify New Implant is Running

In the TUI, you should see:
```
Sessions: 2 active  ← Confirm agent beacon is still coming in
```

Check that the new implant.exe is loaded and communicating:
```
[*] Listeners active on :4444
[*] Implant connected: agent-XXXX (DESKTOP-M919A7K)
```

## Step 3: Test File Upload

In the TUI:
1. Press `[c]` → Select "upload"
2. Enter local path: `/home/branis/Cynosure/upload_test.txt`
3. Enter remote path: `C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\upload_test.txt`
4. Press `[Enter]` twice
5. Watch progress bar animate
6. On Windows agent, check:
   ```powershell
   C:\> ls C:\Users\MALDEV01\Desktop\Maldev-code\cynosure\
   ```
   **You should see the uploaded file!**

## Step 4: Test File Download

In the TUI:
1. Press `[c]` → Select "download"
2. Enter remote path: `C:\Users\MALDEV01\Desktop\cynosure_vpn_comm.dll`
3. File will be saved to current working directory on TUI machine
4. Check:
   ```bash
   $ ls -la cynosure_vpn_comm.dll
   ```

## What's Working Now

✅ **File Upload (file-send)**
- TUI reads local file
- Implant receives chunks via beacon
- Implant reconstructs and writes to disk

✅ **File Download (file-recv)**
- Implant reads file from disk  
- Sends chunks via beacon to TUI
- TUI receives and reconstructs

✅ **Progress Tracking**
- Real-time progress bar in TUI
- Status updates showing "Upload/Download sent to agent"
- File-chunk messages logged to stderr

## Troubleshooting

### File doesn't appear on agent
1. Verify new implant.exe is running (check process list)
2. Check TUI shows correct file-send command in logs
3. Verify paths are correct (Windows absolute paths like `C:\...`)

### File transfer hangs
1. Check network connectivity (beacon working?)
2. Verify file exists and is readable
3. Check file permissions
4. Implant should log any errors to stderr

### Large files slow
- File transfer uses 4KB chunks + base64 encoding
- Expected overhead ~33% (base64)
- For large files, consider file_http_comm module (future enhancement)

## Architecture

```
TUI (user initiates upload)
  ↓
Sends: file-send /local /remote
  ↓
Dispatcher receives & routes
  ↓
cmd_file_send() handler:
  - Opens /local
  - Reads 4KB chunks
  - Base64 encodes
  - Sends via beacon
  ↓
Beacon delivers to TUI
  ↓
TUI receives file-chunk messages
  ↓
TUI reassembles and writes to /remote
```

## Next Steps

- [ ] Deploy new implant.exe to agent
- [ ] Test file upload  
- [ ] Test file download
- [ ] Monitor for any errors
- [ ] Implement HTTP module for faster large-file transfers
- [ ] Add compression to reduce payload size

---

**Important:** The new implant.exe (62KB, built 23:46) has the file transfer handlers. The old implant3.exe will not support file transfers!
