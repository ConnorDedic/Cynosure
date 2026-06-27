# Training TUI Integration Specification

## Overview

The TUI integrates with the training server via HTTP API to provide user control over RL training. When an agent is in training mode, a new popup menu allows the operator to:
- Mark agents as detected (inject negative reward)
- Mark agents as successful (inject positive reward)
- View detailed metrics
- Inject network traffic patterns
- Force immediate training steps
- View action history

This specification details the UI components, state management, and HTTP communication.

---

## Component Architecture

### File Structure
```
src/main.rs                            (MODIFIED)
  - Add PopupMode::TrainingControl variant
  - Add training_menu render function
  - Add HTTP API client wrapper
  - Add training metrics cache
```

### Dependencies
- `reqwest` or `curl` for HTTP (already in Cargo.toml)
- No new external dependencies

### Integration Points
1. **TUI Input**: Keyboard handlers for training menu
2. **HTTP Client**: Call training server on localhost:5557
3. **PopupMode enum**: New variant for training control
4. **Agent state**: Display training metrics alongside normal session data

---

## Data Structures

### Training Control State

```rust
#[derive(Clone)]
struct TrainingControlState {
    /// Training menu mode
    menu_mode: TrainingMenuMode,  // Active menu section
    
    /// Selected agent for training
    selected_agent_id: String,
    
    /// Cached metrics (refreshed every 2 seconds)
    agent_metrics: AgentMetricsCache,
    
    /// Pending HTTP request
    pending_request: Option<TrainingRequest>,
    
    /// Response status
    request_status: String,  // "idle", "loading", "success", "error"
    request_error: Option<String>,
}

#[derive(Clone, PartialEq)]
enum TrainingMenuMode {
    MainMenu,              // Show top-level options
    DetectionConfirm,      // Confirm marking as detected
    SuccessfulConfirm,     // Confirm marking as successful
    NetworkTrafficMenu,    // Choose network condition
    MetricsView,          // Display metrics details
    ActionHistory,        // Show recommended actions
    ForceTrainConfirm,    // Confirm force training
}

#[derive(Clone)]
struct AgentMetricsCache {
    agent_id: String,
    total_beacons: u64,
    successful_beacons: u64,
    failed_beacons: u64,
    detected_count: u64,
    current_transport: String,
    beacon_interval_ms: u32,
    success_rate: f32,
    detection_likelihood: f32,
    last_latency_ms: u32,
    last_action: String,  // JSON-serialized
    avg_reward: f32,
    last_updated: SystemTime,
}

enum TrainingRequest {
    MarkDetected { agent_id: String },
    MarkSuccessful { agent_id: String },
    SimulateNetworkCondition { agent_id: String, condition: String },
    ForceTrain,
    FetchMetrics { agent_id: String },
}
```

### HTTP Request/Response Types

```rust
// POST /training/feedback
#[derive(Serialize)]
struct FeedbackRequest {
    agent_id: String,
    feedback_type: String,  // "detected" or "successful"
    timestamp: f64,
}

#[derive(Deserialize)]
struct FeedbackResponse {
    status: String,
    reward_injected: f32,
    message: String,
}

// GET /training/agent/<agent_id>
#[derive(Deserialize)]
struct AgentMetricsResponse {
    agent_id: String,
    first_seen: f64,
    last_telemetry: f64,
    total_beacons: u64,
    successful_beacons: u64,
    failed_beacons: u64,
    detected_count: u64,
    current_transport: String,
    beacon_interval_ms: u32,
    success_rate: f32,
    detection_likelihood: f32,
    last_action: serde_json::Value,
    action_history: Vec<serde_json::Value>,
    avg_reward: f32,
    samples_trained: u64,
}

// POST /training/train
#[derive(Serialize)]
struct ForceTrainRequest {
    batch_size: u32,
}

#[derive(Deserialize)]
struct ForceTrainResponse {
    status: String,
    training_loss: f32,
    samples_used: u32,
    epsilon: f32,
}

// POST /training/simulate-network
#[derive(Serialize)]
struct SimulateNetworkRequest {
    agent_id: String,
    condition: String,  // "normal", "congested", "unstable"
    duration_seconds: u32,
}

#[derive(Deserialize)]
struct SimulateNetworkResponse {
    status: String,
    condition_active_until: f64,
}
```

### Panel and PopupMode Updates

