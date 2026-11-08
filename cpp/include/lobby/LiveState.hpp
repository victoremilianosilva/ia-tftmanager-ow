#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tft::live {
inline constexpr std::size_t kLiveBoardSlots = 28;
inline constexpr std::size_t kLiveBenchSlots = 9;
inline constexpr std::size_t kLiveShopSlots = 5;
inline constexpr std::size_t kLivePlayers = 8;

template <typename T>
struct Observed {
    bool known{false};
    T value{};
    static Observed unknown() { return {}; }
    static Observed of(T input) {
        Observed result; result.known = true; result.value = std::move(input); return result;
    }
};

struct UnitState {
    std::string champion_api_name;
    Observed<std::int32_t> star;
    std::vector<std::string> item_ids;
};
using UnitSlot = Observed<std::optional<UnitState>>;
using ShopSlot = Observed<std::optional<std::string>>;

struct ItemStack {
    std::string api_name;
    std::int32_t count{0};
};

struct PlayerState {
    std::int32_t player_id{-1}; // Canonical lobby index, never a summoner/account ID.
    bool local_player{false};
    Observed<float> hp;
    Observed<std::int32_t> gold;
    Observed<std::int32_t> level;
    Observed<std::int32_t> xp;
    Observed<bool> alive;
    Observed<std::array<UnitSlot, kLiveBoardSlots>> board;
    Observed<std::array<UnitSlot, kLiveBenchSlots>> bench;
    Observed<std::array<ShopSlot, kLiveShopSlots>> shop;
    Observed<std::vector<ItemStack>> item_components;
};

struct MatchState {
    std::uint32_t schema_version{2}; // transport envelope schemaVersion
    std::uint32_t contract_version{1}; // state.contractVersion
    std::uint64_t game_id{0};\n    std::string session_id;
    std::uint64_t revision{0};
    bool complete{false};
    Observed<std::string> match_id;
    Observed<std::uint32_t> stage;
    Observed<std::uint32_t> round;
    Observed<std::string> phase;
    Observed<std::int32_t> local_player_index;
    std::array<std::optional<PlayerState>, kLivePlayers> players{};
};

struct ValidationReport {
    bool valid{false};
    std::vector<std::string> errors;
    std::vector<std::string> unavailable_features;
};

// Validates the already-normalized Overwolf contract. No GEP paths are parsed here.
ValidationReport validate_match_state(const MatchState& state);
}  // namespace tft::live
