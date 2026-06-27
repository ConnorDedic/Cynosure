# Training Server Specification

## Overview

The Training Server (`training_server.py`) runs on the operator's machine and coordinates the RL training pipeline. It:
- Listens on port 5556 for telemetry from agents
- Parses and validates telemetry packets
- Simulates network detection scenarios
- Computes training rewards
- Stores experiences in the RL model's replay buffer
- Sends feedback to agents
- Exposes HTTP APIs for TUI integration
- Periodically triggers RL model training steps

This specification details the server architecture, detection simulation rules, reward computation, and API contracts.

---

## Component Architecture

### File Structure
```
src/training_server.py              (NEW - ~800 lines)
src/training_detection_sim.py        (NEW - ~400 lines)
src/training_reward_model.py         (NEW - ~300 lines)
start_rl_service.sh                 (MODIFIED - add training_server)
requirements-rl.txt                 (MODIFIED - add dependencies)
```

### Dependencies
```
torch>=2.0.0
numpy>=1.21.0
requests>=2.28.0
flask>=2.0.0
```

### Integration Points
1. **From agents**: TCP socket listener on port 5556
2. **To RL model**: Import from `rl_beacon_agent.py`, call `store_experience()`, `train_step()`
3. **To TUI**: HTTP API on port 5557
4. **Detection simulator**: Internal module in same file

---

## Data Structures

### Server Configuration

```python
class TrainingServerConfig:
    """Configuration for training server"""
    
    # Network
    LISTEN_IP = "127.0.0.1"
    LISTEN_PORT = 5556
    HTTP_API_PORT = 5557
    MAX_CONNECTIONS = 100
    
    # Telemetry processing
    TELEMETRY_BUFFER_SIZE = 10000      # Max telemetry samples to hold
    BATCH_TRAINING_SIZE = 32           # Train on every N samples
    TRAINING_STEP_INTERVAL = 50        # Every 50 samples, call train_step()
    
    # Detection simulation
    BUSINESS_HOURS_START = 9           # 9 AM
    BUSINESS_HOURS_END = 17            # 5 PM (not inclusive)
    WEEKEND_DAYS = [5, 6]              # Saturday, Sunday
    
    # Reward tuning
    REWARD_BEACON_SUCCESS = 10.0
    REWARD_BEACON_FAILURE = -5.0
    REWARD_DETECTION_PENALTY = -20.0
    REWARD_FAST_LATENCY = 2.0          # <1000ms
    REWARD_SLOW_LATENCY = -3.0         # >5000ms
    REWARD_FREQUENCY_ANOMALY = -5.0    # Too fast or too slow
    
    # Logging
    LOG_LEVEL = logging.INFO
    LOG_FILE = "/tmp/training_server.log"
    LOG_MAX_SIZE_MB = 50
    LOG_BACKUP_COUNT = 5
```

### Telemetry Record (Normalized)

```python
@dataclass
class TelemetryRecord:
    """Normalized telemetry after parsing and validation"""
    
    # Identity
    agent_id: str                  # From packet
    
    # Beacon metrics
    total_beacons: int
    successful_beacons: int
    failed_beacons: int
    detected_count: int
    
    # Current transport & timing
    current_transport: str         # "vpn", "https", "dns"
    beacon_interval_ms: int
    
    # Network metrics
    last_latency_ms: int
    last_packet_size: int
    network_condition: str         # "normal", "congested", "unstable"
    recent_failures: int
    
    # Temporal
    timestamp: float               # Unix timestamp (seconds)
    hour_of_day: int               # 0-23
    day_of_week: int               # 0=Monday, 6=Sunday
    is_business_hours: bool
    
    # Computed fields
    success_rate: float            # successful / total
    failure_rate: float            # failed / total
    beacon_frequency: float        # beacons/hour (estimated)
```

### Agent Training State

