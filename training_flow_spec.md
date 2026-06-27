# Training Data Flow Specification

## Overview

This document specifies the complete information flow through the entire RL training system, from agent beacon to model update to operator feedback. It includes exact timing, sequence diagrams, and failure scenarios.

---

## High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        TRAINING SYSTEM                           │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Agent (training_implant)                                       │
│  ├─ Beacon success/failure                                      │
│  ├─ Collect metrics (latency, packet size, transport)          │
│  └─ Report to training client                                  │
│         │                                                       │
│         ├─ TCP connection to training_server:5556              │
│         │                                                       │
│         └─→ Telemetry Packet (256 bytes)                       │
│             [beacon_count, success, transport, timestamp...]  │
│                     │                                          │
│                     │                                          │
│  Training Server (localhost:5556 + :5557 HTTP API)             │
│  ├─ Parse telemetry                                            │
│  ├─ Compute detection likelihood                               │
│  ├─ Compute reward                                             │
│  ├─ Store in replay buffer                                     │
│  ├─ Periodically train RL model (every 50 samples)            │
│  └─ Generate feedback                                          │
│         │                                                       │
│         ├─ TCP → Feedback Packet (128 bytes)                   │
│         │         [recommended_transport, confidence...]      │
│         │                                                       │
│         └─ HTTP API for TUI integration                        │
│            ├─ GET /training/agent/<id>                        │
│            ├─ POST /training/feedback                         │
│            └─ POST /training/train                            │
│                     │                                          │
│  RL Model (in training_server.py)                              │
│  ├─ store_experience(state, action, reward, next_state)       │
│  ├─ train_step() [every 50 samples]                           │
│  └─ select_action(state) [new recommendations]               │
│                                                                 │
│  TUI (operator control, localhost:4444)                        │
│  ├─ View agent metrics (via training server API)              │
│  ├─ Mark detected/successful (HTTP POST)                      │
│  ├─ Inject network patterns (HTTP POST)                       │
│  └─ Force training step (HTTP POST)                           │
│         │                                                       │
│         └─→ HTTP API calls inject feedback into model         │
│                                                                 │
└──────────────────────────────────────────────────────────────────┘
```

---

## Sequence: Normal Beacon with Telemetry

```
Time    Agent                Training Server          RL Model
────────────────────────────────────────────────────────────────
 0ms    │ Beacon success      │                      │
        ├─ record latency    │                      │
        ├─ record transport  │                      │
        └─ increment counter │                      │
                             │                       │
10ms    │ Counter = 10       │                      │
        │ (telemetry trigger)│                      │
        │                     │                      │
20ms    │ Serialize packet   │                      │
        │ (256 bytes)        │                      │
        │ ├─ beacon_count=10 │                      │
        │ ├─ success=9       │                      │
        │ ├─ transport=dns   │                      │
        │ └─ timestamp=X     │                      │
        │                     │                      │
30ms    │ Connect socket     │                      │
        │ (if needed)        │                      │
        │                     │                      │
50ms    │ Send telemetry ────→ Receive (256 bytes) │
        │                    │ ├─ Parse fields    │
        │                    │ ├─ Validate        │
        │                    │ └─ Store in buffer │
        │                    │                     │
60ms    │                    │ Build state vector │
        │                    │ state=[hour, dow,  │
        │                    │   success_rate,    │
        │                    │   uptime, beacon_  │
        │                    │   age, transport]  │
        │                    │                     │
70ms    │                    │ Compute reward    │ (Request state)
        │                    │ ├─ base = +10     │
        │                    │ ├─ detection = -0 │
        │                    │ ├─ latency = +2   │
        │                    │ └─ total = +12    │
        │                    │                     │
80ms    │                    │ Call RL model ────→ store_experience(
        │                    │                    │   state=vec[6],
        │                    │                    │   action=1,
        │                    │                    │   reward=12.0,
        │                    │                    │   next_state=vec[6],
        │                    │                    │   done=False
        │                    │                    │ )
        │                    │                    │
90ms    │                    │                    │ Store in
        │                    │                    │ replay_buffer[450]
        │                    │                    │
100ms   │                    │ Generate feedback │
        │                    │ ├─ detection_lik  │
        │                    │ │   = 0.15        │
        │                    │ ├─ recommend =    │
        │                    │ │   select_action│→ (inference)
        │                    │ │   (state)      │  returns: transport
        │                    │ └─ confidence =   │  = "dns"
        │                    │   0.65           │
        │                    │                   │
