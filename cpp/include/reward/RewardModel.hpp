#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tft::reward_model {

// Formula-compatible counterparts of ai/RewardManager.py.
// State is passed explicitly so both implementations can replay the same
// sequence of snapshots and compare every reward component.
inline double combat(
    bool won,
    double damage_dealt = 0.0,
    double damage_taken = 0.0,
    int enemy_alive = 0,
    int own_alive = 0
) noexcept {
    double reward = won ? 2.0 : -2.0;
    if (damage_dealt > 0.0)
        reward += std::min(std::tanh(damage_dealt / 10.0), 1.0);
    if (own_alive > 0)
        reward += std::min(static_cast<double>(own_alive) * 0.05, 0.25);
    if (enemy_alive > 0)
        reward -= std::min(static_cast<double>(enemy_alive) * 0.05, 0.25);
    if (damage_taken > 0.0)
        reward -= std::min(std::tanh(damage_taken / 15.0), 0.75);
    return reward;
}

inline double power(double current, double previous) noexcept {
    const double delta = current - previous;
    if (std::abs(delta) < 1e-6) return 0.0;
    return std::tanh(delta / 10.0);
}

inline double synergy(double current, double previous) noexcept {
    return std::tanh(current - previous);
}

inline double positioning(double current, double previous) noexcept {
    return 0.5 * std::tanh(current - previous);
}

inline double level(int current, int previous) noexcept {
    if (current <= previous) return 0.0;
    return std::min((current - previous) * 0.5, 1.5);
}

inline double economy(int gold, double hp) noexcept {
    double reward = 0.0;
    for (int threshold = 10; threshold <= 50; threshold += 10)
        if (gold >= threshold) reward += 0.05;
    if (hp <= 15.0) reward -= 0.15;
    else if (hp <= 30.0) reward -= 0.05;
    return reward;
}

inline double streak(int win_streak, int loss_streak) noexcept {
    double reward = 0.0;
    if (win_streak >= 2)
        reward += std::min(win_streak * 0.03, 0.15);
    if (loss_streak >= 2)
        reward += std::min(loss_streak * 0.02, 0.10);
    return reward;
}

inline double board(int occupied, int total_slots) noexcept {
    if (total_slots <= 0) return 0.0;
    return static_cast<double>(occupied) /
        static_cast<double>(total_slots) * 0.15;
}

inline double placement(int placement, int total_players) noexcept {
    if (total_players <= 1) return 0.0;
    return 1.0 - 2.0 *
        (static_cast<double>(placement - 1) /
         static_cast<double>(total_players - 1));
}

// Penalidade explícita para uma ação estruturalmente válida bloqueada pelo AntiLoop.
// É maior que a falha genérica (-0.10), sem dominar a recompensa terminal (+/-1.0).
inline double blocked_move() noexcept {
    return -0.35;
}

// Bônus pequeno, concedido uma única vez ao preencher a última vaga do nível.
inline double board_capacity_completion() noexcept {
    return 0.25;
}

// Potential-based shaping for liquid gold. The small linear term values
// liquidity; the larger 10-gold steps approximate the interest breakpoints.
// Applying only the potential delta avoids paying the agent repeatedly just
// for sitting on the same balance.
inline double gold_potential(int gold) noexcept {
    const int bounded = std::clamp(gold, 0, 50);
    const int surplus = std::clamp(gold - 50, 0, 150);
    return static_cast<double>(bounded) * 0.002 +
        static_cast<double>(bounded / 10) * 0.08 -
        static_cast<double>(surplus) * 0.0013;
}