```python
@dataclass
class AgentTrainingState:
    """Tracks per-agent training progression"""
    
    agent_id: str
    first_seen: float              # Timestamp of first telemetry
    last_telemetry: float          # Timestamp of most recent telemetry
    sample_count: int              # Total training samples generated
    last_action: dict              # Last recommended action (transport, interval)
    action_history: deque           # Last 20 actions (for pattern learning)
    
    # Metrics for this agent
    total_samples_trained: int
    avg_reward: float
    detection_likelihood: float    # Based on current transport/time
    
    # Feedback cache
    pending_feedback: dict         # To be sent on next agent connection
```

### Detection Simulation Rules

```python
class DetectionLikelihood:
    """Detection likelihood matrix by transport and time"""
    
    RULES = {
        # (transport, is_business_hours, is_weekend) → base_likelihood
        ("vpn", True, False):    0.80,   # VPN during business, weekday = very suspicious
        ("vpn", True, True):     0.70,   # VPN on weekend during daytime = suspicious
        ("vpn", False, False):   0.30,   # VPN evening weekday = lower risk
        ("vpn", False, True):    0.15,   # VPN evening weekend = normal
        
        ("https", True, False):  0.50,   # HTTPS during business = moderate
        ("https", True, True):   0.40,
        ("https", False, False): 0.20,   # HTTPS evening = low
        ("https", False, True):  0.10,
        
        ("dns", True, False):    0.20,   # DNS during business = stealthy
        ("dns", True, True):     0.15,
        ("dns", False, False):   0.05,   # DNS evening = almost normal
        ("dns", False, True):    0.02,
    }
    
    FREQUENCY_MULTIPLIERS = {
        # Beacon interval (seconds) → multiplier
        (0, 5):        2.0,        # Every 5s = very suspicious
        (5, 30):       1.5,        # Every 30s = fairly suspicious
        (30, 120):     1.0,        # Every 2min = baseline
        (120, 600):    0.8,        # Every 10min = less suspicious
        (600, 3600):   0.5,        # Every hour = very stealthy
        (3600, None):  1.2,        # Too infrequent = anomalous
    }
    
    def get_likelihood(self, transport, timestamp_sec, beacon_interval_ms):
        """
        Compute detection likelihood (0.0 - 1.0)
        
        Args:
            transport: "vpn", "https", "dns"
            timestamp_sec: Unix timestamp
            beacon_interval_ms: Milliseconds between beacons
        
        Returns:
            float: 0.0 (no detection) to 1.0 (certain detection)
        """
        dt = datetime.fromtimestamp(timestamp_sec)
        is_business = 9 <= dt.hour < 17 and dt.weekday() < 5
        is_weekend = dt.weekday() >= 5
        
        base = self.RULES.get((transport, is_business, is_weekend), 0.5)
        
        interval_sec = beacon_interval_ms / 1000.0
        for (min_s, max_s), mult in self.FREQUENCY_MULTIPLIERS.items():
            if max_s is None:
                if interval_sec >= min_s:
                    base *= mult
                    break
            elif min_s <= interval_sec < max_s:
                base *= mult
                break
        
        return min(1.0, base)
```

### Reward Computation

```python
class RewardComputer:
    """Compute training reward for telemetry sample"""
    
    def __init__(self, config):
        self.config = config
        self.detection_sim = DetectionLikelihood()
    
    def compute_reward(self, telemetry: TelemetryRecord, detected: bool = None) -> float:
        """
        Compute reward for a telemetry sample.
        
        Reward components:
        - Base reward for beacon outcome (+10 success, -5 failure)
        - Detection penalty (-20 if detected, or likelihood-based penalty)
        - Latency bonus/penalty (+2 if <1s, -3 if >5s)
        - Frequency anomaly penalty (-5 if suspiciously frequent/rare)
        
        Args:
            telemetry: Parsed telemetry record
            detected: Optional manual detection signal from TUI
        
        Returns:
            float: Scalar reward value
        """
        reward = 0.0
        
        # 1. Beacon outcome
        if telemetry.success_rate >= 0.9:
            reward += self.config.REWARD_BEACON_SUCCESS
        else:
            reward += self.config.REWARD_BEACON_FAILURE
        
        # 2. Detection penalty
        if detected is not None:
            # Manual feedback from TUI
            reward += -20.0 if detected else 0.0
        else:
            # Simulated detection based on transport/time
            detection_likelihood = self.detection_sim.get_likelihood(
                telemetry.current_transport,
                telemetry.timestamp,
                telemetry.beacon_interval_ms
            )
            # Penalty proportional to detection risk
            reward -= (detection_likelihood * 15.0)
        
        # 3. Latency bonus/penalty
        if telemetry.last_latency_ms < 1000:
            reward += self.config.REWARD_FAST_LATENCY
        elif telemetry.last_latency_ms > 5000:
            reward += self.config.REWARD_SLOW_LATENCY
        
        # 4. Frequency anomaly
        if telemetry.beacon_interval_ms < 5000 or telemetry.beacon_interval_ms > 3600000:
            reward += self.config.REWARD_FREQUENCY_ANOMALY
        
        return reward
```