```rust
// In existing PopupMode enum, add:
enum PopupMode {
    None,
    PSDisplay,
    SysInfoDisplay,
    ShellInteractive,
    TrainingControl,  // NEW
}

// In App struct, add:
struct App {
    // ... existing fields ...
    
    // Training-specific state
    training_control: Option<TrainingControlState>,  // None if not in training mode
    training_http_client: Option<TrainingHttpClient>,  // HTTP client to server
}
```

---

## UI Specification

### Main Training Control Menu

**Popup Title**: " RL Agent Training Control "

**Menu Layout:**
```
┌─ RL Agent Training Control ────────────────────────────────┐
│                                                            │
│ Agent: agent-abc123def456...                              │
│ Status: Connected (last 2 seconds ago)                    │
│                                                            │
│ ┌─ Metrics ─────────────────────────────────────────────┐ │
│ │ Beacons: 150 (120 success, 30 failed, 5 detected)     │ │
│ │ Transport: DNS (70%), HTTPS (20%), VPN (10%)         │ │
│ │ Interval: 30s | Latency: avg 450ms, min 120ms, max   │ │
│ │           3200ms                                       │ │
│ │ Success Rate: 80% | Detection Likelihood: 0.25        │ │
│ │ Avg Reward: 8.2 | Confidence: 0.65                   │ │
│ └──────────────────────────────────────────────────────┘ │
│                                                            │
│ Recommendation:                                            │
│ → Continue DNS every 30-45s during business hours        │
│   Confidence: 0.65                                        │
│                                                            │
│ ┌─ Actions ──────────────────────────────────────────────┐ │
│ │ [D] Mark as DETECTED (inject -20 reward)              │ │
│ │ [S] Mark as SUCCESSFUL (inject +25 reward)            │ │
│ │ [N] Inject Network Traffic...                          │ │
│ │ [M] View Detailed Metrics                              │ │
│ │ [A] View Action History                                │ │
│ │ [T] Force Training Step                                │ │
│ │ [Esc] Close Menu                                       │ │
│ └──────────────────────────────────────────────────────┘ │
│                                                            │
│ Status: Idle                                               │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### Detection Confirmation Dialog

**Activated by**: Press 'D' in main menu

```
┌─ Confirm Detection ────────────────────────────────────┐
│                                                        │
│ Are you sure this agent was DETECTED?                 │
│                                                        │
│ This will inject a -20 reward penalty and retrain     │
│ the model with this negative example.                 │
│                                                        │
│ Current state:                                         │
│   Transport: DNS                                      │
│   Interval: 30s                                       │
│   Time: 2024-06-27 14:25:00 (2pm, business hours)   │
│                                                        │
│ [Y] Yes, mark as detected                            │
│ [N] Cancel                                            │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### Success Confirmation Dialog

**Activated by**: Press 'S' in main menu

```
┌─ Confirm Success ──────────────────────────────────────┐
│                                                        │
│ Mark this agent's evasion as SUCCESSFUL?              │
│                                                        │
│ This will inject a +25 reward bonus and teach the     │
│ model this pattern works well.                        │
│                                                        │
│ Current pattern:                                      │
│   Transport: DNS                                      │
│   Interval: 30s                                       │
│   Time: 2024-06-27 14:25:00 (2pm, business hours)   │
│   Success Rate: 80%                                   │
│                                                        │
│ [Y] Yes, mark successful                             │
│ [N] Cancel                                            │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### Network Traffic Menu

**Activated by**: Press 'N' in main menu

```
┌─ Inject Network Traffic ───────────────────────────────┐
│                                                        │
│ Simulate network condition changes (for testing):     │
│                                                        │
│ [1] Normal      - baseline latency, no packet loss    │
│ [2] Congested   - +500ms latency, 10% packet loss    │
│ [3] Unstable    - variable latency, 20% packet loss  │
│ [4] Severe      - +2000ms latency, 50% packet loss   │
│ [5] Blackout    - complete connectivity loss (10s)   │
│ [Esc] Cancel                                          │
│                                                        │
│ Duration: 60 seconds (configurable with +/-)         │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### Detailed Metrics View

**Activated by**: Press 'M' in main menu

