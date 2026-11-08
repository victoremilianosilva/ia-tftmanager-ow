# Canonical Overwolf live-state contract

## Ownership boundary

The Electron/Overwolf repository is the sole GEP interpreter. It merges GEP updates,
reconciles the baseline, converts GEP feature paths into the canonical state and
publishes it. Python and C++ validate/consume the canonical contract; they must not
contain mappings for `me.me.gold`, `board.board_pieces`, `cell_N`, or other GEP paths.

## Transport schema v2

Envelope: `schemaVersion=2`, `gameId`, `revision`, `capturedAt`, `complete`,
and `state`. The state declares `contract="tft.live-state"` and
`contractVersion=1`.

Known values use `{known:boolean,value:T|null}`. Unknown values must be null.
Known empty slots use `{known:true,unit:null}`; unknown slot groups use
`{known:false,slots:[]}`. This prevents a missing GEP field from being mistaken
for an empty board or a zero-valued economy field.

## Canonical fields

- Stage and round are numeric.
- Eight player positions are indexed 0–7; names/account identifiers are omitted.
- `localPlayerIndex` comes from the roster's local-player flag.
- Roster HP/alive/XP are exposed only when reported.
- Local player receives gold, level, XP, board (28 slots), bench (9 slots), shop
  (5 slots), and item-component stacks with counts.
- Unit items remain attached to units.
- Carousel item IDs are advisory and not treated as inventory.
- Opponent boards, bench, shop, gold, and levels remain unknown because the
  collected source does not provide trustworthy current values for them.
- Historical match-stat boards are never substituted for current opponent boards.

## Runtime guarantees

1. Python validates schema, shape, privacy, finite values and revision ordering.
2. C++ DTO mirrors the canonical fields and validates values; it does not map GEP.
3. Unknown fields remain unknown. The runtime must not invent opponents or
   fabricate missing observation inputs.
4. No `reset()`, `step()`, or test-only setter is used to import a live match.
5. Recommendations remain read-only and are never sent as TFT game actions.

## Version compatibility

Schema v1 carried raw GEP `info` and is intentionally rejected. Overwolf must emit
schema v2 canonical `state`; a producer/consumer version mismatch fails closed.