// Bounded action shaping for the active economic strategy. The base action
// rewards remain authoritative; these small deltas only align behavior with the
// selected plan and cannot dominate combat/terminal outcomes.
inline double strategy_action_reward(
    int strategy_id,
    int action_type,
    int old_gold,
    int new_gold,
    double hp,
    int stage,
    bool useful_purchase,
    bool reroll_improved,
    bool leveled,
    bool has_composition_target) noexcept {
    // Do not mask rerolls: the policy must learn their opportunity cost.
    // Every strategy except SLOWROLL receives a context-sensitive penalty.
    // The penalty grows with surplus liquidity and match progression, while
    // critical HP softens (but never removes) the cost of an emergency roll.
    if (action_type == 1 && strategy_id != 3) {
        // STABILIZE may roll when a pressured board finds a materially better
        // shop. ALL_IN may do the same at critical HP; neither action is masked.
        if (strategy_id == 1 && board_power < 35.0 && hp > 25.0 && hp <= 65.0 && reroll_improved) return 0.04;
        if (strategy_id == 4 && hp <= 25.0 && reroll_improved) return 0.04;
        double penalty = stage >= 4 && old_gold >= 50 ? 0.15
            : (stage >= 3 || old_gold >= 40 ? 0.12 : 0.09);
        if (hp <= 25.0) penalty = std::min(penalty, 0.06);
        return -penalty;
    }

    double reward = 0.0;
    switch (strategy_id) {
        case 0: // ECON: protect early interest, but convert late reserves into strength.
            if (new_gold >= 50 && old_gold < 50 && stage <= 2) reward += 0.04;
            if (old_gold >= 50 && action_type == 0 && useful_purchase)
                reward += 0.05;
            if (old_gold >= 50 && action_type == 2 && new_gold < old_gold)
                reward += 0.025;
            // ECON keeps the interest reserve, then converts true surplus into
            // XP while its strong-board eligibility is active.
            if (board_power >= 35.0 && !useful_shop_purchase_available &&
                old_gold > 50 && action_type == 2 && new_gold >= 50)
                reward += leveled ? 0.07 : 0.025;
            if (new_gold < 50 && new_gold < old_gold && hp > 40.0) {
                // A comp target can justify buying units, not routine XP that
                // fails to reach a level. Keep the purchase-specific exception.
                if (action_type == 2 && !leveled && stage <= 2) reward -= 0.04;
                else if (action_type == 0 && !useful_purchase &&
                         stage <= 2 && !has_composition_target)
                    reward -= 0.06;
            }
            break;
        case 1: // STABILIZE: favor playable-board upgrades over routine leveling.
            if (action_type == 0 && useful_purchase) reward += 0.07;
            if (action_type == 2 && leveled)
                reward += 0.95 * (hp <= 30.0 ? 0.07 : (old_gold >= 50 ? 0.05 : 0.015));
            if (hp <= 35.0 && action_type == 6 && !useful_purchase)
                reward -= 0.04;
            break;
        case 2: // FAST8 is a plan, not permission to ignore early-game economy.
            if (action_type == 2 && leveled)
                reward += hp <= 30.0 ? 0.11 : (old_gold >= 50 ? 0.08 : 0.03);
            break;
        case 3: // SLOWROLL: reward improved rolls without blocking useful XP progress.
            if (action_type == 1) reward += reroll_improved ? 0.12 : -0.025;
            // In the observed match, repeated non-leveling XP purchases drained
            // SLOWROLL players to near-zero gold while the 3-star plan was active.
            // Strengthen the nudge without masking the action: urgent HP states,
            // later stages, level-ups, and the 50-gold reserve remain exempt.
            if (action_type == 2 && !leveled && stage <= 3 &&
                hp > 50.0 && old_gold < 50) reward -= 0.04;
            // A controlled level-up is compatible with a winning slowroll line
            // when enough liquidity remains to keep buying target copies.
            if (action_type == 2 && leveled && old_gold >= 30 && new_gold >= 30)
                reward += 0.05;
            break;
        case 4: // ALL_IN: convert remaining gold into immediate board strength.
            if (action_type == 0 && useful_purchase) reward += 0.10;
            if (action_type == 2 && old_gold > 0 && new_gold < old_gold)
                reward += 0.08;
            if (action_type == 6) reward -= 0.32;
            break;
        case 5: // WINSTREAK: preserve strength, avoid wasteful economy actions.
            if (action_type == 0 && useful_purchase) reward += 0.05;
            if (action_type == 2 && leveled) reward += 0.04;
            break;
        case 6: // Reserved legacy ID; LOSS_STREAK is no longer trained or selected.
            break;
        default:
            break;
    }
    (void)stage;
    (void)old_gold;
    return std::clamp(reward, -0.40, 0.15);
}

inline double gold_change_reward(int old_gold, int new_gold) noexcept {
    return std::clamp(
        gold_potential(new_gold) - gold_potential(old_gold),
        -0.25, 0.25);
}

// A purchase should be judged by marginal composition fit, not simply by
// the fact that a shop action succeeded. Completing an upgrade is a separate
// strategic signal because its value is not fully represented by cost/role.
inline double purchase_quality(double fit_delta, bool completes_upgrade) noexcept {
    // Give a useful acquisition a learnable immediate signal. The board-fit
    // delta alone can be near zero when the team is still being assembled.
    return 0.22 * std::tanh(fit_delta / 5.0) +
        (completes_upgrade ? 0.20 : 0.0);
}

// Composition-aware value for acquiring the second matching 1-star copy.
// A pair has option value even before it changes the board: the next copy can
// produce a 2-star unit. Keep fit as a multiplier, not a gate, so useful pairs
// are not ignored just because the current board is temporarily weak.
inline bool is_pair_purchase(int same_star_copies, int offer_star) noexcept {
    return same_star_copies == 1 && offer_star == 1;
}

inline double purchase_pair_quality(double fit_score) noexcept {
    // A pair is real upgrade progress even before the third copy appears.
    // Keep the baseline meaningful, while favoring units that fit the plan.
    return 0.36 + 0.16 * std::tanh(std::max(0.0, fit_score) / 5.0);
}

// Reward timely, high-value spending when liquidity is already healthy and
// either the player is under pressure or the game is late. This is deliberately
// conditional on purchase quality / an actual level-up: spending by itself is
// never rewarded, so reroll spam and low-fit shopping cannot farm reward.
inline double spending_timing_reward(
    int gold_before,
    double hp,
    int stage,
    double normalized_fit,
    bool completes_upgrade,
    bool leveled_up
) noexcept {
    if (gold_before < 20 || (!completes_upgrade && !leveled_up &&
        normalized_fit < 0.35)) {
        return 0.0;
    }

    // Spending becomes increasingly attractive once the 50-gold interest cap
    // is reached, but there is still a small signal for good early purchases.
    const double liquidity = gold_before >= 50 ? 1.0
        : (gold_before >= 40 ? 0.75
        : (gold_before >= 30 ? 0.50 : 0.25));
    const double health_urgency = std::clamp((55.0 - hp) / 55.0, 0.0, 1.0);
    const double stage_urgency = std::clamp((static_cast<double>(stage) - 2.0) / 4.0,
                                            0.0, 1.0);
    const double baseline_urgency = gold_before >= 50 ? 0.25 : 0.0;
    const double urgency = std::max({health_urgency, stage_urgency, baseline_urgency});
    if (liquidity <= 0.0 || urgency <= 0.0) return 0.0;

    const double quality = (completes_upgrade || leveled_up)
        ? 1.0 : std::clamp(normalized_fit, 0.0, 1.0);
    return 0.24 * liquidity * urgency * quality;
}

