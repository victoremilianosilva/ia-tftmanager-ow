#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tft::live {

inline constexpr std::size_t kLiveBoardSlots = 28;
inline constexpr std::size_t kLiveBenchSlots = 9;
inline constexpr std::size_t kLiveShopSlots = 5;
inline constexpr std::size_t kLivePlayers = 8;

// A known empty slot is different from a slot that GEP did not report.
template <typename T>
struct Observed {
    bool known{false};
    T value{};

    static Observed unknown() { return {}; }
    static Observed of(T input) {
        Observed result;
        result.known = true;
        result.value = std::move(input);
        return result;
    }
};

struct UnitState {
    std::string champion_api_name;
    std::int32_t star{1};
    std::optional<std::string> instance_id;
    std::vector<std::string> item_ids;
};

using UnitSlot = Observed<std::optional<UnitState>>;

struct PlayerState {
    std::int32_t player_id{-1};
    Observed<float> hp;
    Observed<std::int32_t> gold;
    Observed<std::int32_t> level;
    Observed<std::int32_t> xp;
    Observed<bool> alive;
    std::array<UnitSlot, kLiveBoardSlots> board{};
    std::array<UnitSlot, kLiveBenchSlots> bench{};
    std::array<Observed<std::optional<std::string>>, kLiveShopSlots> shop{};
    Observed<std::vector<std::string>> item_components;
    Observed<std::vector<std::string>> item_inventory;
};

struct MatchState {
    std::uint32_t schema_version{1};
    std::uint64_t game_id{0};
    std::uint64_t revision{0};
    bool complete{false};
    Observed<std::uint32_t> stage;
    Observed<std::uint32_t> round;
    std::array<std::optional<PlayerState>, kLivePlayers> players{};
};

struct ValidationReport {
    bool valid{false};
    std::vector<std::string> errors;
    std::vector<std::string> unavailable_features;
};

// Validates the canonical DTO only. It does not mutate TFTEngine, advance a
// match, synthesize missing GEP fields, or claim that a GEP payload is mapped.
ValidationReport validate_match_state(const MatchState& state);

}  // namespace tft::live