```
┌─ Agent Metrics (agent-abc123) ──────────────────────────┐
│                                                         │
│ BEACON STATISTICS:                                      │
│   Total Sent:     150                                   │
│   Successful:     120  (80%)  ▓▓▓▓▓▓▓░░░              │
│   Failed:          30  (20%)  ▓▓░░░░░░░░              │
│   Detected:         5  (3%)   ▓░░░░░░░░░              │
│                                                         │
│ TRANSPORT BREAKDOWN:                                    │
│   DNS:            105  (70%)  ▓▓▓▓▓▓░░░░              │
│   HTTPS:           30  (20%)  ▓▓░░░░░░░░              │
│   VPN:             15  (10%)  ▓░░░░░░░░░              │
│                                                         │
│ TIMING ANALYSIS:                                        │
│   Beacon Interval:     30 seconds                       │
│   Latency (last):      450 ms                          │
│   Latency (avg):       480 ms                          │
│   Latency (min/max):   120 ms / 3200 ms               │
│   Frequency (est):     300 beacons/hour                │
│                                                         │
│ EVASION METRICS:                                        │
│   Success Rate:        80%                              │
│   Detection Risk:      0.25 (25% likelihood)           │
│   Reward (avg):        8.2                              │
│   Samples Trained:     150                              │
│                                                         │
│ TIME ANALYSIS:                                          │
│   First Seen:  2024-06-27 14:15:30 (9 min ago)       │
│   Last Update: 2024-06-27 14:25:10 (just now)        │
│   Business Hours? YES (2:25 PM, Wednesday)            │
│                                                         │
│ [Space] Refresh | [Esc] Back                           │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Action History View

**Activated by**: Press 'A' in main menu

```
┌─ Action History (Last 10) ─────────────────────────────┐
│                                                        │
│ #10  [2:24:50 UTC] DNS  every 30s  (confidence 0.65)  │
│      Reason: Low detection risk during business hrs   │
│                                                        │
│ #9   [2:24:40 UTC] HTTPS every 45s (confidence 0.45)  │
│      Reason: Balance between stealth & connectivity   │
│                                                        │
│ #8   [2:24:30 UTC] VPN  every 60s  (confidence 0.25)  │
│      Reason: High detection risk during 9-5          │
│                                                        │
│ #7   [2:24:20 UTC] DNS  every 30s  (confidence 0.70)  │
│      Reason: Optimal pattern - maintain this        │
│                                                        │
│ (previous 6 actions...)                              │
│                                                        │
│ Pattern: Model recommends DNS during business hours  │
│          HTTPS during off-hours, VPN rarely used     │
│                                                        │
│ [↑/↓] Scroll | [Esc] Back                             │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### Force Training Confirmation

**Activated by**: Press 'T' in main menu

```
┌─ Force Training Step ──────────────────────────────────┐
│                                                        │
│ Trigger immediate training on all buffered samples?   │
│                                                        │
│ Current state:                                         │
│   Samples in buffer: 45                                │
│   Batch size: 32                                      │
│   Will train on: 32 samples (newest)                  │
│                                                        │
│ Training typically runs every 50 samples (automatic)  │
│ Use this only for debugging/urgent model updates.    │
│                                                        │
│ [Y] Yes, train now                                    │
│ [N] Cancel                                            │
│                                                        │
│ After training:                                       │
│ [Will show loss, epsilon, samples_used]              │
│                                                        │
└────────────────────────────────────────────────────────┘
```

---

## Keyboard Controls

### In Training Control Menu

| Key | Action | Transition |
|-----|--------|------------|
| `D` | Mark detected | → DetectionConfirm dialog |
| `S` | Mark successful | → SuccessfulConfirm dialog |
| `N` | Network traffic | → NetworkTrafficMenu |
| `M` | View metrics | → MetricsView (paginated) |
| `A` | Action history | → ActionHistory |
| `T` | Force training | → ForceTrainConfirm dialog |
| `↑` / `↓` | Navigate menu | (visual highlight only) |
| `Space` | Refresh metrics | Fetch latest from server |
| `Esc` | Close menu | → PopupMode::None |

### In Confirmation Dialogs

| Key | Action |
|-----|--------|
| `Y` | Confirm, send HTTP request |
| `N` | Cancel, back to main menu |
| `Esc` | Cancel, back to main menu |

### In Network Traffic Menu

| Key | Action |
|-----|--------|
| `1`-`5` | Select condition, show confirmation |
| `+` | Increase duration by 10s |
| `-` | Decrease duration by 10s |
| `Esc` | Cancel |

### In Metrics View

| Key | Action |
|-----|--------|
| `Space` | Refresh (fetch latest) |
| `↑` / `↓` | Scroll if content overflows |
| `Esc` | Back to main menu |

---

## HTTP Communication Flow

### Entry Point

```rust
// In input handler, when user is on SessionDetail and presses 'T':
if key == KeyCode::Char('t') {
    if agent_in_training_mode(&selected_session) {
        app.popup_mode = PopupMode::TrainingControl;
        app.training_control = Some(TrainingControlState::new(agent_id));
        
        // Spawn background thread to fetch initial metrics
        spawn_fetch_metrics_task(agent_id);
    }
}
```