// Reward buying XP when it is strategically timely, including purchases that
// do not immediately cross a level boundary. This prevents the agent from
// hoarding a large bank until the end of the match. Early XP remains discouraged
// unless the player is under pressure; the strongest signal requires high gold
// or a level-up.
inline double buy_xp_timing_reward(
    int gold_before,
    double hp,
    int stage,
    bool leveled_up
) noexcept {
    // XP purchases that do not reach a new level have no immediate strategic
    // value by themselves. Do not use XP as a generic sink for excess gold.
    if (!leveled_up) return 0.0;
    if (stage <= 2 && hp >= 60.0) return 0.0;
    if (gold_before < 20 && hp >= 35.0) return 0.0;

    const double liquidity = gold_before >= 50 ? 1.0
        : (gold_before >= 40 ? 0.70 : (gold_before >= 30 ? 0.45 : 0.25));
    const double health_urgency = std::clamp((50.0 - hp) / 50.0, 0.0, 1.0);
    const double stage_urgency = std::clamp((static_cast<double>(stage) - 3.0) / 4.0,
                                            0.0, 1.0);
    const double urgency = std::max(health_urgency, stage_urgency);
    if (urgency <= 0.0) return 0.0;
    // Only an actual level transition is rewarded, with a strict cap.
    return std::min(0.10, 0.09 * liquidity * urgency);
}

// Relative positive-XP shaping by economic plan. Interest-breakpoint costs are
// never scaled: only the positive incentive is adjusted.
inline double xp_strategy_reward_scale(int active_strategy) noexcept {
    if (active_strategy == 1) return 0.95; // STABILIZE: -5%
    if (active_strategy == 3) return 0.45; // SLOWROLL: XP is a limited transition, not its primary spend.
    return 1.0;
}

// Marginal value of an XP purchase that actually crosses a level boundary.
// Urgency and the level milestone matter; crossing an interest breakpoint has
// an explicit opportunity cost. Non-leveling XP is not rewarded as a gold sink.
inline double xp_level_transition_value(
    int gold_before, int gold_after, double hp, int stage,
    int old_level, int new_level, double board_power_delta = 0.0,
    int active_strategy = -1
) noexcept {
    if (new_level <= old_level) return 0.0;
    const double urgency = std::max(
        std::clamp((45.0 - hp) / 45.0, 0.0, 1.0),
        std::clamp((static_cast<double>(stage) - 3.0) / 4.0, 0.0, 1.0));
    const bool key_milestone = new_level >= 4 && new_level <= 10;
    const double milestone_value = key_milestone ? 0.045 : 0.020;
    const double surplus = gold_before >= 50 ? 1.0
        : (gold_before >= 40 ? 0.75 : (gold_before >= 30 ? 0.50 : 0.25));
    const double marginal_strength = std::clamp(board_power_delta / 20.0, 0.0, 1.0);
    double value = milestone_value + 0.055 * urgency * surplus +
        0.030 * marginal_strength;
    int lost_breakpoints = 0;
    constexpr int interest_thresholds[] = {10, 20, 30, 40, 50};
    for (const int threshold : interest_thresholds) {
        if (gold_before >= threshold && gold_after < threshold) ++lost_breakpoints;
    }
    value -= 0.025 * static_cast<double>(lost_breakpoints);
    // Reduce only the positive level-up incentive. Preserve the full
    // opportunity-cost penalty when XP crosses an interest breakpoint.
    if (value > 0.0) {
        value *= 0.90;
        value *= xp_strategy_reward_scale(active_strategy);
    }
    return std::clamp(value, -0.05, 0.12);
}

// Convert surplus into XP only when no useful shop acquisition is available.
// This is a fallback, not a generic gold sink: SLOWROLL keeps its 3-star plan.
inline double surplus_xp_spending_reward(
    int gold_before, int gold_after, int stage, int active_strategy,
    bool useful_purchase_available
) noexcept {
    if (active_strategy == 3 || useful_purchase_available || stage < 4 ||
        gold_before < 60 || gold_after >= gold_before ||
        (active_strategy != 4 && gold_after < 50))
        return 0.0;
    const double surplus = std::clamp(
        (static_cast<double>(gold_before) - 50.0) / 100.0, 0.0, 1.0);
    // XP is a measured fallback, not the default sink for surplus. Keep the
    // reward below useful acquisition/pair signals and preserve the 50-gold
    // interest floor. This applies to every non-SLOWROLL plan; no ECON mode
    // assumption is made about balances at or above the interest cap.
    const double all_in_scale = active_strategy == 4 ? 1.25 : 1.0;
    return all_in_scale * xp_strategy_reward_scale(active_strategy) *
        (0.020 + 0.015 * surplus);
}

 // Progress reward for XP that does not immediately cross a level boundary.
