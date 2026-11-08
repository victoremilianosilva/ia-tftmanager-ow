# GEP snapshot contract and mapping status

## Transport envelope (schema version 1)

The Overwolf collector in `victoremilianosilva/tft-manager-ow` publishes a
`TftGepSnapshot` with these fields:

| Field | Meaning | Runtime rule |
|---|---|---|
| `schemaVersion` | Snapshot envelope version | Must be integer `1` |
| `gameId` | GEP game identifier | Positive integer; do not hardcode a guessed ID |
| `revision` | Monotonic revision within one game session | Reject older revisions; ignore duplicates |
| `capturedAt` | Collector timestamp | Valid ISO-8601 timestamp |
| `complete` | Whether baseline reconciliation completed | Inference is blocked until true |
| `info` | Merged strategic GEP feature tree | Object; field mapping remains schema-driven |

The collector merges partial `new-info-update` payloads before publishing snapshots.
The Python boundary must therefore consume the merged snapshot as-is. It must not
replace the state with a partial delta or independently guess merge semantics.

## Known collector behavior

The current Overwolf snapshot manager:
- Reconciles an initial `getInfo` baseline and buffers updates arriving during reconciliation.
- Publishes a new revision only when strategic state changes.
- Filters combat telemetry and account/credential identifiers.
- Redacts augment data because of the collector's display restriction.
- Marks the snapshot incomplete when the baseline cannot be trusted.

The Python boundary validates the envelope, rejects sensitive keys if they appear,
and gates inference on a complete baseline. It does not infer game semantics from
unknown feature names.

## Mapping status

No production GEP-to-Engine mapping is claimed yet. The actual feature tree and
native engine state must be compared using a sanitized, real snapshot fixture
before writing field converters.

| Strategic domain | GEP source path | Engine destination | Status |
|---|---|---|---|
| Player identity/slot | Not verified | Lobby player index | Pending fixture |
| Round/stage/phase | Not verified | Engine round/stage | Pending fixture |
| Health/economy/XP/level | Not verified | Player state | Pending fixture |
| Board units and positions | Not verified | Board slots | Pending fixture |
| Bench units | Not verified | Bench slots | Pending fixture |
| Shop offers | Not verified | Shop slots | Pending fixture |
| Items/components/anvil | Not verified | Item state | Pending fixture |
| Traits/composition | Derived or not directly exposed | Observation features | Pending parity audit |

## Safety and correctness invariants

1. Never call `reset()` or `step()` to import a live match; those are simulation APIs.
2. Never use `*_for_testing` setters as production import APIs.
3. Never invent missing values. Missing required observation inputs must produce
   an explicit unavailable result, not a fabricated observation.
4. Do not pass incomplete, stale, duplicate, malformed, or sensitive snapshots
   into policy inference.
5. Keep GEP ingestion read-only. Recommendations are not game actions.
6. Preserve the original PPO observation layout and checkpoint compatibility;
   verify with golden observations before enabling live recommendations.

## Current implementation

`live_inference/gep_snapshot.py` validates the versioned envelope, checks
sensitive-key redaction, enforces a configurable payload-size limit, and tracks
revision ordering/baseline readiness. It intentionally does not yet build a
native Engine state.
