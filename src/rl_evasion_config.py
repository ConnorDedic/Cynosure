"""
RL Beacon Evasion Configuration

Tunable parameters for optimizing C2 detection evasion.
The RL model will use these to balance connectivity vs. stealth.

Adjust these values to control:
- How aggressive the evasion is (higher = more evasive, lower = more connectivity)
- Beacon timing randomization
- Transport switching patterns
- Activity-based adaptation
"""

# =========================================================================
# EVASION PARAMETERS (Tunable)
# =========================================================================

class EvasionConfig:
    """Configuration for C2 detection evasion optimization"""

    # Stealth vs Connectivity Trade-off (0.0 - 1.0)
    # 0.0 = pure connectivity (no evasion)
    # 1.0 = maximum stealth (highest detection evasion)
    STEALTH_WEIGHT = 0.6

    # Beacon Timing Jitter (seconds)
    # Random variance added to beacon interval to avoid predictable patterns
    # Higher = more jitter, harder to detect from IDS
    JITTER_RANGE = 10  # ±10 seconds

    # Transport Switching Aggressiveness (0.0 - 1.0)
    # Probability of switching transports (VPN → HTTPS → DNS)
    # Higher = more frequent transport switches
    TRANSPORT_SWITCH_PROB = 0.3

    # Time-based Adaptation
    # Avoid beaconing during high-activity hours when anomalies are noticed
    AVOID_PEAK_HOURS = True
    PEAK_HOUR_START = 9   # 9 AM
    PEAK_HOUR_END = 17    # 5 PM

    # Payload Size Variation
    # Add random padding to beacon payload to avoid pattern matching
    # Higher = more variation
    PAYLOAD_VARIANCE = 100  # bytes

    # Detection Likelihood Model
    # Adjust these if you discover what signatures IDS looks for
    SIGNATURE_DETECTION_WEIGHT = 0.3   # How much we penalize known signatures
    BEHAVIOR_DETECTION_WEIGHT = 0.5    # How much we penalize suspicious behavior
    VOLUME_DETECTION_WEIGHT = 0.2      # How much we penalize high beacon volume

    # Adaptive Response
    # If beacon fails repeatedly, increase evasion aggressively
    FAILURE_ESCALATION_THRESHOLD = 3   # failures before escalating
    ESCALATION_MULTIPLIER = 1.5        # multiply stealth weight by this

    # Connection Quality Thresholds
    # Don't sacrifice connectivity too much if conditions are good
    MIN_SUCCESS_RATE_FOR_STEALTH = 0.7  # if success < 70%, reduce stealth
    TARGET_SUCCESS_RATE = 0.95          # try to maintain 95% success

    @classmethod
    def to_dict(cls):
        """Export config as dict for TUI display"""
        return {
            "STEALTH_WEIGHT": cls.STEALTH_WEIGHT,
            "JITTER_RANGE": cls.JITTER_RANGE,
            "TRANSPORT_SWITCH_PROB": cls.TRANSPORT_SWITCH_PROB,
            "AVOID_PEAK_HOURS": cls.AVOID_PEAK_HOURS,
            "PAYLOAD_VARIANCE": cls.PAYLOAD_VARIANCE,
            "SIGNATURE_DETECTION_WEIGHT": cls.SIGNATURE_DETECTION_WEIGHT,
            "BEHAVIOR_DETECTION_WEIGHT": cls.BEHAVIOR_DETECTION_WEIGHT,
            "VOLUME_DETECTION_WEIGHT": cls.VOLUME_DETECTION_WEIGHT,
        }

    @classmethod
    def update(cls, key: str, value):
        """Update a config parameter at runtime"""
        if hasattr(cls, key):
            setattr(cls, key, value)
            return True
        return False


# =========================================================================
# PRESET EVASION PROFILES
# =========================================================================

EVASION_PROFILES = {
    "connectivity": {
        "STEALTH_WEIGHT": 0.2,
        "JITTER_RANGE": 2,
        "TRANSPORT_SWITCH_PROB": 0.1,
        "description": "Prioritize reliability over stealth"
    },
    "balanced": {
        "STEALTH_WEIGHT": 0.6,
        "JITTER_RANGE": 10,
        "TRANSPORT_SWITCH_PROB": 0.3,
        "description": "Balance stealth and connectivity"
    },
    "aggressive": {
        "STEALTH_WEIGHT": 0.9,
        "JITTER_RANGE": 30,
        "TRANSPORT_SWITCH_PROB": 0.7,
        "description": "Maximize detection evasion"
    },
}


def apply_profile(profile_name: str):
    """Apply a preset evasion profile"""
    if profile_name not in EVASION_PROFILES:
        return False

    profile = EVASION_PROFILES[profile_name]
    for key, value in profile.items():
        if key != "description":
            EvasionConfig.update(key, value)
    return True