---

## Server Architecture

### Main Server Loop

```python
class TrainingServer:
    """Multi-threaded RL training server"""
    
    def __init__(self, config: TrainingServerConfig):
        self.config = config
        self.agents = {}                # agent_id → AgentTrainingState
        self.telemetry_buffer = deque(maxlen=config.TELEMETRY_BUFFER_SIZE)
        self.rl_model = None            # Loaded RL model
        self.sample_count = 0           # Total samples processed
        self.lock = threading.RLock()
        
        # HTTP API server
        self.flask_app = Flask(__name__)
        self.setup_http_api()
        
        # Socket server
        self.socket_server = None
        self.listening = False
    
    def start(self):
        """Start training server"""
        # Load RL model
        from rl_beacon_agent import get_agent
        self.rl_model = get_agent()
        
        # Start socket listener
        threading.Thread(target=self._socket_listener_loop, daemon=True).start()
        
        # Start HTTP API
        self.flask_app.run(
            host=self.config.LISTEN_IP,
            port=self.config.HTTP_API_PORT,
            debug=False,
            threaded=True
        )
    
    def _socket_listener_loop(self):
        """Listen for agent connections on port 5556"""
        self.socket_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket_server.bind((self.config.LISTEN_IP, self.config.LISTEN_PORT))
        self.socket_server.listen(self.config.MAX_CONNECTIONS)
        
        logger.info(f"Training server listening on {self.config.LISTEN_IP}:{self.config.LISTEN_PORT}")
        self.listening = True
        
        try:
            while self.listening:
                try:
                    client_socket, addr = self.socket_server.accept()
                    # Handle in background thread
                    threading.Thread(
                        target=self._handle_agent_connection,
                        args=(client_socket, addr),
                        daemon=True
                    ).start()
                except socket.timeout:
                    continue
        finally:
            self.socket_server.close()
    
    def _handle_agent_connection(self, client_socket: socket.socket, addr):
        """Handle single agent connection"""
        agent_id = None
        
        try:
            # Receive telemetry packets indefinitely
            while True:
                data = client_socket.recv(256)  # Telemetry packet size
                
                if not data or len(data) == 0:
                    break
                
                # Parse telemetry
                try:
                    telemetry = self._parse_telemetry_packet(data)
                    agent_id = telemetry.agent_id
                    
                    # Process telemetry
                    self._process_telemetry(telemetry)
                    
                    # Prepare feedback if available
                    feedback = self._generate_feedback(agent_id, telemetry)
                    
                    # Send feedback (128 bytes)
                    feedback_packet = self._serialize_feedback_packet(feedback)
                    client_socket.sendall(feedback_packet)
                    
                except Exception as e:
                    logger.error(f"Error parsing telemetry: {e}")
                    break
        
        except Exception as e:
            logger.error(f"Connection error with {addr}: {e}")
        
        finally:
            client_socket.close()
            if agent_id:
                logger.info(f"Agent {agent_id} disconnected")
```

### Telemetry Processing

