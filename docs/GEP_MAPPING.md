# Canonical Overwolf live-state contract

## Ownership boundary

The Electron/Overwolf repository is the sole GEP interpreter. It merges GEP updates,
reconciles the baseline, converts GEP feature paths into the canonical state and
publishes it. Python and C++ validate/consume the canonical contract; they must not
contain mappings for `me.me.gold`, `board.board_pieces`, `cell_N`, or other GEP paths.

## Transport schema v2

Envelope: `schemaVersion=2`, `gameId`, `sessionId`, `revision`, `capturedAt`, `complete`,
and `state`. The state declares `contract="tft.live-state"` and
`contractVersion=1`.

Known values use `{known:boolean,value:T|null}`. Unknown values must be null.
Known empty slots use `{known:true,unit:null}`; unknown slot groups use
`{known:false,slots:[]}`. This prevents a missing GEP field from being mistaken
for an empty board or a zero-valued economy field.

## Canonical fields

- Match ID is a canonical string when available and scopes match revisions even while the GEP game ID remains constant.
- Stage and round are numeric.
- Eight player positions are indexed 0–7; names/account identifiers are omitted from the canonical state.
- `localPlayerIndex` comes from the roster's local-player flag.
- Roster HP/alive/XP are exposed only when reported.
- Local player receives gold, level, XP, board (28 slots), bench (9 slots), shop
  (5 slots), and item-component stacks with counts.
- Unit items remain attached to units.
- Carousel item IDs are advisory and not treated as inventory.
- Opponent boards, bench, shop, gold, and levels remain unknown because the
  collected source does not provide trustworthy current values for them.
- Historical match-stat boards are never substituted for current opponent boards.

## Runtime guarantees and current implementation boundary

1. Python validates schema, shape, finite values, sensitive-key absence and revision ordering; it does not map GEP paths.
2. `cpp/include/lobby/LiveState.hpp` is a typed DTO and `LiveState.cpp` validates it. The DTO currently does not mutate/import into `TFTEngine`.
3. `live_inference/service.py` is a loopback snapshot receiver/sequence validator only. Its health and ingest responses report `inferenceAvailable=false`.
4. `PPOInferenceRunner` consumes an already-built native observation and matching native action masks. It does not convert this canonical DTO into Engine state or invoke `reset()`/`step()`.
5. The Overwolf capture repository currently writes local audit JSONL files and does not send snapshots to Python. A transport connection is not established by the presence of the receiver.
6. Unknown fields remain unknown. The runtime must not invent opponents or fabricate missing observation inputs.
7. Recommendations remain read-only and are never sent as TFT game actions.

## Required live-import acceptance

Before calling any output live, a production importer and transport must:
- reconcile board, bench, shop, unit instances, equipment, loose items, economy, round, and all derived Engine state needed by observations/masks;
- preserve per-field provenance and distinguish unknown from known zero/empty;
- validate catalog identities, board coordinates, stale/out-of-order revisions, match boundaries and idempotency;
- avoid simulation `reset()`, `step()`, test-only setters, rewards, round advancement and action execution;
- prove the real sanitized GEP fixture → transport → DTO → native import → observations/masks → PPO inference → read-only recommendation chain in deterministic replay.

## Version compatibility

Schema v1 carried raw GEP `info` and is intentionally rejected. Overwolf must emit
schema v2 canonical `state`; a producer/consumer version mismatch fails closed.
