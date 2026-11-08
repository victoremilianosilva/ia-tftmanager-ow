#include "lobby/LiveState.hpp"

#include <cmath>
#include <set>
#include <utility>

namespace tft::live {
namespace {

template <typename T, typename Predicate>
void validate_observed(const Observed<T>& field, const char* name,
                       Predicate predicate, std::vector<std::string>& errors) {
    if (field.known && !predicate(field.value)) {
        errors.emplace_back(std::string(name) + " has an invalid value");
    }
}

void validate_slots(const auto& slots, const char* name,
                    std::vector<std::string>& errors) {
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto& slot = slots[index];
        if (!slot.known || !slot.value.has_value()) continue;
        const auto& unit = *slot.value;
        if (unit.champion_api_name.empty()) {
            errors.emplace_back(std::string(name) + "[" + std::to_string(index) +
                                "] has no champion API name");
        }
        if (unit.star < 1 || unit.star > 3) {
            errors.emplace_back(std::string(name) + "[" + std::to_string(index) +
                                "] has an invalid star level");
        }
        for (const auto& item : unit.item_ids) {
            if (item.empty()) {
                errors.emplace_back(std::string(name) + "[" + std::to_string(index) +
                                    "] contains an empty item id");
            }
        }
    }
}

}  // namespace

ValidationReport validate_match_state(const MatchState& state) {
    ValidationReport report;
    if (state.schema_version != 1) report.errors.emplace_back("unsupported DTO schema version");
    if (state.game_id == 0) report.errors.emplace_back("game_id must be positive");
    if (!state.complete) report.errors.emplace_back("baseline is incomplete");

    validate_observed(state.stage, "stage", [](std::uint32_t v) { return v > 0; }, report.errors);
    validate_observed(state.round, "round", [](std::uint32_t v) { return v > 0; }, report.errors);

    std::set<std::int32_t> player_ids;
    for (std::size_t index = 0; index < state.players.size(); ++index) {
        if (!state.players[index]) {
            report.unavailable_features.emplace_back("player[" + std::to_string(index) + "]");
            continue;
        }
        const auto& player = *state.players[index];
        if (player.player_id < 0 || player.player_id >= static_cast<std::int32_t>(kLivePlayers)) {
            report.errors.emplace_back("player_id is outside the lobby range");
        } else if (!player_ids.insert(player.player_id).second) {
            report.errors.emplace_back("duplicate player_id");
        }

        validate_observed(player.hp, "player.hp", [](float v) {
            return std::isfinite(v) && v >= 0.0f && v <= 100.0f;
        }, report.errors);
        validate_observed(player.gold, "player.gold", [](std::int32_t v) { return v >= 0; }, report.errors);
        validate_observed(player.level, "player.level", [](std::int32_t v) { return v >= 1; }, report.errors);
        validate_observed(player.xp, "player.xp", [](std::int32_t v) { return v >= 0; }, report.errors);

        validate_slots(player.board, "board", report.errors);
        validate_slots(player.bench, "bench", report.errors);
        for (std::size_t slot = 0; slot < player.shop.size(); ++slot) {
            const auto& offer = player.shop[slot];
            if (offer.known && offer.value && offer.value->empty()) {
                report.errors.emplace_back("shop[" + std::to_string(slot) + "] has an empty champion id");
            }
        }
        if (!player.item_components.known) report.unavailable_features.emplace_back("item_components");
        if (!player.item_inventory.known) report.unavailable_features.emplace_back("item_inventory");
    }

    report.valid = report.errors.empty();
    return report;
}

}  // namespace tft::live