```python
def _parse_telemetry_packet(self, data: bytes) -> TelemetryRecord:
    """
    Parse 256-byte telemetry packet into normalized record.
    
    Packet format (see training_client_spec.md):
    [0-3]: packet_type (0x01)
    [4-7]: protocol_version
    [8-71]: agent_id
    [72-79]: beacon_count
    [80-87]: success_count
    [88-95]: failed_count
    [96-103]: detected_count
    [104-135]: current_transport
    [136-139]: beacon_interval_ms
    [140-143]: last_latency_ms
    [144-147]: last_packet_size
    [148-155]: timestamp
    [156-187]: network_condition
    [188-191]: recent_failures
    [192-255]: reserved
    """
    import struct
    
    if len(data) < 192:
        raise ValueError(f"Telemetry packet too short: {len(data)} bytes")
    
    packet_type = struct.unpack('>I', data[0:4])[0]
    if packet_type != 0x01:
        raise ValueError(f"Invalid packet type: {packet_type:#x}")
    
    # Unpack fields
    agent_id = data[8:72].rstrip(b'\x00').decode('utf-8')
    total_beacons = struct.unpack('>Q', data[72:80])[0]
    successful = struct.unpack('>Q', data[80:88])[0]
    failed = struct.unpack('>Q', data[88:96])[0]
    detected = struct.unpack('>Q', data[96:104])[0]
    transport = data[104:136].rstrip(b'\x00').decode('utf-8')
    interval_ms = struct.unpack('>I', data[136:140])[0]
    latency_ms = struct.unpack('>I', data[140:144])[0]
    pkt_size = struct.unpack('>I', data[144:148])[0]
    timestamp = struct.unpack('>Q', data[148:156])[0]
    network_cond = data[156:188].rstrip(b'\x00').decode('utf-8')
    recent_fail = struct.unpack('>I', data[188:192])[0]
    
    # Validate
    if total_beacons == 0:
        success_rate = 1.0
    else:
        success_rate = successful / total_beacons
    
    # Compute temporal fields
    dt = datetime.fromtimestamp(timestamp)
    
    return TelemetryRecord(
        agent_id=agent_id,
        total_beacons=total_beacons,
        successful_beacons=successful,
        failed_beacons=failed,
        detected_count=detected,
        current_transport=transport or "unknown",
        beacon_interval_ms=interval_ms,
        last_latency_ms=latency_ms,
        last_packet_size=pkt_size,
        network_condition=network_cond or "unknown",
        recent_failures=recent_fail,
        timestamp=float(timestamp),
        hour_of_day=dt.hour,
        day_of_week=dt.weekday(),
        is_business_hours=(9 <= dt.hour < 17 and dt.weekday() < 5),
        success_rate=success_rate,
        failure_rate=1.0 - success_rate,
        beacon_frequency=(total_beacons / max(1, (time.time() - timestamp) / 3600.0))
    )

def _process_telemetry(self, telemetry: TelemetryRecord):
    """
    Store telemetry, compute reward, store training sample.
    """
    with self.lock:
        # Track agent state
        if telemetry.agent_id not in self.agents:
            self.agents[telemetry.agent_id] = AgentTrainingState(
                agent_id=telemetry.agent_id,
                first_seen=telemetry.timestamp,
                last_telemetry=telemetry.timestamp,
                sample_count=0,
                last_action={},
                action_history=deque(maxlen=20),
                total_samples_trained=0,
                avg_reward=0.0,
                detection_likelihood=0.0,
                pending_feedback={}
            )
        
        agent_state = self.agents[telemetry.agent_id]
        agent_state.last_telemetry = telemetry.timestamp
        agent_state.sample_count += 1
        
        # Store telemetry in buffer
        self.telemetry_buffer.append(telemetry)
        self.sample_count += 1
        
        # Compute reward
        reward_computer = RewardComputer(self.config)
        reward = reward_computer.compute_reward(telemetry)
        
        # Build state vector for RL model
        state_vector = self._build_state_vector(telemetry)
        
        # Store experience in RL model
        action_idx = self.rl_model.select_action(
            torch.tensor(state_vector),
            training=True
        )
        
        self.rl_model.store_experience(
            state=torch.tensor(state_vector),
            action_idx=action_idx['action_idx'],
            reward=reward,
            next_state=torch.tensor(state_vector),  # Simplified; should be actual next state
            done=False
        )
        
        # Update agent metrics
        agent_state.detection_likelihood = reward_computer.detection_sim.get_likelihood(
            telemetry.current_transport,
            telemetry.timestamp,
            telemetry.beacon_interval_ms
        )
        
        # Periodic training step
        if self.sample_count % self.config.TRAINING_STEP_INTERVAL == 0:
            loss = self.rl_model.train_step()
            logger.info(f"Training step: loss={loss:.4f}, samples={self.sample_count}")

def _build_state_vector(self, telemetry: TelemetryRecord) -> list:
    """Convert telemetry to RL state vector (6-dimensional)"""
    from rl_beacon_agent import TRANSPORTS
    
    transport_idx = TRANSPORTS.index(telemetry.current_transport) / len(TRANSPORTS) if telemetry.current_transport in TRANSPORTS else 0.0
    
    return [
        telemetry.hour_of_day / 24.0,
        telemetry.day_of_week / 7.0,
        telemetry.success_rate,
        min(telemetry.last_latency_ms / 10000.0, 1.0),
        min(telemetry.beacon_interval_ms / 600000.0, 1.0),
        transport_idx
    ]
```