110ms   │ Receive feedback ←─── Serialize (128) │
        │ ├─ transport=dns  │ bytes & send    │
        │ ├─ confidence=0.65│                 │
        │ └─ cache for      │                 │
        │    next use       │                 │
        │                    │                 │
120ms   │ (can use feedback │                 │
        │  for adaptive     │                 │
        │  beacon changes)  │                 │
        │                    │                 │

Sample Counter = 450
        │                    │ Check if trigger │
        │                    │ train_step       │
        │                    │ (450 % 50 == 0)  │
        │                    │                  │
130ms   │                    │ Call train_step()→ train_step():
        │                    │                  │
        │                    │                  │ Sample batch
        │                    │                  │ of 32 from
        │                    │                  │ replay_buffer
        │                    │                  │
        │                    │                  │ Compute loss
        │                    │                  │ backward pass
        │                    │                  │ optimizer.step()
        │                    │                  │
        │                    │                  │ Update epsilon
        │                    │                  │ increment step_count
        │                    │                  │
200ms   │                    │ Training complete←─ loss=0.234
        │                    │ log "loss=0.234" │
        │                    │                  │
        │ Reset counter=0    │                  │
        │ Continue beacons   │                  │
        │                    │                  │
```

---

## Sequence: Manual Detection Feedback from TUI

```
Time    TUI                 Training Server          RL Model
────────────────────────────────────────────────────────────────
 0ms    │ User presses 'D'  │                      │
        │ on agent screen   │                      │
        │                    │                      │
10ms    │ Show confirm      │                      │
        │ dialog            │                      │
        │                    │                      │
50ms    │ User presses 'Y'  │                      │
        │                    │                      │
60ms    │ POST /training/   │                      │
        │ feedback          │                      │
        │ ├─ agent_id="abc" │                      │
        │ ├─ feedback_type= │                      │
        │ │ "detected"      │                      │
        │ └─ timestamp=X    │                      │
        │       │            │                      │
        │       │            │                      │
70ms    │       └───────────→ Receive HTTP POST   │
        │                    │ ├─ Parse JSON      │
        │                    │ └─ Extract fields  │
        │                    │                     │
80ms    │                    │ Create synthetic   │
        │                    │ training sample:   │
        │                    │ ├─ state = last    │
        │                    │ │ known state for  │
        │                    │ │ agent "abc"      │
        │                    │ ├─ action = last   │
        │                    │ │ action (what was │
        │                    │ │ it doing?)       │
        │                    │ ├─ reward = -50.0  │
        │                    │ │ (high penalty)   │
        │                    │ └─ next_state =    │
        │                    │   state (no change)│
        │                    │                     │
90ms    │                    │ Call store_exper- │
        │                    │ ience() ──────────→ store_experience(
        │                    │                    │   synthetic_sample
        │                    │                    │ )
        │                    │                    │
100ms   │                    │                    │ Add to
        │                    │                    │ replay_buffer[451]
        │                    │                    │
110ms   │                    │ Log detection      │
        │                    │ feedback event     │
        │                    │                    │
120ms   │ ← Return 200 OK    │                    │
        │ ├─ status="ok"     │                    │
        │ ├─ reward_injected │                    │
        │ │ =-50.0           │                    │
        │ └─ message="..."   │                    │
        │                    │                    │
130ms   │ Show success       │                    │
        │ message (2 sec)    │                    │
        │                    │                    │

Sample Counter = 451
        │                    │ (no train_step    │
        │                    │  trigger yet,     │
        │                    │  wait for 2 more) │
        │                    │                    │
```

---

## Sequence: RL Training Step (Auto-triggered)

```
Time    Training Server     RL Model             Database
──────────────────────────────────────────────────────────────
 0ms    │ Sample count      │                    │
        │ reaches 500       │                    │
        │ (500 % 50 == 0)   │                    │
        │                   │                    │
10ms    │ Call train_step() │                    │
        │ ──────────────────→ train_step():      │
        │                   │ ├─ Sample 32 from │
        │                   │ │ replay_buffer   │
        │                   │ │ [batch indices  │
        │                   │ │ randomly chosen]│
        │                   │ │                 │
