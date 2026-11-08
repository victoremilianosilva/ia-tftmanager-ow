#include "lobby/LiveState.hpp"

#include <cassert>
#include <optional>

int main() {
    using namespace tft::live;

    MatchState state;
    state.game_id = 42;
    state.revision = 7;
    state.complete = true;
    state.stage = Observed<std::uint32_t>::of(2);
    state.round = Observed<std::uint32_t>::of(3);

    PlayerState player;
    player.player_id = 0;
    player.hp = Observed<float>::of(82.0f);
    player.gold = Observed<std::int32_t>::of(30);
    player.level = Observed<std::int32_t>::of(6);
    player.xp = Observed<std::int32_t>::of(12);
    player.alive = Observed<bool>::of(true);
    player.board[0] = UnitSlot::of(UnitState{"TFT18_Test", 2, std::nullopt, {}});
    player.bench[0] = UnitSlot::of(std::optional<UnitState>{}); // Known empty is not unknown.
    state.players[0] = player;

    auto report = validate_match_state(state);
    assert(report.valid);
    assert(!report.unavailable_features.empty());

    state.players[1] = player; // Duplicate player id.
    report = validate_match_state(state);
    assert(!report.valid);

    state.players[1]->player_id = 1;
    state.players[1]->board[0] = UnitSlot::of(UnitState{"TFT18_Test", 4, std::nullopt, {}});
    report = validate_match_state(state);
    assert(!report.valid);

    state.players[1].reset();
    state.players[0]->board[0] = UnitSlot::unknown();
    state.complete = false;
    report = validate_match_state(state);
    assert(!report.valid);

    return 0;
}