### Feedback Generation

```python
def _generate_feedback(self, agent_id: str, telemetry: TelemetryRecord) -> dict:
    """
    Generate feedback packet to send back to agent.
    """
    with self.lock:
        agent_state = self.agents[agent_id]
        
        # Compute detection likelihood
        detection_sim = DetectionLikelihood()
        likelihood = detection_sim.get_likelihood(
            telemetry.current_transport,
            telemetry.timestamp,
            telemetry.beacon_interval_ms
        )
        
        # Use RL model to recommend action
        state_vector = self._build_state_vector(telemetry)
        state_tensor = torch.tensor(state_vector, dtype=torch.float32)
        action = self.rl_model.select_action(state_tensor, training=False)
        
        feedback = {
            'agent_id': agent_id,
            'feedback_type': 2 if likelihood > 0.7 else 1,  # 2=detected risk, 1=safe
            'detection_likelihood': likelihood,
            'recommended_transport': action.get('transport', 'dns'),
            'recommended_interval': action.get('beacon_interval', telemetry.beacon_interval_ms),
            'confidence': 1.0 - likelihood if likelihood > 0.5 else 0.5
        }
        
        # Store in agent state for future reference
        agent_state.last_action = feedback
        agent_state.action_history.append(feedback)
        
        return feedback

def _serialize_feedback_packet(self, feedback: dict) -> bytes:
    """
    Serialize feedback dict to 128-byte packet.
    Format (see training_client_spec.md):
    [0-3]: packet_type (0x02)
    [4-7]: protocol_version
    [8-11]: feedback_type
    [12-15]: detection_likelihood (float)
    [16-47]: recommended_transport
    [48-51]: recommended_interval
    [52-55]: confidence (float)
    [56-127]: reserved
    """
    import struct
    
    packet = bytearray(128)
    
    struct.pack_into('>I', packet, 0, 0x02)  # packet_type
    struct.pack_into('>I', packet, 4, 0x01000000)  # protocol_version
    struct.pack_into('>I', packet, 8, feedback['feedback_type'])
    struct.pack_into('>f', packet, 12, feedback['detection_likelihood'])
    
    transport = feedback['recommended_transport'].encode('utf-8')[:31]
    packet[16:16+len(transport)] = transport
    
    struct.pack_into('>I', packet, 48, feedback['recommended_interval'])
    struct.pack_into('>f', packet, 52, feedback['confidence'])
    
    return bytes(packet)
```

---

## HTTP API Specification

### Endpoints

All endpoints return JSON. Base URL: `http://localhost:5557`

#### GET `/training/status`

Get current training server status.

