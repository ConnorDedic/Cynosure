# RL Beacon Agent — Server-Side Beacon Timing Optimization

## Overview

The RL Beacon Agent is a centralized **Deep Q-Learning (DQN)** model that learns optimal beacon timing strategies across all implants. The model runs on the C2 server and provides beacon recommendations globally, observing connectivity success, uptime, and network conditions to minimize noise while maximizing response time.

## Architecture

### Components

#### 1. **Core Model** (`src/rl_beacon_agent.py`)
- **DQN Network**: 3-layer neural network (state_dim=6 → 128 → 128 → action_dim=24)
- **Target Network**: Separate copy updated periodically (every 100 steps)
- **Experience Replay**: Buffer holds up to 10,000 transitions
- **Epsilon-Greedy Exploration**: Starts at 1.0, decays to 0.01 over training

#### 2. **HTTP Service** (`src/rl_beacon_service.py`)
- RESTful API for beacon timing decisions
- Listens on `127.0.0.1:5555` (configurable)
- Four main endpoints:
  - `POST /beacon/action` — Get recommended beacon interval/retry count
  - `POST /beacon/feedback` — Provide beacon result feedback
  - `GET /model/metrics` — Query current model status
  - `POST /model/train` — Trigger training step

#### 3. **TUI Integration** (`src/main.rs`)
- Panel: `RLModelStatus` (shortcut: `[l]` from SessionDetail)
- Displays:
  - Training metrics (steps, epsilon, avg loss)
  - Beacon performance (success rate, counts)
  - Current episode reward & buffer size

### State Space (6D)

```
[hour/24, day_of_week/7, success_rate, uptime, beacon_age/600, transport_idx]
```

- **hour**: Current hour normalized (0.0-1.0)
- **day_of_week**: Day index normalized (0.0-1.0)
- **success_rate**: Beacon success ratio (0.0-1.0)
- **uptime**: Agent uptime ratio (0.0-1.0)
- **beacon_age**: Seconds since last beacon / 600 (capped at 1.0)
- **transport_idx**: Transport index / 4 (0.0-1.0)

### Action Space (24 Actions)

6 beacon intervals × 4 retry counts:
```
Intervals: [5, 10, 30, 60, 120, 300] seconds
Retries:   [1, 2, 3, 5]
```

Action decoding:
```python
action_idx = 0..23
interval_idx = action_idx // 4
retry_idx = action_idx % 4
beacon_interval = INTERVALS[interval_idx]
retry_count = RETRIES[retry_idx]
```

### Reward Function

```
+10.0   : Beacon successful
-5.0    : Beacon failed
+2.0    : Fast response (< 1 second)
-0.1/60 : Penalty per beacon interval second
```

**Rationale**: 
- Success prioritized (connectivity goal)
- Failures penalized to learn reliability
- Fast response bonused (operational speed)
- Interval penalty teaches stealth tradeoff

## Installation

### Prerequisites
- Python 3.8+
- PyTorch 2.0+
- aiohttp 3.8+
- NumPy 1.21+

```bash
pip install -r requirements-rl.txt
```

## Usage

### Starting the Service

```bash
cd src
python rl_beacon_service.py
# Output: [+] RL Beacon Service listening on 127.0.0.1:5555
```

### API Endpoints

#### 1. Get Beacon Action
```bash
curl -X POST http://127.0.0.1:5555/beacon/action \
  -H "Content-Type: application/json" \
  -d '{
    "implant_id": "agent-1234",
    "success_rate": 0.95,
    "uptime": 0.99,
    "seconds_since_beacon": 30,
    "transport": "vpn"
  }'
```

**Response:**
```json
{
  "beacon_interval": 30,
  "retry_count": 2,
  "transport": "vpn",
  "confidence": 0.85,
  "action_idx": 8
}
```

#### 2. Provide Feedback
```bash
curl -X POST http://127.0.0.1:5555/beacon/feedback \
  -H "Content-Type: application/json" \
  -d '{
    "implant_id": "agent-1234",
    "success": true,
    "response_time": 0.5,
    "beacon_interval": 30
  }'
```

**Response:**
```json
{
  "reward": 11.95,
  "processed": true
}
```

#### 3. Get Model Metrics
```bash
curl http://127.0.0.1:5555/model/metrics
```

**Response:**
```json
{
  "step_count": 1024,
  "epsilon": 0.7342,
  "avg_loss": 0.1234,
  "success_rate": 0.92,
  "successful_beacons": 46,
  "failed_beacons": 4,
  "memory_size": 523,
  "current_episode_reward": 85.3
}
```

#### 4. Train Step
```bash
curl -X POST http://127.0.0.1:5555/model/train
```

**Response:**
```json
{
  "loss": 0.0856,
  "metrics": { ... }
}
```

## Integration with Dispatcher

### Proposed Flow