20ms    │                   │ For each sample:  │
        │                   │ ├─ state_tensor  │
        │                   │ │ q_values =      │
        │                   │ │ q_network(state)│
        │                   │ │                 │
        │                   │ ├─ target_value  │
        │                   │ │ = reward +      │
        │                   │ │ gamma * max     │
        │                   │ │ (target_network │
        │                   │ │ (next_state))   │
        │                   │ │                 │
        │                   │ ├─ loss = MSE(    │
        │                   │ │ q_values[action│
        │                   │ │ ], target)      │
        │                   │ │                 │
30ms    │                   │ Backward pass:    │
        │                   │ ├─ loss.backward()│
        │                   │ ├─ clip_grad      │
        │                   │ │ (avoid exploding│
        │                   │ │ gradients)      │
        │                   │ └─ optimizer.step()
        │                   │                   │
40ms    │                   │ Update target net:│
        │                   │ if step_count % 100│
        │                   │   == 0:           │
        │                   │   target_network  │
        │                   │   .load_state_dict│
        │                   │   (q_network.state│
        │                   │   _dict())        │
        │                   │                   │
50ms    │                   │ Decay epsilon:    │
        │                   │ ├─ epsilon *= 0.99│
        │                   │ ├─ epsilon = max( │
        │                   │ │ epsilon, 0.01)  │
        │                   │ └─ step_count += 1│
        │                   │                   │
60ms    │                   │ Return:           │
        │ ← train_loss=0.234 │ loss.item()       │
        │   epsilon=0.45     │                   │
        │   step_count=501   │                   │
        │                    │                   │
70ms    │ Log training event │                   │
        │ "Completed        │                    │
        │  train_step:      │                    │
        │  loss=0.234,      │                    │
        │  epsilon=0.45,    │                    │
        │  samples=500"     │                    │
        │                    │                   │
        │ (Next train_step   │                   │
        │  will trigger at   │                   │
        │  sample 550)       │                   │
        │                    │                   │
```

---

## Sequence: Detection Simulation (Server-side)

```
Time    Incoming             Detection Simulator  Reward Computer
────────────────────────────────────────────────────────────────
 0ms    │ Telemetry arrives │                    │
        │ ├─ transport=vpn  │                    │
        │ ├─ timestamp=14:30│                    │
        │ │ (2:30 PM, Tue)  │                    │
        │ └─ interval=10s   │                    │
        │                    │                    │
10ms    │ Pass to detection  │                    │
        │ simulator ────────→ get_likelihood():  │
        │                    │ ├─ is_business=  │
        │                    │ │ true (2:30pm)  │
        │                    │ ├─ is_weekend=   │
        │                    │ │ false (Tue)    │
        │                    │ └─ lookup base:  │
        │                    │   rules[vpn,     │
        │                    │   business,      │
        │                    │   weekday]       │
        │                    │   = 0.80         │
        │                    │                  │
20ms    │                    │ Apply frequency  │
        │                    │ multiplier:      │
        │                    │ ├─ interval=10s  │
        │                    │ ├─ check range:  │
        │                    │ │ (5, 30) → 1.5x │
        │                    │ ├─ likelihood =  │
        │                    │ │ 0.80 * 1.5     │
        │                    │ │ = 1.20         │
        │                    │ └─ cap at 1.0:   │
        │                    │   = 1.0          │
        │                    │                  │
30ms    │ Receive likelihood │                  │
        │ = 1.0 ──────────────────────────────→ compute_reward():
        │ (100% detection     │                  │
        │  risk!)             │                  │
        │                     │                  │ ├─ base = +10
        │                     │                  │ │ (successful
        │                     │                  │ │ beacon)
        │                     │                  │ ├─ detection_pen
        │                     │                  │ │ = -15.0 *
        │                     │                  │ │   1.0
        │                     │                  │ │ = -15.0
        │                     │                  │ ├─ latency = +2
        │                     │                  │ │ (fast)
        │                     │                  │ ├─ frequency = -5
        │                     │                  │ │ (10s too fast)
        │                     │                  │ └─ total = 10
        │                     │                  │   -15 +2 -5 = -8
        │                     │                  │
40ms    │                     │                  │ Return reward=-8
        │                     │                  │
        │ Store experience:  │                  │
        │ ├─ state=[14, 2,  │                  │
        │ │ 0.9, 0.5, 0.02, │                  │
        │ │ 2]               │                  │
        │ ├─ action=18       │                  │
        │ │ (vpn+10s)        │                  │
        │ ├─ reward=-8.0     │                  │
        │ └─ next_state=[..] │                  │
        │                     │                  │
50ms    │ Add to replay buf  │                  │
        │ Sample counter++   │                  │
        │                     │                  │
