# Cynosure RL Beacon Optimization Model

## Overview

The Cynosure framework includes a **Deep Q-Learning (DQN)** model that automatically optimizes beacon timing for implants. The model learns when and how to beacon to maximize connectivity while minimizing detection risk.

**Status**: ✓ FULLY FUNCTIONAL with C2 evasion tuning parameters

---

## Model Architecture

### Deep Q-Network (DQN)

- **Input (State)**: 6-dimensional vector
  - Hour of day (0-1 normalized)
  - Day of week (0-1 normalized)  
  - Recent beacon success rate (0-1)
  - Implant uptime (0-1)
  - Time since last beacon (0-1)
  - Transport method (vpn/https/dns)

- **Output (Action)**: 24 discrete actions
  - 6 beacon intervals: 5s, 10s, 30s, 60s, 120s, 300s
  - 4 retry counts: 1, 2, 3, 5
  - Total combinations: 6 × 4 = 24 actions

- **Network**: 2-layer feedforward neural network
  - Input → 128 hidden units → 24 output units
  - ReLU activation
  - Uses target network for stable training

### Learning Algorithm

- **Algorithm**: Double DQN with experience replay
- **Optimizer**: Adam (learning rate 1e-3)
- **Discount factor (γ)**: 0.99
- **Epsilon-greedy exploration**: Start 1.0 → Decay to 0.01
- **Experience replay buffer**: 10,000 transitions

---

## C2 Detection Evasion Features

### Evasion Configuration

The model includes tunable evasion parameters (see `rl_evasion_config.py`):

```python
STEALTH_WEIGHT = 0.6              # Balance stealth vs connectivity (0-1)
JITTER_RANGE = 10                 # ±seconds added to intervals
TRANSPORT_SWITCH_PROB = 0.3       # Probability of switching transports
AVOID_PEAK_HOURS = True           # Skip beaconing 9-5
PAYLOAD_VARIANCE = 100            # Random padding to avoid signatures
```

### Evasion Profiles

Pre-configured profiles for different scenarios:

1. **connectivity** - Prioritize reliability
   - STEALTH_WEIGHT: 0.2
   - JITTER_RANGE: 2
   - TRANSPORT_SWITCH_PROB: 0.1

2. **balanced** - Default (recommended)
   - STEALTH_WEIGHT: 0.6
   - JITTER_RANGE: 10
   - TRANSPORT_SWITCH_PROB: 0.3

3. **aggressive** - Maximum detection evasion
   - STEALTH_WEIGHT: 0.9
   - JITTER_RANGE: 30
   - TRANSPORT_SWITCH_PROB: 0.7

### Reward Function with Evasion

The reward function balances:

```
reward = connectivity_reward 
       + stealth_penalty 
       + transport_evasion_bonus
       + success_rate_protection
```

- **Connectivity**: +10 for success, -5 for failure
- **Stealth**: Penalizes predictable intervals, rewards varied timing
- **Transport**: Prefers DNS (1.5x) > HTTPS (1.0x) > VPN (0.5x)
- **Detection**: If success rate drops, reduces stealth to regain connectivity

---

## API Endpoints

The RL service runs on `http://localhost:5555` with these endpoints:

### Beacon Actions
```
POST /beacon/action
Body: {
    "implant_id": "agent-1234",
    "success_rate": 0.95,
    "uptime": 0.99,
    "seconds_since_beacon": 30,
    "transport": "vpn"
}

Response: {
    "beacon_interval": 30,
    "retry_count": 2,
    "transport": "vpn",
    "confidence": 0.85,
    "action_idx": 5
}
```

### Beacon Feedback
```
POST /beacon/feedback
Body: {
    "implant_id": "agent-1234",
    "success": true,
    "response_time": 0.5,
    "beacon_interval": 30
}

Response: {
    "reward": 11.5,
    "processed": true
}
```

### Model Metrics
```
GET /model/metrics

Response: {
    "step_count": 1234,
    "epsilon": 0.87,
    "avg_loss": 0.45,
    "success_rate": 0.95,
    "successful_beacons": 95,
    "failed_beacons": 5,
    "memory_size": 432,
    "current_episode_reward": 45.2
}
```