// It is strategy-gated and deliberately smaller than an actual level-up.
// Useful shop pairs/upgrades keep priority; non-SLOWROLL plans may use the
// bounded surplus fallback, while SLOWROLL preserves gold for its three-star plan.
inline double xp_progress_reward(
    int gold_before, int gold_after, int old_level, int new_level,
    int old_xp, int new_xp, double hp, int stage, int active_strategy,
    bool useful_purchase_available
) noexcept {
    if (gold_before < 4 || gold_after >= gold_before || new_level != old_level
        || new_xp <= old_xp)
        return 0.0;
    if (active_strategy == 3 &&
        (stage < 4 || old_level >= 6 || hp > 35.0))
        return 0.0;
    // The shared surplus fallback already rewards safe XP above 50 gold.
    // This extra signal is only for urgent progress below that reserve, and
    // never competes with an actually useful shop offer.
    if (useful_purchase_available)
        return 0.0;
    const bool urgent = hp <= 35.0;
    if (!urgent)
        return 0.0;
    const double progress = std::clamp(
        static_cast<double>(new_xp - old_xp) / 4.0, 0.0, 1.0);
    if (progress <= 0.0) return 0.0;
    double value = 0.0;
    switch (active_strategy) {
        case 2: // FAST8: make incremental progress toward the planned level 8.
            if (stage >= 3 && old_level < 8)
                value = 0.035;
            break;
        case 1: // STABILIZE: modest XP progress is valid, but board purchases stay first.
            if (stage >= 3 || urgent)
                value = 0.035;
            break;
        case 4: // ALL_IN: only when life is critical and XP is a viable survival action.
            if (hp <= 25.0) value = 0.035;
            break;
        case 3: // SLOWROLL: only an urgent, reserve-safe transition toward level 6.
            if (stage >= 4 && old_level < 6)
                value = 0.035;
            break;
        case 5: // WINSTREAK: use only safe surplus.
            // Safe surplus XP is already covered by the shared fallback.
            break;
        default:
            break;
    }
    return std::clamp(value * progress, 0.0, 0.060);
}

// Phase-aware XP progress: the expected level rises with the match. This
// provides a learnable signal even when a purchase adds XP without leveling,
// while preserving the 50-gold interest floor and the SLOWROLL three-star plan.
inline int economy_phase_target_level(int stage, int active_strategy = -1) noexcept {
    if (active_strategy == 2) return 8; // FAST8 is always anchored to level 8.
    if (active_strategy == 3) return 6; // SLOWROLL keeps its level-6 roll window.
    switch (stage) {
        case 1: return 3;
        case 2: return 5;
        case 3: return 7;
        case 4: return 8;
        case 5: return 9;
        default: return 9;
    }
}

// The interest cap is a reference, not a hard reserve. As the match advances,
// the amount kept liquid falls so the policy cannot finish rich and under-levelled.
inline int economy_phase_minimum_reserve(
    int stage, double hp, int active_strategy
) noexcept {
    // Emergency stabilization takes precedence over economic targets.
    if (hp <= 20.0 || active_strategy == 4) return 0;
    if (hp <= 30.0) return 5;

    // SLOWROLL protects enough liquidity to continue buying target copies;
    // its reserve is intentionally independent of the generic XP curve.
    if (active_strategy == 3) {
        const int reserve = stage <= 3 ? 30 : (stage == 4 ? 25 : 15);
        return hp <= 40.0 ? std::max(5, reserve - 5) : reserve;
    }

    // A gradually declining floor: preserve opening economy, then release
    // liquidity as leveling becomes more valuable. It is a soft eligibility
    // threshold for XP shaping, never a hard spending restriction.
    int reserve = stage <= 2 ? 30
        : (stage == 3 ? 20
        : (stage == 4 ? 10
        : (stage == 5 ? 5 : 0)));
    if (hp <= 40.0) reserve = std::max(0, reserve - 5);
    return reserve;
}

