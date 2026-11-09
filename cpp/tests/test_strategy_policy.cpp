#include "strategy/StrategyPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void require(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using tft::strategy::EconomicStrategy;
    using tft::strategy::EconomicStrategyContext;
    using tft::strategy::select_economic_strategy;

    EconomicStrategyContext s;
    s.gold = 42; s.hp = 85; s.level = 6; s.board_power = 45;
    auto d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Econ,
            "healthy state without a comp goal should choose ECON");

    s.gold = 50;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Econ &&
            std::string(d.reason).find("excedente") != std::string::npos,
            "ECON at 50 should explicitly permit spending surplus");

    // Late stabilization is a soft correction, not a blanket late-game rule.
    s.stage = 4; s.gold = 69; s.board_power = 30; s.hp = 85;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Econ,
            "late stage alone must not displace ECON without meaningful surplus");

    s.gold = 100; s.stage = 5;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Stabilize &&
            std::string(d.reason).find("late game") != std::string::npos,
            "late high-surplus weak board should gently prioritize stabilization");

    s.board_power = 45; s.gold = 50;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Econ,
            "healthy strong board should retain ECON even in late stages");

    s.composition_requests_fast8 = true;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Fast8,
            "FAST8 must be selected when the composition requires level 8");
    s.gold = 0;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Fast8,
            "FAST8 must remain the active plan while funds fluctuate");

    s.composition_requests_fast8 = false;
    s.composition_requests_three_star = true;
    s.gold = 10; s.level = 6; s.stage = 3;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Slowroll,
            "SLOWROLL must be composition-driven by a 3-star target inside its stage window");
    s.stage = 2;
    d = select_economic_strategy(s);
    require(d.selected != EconomicStrategy::Slowroll,
            "SLOWROLL must not activate before stage 3");
    s.stage = 3;
    s.composition_requests_three_star = false;
    d = select_economic_strategy(s);
    require(d.selected != EconomicStrategy::Slowroll,
            "low level and gold alone must not select SLOWROLL");

    s.hp = 30; s.gold = 50; s.board_power = 45;
    s.composition_requests_fast8 = true;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Stabilize,
            "low health should stabilize before a non-emergency FAST8 plan");
    s.composition_requests_fast8 = false;

    s.hp = 12;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::AllIn,
            "critical HP must override economic greed");

    s.hp = 30; s.board_power = 20; s.composition_requests_fast8 = false;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Stabilize,
            "weak board under health pressure should stabilize");

    s.hp = 80; s.board_power = 38; s.gold = 40;
    s.composition_requests_three_star = false;
    s.loss_streak = 4; s.win_streak = 0;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::Econ,
            "LOSS_STREAK is retired; ECON handles low-spend states");

    // FAST8 must not start from a weak board even with a composition goal.
    s.composition_requests_fast8 = true;
    s.board_power = 30; s.gold = 60; s.level = 6; s.stage = 4;
    d = select_economic_strategy(s);
    require(d.selected != EconomicStrategy::Fast8,
            "FAST8 requires a strong board before leveling toward eight");
    s.composition_requests_fast8 = false;

    s.loss_streak = 0; s.win_streak = 4; s.board_power = 45;
    d = select_economic_strategy(s);
    require(d.selected == EconomicStrategy::WinStreak,
            "strong win streak should be preserved");

    std::cout << "[OK] strategy policy\n";
    return 0;
}
