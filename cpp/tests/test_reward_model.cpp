#include "reward/RewardModel.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <stdexcept>

namespace {
void require_close(double actual, double expected, const std::string& name) {
    if (std::abs(actual - expected) > 1e-9) {
        std::cerr << "[FAIL] " << name << ": actual=" << actual
                  << ", expected=" << expected << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace tft::reward_model;

    // Golden values mirror the formula-level Python tests and let us replay
    // the same sequence of snapshots on both implementations.
    require_close(combat(true, 3, 0, 0, 2),
                  2.0 + std::tanh(0.3) + 0.1,
                  "combat win reward");
    require_close(combat(false, 0, 3, 2, 0),
                  -2.0 - 0.1 - std::tanh(0.2),
                  "combat loss reward");

    // Sequence: empty board -> buy/place a 2-cost 1-star unit ->
    // improve synergy/positioning on the next action.
    const double first_action =
        0.05 + power(4.0, 0.0) + synergy(0.5, 0.0) +
        positioning(0.3, 0.0) + board(1, 28);
    const double second_action =
        power(4.0, 4.0) + synergy(1.0, 0.5) +
        positioning(0.8, 0.3) + economy(20, 100.0) +
        board(1, 28);

    require_close(first_action,
                  0.05 + std::tanh(0.4) + std::tanh(0.5) +
                      0.5 * std::tanh(0.3) + (1.0 / 28.0) * 0.15,
                  "first deterministic action reward");
    require_close(second_action,
                  std::tanh(0.5) + 0.5 * std::tanh(0.5) +
                      0.10 + (1.0 / 28.0) * 0.15,
                  "second deterministic action reward");

    require_close(placement(1, 8), 1.0, "first-place terminal reward");
    require_close(placement(8, 8), -1.0, "last-place terminal reward");
    require_close(placement(4, 8), 1.0 / 7.0, "middle-place terminal reward");

    require_close(xp_level_transition_value(60, 56, 80.0, 4, 7, 8),
                  0.052875, "XP level reward is slightly reduced to favor useful unit purchases");
    require_close(xp_level_transition_value(22, 18, 80.0, 2, 4, 5),
                  0.018, "XP transition accounts for lost interest breakpoint");
    require_close(xp_level_transition_value(40, 36, 80.0, 2, 5, 5),
                  0.0, "XP without level transition has no progress reward");
    require_close(xp_progress_reward(54, 50, 6, 6, 0, 4, 80.0, 4, 2, false),
                  0.0, "safe surplus XP uses the shared fallback without double-counting");
    require_close(xp_progress_reward(54, 50, 6, 6, 0, 4, 80.0, 4, 2, true),
                  0.0, "a useful shop pair keeps priority over incremental XP");
    require_close(xp_progress_reward(54, 50, 6, 6, 0, 4, 80.0, 4, 3, false),
                  0.0, "SLOWROLL at level 6 does not receive further incremental XP progress reward");
    require_close(xp_progress_reward(40, 36, 5, 5, 0, 4, 30.0, 4, 3, false),
                  0.035, "SLOWROLL may receive urgent XP progress for its controlled transition to level 6");
    require_close(xp_progress_reward(40, 36, 6, 6, 0, 4, 30.0, 4, 1, false),
                  0.035, "STABILIZE can value urgent XP progress below the interest reserve");
    require_close(xp_progress_reward(40, 36, 6, 6, 0, 4, 30.0, 4, 2, false),
                  0.035, "FAST8 can value urgent progress toward level eight");
    require_close(xp_progress_reward(40, 36, 6, 6, 0, 4, 30.0, 4, 2, true),
                  0.0, "urgent XP does not outrank a useful shop offer");
    require_close(xp_progress_reward(40, 36, 6, 6, 0, 4, 80.0, 4, 2, false),
                  0.0, "safe XP progress below the interest cap is not encouraged");
    require_close(xp_strategy_reward_scale(1), 0.95,
                  "STABILIZE positive XP shaping is reduced by five percent");
    require_close(xp_strategy_reward_scale(3), 0.45,
                  "SLOWROLL XP shaping is limited to a controlled transition toward level 6");
    require_close(xp_strategy_reward_scale(2), 1.0,
                  "FAST8 XP shaping remains unchanged");
    require_close(strategy_action_reward(3, 2, 12, 8, 80.0, 2,
                                         false, false, false, true),
                  -0.04,
                  "non-leveling early XP during healthy SLOWROLL is discouraged");
    require_close(strategy_action_reward(3, 2, 12, 8, 30.0, 2,
                                         false, false, false, true),
                  0.0,
                  "urgent low-health SLOWROLL XP remains available");
    require_close(strategy_action_reward(3, 2, 12, 8, 80.0, 2,
                                         false, false, true, true),
                  0.0,
                  "SLOWROLL XP that secures a level-up is not penalized");
    require_close(strategy_action_reward(0, 2, 17, 13, 80.0, 2,
                                         false, false, false, true),
                  -0.04,
                  "early ECON discourages non-leveling XP below the 50-gold reserve even with a comp target");
    require_close(strategy_action_reward(0, 2, 17, 13, 30.0, 5,
                                         false, false, false, true),
                  0.0,
                  "urgent low-health ECON XP remains available");
    require_close(strategy_action_reward(0, 2, 17, 13, 80.0, 5,
                                         false, false, true, true),
                  0.0,
                  "ECON XP that secures a level-up is not penalized");
    require_close(strategy_action_reward(0, 2, 60, 56, 80.0, 4,
                                         false, false, true, false, 40.0),
                  0.095,
                  "strong-board ECON converts surplus above 50 into a level-up");
    require_close(strategy_action_reward(1, 1, 60, 58, 60.0, 4,
                                         false, true, false, false, 20.0),
                  0.04,
                  "STABILIZE may reroll when a pressured board finds a better shop");
    require_close(strategy_action_reward(4, 1, 20, 18, 20.0, 4,
                                         false, true, false, false),
                  0.04,
                  "ALL_IN may reroll when a critical-HP shop improves");
    require_close(strategy_action_reward(3, 2, 40, 36, 80.0, 3,
                                         false, false, true, true),
                  0.05,
                  "SLOWROLL may take a controlled level-up while preserving 30 gold");
    const double neutral_xp = xp_level_transition_value(60, 56, 80.0, 4, 7, 8, 0.0, 0);
    const double stabilize_xp = xp_level_transition_value(60, 56, 80.0, 4, 7, 8, 0.0, 1);
    const double slowroll_xp = xp_level_transition_value(60, 56, 80.0, 4, 7, 8, 0.0, 3);
    if (!(stabilize_xp < neutral_xp && slowroll_xp < stabilize_xp)) {
        throw std::runtime_error("strategy-specific XP shaping must reduce only positive incentives");
    }
    if (!(xp_level_transition_value(60, 56, 80.0, 4, 7, 8, 20.0) >
          xp_level_transition_value(60, 56, 80.0, 4, 7, 8, 0.0))) {
        throw std::runtime_error("a level-up that unlocks deployable board power must be valued above a level-only transition");
    }

    require_close(level(4, 1), 1.5, "level reward cap");
    require_close(economy(50, 20), 0.25 - 0.05, "economy reward");
    require_close(streak(5, 4), 0.15 + 0.08, "streak reward");
    require_close(blocked_move(), -0.35, "AntiLoop blocked move penalty");
    require_close(board_capacity_completion(), 0.25, "board capacity completion bonus");
    require_close(clamp_total(9.0), 5.0, "upper reward clamp");
    require_close(clamp_total(-9.0), -5.0, "lower reward clamp");
    require_close(gold_potential(0), 0.0, "zero gold potential");
    require_close(gold_potential(9), 0.018, "liquid gold potential below breakpoint");
    require_close(gold_potential(10), 0.10, "interest breakpoint potential");
    require_close(gold_potential(100), 0.435, "surplus above 50 has a slightly stronger holding penalty");
    require_close(gold_change_reward(10, 9), -0.082, "spending below interest breakpoint");
    require_close(gold_change_reward(9, 10), 0.082, "saving to interest breakpoint");
    require_close(core_displacement_penalty(true, 0.0), -0.30,
                  "replacing a core unit without clear improvement is penalized");
    require_close(core_displacement_penalty(true, 2.0), 0.0,
                  "clear composition improvement can replace a core unit");
    require_close(core_displacement_penalty(false, -1.0), 0.0,
                  "non-core displacement does not receive the core-specific penalty");
    require_close(movement_no_progress_penalty(0.0, 0.0, 0.0), -0.10,
                  "no-progress movement baseline penalty");
    if (!(movement_no_progress_penalty(0.0, -2.0, -1.0) <
          movement_no_progress_penalty(0.0, 0.0, 0.0))) {
        throw std::runtime_error("movement degradation must increase penalty");
    }
    if (!(movement_no_progress_penalty(-4.0, 0.0, 0.0) <
          movement_no_progress_penalty(0.0, 0.0, 0.0))) {
        throw std::runtime_error("composition degradation must increase penalty");
    }
    require_close(purchase_quality(5.0, false), 0.22 * std::tanh(1.0),
                  "composition-aware purchase reward");
    require_close(purchase_quality(0.0, true), 0.20,
                  "purchase completing an upgrade");
    if (!(purchase_pair_quality(0.0) > 0.30)) {
        throw std::runtime_error("pair completion should have a clear but bounded acquisition signal");
    }
    if (!(unit_purchase_timing_reward(4, 54, 5, 1, 0.8, 80.0, false, true) > 0.0)) {
        throw std::runtime_error("a high-value pair may spend surplus while retaining 40 gold");
    }
    if (!(unit_purchase_timing_reward(4, 54, 5, 1, 0.8, 80.0, false, false) > 0.0))
        throw std::runtime_error("useful purchases may spend below 50 in the mid/late game");
    require_close(surplus_xp_spending_reward(54, 50, 4, 0, false), 0.0,
                  "small surplus should not trigger routine XP spending");
    require_close(surplus_xp_spending_reward(60, 56, 4, 0, false), 0.0215,
                  "XP fallback starts modestly when there is meaningful surplus");
    require_close(surplus_xp_spending_reward(103, 99, 5, 0, false), 0.02795,
                  "large surplus increases XP value gradually without dominating purchases");
    require_close(surplus_xp_spending_reward(50, 46, 5, 0, false), 0.0,
                  "XP surplus fallback must preserve the 50-gold interest floor");
    require_close(surplus_xp_spending_reward(54, 50, 4, 0, true), 0.0,
                  "useful shop offers take priority over surplus XP");
    require_close(surplus_xp_spending_reward(54, 50, 4, 3, false), 0.0,
                  "SLOWROLL never receives the surplus XP fallback");
    require_close(surplus_xp_spending_reward(103, 99, 5, 3, false), 0.0,
                  "even a large surplus must not turn SLOWROLL into a leveling strategy");
    const double late_surplus_xp =
        surplus_xp_spending_reward(103, 99, 5, 1, false);
    const double all_in_surplus_xp =
        surplus_xp_spending_reward(103, 45, 5, 4, false);
    if (!(all_in_surplus_xp > 0.0))
        throw std::runtime_error("ALL_IN may spend surplus XP below the 50-gold interest floor");
    const double late_surplus_pass =
        pass_opportunity_penalty(103, 80.0, 5, false);
    const double low_hp_surplus_pass =
        pass_opportunity_penalty(103, 20.0, 5, false);
    if (!(low_hp_surplus_pass < late_surplus_pass))
        throw std::runtime_error("holding surplus gold under low HP receives additional pass pressure");
    if (!(late_surplus_xp > late_surplus_pass)) {
        throw std::runtime_error(
            "late-game surplus XP progress must be preferred over passive hoarding");
    }
    if (!(unit_purchase_timing_reward(5, 50, 2, 1, 0.7, 80.0, false, false) > 0.0)) {
        throw std::runtime_error("useful unit purchase with capped gold should be rewarded in mid-game");
    }
    if (!(unit_purchase_timing_reward(4, 51, 2, 1, 0.8, 80.0, false, false) > 0.0))
        throw std::runtime_error("stage 4 useful purchases may spend through the interest breakpoint");
    require_close(unit_purchase_timing_reward(4, 52, 2, 1, 0.8, 80.0, false, false),
                  0.0853065,
                  "small surplus purchase nudge stays bounded while preserving 50 gold");
    if (!(unit_purchase_timing_reward(5, 25, 2, 1, 0.7, 80.0, false, false) > 0.0))
        throw std::runtime_error("stage 5 should convert idle gold into useful board strength");
    if (!(unit_purchase_timing_reward(6, 25, 2, 1, 0.7, 25.0, false, false) > 0.0)) {
        throw std::runtime_error("useful unit purchase should be rewarded under low-health pressure");
    }
    const double buy_50 = unit_purchase_timing_reward(4, 50, 2, 2, 0.8, 80.0, false, true);
    const double buy_100 = unit_purchase_timing_reward(4, 100, 2, 2, 0.8, 80.0, false, true);
    if (!(buy_100 > buy_50))
        throw std::runtime_error("useful unit acquisition should gain value from surplus gold");
    require_close(unit_purchase_timing_reward(4, 100, 2, 7, 0.8, 80.0, false, true),
                  0.0, "surplus must not fill an overcrowded bench");
    require_close(unit_purchase_timing_reward(5, 50, 1, 7, 0.7, 30.0, false, false),
                  0.0, "do not reward purchases when the bench is nearly full");
    require_close(early_purchase_reward(2, 10, 1, 2, 0.5, false, false),
                  0.105625, "early flexible unit acquisition bonus");
    require_close(early_purchase_reward(2, 10, 1, 2, 0.0, false, true),
                  0.232, "early pair purchase bonus remains stronger than a single flexible unit");
    if (!(early_purchase_reward(2, 10, 1, 2, 0.10, false, false) > 0.0))
        throw std::runtime_error("modestly compatible early units should retain option value");
    require_close(early_purchase_reward(2, 10, 1, 2, 0.0, false, false),
                  0.06625, "cheap early flex acquisition has bounded option value");
    require_close(early_purchase_reward(2, 10, 4, 2, 0.0, false, false),
                  0.0, "low-fit expensive early acquisition is not rewarded");
    require_close(early_purchase_reward(2, 10, 1, 8, 0.0, false, false),
                  0.0, "early low-fit purchase rejected with crowded bench");
    require_close(early_purchase_reward(3, 10, 1, 2, 0.5, false, false),
                  0.0, "early purchase bonus does not persist into later stages");
    require_close(reversed_purchase_penalty(), -0.30,
                  "targeted reversed-purchase penalty");
    // Regressions from the Viewer match: Teemo/Caitlyn were bought and sold
    // on the next action without being deployed or completing an upgrade.
    // Their low strategic-fit values must trigger the same reversal rule.
    if (!should_penalize_reversed_purchase(0.08, false, false, false, false))
        throw std::runtime_error("Teemo-like low-fit immediate reversal should be penalized");
    if (!should_penalize_reversed_purchase(0.14, false, false, false, false))
        throw std::runtime_error("Caitlyn-like low-fit immediate reversal should be penalized");
    if (!should_penalize_reversed_purchase(0.20, false, false, false, false))
        throw std::runtime_error("threshold-fit immediate reversal should be penalized");
    if (should_penalize_reversed_purchase(0.80, false, false, false, false))
        throw std::runtime_error("a high-fit experimental purchase must not be penalized solely because it was sold");
    if (should_penalize_reversed_purchase(0.08, false, false, false, false, true))
        throw std::runtime_error("a purchase that built a real pair must retain its strategic value");
    if (should_penalize_reversed_purchase(0.08, false, false, false, false, false, true))
        throw std::runtime_error("an experiment replaced by a better owned alternative must not be penalized");
    if (should_penalize_reversed_purchase(0.08, false, false, false, false, false, false, true))
        throw std::runtime_error("a sale that unlocks an essential economic threshold must not be penalized");
    if (!should_penalize_reversed_purchase(0.08, false, false, false, false))
        throw std::runtime_error("a low-fit immediate reversal without justification must be penalized");
    static_assert(kRecentPurchaseHistoryActions == 8,
                  "recent purchase association window should cover intervening actions");
    if (should_penalize_reversed_purchase(0.08, true, false, false, false))
        throw std::runtime_error("completed upgrades must remain exempt from reversal penalty");
    if (should_penalize_reversed_purchase(0.08, false, true, false, false))
        throw std::runtime_error("deployed purchases must remain exempt from reversal penalty");
    if (should_penalize_reversed_purchase(0.08, false, false, true, false))
        throw std::runtime_error("a sale required to free a full bench must remain exempt");
    if (should_penalize_reversed_purchase(0.08, false, false, false, true))
        throw std::runtime_error("explicitly verified composition improvement must remain exempt");
    const double teemo_sequence = 0.06 + (-0.02) +
        reversed_sequence_adjustment(0.06, -0.02);
    if (teemo_sequence > -0.05 + 1e-9)
        throw std::runtime_error("positive buy followed by a weakly negative sale must not retain positive aggregate reward");
    const double caitlyn_sequence = -0.09 + (-0.11) +
        reversed_sequence_adjustment(-0.09, -0.11);
    if (caitlyn_sequence > -0.05 + 1e-9)
        throw std::runtime_error("already-negative buy/sell sequence must remain negative");
    if (should_penalize_reversed_purchase(0.0, true, false, false, false))
        throw std::runtime_error("completed upgrade must be exempt from reversal penalty");
    if (should_penalize_reversed_purchase(0.0, false, true, false, false))
        throw std::runtime_error("deployed unit must be exempt from reversal penalty");
    if (should_penalize_reversed_purchase(0.0, false, false, true, false))
        throw std::runtime_error("full bench must be exempt from reversal penalty");
    if (should_penalize_reversed_purchase(0.0, false, false, false, true))
        throw std::runtime_error("composition transition must be exempt from reversal penalty");
    const double reversal_adjustment = reversed_sequence_adjustment(0.02, 0.20);
    if (!(0.02 + 0.20 + reversal_adjustment <= -0.05 + 1e-9)) {
        throw std::runtime_error("reversed purchase sequence must not retain positive aggregate reward");
    }
    require_close(reversed_sequence_adjustment(-0.10, -0.20), 0.0,
                  "already-negative reversal sequence needs no extra adjustment");
    if (!is_pair_purchase(1, 1) || is_pair_purchase(0, 1) ||
        is_pair_purchase(1, 2)) {
        throw std::runtime_error("pair recognition must not depend on composition fit and must match the same star");
    }
    require_close(purchase_pair_quality(0.0), 0.36,
                  "pair-building baseline bonus");
    if (!(purchase_pair_quality(10.0) > purchase_pair_quality(0.0))) {
        throw std::runtime_error("pair reward must increase with composition fit");
    }
    if (!(purchase_pair_quality(10.0) > purchase_quality(0.0, true))) {
        throw std::runtime_error("forming a pair must be valued above a generic upgrade signal");
    }
    if (!(spending_timing_reward(50, 100.0, 2, 1.0, false, false) > 0.0)) {
        throw std::runtime_error("a capped gold balance with a useful early purchase should have a small spending signal");
    }
    if (!(spending_timing_reward(50, 20.0, 5, 0.0, true, false) > 0.0)) {
        throw std::runtime_error("completing an upgrade with high gold must be rewarded under pressure");
    }
    if (!(spending_timing_reward(50, 20.0, 5, 0.9, false, false) > 0.0)) {
        throw std::runtime_error("high-fit purchase with excess gold must be rewarded under pressure");
    }
    if (!(spending_timing_reward(20, 10.0, 7, 1.0, true, false) > 0.0)) {
        throw std::runtime_error("an urgent upgrade should still be rewarded with low liquidity");
    }
    require_close(buy_xp_timing_reward(125, 80.0, 5, false), 0.0,
                  "non-leveling XP must not be a generic excess-gold sink");
    require_close(buy_xp_timing_reward(50, 35.0, 5, false), 0.0,
                  "non-leveling XP must not compete with useful unit purchases");
    if (!(buy_xp_timing_reward(50, 35.0, 5, true) >
          buy_xp_timing_reward(20, 80.0, 2, true))) {
        throw std::runtime_error("urgent level-up should be rewarded above safe early leveling");
    }
    require_close(buy_xp_timing_reward(25, 80.0, 5, false), 0.0,
                  "do not reward XP spending with low gold and safe health");
    require_close(buy_xp_timing_reward(40, 30.0, 5, false), 0.0,
                  "non-leveling XP below the interest cap must not receive a reward");
    if (!(buy_xp_timing_reward(25, 30.0, 5, true) > 0.0)) {
        throw std::runtime_error("an urgent level-up may be rewarded even below the interest cap");
    }
    require_close(buy_xp_timing_reward(10, 100.0, 2, false), 0.0,
                  "do not encourage early XP spending when economy and health are safe");
    if (!(buy_xp_timing_reward(50, 20.0, 5, true) <= 0.10)) {
        throw std::runtime_error("XP spending reward must remain capped to avoid repeated XP spam");
    }
    if (!(pass_opportunity_penalty(50, 100.0, 2, true) < 0.0)) {
        throw std::runtime_error("passing with a useful shop offer should be discouraged");
    }
    // Above the 50-gold interest cap, a poor shop and unavailable XP do
    // not make PASS neutral: the policy must still choose a fallback action.
    if (!(pass_opportunity_penalty(200, 100.0, 6, false) < 0.0)) {
        throw std::runtime_error(
            "PASS above 50 gold must be penalized even without purchase or XP opportunity");
    }
    const double pass_50 = pass_opportunity_penalty(50, 100.0, 5, false, true);
    const double pass_75 = pass_opportunity_penalty(75, 100.0, 5, false, true);
    const double pass_125 = pass_opportunity_penalty(125, 100.0, 5, false, true);
    const double pass_200 = pass_opportunity_penalty(200, 100.0, 5, false, true);
    if (!(pass_125 < -0.02))
        throw std::runtime_error("large reserves with useful XP progress must have an opportunity cost");
    if (!(pass_50 > pass_75 && pass_75 > pass_125 && pass_125 > pass_200))
        throw std::runtime_error("PASS penalty must grow gradually with surplus when XP is useful");
    if (!(pass_opportunity_penalty(125, 80.0, 5, true, true) <
          pass_opportunity_penalty(125, 80.0, 5, false, true)))
        throw std::runtime_error("PASS with a useful unit offer must be penalized more");
    if (!(pass_opportunity_penalty(50, 20.0, 6, true) < 0.0))
        throw std::runtime_error("passing with a useful offer, high gold and low HP must be discouraged");
    if (!(pass_opportunity_penalty(50, 20.0, 6, false, true) < 0.0))
        throw std::runtime_error("late-game XP opportunity and low HP must discourage passive saving");
    if (!(pass_opportunity_penalty(125, 80.0, 6, false, true) <
          pass_opportunity_penalty(125, 80.0, 3, false, true))) {
        throw std::runtime_error("late-game surplus opportunity cost must exceed opening-stage pressure");
    }
    const double stage4_xp_without_shop =
        phase_xp_progress_reward(54, 50, 6, 6, 0, 4, 80.0, 4, 1, false);
    const double stage4_xp_with_shop =
        phase_xp_progress_reward(54, 50, 6, 6, 0, 4, 80.0, 4, 1, true);
    if (!(stage4_xp_without_shop > 0.02 &&
          stage4_xp_without_shop > stage4_xp_with_shop))
        throw std::runtime_error("stage 4 should give XP a clearer fallback signal only when no useful shop purchase exists");
    require_close(phase_xp_progress_reward(103, 99, 7, 7, 0, 4, 80.0, 5, 0, false),
                  0.0468,
                  "stage 5 ECON receives phase-aware XP progress toward its level target");
    if (!(phase_xp_progress_reward(108, 104, 8, 8, 0, 4, 80.0, 5, 1, false) > 0.035))
        throw std::runtime_error("level 8 with surplus gold must receive useful partial-XP signal toward level 9");
    if (!(phase_xp_progress_reward(97, 93, 7, 7, 0, 4, 80.0, 5, 1, false) > 0.035))
        throw std::runtime_error("level 7 with surplus gold must receive useful partial-XP signal toward level 9");
    if (!(phase_xp_progress_reward(103, 99, 7, 7, 0, 4, 80.0, 5, 0, true) > 0.014))
        throw std::runtime_error("phase XP keeps a reduced signal when a useful shop offer exists");
    if (!(phase_xp_progress_reward(103, 99, 6, 6, 0, 4, 80.0, 4, 2, true) > 0.015))
        throw std::runtime_error("FAST8 should retain a modest XP signal when a useful shop offer exists");
    require_close(phase_xp_progress_reward(103, 99, 6, 6, 0, 4, 80.0, 4, 3, false),
                  0.0, "SLOWROLL keeps its three-star economy");
    if (!(phase_xp_progress_reward(40, 36, 6, 6, 0, 4, 80.0, 5, 0, false) > 0.02))
        throw std::runtime_error("late-game XP may spend below 50 when behind target and above the soft reserve");
    require_close(phase_xp_progress_reward(40, 36, 7, 7, 0, 4, 25.0, 5, 1, false),
                  0.0, "urgent partial XP uses the dedicated urgent reward, not duplicate phase shaping");
    if (!(xp_progress_reward(40, 36, 7, 7, 0, 4, 25.0, 5, 1, false) > 0.0))
        throw std::runtime_error("critical HP may justify XP progress below the interest floor");
    require_close(phase_xp_progress_reward(54, 50, 4, 4, 0, 4, 80.0, 2, 0, false),
                  0.0, "stage 2 must prioritize board development over XP");
    if (!(phase_xp_progress_reward(54, 50, 5, 5, 0, 4, 80.0, 3, 0, false) > 0.015))
        throw std::runtime_error("stage 3 begins gradual progression toward level 7");
    require_close(economy_phase_target_level(1), 3.0, "stage 1 target level");
    require_close(economy_phase_target_level(2), 5.0, "stage 2 target level");
    require_close(economy_phase_target_level(3), 7.0, "stage 3 target level");
    require_close(economy_phase_target_level(4), 8.0, "stage 4 target level");
    require_close(economy_phase_target_level(5), 9.0, "stage 5 target level");
    require_close(economy_phase_target_level(4, 2), 8.0, "FAST8 target level");
    require_close(economy_phase_target_level(4, 3), 6.0, "SLOWROLL target level");
    require_close(economy_phase_minimum_reserve(3, 80.0, 0), 20.0,
                  "stage 3 releases liquidity gradually");
    require_close(economy_phase_minimum_reserve(4, 80.0, 0), 10.0,
                  "stage 4 keeps only a modest soft reserve");
    require_close(economy_phase_minimum_reserve(5, 80.0, 0), 5.0,
                  "stage 5 releases nearly all reserved liquidity");
    require_close(economy_phase_minimum_reserve(6, 80.0, 0), 0.0,
                  "late game does not protect idle gold");
    require_close(economy_phase_minimum_reserve(4, 80.0, 3), 25.0,
                  "SLOWROLL protects its roll economy");
    require_close(economy_phase_minimum_reserve(4, 80.0, 4), 0.0,
                  "ALL_IN removes the soft reserve");
    require_close(economy_phase_target_level(6), 9.0, "stage 6 target level");
    if (!phase_xp_opportunity_available(40, 6, 80.0, 4, 2))
        throw std::runtime_error("FAST8 should expose XP as a useful spend below its target");
    if (phase_xp_opportunity_available(40, 8, 80.0, 4, 2))
        throw std::runtime_error("XP is not a useful phase opportunity after reaching the target");
    if (phase_xp_opportunity_available(13, 6, 80.0, 4, 0))
        throw std::runtime_error("XP opportunity must preserve the phase reserve");
    if (phase_xp_opportunity_available(40, 6, 80.0, 4, 3))
        throw std::runtime_error("SLOWROLL must not be pressured to spend on XP");
    require_close(item_crafting_bonus(-0.02), 0.0,
                  "poor item craft should not receive a crafting bonus");
    if (!(item_crafting_bonus(0.15) > 0.0 && item_crafting_bonus(0.15) <= 0.08)) {
        throw std::runtime_error("useful crafted items should receive a small bounded bonus");
    }
    require_close(tactical_reroll_quality_scale(0, 90.0, 39, 4), 0.0,
                  "healthy tactical reroll below 40 gold remains discouraged");
    require_close(tactical_reroll_quality_scale(0, 90.0, 40, 3), 0.0,
                  "pre-stage-4 tactical reroll below 50 gold remains discouraged");
    require_close(tactical_reroll_quality_scale(0, 90.0, 40, 4), 0.0,
                  "non-SLOWROLL stage-4 reroll receives no positive quality signal");
    require_close(tactical_reroll_quality_scale(0, 90.0, 50, 2), 0.0,
                  "non-SLOWROLL 50-gold reroll receives no positive quality signal");
    require_close(tactical_reroll_quality_scale(1, 25.0, 20, 2), 0.0,
                  "critical-health reroll is still not positively shaped outside SLOWROLL");
    require_close(tactical_reroll_quality_scale(3, 90.0, 20, 2), 1.0,
                  "SLOWROLL retains its full reroll quality signal");
    require_close(reroll_quality_reward(8.0, 8.0), 0.0,
                  "reroll with no quality improvement receives no reward");
    if (!(reroll_quality_reward(2.0, 10.0, 50, 25.0, 5) >
          reroll_quality_reward(2.0, 10.0, 50, 90.0, 2))) {
        throw std::runtime_error("critical health should increase the value of a genuinely improved shop");
    }
    if (!(reroll_quality_reward(2.0, 10.0, 50, 90.0, 5) >
          reroll_quality_reward(2.0, 10.0, 15, 90.0, 2))) {
        throw std::runtime_error("later-stage surplus should make a useful reroll more viable");
    }
    require_close(strategy_action_reward(0, 1, 60, 58, 80.0, 4,
                                         false, true, false, false),
                  -0.15, "rich late ECON rerolls receive a strong learnable penalty");
    require_close(strategy_action_reward(1, 1, 55, 53, 90.0, 3,
                                         false, false, false, true),
                  -0.12, "STABILIZE rerolls are penalized without masking the action");
    require_close(strategy_action_reward(1, 1, 55, 53, 90.0, 3,
                                         false, true, false, true),
                  -0.12, "an improved non-SLOWROLL reroll remains costly");
    require_close(strategy_action_reward(2, 1, 59, 57, 60.0, 5,
                                         false, true, false, true),
                  -0.15, "late rich FAST8 rerolls receive the strongest penalty");
    require_close(strategy_action_reward(2, 1, 40, 38, 80.0, 3,
                                         false, true, false, true),
                  -0.12, "stage-three FAST8 rerolls remain legal but discouraged");
    require_close(strategy_action_reward(4, 1, 60, 58, 80.0, 4,
                                         false, false, false, false),
                  -0.15, "ALL_IN rerolls are discouraged outside SLOWROLL");
    require_close(strategy_action_reward(4, 6, 90, 90, 20.0, 5,
                                         false, false, false, false),
                  -0.32, "ALL_IN strongly penalizes PASS while meaningful gold remains");
    if (!(strategy_action_reward(0, 2, 80, 76, 80.0, 4,
                                 false, false, false, false) > 0.0))
        throw std::runtime_error("ECON receives a small XP incentive when spending above 50 gold");
    require_close(strategy_action_reward(5, 1, 35, 33, 80.0, 2,
                                         false, false, false, false),
                  -0.09, "WINSTREAK rerolls are discouraged in the opening");
    require_close(strategy_action_reward(6, 1, 60, 58, 80.0, 4,
                                         false, false, false, false),
                  -0.15, "LOSS_STREAK rerolls are discouraged with surplus gold");
    require_close(strategy_action_reward(1, 1, 60, 58, 20.0, 4,
                                         false, true, false, true),
                  -0.06, "critical HP softens but does not erase non-SLOWROLL reroll cost");
    require_close(strategy_action_reward(3, 1, 55, 53, 90.0, 3,
                                         false, true, false, true),
                  0.12, "slow-roll strategy receives a stronger improved-reroll signal");
    require_close(strategy_action_reward(3, 2, 30, 28, 80.0, 2,
                                         false, false, false, false),
                  -0.04, "safe early slow-roll XP nudge remains bounded");
    require_close(strategy_action_reward(3, 2, 60, 58, 25.0, 5,
                                         false, false, false, false),
                  0.0, "urgent or surplus XP progress is not penalized by slow-roll label");
    if (!(reroll_quality_reward(2.0, 10.0) > 0.0)) {
        throw std::runtime_error("reroll that reveals a much better offer should be rewarded");
    }
    if (!(reroll_quality_reward(10.0, 2.0) < 0.0)) {
        throw std::runtime_error("reroll that destroys a strong shop option should be discouraged");
    }
    if (!(reroll_quality_reward(2.0, 10.0, 50, 80.0, 4) >
          reroll_quality_reward(2.0, 10.0, 15, 80.0, 1))) {
        throw std::runtime_error("reroll improvement must be more valuable with surplus gold than in the safe early game");
    }
    if (!(reroll_quality_reward(10.0, 2.0, 50, 80.0, 4) < 0.0)) {
        throw std::runtime_error("reroll that destroys a stronger shop must receive a negative quality signal");
    }

    if (!protects_upgrade_path(1, 1, 0) || protects_upgrade_path(1, 0, 0) ||
        !protects_upgrade_path(2, 1, 0) || !protects_upgrade_path(2, 0, 3) ||
        protects_upgrade_path(2, 0, 2) || protects_upgrade_path(3, 0, 3)) {
        throw std::runtime_error("only real same-star pairs and complete 1-star-to-2-star paths should be protected");
    }
    require_close(upgrade_path_strength(1, 1, 0), 1.0,
                  "selling one of exactly two 1-star copies destroys the pair");
    require_close(upgrade_path_strength(1, 2, 0), 0.5,
                  "extra 1-star copies reduce, but do not erase, upgrade risk");
    require_close(upgrade_path_strength(2, 1, 0), 0.65,
                  "a second 2-star copy retains meaningful 3-star potential");
    require_close(upgrade_path_strength(2, 0, 3), 0.45,
                  "three 1-star copies provide a weaker alternative upgrade path");
    require_close(upgrade_path_strength(2, 0, 2), 0.0,
                  "two 1-star copies are not yet a complete upgrade path");

    if (!(sale_quality(0.0, false, true, true) < 0.0)) {
        throw std::runtime_error("selling a pair copy must remain unattractive even when it frees bench space");
    }
    require_close(critical_all_in_sale_penalty(4, 3.0, true, true, false),
                  -0.1416,
                  "critical ALL_IN at 3 HP adds retention pressure to core/pair sales");
    require_close(critical_all_in_sale_penalty(4, 3.0, true, true, true),
                  0.0,
                  "measurable board improvement can justify a critical ALL_IN sale");
    require_close(critical_all_in_sale_penalty(1, 3.0, true, true, false),
                  0.0,
                  "critical sale retention adjustment is scoped to ALL_IN");
    require_close(sale_quality(0.0, false, false, false), -0.025,
                  "selling non-core bench clutter has a small retention/action cost");
    require_close(sale_quality(0.0, false, false, true), 0.020,
                  "selling low-fit clutter is rewarded only when the bench is full");
    require_close(sale_quality(1.0, true, true, false), -1.025,
                  "sale strongly protects core units and upgrade copies");
    require_close(sale_quality(1.0, true, true, false, true), -0.265,
                  "measurable board gain removes the core and pair sale penalties");
    require_close(sale_quality(1.0, true, true, false, false, true), -1.025,
                  "context alone must not soften core and upgrade-path protection");
    require_close(sale_quality(0.8, true, true, false, false, true, 1.0),
                  sale_quality(0.8, true, true, false, false, false, 1.0),
                  "context must not weaken core-plus-pair retention");
    require_close(sale_quality(0.0, false, true, false, false, true, 1.0),
                  sale_quality(0.0, false, true, false, false, false, 1.0),
                  "context must not erase a real upgrade path");
    if (!(sale_quality(0.25, true, false, false) >
          sale_quality(1.0, true, false, false))) {
        throw std::runtime_error("core retention pressure must scale with actual composition fit");
    }
    if (!(sale_quality(0.0, false, false, false, false, false, 0.25) >
          sale_quality(0.0, false, false, false, false, false, 1.0))) {
        throw std::runtime_error("upgrade retention pressure must scale with remaining copy potential");
    }
    if (!(sale_quality(0.0, false, false, false) >
          sale_quality(0.5, false, false, false) &&
          sale_quality(0.5, false, false, false) >
          sale_quality(1.0, false, false, false))) {
        throw std::runtime_error("sale reward must decrease as bench-unit fit increases");
    }

    // Test A: a high-fit purchase should beat PASS even when it spends through
    // an interest threshold, while a low-value purchase should not.
    const double pass_reward = 0.0;
    const double strong_buy_reward = 0.05 +
        purchase_quality(10.0, false) + gold_change_reward(10, 8);
    const double weak_buy_reward = 0.05 +
        purchase_quality(0.0, false) + gold_change_reward(10, 8);
    if (!(strong_buy_reward > pass_reward && weak_buy_reward < pass_reward)) {
        std::cerr << "[FAIL] Economic decision shaping does not distinguish a strong buy from PASS.\\n";
        std::exit(EXIT_FAILURE);
    }

    // Phase 5: item reward shaping balances immediate combat value with
    // data-driven role/composition fit, shifting that balance by game stage.
    const double weak_item_strength = item_immediate_strength(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    const double damage_item_strength = item_immediate_strength(
        10.0, 0.15, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    const double defense_item_strength = item_immediate_strength(
        0.0, 0.0, 150.0, 10.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    const double ability_item_strength = item_immediate_strength(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 30.0, 0.0, 0.0);
    const double resist_item_strength = item_immediate_strength(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 30.0, 0.0);
    const double mana_item_strength = item_immediate_strength(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 30.0);
    require_close(weak_item_strength, 0.0, "no numeric combat effects have zero immediate strength");
    if (!(damage_item_strength > weak_item_strength &&
          defense_item_strength > weak_item_strength &&
          ability_item_strength > weak_item_strength &&
          resist_item_strength > weak_item_strength &&
          mana_item_strength > weak_item_strength &&
          damage_item_strength <= 1.0 && defense_item_strength <= 1.0 &&
          ability_item_strength <= 1.0 && resist_item_strength <= 1.0 &&
          mana_item_strength <= 1.0)) {
        throw std::runtime_error("item immediate strength must respond to supported combat stats and stay normalized");
    }
    const double early_comp_fit = item_assignment_quality(0.2, 0.9, 0.9, 1);
    const double early_raw_power = item_assignment_quality(0.9, 0.1, 0.1, 1);
    if (!(early_comp_fit > early_raw_power)) {
        throw std::runtime_error("early-game item reward should favor planned-comp fit over raw stats alone");
    }
    const double late_raw_power = item_assignment_quality(0.9, 0.1, 0.1, 7);
    const double late_comp_fit = item_assignment_quality(0.1, 0.9, 0.9, 7);
    if (!(late_raw_power > late_comp_fit)) {
        throw std::runtime_error("late-game item reward should favor immediate combat strength when comp fit is weak");
    }
    require_close(item_assignment_quality(0.0, 0.0, 0.0, 1), -0.03,
                  "strategically poor item assignment has bounded negative reward");
    if (item_assignment_quality(2.0, 2.0, 2.0, 4) > 0.20 ||
        item_assignment_quality(-1.0, -1.0, -1.0, 4) < -0.03) {
        throw std::runtime_error("item assignment reward must clamp out-of-range inputs");
    }

    // Strategy-specific shaping stays small and rewards the plan's actual goal.
    require_close(strategy_action_reward(0, 2, 40, 35, 80.0, 3,
                                         false, false, false, false),
                  0.0, "ECON no longer adds a blanket XP penalty after stage 2");
    require_close(strategy_action_reward(0, 2, 50, 46, 80.0, 3,
                                         false, false, false, false),
                  0.025, "ECON gives a small XP spending incentive above 50 gold");
    require_close(strategy_action_reward(0, 1, 40, 38, 80.0, 3,
                                         false, true, false, false),
                  -0.12, "stage-three ECON rerolls remain discouraged even when the shop improves");
    require_close(strategy_action_reward(0, 1, 40, 38, 80.0, 3,
                                         false, false, false, false),
                  -0.12, "stage-three ECON applies the same bounded reroll penalty");
    require_close(strategy_action_reward(0, 0, 55, 50, 80.0, 4,
                                         true, false, false, false),
                  0.05, "ECON allows useful spending from surplus above 50");
    require_close(strategy_action_reward(1, 2, 60, 56, 80.0, 4,
                                         false, false, true, false),
                  0.0475, "STABILIZE XP reward is reduced by five percent");
    require_close(surplus_xp_spending_reward(103, 99, 5, 1, false), 0.0265525,
                  "STABILIZE receives only a modest scaled surplus XP signal");
    require_close(strategy_action_reward(2, 2, 40, 35, 80.0, 4,
                                         false, false, true, true),
                  0.03, "FAST8 keeps level progress modest below the interest cap");
    require_close(strategy_action_reward(3, 1, 40, 38, 80.0, 3,
                                         false, true, false, true),
                  0.12, "SLOWROLL rewards a useful reroll");
    require_close(strategy_action_reward(3, 1, 40, 38, 80.0, 3,
                                         false, false, false, true),
                  -0.025, "SLOWROLL penalizes a reroll without improvement");
    require_close(strategy_action_reward(1, 0, 25, 22, 30.0, 3,
                                         true, false, false, false),
                  0.07, "STABILIZE rewards a useful purchase");
    require_close(strategy_action_reward(6, 0, 40, 37, 70.0, 3,
                                         false, false, false, false),
                  -0.04, "LOSS_STREAK discourages wasteful spending");
    const double econ_xp_below_interest =
        strategy_action_reward(0, 2, 40, 36, 80.0, 4,
                               false, false, false, false);
    const double econ_reroll_below_interest =
        strategy_action_reward(0, 1, 40, 36, 80.0, 4,
                               false, true, false, false);
    if (!(econ_xp_below_interest > econ_reroll_below_interest))
        throw std::runtime_error("outside SLOWROLL, XP must be slightly preferable to reroll at the same gold cost");

    const double fast8_xp_urgent =
        strategy_action_reward(2, 2, 40, 36, 30.0, 4,
                               false, false, false, true) - 0.025 +
        xp_progress_reward(40, 36, 6, 6, 0, 4, 30.0, 4, 2, false);
    const double fast8_reroll_urgent =
        strategy_action_reward(2, 1, 40, 36, 30.0, 4,
                               false, true, false, true) +
        reroll_quality_reward(2.0, 10.0) *
            tactical_reroll_quality_scale(2, 30.0, 36, 4);
    if (!(fast8_xp_urgent > fast8_reroll_urgent))
        throw std::runtime_error("FAST8 urgent XP progress must beat a better shop reroll");

    const double slowroll_reroll =
        strategy_action_reward(3, 1, 40, 36, 80.0, 4,
                               false, true, false, true) +
        reroll_quality_reward(2.0, 10.0) *
            tactical_reroll_quality_scale(3, 80.0, 36, 4);
    const double slowroll_xp_action_reward =
        strategy_action_reward(3, 2, 40, 36, 80.0, 4,
                               false, false, false, false) - 0.12;
    if (!(slowroll_reroll > slowroll_xp_action_reward))
        throw std::runtime_error("only SLOWROLL should prefer an improved reroll over XP");

    // Consecutive movement shaping is a potential difference, so any
    // intermediate route telescopes to the same net reward as the direct move.
    const double seq_ab = movement_sequence_reward(0.10, 0.10, 0.35, 0.40);
    const double seq_bc = movement_sequence_reward(0.35, 0.40, 0.20, 0.25);
    const double seq_ac = movement_sequence_reward(0.10, 0.10, 0.20, 0.25);
    require_close(seq_ab + seq_bc, seq_ac,
                  "movement sequence reward telescopes to net board gain");
    require_close(movement_sequence_reward(0.25, 0.50, 0.25, 0.50),
                  0.0, "neutral movement sequence has no potential reward");
    require_close(movement_sequence_action_cost(0), 0.0,
                  "first movement in sequence has no extra action cost");
    require_close(movement_sequence_action_cost(1), -0.015,
                  "second movement receives a small bounded sequence cost");
    require_close(movement_sequence_action_cost(4), -0.015,
                  "fifth movement receives the final sequence cost");
    require_close(movement_sequence_action_cost(5), 0.0,
                  "sequence cost is capped after four extra movements");

    std::cout << "[PASS] C++ reward formulas and deterministic action sequence.\n";
    return EXIT_SUCCESS;
}
