# Live inference foundation

This package is the Overwolf-facing inference boundary. It is intentionally
separate from the training loop and does not control the TFT client.

## Current components

- `gep_snapshot.py`: validates the schema-v1 snapshot envelope, rejects
  sensitive identifiers, and gates stale/incomplete revisions.
- `policy_runner.py`: loads the Trainer's existing `checkpoint["policy"]`
  state dictionary and performs deterministic, mask-constrained inference.

## Minimal usage

The native Engine integration is not implemented yet. Once a validated native
adapter supplies the selected player's observation and the corresponding masks:

```python
from live_inference.policy_runner import PPOInferenceRunner

runner = PPOInferenceRunner.from_checkpoint(
    "checkpoints/tft_checkpoint.pt",
    obs_dim=native_engine.observation_dim(),
)

recommendation = runner.recommend(
    observation=player_observation,
    action_masks=player_action_masks,
)
```

The runner returns the six-component PPO action as recommendation data and sets
`executed=false`. It never invokes `TFTEngine.step()` and does not communicate
with the game client.

## Runtime requirements

- Python environment with the same PyTorch version family used for the model.
- Current repository root on `PYTHONPATH` so `policy.py` and
  `engine_contracts.py` resolve.
- A checkpoint produced by the repository Trainer and an observation dimension
  matching the native C++ build/catalog.
- Masks returned by the native Engine for the same player and same snapshot.

## Not implemented yet

- GEP feature-path mapping to canonical live state.
- Native C++ live-state import API.
- `.pyd` bindings for that API.
- Local IPC/WebSocket service and Overwolf startup/reconnect.
- UI-specific recommendation explanations.

Do not bypass these missing layers by calling `reset()`, `step()`, or
`*_for_testing` methods to fabricate a live match.