```

---

## Sequence: Adaptive Feedback Response (Agent Adopts Recommendation)

```
Time    Agent                Training Server        Agent Logic
──────────────────────────────────────────────────────────────────
 0ms    │ (Previous beacons  │                      │
        │  with VPN@2pm)     │                      │
        │                     │                      │
        │ Feedback: transport │                      │
        │ = "dns", confid=0.7 │                      │
        │ (cached after last  │                      │
        │  telemetry send)    │                      │
        │                     │                      │
10ms    │ Before next beacon: │                      │
        │ Check feedback      │                      │
        │ (optional agent     │                      │
        │  behavior)          │                      │
        │                     │                      │
20ms    │ if (feedback &&     │                      │
        │     confidence > 0.5│                      │
        │  ):                 │                      │
        │   switch to DNS     │                      │
        │                     │                      │
30ms    │ Send beacon via DNS │                      │
        │ (not VPN)           │                      │
        │                     │                      │
40ms    │ Beacon success      │                      │
        │ latency=400ms       │                      │
        │                     │                      │
50ms    │ Report beacon:      │                      │
        │ training_client_    │                      │
        │ report_beacon(      │                      │
        │   success=1,        │                      │
        │   latency=400,      │                      │
        │   transport="dns"   │                      │
        │ ) ────────────────→ Record metrics     │
        │                    │ ├─ success++      │
        │                    │ ├─ latency=400   │
        │                    │ └─ transport=dns │
        │                     │                  │
        │ (Telemetry sent     │                  │
        │  on next interval)  │                  │
        │                     │                  │
60ms    │                     │ New telemetry:  │
        │                     │ ├─ transport=dns│
        │                     │ ├─ interval=30s │
        │                     │ └─ latency=400  │
        │                     │                  │
70ms    │                     │ Detection lik   │
        │                     │ for DNS@2pm:    │
        │                     │ = 0.20          │
        │                     │                  │
80ms    │                     │ Reward:         │
        │                     │ ├─ base = +10   │
        │                     │ ├─ detection    │
        │                     │ │ = -3.0 (0.20) │
        │                     │ ├─ latency = +2 │
        │                     │ └─ total = +9   │
        │                     │                  │
90ms    │                     │ Model learns:   │
        │                     │ state=[14, 2,   │
        │                     │ 0.91, 0.5,      │
        │                     │ 0.02, 1(dns)]   │
        │                     │ → reward=+9     │
        │                     │ (better than    │
        │                     │ VPN: -8)        │
        │                     │                  │
100ms   │ Next feedback:      │                  │
        │ transport="dns"     │                  │
        │ confidence=0.80     │                  │
        │ (increased because  │                  │
        │ model saw success)  │                  │
        │                     │                  │
```

---

## Network Condition State Machine

```
Agent State           Beacon Behavior
─────────────────────────────────────
┌──────────────────────────────────────┐
│  NORMAL (latency <100ms, no loss)    │
│  └─ Success: +10                      │
│     Latency: +2                       │
└──────────────────────────────────────┘
         ↓ (if network detected as)
         │  congested/slow
         │
┌──────────────────────────────────────┐
│  CONGESTED (+500ms latency, 10% loss)│
│  └─ Success: +10                      │
│     Latency: -3 (>5s)                │
│     Failure: -5 (more frequent)      │
└──────────────────────────────────────┘
         ↓ (if latency spike)
         │
┌──────────────────────────────────────┐
│  UNSTABLE (variable, 20% loss)       │
│  └─ Success: varies                   │
│     Frequent timeouts: -5 each       │
│     Consider interval increase       │
└──────────────────────────────────────┘
         ↓ (if blackout detected)
         │