### Async HTTP Requests

```rust
// HTTP client wrapper (in main.rs or separate module)
struct TrainingHttpClient {
    client: reqwest::Client,
    base_url: String,  // "http://localhost:5557"
    timeout: Duration,
}

impl TrainingHttpClient {
    async fn mark_detected(&self, agent_id: &str) -> Result<FeedbackResponse> {
        let body = FeedbackRequest {
            agent_id: agent_id.to_string(),
            feedback_type: "detected".to_string(),
            timestamp: current_timestamp(),
        };
        
        self.client
            .post(format!("{}/training/feedback", self.base_url))
            .json(&body)
            .timeout(self.timeout)
            .send()
            .await?
            .json()
            .await
    }
    
    async fn mark_successful(&self, agent_id: &str) -> Result<FeedbackResponse> {
        let body = FeedbackRequest {
            agent_id: agent_id.to_string(),
            feedback_type: "successful".to_string(),
            timestamp: current_timestamp(),
        };
        
        self.client
            .post(format!("{}/training/feedback", self.base_url))
            .json(&body)
            .timeout(self.timeout)
            .send()
            .await?
            .json()
            .await
    }
    
    async fn get_agent_metrics(&self, agent_id: &str) -> Result<AgentMetricsResponse> {
        self.client
            .get(format!("{}/training/agent/{}", self.base_url, agent_id))
            .timeout(self.timeout)
            .send()
            .await?
            .json()
            .await
    }
    
    async fn force_train(&self) -> Result<ForceTrainResponse> {
        let body = ForceTrainRequest { batch_size: 32 };
        
        self.client
            .post(format!("{}/training/train", self.base_url))
            .json(&body)
            .timeout(self.timeout)
            .send()
            .await?
            .json()
            .await
    }
    
    async fn simulate_network(
        &self,
        agent_id: &str,
        condition: &str,
        duration_sec: u32,
    ) -> Result<SimulateNetworkResponse> {
        let body = SimulateNetworkRequest {
            agent_id: agent_id.to_string(),
            condition: condition.to_string(),
            duration_seconds: duration_sec,
        };
        
        self.client
            .post(format!("{}/training/simulate-network", self.base_url))
            .json(&body)
            .timeout(self.timeout)
            .send()
            .await?
            .json()
            .await
    }
}
```

### Request State Machine

```rust
// In render function:
match app.training_control.request_status.as_str() {
    "idle" => {
        // Show menu normally
        draw_training_control_menu(f, app);
    }
    "loading" => {
        // Show spinner
        draw_loading_spinner(f, "Sending request...");
    }
    "success" => {
        // Show success message (2 second timeout)
        draw_success_message(f, &app.training_control.request_status);
        
        // After 2 seconds, reset to idle and refresh metrics
        if elapsed_since_request > 2000ms {
            app.training_control.request_status = "idle".to_string();
            spawn_fetch_metrics_task(agent_id);
        }
    }
    "error" => {
        // Show error message (3 second timeout)
        draw_error_message(f, app.training_control.request_error.as_ref());
        
        if elapsed_since_request > 3000ms {
            app.training_control.request_status = "idle".to_string();
        }
    }
    _ => {}
}
```

### Metrics Refresh Strategy

```rust
// Auto-refresh metrics every 2 seconds while training menu is open
fn update_training_control(app: &mut App) {
    if let Some(training_state) = &mut app.training_control {
        let now = SystemTime::now();
        let elapsed = now.duration_since(training_state.agent_metrics.last_updated)
            .unwrap_or(Duration::from_secs(10));
        
        if elapsed > Duration::from_secs(2) {
            spawn_fetch_metrics_task(training_state.selected_agent_id.clone());
            training_state.agent_metrics.last_updated = now;
        }
    }
}
```

---

## Error Handling

### Network Errors

| Error | Display | Recovery |
|-------|---------|----------|
| Server unreachable | "Training server offline" | Retry on next refresh |
| Request timeout | "Request timed out" | Show cached metrics |
| Invalid JSON response | "Server error: invalid response" | Log error |
| Agent not found | "Agent not found on server" | Go back to menu |

### User Input Errors

| Error | Handling |
|-------|----------|
| Menu open without training mode | Don't open (check flag) |
| Agent disconnected from server | Show "Last seen: X minutes ago" |
| Server returned error on request | Display error message, keep menu open |
| Metrics refresh fails | Keep showing previous metrics |