**Response (200):**
```json
{
  "status": "running",
  "agents_connected": 3,
  "agents": [
    {
      "agent_id": "abc123...",
      "first_seen": 1719513825.456,
      "last_telemetry": 1719513935.678,
      "samples": 150,
      "avg_reward": 8.2,
      "detection_likelihood": 0.25
    }
  ],
  "total_samples_processed": 450,
  "total_training_steps": 9,
  "rl_model_metrics": {
    "epsilon": 0.45,
    "avg_loss": 0.234,
    "success_rate": 0.87
  }
}
```

#### GET `/training/agent/<agent_id>`

Get detailed metrics for a specific agent.

**Response (200):**
```json
{
  "agent_id": "abc123...",
  "first_seen": 1719513825.456,
  "last_telemetry": 1719513935.678,
  "total_beacons": 150,
  "successful_beacons": 120,
  "failed_beacons": 30,
  "detected_count": 5,
  "current_transport": "dns",
  "beacon_interval_ms": 30000,
  "success_rate": 0.80,
  "detection_likelihood": 0.25,
  "last_action": {
    "recommended_transport": "https",
    "recommended_interval": 45000,
    "confidence": 0.65
  },
  "action_history": [
    {"recommended_transport": "dns", "confidence": 0.45},
    {"recommended_transport": "https", "confidence": 0.65}
  ],
  "avg_reward": 8.2,
  "samples_trained": 150
}
```

#### POST `/training/feedback`

Manually mark agent as detected or successful (TUI use).

**Request body:**
```json
{
  "agent_id": "abc123...",
  "feedback_type": "detected",  // "detected" or "successful"
  "timestamp": 1719513935.678
}
```

**Response (200):**
```json
{
  "status": "ok",
  "reward_injected": -20.0,
  "message": "Detection feedback recorded, model will be retrained"
}
```

#### POST `/training/train`

Force immediate training step (bypass interval).

**Request body:**
```json
{
  "batch_size": 32
}
```

**Response (200):**
```json
{
  "status": "ok",
  "training_loss": 0.234,
  "samples_used": 32,
  "epsilon": 0.45
}
```

#### GET `/training/telemetry`

Retrieve raw telemetry records (pagination).

**Query params:**
- `agent_id`: Filter by agent (optional)
- `limit`: Max records (default 100)
- `offset`: Pagination offset (default 0)

**Response (200):**
```json
{
  "total": 450,
  "offset": 0,
  "limit": 100,
  "records": [
    {
      "agent_id": "abc123...",
      "timestamp": 1719513825.456,
      "total_beacons": 10,
      "successful_beacons": 9,
      "current_transport": "dns",
      "beacon_interval_ms": 30000,
      "last_latency_ms": 450,
      "success_rate": 0.90,
      "detection_likelihood": 0.15
    }
  ]
}
```

#### POST `/training/simulate-network`

Simulate network condition change (for testing).

**Request body:**
```json
{
  "agent_id": "abc123...",
  "condition": "congested",  // "normal", "congested", "unstable"
  "duration_seconds": 60
}
```

**Response (200):**
```json
{
  "status": "ok",
  "condition_active_until": 1719513995.678
}
```

---

## Logging

### Log Format

```
[2024-06-27 14:23:45.123] INFO  [training_server] Training server listening on 127.0.0.1:5556
[2024-06-27 14:23:46.234] INFO  [socket_listener] Agent abc123... connected from 127.0.0.1:54321
[2024-06-27 14:23:50.456] DEBUG [telemetry] Parsed: agent=abc123, beacons=10, success=9, transport=dns
[2024-06-27 14:24:00.567] INFO  [reward] Computed reward=8.2 (success bonus + latency bonus - frequency penalty)
[2024-06-27 14:24:00.678] DEBUG [feedback] Generated: transport=https, confidence=0.65
[2024-06-27 14:24:01.789] INFO  [agent_state] Updated: abc123 (samples=10, avg_reward=8.2)
[2024-06-27 14:25:00.890] INFO  [training] Triggered train_step (samples=50, loss=0.234)
[2024-06-27 14:25:01.901] WARN  [telemetry] Suspicious pattern detected: agent=xyz789, freq=0.5s (very fast)
[2024-06-27 14:25:02.012] ERROR [socket] Connection error: Agent timeout
```