┌──────────────────────────────────────┐
│  BLACKOUT (no connectivity, 30s+)    │
│  └─ Failure: -5 per beacon           │
│     After 5 failures: -frequency_anom│
│     (train model: "wait, don't ping" │
└──────────────────────────────────────┘
```

---

## Metrics Accumulation and Reporting

### Agent-Side Metrics Buffer (training_client.c)

```
Beacon Event 1:     latency_ms=450, success=1
                    └─ internal: metrics.last_latency_ms = 450
                    
Beacon Event 2:     latency_ms=520, success=1
                    └─ internal: metrics.last_latency_ms = 520
                    
Beacon Event 3:     latency_ms=480, success=1
                    └─ internal: metrics.last_latency_ms = 480
                    
...

Beacon Event 10:    latency_ms=490, success=0
                    └─ TRIGGER TELEMETRY SEND
                    
Telemetry Packet:   total_beacons = 10
                    successful = 9
                    failed = 1
                    detected = 0
                    last_latency_ms = 490
                    
Reset Counter = 0

Next Beacons 11-20: [accumulate again...]
```

### Training Server Reward Aggregation

```
Sample 1: reward = +8.2
Sample 2: reward = +9.1
Sample 3: reward = -5.0
Sample 4: reward = +10.5
Sample 5: reward = +9.8

Avg Reward = (8.2 + 9.1 - 5.0 + 10.5 + 9.8) / 5 = 6.52

Stored in agent_state.avg_reward = 6.52
Returned in GET /training/agent/<id> API response
```

---

## Error Recovery Flows

### Agent Loses Connection to Training Server

```
Agent                                Training Server
├─ Beacon success, report to         │
│  training_client                    │
├─ Counter reaches 10 (telemetry)    │
├─ Try to send telemetry ────X        │
│  (socket error)              no connection
│                               
├─ Catch error:                       
│  └─ connection_state =              
│     DISCONNECTED               
│                                
├─ Wait backoff (1s)                 
├─ Reconnect ────────→ (success)
├─ Resend pending ────────────→ (accepted)
│  telemetry                   
│                               
├─ Receive feedback ←──────────── (from cache)
│  (or empty if no new feedback)
│                               
└─ Continue beacons normally
```

### Training Server Crashes

```
Agent                              Training Server
│
├─ Sending telemetry ──X            (DOWN)
│  (connection timeout
│   after 10s)
│
├─ Report error, close socket
│
├─ Backoff exponentially
│  1s, 2s, 4s, 8s, 16s, 32s, 60s
│
├─ Continuous reconnect attempts
│
│
├─ (Training server restarts)
│                                  (ONLINE)
│
└─ Next reconnect attempt ─────→ (succeeds)
   (when backoff expires)        
   │                                
   ├─ Send pending telemetry ───→ (accepted)
   │                                
   └─ Resume normal operation
```

### Model Training Fails

```
Training Server
├─ Telemetry arrives
├─ Build state vector
├─ Call store_experience()
│
├─ During train_step():
│  ├─ Try to load batch from
│  │  replay_buffer
│  ├─ Torch computation
│  └─ ERROR: GPU out of memory
│
├─ Catch exception:
│  ├─ Log error
│  ├─ Fall back to CPU
│  └─ Retry train_step()
│
├─ Continue accepting telemetry
│  (samples still stored)
│
└─ Alert operator (TUI may show warning)
```

---

## Timing Guarantees

### Agent-Side Telemetry Latency

```
Beacon sent:                        T+0ms
└─ Success recorded in metrics
Telemetry triggered:                T+300ms (when counter hits 10)
│                                   (assumes 30s beacons)
└─ Packet serialized:               T+310ms
   Socket send initiated:           T+320ms
   (assuming normal network)
   └─ Server receives:              T+350ms (±50ms)
      └─ Parsed and stored:         T+360ms
         └─ Feedback generated:    T+375ms
            └─ Sent back:          T+385ms
               └─ Agent receives: T+420ms (±50ms)

Total latency: ~420ms from beacon to feedback
```

### Server-Side Training Latency

```
Sample 450 received:                T+0ms
└─ Stored in replay buffer
Sample 451 received:                T+100ms
├─ 450 % 50 = 0 (trigger train!)
├─ Batch sampling:                  T+110ms
├─ Forward pass:                    T+150ms
├─ Backward pass:                   T+200ms
├─ Optimizer step:                  T+220ms
└─ Model updated:                   T+230ms
   └─ New epsilon available:        T+240ms
      └─ Next select_action() uses new params

Total latency: ~240ms from trigger to model update
Next feedback uses new model:       T+350ms+
```

---

## Scaling Considerations

### Single Server Capacity

```
Telemetry packets/sec:              1000+ (socket bottleneck)
Training steps/sec:                 10-50 (GPU/CPU bound)
Concurrent agent connections:       100-200 (thread limit)

Assuming 10 beacons per telemetry:
└─ Supports ~100 agents @ 1 beacon/sec

Assuming 50 samples per train_step:
└─ Can train every 5 seconds (10 samples/sec rate)
```

### Multi-Agent Load Distribution

```
Agent 1: ├─ 20 beacons/sec → telemetry every 0.5s
Agent 2: ├─ 15 beacons/sec → telemetry every 0.67s
Agent 3: ├─ 10 beacons/sec → telemetry every 1.0s
Agent 4: └─ 30 beacons/sec → telemetry every 0.33s

Total telemetry: 4 packets / 0.5s = 8 packets/sec
Total training samples: ~150 samples/sec
Train_step triggered: every 50 samples = every 0.33s
```

---

## Data Persistence

### What Gets Persisted

```
Training Server (in memory during runtime):
├─ Telemetry buffer (last 10000 samples)
├─ Agent states (per-agent metrics)
├─ RL model state (PyTorch model.state_dict())
└─ Training metrics (loss history, epsilon, etc.)

Logging (to disk):
├─ /tmp/training_server.log (events, errors)
└─ /tmp/training_agent_<PID>.log (agent-side events)
```

### What Gets Lost on Restart

```
If training server crashes:
├─ Telemetry buffer cleared (agents will resend)
├─ Agent state metrics reset (but RL model preserved)
├─ Pending HTTP requests aborted
└─ Agents reconnect and resume

If agent crashes:
├─ Metrics counters reset
├─ Cached feedback lost (server will resend)
└─ Telemetry buffer (in client) lost
```

---

## Cold Start Scenario

```
Day 1:
├─ Start training server
├─ Start agent
│
├─ First beacon:
│  ├─ Server: "No history for this agent"
│  ├─ Default state: [hour, dow, 0.5, 0.5, 0.0, 0]
│  ├─ Action: random (epsilon=1.0)
│  └─ Reward: computed normally
│
├─ After 10 beacons:
│  ├─ First telemetry sent
│  ├─ Model stores experience
│  └─ First feedback: generic "maintain current" (low confidence)
│
├─ After 50 samples:
│  ├─ First train_step() triggered
│  ├─ Model learns from random exploration
│  └─ Epsilon drops (1.0 → 0.995^50 ≈ 0.61)
│
├─ After 1 hour:
│  ├─ 600 samples collected (60 beacons/hour)
│  ├─ 12 training steps executed
│  ├─ Model has seen:
│  │  ├─ VPN at 2pm → high detection
│  │  ├─ DNS at 2pm → low detection
│  │  └─ Latency patterns
│  ├─ Recommendations becoming confident
│  └─ Agent starts following recommendations
```

---

## Example Training Scenario: Agent Learns DNS is Better than VPN

```
Hour 1 (Explore):
├─ Beacons 1-5 with VPN:
│  ├─ Time: 2pm (business hours)
│  ├─ Detection likelihood: 0.80
│  ├─ Reward: +10 - 12 = -2 each
│  └─ Model: "VPN @ 2pm = bad"
│
├─ Beacons 6-10 with HTTPS:
│  ├─ Detection likelihood: 0.50
│  ├─ Reward: +10 - 7.5 = +2.5 each
│  └─ Model: "HTTPS @ 2pm = okay"
│
├─ Beacons 11-15 with DNS:
│  ├─ Detection likelihood: 0.20
│  ├─ Reward: +10 - 3.0 = +7.0 each
│  └─ Model: "DNS @ 2pm = good"
│
├─ After 50 samples (15 beacons):
│  └─ train_step() triggers
│     ├─ Q-network learns:
│     │  action[dns] @ state[hour=14] = highest reward
│     ├─ Epsilon decays: 1.0 → 0.99^1 ≈ 0.99
│     └─ Model more confident

Hour 2 (Exploit):
├─ select_action() now returns DNS with ~70% probability
│  (and explores alternatives with ~30%)
│
├─ Beacons 51-100: mostly DNS
│  ├─ Avg reward: +6.8 per beacon
│  └─ Model confirms: "DNS is best @ 2pm"
│
├─ After 100 samples:
│  └─ train_step() again
│     ├─ Q-network refines DNS Q-value
│     ├─ Epsilon: ~0.81
│     └─ Confidence at 80%

Hour 3 (Fine-tune):
├─ Agent now consistently uses DNS
├─ TUI shows: "DNS recommended, confidence 0.80"
├─ Operator confirms: "Agent is successfully undetected"
├─ Manual feedback: POST /training/feedback {"detected": false}
│  ├─ Injects +25 reward
│  ├─ Reinforces DNS behavior
│  └─ Model: "DNS @ 2pm with operator approval = +25"
│
└─ Agent continues with DNS, model converges
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-06-27 | Initial specification |

