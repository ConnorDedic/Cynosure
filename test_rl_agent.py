#!/usr/bin/env python3
"""
Test script for RL Beacon Agent

Simulates beacon requests and feedback to test the model.
Run this after starting rl_beacon_service.py.
"""

import requests
import json
import time
import random
from datetime import datetime

BASE_URL = "http://127.0.0.1:5555"

def test_get_action():
    """Test beacon action request"""
    print("\n[*] Testing /beacon/action endpoint...")

    payload = {
        "implant_id": f"agent-{random.randint(1000, 9999)}",
        "success_rate": random.uniform(0.5, 1.0),
        "uptime": random.uniform(0.5, 1.0),
        "seconds_since_beacon": random.choice([5, 10, 30, 60]),
        "transport": random.choice(["vpn", "https", "dns"])
    }

    try:
        response = requests.post(f"{BASE_URL}/beacon/action", json=payload)
        response.raise_for_status()
        data = response.json()

        print(f"  Request: {json.dumps(payload, indent=2)}")
        print(f"  Response: {json.dumps(data, indent=2)}")
        print("  ✓ Success")
        return data
    except Exception as e:
        print(f"  ✗ Error: {e}")
        return None

def test_feedback(action_data):
    """Test beacon feedback"""
    print("\n[*] Testing /beacon/feedback endpoint...")

    if not action_data:
        print("  Skipping (no action data)")
        return

    payload = {
        "implant_id": f"agent-{random.randint(1000, 9999)}",
        "success": random.random() > 0.1,  # 90% success rate
        "response_time": random.uniform(0.1, 2.0),
        "beacon_interval": action_data.get("beacon_interval", 30)
    }

    try:
        response = requests.post(f"{BASE_URL}/beacon/feedback", json=payload)
        response.raise_for_status()
        data = response.json()

        print(f"  Request: {json.dumps(payload, indent=2)}")
        print(f"  Response: {json.dumps(data, indent=2)}")
        print("  ✓ Success")
    except Exception as e:
        print(f"  ✗ Error: {e}")

def test_metrics():
    """Test metrics endpoint"""
    print("\n[*] Testing /model/metrics endpoint...")

    try:
        response = requests.get(f"{BASE_URL}/model/metrics")
        response.raise_for_status()
        data = response.json()

        print(f"  Response:")
        for key, value in data.items():
            print(f"    {key}: {value}")
        print("  ✓ Success")
    except Exception as e:
        print(f"  ✗ Error: {e}")

def test_train():
    """Test training step"""
    print("\n[*] Testing /model/train endpoint...")

    try:
        response = requests.post(f"{BASE_URL}/model/train")
        response.raise_for_status()
        data = response.json()

        print(f"  Loss: {data.get('loss', 'N/A')}")
        print("  ✓ Success")
    except Exception as e:
        print(f"  ✗ Error: {e}")

def run_simulation(num_beacons=50, num_training_steps=10):
    """Simulate beacon cycle with training"""
    print(f"\n[*] Running simulation with {num_beacons} beacons and {num_training_steps} training steps...")

    for i in range(num_beacons):
        # Get beacon action
        action = test_get_action()

        # Provide feedback
        if action:
            test_feedback(action)

        # Every N beacons, train
        if (i + 1) % (num_beacons // num_training_steps) == 0:
            test_train()

        time.sleep(0.1)  # Small delay between requests

    # Final metrics
    print("\n[*] Final Model Metrics:")
    test_metrics()

def main():
    print("="*60)
    print("RL Beacon Agent Test Suite")
    print("="*60)

    # Check connectivity
    try:
        requests.get(f"{BASE_URL}/model/metrics", timeout=1)
    except Exception as e:
        print(f"\n[!] Cannot connect to service at {BASE_URL}")
        print(f"[!] Error: {e}")
        print(f"\nMake sure to start the service first:")
        print(f"  python3 src/rl_beacon_service.py")
        return

    # Run tests
    print("\n" + "="*60)
    print("Individual Endpoint Tests")
    print("="*60)

    action = test_get_action()
    test_feedback(action)
    test_metrics()
    test_train()

    # Run simulation
    print("\n" + "="*60)
    print("Beacon Simulation")
    print("="*60)

    run_simulation(num_beacons=20, num_training_steps=5)

    print("\n" + "="*60)
    print("All tests completed!")
    print("="*60)

if __name__ == "__main__":
    main()
