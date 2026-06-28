#!/usr/bin/env python3
"""
Realistic Detection Simulator for Cynosure C2
- Models EDR/IDS detection patterns
- Simulates network anomaly detection
- Tests behavioral evasion
- Provides realistic training rewards
"""

import random
import json
from datetime import datetime, timedelta
from typing import Dict, Tuple

class DetectionSimulator:
    """Simulates realistic network and behavioral detection"""

    def __init__(self):
        self.detection_thresholds = {
            "beacon_frequency": {
                "very_high": (0, 5),      # Every 5s = very suspicious
                "high": (5, 30),           # Every 30s = suspicious
                "normal": (30, 300),       # Every 30-5min = normal
                "low": (300, 3600),        # Every 5min+ = stealth
            },
            "transport_patterns": {
                "vpn": {"baseline_risk": 0.4, "variation_bonus": -0.1},
                "https": {"baseline_risk": 0.2, "variation_bonus": -0.05},
                "dns": {"baseline_risk": 0.05, "variation_bonus": -0.15},
            },
            "behavioral_signatures": {
                "same_interval_8x": 0.3,        # Using exact same interval 8 times = suspicious
                "always_same_transport": 0.25,  # Always same transport = signature
                "midnight_beacons": 0.2,        # 2-6am beacons = suspicious
                "business_hours_only": 0.15,    # Only 9-5 = suspicious
                "no_variance": 0.35,            # Zero jitter = detected
            },
        }

        # Track beacon history for pattern detection
        self.beacon_history = []
        self.transport_history = []

    def calculate_detection_risk(self, beacon_interval: int, transport: str,
                                 jitter: float = 0.1, time_of_day: int = 12) -> Tuple[float, str]:
        """
        Calculate realistic detection risk based on behavioral patterns

        Returns:
            (risk_score: 0.0-1.0, reason: string)
        """
        risk = 0.0
        reasons = []

        # --- Beacon Interval Analysis ---
        interval_category = self._categorize_interval(beacon_interval)
        interval_risks = {
            "very_high": 0.5,
            "high": 0.3,
            "normal": 0.1,
            "low": 0.05,
        }
        interval_risk = interval_risks[interval_category]
        risk += interval_risk
        reasons.append(f"Interval: {interval_category} (+{interval_risk:.2f})")

        # --- Transport Analysis ---
        transport_baseline = self.detection_thresholds["transport_patterns"][transport]["baseline_risk"]
        risk += transport_baseline
        reasons.append(f"Transport: {transport} (+{transport_baseline:.2f})")

        # --- Jitter/Variance Analysis ---
        if jitter < 0.05:
            risk += 0.3
            reasons.append("No jitter detected (+0.30)")
        elif jitter < 0.15:
            risk += 0.15
            reasons.append("Low jitter (+0.15)")
        else:
            risk -= 0.1  # Good variance
            reasons.append("Good variance (-0.10)")

        # --- Behavioral Patterns ---
        if len(self.beacon_history) >= 8:
            # Check for repeated intervals
            recent_intervals = self.beacon_history[-8:]
            if all(i == recent_intervals[0] for i in recent_intervals):
                risk += self.detection_thresholds["behavioral_signatures"]["same_interval_8x"]
                reasons.append("8x same interval pattern (+0.30)")

        # Check transport switching
        if len(self.transport_history) >= 10:
            unique_transports = len(set(self.transport_history[-10:]))
            if unique_transports == 1:
                risk += self.detection_thresholds["behavioral_signatures"]["always_same_transport"]
                reasons.append("Never changes transport (+0.25)")
            else:
                risk -= 0.05 * unique_transports  # Bonus for variety
                reasons.append(f"Transport variety bonus (-{0.05*unique_transports:.2f})")

        # --- Time-based Analysis ---
        if 2 <= time_of_day <= 6:  # 2-6 AM
            risk += 0.2
            reasons.append("Late night beacon (+0.20)")
        elif 9 <= time_of_day <= 17:  # Business hours
            risk += 0.1
            reasons.append("Business hours (+0.10)")
        else:
            risk -= 0.05
            reasons.append("Off-hours bonus (-0.05)")

        # --- Data Volume Analysis ---
        # (Simulated: would be real beacon size in production)
        risk = max(0.0, min(1.0, risk))  # Clamp to [0, 1]

        return risk, " | ".join(reasons)

    def _categorize_interval(self, interval: int) -> str:
        """Categorize beacon interval by suspicion level"""
        thresholds = self.detection_thresholds["beacon_frequency"]
        for category, (low, high) in thresholds.items():
            if low <= interval < high:
                return category
        return "low"

    def simulate_detection(self, risk_score: float) -> Tuple[bool, str]:
        """
        Probabilistically determine if this beacon is detected

        Returns:
            (detected: bool, detection_method: str)
        """
        if random.random() < risk_score:
            methods = [
                "Statistical anomaly detection",
                "Behavioral fingerprint match",
                "C2 signature detection",
                "Network ML classifier",
                "Manual IR investigation",
            ]
            method = random.choice(methods)
            return True, method
        return False, "Not detected"

    def record_beacon(self, interval: int, transport: str):
        """Record beacon for pattern analysis"""
        self.beacon_history.append(interval)
        self.transport_history.append(transport)

        # Keep history window reasonable
        if len(self.beacon_history) > 100:
            self.beacon_history.pop(0)
        if len(self.transport_history) > 100:
            self.transport_history.pop(0)

    def get_evasion_metrics(self) -> Dict:
        """Get current evasion score"""
        if not self.beacon_history:
            return {"entropy": 0.0, "transport_diversity": 0.0, "timing_variance": 0.0}

        # Calculate entropy of intervals
        from collections import Counter
        interval_counts = Counter(self.beacon_history[-30:])
        total = len(self.beacon_history[-30:])
        entropy = -sum((c/total) * (c/total)**0.5 for c in interval_counts.values() if c > 0)

        # Transport diversity
        transport_diversity = len(set(self.transport_history[-30:])) / 3.0

        # Timing variance
        if len(self.beacon_history) > 1:
            intervals = self.beacon_history[-30:]
            mean = sum(intervals) / len(intervals)
            variance = sum((x - mean)**2 for x in intervals) / len(intervals)
            std_dev = variance ** 0.5
            timing_variance = min(1.0, std_dev / mean) if mean > 0 else 0.0
        else:
            timing_variance = 0.0

        return {
            "entropy": entropy,
            "transport_diversity": transport_diversity,
            "timing_variance": timing_variance,
            "overall_evasion": (entropy + transport_diversity + timing_variance) / 3.0,
        }