### Evasion Configuration (NEW)
```
GET /evasion/config
Response: {
    "STEALTH_WEIGHT": 0.6,
    "JITTER_RANGE": 10,
    "TRANSPORT_SWITCH_PROB": 0.3,
    ...
}

POST /evasion/config
Body: {
    "STEALTH_WEIGHT": 0.8,
    "JITTER_RANGE": 20
}
Response: {
    "updated": {...},
    "config": {...}
}
```

### Evasion Profiles (NEW)
```
GET /evasion/profiles

Response: {
    "profiles": {
        "connectivity": {
            "description": "Prioritize reliability over stealth",
            "config": {...}
        },
        "balanced": {...},
        "aggressive": {...}
    }
}
```

---

## Training Workflow

### 1. Start the Service
```bash
python3 src/rl_beacon_service.py
```

### 2. Run Beacon Simulation
```bash
python3 test_rl_agent.py
```

This will:
- Query the beacon action endpoint 20 times
- Provide random feedback (90% success rate)
- Train the model every 4 beacons
- Display final metrics

### 3. Monitor Training
```bash
curl http://localhost:5555/model/metrics
```

### 4. Tune Evasion Parameters
```bash
# Get current config
curl http://localhost:5555/evasion/config

# Switch to aggressive profile
curl -X POST http://localhost:5555/evasion/config \
  -H "Content-Type: application/json" \
  -d '{"STEALTH_WEIGHT": 0.9}'
```

---

## Integration with C2

Currently, the RL service runs independently on port 5555. To integrate with the main C2:

1. **Option A**: Have the Rust listener query the RL service HTTP endpoints
2. **Option B**: Embed the Python model in a native library (PyO3)
3. **Option C**: Use the RL agent as a separate service that implants call

Recommended: **Option A** (HTTP query to port 5555)

---

## Future Enhancements

### 1. Advanced Detection Evasion
- Learn transport switching from observed EDR signatures
- Adapt beacon intervals based on network traffic patterns
- Implement decoy beacons to confuse IDS

### 2. Multi-Agent Coordination
- Different evasion strategies per implant
- Coordinate beacon times to avoid synchronized patterns

### 3. Adversarial Learning
- Train against simulated IDS detection model
- Minimax optimization: implant vs defense

### 4. Custom Reward Functions
- Per-environment reward shaping
- User-defined detection likelihood models
- Integration with threat intel feeds

---

## Files

| File | Purpose |
|------|---------|
| `src/rl_beacon_agent.py` | DQN agent implementation |
| `src/rl_beacon_service.py` | HTTP API server |
| `src/rl_evasion_config.py` | Evasion parameters & profiles |
| `test_rl_agent.py` | Test suite and simulator |

---

## Quick Start: Tune for Your Network

1. **Default (Balanced)**
   ```
   No action needed - uses STEALTH_WEIGHT=0.6
   ```

2. **High Priority: Keep Connectivity**
   ```bash
   curl -X POST http://localhost:5555/evasion/config \
     -d '{"STEALTH_WEIGHT": 0.3, "JITTER_RANGE": 5}'
   ```

3. **High Priority: Avoid Detection**
   ```bash
   curl -X POST http://localhost:5555/evasion/config \
     -d '{"STEALTH_WEIGHT": 0.9, "TRANSPORT_SWITCH_PROB": 0.8}'
   ```

4. **Adapt Based on Results**
   - Monitor `/model/metrics` success rate
   - If success < 70%, reduce STEALTH_WEIGHT
   - If beacons are detected, increase STEALTH_WEIGHT

---

## Status Report

✓ **RL Model**: Fully functional DQN implementation  
✓ **Evasion Config**: Tunable parameters for C2 detection evasion  
✓ **HTTP API**: Full endpoints for querying and feedback  
✓ **Testing**: Complete test suite and simulator  
✓ **Profiles**: 3 pre-configured evasion profiles  

**Ready for**: Custom tuning and adversarial training
