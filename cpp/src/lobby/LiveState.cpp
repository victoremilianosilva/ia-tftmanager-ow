#include "lobby/LiveState.hpp"
#include <cmath>
#include <set>
#include <utility>

namespace tft::live {
namespace {
template <typename T, typename Predicate>
void validate_observed(const Observed<T>& field, const char* name, Predicate predicate,
                       std::vector<std::string>& errors) {
    if (field.known && !predicate(field.value)) errors.emplace_back(std::string(name) + " has an invalid value");
}
template <typename Slots>
void validate_unit_slots(const Observed<Slots>& observed, const char* name,
                         std::vector<std::string>& errors) {
    if (!observed.known) return;
    for (std::size_t index = 0; index < observed.value.size(); ++index) {
        const auto& slot = observed.value[index];
        if (!slot.known || !slot.value.has_value()) continue;
        const auto& unit = *slot.value;
        if (unit.champion_api_name.empty()) errors.emplace_back(std::string(name) + " has an empty champion API name");
        validate_observed(unit.star, "unit.star", [](std::int32_t v) { return v >= 1 && v <= 3; }, errors);
        for (const auto& item : unit.item_ids) if (item.empty()) errors.emplace_back(std::string(name) + " has an empty item API name");
    }
}
} // namespace

ValidationReport validate_match_state(const MatchState& state) {
    ValidationReport report;
    if (state.schema_version != 2) report.errors.emplace_back("unsupported transport schema version");
    if (state.contract_version != 1) report.errors.emplace_back("unsupported canonical state contract version");
    if (state.game_id == 0) report.errors.emplace_back("game_id must be positive");
    if (!state.complete) report.errors.emplace_back("baseline is incomplete");
    validate_observed(state.match_id, "match_id", [](const std::string& v) { return !v.empty(); }, report.errors);
    validate_observed(state.stage, "stage", [](std::uint32_t v) { return v > 0; }, report.errors);
    validate_observed(state.round, "round", [](std::uint32_t v) { return v > 0; }, report.errors);
    validate_observed(state.phase, "phase", [](const std::string& v) { return !v.empty(); }, report.errors);
    validate_observed(state.local_player_index, "local_player_index", [](std::int32_t v) {
        return v >= 0 && v < static_cast<std::int32_t>(kLivePlayers);
    }, report.errors);
    std::set<std::int32_t> ids;
    for (std::size_t index = 0; index < state.players.size(); ++index) {
        if (!state.players[index]) {
            report.unavailable_features.emplace_back("player[" + std::to_string(index) + "]");
            continue;
        }
        const auto& player = *state.players[index];
        if (player.player_id != static_cast<std::int32_t>(index)) report.errors.emplace_back("player index does not match canonical array position");
        if (player.local_player && (!state.local_player_index.known ||
            state.local_player_index.value != player.player_id))
            report.errors.emplace_back("local player flag disagrees with local_player_index");
        if (!ids.insert(player.player_id).second) report.errors.emplace_back("duplicate player index");
        validate_observed(player.hp, "player.hp", [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 100.0f; }, report.errors);
        validate_observed(player.gold, "player.gold", [](std::int32_t v) { return v >= 0; }, report.errors);
        validate_observed(player.level, "player.level", [](std::int32_t v) { return v >= 1; }, report.errors);
        validate_observed(player.xp, "player.xp", [](std::int32_t v) { return v >= 0; }, report.errors);
        validate_unit_slots(player.board, "board", report.errors);
        validate_unit_slots(player.bench, "bench", report.errors);
        if (player.shop.known) {
            for (const auto& offer : player.shop.value) {
                if (offer.known && offer.value && offer.value->empty())
                    report.errors.emplace_back("shop contains an empty champion API name");
            }
        }
        if (player.item_components.known) {
            for (const auto& item : player.item_components.value) {
                if (item.api_name.empty() || item.count <= 0) report.errors.emplace_back("invalid component stack");
            }
        } else report.unavailable_features.emplace_back("item_components[" + std::to_string(index) + "]");
    }
    if (state.local_player_index.known) {
        const auto index = static_cast<std::size_t>(state.local_player_index.value);
        if (!state.players[index] || !state.players[index]->local_player)
            report.errors.emplace_back("local_player_index does not reference the local player");
    }
    report.valid = report.errors.empty();
    return report;
}
} // namespace tft::live
