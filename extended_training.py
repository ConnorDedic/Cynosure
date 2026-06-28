#!/usr/bin/env python3
"""
Extended RL Training - 500 Beacons per Scenario
Tests model convergence and transport diversification
"""

import requests
import random
import json
from datetime import datetime

RL_SERVICE = "http://127.0.0.1:5555"

class ExtendedTraining:
    def __init__(self, scenario_name, description):
        self.name = scenario_name
        self.description = description
        self.rewards = []
        self.detections = []
        self.transports_used = {"vpn": 0, "https": 0, "dns": 0}
        self.intervals_used = {}

    def run(self, iterations=500):
        """Run extended training scenario"""
        print(f"\n{'='*80}")
        print(f"EXTENDED TRAINING: {self.name}")
        print(f"{self.description}")
        print(f"Running {iterations} beacon cycles...")
        print(f"{'='*80}\n")

        detected_count = 0
        total_reward = 0.0

        for i in range(iterations):
            # Random beacon state
            beacon_state = {
                "implant_id": f"extended-{self.name}-{i}",
                "success_rate": random.uniform(0.6, 0.99),
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
            except:
                continue

            beacon_interval = action["beacon_interval"]
            transport = action["transport"]

            # Track statistics
            self.transports_used[transport] = self.transports_used.get(transport, 0) + 1
            self.intervals_used[beacon_interval] = self.intervals_used.get(beacon_interval, 0) + 1

            # Simulate detection
            latency = random.uniform(0.1, 2.5)
            success = random.random() > 0.1  # 90% baseline

            # Higher risk for suspicious patterns
            if beacon_interval == 5 or beacon_interval == 10:
                if random.random() < 0.3:  # 30% detection for fast intervals
                    detected_count += 1
                    success = False

            if transport == "vpn" and beacon_interval == 30:
                if random.random() < 0.25:  # VPN+30s is suspicious
                    detected_count += 1
                    success = False

            # Send feedback
            try:
                feedback_resp = requests.post(
                    f"{RL_SERVICE}/beacon/feedback",
                    json={
                        "implant_id": beacon_state["implant_id"],
                        "success": success,
                        "response_time": latency,
                        "beacon_interval": beacon_interval,
                    },
                    timeout=2
                )
                feedback = feedback_resp.json()
                reward = feedback.get("reward", 0.0)
                total_reward += reward
                self.rewards.append(reward)
                self.detections.append(1 if detected_count > 0 else 0)
            except:
                pass

            # Train every 25 iterations
            if (i + 1) % 25 == 0:
                try:
                    requests.post(f"{RL_SERVICE}/model/train", timeout=2)
                    avg_reward = sum(self.rewards[-25:]) / 25 if len(self.rewards) > 0 else 0
                    detection_rate = sum(self.detections[-25:]) / 25 if len(self.detections) > 0 else 0

                    # Get model state
                    metrics_resp = requests.get(f"{RL_SERVICE}/model/metrics", timeout=2)
                    metrics = metrics_resp.json()

                    print(f"  [{i+1:4d}] Avg Reward: {avg_reward:+7.2f} | "
                          f"Detection: {detection_rate:.1%} | "
                          f"Loss: {metrics.get('avg_loss', 0):.4f} | "
                          f"Epsilon: {metrics.get('epsilon', 1.0):.3f}")
                except:
                    pass

        # Final statistics
        print(f"\n{'─'*80}")
        print(f"FINAL STATISTICS")
        print(f"{'─'*80}")
        print(f"Total Beacons:     {iterations}")
        print(f"Detected:          {detected_count} ({detected_count*100//iterations}%)")
        print(f"Evasion Success:   {(iterations-detected_count)*100//iterations}%")
        print(f"Total Reward:      {total_reward:.2f}")
        print(f"Avg Reward/Beacon: {total_reward/iterations:.2f}")
        print(f"\nTransport Distribution:")
        for transport, count in sorted(self.transports_used.items(), key=lambda x: -x[1]):
            pct = count * 100 // iterations
            print(f"  {transport:6s}: {count:3d} beacons ({pct:2d}%)")

        print(f"\nTop 5 Interval Choices:")
        for interval, count in sorted(self.intervals_used.items(), key=lambda x: -x[1])[:5]:
            pct = count * 100 // iterations
            print(f"  {interval:3d}s: {count:3d} beacons ({pct:2d}%)")

        return {
            "detected": detected_count,
            "evasion_rate": (iterations - detected_count) / iterations,
            "total_reward": total_reward,
            "avg_reward": total_reward / iterations,
        }


def main():
    print("\n" + "="*80)
    print("CYNOSURE EXTENDED RL TRAINING")
    print("500 Beacons Per Scenario - Convergence Testing")
    print("="*80)

    scenarios = [
        ExtendedTraining(
            "Balanced-500",
            "500-beacon balanced training (from best previous scenario)"
        ),
    ]

    results = []
    for scenario in scenarios:
        result = scenario.run(iterations=500)
        results.append({
            "scenario": scenario.name,
            "result": result,
        })

    # Get final model metrics
    print(f"\n{'='*80}")
    print("FINAL MODEL STATE AFTER 500-BEACON TRAINING")
    print(f"{'='*80}\n")

    try:
        metrics_resp = requests.get(f"{RL_SERVICE}/model/metrics", timeout=2)
        metrics = metrics_resp.json()

        print(f"Training Steps:         {metrics.get('step_count', 0)}")
        print(f"Overall Success Rate:   {metrics.get('success_rate', 0):.2%}")
        print(f"Average Loss:           {metrics.get('avg_loss', 0):.6f}")
        print(f"Total Episode Reward:   {metrics.get('current_episode_reward', 0):.2f}")
        print(f"Memory Experiences:     {metrics.get('memory_size', 0)}")
        print(f"Exploration Rate (ε):   {metrics.get('epsilon', 1.0):.4f}")
    except:
        print("Could not fetch model metrics")

    print(f"\n{'='*80}")
    print("✅ EXTENDED TRAINING COMPLETE")
    print(f"{'='*80}\n")


if __name__ == "__main__":
    main()