inline double phase_xp_progress_reward(
    int gold_before, int gold_after, int old_level, int new_level,
    int old_xp, int new_xp, double hp, int stage, int active_strategy,
    bool useful_purchase_available
) noexcept {
    // This signal is only for partial XP progress. Actual level transitions
    // already receive milestone, level-up and board-capacity rewards elsewhere.
    // Urgent low-HP partial progress remains covered by xp_progress_reward().
    if (stage < 3 || hp <= 35.0 ||
        gold_after >= gold_before || new_level != old_level ||
        new_xp <= old_xp)
        return 0.0;
    // SLOWROLL may make a controlled transition from level 5 to 6, but only
    // from stage 4 onward; its reserve and target-level checks still apply.
    if (active_strategy == 3 && (stage < 4 || old_level >= 6))
        return 0.0;

    // Keep phase-aware progress active with 60+ gold when the player
    // retains the 50-gold interest floor. The generic surplus fallback is
    // intentionally suppressed by the caller whenever this stronger,
    // target-aware signal applies; otherwise FAST8/STABILIZE receives only
    // the tiny fallback and can prefer PASS despite a valid level-up plan.

    const int target_level = economy_phase_target_level(stage, active_strategy);
    if (old_level >= target_level)
        return 0.0;

    const int reserve = economy_phase_minimum_reserve(stage, hp, active_strategy);
    if (gold_after < reserve)
        return 0.0;

    const double deficit = std::clamp(
        static_cast<double>(target_level - old_level) / 3.0, 0.0, 1.0);
    // Keep useful XP purchases meaningful when only one level remains.
    // Linear deficit scaling made each partial purchase worth less as the
    // player approached the target, despite the same marginal XP progress.
    const double target_proximity = 0.70 + 0.30 * deficit;
    const double stage_scale = stage >= 6 ? 1.05
        : (stage >= 5 ? 1.0 : (stage >= 4 ? 0.90 : 0.70));
    const double liquidity_scale = gold_after >= 50 ? 1.0 : 0.75;
    const double shop_priority_scale = useful_purchase_available ? 0.50 : 1.0;
    double strategy_scale = 1.0;
    if (active_strategy == 2) strategy_scale = 1.20; // FAST8
    else if (active_strategy == 1) strategy_scale = 0.90; // STABILIZE: board first
    else if (active_strategy == 3) strategy_scale = 0.45; // SLOWROLL: controlled transition to level 6 only
    else if (active_strategy == 5) strategy_scale = 0.90; // WINSTREAK: protect strength
    else if (active_strategy == 6) strategy_scale = 0.80; // LOSS_STREAK

    const double progress = std::clamp(
        static_cast<double>(new_xp - old_xp) / 4.0, 0.0, 1.0);
    // When the shop has no useful acquisition, give phase-relevant XP a
    // slightly clearer fallback signal. If a useful offer exists, retain the
    // previous lower value so XP does not compete with buying that unit.
    // This remains partial-progress shaping and never stacks on a level-up.
    const double base_signal = useful_purchase_available ? 0.045 : 0.052;
    return std::clamp(base_signal * target_proximity * stage_scale * liquidity_scale *
        shop_priority_scale * strategy_scale * progress, 0.0, 0.060);
}

// True only when a legal BUY_XP action can make phase-relevant progress while
// respecting the strategy's soft reserve. Used to distinguish idle PASS from
// a turn where the player genuinely has no useful spending opportunity.
inline bool phase_xp_opportunity_available(
    int gold, int level, double hp, int stage, int active_strategy
) noexcept {
    if (stage < 3 || gold < 4)
        return false;
    // Do not invite SLOWROLL XP unless it is a controlled transition to level 6.
    if (active_strategy == 3 && (stage < 4 || level >= 6))
        return false;
    const int target_level = economy_phase_target_level(stage, active_strategy);
    if (level >= target_level)
        return false;
    const int reserve = economy_phase_minimum_reserve(stage, hp, active_strategy);
    return gold - 4 >= reserve;
}

// Small additional signal for useful unit acquisitions beyond the early game.
// Requires real composition fit or upgrade progress, available bench space and
// enough remaining gold, so this cannot reward arbitrary shop-clearing.
inline double unit_purchase_timing_reward(
    int stage,
    int gold_before,
    int cost,
    int bench_count,
    double normalized_fit,
    double hp,
    bool completes_upgrade,
    bool builds_pair,
    int active_strategy = 0
) noexcept {
    if (stage < 3 || cost <= 0 || gold_before < cost ||
        bench_count >= 7) return 0.0;

    const double fit = std::clamp(normalized_fit, 0.0, 1.0);
    if (!completes_upgrade && !builds_pair && fit < 0.35) return 0.0;

    const int gold_after = gold_before - cost;
    const int reserve = economy_phase_minimum_reserve(stage, hp, active_strategy);
    if (gold_after < reserve) return 0.0;

    const double liquidity = gold_before >= 50 ? 1.0
        : (gold_before >= 30 ? 0.80 : 0.65);
    const double surplus = std::clamp(
        (static_cast<double>(gold_before) - 50.0) / 100.0, 0.0, 1.0);
    const double stage_urgency = std::clamp(
        (static_cast<double>(stage) - 2.0) / 4.0, 0.0, 1.0);
    const double health_urgency = std::clamp((50.0 - hp) / 50.0, 0.0, 1.0);
    const double urgency = std::max({stage_urgency, health_urgency,
                                     gold_before >= 50 ? 0.75 : 0.0});
    if (urgency <= 0.0) return 0.0;

    const double quality = (completes_upgrade || builds_pair)
        ? 1.0 : (0.45 + 0.55 * fit);
    const double bench_capacity = bench_count <= 3 ? 1.0
        : (bench_count <= 5 ? 0.75 : 0.40);
    const double strategy_scale = active_strategy == 2 ? 1.10
        : (active_strategy == 1 || active_strategy == 4 ? 1.05 : 1.0);
    // Tiny calibration nudge: convert a little more genuine surplus into
    // useful board strength, without lowering the phase reserve or rewarding
    // low-fit buys. The increase is deliberately limited to about 4-5%.
    return std::min(0.24,
        (0.125 * liquidity * urgency + 0.105 * surplus) *
        quality * bench_capacity * strategy_scale);
}