def demonstrate_evasion():
    """Show how different strategies affect detection"""
    print("\n" + "="*70)
    print("DETECTION EVASION DEMONSTRATION")
    print("="*70 + "\n")

    sim = DetectionSimulator()

    strategies = [
        {
            "name": "No Evasion (Detected Immediately)",
            "intervals": [30, 30, 30, 30, 30, 30, 30, 30],
            "transports": ["vpn", "vpn", "vpn", "vpn", "vpn", "vpn", "vpn", "vpn"],
        },
        {
            "name": "Basic Jitter (Random ±20%)",
            "intervals": [24, 31, 29, 35, 26, 32, 28, 34],
            "transports": ["vpn", "vpn", "vpn", "vpn", "vpn", "vpn", "vpn", "vpn"],
        },
        {
            "name": "Transport Switching",
            "intervals": [30, 30, 30, 30, 30, 30, 30, 30],
            "transports": ["vpn", "https", "dns", "https", "dns", "vpn", "https", "dns"],
        },
        {
            "name": "Advanced Evasion (Adaptive)",
            "intervals": [45, 120, 60, 30, 90, 50, 110, 35],
            "transports": ["dns", "https", "dns", "vpn", "dns", "https", "vpn", "dns"],
        },
    ]

    for strategy in strategies:
        sim = DetectionSimulator()
        print(f"\n{strategy['name']}:")
        print("-" * 70)

        detected_count = 0

        for i, (interval, transport) in enumerate(zip(strategy["intervals"], strategy["transports"])):
            risk, reason = sim.calculate_detection_risk(interval, transport, jitter=0.2)
            detected, method = sim.simulate_detection(risk)
            sim.record_beacon(interval, transport)

            if detected:
                detected_count += 1

            status = "🔴 DETECTED" if detected else "🟢 CLEAR"
            print(f"  [{i+1}] {status} | Risk: {risk:.2%} | {method}")

        metrics = sim.get_evasion_metrics()
        print(f"\n  Detection Rate: {detected_count}/{len(strategy['intervals'])} ({detected_count*100//len(strategy['intervals'])}%)")
        print(f"  Evasion Score: {metrics['overall_evasion']:.2%}")


if __name__ == "__main__":
    demonstrate_evasion()