1. **Implant beacons server** → dispatcher logs outcome
2. **Dispatcher queries RL agent** via HTTP `/beacon/action`
3. **RL agent recommends** interval/retry based on learned policy
4. **Dispatcher returns recommendation** to implant
5. **Implant adjusts beacon** parameters
6. **On next beacon**, dispatcher logs outcome via `/beacon/feedback`
7. **RL agent trains** on accumulated experience

### Dispatcher Integration Example (pseudocode)

```c
// When handling beacon from implant
void handle_beacon_response(edr_dispatcher_t *d, edr_message_t *msg) {
    // 1. Get implant metrics
    implant_metrics_t metrics = extract_metrics(msg);
    
    // 2. Query RL agent for next beacon params
    curl_handle_t curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:5555/beacon/action");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_encode(metrics));
    
    json_t *response = curl_perform_json(curl);
    int beacon_interval = json_get_int(response, "beacon_interval");
    int retry_count = json_get_int(response, "retry_count");
    
    // 3. Send recommendation back to implant
    edr_message_t beacon_config = {
        .command_id = "beacon-config",
        .payload = json_encode({
            "interval": beacon_interval,
            "retries": retry_count
        })
    };
    send_to_implant(d, &beacon_config);
    
    // 4. Send feedback on previous beacon
    curl_handle_t feedback = curl_easy_init();
    curl_easy_setopt(feedback, CURLOPT_URL, "http://127.0.0.1:5555/beacon/feedback");
    curl_easy_setopt(feedback, CURLOPT_POSTFIELDS, json_encode({
        "success": metrics.beacon_success,
        "response_time": metrics.response_time,
        "beacon_interval": metrics.last_interval
    }));
    curl_perform(feedback);
    
    curl_easy_cleanup(curl);
    curl_easy_cleanup(feedback);
}
```

## TUI Display

Access via `[l]` shortcut from SessionDetail panel:

```
┌─────────── RL Beacon Agent Model Status ──────────┐
│                                                    │
│ Training Status                                    │
│                                                    │
│   Steps: 1024                                      │
│   Epsilon: 0.7342 (exploration rate)              │
│   Avg Loss: 0.123456                              │
│                                                    │
│ Beacon Performance                                 │
│                                                    │
│   Success Rate: 92.0%                             │
│   Successful: 46  Failed: 4                        │
│                                                    │
│ Current Episode                                    │
│                                                    │
│   Reward: 85.30                                    │
│   Buffer Size: 523 / 10000                         │
│                                                    │
│ [l] RL Model  [Esc] Back                           │
└────────────────────────────────────────────────────┘
```

## Training Dynamics

### Epsilon Decay
- **Start**: 1.0 (100% exploration)
- **Decay**: Multiply by 0.995 each training step
- **End**: 0.01 (1% exploration)
- **Effect**: Agent explores uniformly early, then follows learned policy

### Target Network Updates
- Copy Q-network → target network every **100 steps**
- Prevents Q-value instability
- Allows credit assignment over longer horizons

### Replay Buffer
- Capacity: **10,000 transitions**
- Sample: **32 transitions** per training step
- Breaks sequential correlation in training data

### Optimizer
- **Algorithm**: Adam
- **Learning Rate**: 1e-3
- **Gradient Clipping**: Norm ≤ 1.0
- **Discount Factor (γ)**: 0.99

## Monitoring & Debugging

### Key Metrics

| Metric | Interpretation |
|--------|-----------------|
| `avg_loss` | Training stability (lower is better) |
| `success_rate` | Beacon reliability learned |
| `epsilon` | Exploration level (should decay) |
| `step_count` | Total learning steps |
| `memory_size` | Experiences collected |

### Troubleshooting

**High loss + low success rate**
→ Agent still learning, try more steps or easier reward structure

**Epsilon stuck high**
→ Check decay rate, verify training steps are executing

**Memory full (10k transitions)**
→ Consider lowering MEMORY_SIZE or running training more frequently

**All actions same (confidence ~0.04)**
→ Cold start; collect more diverse beacon data

## Model Persistence

### Save Checkpoint
```python
from rl_beacon_agent import get_agent
agent = get_agent()
agent.save("rl_model_checkpoint.pt")
```

### Load Checkpoint
```python
from rl_beacon_agent import get_agent
agent = get_agent()
agent.load("rl_model_checkpoint.pt")
```

## Next Steps

1. **Wire dispatcher integration** — add HTTP calls to RL service in listener
2. **Implant beacon config** — add command for implant to receive beacon params
3. **Training loop** — periodically call `/model/train` based on beacon feedback
4. **Experimentation** — adjust reward weights, interval ranges, explore different state features

## References

- Mnih et al. (2015): "Human-Level Control through Deep Reinforcement Learning"
- OpenAI Spinning Up: https://spinningup.openai.com/en/latest/spinup/rl_intro.html