// Early-game acquisition shaping: prefer filling useful pairs and building a
// playable bench before spending actions on XP. This is deliberately bounded
// and only applies when there is room and the purchase is affordable.
inline double early_purchase_reward(
    int stage,
    int gold_before,
    int cost,
    int bench_count,
    double normalized_fit,
    bool completes_upgrade,
    bool builds_pair
) noexcept {
    if (stage > 2 || bench_count >= 9 || cost <= 0 || gold_before < cost)
        return 0.0;

    const double fit = std::clamp(normalized_fit, 0.0, 1.0);
    // In stages 1-2, a modestly compatible acquisition has option value
    // before the final composition is known. Avoid arbitrary clutter: require
    // a small fit signal unless it forms a real pair or completes an upgrade.
    if (!completes_upgrade && !builds_pair && fit < 0.08 &&
        (cost > 2 || bench_count >= 7))
        return 0.0;

    const double capacity = bench_count <= 3 ? 1.0
        : (bench_count <= 5 ? 0.75 : 0.40);
    const double quality = completes_upgrade ? 1.0
        : (builds_pair ? std::max(0.70, fit) : (0.35 + 0.65 * fit));
    const double base = (completes_upgrade || builds_pair) ? 0.12
        : (fit < 0.08 ? 0.04 : 0.055);
    const double fit_bonus = (completes_upgrade || builds_pair) ? 0.16 : 0.075;
    return capacity * (base + fit_bonus * quality);
}

// Small opportunity-cost signal for passing while rich and under pressure,
// but only when a genuinely useful shop purchase or upgrade is available.
// The caller must establish that opportunity; an empty/weak shop is not a
// reason to force spending.
inline double reroll_quality_reward(
    double old_best_fit,
    double new_best_fit,
    int gold_before = 50,
    double hp = 100.0,
    int stage = 1
) noexcept {
    // Reroll only has value when the shop gains a genuinely better strategic
    // option. Keep it weaker than the interest opportunity cost while poor,
    // but allow a strong roll to be worthwhile with surplus gold or under
    // meaningful health pressure.
    const double improvement = std::tanh((new_best_fit - old_best_fit) / 5.0);
    const double liquidity = gold_before >= 50 ? 1.0
        : (gold_before >= 40 ? 0.45
        : (gold_before >= 30 ? 0.25
        : (gold_before >= 20 ? 0.12 : 0.05)));
    const double urgency = std::clamp((45.0 - hp) / 45.0, 0.0, 1.0);
    const double stage_factor = stage <= 2 && hp > 50.0 ? 0.65 : 1.0;
    const double pressure_factor = 1.0 + 0.75 * urgency;
    // Below the interest cap a roll should usually lose to saving; under
    // critical HP, pressure can justify that opportunity cost.
    return 0.14 * improvement * liquidity * stage_factor * pressure_factor;
}

// Shared reroll gate: only SLOWROLL receives positive shop-quality shaping.
inline double tactical_reroll_quality_scale(
    int active_strategy,
    double hp,
    int gold_after,
    int stage
) noexcept {
    // Reroll is the defining spend action only for SLOWROLL. Other strategies
    // receive no positive reroll shaping; their shop search must not compete
    // with planned XP progress or interest preservation.
    (void)hp;
    (void)gold_after;
    (void)stage;
    return active_strategy == 3 ? 1.0 : 0.0;
}

inline double pass_opportunity_penalty(
    int gold,
    double hp,
    int stage,
    bool useful_purchase_available,
    bool useful_xp_available = false
) noexcept {
    if (gold < 15)
        return 0.0;
    // Above the 50-gold interest cap, an empty/weak shop is not a reason to
    // PASS: the policy must choose its strategy-appropriate fallback (XP or
    // reroll). At/below 50, keep the previous opportunity-based behavior.
    if (gold <= 50 && !useful_purchase_available && !useful_xp_available)
        return 0.0;

    // Rich PASS is always discouraged. A useful shop offer increases the
    // penalty; otherwise the base surplus pressure teaches the policy to
    // convert the excess through XP or a strategic reroll.
    const double surplus = std::clamp(
        (static_cast<double>(gold) - 50.0) / 100.0, 0.0, 2.0);
    const double stage_scale = stage <= 2 ? 0.25
        : (stage == 3 ? 0.70 : (stage == 4 ? 1.0
        : (stage == 5 ? 1.10 : 1.20)));
    const double surplus_pressure = gold >= 50
        ? 0.035 + 0.075 * std::pow(surplus, 1.10)
        : 0.025 * std::clamp(static_cast<double>(gold - 15) / 35.0, 0.0, 1.0);
    const double useful_offer_pressure = useful_purchase_available
        ? (gold >= 30 ? 0.14 : 0.08) : 0.0;
    const double useful_xp_pressure = useful_xp_available
        ? (gold >= 50 ? 0.045 : 0.025) : 0.0;
    const double health_pressure =
        std::clamp((45.0 - hp) / 45.0, 0.0, 1.0) *
        std::clamp(static_cast<double>(gold - 10) / 30.0, 0.0, 1.0) * 0.08;
    const double low_hp_surplus_pressure =
        std::clamp((50.0 - hp) / 50.0, 0.0, 1.0) *
        std::clamp((static_cast<double>(gold) - 50.0) / 75.0, 0.0, 1.0) * 0.050;

    return -std::clamp(
        stage_scale * (surplus_pressure + useful_offer_pressure +
                       useful_xp_pressure + health_pressure +
                       low_hp_surplus_pressure),
        0.0, 0.50);
}

