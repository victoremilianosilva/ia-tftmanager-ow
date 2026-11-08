# Native Engine live-import audit

## Existing public surface

The current `TFTEngine` API exposes:
- `reset()`: initializes a simulated eight-player lobby.
- `step()`: executes one action batch and advances the simulated lobby.
- `get_observations()`: read-only observation generation.
- `get_action_masks()`: read-only mask generation.
- `get_audit_snapshot()` and `get_public_state()`: read-only diagnostics.

The pybind11 class `TFTLobby` exposes those simulation/read APIs. It does not
expose a production method for importing a live GEP snapshot.

## Internal state that a production importer must reconcile

`TFTEngine::Impl::PlayerEnv` owns per-player:
- `board[28]` and `bench[9]` unit instances;
- `PlayerEconomy` and `PlayerInfo`;
- `ShopEngine` offers and lock state;
- `ItemEngine` inventory, components, equipment and pending anvils;
- strategy/composition state;
- action masks and anti-loop history.

The lobby also owns shared state:
- champion catalog, unit pool and unit-instance factory;
- item catalog and encoded recipe observations;
- round/stage and player-liveness state;
- shared combat, economy and action-mask services.

An importer that only fills board/bench arrays would still leave shop, item,
pool, strategy and action-mask data inconsistent with the imported match.

## API gaps discovered

- `ShopEngine::set_offer_for_testing` is a test hook, not a production import API.
- `TFTEngine::set_*_for_testing` methods must not be used by the live runtime.
- `get_observations()` and `get_action_masks()` are read-only, but depend on
  the complete internal state being coherent.
- `step()` changes the match and may run economy, rewards, anti-loop, shop and
  round transitions. It is prohibited in the live recommendation path.
- The native lobby models eight players and a shared champion pool. The
  GEP-to-native adapter must define what is known for all relevant players and
  must not invent opponent or pool state.

## Required design for the next native change

Introduce a typed, validated canonical live-state DTO independent of JSON/GEP
field names. Import it through a dedicated production API that:
1. validates player, round, board/bench/shop slot and catalog identifiers;
2. reconciles owned unit instances, equipment, loose items, economy and shop;
3. explicitly represents unknown/unobserved fields instead of filling defaults;
4. invalidates/rebuilds derived strategy, composition, board-evaluation and masks;
5. never advances round, applies rewards, spends currency, draws from the pool,
   or executes a policy action;
6. reports which observation features are unavailable when source coverage is
   insufficient.

Before implementing field converters, commit a sanitized real GEP snapshot
fixture and document exact source paths, types, coordinate conventions, and
null/removal semantics. The existing collector's TypeScript tests are useful
for envelope behavior but are not a substitute for a real game capture.

## DTO foundation added in the integration branch

cpp/include/lobby/LiveState.hpp now defines a canonical, GEP-independent DTO:
- explicit known/unknown values, so an unreported field is not confused with zero;
- separate known-empty and unknown board/bench/shop slots;
- fixed native dimensions (8 players, 28 board slots, 9 bench slots, 5 shop slots);
- optional economy, item and round fields;
- a validator for ranges, duplicate player IDs, unit stars and empty identifiers.

cpp/src/lobby/LiveState.cpp implements validation only. It deliberately does not mutate TFTEngine. A native CTest target covers the DTO validation contract, and the source is included in the tft_engine extension build.

## Remaining hard gate

This is the typed DTO foundation, not yet the production GEP importer. The live collector currently guarantees feature buckets (me, match_info, roster, store, board, bench, carousel, etc.), but the inspected source does not establish the exact leaf-field names, coordinate conventions, unit/item identifiers, or null/removal semantics needed to populate every native input. The collector also redacts some data. The next safe step is to validate a sanitized, real info baseline and record its exact field paths before wiring the DTO into TFTEngine::Impl.

Until that mapping exists, the native observations and masks must not be presented as live-state output. The existing simulation APIs remain unchanged.