---

## Rendering Implementation

### Main Menu Render

```rust
fn draw_training_control_menu(f: &mut Frame, app: &App) {
    let training = app.training_control.as_ref().unwrap();
    let metrics = &training.agent_metrics;
    
    // Main block
    let block = Block::default()
        .title(" RL Agent Training Control ")
        .borders(Borders::ALL)
        .border_type(BorderType::Double);
    
    let inner = block.inner(popup_area);
    f.render_widget(block, popup_area);
    
    // Title with agent ID
    let title_line = Line::from(vec![
        Span::styled("Agent: ", Style::default().fg(Color::Cyan)),
        Span::raw(&metrics.agent_id),
    ]);
    f.render_widget(Paragraph::new(title_line), inner);
    
    // Metrics summary (wrapped in a block)
    let metrics_block = Block::default()
        .title(" Metrics ")
        .borders(Borders::ALL)
        .style(Style::default().fg(Color::Gray));
    
    let metrics_text = format!(
        "Beacons: {} ({} success, {} failed, {} detected)\n\
         Transport: {} | Interval: {}ms\n\
         Latency: {} avg | Success Rate: {:.1}% | Detection: {:.2}",
        metrics.total_beacons,
        metrics.successful_beacons,
        metrics.failed_beacons,
        metrics.detected_count,
        metrics.current_transport,
        metrics.beacon_interval_ms,
        metrics.last_latency_ms,
        metrics.success_rate * 100.0,
        metrics.detection_likelihood
    );
    
    f.render_widget(
        Paragraph::new(metrics_text).block(metrics_block),
        metrics_area
    );
    
    // Action menu
    let actions = vec![
        "[D] Mark as DETECTED",
        "[S] Mark as SUCCESSFUL",
        "[N] Inject Network Traffic",
        "[M] View Metrics",
        "[A] Action History",
        "[T] Force Training",
        "[Esc] Close Menu",
    ];
    
    for (i, action) in actions.iter().enumerate() {
        let style = if i == training.selected_index {
            Style::default().bg(Color::DarkGray)
        } else {
            Style::default()
        };
        
        f.render_widget(
            Paragraph::new(*action).style(style),
            action_area[i]
        );
    }
}
```

---

## Platform-Specific Notes

### Windows (MinGW)

- HTTP client requires `reqwest` with native TLS (uses system cert store)
- No special handling needed

### macOS

- HTTP client uses native TLS (SecureTransport)
- No special handling needed

### Linux

- HTTP client uses OpenSSL or rustls (depend on `reqwest` feature)
- Verify system has CA certificates installed

---

## Testing Approach

### Unit Tests

```rust
#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_training_state_creation() {
        let state = TrainingControlState::new("agent123");
        assert_eq!(state.menu_mode, TrainingMenuMode::MainMenu);
    }
    
    #[tokio::test]
    async fn test_http_client_mark_detected() {
        let client = TrainingHttpClient::new("http://localhost:5557");
        // Mock server would be needed here
        // let resp = client.mark_detected("agent123").await;
    }
}
```

### Integration Tests

```bash
# Start training server and TUI
python3 src/training_server.py &
cargo run --release

# Manually test:
# 1. Select agent in SessionDetail
# 2. Press 'T' to open training menu
# 3. Verify metrics display
# 4. Press 'D' to mark detected
# 5. Verify confirmation dialog
# 6. Press 'Y' to confirm
# 7. Check /tmp/training_server.log for feedback receipt
```

---

## Performance Characteristics

| Metric | Target | Tolerance |
|--------|--------|-----------|
| Menu open latency | <200ms | ±100ms |
| Metrics fetch | <500ms | ±250ms |
| HTTP request | <1000ms | ±500ms |
| Metrics refresh rate | 2s interval | ±500ms |
| Menu responsiveness | <50ms per keypress | ±20ms |

---

## Security Considerations

### What is NOT protected

- **HTTP traffic**: Telemetry and feedback sent over plain HTTP (use on trusted network)
- **No input sanitization**: User-provided agent_id passed directly to URL

### What IS protected

- **Timeout protection**: All HTTP requests have 10-second timeout (prevents hang)
- **Error recovery**: Network errors don't crash TUI
- **UI state isolation**: Training state independent from session state

### Hardening Recommendations

- Run training server on private/internal network only
- Use firewall to restrict port 5557 to TUI operator only
- Consider adding HTTP basic auth to training server API
- Run on same machine as training server (localhost) if possible

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-06-27 | Initial specification |