// A recent buy/sell sequence is not automatically a mistake. Penalize only
// low-fit acquisitions with no concrete strategic, experimental, composition,
// or economic justification. Fit is normalized to [0, 1].
inline constexpr double kRecentPurchaseLowFitThreshold = 0.20;
// Keep purchase provenance long enough to associate a sale after intervening
// shop/position actions, without carrying stale intent across a whole round.
inline constexpr std::uint64_t kRecentPurchaseHistoryActions = 8;

inline bool should_penalize_reversed_purchase(
    double normalized_fit,
    bool completed_upgrade,
    bool was_deployed,
    bool bench_was_full,
    bool composition_changed,
    bool built_pair = false,
    bool better_alternative_found = false,
    bool economic_need = false
) noexcept {
    if (completed_upgrade || built_pair || was_deployed || bench_was_full ||
        composition_changed || better_alternative_found || economic_need) {
        return false;
    }
    return std::clamp(normalized_fit, 0.0, 1.0) <=
        kRecentPurchaseLowFitThreshold;
}

inline double reversed_purchase_penalty() noexcept {
    return -0.30;
}

inline double reversed_sequence_adjustment(
    double prior_buy_reward,
    double sale_reward_including_gold,
    double target = -0.05
) noexcept {
    return std::min(0.0, target - prior_buy_reward - sale_reward_including_gold);
}

// Protect a real same-star pair. A 2-star also remains valuable when three
// 1-star copies can create another 2-star, but a lone 1-star beside a 2-star
// is not a pair and should not be artificially locked on the bench.
inline double upgrade_path_strength(
    int sold_star, int same_star_copies, int one_star_copies
) noexcept {
    same_star_copies = std::max(0, same_star_copies);
    one_star_copies = std::max(0, one_star_copies);
    if (sold_star == 1) {
        // Selling one of exactly two copies destroys the immediate pair.
        // Extra copies reduce the opportunity cost because a pair remains.
        if (same_star_copies <= 0) return 0.0;
        return same_star_copies == 1 ? 1.0 :
            1.0 / static_cast<double>(same_star_copies);
    }
    if (sold_star == 2) {
        // A second 2-star copy contributes to a 3-star route; three 1-stars
        // are a viable, but less immediate, replacement path to another 2-star.
        if (same_star_copies >= 1)
            return std::min(1.0, 0.65 + 0.15 * (same_star_copies - 1));
        if (one_star_copies >= 3)
            return std::min(0.45, 0.15 * static_cast<double>(one_star_copies));
    }
    return 0.0;
}

inline bool protects_upgrade_path(
    int sold_star, int same_star_copies, int one_star_copies
) noexcept {
    // 0.15 * 3 can be represented just below 0.45 in binary floating
    // point; use a tiny tolerance so the documented three-copy threshold
    // does not depend on rounding.
    return upgrade_path_strength(sold_star, same_star_copies, one_star_copies) +
        1e-9 >= 0.45;
}

// Selling should be attractive for low-value clutter, but not intrinsically
// rewarding for disposing of useful bench units. Fit is normalized to [0, 1].
// Gold recovery is shaped separately by gold_change_reward().
// Additional retention signal for emergency ALL_IN states. This does not
// block SELL: it only makes discarding core units or real upgrade paths less
// attractive when HP is critical and the sale does not improve the board.
inline double critical_all_in_sale_penalty(
    int active_strategy, double hp, bool core_unit,
    bool protects_upgrade, bool compensated_by_board_gain
) noexcept {
    if (active_strategy != 4 || hp > 25.0 ||
        (!core_unit && !protects_upgrade) || compensated_by_board_gain)
        return 0.0;
    const double urgency = std::clamp((25.0 - hp) / 25.0, 0.0, 1.0);
    return -(0.08 + 0.07 * urgency);
}

inline double sale_quality(double fit_score, bool core_unit,
                           bool protects_upgrade, bool frees_bench_space,
                           bool compensated_by_board_gain = false,
                           bool justified_by_context = false,
                           double upgrade_strength = -1.0) noexcept {
    const double fit = std::clamp(fit_score, 0.0, 1.0);
    // Preserve the old boolean behavior for callers without copy-count data.
    const double upgrade_pressure = upgrade_strength < 0.0
        ? (protects_upgrade ? 1.0 : 0.0)
        : std::clamp(upgrade_strength, 0.0, 1.0);
    // Strategic importance is continuous: a core label alone is not enough;
    // actual composition fit and a concrete upgrade path determine retention.
    const double core_pressure = core_unit ? (0.20 + 0.80 * fit) : 0.0;
    // A sale is not a positive action merely because it produces gold. Keep a
    // small retention cost unless the bench is genuinely full; the gold
    // potential already rewards economically meaningful threshold crossings.
    // Context may soften retention pressure for ordinary bench clutter,
    // but it must not erase protection for a core/planned unit or a real
    // upgrade path. Those sales need measurable board compensation instead.
    const double context_factor =
        justified_by_context && core_pressure <= 0.0 && upgrade_pressure <= 0.0
            ? 0.65 : 1.0;
    return -0.025 - 0.24 * fit * context_factor -
        (core_pressure > 0.0 && !compensated_by_board_gain
            ? 0.37 * core_pressure * context_factor : 0.0) -
        (upgrade_pressure > 0.0 && !compensated_by_board_gain
            ? 0.39 * upgrade_pressure * context_factor : 0.0) +
        (frees_bench_space ? 0.045 : 0.0);
}

