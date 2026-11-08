"""Read-only deterministic inference using the repository's existing PPO policy."""
from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

import torch

from engine_contracts import ActionType
from policy import TFTPolicy


ACTION_DIMS = (9, 5, 9, 28, 28, 28)
MASK_SHAPES = {
    "type_mask": (9,),
    "shop_mask": (5,),
    "bench_source_mask": (9,),
    "board_target_mask": (28,),
    "board_source_mask": (28,),
    "board_move_target_masks": (28, 28),
    "bench_move_source_mask": (9,),
    "bench_to_board_target_masks": (9, 28),
    "item_inventory_mask": (9,),
    "item_component_first_mask": (10,),
    "item_component_pair_masks": (10, 10),
    "item_target_mask": (37,),
}


class InferenceContractError(ValueError):
    """Raised when observation, masks, or checkpoint do not match PPO contracts."""


class PPOInferenceRunner:
    """Loads an existing checkpoint and recommends one legal policy action.

    This class never calls the native engine's reset/step methods and never
    executes the returned action. The caller supplies one player's observation
    and the matching native action masks.
    """

    def __init__(self, policy: TFTPolicy, *, device: str = "cpu") -> None:
        self.device = torch.device(device)
        self.policy = policy.to(self.device)
        self.policy.eval()
        self.obs_dim = int(policy.obs_dim)

    @classmethod
    def from_checkpoint(
        cls,
        checkpoint_path: str | Path,
        *,
        obs_dim: int,
        device: str = "cpu",
    ) -> "PPOInferenceRunner":
        if type(obs_dim) is not int or obs_dim <= 0:
            raise InferenceContractError("obs_dim must be a positive integer")
        path = Path(checkpoint_path).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"PPO checkpoint not found: {path}")

        # weights_only=True avoids loading arbitrary Python objects from a
        # checkpoint. The repository's trainer stores a plain policy state_dict.
        try:
            checkpoint = torch.load(path, map_location="cpu", weights_only=True)
        except TypeError as exc:
            raise RuntimeError(
                "This runtime requires a PyTorch version supporting "
                "torch.load(weights_only=True)."
            ) from exc

        if not isinstance(checkpoint, Mapping):
            raise InferenceContractError("checkpoint root must be a mapping")
        state_dict = checkpoint.get("policy")
        if not isinstance(state_dict, Mapping):
            raise InferenceContractError(
                "checkpoint must contain the trainer's 'policy' state_dict"
            )

        policy = TFTPolicy(obs_dim=obs_dim, action_dims=list(ACTION_DIMS))
        try:
            policy.load_compatible_state_dict(state_dict)
        except (RuntimeError, KeyError, ValueError) as exc:
            raise InferenceContractError(
                f"checkpoint is incompatible with the current PPO policy: {exc}"
            ) from exc
        return cls(policy, device=device)

    def recommend(
        self,
        observation: Any,
        action_masks: Mapping[str, Any],
    ) -> dict[str, Any]:
        obs = torch.as_tensor(observation, dtype=torch.float32, device=self.device)
        if obs.ndim == 1:
            obs = obs.unsqueeze(0)
        if tuple(obs.shape) != (1, self.obs_dim):
            raise InferenceContractError(
                f"observation shape must be (1, {self.obs_dim}); got {tuple(obs.shape)}"
            )
        if not torch.isfinite(obs).all():
            raise InferenceContractError("observation contains NaN or infinity")

        masks = self._prepare_masks(action_masks)
        with torch.inference_mode():
            actions, _logprob, entropy, value = self.policy.get_action_and_value(
                obs, masks=masks, deterministic=True
            )

        if tuple(actions.shape) != (1, len(ACTION_DIMS)):
            raise InferenceContractError(
                f"policy returned unexpected action shape: {tuple(actions.shape)}"
            )
        action = [int(value) for value in actions[0].detach().cpu().tolist()]
        try:
            action_type = ActionType(action[0])
        except ValueError as exc:
            raise InferenceContractError(
                f"policy returned unknown action type: {action[0]}"
            ) from exc

        return {
            "status": "ready",
            "action_type": action_type.name.lower(),
            "action_type_id": int(action_type),
            "action_values": action,
            "value_estimate": float(value[0].detach().cpu().item()),
            "policy_entropy": float(entropy[0].detach().cpu().item()),
            "executed": False,
        }

    def _prepare_masks(self, action_masks: Mapping[str, Any]) -> dict[str, torch.Tensor]:
        if not isinstance(action_masks, Mapping):
            raise InferenceContractError("action_masks must be a mapping")
        missing = set(MASK_SHAPES).difference(action_masks.keys())
        if missing:
            raise InferenceContractError(
                "native action masks are missing: " + ", ".join(sorted(missing))
            )

        prepared: dict[str, torch.Tensor] = {}
        for name, expected_shape in MASK_SHAPES.items():
            tensor = torch.as_tensor(action_masks[name], device=self.device)
            if tensor.ndim == len(expected_shape):
                tensor = tensor.unsqueeze(0)
            expected_batched_shape = (1, *expected_shape)
            if tuple(tensor.shape) != expected_batched_shape:
                raise InferenceContractError(
                    f"{name} shape must be {expected_batched_shape}; got {tuple(tensor.shape)}"
                )
            if tensor.dtype != torch.bool:
                if tensor.is_floating_point() and not torch.isfinite(tensor).all():
                    raise InferenceContractError(f"{name} contains NaN or infinity")
                if not torch.all((tensor == 0) | (tensor == 1)):
                    raise InferenceContractError(f"{name} must contain only 0/1 values")
                tensor = tensor.bool()
            prepared[name] = tensor

        if not bool(prepared["type_mask"].any()):
            raise InferenceContractError("type_mask has no legal action")
        return prepared
