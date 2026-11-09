# Live inference foundation

This package consumes canonical snapshots emitted by the Electron/Overwolf app.
It does not interpret GEP fields or merge raw GEP deltas.

## Components

- `gep_snapshot.py`: validates schema-v2 `tft.live-state` snapshots, privacy,
  finite values, fixed slot counts and revision ordering.
- `service.py`: loopback-only HTTP receiver. Run with
  `python -m live_inference.service --host 127.0.0.1 --port 8765`.
  It exposes `GET /health` and `POST /v1/snapshot`.
- `policy_runner.py`: loads the existing PPO checkpoint and performs deterministic,
  mask-constrained inference. It never executes the returned action.
- `cpp/include/lobby/LiveState.hpp`: C++ DTO matching the canonical contract.

## Local transport

The Overwolf main process posts canonical JSON directly to
`http://127.0.0.1:8765/v1/snapshot`. The receiver binds only to loopback,
rejects non-JSON, oversized, malformed, stale and duplicate snapshots, and
keeps the latest accepted baseline in memory. It does not persist raw account
identifiers or expose a network-facing listener.

Start the receiver before launching TFT/Overwolf. Check
`http://127.0.0.1:8765/health` for transport status. A `snapshotReady` result
means the canonical baseline and local-player mapping passed validation; it
does **not** mean native observations, action masks or policy inference are
available yet.

## Contract boundary

Overwolf owns all GEP conversion. The Python and C++ layers consume only
`snapshot.state`; neither contains GEP path mappings. Schema v1/raw `info` is
rejected. Unknown values remain explicitly unknown, and the seven opponent boards
are not fabricated from historical match statistics.

## Native import gate

The production adapter that imports the DTO into the native Engine, builds
observations/masks and exposes them through the Python extension remains gated
on a sanitized real GEP baseline and verified source-field/null/removal semantics.
Do not use `reset()`, `step()`, or test-only setters to simulate a live import.