// Potential-based movement shaping. Using the difference of bounded absolute
// potentials (rather than tanh of each local delta) makes a sequence telescope:
// the sum of intermediate rewards equals the net start-to-finish board change.
inline double movement_sequence_potential(
    double positioning_score,
    double synergy_score
) noexcept {
    return 0.18 * std::tanh(positioning_score / 0.20) +
        0.10 * std::tanh(synergy_score);
}

inline double movement_sequence_reward(
    double before_positioning,
    double before_synergy,
    double after_positioning,
    double after_synergy
) noexcept {
    return movement_sequence_potential(after_positioning, after_synergy) -
        movement_sequence_potential(before_positioning, before_synergy);
}

// First movement is free. The next four successful movements pay a tiny
// incremental cost; longer sequences do not accumulate an unbounded penalty.
inline double movement_sequence_action_cost(
    std::uint32_t prior_successful_moves
) noexcept {
    return prior_successful_moves >= 1 && prior_successful_moves <= 4
        ? -0.015 : 0.0;
}

// Shared no-progress penalty for bench-to-board replacement. Negative deltas add
// proportional cost instead of being flattened by a fixed assignment.
// Penalize replacing a planned/core board unit with an equivalent or
// weaker bench unit. Clear composition upgrades remain allowed.
inline double core_displacement_penalty(
    bool displaced_core_unit,
    double composition_quality_delta
) noexcept {
    return displaced_core_unit && composition_quality_delta < 2.0
        ? -0.30 : 0.0;
}

inline double movement_no_progress_penalty(
    double composition_delta,
    double synergy_delta,
    double positioning_delta
) noexcept {
    // A move that changes the slot but does not improve any strategic metric
    // should be less attractive than buying/deploying a useful unit. The larger
    // baseline specifically discourages the repeated empty-slot shuffling seen
    // in Viewer logs, while degradations add further cost.
    return -0.10
        - 0.14 * std::tanh(std::max(0.0, -composition_delta) / 5.0)
        - 0.14 * std::tanh(std::max(0.0, -synergy_delta))
        - 0.08 * std::tanh(std::max(0.0, -positioning_delta));
}


// Normalized immediate combat value for the numeric effects currently consumed
// by CombatEngine. Stats that the simulator does not yet model (for example,
// ability-specific passives) are intentionally excluded rather than guessed.
inline double item_immediate_strength(
    double attack_damage,
    double attack_speed,
    double max_health,
    double armor,
    double critical_strike_chance,
    double critical_strike_damage,
    double omnivamp,
    double damage_amplification,
    double damage_reduction,
    double ability_power = 0.0,
    double magic_resist = 0.0,
    double mana = 0.0
) noexcept {
    const double raw =
        std::max(0.0, attack_damage) / 100.0 +
        std::max(0.0, attack_speed) / 0.5 +
        std::max(0.0, ability_power) / 100.0 +
        std::max(0.0, max_health) / 1000.0 +
        std::max(0.0, armor) / 100.0 +
        std::max(0.0, magic_resist) / 100.0 +
        std::max(0.0, mana) / 100.0 +
        std::max(0.0, critical_strike_chance) / 0.5 +
        std::max(0.0, critical_strike_damage) +
        std::max(0.0, omnivamp) / 0.5 +
        std::max(0.0, damage_amplification) / 0.5 +
        std::max(0.0, damage_reduction) / 0.5;
    return std::clamp(1.0 - std::exp(-raw), 0.0, 1.0);
}

// Potential-like reward for item assignment. Composition fit and catalog role
// weights preserve long-term build value early; the balance shifts toward
// measured immediate combat stats as the match approaches its end. This is
// shaping only: it does not force a target or prescribe a particular item.
inline double item_assignment_quality(
    double immediate_strength,
    double composition_fit,
    double role_alignment,
    int stage
) noexcept {
    const double immediate = std::clamp(immediate_strength, 0.0, 1.0);
    const double fit = std::clamp(composition_fit, 0.0, 1.0);
    const double role = std::clamp(role_alignment, 0.0, 1.0);
    const double progress = std::clamp((static_cast<double>(stage) - 1.0) / 6.0, 0.0, 1.0);
    const double composition_weight = 0.70 - 0.55 * progress;
    const double immediate_weight = 1.0 - composition_weight;
    const double long_term_value = 0.70 * fit + 0.30 * role;
    return std::clamp(
        0.28 * (immediate_weight * immediate +
                composition_weight * long_term_value - 0.15),
        -0.03, 0.20);
}

// Crafting consumes two components and produces a completed item. Add only a
// small bonus when the resulting item has positive value for its recipient;
// the policy must still learn which item/target is best.
inline double item_crafting_bonus(double assignment_quality) noexcept {
    return assignment_quality > 0.0
        ? std::min(0.08, assignment_quality * 0.40)
        : 0.0;
}

inline double clamp_total(double total) noexcept {
    return std::clamp(total, -5.0, 5.0);
}

} // namespace tft::reward_model
