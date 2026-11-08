# Live inference foundation

This package consumes canonical snapshots emitted by the Electron/Overwolf app.
It does not interpret GEP fields or merge raw GEP deltas.

## Components

- `gep_snapshot.py`: validates schema-v2 `tft.live-state` snapshots, privacy,
  finite values, fixed slot counts and revision ordering.
- `policy_runner.py`: loads the existing PPO checkpoint and performs deterministic,
  mask-constrained inference. It never executes the returned action.
- `cpp/include/lobby/LiveState.hpp`: C++ DTO matching the canonical contract.

## Contract boundary

Overwolf owns all GEP conversion. The Python and C++ layers consume only
`snapshot.state`; neither contains GEP path mappings. Schema v1/raw `info` is
rejected. Unknown values remain explicitly unknown, and the seven opponent boards
are not fabricated from historical match statistics.

## Not yet complete

The canonical contract is now defined and validated, but the production adapter
that imports this DTO into the native Engine, builds observations/masks and exposes
them through the Python extension remains a separate gated stage. Do not use
`reset()`, `step()`, or test-only setters to simulate a live import.
