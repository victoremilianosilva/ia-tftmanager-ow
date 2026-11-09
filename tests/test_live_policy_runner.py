"""Tests for checkpoint-backed, non-executing PPO inference."""
import tempfile
import unittest
from pathlib import Path

import torch

from live_inference.policy_runner import (
    ACTION_DIMS,
    InferenceContractError,
    MASK_SHAPES,
    PPOInferenceRunner,
)
from policy import TFTPolicy


OBS_DIM = 8000


def legal_pass_masks():
    masks = {}
    for name, shape in MASK_SHAPES.items():
        masks[name] = torch.zeros(shape, dtype=torch.bool)
    masks["type_mask"][6] = True
    return masks


class PPOInferenceRunnerTests(unittest.TestCase):
    def test_checkpoint_runner_returns_deterministic_pass_without_execution(self):
        policy = TFTPolicy(obs_dim=OBS_DIM, action_dims=list(ACTION_DIMS))
        with tempfile.TemporaryDirectory() as directory:
            checkpoint_path = Path(directory) / "checkpoint.pt"
            torch.save({"policy": policy.state_dict(), "update": 4}, checkpoint_path)
            runner = PPOInferenceRunner.from_checkpoint(
                checkpoint_path, obs_dim=OBS_DIM
            )
            observation = torch.zeros(OBS_DIM)
            masks = legal_pass_masks()

            first = runner.recommend(observation, masks)
            second = runner.recommend(observation, masks)

        self.assertEqual(first["status"], "ready")
        self.assertEqual(first["action_type"], "pass")
        self.assertEqual(first["action_type_id"], 6)
        self.assertEqual(first["action_values"][0], 6)
        self.assertFalse(first["executed"])
        self.assertEqual(first["action_values"], second["action_values"])

    def test_rejects_missing_native_mask(self):
        policy = TFTPolicy(obs_dim=OBS_DIM, action_dims=list(ACTION_DIMS))
        runner = PPOInferenceRunner(policy)
        masks = legal_pass_masks()
        del masks["type_mask"]
        with self.assertRaises(InferenceContractError):
            runner.recommend(torch.zeros(OBS_DIM), masks)

    def test_rejects_observation_dimension_mismatch(self):
        policy = TFTPolicy(obs_dim=OBS_DIM, action_dims=list(ACTION_DIMS))
        runner = PPOInferenceRunner(policy)
        with self.assertRaises(InferenceContractError):
            runner.recommend(torch.zeros(OBS_DIM - 1), legal_pass_masks())


if __name__ == "__main__":
    unittest.main()
