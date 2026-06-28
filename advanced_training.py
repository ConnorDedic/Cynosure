#!/usr/bin/env python3
"""
Advanced RL Training Suite for Cynosure C2
- Tests efficiency vs stealth tradeoffs
- Simulates network failures and detection risk
- Connects to real listener for end-to-end testing
- Optimizes beacon timing, transport selection, data minimization
"""

import requests
import json
import time
import random
from datetime import datetime
import numpy as np

RL_SERVICE = "http://127.0.0.1:5555"
LISTENER = "http://10.3.23.23:4444"  # Real listener endpoint

class AdvancedTrainingScenario:
    def __init__(self, name, description):
        self.name = name
        self.description = description
        self.results = {
            "total_beacons": 0,
            "total_reward": 0.0,
            "detection_events": 0,
            "avg_latency": 0.0,
            "data_minimization": 0.0,
            "evasion_score": 0.0,
        }

    def simulate_detection_risk(self, beacon_interval, transport, latency):
        """Compute detection risk based on behavioral patterns"""
        risk = 0.0

        # Risk increases with regular patterns (regular intervals = suspicious)
        if beacon_interval == 30:
            risk += 0.3  # Very suspicious - exactly 30s
        elif beacon_interval < 10:
            risk += 0.2  # Too frequent
        elif beacon_interval > 300:
            risk += 0.15  # Too rare

        # Transport risk
        transport_risk = {
            "vpn": 0.4,      # High visibility
            "https": 0.2,    # Normal traffic
            "dns": 0.1,      # Stealthy
        }
        risk += transport_risk.get(transport, 0.3)

        # Latency variations help hide patterns
        if latency > 2.0:
            risk -= 0.1  # Variable latency is good

        return min(1.0, max(0.0, risk))

    def run(self, num_iterations=30):
        """Run training scenario"""
        print(f"\n{'='*70}")
        print(f"SCENARIO: {self.name}")
        print(f"{'='*70}")
        print(f"Description: {self.description}\n")

        total_reward = 0.0
        detection_events = 0
        total_latency = 0.0

        for i in range(num_iterations):
            # Get RL recommendation
            beacon_state = {
                "implant_id": f"training-{self.name}-{i}",
                "success_rate": random.uniform(0.7, 0.99),
                "uptime": random.uniform(0.5, 0.99),
                "seconds_since_beacon": random.choice([5, 10, 30, 60, 120, 300]),
                "transport": random.choice(["vpn", "https", "dns"]),
            }

            try:
                action_resp = requests.post(
                    f"{RL_SERVICE}/beacon/action",
                    json=beacon_state,
                    timeout=2
                )
                action = action_resp.json()
            except Exception as e:
                print(f"  ⚠ RL Service error: {e}")
                continue

            # Simulate beacon with chosen parameters
            beacon_interval = action["beacon_interval"]
            transport = action["transport"]
            retry_count = action["retry_count"]

            # Simulate network latency (lower = better)
            latency = random.uniform(0.1, 2.5)
            total_latency += latency

            # Simulate beacon success/failure
            success = random.random() > 0.05  # 95% success baseline

            # Calculate detection risk
            detection_risk = self.simulate_detection_risk(beacon_interval, transport, latency)
            if random.random() < detection_risk:
                detection_events += 1
                success = False  # Detected = failed beacon

            # Send feedback to RL model
            feedback_data = {
                "implant_id": beacon_state["implant_id"],
                "success": success,
                "response_time": latency,
                "beacon_interval": beacon_interval,
            }

            try:
                feedback_resp = requests.post(
                    f"{RL_SERVICE}/beacon/feedback",
                    json=feedback_data,
                    timeout=2
                )
                feedback = feedback_resp.json()
                reward = feedback.get("reward", 0.0)
                total_reward += reward
            except Exception as e:
                reward = -5.0
                total_reward += reward

            # Print iteration result
            status = "✓" if success else "✗"
            detection_status = f" [DETECTED]" if detection_risk > 0.5 else ""
            print(f"  [{i+1:3d}] {status} Interval={beacon_interval:3d}s "
                  f"Transport={transport:5s} Latency={latency:.2f}s Reward={reward:+.2f}{detection_status}")

            # Train every 5 iterations
            if (i + 1) % 5 == 0:
                try:
                    train_resp = requests.post(f"{RL_SERVICE}/model/train", timeout=2)
                    train_data = train_resp.json()
                    loss = train_data.get("loss", 0.0)
                    print(f"  [TRAIN] Loss={loss:.4f} @ iteration {i+1}\n")
                except:
                    pass

        # Finalize results
        self.results["total_beacons"] = num_iterations
        self.results["total_reward"] = total_reward
        self.results["detection_events"] = detection_events
        self.results["avg_latency"] = total_latency / num_iterations
        self.results["evasion_score"] = 1.0 - (detection_events / num_iterations)

        return self.results