### Log Rotation

- File: `/tmp/training_server.log`
- Max size: 50 MB
- Keep 5 backup files
- Auto-rotate when size exceeded

---

## Error Handling

### Telemetry Parsing Errors

| Error | Action | Logging |
|-------|--------|---------|
| Packet too short | Skip, drop connection | ERROR |
| Invalid packet type | Skip | DEBUG |
| Corrupted timestamp | Use current time | WARN |
| Invalid transport | Default to "unknown" | WARN |
| Negative beacon count | Cap at 0 | WARN |

### Connection Errors

| Error | Action | Backoff |
|-------|--------|---------|
| Client disconnect | Close gracefully | None |
| Socket I/O error | Log, close | None |
| Buffer overflow | Drop oldest sample | WARN |

### Model Errors

| Error | Action | Fallback |
|-------|--------|----------|
| Model not loaded | Skip training | ERROR |
| Invalid state vector | Skip sample | WARN |
| GPU memory exhausted | Fall back to CPU | WARN |

---

## Performance Characteristics

| Metric | Target | Tolerance |
|--------|--------|-----------|
| Telemetry parse latency | <10ms | ±5ms |
| Feedback generation latency | <20ms | ±10ms |
| Training step duration | <500ms | ±200ms |
| Memory footprint | <200MB | ±50MB |
| Concurrent connections | 100 agents | ±20 |
| HTTP API response time | <100ms | ±50ms |

---

## Testing Approach

### Unit Tests

```bash
# Test detection likelihood computation
python3 -m pytest tests/test_detection_sim.py -v

# Test reward computation
python3 -m pytest tests/test_reward_computer.py -v

# Test packet parsing
python3 -m pytest tests/test_telemetry_parsing.py -v
```

### Integration Tests

```bash
# Start server
python3 src/training_server.py &
SERVER_PID=$!

# Connect mock agent
python3 tests/test_mock_agent.py --agent-count=5 --duration=60s

# Verify telemetry flow
python3 tests/verify_telemetry_flow.py

kill $SERVER_PID
```

### Load Tests

```bash
# Simulate 100 concurrent agents sending telemetry
python3 tests/load_test.py --agents=100 --duration=300s --telemetry-rate=10hz
```

---

## Security Considerations

### What is NOT protected

- **No authentication**: Server trusts any agent_id in telemetry (trust network boundary)
- **No encryption**: TCP traffic in plaintext (use firewall or VPN)
- **No input validation beyond range checks**: Agent can send arbitrary strings in transport field

### What IS protected

- **Thread safety**: All shared state protected by locks
- **Buffer size limits**: Fixed-size packets prevent overflow
- **Feedback format validation**: Only structured fields, no command injection
- **Model safety**: RL model training isolated, no dangerous computation

### Hardening Recommendations

- Run on private/internal network only
- Use firewall to restrict port 5556/5557 to approved agents
- Monitor `/tmp/training_server.log` for suspicious patterns
- Rate-limit telemetry per agent (max 1 packet per second)
- Implement agent authentication token if needed

---

## Scaling Considerations

### Single Server Limits

- **Max agents**: 100-200 (limited by thread count, socket descriptors)
- **Max telemetry/sec**: 1000+ (Python socket bottleneck)
- **Max RL training/sec**: 10-50 (GPU/CPU bottleneck)

### Multi-Server Architecture (Future)

```
┌────────────┐  ┌────────────┐  ┌────────────┐
│Training    │  │Training    │  │Training    │
│Server #1   │  │Server #2   │  │Server #3   │
└───┬────────┘  └────┬───────┘  └────┬───────┘
    │               │                │
    └───────────────┼────────────────┘
                    │
            ┌───────▼────────┐
            │ Redis Broker   │ (Shared samples)
            │ (Samples queue)│
            └────────────────┘
                    │
            ┌───────▼────────┐
            │Training Master │ (RL model, train_step)
            │                │
            └────────────────┘
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-06-27 | Initial specification |

