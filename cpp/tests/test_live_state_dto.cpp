#include "lobby/LiveState.hpp"
#include <cassert>
#include <optional>

int main() {
    using namespace tft::live;
    MatchState state;
    state.game_id = 42; state.session_id = "session-a"; state.revision = 7; state.complete = true;
    state.match_id = Observed<std::string>::of("778899");
    state.stage = Observed<std::uint32_t>::of(2);
    state.round = Observed<std::uint32_t>::of(3);
    state.local_player_index = Observed<std::int32_t>::of(0);

    PlayerState player;
    player.player_id = 0; player.local_player = true;
    player.hp = Observed<float>::of(82.0f);
    player.gold = Observed<std::int32_t>::of(30);
    player.level = Observed<std::int32_t>::of(6);
    player.xp = Observed<std::int32_t>::of(12);
    player.alive = Observed<bool>::of(true);
    std::array<UnitSlot, kLiveBoardSlots> board{};
    board[0] = UnitSlot::of(UnitState{"TFT18_Test", Observed<std::int32_t>::of(2), {"DA_InfinityEdge"}});
    player.board = Observed<decltype(board)>::of(board);
    std::array<UnitSlot, kLiveBenchSlots> bench{};
    bench[0] = UnitSlot::of(std::optional<UnitState>{});
    player.bench = Observed<decltype(bench)>::of(bench);
    player.item_components = Observed<std::vector<ItemStack>>::of({{"DA_Component_BFSword", 2}});
    state.players[0] = player;

    auto report = validate_match_state(state);
    assert(report.valid);
    assert(!report.unavailable_features.empty());

    state.players[1] = player;
    report = validate_match_state(state);
    assert(!report.valid);

    state.players[1]->player_id = 1;
    std::array<UnitSlot, kLiveBoardSlots> invalid_board{};
    invalid_board[0] = UnitSlot::of(UnitState{"TFT18_Test", Observed<std::int32_t>::of(4), {}});
    state.players[1]->board = Observed<decltype(invalid_board)>::of(invalid_board);
    report = validate_match_state(state);
    assert(!report.valid);

    state.players[1].reset();
    state.players[0]->board = Observed<decltype(board)>::of(board);
    state.complete = false;
    report = validate_match_state(state);
    assert(!report.valid);
    return 0;
}