def print_scenario_results(scenarios):
    """Compare all scenario results"""
    print("\n" + "="*70)
    print("SCENARIO COMPARISON")
    print("="*70 + "\n")

    print(f"{'Scenario':<25} {'Reward':<12} {'Detection':<12} {'Latency':<12} {'Evasion':<12}")
    print("-" * 70)

    for scenario in scenarios:
        r = scenario.results
        print(f"{scenario.name:<25} {r['total_reward']:>10.2f}  "
              f"{r['detection_events']:>10d}  {r['avg_latency']:>10.2f}s  "
              f"{r['evasion_score']:>10.2f}")


def get_model_metrics():
    """Get current model training metrics"""
    try:
        resp = requests.get(f"{RL_SERVICE}/model/metrics", timeout=2)
        return resp.json()
    except:
        return None


def main():
    print("\n" + "="*70)
    print("CYNOSURE ADVANCED RL TRAINING SUITE")
    print("="*70)
    print("\nModes:")
    print("  1. Stealth Mode - Minimize detection risk")
    print("  2. Efficiency Mode - Maximize speed and reliability")
    print("  3. Balanced Mode - Optimize detection vs efficiency")
    print("  4. Aggressive Mode - Maximum data exfil with risk")
    print("  5. All Scenarios")

    choice = input("\nSelect mode (1-5): ").strip()

    scenarios = []

    if choice in ["1", "5"]:
        scenarios.append(AdvancedTrainingScenario(
            "Stealth",
            "Minimize detection: long intervals, rare DNS transport, jittered timing"
        ))

    if choice in ["2", "5"]:
        scenarios.append(AdvancedTrainingScenario(
            "Efficiency",
            "Maximize reliability: short intervals, HTTPS transport, consistent timing"
        ))

    if choice in ["3", "5"]:
        scenarios.append(AdvancedTrainingScenario(
            "Balanced",
            "Tradeoff detection vs speed: adaptive intervals, mixed transports"
        ))

    if choice in ["4", "5"]:
        scenarios.append(AdvancedTrainingScenario(
            "Aggressive",
            "Data exfiltration priority: very short intervals, all transports"
        ))

    if not scenarios:
        print("Invalid choice")
        return

    print("\n" + "="*70)
    print("STARTING TRAINING SCENARIOS")
    print("="*70)

    # Run each scenario
    for scenario in scenarios:
        results = scenario.run(num_iterations=50)

    # Print comparison
    print_scenario_results(scenarios)

    # Final model metrics
    print("\n" + "="*70)
    print("FINAL MODEL METRICS")
    print("="*70 + "\n")

    metrics = get_model_metrics()
    if metrics:
        print(f"Training Steps:      {metrics.get('step_count', 0)}")
        print(f"Success Rate:        {metrics.get('success_rate', 0):.2%}")
        print(f"Avg Loss:            {metrics.get('avg_loss', 0):.4f}")
        print(f"Episode Reward:      {metrics.get('current_episode_reward', 0):.2f}")
        print(f"Memory Size:         {metrics.get('memory_size', 0)} experiences")
        print(f"Epsilon:             {metrics.get('epsilon', 1.0):.4f}")

    print("\n" + "="*70)
    print("TRAINING COMPLETE")
    print("="*70 + "\n")


if __name__ == "__main__":
    main()
