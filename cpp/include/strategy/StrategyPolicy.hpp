#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace tft::strategy {

enum class EconomicStrategy : std::int32_t {
    Econ = 0,
    Stabilize = 1,
    Fast8 = 2,
    Slowroll = 3,
    AllIn = 4,
    WinStreak = 5,
    LossStreak = 6 // Reserved legacy slot; no longer selected.
};

struct EconomicStrategyContext {
    int gold{0};
    double hp{100.0};
    int level{1};
    int win_streak{0};
    int loss_streak{0};
    double board_power{0.0};
    bool composition_requests_fast8{false};
    bool composition_requests_three_star{false};
    int stage{1};
};

struct EconomicStrategyDecision {
    EconomicStrategy selected{EconomicStrategy::Econ};
    std::array<double, 7> scores{};
    double confidence{0.5};
    const char* reason{"Economia neutra"};
};

inline EconomicStrategyDecision select_economic_strategy(
    const EconomicStrategyContext& s) noexcept {
    constexpr double kIneligible = -1000.0;
    EconomicStrategyDecision out;
    out.scores.fill(kIneligible);

    // Emergency spending overrides greed and long-term plans.
    if (s.hp <= 25.0) {
        out.scores[4] = 100.0 + (25.0 - s.hp);
        out.reason = "HP crítico: gastar para sobreviver";
    }

    if (s.hp <= 38.0 || (s.board_power < 35.0 && s.hp < 65.0)) {
        out.scores[1] = 65.0 + std::max(0.0, 38.0 - s.hp) * 0.25
            + std::max(0.0, 25.0 - s.board_power) * 0.15;
        if (s.hp <= 25.0) out.scores[1] = kIneligible;
        out.reason = s.hp <= 38.0
            ? "STABILIZE: pressão de vida"
            : "STABILIZE: tabuleiro fraco sob pressão";
    }

    // Gradual late-game correction: a healthy player may still need to
    // stabilize when holding substantial surplus gold and a weak board.
    // The score is intentionally close to ECON and never overrides a
    // composition-specific FAST8/SLOWROLL plan or emergency ALL_IN.
    if (s.stage >= 4 && s.gold >= 70 && s.board_power < 35.0 &&
        s.hp > 38.0 && s.hp > 15.0) {
        const double stage_pressure =
            std::clamp(static_cast<double>(s.stage - 4), 0.0, 3.0) * 1.5;
        const double surplus_pressure =
            std::clamp(static_cast<double>(s.gold - 70) / 30.0, 0.0, 2.0) * 1.5;
        const double board_pressure =
            std::clamp((35.0 - s.board_power) / 10.0, 0.0, 1.0) * 1.0;
        const double late_stabilize_score =
            42.0 + stage_pressure + surplus_pressure + board_pressure;
        out.scores[1] = std::max(out.scores[1], late_stabilize_score);
    }

    const bool has_comp_goal =
        s.composition_requests_fast8 || s.composition_requests_three_star;

    if (!has_comp_goal && s.win_streak >= 3 && s.hp >= 55.0 &&
        s.board_power >= 35.0) {
        out.scores[5] = 55.0 + std::min(s.win_streak, 6) * 1.5;
        out.reason = "WINSTREAK: preservar vantagem atual";
    }

    // Slowroll is a comp-driven plan, never a generic fallback just because
    // the player has gold and is at a low level.
    if (s.composition_requests_three_star && !s.composition_requests_fast8 &&
        s.level >= 5 && s.level <= 6 && s.stage >= 3 &&
        s.stage <= 4 && s.hp > 38.0) {
        out.scores[3] = 76.0 + std::min(s.gold, 50) * 0.04;
        out.reason = "SLOWROLL: composição tem alvo de 3 estrelas";
    }

    // Fast 8 is only enabled when the active composition plan calls for at
    // least eight units; gold alone must never select this strategy.
    if (s.composition_requests_fast8 && s.level >= 6 && s.level < 8 &&
        s.hp > 38.0 && s.board_power >= 35.0) {
        out.scores[2] = 76.0 + std::min(s.gold, 50) * 0.04
            + (s.level >= 7 ? 3.0 : 0.0)
            + (s.stage >= 5 ? 2.0 : 0.0);
        out.reason = "FAST8: composição planejada exige nível 8";
    }

    // LOSS_STREAK was retired: ECON already covers deliberate low-spend states.

    // ECON means protecting the interest threshold, not hoarding forever.
    // At/above 50 gold it remains eligible as a reserve policy, but reward
    // shaping must permit useful spending from the surplus.
    if (s.hp >= 40.0 && s.board_power >= 25.0 &&
        !s.composition_requests_fast8 &&
        !s.composition_requests_three_star &&
        s.hp > 15.0) {
        out.scores[0] = 35.0 + std::min(s.gold, 50) * 0.20;
        out.reason = s.gold < 50
            ? "ECON: acumular até o limite de juros de 50"
            : "ECON: manter reserva de 50 e gastar excedente útil";
    }

    // If no other mode is eligible, stabilize rather than silently selecting
    // ECON while the board is in trouble.
    if (out.scores[1] == kIneligible && out.scores[2] == kIneligible &&
        out.scores[3] == kIneligible && out.scores[4] == kIneligible &&
        out.scores[5] == kIneligible && out.scores[6] == kIneligible &&
        out.scores[0] == kIneligible) {
        out.scores[1] = 30.0;
        out.reason = "STABILIZE: modo de segurança";
    }

    int best = 0;
    for (int i = 1; i < 7; ++i)
        if (out.scores[static_cast<std::size_t>(i)] >
            out.scores[static_cast<std::size_t>(best)]) best = i;

    out.selected = static_cast<EconomicStrategy>(best);
    switch (out.selected) {
        case EconomicStrategy::Econ:
            out.reason = s.gold < 50
                ? "ECON: acumular até o limite de juros de 50"
                : "ECON: manter reserva de 50 e gastar excedente útil";
            break;
        case EconomicStrategy::Stabilize:
            out.reason = (s.stage >= 4 && s.gold >= 70 &&
                          s.board_power < 35.0 && s.hp > 38.0)
                ? "STABILIZE: excedente alto e tabuleiro fraco no late game"
                : (s.hp <= 38.0
                    ? "STABILIZE: pressão de vida"
                    : "STABILIZE: tabuleiro fraco sob pressão");
            break;
        case EconomicStrategy::Fast8:
            out.reason = "FAST8: composição planejada exige nível 8";
            break;
        case EconomicStrategy::Slowroll:
            out.reason = "SLOWROLL: composição tem alvo de 3 estrelas";
            break;
        case EconomicStrategy::AllIn:
            out.reason = "HP crítico: gastar para sobreviver";
            break;
        case EconomicStrategy::WinStreak:
            out.reason = "WINSTREAK: preservar vantagem atual";
            break;
        case EconomicStrategy::LossStreak:
            out.reason = "LOSS_STREAK: estratégia aposentada";
            break;
    }
    double second = kIneligible;
    for (int i = 0; i < 7; ++i)
        if (i != best) second = std::max(second, out.scores[static_cast<std::size_t>(i)]);
    out.confidence = std::clamp(
        0.5 + (out.scores[static_cast<std::size_t>(best)] - second) / 100.0,
        0.5, 0.99);
    return out;
}

} // namespace tft::strategy
