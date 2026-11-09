
#include "lobby/TraitMilestones.hpp"
#include "lobby/TftEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lobby/ChampionCatalog.hpp"
#include "lobby/UnitInstanceFactory.hpp"
#include "lobby/UnitPool.hpp"
#include "lobby/EconomyEngine.hpp"
#include "combat/CombatEngine.hpp"
#include "lobby/ShopEngine.hpp"
#include "strategy/SynergyCounter.hpp"
#include "strategy/PositioningEngine.hpp"
#include "strategy/BoardEvaluation.hpp"
#include "strategy/UnitCombatClassifier.hpp"
#include "reward/RewardModel.hpp"
#include "strategy/CompositionDirectionPolicy.hpp"
#include "strategy/StrategyPolicy.hpp"
#include "strategy/CompositionPlanner.hpp"

#include "decision/ActionMaskEngine.hpp"
#include "decision/DecisionState.hpp"
#include "decision/AntiLoopEngine.hpp"
#include "decision/MoveValidator.hpp"
#include "decision/DecisionTypes.hpp"
#include "items/ItemCatalog.hpp"
#include "items/ItemEngine.hpp"
#include "items/PveLootDistribution.hpp"
#include "items/CombatItemAdapter.hpp"

#ifndef TFT_DEFAULT_ITEM_CATALOG_PATH
#define TFT_DEFAULT_ITEM_CATALOG_PATH "data/itens.json"
#endif

#ifndef TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE
#define TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE 0.20
#endif
#ifndef TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE
#define TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE 0.75
#endif
#ifndef TFT_COMP_DIRECTION_REQUIRED_UPDATES
#define TFT_COMP_DIRECTION_REQUIRED_UPDATES 3
#endif

#ifndef TFT_DEFAULT_CATALOG_PATH
#define TFT_DEFAULT_CATALOG_PATH "data/units_gerado.json"
#endif

namespace tft {

namespace {

constexpr std::uint32_t kRoundsPerStage = 7;
constexpr std::size_t kCoreObservationDim = 22;
constexpr std::size_t kBoardObservationDim = kBoardSlots * 5;
constexpr std::size_t kBenchObservationDim = kBenchSlots * 5;
constexpr std::size_t kShopObservationDim = kShopSlots * 5;
constexpr std::size_t kExtraObservationDim = 5;
constexpr std::size_t kSynergyObservationDim = 5;
constexpr std::size_t kPositioningObservationDim = 4;
constexpr std::size_t kCapacityObservationDim = 2;
// Composition-aware features: direction and core-trait flags, plus fit scores
// for each board, bench, and shop slot.
constexpr std::size_t kCompositionUnitSlots =
    kBoardSlots + kBenchSlots + kShopSlots;
// Per-slot strategic features used by the policy's shared candidate scorers.
constexpr std::size_t kCandidateFeatureDim = 8;
constexpr std::size_t kStrategicCandidateObservationDim =
    (kBenchSlots + kShopSlots) * kCandidateFeatureDim;
// Pending-anvil state plus nine option records: ten item/component features
// and one strategic-fit score per option. Inserted before the candidate tail.
constexpr std::size_t kAnvilOptionCount = 9;
constexpr std::size_t kAnvilOptionFeatureDim = 11;
constexpr std::size_t kAnvilObservationDim = 3 + kAnvilOptionCount * kAnvilOptionFeatureDim;
// Semantic item-state block: component counts, bag item definitions, craftable
// recipe definitions, and per-unit target profiles. It is appended before the
// fixed strategic-candidate tail so legacy checkpoints can migrate by insertion.
constexpr std::size_t kItemSemanticFeatureDim = 17;
constexpr std::size_t kItemTargetFeatureDim = 18;
constexpr std::size_t kItemCompatibilityObservationDim =
    (9 + 100) * (kBoardSlots + kBenchSlots);
constexpr std::size_t kItemGlobalObservationDim =
    10 + 9 * kItemSemanticFeatureDim + 100 * kItemSemanticFeatureDim +
    (kBoardSlots + kBenchSlots) * kItemTargetFeatureDim;
constexpr std::size_t kItemStateObservationDim =
    kItemGlobalObservationDim + kItemCompatibilityObservationDim;
constexpr std::size_t kActionMemoryObservationDim = 1 + kBoardSlots;
constexpr std::uint32_t kActionMemoryHorizon = 10;

std::int32_t stage_damage(std::uint32_t stage) noexcept {
    switch (stage) {
        case 1: return 0;
        case 2: return 2;
        case 3: return 6;
        case 4: return 7;
        case 5: return 10;
        case 6: return 12;
        case 7: return 17;
        default: return 17;
    }
}

// Internal round index follows TFT's actual 1-based round sequence:
 // 1-1..1-4 are opening/PvE; each following stage has seven rounds.
std::uint32_t stage_for_round(std::uint32_t round_number) noexcept {
    if (round_number <= 4) return 1;
    return 2 + (round_number - 5) / 7;
}

bool is_pvp_round(std::uint32_t round_number) noexcept {
    if (round_number <= 4) return false;
    const auto round_in_stage = ((round_number - 5) % 7) + 1;
    return round_in_stage != 4 && round_in_stage != 7;
}

std::int32_t star_copies(std::int32_t star) noexcept {
    switch (star) {
        case 2: return 3;
        case 3: return 9;
        default: return 1;
    }
}

std::uint64_t matchup_key(std::size_t a, std::size_t b) noexcept {
    const auto low = static_cast<std::uint32_t>(std::min(a, b));
    const auto high = static_cast<std::uint32_t>(std::max(a, b));
    return (static_cast<std::uint64_t>(low) << 32U) | high;
}

// Máscaras provisórias utilizadas como fallback.
PlayerActionMasks make_default_masks() {
    PlayerActionMasks masks{};

    masks.type_mask[6] = 1;
    masks.shop_mask[0] = 1;
    masks.bench_source_mask[0] = 1;
    masks.board_target_mask[0] = 1;
    masks.board_source_mask[0] = 1;
    masks.bench_move_source_mask[0] = 1;
    masks.board_move_target_masks[0] = 1;
    masks.bench_to_board_target_masks[0] = 1;

    return masks;
}

float role_state_value(std::int32_t role) {
    switch (role) {
        case 0: return 0.2f; // tank
        case 1: return 0.4f; // carry
        case 2: return 0.6f; // support
        case 3: return 0.8f; // fighter
        case 4: return 1.0f; // assassin
        default: return 0.0f;
    }
}

std::array<float, kItemSemanticFeatureDim> encode_item_semantic_features(
    const items::ItemDefinition& item
) {
    return {
        static_cast<float>(std::clamp(item.attack_damage / 100.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.attack_speed / 2.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.ability_power / 100.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.critical_strike_chance, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.critical_strike_damage / 2.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.max_health / 2000.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.armor / 100.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.magic_resist / 100.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.omnivamp, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.damage_amplification, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.damage_reduction, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.mana / 100.0, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.carry_weight, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.tank_weight, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.support_weight, 0.0, 1.0)),
        static_cast<float>(std::clamp(item.utility_weight, 0.0, 1.0)),
        item.grants_trait.empty() ? 0.0f : 1.0f
    };
}

float unit_strength(const ChampionDefinition& def, const UnitInstance& unit) {
    const float cost = static_cast<float>(def.cost);
    const float star = static_cast<float>(unit.star);
    return std::min(1.0f, cost / 5.0f * 0.5f + star / 3.0f * 0.5f);
}

double unit_composition_score(
    const StrategyInfo& strategy, const ChampionDefinition& def, std::int32_t star,
    const std::unordered_map<std::string, std::vector<int>>& trait_milestones
) {
    // Strategic fit adapted from old/env.py::_compute_unit_comp_score().
    double score = 0.0;
    const bool core_unit = std::find(strategy.core_unit_ids.begin(), strategy.core_unit_ids.end(),
        def.internal_id) != strategy.core_unit_ids.end();
    const bool planned_unit = std::find(strategy.planned_unit_ids.begin(),
        strategy.planned_unit_ids.end(), def.internal_id) != strategy.planned_unit_ids.end();
    if (core_unit) score += 5.0;
    if (planned_unit) score += 4.5;
    const bool main_trait = !strategy.composition_direction.empty() &&
        std::find(def.traits.begin(), def.traits.end(), strategy.composition_direction) != def.traits.end();
    if (main_trait) score += 3.5;
    const auto active_count = [&](const std::string& trait) {
        const auto it = std::find_if(strategy.active_traits.begin(), strategy.active_traits.end(),
            [&](const auto& x) { return x.first == trait; });
        return it == strategy.active_traits.end() ? 0 : it->second;
    };
    const auto focus = [&](const std::string& trait) {
        const auto it = std::find_if(strategy.composition_focus_weights.begin(),
            strategy.composition_focus_weights.end(), [&](const auto& x) { return x.first == trait; });
        return it == strategy.composition_focus_weights.end() ? 0.0 : it->second;
    };
    for (const auto& trait : def.traits) {
        const bool core_trait = std::find(strategy.core_traits.begin(), strategy.core_traits.end(), trait)
            != strategy.core_traits.end();
        const bool planned_trait = std::find(strategy.planned_traits.begin(),
            strategy.planned_traits.end(), trait) != strategy.planned_traits.end();
        score += core_trait ? 1.2 : (planned_trait ? 1.0 : -0.5);
        const int count = active_count(trait);
        int next = 0;
        const auto ms = trait_milestones.find(trait);
        if (ms != trait_milestones.end()) {
            for (const int milestone : ms->second) if (milestone > count) { next = milestone; break; }
        } else if (count < 2) next = 2;
        if (next > 0) {
            const int needed = next - count;
            score += needed == 1 ? 5.0 : (needed == 2 ? 2.5 : 1.0);
        } else score += 0.5;
        const double f = focus(trait);
        score += f > 0.0 ? std::min(3.5, f * 0.45) : -0.1;
    }
    const auto copies = std::find_if(strategy.unit_counts_by_id.begin(), strategy.unit_counts_by_id.end(),
        [&](const auto& x) { return x.first == def.internal_id; });
    const int same = copies == strategy.unit_counts_by_id.end() ? 0 : copies->second;
    if (same >= 9) score += 6.0;
    else if (same >= 3) score += 4.0;
    else if (same >= 2) score += 2.0;
    score += (std::clamp(star, 1, 3) - 1) * 2.5;
    score += std::min(1.5, std::max(0, def.cost) * 0.25);
    // A planned bridge/support unit is allowed to sit outside the anchor
    // trait; otherwise a locked direction would punish flexible compositions.
    if (!main_trait && !planned_unit && !strategy.composition_direction.empty()) {
        score -= 1.2;
        if (strategy.composition_locked) score -= 2.0;
    }
    return score;
}

// Approximation of Python _compute_unit_board_impact(). This is deliberately a
// heuristic for reward shaping, not a replacement for simulated combat.
double unit_board_impact_score(
    const StrategyInfo& strategy, const ChampionDefinition& def, std::int32_t star,
    const std::unordered_map<std::string, std::vector<int>>& milestones,
    int board_count, int level, int stage
) {
    const auto trait_count = [&](const std::string& trait) {
        const auto it = std::find_if(strategy.active_traits.begin(), strategy.active_traits.end(),
            [&](const auto& x) { return x.first == trait; });
        return it == strategy.active_traits.end() ? 0 : it->second;
    };
    const auto focus_weight = [&](const std::string& trait) {
        const auto it = std::find_if(strategy.composition_focus_weights.begin(),
            strategy.composition_focus_weights.end(), [&](const auto& x) { return x.first == trait; });
        return it == strategy.composition_focus_weights.end() ? 0.0 : it->second;
    };
    const auto next_milestone = [&](const std::string& trait, int count) {
        const auto it = milestones.find(trait);
        if (it != milestones.end()) {
            for (const int milestone : it->second) if (milestone > count) return milestone;
        }
        return count < 2 ? 2 : 0;
    };

    const auto& profile = def.combat_profile;
    const double hp = profile.max_hp > 0.0 ? profile.max_hp : def.stats.max_hp;
    const double attack_speed = std::max(0.5, profile.attack_speed > 0.0
        ? profile.attack_speed : def.stats.attack_speed);
    const double attack_damage = profile.attack_damage > 0.0
        ? profile.attack_damage : def.stats.attack_damage;
    const double armor = std::max(0.0, profile.armor > 0.0 ? profile.armor : def.stats.armor);
    const double effective_dps = attack_damage * attack_speed * (100.0 / (100.0 + armor));
    const double star_multiplier = star >= 3 ? 9.0 : (star == 2 ? 3.0 : 1.0);
    const double combat_power = (hp / 1000.0) * (effective_dps / 50.0) *
        attack_speed * star_multiplier * std::pow(std::max(1, def.cost), 0.7);

    const double comp_score = unit_composition_score(strategy, def, star, milestones);
    double synergy_score = 0.0;
    double score = combat_power * 0.5 + comp_score * 0.25;
    for (const auto& trait : def.traits) {
        const int count = trait_count(trait);
        const int next = next_milestone(trait, count);
        const int before_level = [&]() {
            const auto it = milestones.find(trait);
            if (it == milestones.end()) return count >= 2 ? 1 : 0;
            int reached = 0;
            for (const int milestone : it->second) if (count >= milestone) ++reached;
            return reached;
        }();
        const int after_level = next > 0 && count + 1 >= next ? before_level + 1 : before_level;
        if (after_level > before_level) score += 1.5 + before_level * 0.3;
        synergy_score += std::max(0, count) * 0.15 + std::min(2.2, focus_weight(trait) * 0.12);
    }
    score += synergy_score * 0.6;

    const auto copies = std::find_if(strategy.unit_counts_by_id.begin(), strategy.unit_counts_by_id.end(),
        [&](const auto& x) { return x.first == def.internal_id; });
    const int owned_copies = copies == strategy.unit_counts_by_id.end() ? 0 : copies->second;
    if (owned_copies < 3) score += 1.2 + (owned_copies / 3.0) * 2.0;
    else if (owned_copies < 9) score += 3.5 + ((owned_copies - 3) / 6.0) * 5.5;
    else score += 0.5;
    if (board_count < level) score += 1.5;
    if (comp_score < 0.0 && synergy_score <= 0.0 && combat_power < 1.5) score -= 3.0;

    const bool main = !strategy.composition_direction.empty() &&
        std::find(def.traits.begin(), def.traits.end(), strategy.composition_direction) != def.traits.end();
    const bool secondary = std::any_of(def.traits.begin(), def.traits.end(), [&](const auto& trait) {
        return std::find(strategy.core_traits.begin(), strategy.core_traits.end(), trait) != strategy.core_traits.end();
    });
    if (main) score *= 1.5;
    else if (secondary) score *= 1.1;
    else score *= stage <= 3 ? 0.9 : 0.6;
    if (synergy_score <= 0.0 && combat_power < 1.5) score -= 2.5;
    return std::tanh(score / 5.0) * 4.0;
}

bool should_buy_unit(
    double impact, double upgrade_value, bool in_top_trait,
    int bench_count, int stage, int cost
) {
    // Early composition direction is still provisional. Avoid letting a
    // conservative comp-fit estimate suppress cheap pieces and pair progress.
    if (impact < (stage <= 2 ? -1.8 : -0.8) && upgrade_value == 0.0 &&
        !(stage <= 2 && bench_count <= 5 && cost <= 2)) return false;
    if (stage <= 2) {
        if (bench_count <= 5)
            return impact >= -1.0 || upgrade_value > 0.0 || in_top_trait || cost <= 2;
        if (bench_count <= 7)
            return upgrade_value > 0.0 || impact > -0.1 ||
                (cost <= 2 && impact > -1.0);
        return upgrade_value >= 0.6 || (upgrade_value >= 0.2 && in_top_trait);
    }
    if (bench_count <= 4) {
        return upgrade_value > 0.0 || impact > 0.3 || in_top_trait;
    }
    if (bench_count <= 6) {
        if (upgrade_value > 0.0 || impact > 0.6) return true;
        return cost >= 3 && in_top_trait && impact > 0.2;
    }
    // A nearly full bench should still allow a composition-relevant pair: the
    // marginal value is a future 2-star, not just the candidate's board impact.
    // At a completely full bench, require a stronger upgrade (third copy) or
    // exceptional immediate impact so the policy cannot fill itself into a corner.
    if (bench_count <= 8 && upgrade_value >= 0.2 && in_top_trait) return true;
    return upgrade_value >= 0.6 || impact > 1.2;
}

} // namespace

// ============================================================
// IMPLEMENTAÇÃO INTERNA
// ============================================================

struct TFTEngine::Impl {
    struct PlayerTransition {
        float action_reward{0.0f};
        bool acted{false};
        bool board_capacity_violation{false};
        std::int32_t action_cost{0};
        std::int32_t sale_proceeds{0};
        std::int32_t gold_before_action{0};
        std::int32_t gold_after_action{0};
        std::int32_t interest_before_action{0};
        std::int32_t interest_after_action{0};
        std::int32_t level_before_action{0};
        std::int32_t level_after_action{0};
        std::int32_t xp_before_action{0};
        std::int32_t xp_after_action{0};
    };

    struct PlayerEnv {
        // Estado e serviços pertencentes exclusivamente a um jogador.
        std::unique_ptr<ShopEngine> shop;
        std::unique_ptr<items::ItemEngine> item_engine;
        items::Set18ComponentPool pve_component_pool;
        std::array<std::optional<UnitInstance>, kBenchSlots> bench{};
        std::array<std::optional<UnitInstance>, kBoardSlots> board{};
        // Temporal context for the feed-forward policy: actions left this round
        // and age (in actions) of the unit occupying each board slot.
        std::array<std::uint32_t, kBoardSlots> board_move_age{};
        std::uint32_t actions_taken_this_round{0};
        decision::AntiLoopEngine anti_loop;
        PlayerInfo info{};
        PlayerActionMasks action_masks{};
        std::vector<StrategyInfo> strategy;
        AntiLoopMetrics anti_loop_metrics{};
        PlayerEconomy economy{};

        double previous_board_power{0.0};
        double previous_synergy_score{0.0};
        double previous_positioning_score{0.0};
        std::int32_t previous_reward_level{1};
        double last_combat_reward{0.0};
        double round_reward_total{0.0};
        double total_reward{0.0};
        bool placement_reward_given{false};
        // Consecutive successful board movements are shaped as a telescoping
        // potential delta. Reset at every round boundary and non-movement action.
        std::uint32_t movement_sequence_length{0};
        std::uint32_t movement_sequence_round{0};
        bool movement_sequence_round_initialized{false};
        struct RecentPurchase {
            std::string uuid;
            std::int32_t champion_id{-1};
            std::int32_t star{1};
            std::uint64_t action_serial{0};
            int cost{0};
            bool built_pair{false};
            bool completed_upgrade{false};
            bool was_deployed{false};
            double fit_score{0.0};
            double reward{0.0};
            std::string comp_direction;
            std::string comp_focus;
        };
        std::uint64_t action_serial{0};
        std::vector<RecentPurchase> recent_purchases;
        struct ItemActionAudit {
            std::string kind;
            std::string item_id;
            std::string first_component;
            std::string second_component;
            std::string target_uuid;
            std::int32_t target_slot{-1};
            bool target_on_board{false};
            double fit{0.0};
            double immediate_strength{0.0};
            double role_alignment{0.0};
            double assignment_reward{0.0};
        };
        ItemActionAudit last_item_action{};

        PlayerTransition step(
            TFTEngine::Impl& lobby,
            std::size_t player_id,
            const PlayerAction& player_action
        );
    };
    std::uint64_t seed{0};
    std::uint64_t episode_id{0};
    std::mt19937_64 matchmaking_rng{0};
    std::set<std::uint64_t> previous_matchups;

    std::uint32_t round{0};
    std::uint32_t stage{1};

    std::optional<std::int32_t> winner_id;
    std::vector<std::int32_t> ranking;

    std::unique_ptr<ChampionCatalog> catalog;
    std::optional<items::ItemCatalog> item_catalog;
    // Catalog-derived recipe observations are immutable for this engine
    // instance and shared by every player's observation.
    std::array<std::array<float, kItemSemanticFeatureDim>, 100>
        recipe_observation_features{};
    std::array<std::string, 100> recipe_observation_item_ids{};
    std::unique_ptr<UnitPool> pool;
    std::unique_ptr<UnitInstanceFactory> unit_factory;

    // Board evaluation is read by observations, rewards and viewer snapshots.
    // The signature covers every mutable input used by the evaluator.
    mutable std::array<strategy::BoardEvaluationCache, kPlayersPerLobby>
        board_evaluation_cache{};

    // Uma instância C++ por jogador; o lobby coordena as oito.
    std::array<PlayerEnv, kPlayersPerLobby> player_envs{};

    // Serviços compartilhados pela lobby, não pertencem a um jogador.
    EconomyEngine economy_engine;
    CombatEngine combat_engine;
    decision::ActionMaskEngine action_mask_engine;

    std::int32_t alive_players{
        static_cast<std::int32_t>(kPlayersPerLobby)
    };

    bool done{false};
    bool closed{false};

    // No treinamento, cada jogador recebe 15 ações por rodada; PASS também conta como ação.
    // São cinco ações adicionais para permitir mais compras, XP, rerolls e gestão de itens.
    // O viewer usa o temporizador de parede e não aplica este limite de ações.
    static constexpr std::uint32_t kMaxActionsPerRound = 15;
    std::array<std::uint32_t, kPlayersPerLobby> round_actions{};
    std::array<bool, kPlayersPerLobby> round_finished{};

    std::unordered_map<std::string, std::vector<int>> trait_milestones;
    // Same insertion order as UnitManager.get_all_champions() -> traits.
    std::vector<std::string> observation_trait_names;

    explicit Impl(
        std::uint64_t initial_seed,
        const std::string& catalog_path,
        const std::string& item_catalog_path
    ) : seed(initial_seed),
        matchmaking_rng(initial_seed) {
        const std::string resolved_path =
            catalog_path.empty()
                ? TFT_DEFAULT_CATALOG_PATH
                : catalog_path;

        catalog = std::make_unique<ChampionCatalog>(
            resolved_path
        );

        // The generated, versioned active-set catalog is the default. An
        // explicit path still supports controlled fixtures and set-specific runs.
        const std::string resolved_item_catalog_path =
            item_catalog_path.empty()
                ? TFT_DEFAULT_ITEM_CATALOG_PATH
                : item_catalog_path;
        if (!resolved_item_catalog_path.empty()) {
            std::ifstream item_file(resolved_item_catalog_path);
            if (!item_file) {
                throw std::runtime_error(
                    "Nao foi possivel abrir catalogo de itens: " +
                    resolved_item_catalog_path
                );
            }
            item_catalog = items::load_item_catalog(item_file);
        }

        // Recipes and their encoded attributes depend only on the loaded item
        // catalog. Build the exact same symmetric 10x10 mapping once, rather
        // than repeating catalog searches and feature encoding per player and
        // observation. Iteration order intentionally matches the old path.
        if (item_catalog) {
            for (const auto& definition : item_catalog->items) {
                if (definition.recipe_components[0].empty() ||
                    definition.recipe_components[1].empty()) continue;
                const auto first = std::find_if(item_catalog->components.begin(),
                    item_catalog->components.end(), [&](const auto& entry) {
                        return entry.first == definition.recipe_components[0];
                    });
                const auto second = std::find_if(item_catalog->components.begin(),
                    item_catalog->components.end(), [&](const auto& entry) {
                        return entry.first == definition.recipe_components[1];
                    });
                if (first == item_catalog->components.end() ||
                    second == item_catalog->components.end()) continue;
                const auto a = static_cast<std::size_t>(
                    std::distance(item_catalog->components.begin(), first));
                const auto b = static_cast<std::size_t>(
                    std::distance(item_catalog->components.begin(), second));
                if (a >= 10 || b >= 10) continue;
                const auto encoded = encode_item_semantic_features(definition);
                recipe_observation_features[a * 10 + b] = encoded;
                recipe_observation_features[b * 10 + a] = encoded;
                recipe_observation_item_ids[a * 10 + b] = definition.id;
                recipe_observation_item_ids[b * 10 + a] = definition.id;
            }
        }

        for (const auto& champion : catalog->all()) {
            for (const auto& trait : champion.traits) {
                if (std::find(observation_trait_names.begin(),
                              observation_trait_names.end(), trait)
                    == observation_trait_names.end()) {
                    observation_trait_names.push_back(trait);
                }
            }
        }

        // Load actual active-set milestones. Prefer traits.json beside the
        // selected unit catalog; custom/test catalogs may use the repository
        // default. A present but malformed file is an error, never a silent
        // fallback to generic 2-unit milestones.
        const auto separator = resolved_path.find_last_of("/\\");
        const auto directory = separator == std::string::npos
            ? std::string{}
            : resolved_path.substr(0, separator + 1);
        std::string traits_path = directory + "traits.json";
        std::ifstream traits_file(traits_path);
#ifdef TFT_DEFAULT_TRAITS_PATH
        if (!traits_file) {
            traits_path = TFT_DEFAULT_TRAITS_PATH;
            traits_file.clear();
            traits_file.open(traits_path);
        }
#endif
        if (!traits_file) {
            throw std::runtime_error("Nao foi possivel abrir catalogo de traits: " + traits_path);
        }
        trait_milestones = load_trait_milestones(traits_file);

        pool = std::make_unique<UnitPool>(*catalog);

        unit_factory = std::make_unique<UnitInstanceFactory>(
            *catalog,
            seed
        );

        initialize_players();
    }

    void ensure_open() const {
        if (closed) {
            throw std::runtime_error(
                "TFTEngine ja foi fechado."
            );
        }
    }

    // ========================================================
    // ESTADO DE DECISÃO E SNAPSHOTS
    // ========================================================

    decision::PlayerState build_decision_state(
        std::size_t player_index
    ) const {
        decision::PlayerState state;

        state.player_id =
            static_cast<std::int64_t>(player_index);

        state.is_alive = player_envs[player_index].info.alive;

        const auto& economy = player_envs[player_index].economy;

        state.economy.gold = economy.gold;
        state.economy.level = economy.level;
        state.economy.max_level = EconomyEngine::MAX_LEVEL;
        state.economy.current_xp = economy.xp;
        state.economy.xp_to_next_level =
            economy_engine.get_xp_to_next(economy);
        state.economy.reroll_cost =
            EconomyEngine::REROLL_COST;
        state.economy.xp_purchase_cost =
            EconomyEngine::BUY_XP_COST;

        const auto snapshot = [this](
            const std::optional<UnitInstance>& instance
        ) -> std::optional<decision::UnitSnapshot> {
            if (!instance.has_value()) {
                return std::nullopt;
            }

            decision::UnitSnapshot unit;

            unit.identity.uuid = instance->uuid;
            unit.identity.id = instance->champion_internal_id;
            unit.identity.textual_id = instance->champion_id;

            unit.identity.star = static_cast<std::uint8_t>(
                std::clamp(instance->star, 1, 3)
            );

            const auto* definition =
                catalog->find_by_id(
                    instance->champion_internal_id
                );

            unit.cost = static_cast<std::uint8_t>(
                definition
                    ? std::clamp(definition->cost, 1, 5)
                    : 1
            );

            unit.available = true;

            return unit;
        };

        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            state.board[slot] =
                snapshot(player_envs[player_index].board[slot]);
        }

        for (std::size_t slot = 0; slot < kBenchSlots; ++slot) {
            state.bench[slot] =
                snapshot(player_envs[player_index].bench[slot]);
        }

        for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
            const auto* offer =
                player_envs[player_index].shop->get_unit(
                    static_cast<std::int32_t>(slot)
                );

            if (offer) {
                state.shop[slot] = snapshot(
                    std::optional<UnitInstance>{*offer}
                );
            }
        }

        return state;
    }

    void register_position_state(std::size_t player_index) {
        const auto state =
            build_decision_state(player_index);

        const auto signature =
            player_envs[player_index].anti_loop
                .make_board_signature(state);

        (void)player_envs[player_index].anti_loop
            .register_state(signature);
    }

    // ========================================================
    // MÁSCARAS DE AÇÃO
    // ========================================================

    std::vector<items::UnitProfile> item_unit_profiles(std::size_t player_index) const {
        std::vector<items::UnitProfile> profiles;
        profiles.reserve(kBoardSlots + kBenchSlots);
        const auto& player = player_envs[player_index];
        const auto append = [&](const UnitInstance& unit, bool frontline, bool backline) {
            const auto* def = catalog->find_by_id(unit.champion_internal_id);
            if (!def || unit.uuid.empty()) return;
            const auto& stats = def->stats;
            const auto& combat = def->combat_profile;
            const auto strategy = player.strategy.empty() ? StrategyInfo{} : player.strategy.back();
            const double fit = unit_composition_score(strategy, *def, unit.star, trait_milestones);
            std::vector<std::string> preferred_traits = strategy.core_traits;
            preferred_traits.insert(preferred_traits.end(),
                strategy.planned_traits.begin(), strategy.planned_traits.end());
            if (!strategy.composition_direction.empty())
                preferred_traits.push_back(strategy.composition_direction);
            for (const auto& [trait, weight] : strategy.composition_focus_weights) {
                if (weight > 0.1) preferred_traits.push_back(trait);
            }
            for (const auto& [trait, count] : strategy.active_traits) {
                if (count > 0) preferred_traits.push_back(trait);
            }
            std::sort(preferred_traits.begin(), preferred_traits.end());
            preferred_traits.erase(
                std::unique(preferred_traits.begin(), preferred_traits.end()),
                preferred_traits.end());
            profiles.push_back(items::UnitProfile{
                unit.uuid, unit.star, std::clamp(combat.role, 0, 4),
                frontline, backline,
                combat.attack_damage > 0.0 ? combat.attack_damage : stats.attack_damage,
                combat.attack_speed > 0.0 ? combat.attack_speed : stats.attack_speed,
                0.0,
                combat.max_hp > 0.0 ? combat.max_hp : stats.max_hp,
                combat.armor > 0.0 ? combat.armor : stats.armor,
                combat.magic_resist > 0.0 ? combat.magic_resist : stats.magic_resist,
                std::clamp(fit / 32.5, 0.0, 1.0),
                def->traits,
                std::move(preferred_traits)
            });
        };
        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            if (!player.board[slot]) continue;
            const auto row = slot / 7;
            append(*player.board[slot], row <= 1, row >= 2);
        }
        for (const auto& unit : player.bench) if (unit) append(*unit, false, false);
        return profiles;
    }

    const UnitInstance* item_target_unit(std::size_t player_index, std::size_t target_index) const {
        if (target_index < kBoardSlots) {
            const auto& unit = player_envs[player_index].board[target_index];
            return unit ? &*unit : nullptr;
        }
        const auto bench_index = target_index - kBoardSlots;
        if (bench_index >= kBenchSlots) return nullptr;
        const auto& unit = player_envs[player_index].bench[bench_index];
        return unit ? &*unit : nullptr;
    }

    UnitInstance* item_target_unit(std::size_t player_index, std::size_t target_index) {
        if (target_index < kBoardSlots) {
            auto& unit = player_envs[player_index].board[target_index];
            return unit ? &*unit : nullptr;
        }
        const auto bench_index = target_index - kBoardSlots;
        if (bench_index >= kBenchSlots) return nullptr;
        auto& unit = player_envs[player_index].bench[bench_index];
        return unit ? &*unit : nullptr;
    }

    void grant_round_pve_item_rewards(
        std::uint32_t current_round_number,
        std::vector<std::string>& events
    ) {
        if (!item_catalog || item_catalog->pve_loot_components.size() != 8 || current_round_number == 0)
            return;

        const auto current_stage = stage_for_round(current_round_number);
        const auto round_in_stage = current_round_number <= 4
            ? current_round_number
            : ((current_round_number - 5) % kRoundsPerStage) + 1;

        // Stages 1-4 resolve their weighted PvE package directly into
        // components. No orb entities or orb telemetry are created.
        const bool component_reward_round =
            (current_stage == 1 && current_round_number == 4) ||
            (current_stage >= 2 && current_stage <= 4 && round_in_stage == 7);

        // From stage 5 onward, the PvE boss reward is a direct completed item.
        const bool completed_item_reward_round =
            current_stage >= 5 && round_in_stage == 7;
        if (!component_reward_round && !completed_item_reward_round) return;

        std::vector<std::size_t> living;
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (player_envs[i].info.alive && player_envs[i].item_engine)
                living.push_back(i);
        }
        if (living.empty()) return;

        const auto mixed_seed = seed ^
            (episode_id * 0xD1B54A32D192ED03ULL) ^
            (static_cast<std::uint64_t>(current_round_number) << 16U) ^
            0x53455431385F4C54ULL;

        if (component_reward_round) {
            std::vector<std::string> component_ids;
            component_ids = item_catalog->pve_loot_components;

            // All players share the weighted component-count roll; component
            // identities are drawn independently from each player's pool.
            std::mt19937_64 outcome_rng(mixed_seed);
            const auto outcome = items::roll_set18_pve_loot(current_stage, outcome_rng);
            events.emplace_back(
                "PVE_COMPONENT_REWARD_ROLLED: stage=" + std::to_string(current_stage) +
                ", round=" + std::to_string(current_round_number) +
                ", components=" + std::to_string(outcome.components) +
                ", source=set18_weighted_table");

            for (const auto player_index : living) {
                auto& player = player_envs[player_index];
                auto& pool = player.pve_component_pool;
                if (current_stage == 1) {
                    if (pool.empty()) pool.initialize(component_ids);
                } else {
                    if (pool.empty()) pool.initialize(component_ids);
                    pool.add_stage_refresh(component_ids);
                }

                auto component_rng = std::mt19937_64(
                    mixed_seed ^ (static_cast<std::uint64_t>(player_index) *
                                  0x9E3779B97F4A7C15ULL));
                std::size_t granted_count = 0;
                for (unsigned drop = 0; drop < outcome.components && !pool.empty(); ++drop) {
                    const auto component_id = pool.draw(component_rng);
                    if (!component_id.empty() && player.item_engine->add_component(component_id))
                        ++granted_count;
                }
                if (granted_count > 0) {
                    events.emplace_back(
                        "COMPONENT_LOOT_GRANTED: player=" + std::to_string(player_index) +
                        ", stage=" + std::to_string(current_stage) +
                        ", count=" + std::to_string(granted_count) +
                        ", pool_remaining=" + std::to_string(pool.remaining()) +
                        ", source=set18_pve_component_pool");
                }
                refresh_masks(player_index);
            }
            return;
        }

        std::vector<std::string> item_pool;
        item_pool.reserve(item_catalog->items.size());
        for (const auto& item : item_catalog->items)
            if (item.unit_equipable) item_pool.push_back(item.id);
        if (item_pool.empty()) return;

        for (const auto player_index : living) {
            auto& player = player_envs[player_index];
            auto loot_rng = std::mt19937_64(
                mixed_seed ^ (static_cast<std::uint64_t>(player_index) *
                              0x9E3779B97F4A7C15ULL));
            std::uniform_int_distribution<std::size_t> pick(0, item_pool.size() - 1);
            const auto& item_id = item_pool[pick(loot_rng)];
            if (player.item_engine->add_completed_item(item_id)) {
                events.emplace_back(
                    "COMPLETED_ITEM_GRANTED: player=" + std::to_string(player_index) +
                    ", stage=" + std::to_string(current_stage) +
                    ", item=" + item_id +
                    ", source=set18_pve_boss_reward");
            }
            refresh_masks(player_index);
        }
    }
    void sync_unit_items_from_engine(std::size_t player_index, const std::string& uuid) {
        auto& player = player_envs[player_index];
        if (!player.item_engine || uuid.empty()) return;
        UnitInstance* instance = nullptr;
        for (auto& unit : player.board) if (unit && unit->uuid == uuid) { instance = &*unit; break; }
        if (!instance) for (auto& unit : player.bench) if (unit && unit->uuid == uuid) { instance = &*unit; break; }
        if (!instance) return;
        instance->items.clear();
        for (const auto& item_id : player.item_engine->equipped(uuid)) {
            const auto* definition = player.item_engine->find(item_id);
            if (definition) instance->items.push_back(items::to_combat_item(*definition));
        }
    }

    void refresh_masks(std::size_t player_index) {
        const auto state =
            build_decision_state(player_index);

        const auto batch =
            action_mask_engine.generate_masks(
                state,
                player_envs[player_index].anti_loop,
                false
            );

        // Contabiliza uma vez por geração de máscara; as consultas individuais
        // do anti-loop permanecem sem efeitos colaterais.
        player_envs[player_index].anti_loop.record_mask_diagnostics(
            batch.diagnostics
        );

        auto& current = player_envs[player_index].action_masks;
        current = PlayerActionMasks{};

        // Keep every legal action available. Reroll-vs-XP is learned
        // through strategy-aware rewards rather than hard action suppression.
        current.type_mask = batch.type_mask;
        current.shop_mask = batch.shop_mask;
        current.bench_source_mask =
            batch.bench_source_mask;
        current.board_target_mask =
            batch.board_target_mask;
        current.board_source_mask =
            batch.board_source_mask;
        current.bench_move_source_mask =
            batch.bench_move_source_mask;

        // Item masks are generated from the exact per-player ItemEngine state.
        auto& player = player_envs[player_index];
        if (player.item_engine) {
            const auto& inventory = player.item_engine->inventory();
            for (std::size_t i = 0; i < std::min<std::size_t>(9, inventory.size()); ++i)
                current.item_inventory_mask[i] = 1;
            for (std::size_t target = 0; target < kBoardSlots + kBenchSlots; ++target) {
                const auto* unit = item_target_unit(player_index, target);
                if (!unit || std::max(unit->items.size(), player.item_engine->equipped(unit->uuid).size()) >= items::ItemEngine::kMaxItemsPerUnit)
                    continue;
                current.item_target_mask[target] = 1;
            }
            const bool target_available = std::any_of(current.item_target_mask.begin(), current.item_target_mask.end(),
                [](std::uint8_t value) { return value == 1; });
            if (item_catalog) {
                for (std::size_t first = 0; first < std::min<std::size_t>(10, item_catalog->components.size()); ++first) {
                    for (std::size_t second = 0; second < std::min<std::size_t>(10, item_catalog->components.size()); ++second) {
                        if (!player.item_engine->can_craft(
                                item_catalog->components[first].first,
                                item_catalog->components[second].first)) continue;
                        current.item_component_pair_masks[first * 10 + second] = 1;
                        current.item_component_first_mask[first] = 1;
                    }
                }
            }
            const bool inventory_available = std::any_of(current.item_inventory_mask.begin(), current.item_inventory_mask.end(),
                [](std::uint8_t value) { return value == 1; });
            const bool recipe_available = std::any_of(current.item_component_pair_masks.begin(), current.item_component_pair_masks.end(),
                [](std::uint8_t value) { return value == 1; });
            current.type_mask[7] = inventory_available && target_available ? 1 : 0;
            current.type_mask[8] = recipe_available && target_available ? 1 : 0;
            // Selling a bench unit is legal even when it carries equipment.
            // The equipped items are returned to the shared item inventory as
            // part of the sale transition; do not reuse the move-source mask
            // to forbid selling equipped units.
            for (std::size_t shop_slot = 0; shop_slot < kShopSlots; ++shop_slot) {
                if (current.shop_mask[shop_slot] != 1 || !player.shop) continue;
                const auto* offer = player.shop->get_unit(static_cast<std::int32_t>(shop_slot));
                if (!offer) continue;
                // Keep upgrades legal even when matching copies carry items.
                // Merge transfers equipment and returns overflow to inventory.
            }
            if (std::none_of(current.shop_mask.begin(), current.shop_mask.end(),
                [](std::uint8_t value) { return value == 1; })) current.type_mask[0] = 0;

            // Opening an anvil suspends every other action until exactly one
            // offered option is selected. Reuse the existing item-choice head
            // to keep the six-component PPO action contract stable.
            if (const auto* anvil = player.item_engine->pending_anvil()) {
                current.type_mask.fill(0);
                current.type_mask[7] = 1;
                current.item_inventory_mask.fill(0);
                for (std::size_t option = 0;
                     option < std::min(kAnvilOptionCount, anvil->options.size()); ++option)
                    current.item_inventory_mask[option] = 1;
                current.item_target_mask.fill(0);
                current.item_target_mask[0] = 1; // degenerate target head: no extra choice
                current.item_component_first_mask.fill(0);
                current.item_component_pair_masks.fill(0);
            }

        }

        for (std::size_t source = 0;
             source < kBoardSlots;
             ++source) {
            for (std::size_t target = 0;
                 target < kBoardSlots;
                 ++target) {
                current.board_move_target_masks[
                    source * kBoardSlots + target
                ] = batch.board_move_target_masks[source][target];
            }
        }

        for (std::size_t source = 0;
             source < kBenchSlots;
             ++source) {
            for (std::size_t target = 0;
                 target < kBoardSlots;
                 ++target) {
                current.bench_to_board_target_masks[
                    source * kBoardSlots + target
                ] = batch.bench_to_board_target_masks[source][target];
            }
        }
    }

    // ========================================================
    // INICIALIZAÇÃO
    // ========================================================

    void initialize_players() {
        alive_players =
            static_cast<std::int32_t>(kPlayersPerLobby);

        // RewardManager.py starts with previous_level = 1.
        // Initialize this on first construction as well as on reset.
        for (auto& player_env : player_envs) player_env.previous_reward_level = 1;
        round_actions.fill(0);
        round_finished.fill(false);
        previous_matchups.clear();
        matchmaking_rng.seed(seed + episode_id);

        // Python creates one random cost-1 starter on each player's bench
        // before rolling that player's shop. Keep the same state transition
        // and pool accounting in the native lobby.
        std::mt19937_64 starting_rng(seed + episode_id);
        for (std::size_t i = 0;
             i < kPlayersPerLobby;
             ++i) {
            PlayerInfo player{};

            player.player_id =
                static_cast<std::int32_t>(i);

            player.hp = 100.0f;

            economy_engine.reset(player_envs[i].economy);

            player.gold = player_envs[i].economy.gold;
            player.level = player_envs[i].economy.level;
            player.xp = player_envs[i].economy.xp;

            player.alive = true;
            player.eliminated = false;

            player.wins = 0;
            player.losses = 0;

            player.placement = std::nullopt;
            player.opponent_id = std::nullopt;
            player.damage_taken = 0.0f;

            player_envs[i].info = player;

            player_envs[i].bench.fill(std::nullopt);
            player_envs[i].board.fill(std::nullopt);
            player_envs[i].pve_component_pool = items::Set18ComponentPool{};
            if (item_catalog) {
                std::vector<std::string> component_ids;
                component_ids.reserve(item_catalog->components.size());
                for (const auto& component : item_catalog->components) component_ids.push_back(component.first);
                if (!player_envs[i].item_engine)
                    player_envs[i].item_engine = std::make_unique<items::ItemEngine>(item_catalog->items, component_ids);
                else player_envs[i].item_engine->reset();
            } else {
                player_envs[i].item_engine.reset();
            }
            player_envs[i].board_move_age.fill(kActionMemoryHorizon);
            player_envs[i].actions_taken_this_round = 0;
            player_envs[i].action_serial = 0;
            player_envs[i].recent_purchases.clear();

            // Impede que o histórico de uma partida anterior
            // interfira no novo episódio.
            // Limpa o histórico da partida, preservando as métricas acumuladas.
            player_envs[i].anti_loop.reset();

            const auto starting_champion = pool->draw_by_cost(1, starting_rng);
            if (!starting_champion.has_value()) {
                throw std::runtime_error(
                    "UnitPool sem campeao de custo 1 para a unidade inicial.");
            }
            player_envs[i].bench[0] = unit_factory->create(*starting_champion);

            player_envs[i].shop = std::make_unique<ShopEngine>(
                *pool,
                *unit_factory,
                seed + static_cast<std::uint64_t>(i)
                    + episode_id
            );

            player_envs[i].shop->roll(player.level);

            player_envs[i].action_masks = make_default_masks();

            player_envs[i].strategy.clear();
            player_envs[i].strategy.push_back(StrategyInfo{});

            player_envs[i].anti_loop_metrics = AntiLoopMetrics{};

            register_position_state(i);
            refresh_masks(i);
        }
    }

    // ========================================================
    // OBSERVAÇÕES
    // ========================================================

    std::size_t observation_dim() const noexcept {
        return kCoreObservationDim + kBoardObservationDim +
            kBenchObservationDim + kShopObservationDim +
            kExtraObservationDim + kSynergyObservationDim +
            kPositioningObservationDim + observation_trait_names.size() +
            kCapacityObservationDim +
            2 * observation_trait_names.size() + kCompositionUnitSlots +
            kActionMemoryObservationDim +
            observation_trait_names.size() + 2 +
            kAnvilObservationDim + kItemStateObservationDim +
            kStrategicCandidateObservationDim;
    }

    std::vector<float> build_observations() const {
        const auto dim = observation_dim();
        std::vector<float> observations(kPlayersPerLobby * dim, 0.0f);

        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            float* out = observations.data() + i * dim;
            const auto& player = player_envs[i].info;

            // TFTLobby.get_observations() returns an all-zero vector for
            // eliminated players. Preserve that public PPO contract exactly.
            if (!player.alive) {
                continue;
            }

            const auto& economy = player_envs[i].economy;
            const auto& strategy = player_envs[i].strategy.empty()
                ? StrategyInfo{} : player_envs[i].strategy.back();

            std::array<int, kBoardSlots> board_roles{};
            std::array<bool, kBoardSlots> board_occupied{};
            std::array<bool, kBenchSlots> bench_occupied{};
            std::vector<std::pair<std::string, int>> trait_counts;
            std::unordered_map<std::string, std::size_t> trait_count_index;
            // Bound the common case to one allocation/rehash-free accumulation
            // pass while retaining trait_counts insertion order for observations.
            trait_counts.reserve(observation_trait_names.size());
            trait_count_index.reserve(observation_trait_names.size());
            int board_count = 0, bench_count = 0, frontline = 0, backline = 0;
            std::array<bool, 5> seen_roles{};
            std::size_t unique_role_count = 0;

            const auto count_traits = [&](const UnitInstance& unit) {
                const auto* def = catalog->find_by_id(unit.champion_internal_id);
                if (!def) return;
                for (const auto& trait : def->traits) {
                    const auto [it, inserted] =
                        trait_count_index.try_emplace(trait, trait_counts.size());
                    if (inserted) trait_counts.emplace_back(trait, 0);
                    ++trait_counts[it->second].second;
                }
            };
            // Match the exact combat-role classifier used by BoardEvaluation.
            // Otherwise PPO observes a catalog role while the reward evaluator
            // scores the same unit as a carry/tank, creating contradictory labels.
            const auto observation_role = [&](const UnitInstance& unit,
                                               const ChampionDefinition& def) {
                tft::strategy::UnitCombatSignals signals;
                signals.base_role = def.combat_profile.role;
                signals.attack_range = def.stats.attack_range > 0.0
                    ? def.stats.attack_range : def.combat_profile.attack_range;
                signals.max_health = def.stats.max_hp;
                signals.armor = def.stats.armor;
                signals.magic_resist = def.stats.magic_resist;
                signals.attack_damage = def.stats.attack_damage;
                signals.attack_speed = def.stats.attack_speed;
                signals.ability_text = def.ability.description + " " + def.ability.name;
                signals.planned_main_carry =
                    unit.champion_internal_id == strategy.planned_main_carry_id;
                for (const auto& item : unit.items) {
                    signals.item_health += item.health;
                    signals.item_armor += item.armor;
                    signals.item_magic_resist += item.magic_resist;
                    signals.item_attack_damage += item.attack_damage;
                    signals.item_attack_speed += item.attack_speed;
                    signals.item_ability_power += item.ability_power;
                    signals.item_crit += item.critical_strike_chance;
                }
                return tft::strategy::UnitCombatClassifier::classify(signals).positioning_role;
            };

            for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
                if (!player_envs[i].board[slot]) continue;
                ++board_count;
                board_occupied[slot] = true;
                const auto& unit = *player_envs[i].board[slot];
                const auto* def = catalog->find_by_id(unit.champion_internal_id);
                if (!def) continue;
                const int role = std::clamp(observation_role(unit, *def), 0, 4);
                board_roles[slot] = role;
                const auto role_index = static_cast<std::size_t>(role);
                if (!seen_roles[role_index]) {
                    seen_roles[role_index] = true;
                    ++unique_role_count;
                }
                const int row = static_cast<int>(slot / 7);
                if (row <= 1) ++frontline;
                if (row >= 2) ++backline;
                count_traits(unit);
            }
            for (std::size_t slot = 0; slot < kBenchSlots; ++slot) {
                if (!player_envs[i].bench[slot]) continue;
                ++bench_count;
                bench_occupied[slot] = true;
                count_traits(*player_envs[i].bench[slot]);
            }

            // 1) CORE (same order/scales as MiniTFTEnv._get_state()).
            std::size_t offset = 0;
            out[offset++] = player.hp / 100.0f;
            out[offset++] = static_cast<float>(economy.gold) / 50.0f;
            out[offset++] = static_cast<float>(economy.level) / 9.0f;
            out[offset++] = static_cast<float>(economy.xp) /
                static_cast<float>(std::max(1, economy_engine.get_xp_to_next(economy)));
            out[offset++] = static_cast<float>(economy.win_streak) / 10.0f;
            out[offset++] = static_cast<float>(economy.loss_streak) / 10.0f;
            const float current_round = static_cast<float>(round + 1);
            out[offset++] = current_round / (current_round + 10.0f);
            out[offset++] = static_cast<float>(board_count) /
                static_cast<float>(std::max(1, economy.level));
            out[offset++] = static_cast<float>(bench_count) / static_cast<float>(kBenchSlots);
            out[offset++] = 1.0f; // shop vector always contains five slots in Python.
            for (int s = 0; s < 7; ++s)
                out[offset++] = strategy.strategy == s ? 1.0f : 0.0f;
            // Python's 22-wide core reserves five trailing zero features.
            offset = kCoreObservationDim;

            // 2) BOARD: occupied, cost, stars, role, strength per slot.
            for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
                if (!player_envs[i].board[slot]) { offset += 5; continue; }
                const auto& unit = *player_envs[i].board[slot];
                const auto* def = catalog->find_by_id(unit.champion_internal_id);
                if (!def) { offset += 5; continue; }
                out[offset++] = 1.0f;
                out[offset++] = static_cast<float>(def->cost) / 5.0f;
                out[offset++] = static_cast<float>(unit.star) / 3.0f;
                out[offset++] = role_state_value(observation_role(unit, *def));
                out[offset++] = unit_strength(*def, unit);
            }

            // 3) BENCH.
            for (std::size_t slot = 0; slot < kBenchSlots; ++slot) {
                if (!player_envs[i].bench[slot]) { offset += 5; continue; }
                const auto& unit = *player_envs[i].bench[slot];
                const auto* def = catalog->find_by_id(unit.champion_internal_id);
                if (!def) { offset += 5; continue; }
                out[offset++] = 1.0f;
                out[offset++] = static_cast<float>(def->cost) / 5.0f;
                out[offset++] = static_cast<float>(unit.star) / 3.0f;
                out[offset++] = role_state_value(observation_role(unit, *def));
                out[offset++] = unit_strength(*def, unit);
            }

            // 4) SHOP.
            for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
                const auto* unit = player_envs[i].shop ? player_envs[i].shop->get_unit(static_cast<std::int32_t>(slot)) : nullptr;
                if (!unit) { offset += 5; continue; }
                const auto* def = catalog->find_by_id(unit->champion_internal_id);
                if (!def) { offset += 5; continue; }
                out[offset++] = 1.0f;
                out[offset++] = static_cast<float>(def->cost) / 5.0f;
                out[offset++] = 1.0f;
                out[offset++] = role_state_value(observation_role(*unit, *def));
                out[offset++] = 0.0f; // Python shop strength slot is intentionally zero.
            }

            // 5) EXTRA.
            out[offset++] = static_cast<float>(board_count) / static_cast<float>(kBoardSlots);
            out[offset++] = static_cast<float>(bench_count) / static_cast<float>(kBenchSlots);
            out[offset++] = static_cast<float>(economy.gold) / 50.0f;
            out[offset++] = static_cast<float>(economy.level) / 9.0f;
            out[offset++] = player.hp / 100.0f;

            // 6) SYNERGY: first five active trait counts in insertion order.
            for (std::size_t n = 0; n < kSynergyObservationDim; ++n) {
                const float value = n < trait_counts.size()
                    ? static_cast<float>(trait_counts[n].second) / 5.0f : 0.0f;
                out[offset++] = std::min(1.0f, value);
            }

            // 7) POSITIONING: use the same tactical evaluator as rewards and viewer diagnostics.
            const double positioning_score = board_evaluation(i).total_score;
            out[offset++] = static_cast<float>(positioning_score);
            out[offset++] = board_count ? static_cast<float>(frontline) / board_count : 0.0f;
            out[offset++] = board_count ? static_cast<float>(backline) / board_count : 0.0f;
            out[offset++] = std::min(1.0f, static_cast<float>(unique_role_count) / 5.0f);

            // 8) TRAITS: fixed catalog order shared across all players.
            // trait_count_index already maps each observed trait to its
            // insertion-ordered count; avoid building a second hash table.
            for (const auto& trait : observation_trait_names) {
                const auto it = trait_count_index.find(trait);
                const float value = it == trait_count_index.end()
                    ? 0.0f
                    : static_cast<float>(trait_counts[it->second].second) / 5.0f;
                out[offset++] = std::min(1.0f, value);
            }

            // 9) CAPACITY.
            out[offset++] = static_cast<float>(std::max(0, economy.level - board_count)) / 9.0f;
            out[offset++] = static_cast<float>(kBenchSlots - bench_count) / 9.0f;

            // 10) COMPOSITION DIRECTION: explicit, stable one-hot over the
            // catalog trait order. This is derived from board + bench state.
            for (const auto& trait : observation_trait_names) {
                out[offset++] =
                    !strategy.composition_direction.empty() &&
                    strategy.composition_direction == trait ? 1.0f : 0.0f;
            }

            // 11) CORE TRAITS: explicit flags, aligned with the same trait order.
            for (const auto& trait : observation_trait_names) {
                out[offset++] =
                    std::find(strategy.core_traits.begin(),
                              strategy.core_traits.end(), trait) !=
                    strategy.core_traits.end() ? 1.0f : 0.0f;
            }

            // 12) COMPOSITION FIT: normalized per-unit compatibility score.
            // The score is exposed for all candidates, including shop units,
            // so PPO can compare a purchase against the current comp direction.
            const auto append_unit_fit = [&](const UnitInstance* unit) {
                if (!unit) {
                    out[offset++] = 0.0f;
                    return;
                }
                const auto* def = catalog->find_by_id(unit->champion_internal_id);
                if (!def) {
                    out[offset++] = 0.0f;
                    return;
                }
                const double score =
                    unit_composition_score(strategy, *def, unit->star, trait_milestones);
                out[offset++] = static_cast<float>(
                    std::clamp(score / 32.5, 0.0, 1.0));
            };
            for (const auto& unit : player_envs[i].board)
                append_unit_fit(unit ? &*unit : nullptr);
            for (const auto& unit : player_envs[i].bench)
                append_unit_fit(unit ? &*unit : nullptr);
            for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
                const auto* unit = player_envs[i].shop
                    ? player_envs[i].shop->get_unit(static_cast<std::int32_t>(slot))
                    : nullptr;
                append_unit_fit(unit);
            }

            // 13) TEMPORAL ACTION MEMORY. These features are inserted before the
            // candidate tail so policy.py can continue reading candidate features
            // from the end of the observation without changing its offsets.
            out[offset++] = static_cast<float>(kActionMemoryHorizon -
                std::min(player_envs[i].actions_taken_this_round, kActionMemoryHorizon)) /
                static_cast<float>(kActionMemoryHorizon);
            for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
                const auto age = player_envs[i].board[slot]
                    ? std::min(player_envs[i].board_move_age[slot], kActionMemoryHorizon)
                    : kActionMemoryHorizon;
                out[offset++] = static_cast<float>(age) /
                    static_cast<float>(kActionMemoryHorizon);
            }

            // 14) COMPOSITION CONTROL FEATURES. Keep these immediately before the
            // candidate tail: policy.py extracts candidate records from the end,
            // and checkpoint migration inserts new global features at this boundary.
            // Focus weights use the stable catalog trait order shared by direction/core
            // flags and are normalized from Python's [0, 10] scale to [0, 1].
            for (const auto& trait : observation_trait_names) {
                double weight = 0.0;
                for (const auto& [focus_trait, focus_weight] :
                     strategy.composition_focus_weights) {
                    if (focus_trait == trait) {
                        weight = focus_weight;
                        break;
                    }
                }
                out[offset++] = static_cast<float>(
                    std::clamp(weight / 10.0, 0.0, 1.0));
            }
            out[offset++] = strategy.composition_locked ? 1.0f : 0.0f;
            // Strategy strength is bounded to [0, 10] by its scoring formula.
            out[offset++] = static_cast<float>(
                std::clamp(strategy.composition_strength / 10.0, 0.0, 1.0));

            // 15) ITEM ANVIL CHOICES. A pending anvil is a forced single-choice
            // decision. Each option exposes either a component one-hot or the
            // completed item's combat-relevant stats, plus strategic fit.
            const auto* pending_anvil = player_envs[i].item_engine
                ? player_envs[i].item_engine->pending_anvil() : nullptr;
            out[offset++] = pending_anvil ? 1.0f : 0.0f;
            out[offset++] = pending_anvil &&
                pending_anvil->kind == items::ItemAnvilKind::COMPONENT ? 1.0f : 0.0f;
            out[offset++] = pending_anvil &&
                pending_anvil->kind == items::ItemAnvilKind::COMPLETED_ITEM ? 1.0f : 0.0f;
            const auto item_profiles = player_envs[i].item_engine
                ? item_unit_profiles(i) : std::vector<items::UnitProfile>{};
            // Scoped to this observation so changed unit stats/traits never reuse stale fits.
            items::ItemFitCache item_fit_cache(player_envs[i].item_engine.get());
            for (std::size_t option_index = 0; option_index < kAnvilOptionCount; ++option_index) {
                std::array<float, kAnvilOptionFeatureDim> features{};
                if (pending_anvil && option_index < pending_anvil->options.size() && item_catalog) {
                    const auto& option_id = pending_anvil->options[option_index];
                    double best_fit = 0.0;
                    if (pending_anvil->kind == items::ItemAnvilKind::COMPONENT) {
                        const auto component = std::find_if(
                            item_catalog->components.begin(), item_catalog->components.end(),
                            [&](const auto& entry) { return entry.first == option_id; });
                        if (component != item_catalog->components.end()) {
                            const auto component_index = static_cast<std::size_t>(
                                std::distance(item_catalog->components.begin(), component));
                            if (component_index < 10) features[component_index] = 1.0f;
                        }
                        // A component is scored by the best catalog item it can
                        // help complete for any currently owned unit.
                        for (const auto& candidate_item : item_catalog->items) {
                            if (candidate_item.recipe_components[0] != option_id &&
                                candidate_item.recipe_components[1] != option_id) continue;
                            for (const auto& profile : item_profiles)
                                best_fit = std::max(best_fit,
                                    item_fit_cache.get(candidate_item.id, profile));
                        }
                    } else {
                        const auto* definition = player_envs[i].item_engine->find(option_id);
                        if (definition) {
                            const std::array<double, 10> stats{
                                definition->attack_damage / 100.0,
                                definition->attack_speed / 2.0,
                                definition->ability_power / 100.0,
                                definition->critical_strike_chance,
                                definition->critical_strike_damage / 2.0,
                                definition->max_health / 2000.0,
                                definition->armor / 100.0,
                                definition->magic_resist / 100.0,
                                definition->omnivamp,
                                definition->damage_amplification
                            };
                            for (std::size_t stat = 0; stat < stats.size(); ++stat)
                                features[stat] = static_cast<float>(
                                    std::clamp(stats[stat], 0.0, 1.0));
                            for (const auto& profile : item_profiles)
                                best_fit = std::max(best_fit,
                                    item_fit_cache.get(option_id, profile));
                        }
                    }
                    features[10] = static_cast<float>(
                        std::clamp(best_fit / 20.0, 0.0, 1.0));
                }
                for (const float feature : features) out[offset++] = feature;
            }

            // 16) SEMANTIC ITEM STATE. Masks express legality only; this block
            // gives PPO the actual component/item identities and stats, recipe
            // outcomes, and the role/location/profile of every possible target.
            const auto* item_state = player_envs[i].item_engine.get();
            for (std::size_t component = 0; component < 10; ++component) {
                double count = 0.0;
                if (item_state && item_catalog && component < item_catalog->components.size()) {
                    const auto& id = item_catalog->components[component].first;
                    count = static_cast<double>(std::count(
                        item_state->components().begin(), item_state->components().end(), id));
                }
                out[offset++] = static_cast<float>(std::clamp(count / 6.0, 0.0, 1.0));
            }

            for (std::size_t slot = 0; slot < 9; ++slot) {
                std::array<float, kItemSemanticFeatureDim> features{};
                if (item_state && slot < item_state->inventory().size()) {
                    if (const auto* definition = item_state->find(item_state->inventory()[slot]))
                        features = encode_item_semantic_features(*definition);
                }
                for (const float feature : features) out[offset++] = feature;
            }

            // Recipe features are indexed [first_component * 10 + second].
            // The immutable catalog-derived matrix is shared by all players.
            for (const auto& recipe : recipe_observation_features)
                for (const float feature : recipe) out[offset++] = feature;

            for (std::size_t target = 0; target < kBoardSlots + kBenchSlots; ++target) {
                std::array<float, kItemTargetFeatureDim> features{};
                const auto* unit = item_target_unit(i, target);
                if (unit) {
                    const auto* definition = catalog->find_by_id(unit->champion_internal_id);
                    if (definition) {
                        const bool on_board = target < kBoardSlots;
                        const auto row = on_board ? target / 7 : 0;
                        const int role = std::clamp(observation_role(*unit, *definition), 0, 4);
                        const auto& stats = definition->stats;
                        const auto& combat = definition->combat_profile;
                        const auto fit = unit_composition_score(strategy, *definition,
                            unit->star, trait_milestones);
                        features[0] = 1.0f;
                        features[1] = on_board ? 1.0f : 0.0f;
                        features[2] = on_board ? 0.0f : 1.0f;
                        features[3] = static_cast<float>(std::clamp(unit->star / 3.0, 0.0, 1.0));
                        features[4 + role] = 1.0f;
                        features[9] = on_board && row <= 1 ? 1.0f : 0.0f;
                        features[10] = on_board && row >= 2 ? 1.0f : 0.0f;
                        features[11] = static_cast<float>(std::clamp(fit / 32.5, 0.0, 1.0));
                        features[12] = static_cast<float>(std::clamp(
                            (combat.attack_damage > 0.0 ? combat.attack_damage : stats.attack_damage) / 200.0, 0.0, 1.0));
                        features[13] = static_cast<float>(std::clamp(
                            (combat.attack_speed > 0.0 ? combat.attack_speed : stats.attack_speed) / 2.0, 0.0, 1.0));
                        features[14] = static_cast<float>(std::clamp(
                            (combat.max_hp > 0.0 ? combat.max_hp : stats.max_hp) / 2000.0, 0.0, 1.0));
                        features[15] = static_cast<float>(std::clamp(stats.armor / 150.0, 0.0, 1.0));
                        features[16] = static_cast<float>(std::clamp(stats.magic_resist / 150.0, 0.0, 1.0));
                        const auto equipped_count = item_state
                            ? item_state->equipped(unit->uuid).size() : unit->items.size();
                        features[17] = static_cast<float>(std::clamp(equipped_count / 3.0, 0.0, 1.0));
                    }
                }
                for (const float feature : features) out[offset++] = feature;
            }

            // Exact item-to-target compatibility for the currently held items
            // and every possible craft result. These matrices are consumed only
            // by the autoregressive target scorer, not the global encoder.
            std::unordered_map<std::string, const items::UnitProfile*> profile_by_uuid;
            profile_by_uuid.reserve(item_profiles.size());
            for (const auto& profile : item_profiles)
                profile_by_uuid.emplace(profile.uuid, &profile);
            // Resolve each board/bench target once. The inventory and 100 recipe
            // rows reuse this compact slot table instead of repeating UUID lookups.
            std::array<const items::UnitProfile*, kBoardSlots + kBenchSlots>
                profile_by_target{};
            for (std::size_t target = 0; target < profile_by_target.size(); ++target) {
                const auto* unit = item_target_unit(i, target);
                if (!unit) continue;
                const auto profile = profile_by_uuid.find(unit->uuid);
                if (profile != profile_by_uuid.end())
                    profile_by_target[target] = profile->second;
            }

            for (std::size_t item_slot = 0; item_slot < 9; ++item_slot) {
                const std::string* item_id =
                    item_state && item_slot < item_state->inventory().size()
                    ? &item_state->inventory()[item_slot] : nullptr;
                for (std::size_t target = 0; target < kBoardSlots + kBenchSlots; ++target) {
                    double fit = 0.0;
                    const auto* profile = profile_by_target[target];
                    if (item_id && profile)
                        fit = item_fit_cache.get(*item_id, *profile);
                    out[offset++] = static_cast<float>(std::clamp(fit / 50.0, 0.0, 1.0));
                }
            }

            // Recipe availability is symmetric: check each unordered pair once.
            std::array<bool, 100> recipe_available_cache{};
            if (item_state && item_catalog) {
                for (std::size_t first = 0; first < 10; ++first) {
                    for (std::size_t second = first; second < 10; ++second) {
                        bool available = false;
                        if (first < item_catalog->components.size() &&
                            second < item_catalog->components.size()) {
                            available = item_state->can_craft(
                                item_catalog->components[first].first,
                                item_catalog->components[second].first);
                        }
                        recipe_available_cache[first * 10 + second] = available;
                        recipe_available_cache[second * 10 + first] = available;
                    }
                }
            }

            for (std::size_t recipe = 0; recipe < recipe_observation_item_ids.size(); ++recipe) {
                const auto& item_id = recipe_observation_item_ids[recipe];
                const bool recipe_available = recipe_available_cache[recipe];
                for (std::size_t target = 0; target < kBoardSlots + kBenchSlots; ++target) {
                    double fit = 0.0;
                    const auto* profile = profile_by_target[target];
                    if (recipe_available && !item_id.empty() && profile)
                        fit = item_fit_cache.get(item_id, *profile);
                    out[offset++] = static_cast<float>(std::clamp(fit / 50.0, 0.0, 1.0));
                }
            }

            // 17) PER-CANDIDATE DECISION FEATURES. These are explicit slot-local
            // features so the policy can compare offers/sale candidates rather than
            // infer which global features correspond to each output index.
            // Precompute owned copies once per player. Candidate features below
            // query this table for every bench/shop slot; repeated board+bench
            // scans were doing the same work for each candidate.
            std::unordered_map<std::int32_t, int> owned_copies;
            std::unordered_map<std::int32_t, std::array<int, 3>> owned_star_counts;
            owned_copies.reserve(kBoardSlots + kBenchSlots);
            owned_star_counts.reserve(kBoardSlots + kBenchSlots);
            const auto add_owned_copies = [&owned_copies, &owned_star_counts](const UnitInstance& unit) {
                const int copies = unit.star == 1 ? 1 : (unit.star == 2 ? 3 : 9);
                owned_copies[unit.champion_internal_id] += copies;
                if (unit.star >= 1 && unit.star <= 3)
                    ++owned_star_counts[unit.champion_internal_id][static_cast<std::size_t>(unit.star - 1)];
            };
            for (const auto& unit : player_envs[i].board)
                if (unit) add_owned_copies(*unit);
            for (const auto& unit : player_envs[i].bench)
                if (unit) add_owned_copies(*unit);

            const auto count_same_champion = [&owned_copies](const UnitInstance& candidate) {
                const auto it = owned_copies.find(candidate.champion_internal_id);
                return it == owned_copies.end() ? 0 : it->second;
            };
            // Distinguish a real same-star pair from a lone copy plus a
            // different-star upgrade. Exclude the candidate itself for SELL;
            // for shop offers, estimate the path created by buying the copy.
            const auto candidate_upgrade_signal =
                [&owned_star_counts](const UnitInstance& candidate, bool sale_candidate) {
                    const auto it = owned_star_counts.find(candidate.champion_internal_id);
                    if (it == owned_star_counts.end() || candidate.star < 1 || candidate.star > 3)
                        return 0.0;
                    const auto& counts = it->second;
                    const int same_star = std::max(
                        0, counts[static_cast<std::size_t>(candidate.star - 1)] -
                            (sale_candidate ? 1 : 0));
                    const int one_star = std::max(
                        0, counts[0] - (sale_candidate && candidate.star == 1 ? 1 : 0));
                    if (sale_candidate)
                        return reward_model::upgrade_path_strength(
                            candidate.star, same_star, one_star);
                    return reward_model::upgrade_path_strength(1, counts[0], counts[0]);
                };
            const auto append_candidate_features = [&](const UnitInstance* unit, bool sale_candidate) {
                if (!unit) {
                    for (std::size_t n = 0; n < kCandidateFeatureDim; ++n)
                        out[offset++] = 0.0f;
                    return;
                }
                const auto* def = catalog->find_by_id(unit->champion_internal_id);
                if (!def) {
                    for (std::size_t n = 0; n < kCandidateFeatureDim; ++n)
                        out[offset++] = 0.0f;
                    return;
                }
                const double fit = unit_composition_score(strategy, *def, unit->star, trait_milestones);
                const bool core = std::find(strategy.core_unit_ids.begin(),
                    strategy.core_unit_ids.end(), def->internal_id) != strategy.core_unit_ids.end() ||
                    std::find(strategy.planned_unit_ids.begin(),
                    strategy.planned_unit_ids.end(), def->internal_id) != strategy.planned_unit_ids.end();
                const int copies = count_same_champion(*unit);
                out[offset++] = static_cast<float>(std::clamp(fit / 22.5, 0.0, 1.0));
                out[offset++] = static_cast<float>(def->cost) / 5.0f;
                out[offset++] = static_cast<float>(unit->star) / 3.0f;
                out[offset++] = role_state_value(observation_role(*unit, *def));
                out[offset++] = core ? 1.0f : 0.0f;
                // Number of owned copies (including star-equivalent copies), normalized.
                out[offset++] = static_cast<float>(std::min(copies, 9)) / 9.0f;
                // Exact same-star upgrade risk/potential replaces trait overlap:
                // composition fit already includes trait compatibility, while this
                // feature tells the SELL head when a copy is a real pair.
                out[offset++] = static_cast<float>(std::clamp(
                    candidate_upgrade_signal(*unit, sale_candidate), 0.0, 1.0));
                if (sale_candidate) {
                    out[offset++] = static_cast<float>(
                        EconomyEngine::get_sell_value(def->cost, unit->star)) / 50.0f;
                } else {
                    out[offset++] = economy.gold >= def->cost ? 1.0f : 0.0f;
                }
            };
            for (const auto& unit : player_envs[i].bench)
                append_candidate_features(unit ? &*unit : nullptr, true);
            for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
                const auto* unit = player_envs[i].shop
                    ? player_envs[i].shop->get_unit(static_cast<std::int32_t>(slot))
                    : nullptr;
                append_candidate_features(unit, false);
            }
        }
        return observations;
    }

    // ========================================================
    // VALIDAÇÃO DE AÇÕES
    // ========================================================

    void validate_action(const PlayerAction& action) const {
        const auto type = action.values[0];
        if (type == 7) {
            if (action.values[1] < 0 || action.values[1] >= 9 ||
                action.values[2] < 0 || action.values[2] >= static_cast<std::int32_t>(kBoardSlots + kBenchSlots) ||
                action.values[3] != 0 || action.values[4] != 0 || action.values[5] != 0)
                throw std::invalid_argument("Argumentos fora dos limites para EQUIP_FROM_INVENTORY.");
            return;
        }
        if (type == 8) {
            if (action.values[1] < 0 || action.values[1] >= 10 ||
                action.values[2] < 0 || action.values[2] >= 10 ||
                action.values[3] < 0 || action.values[3] >= static_cast<std::int32_t>(kBoardSlots + kBenchSlots) ||
                action.values[4] != 0 || action.values[5] != 0)
                throw std::invalid_argument("Argumentos fora dos limites para CRAFT_AND_EQUIP.");
            return;
        }
        for (std::size_t i = 0;
             i < kActionComponents;
             ++i) {
            const std::int32_t value = action.values[i];

            if (value < 0 ||
                static_cast<std::size_t>(value) >=
                    kActionDims[i]) {
                throw std::invalid_argument(
                    "Componente de acao fora dos limites. "
                    "Dimensao: " + std::to_string(i) +
                    ", valor: " + std::to_string(value)
                );
            }
        }
    }

    // Preflight sem efeitos colaterais: valida a ação selecionada contra
    // o estado atual antes que qualquer jogador seja processado.
    void validate_action_against_masks(
        std::size_t player_index,
        const PlayerAction& action
    ) const {
        const auto& values = action.values;
        const auto type = static_cast<std::size_t>(values[0]);
        const auto& player_masks = player_envs[player_index].action_masks;

        // A tentativa de implantar além do nível deve ser rejeitada com
        // penalidade mesmo quando MOVE_BENCH segue habilitado por existir
        // outra substituição válida no mesmo estado.
        if (type == 3) {
            const auto state = build_decision_state(player_index);
            const auto source = static_cast<std::size_t>(values[2]);
            const auto target = static_cast<std::size_t>(values[3]);
            const auto occupied = static_cast<std::size_t>(
                std::count_if(
                    state.board.begin(),
                    state.board.end(),
                    [](const auto& unit) { return unit.has_value(); }
                )
            );
            if (
                source < state.bench.size()
                && target < state.board.size()
                && state.bench[source].has_value()
                && !state.board[target].has_value()
                && occupied >= static_cast<std::size_t>(state.economy.level)
            ) {
                return;
            }
        }

        // Se o movimento e estruturalmente valido, mas foi bloqueado
        // pelo AntiLoop, deixe-o chegar ao fluxo de execucao mesmo quando
        // nao restam movimentos validos desse tipo. A execucao registra a
        // tentativa e o motivo do bloqueio sem alterar o tabuleiro.
        // Sem este desvio, o type_mask desabilitado faz o preflight lancar
        // antes de record_execution_attempt()/record_execution_block().
        if (type == 3 || type == 4) {
            const auto state = build_decision_state(player_index);
            std::optional<std::string> block_reason;

            if (type == 3) {
                const auto source = values[2];
                const auto target = values[3];
                const auto validation =
                    decision::MoveValidator::validate_bench_to_board_move(
                        state.board,
                        state.bench,
                        source,
                        target
                    );

                if (validation.is_valid()) {
                    const auto source_index =
                        static_cast<std::size_t>(source);
                    const auto unit_identity =
                        std::optional<decision::UnitIdentity>{
                            state.bench[source_index]->identity
                        };
                    block_reason =
                        player_envs[player_index].anti_loop.get_block_reason(
                            "bench_to_board",
                            source,
                            target,
                            unit_identity
                        );
                }
            } else {
                const auto source = values[4];
                const auto target = values[5];
                const auto validation =
                    decision::MoveValidator::validate_board_move(
                        state.board,
                        source,
                        target
                    );

                if (validation.is_valid() &&
                    validation.move_type.has_value()) {
                    const auto source_index =
                        static_cast<std::size_t>(source);
                    const auto unit_identity =
                        std::optional<decision::UnitIdentity>{
                            state.board[source_index]->identity
                        };
                    const std::string move_type =
                        *validation.move_type ==
                            decision::MoveType::BOARD_SWAP
                            ? "board_swap"
                            : "board_move";
                    block_reason =
                        player_envs[player_index].anti_loop.get_block_reason(
                            move_type,
                            source,
                            target,
                            unit_identity
                        );
                }
            }

            if (block_reason.has_value()) {
                return;
            }
        }

        if (type >= player_masks.type_mask.size() || player_masks.type_mask[type] != 1) {
            throw std::invalid_argument(
                "Acao indisponivel para o estado atual do jogador "
                + std::to_string(player_index)
                + ". Tipo: " + std::to_string(type)
            );
        }

        if (type == 7) {
            const auto item_slot = static_cast<std::size_t>(values[1]);
            const auto target_slot = static_cast<std::size_t>(values[2]);
            if (player_masks.item_inventory_mask[item_slot] != 1 || player_masks.item_target_mask[target_slot] != 1)
                throw std::invalid_argument("Equipamento de item indisponivel para os indices selecionados.");
            return;
        }
        if (type == 8) {
            const auto first = static_cast<std::size_t>(values[1]);
            const auto second = static_cast<std::size_t>(values[2]);
            const auto target = static_cast<std::size_t>(values[3]);
            if (player_masks.item_component_first_mask[first] != 1 ||
                player_masks.item_component_pair_masks[first * 10 + second] != 1 ||
                player_masks.item_target_mask[target] != 1)
                throw std::invalid_argument("Receita/target invalido para CRAFT_AND_EQUIP.");
            return;
        }
        if (type == 0) {
            const auto slot = static_cast<std::size_t>(values[1]);
            if (player_masks.shop_mask[slot] != 1) {
                throw std::invalid_argument(
                    "Compra invalida: oferta indisponivel, ouro insuficiente "
                    "ou banco cheio."
                );
            }
            return;
        }

        if (type == 5) {
            const auto slot = static_cast<std::size_t>(values[2]);
            if (player_masks.bench_source_mask[slot] != 1) {
                throw std::invalid_argument(
                    "Venda invalida: o slot selecionado do banco esta vazio."
                );
            }
            return;
        }

        // Movimentos estruturalmente válidos que o AntiLoop bloqueia
        // ainda devem chegar à engine para contabilizar a tentativa.
        if (type == 3 || type == 4) {
            const auto state = build_decision_state(player_index);
            bool valid_move = false;

            if (type == 3) {
                valid_move =
                    decision::MoveValidator::validate_bench_to_board_move(
                        state.board,
                        state.bench,
                        values[2],
                        values[3]
                    ).is_valid();
            } else {
                valid_move =
                    decision::MoveValidator::validate_board_move(
                        state.board,
                        values[4],
                        values[5]
                    ).is_valid();
            }

            if (!valid_move) {
                throw std::invalid_argument(
                    "Movimento invalido: origem vazia, destino ocupado "
                    "ou posicoes incompatíveis."
                );
            }
        }
    }

    // ========================================================
    // SINCRONIZAÇÃO DA ECONOMIA
    // ========================================================

    double board_power(std::size_t player_index) const {
        // Mesma estimativa de MiniTFTEnv._update_strategy():
        // sum(cost * star * 2.0) para as unidades no tabuleiro.
        double power = 0.0;
        for (const auto& unit : player_envs[player_index].board) {
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            power += static_cast<double>(def->cost)
                * static_cast<double>(unit->star) * 2.0;
        }
        return power;
    }

    double synergy_score(std::size_t player_index) const {
        std::vector<strategy::ChampionTraitEntry> entries;
        entries.reserve(kBoardSlots);
        for (const auto& unit : player_envs[player_index].board) {
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            auto traits = def->traits;
            for (const auto& item : unit->items) {
                if (!item.grants_trait.empty() &&
                    std::find(traits.begin(), traits.end(), item.grants_trait) == traits.end()) {
                    traits.push_back(item.grants_trait);
                }
            }
            entries.emplace_back(unit->champion_internal_id, std::move(traits));
        }
        // Keep the same unique-by-unit["id"] semantics as Python.
        const auto counts = strategy::count_unique_champion_traits(entries);
        double score = 0.0;
        for (const auto& [trait, count] : counts) {
            const auto milestones = trait_milestones.find(trait);
            if (milestones == trait_milestones.end()) continue;
            int reached = 0;
            for (const int milestone : milestones->second)
                if (count >= milestone) ++reached;
            if (reached > 0) score += static_cast<double>(reached);
            else if (std::any_of(milestones->second.begin(), milestones->second.end(),
                                 [count](int milestone) { return count == milestone - 1; }))
                score += 0.5; // progresso parcial, faltando uma unidade
        }
        return score;
    }

    std::uint64_t board_evaluation_signature(std::size_t player_index) const {
        const auto& env = player_envs[player_index];
        std::size_t hash = 0;
        const auto mix = [&hash](std::size_t value) {
            hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
                (hash << 6U) + (hash >> 2U);
        };
        const auto add_int = [&mix](std::int32_t value) {
            mix(std::hash<std::int32_t>{}(value));
        };
        const auto add_string = [&mix](const std::string& value) {
            mix(std::hash<std::string>{}(value));
        };
        const auto add_double = [&mix](double value) {
            mix(std::hash<double>{}(value));
        };
        const auto add_unit = [&](const std::optional<UnitInstance>& maybe_unit,
                                  bool include_equipment) {
            mix(maybe_unit.has_value() ? 1U : 0U);
            if (!maybe_unit) return;
            const auto& unit = *maybe_unit;
            add_int(unit.champion_internal_id);
            add_int(unit.star);
            add_string(unit.champion_id);
            if (!include_equipment) return;
            mix(unit.items.size());
            for (const auto& item : unit.items) {
                add_double(item.health);
                add_double(item.armor);
                add_double(item.magic_resist);
                add_double(item.attack_damage);
                add_double(item.attack_speed);
                add_double(item.ability_power);
                add_double(item.critical_strike_chance);
                add_string(item.grants_trait);
            }
        };

        // Position and occupancy are significant to tactical evaluation.
        for (const auto& unit : env.board) add_unit(unit, true);
        for (const auto& unit : env.bench) add_unit(unit, false);
        add_int(env.economy.level);

        const StrategyInfo empty_strategy{};
        const StrategyInfo& active = env.strategy.empty()
            ? empty_strategy : env.strategy.back();
        add_int(active.planned_main_carry_id);
        add_string(active.composition_direction);
        for (const auto& [trait, weight] : active.composition_focus_weights) {
            add_string(trait);
            add_double(weight);
        }
        mix(active.composition_focus_weights.size());
        for (const auto id : active.core_unit_ids) add_int(id);
        mix(active.core_unit_ids.size());
        for (const auto id : active.planned_unit_ids) add_int(id);
        mix(active.planned_unit_ids.size());
        for (const auto& trait : active.core_traits) add_string(trait);
        mix(active.core_traits.size());
        for (const auto& trait : active.planned_traits) add_string(trait);
        mix(active.planned_traits.size());
        for (const auto& name : active.core_units) add_string(name);
        mix(active.core_units.size());
        return static_cast<std::uint64_t>(hash);
    }

    strategy::BoardEvaluationResult board_evaluation(
        std::size_t player_index) const {
        auto& cached = board_evaluation_cache[player_index];
        const auto signature = board_evaluation_signature(player_index);
        if (cached.matches(signature))
            return cached.value();
        const StrategyInfo active_strategy = player_envs[player_index].strategy.empty()
            ? StrategyInfo{} : player_envs[player_index].strategy.back();
        const auto classify_unit = [&](const UnitInstance& unit, const ChampionDefinition& def) {
            strategy::UnitCombatSignals signals;
            signals.base_role = def.combat_profile.role;
            signals.attack_range = def.stats.attack_range > 0.0 ? def.stats.attack_range : def.combat_profile.attack_range;
            signals.max_health = def.stats.max_hp; signals.armor = def.stats.armor;
            signals.magic_resist = def.stats.magic_resist; signals.attack_damage = def.stats.attack_damage;
            signals.attack_speed = def.stats.attack_speed; signals.ability_text = def.ability.description + " " + def.ability.name;
            signals.planned_main_carry = unit.champion_internal_id == active_strategy.planned_main_carry_id;
            for (const auto& item : unit.items) {
                signals.item_health += item.health; signals.item_armor += item.armor;
                signals.item_magic_resist += item.magic_resist; signals.item_attack_damage += item.attack_damage;
                signals.item_attack_speed += item.attack_speed; signals.item_ability_power += item.ability_power;
                signals.item_crit += item.critical_strike_chance;
            }
            return strategy::UnitCombatClassifier::classify(signals);
        };
        std::vector<int> board_roles(kBoardSlots, -1);
        std::vector<strategy::BoardUnitProfile> board_profiles(kBoardSlots);
        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            const auto& unit = player_envs[player_index].board[slot];
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (def) {
                board_roles[slot] = classify_unit(*unit, *def).positioning_role;
                auto& profile = board_profiles[slot];
                profile.champion_id = unit->champion_internal_id;
                profile.role = board_roles[slot];
                profile.star = unit->star;
                profile.cost = def->cost;
                profile.traits = def->traits;
                // Emblems and other trait-granting items are part of the
                // effective board identity for synergy and strategic fit.
                for (const auto& item : unit->items) {
                    if (!item.grants_trait.empty() &&
                        std::find(profile.traits.begin(), profile.traits.end(),
                                  item.grants_trait) == profile.traits.end())
                        profile.traits.push_back(item.grants_trait);
                }
            }
        }

        strategy::StrategicBoardContext strategic_context;
        strategic_context.level = player_envs[player_index].economy.level;
        strategic_context.direction = active_strategy.composition_direction;
        strategic_context.focus_weights = active_strategy.composition_focus_weights;
        strategic_context.core_unit_ids = active_strategy.core_unit_ids;
        strategic_context.planned_unit_ids = active_strategy.planned_unit_ids;
        strategic_context.core_traits = active_strategy.core_traits;
        strategic_context.planned_traits = active_strategy.planned_traits;
        strategic_context.trait_milestones = trait_milestones;
        std::vector<int> main_carry_positions;
        int fallback_carry_slot = -1;
        double fallback_carry_fit = -std::numeric_limits<double>::infinity();
        int core_carry_slot = -1;
        double core_carry_fit = -std::numeric_limits<double>::infinity();
        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            const auto& unit = player_envs[player_index].board[slot];
            if (!unit || board_roles[slot] != PositioningEngine::carry) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            const bool core_by_id = std::find(
                active_strategy.core_unit_ids.begin(), active_strategy.core_unit_ids.end(),
                unit->champion_internal_id) != active_strategy.core_unit_ids.end() ||
                unit->champion_internal_id == active_strategy.planned_main_carry_id;
            const auto name_is_core = [&active_strategy](const std::string& name) {
                return !name.empty() && std::find(
                    active_strategy.core_units.begin(), active_strategy.core_units.end(),
                    name) != active_strategy.core_units.end();
            };
            const bool core_by_name = name_is_core(def->api_name) ||
                name_is_core(def->character_name) || name_is_core(def->name) ||
                name_is_core(unit->champion_id);
            const double fit = unit_composition_score(
                active_strategy, *def, unit->star, trait_milestones);
            if (fit > fallback_carry_fit) {
                fallback_carry_fit = fit;
                fallback_carry_slot = static_cast<int>(slot);
            }
            if ((core_by_id || core_by_name) && fit > core_carry_fit) {
                core_carry_fit = fit;
                core_carry_slot = static_cast<int>(slot);
            }
        }
        // Track one main carry: prefer the best-fitting deployed core carry.
        // If the composition core is not deployed, use the best-fitting carry.
        const int selected_main_carry =
            core_carry_slot >= 0 ? core_carry_slot : fallback_carry_slot;
        if (selected_main_carry >= 0)
            main_carry_positions.push_back(selected_main_carry);

        std::vector<strategy::BenchUnitProfile> bench_profiles;
        bench_profiles.reserve(kBenchSlots);
        for (const auto& unit : player_envs[player_index].bench) {
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            bench_profiles.push_back({
                classify_unit(*unit, *def).positioning_role,
                unit->star
            });
        }

        // Trait milestones contribute a bounded coherence diagnostic. The
        // separate synergy reward remains the primary active-trait signal.
        const auto result = strategy::BoardEvaluation::evaluate(
            board_roles, bench_profiles, 0.0, main_carry_positions,
            board_profiles, strategic_context);
        cached.store(signature, result);
        return result;
    }

    double positioning_score(std::size_t player_index) const {
        // Composition synergy has a separate reward component, so it is not
        // duplicated in this positioning/structure score.
        return board_evaluation(player_index).total_score;
    }

    double combat_reward(bool won, int damage_dealt, int damage_taken,
                         int enemy_alive, int own_alive) const {
        return reward_model::combat(
            won, damage_dealt, damage_taken, enemy_alive, own_alive);
    }

    void update_strategy(std::size_t i) {
        auto& list = player_envs[i].strategy;
        StrategyInfo info = list.empty() ? StrategyInfo{} : list.back();
        // Keep the last confirmed lineup so small score fluctuations do not
        // repeatedly replace the strategic plan on every refresh.
        const auto previous_planned_units = info.planned_units;
        const auto previous_planned_unit_ids = info.planned_unit_ids;
        const auto previous_planned_traits = info.planned_traits;
        const auto previous_planned_main_carry_id = info.planned_main_carry_id;
        const double previous_planned_score = info.planned_composition_score;
        const auto previous_planned_notes = info.planned_acquisition_notes;
        const bool previous_composition_locked = info.composition_locked;
        const std::string previous_direction = info.composition_direction;
        const auto& player = player_envs[i].info;
        const auto& economy = player_envs[i].economy;
        const double power = board_power(i);
        double bench_strength = 0.0;
        for (const auto& unit : player_envs[i].bench) {
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (def) bench_strength += static_cast<double>(def->cost) * unit->star;
        }
        // A estratégia econômica é selecionada por uma única política.
        // FAST8 e SLOWROLL só ficam elegíveis quando a composição os solicita.
        const auto composition_has_unit = [&](std::int32_t id) {
            return std::find(info.planned_unit_ids.begin(), info.planned_unit_ids.end(), id)
                    != info.planned_unit_ids.end() ||
                std::find(info.core_unit_ids.begin(), info.core_unit_ids.end(), id)
                    != info.core_unit_ids.end();
        };
        std::unordered_map<std::int32_t, int> owned_copies;
        const auto count_owned_copy = [&](const UnitInstance& unit) {
            owned_copies[unit.champion_internal_id] += star_copies(unit.star);
        };
        for (const auto& unit : player_envs[i].board) if (unit) count_owned_copy(*unit);
        for (const auto& unit : player_envs[i].bench) if (unit) count_owned_copy(*unit);

        // SLOWROLL is enabled only while at least one concrete planned/core
        // low-cost target remains realistically obtainable. Count copies held
        // by opponents and check remaining pool supply so a completed, exhausted,
        // or heavily contested target cannot keep the strategy active forever.
        bool composition_requests_three_star = false;
        int best_three_star_copies = 0;
        const int slowroll_stage_window = static_cast<int>(stage_for_round(round + 1));
        if (slowroll_stage_window >= 3 && slowroll_stage_window <= 4 &&
            economy.level >= 5 && economy.level <= 6) {
            for (const auto& [unit_id, copies] : owned_copies) {
                if (copies < 2 || copies >= 9 || !composition_has_unit(unit_id)) continue;
                const auto* definition = catalog->find_by_id(unit_id);
                if (!definition || definition->cost > 3) continue;

                int contested_copies = 0;
                for (std::size_t opponent = 0; opponent < kPlayersPerLobby; ++opponent) {
                    if (opponent == i || !player_envs[opponent].info.alive) continue;
                    const auto count_opponent_copy = [&](const UnitInstance& unit) {
                        if (unit.champion_internal_id == unit_id)
                            contested_copies += star_copies(unit.star);
                    };
                    for (const auto& unit : player_envs[opponent].board)
                        if (unit) count_opponent_copy(*unit);
                    for (const auto& unit : player_envs[opponent].bench)
                        if (unit) count_opponent_copy(*unit);
                }

                const int pool_available = pool ? pool->available(unit_id) : 0;
                const int copies_still_needed = 9 - copies;
                const int minimum_supply_check = std::min(copies_still_needed, 3);
                if (pool_available < minimum_supply_check || contested_copies >= 6)
                    continue;

                if (copies > best_three_star_copies) {
                    best_three_star_copies = copies;
                    composition_requests_three_star = true;
                }
            }
        }
        const bool composition_requests_fast8 =
            !info.composition_direction.empty() &&
            info.planned_unit_ids.size() >= 8 &&
            economy.level < 8;
        const strategy::EconomicStrategyContext strategy_context{
            economy.gold,
            static_cast<double>(player.hp),
            economy.level,
            economy.win_streak,
            economy.loss_streak,
            power,
            composition_requests_fast8,
            composition_requests_three_star,
            static_cast<int>(stage_for_round(round + 1))
        };
        const auto strategy_decision =
            strategy::select_economic_strategy(strategy_context);
        const std::int32_t chosen = static_cast<std::int32_t>(strategy_decision.selected);
        const float confidence = static_cast<float>(strategy_decision.confidence);
        const std::string reason = strategy_decision.reason;

        const bool changed = chosen != info.strategy;
        if (changed) {
            info.previous_strategy = info.strategy;
            info.strategy_age = 0;
            if (chosen == 1)
                info.comp_stabilize_start_round = static_cast<std::int32_t>(round + 1);
        } else {
            ++info.strategy_age;
        }
        static const std::array<const char*, 7> names{
            "ECON", "STABILIZE", "FAST8", "SLOWROLL",
            "ALL_IN", "WINSTREAK", "LOSS_STREAK"
        };
        info.strategy = chosen;
        info.strategy_name = names[static_cast<std::size_t>(chosen)];
        info.confidence = confidence;
        info.reason = reason;
        info.changed = changed;
        info.board_power = power;
        info.bench_strength = bench_strength;

        // Equivalente ao CompositionManager.update(board, bench).
        std::unordered_map<std::string, int> trait_counts;
        std::unordered_map<std::int32_t, int> copies_by_unit;
        std::unordered_map<std::int32_t, double> focus_scores;
        std::unordered_map<std::int32_t, std::string> unit_names;
        // Preserve Python dict/Counter insertion order for tie-breaking.
        std::vector<std::string> trait_order;
        std::vector<std::int32_t> unit_order;
        const auto add_owned_unit = [&](const UnitInstance& unit) {
            const auto* def = catalog->find_by_id(unit.champion_internal_id);
            if (!def) return;
            const std::int32_t id = def->internal_id;
            if (copies_by_unit.find(id) == copies_by_unit.end()) unit_order.push_back(id);
            unit_names[id] = def->api_name;
            copies_by_unit[id] += star_copies(unit.star);
            // Trait emblems count as a trait-bearing unit in the owned-team
            // planner, just as they do in combat. Deduplicate per unit so two
            // copies of the same emblem cannot count as two units for a breakpoint.
            std::unordered_set<std::string> unit_traits(def->traits.begin(), def->traits.end());
            for (const auto& item : unit.items) {
                if (!item.grants_trait.empty()) unit_traits.insert(item.grants_trait);
            }
            for (const auto& trait : unit_traits) {
                if (trait_counts.find(trait) == trait_counts.end())
                    trait_order.push_back(trait);
                ++trait_counts[trait];
            }
        };
        for (const auto& unit : player_envs[i].board) if (unit) add_owned_unit(*unit);
        for (const auto& unit : player_envs[i].bench) if (unit) add_owned_unit(*unit);

        const auto score_owned_unit = [&](const UnitInstance& unit) {
            const auto* def = catalog->find_by_id(unit.champion_internal_id);
            if (!def) return;
            const std::int32_t id = def->internal_id;
            const double score = (unit.star - 1) * 3.0 + def->cost * 1.5
                + def->traits.size() + std::min(copies_by_unit[id], 9) * 0.5;
            focus_scores[id] = std::max(focus_scores[id], score);
        };
        for (const auto& unit : player_envs[i].board) if (unit) score_owned_unit(*unit);
        for (const auto& unit : player_envs[i].bench) if (unit) score_owned_unit(*unit);

        // Retain the previous focus/core snapshot before rebuilding candidates.
        // Owned, still-viable pieces should not churn merely because a refresh
        // produces a tiny score fluctuation.
        const std::int32_t previous_focus_id = info.composition_focus_id;
        const auto previous_core_unit_ids = info.core_unit_ids;
        info.composition_focus.clear();
        info.composition_focus_id = -1;
        // Keep composition_direction persistent while composition_locked is true.
        info.core_units.clear();
        info.core_unit_ids.clear();
        info.core_traits.clear();
        info.planned_units.clear();
        info.planned_unit_ids.clear();
        info.planned_traits.clear();
        info.planned_main_carry_id = -1;
        info.planned_composition_score = 0.0;
        info.active_traits.clear();
        info.unit_counts.clear();
        info.unit_counts_by_id.clear();

        // Port of Python _update_comp_direction(), _update_comp_focus(), and
        // _should_lock_comp(). Python's _get_offensive_factor() helper is not
        // part of this native engine, so champion role is used as a proxy.
        const auto offensive_factor = [](const ChampionDefinition& def) {
            switch (def.combat_profile.role) {
                case 1: // carry
                case 4: // assassin
                    return 1.20;
                case 3: // fighter
                    return 1.05;
                case 0: // tank
                case 2: // support
                    return 0.90;
                default:
                    return 1.0;
            }
        };
        const auto current_stage = stage_for_round(round + 1);
        const auto current_round = static_cast<std::int32_t>(round + 1);
        const auto count_occupied = [](const auto& slots) {
            return static_cast<std::size_t>(std::count_if(
                slots.begin(), slots.end(),
                [](const auto& unit) { return unit.has_value(); }));
        };
        const auto unit_count = count_occupied(player_envs[i].board) +
            count_occupied(player_envs[i].bench);

        std::map<std::string, double> focus;
        for (const auto& [trait, value] : info.composition_focus_weights)
            focus[trait] = value;

        std::unordered_map<std::string, int> board_trait_counts;
        for (const auto& unit : player_envs[i].board) {
            if (!unit) continue;
            const auto* def = catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            std::unordered_set<std::string> unit_traits(def->traits.begin(), def->traits.end());
            for (const auto& item : unit->items) {
                if (!item.grants_trait.empty()) unit_traits.insert(item.grants_trait);
            }
            for (const auto& trait : unit_traits) ++board_trait_counts[trait];
        }
        // Active traits mirror Python's board-only SynergyManager.active_traits.
        for (const auto& trait : trait_order) {
            const auto found = board_trait_counts.find(trait);
            if (found != board_trait_counts.end()) info.active_traits.emplace_back(trait, found->second);
        }
        const auto trait_level = [&](const std::string& trait, int count) {
            const auto found = trait_milestones.find(trait);
            if (found == trait_milestones.end()) return count >= 2 ? 1 : 0;
            int level = 0;
            for (const int milestone : found->second) {
                if (count >= milestone) ++level;
            }
            return level;
        };

        std::unordered_map<std::string, double> trait_score;
        const auto score_composition_unit = [&](const UnitInstance& unit) {
            const auto* def = catalog->find_by_id(unit.champion_internal_id);
            if (!def) return;
            const double base = 1.0 + def->cost * 0.15 +
                (std::clamp(unit.star, 1, 3) - 1) * 0.5;
            const double weighted = base * offensive_factor(*def);
            std::unordered_set<std::string> unit_traits(def->traits.begin(), def->traits.end());
            for (const auto& item : unit.items) {
                if (!item.grants_trait.empty()) unit_traits.insert(item.grants_trait);
            }
            for (const auto& trait : unit_traits) trait_score[trait] += weighted;
        };
        for (const auto& unit : player_envs[i].board) if (unit) score_composition_unit(*unit);
        for (const auto& unit : player_envs[i].bench) if (unit) score_composition_unit(*unit);

        for (const auto& [trait, count] : board_trait_counts) {
            if (trait_score.find(trait) == trait_score.end()) continue;
            const int level = trait_level(trait, count);
            if (level > 0) {
                double avg_off = 0.0;
                int matching = 0;
                const auto accumulate_offense = [&](const auto& slots) {
                    for (const auto& unit : slots) {
                        if (!unit) continue;
                        const auto* def = catalog->find_by_id(unit->champion_internal_id);
                        if (def && std::find(def->traits.begin(), def->traits.end(), trait) != def->traits.end()) {
                            avg_off += offensive_factor(*def);
                            ++matching;
                        }
                    }
                };
                accumulate_offense(player_envs[i].board);
                if (matching > 0) avg_off /= matching;
                else avg_off = 1.0;
                trait_score[trait] += (level * 2.0 + count * 0.3) * avg_off;
            }
        }
        for (const auto& [trait, count] : trait_counts) {
            if (board_trait_counts.find(trait) == board_trait_counts.end() ||
                trait_level(trait, board_trait_counts[trait]) == 0) {
                if (trait_score.find(trait) != trait_score.end())
                    trait_score[trait] += 1.0 + count * 0.2;
            }
        }
        for (auto& [trait, score] : trait_score) {
            const auto found = focus.find(trait);
            if (found != focus.end())
                score *= 1.0 + std::min(0.4, found->second * 0.08);
        }

        std::vector<std::pair<std::string, double>> sorted_traits(
            trait_score.begin(), trait_score.end());
        std::stable_sort(sorted_traits.begin(), sorted_traits.end(),
            [&](const auto& lhs, const auto& rhs) {
                if (lhs.second != rhs.second) return lhs.second > rhs.second;
                const auto left_pos = std::find(trait_order.begin(), trait_order.end(), lhs.first);
                const auto right_pos = std::find(trait_order.begin(), trait_order.end(), rhs.first);
                return left_pos < right_pos;
            });

        const auto current_trait_score = trait_score.find(info.composition_direction);
        const bool current_direction_viable =
            !info.composition_direction.empty() &&
            current_trait_score != trait_score.end() && current_trait_score->second > 2.0;
        bool composition_transition_unlocked_this_update = false;

        std::map<std::string, double> new_comp_traits;
        // The first composition decision is made at round 9 (stage 2-5),
        // after the opening PvE rounds and enough shop/board evidence.
        if (current_round < 9) {
            info.composition_locked = false;
            info.composition_direction.clear();
            info.composition_strength = 0.0;
            info.composition_candidate_direction.clear();
            info.composition_candidate_strength = 0.0;
            info.composition_candidate_streak = 0;
        } else if (unit_count == 0) {
            info.composition_locked = false;
            info.composition_direction.clear();
            info.composition_strength = 0.0;
            info.composition_candidate_direction.clear();
            info.composition_candidate_strength = 0.0;
            info.composition_candidate_streak = 0;
            focus.clear();
        } else {
            ++info.comp_direction_update_steps;
            if (info.comp_direction_update_steps >= 2 ||
                (current_round == 9 && info.composition_direction.empty())) {
                info.comp_direction_update_steps = 0;
                if (!sorted_traits.empty()) {
                    const auto& top = sorted_traits.front();
                    const double second = sorted_traits.size() > 1
                        ? sorted_traits[1].second : 0.0;
                    const double gap = top.second - second;
                    const double ratio = top.second / (second + 1e-5);
                    if (!info.composition_locked && current_stage >= 3 &&
                        unit_count >= 6 && (ratio > 1.6 || gap > 2.5)) {
                        info.composition_locked = true;
                    }
                    if (info.composition_locked && current_direction_viable && !focus.empty()) {
                        std::map<std::string, double> combined;
                        for (const auto& [trait, weight] : focus)
                            combined[trait] = weight * 0.6;
                        for (const auto& [trait, score] : trait_score)
                            combined[trait] += score * 0.4;
                        std::vector<std::pair<std::string, double>> ordered(
                            combined.begin(), combined.end());
                        std::stable_sort(ordered.begin(), ordered.end(),
                            [](const auto& lhs, const auto& rhs) {
                                return lhs.second > rhs.second;
                            });
                        if (!ordered.empty()) {
                            new_comp_traits[ordered[0].first] = ordered[0].second * 1.4;
                            for (std::size_t index = 1;
                                 index < std::min<std::size_t>(4, ordered.size()); ++index) {
                                if (ordered[index].second > ordered[0].second * 0.35)
                                    new_comp_traits[ordered[index].first] = ordered[index].second * 0.5;
                            }
                        }
                    } else {
                        new_comp_traits[top.first] = top.second * 1.4;
                        for (std::size_t index = 1;
                             index < std::min<std::size_t>(4, sorted_traits.size()); ++index) {
                            if (sorted_traits[index].second > top.second * 0.35)
                                new_comp_traits[sorted_traits[index].first] =
                                    sorted_traits[index].second * 0.5;
                        }
                    }
                }

                for (auto& [trait, weight] : focus) weight *= 0.95;
                for (auto it = focus.begin(); it != focus.end();) {
                    if (it->second < 0.05) it = focus.erase(it);
                    else ++it;
                }
                for (const auto& [trait, value] : new_comp_traits)
                    focus[trait] += value * 0.04;
                if (!new_comp_traits.empty()) {
                    const auto main = std::max_element(
                        new_comp_traits.begin(), new_comp_traits.end(),
                        [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
                    focus[main->first] += 0.8;
                }
                if (!info.composition_direction.empty() && !new_comp_traits.empty()) {
                    const bool overlap = new_comp_traits.find(info.composition_direction) != new_comp_traits.end();
                    if (!overlap && current_stage >= 4)
                        focus[info.composition_direction] = std::max(
                            0.1, focus[info.composition_direction]) * 1.1;
                }
                for (auto& [trait, weight] : focus) weight = std::min(weight, 5.0);

                // The candidate is recalculated each refresh, but a locked direction
                // only changes through explicit hysteresis or loss-of-viability.
                strategy::CompositionDirectionState direction_state;
                direction_state.direction = info.composition_direction;
                direction_state.strength = info.composition_strength;
                direction_state.locked = info.composition_locked;
                direction_state.candidate_direction = info.composition_candidate_direction;
                direction_state.candidate_strength = info.composition_candidate_strength;
                direction_state.candidate_streak = info.composition_candidate_streak;
                direction_state.previous_direction = info.previous_composition_direction;
                direction_state.change_reason = info.composition_direction_change_reason;
                direction_state.change_count = info.composition_direction_change_count;

                std::string candidate_direction;
                double candidate_strength = 0.0;
                if (!new_comp_traits.empty()) {
                    const auto main = std::max_element(
                        new_comp_traits.begin(), new_comp_traits.end(),
                        [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
                    candidate_direction = main->first;
                    double total = 0.0;
                    for (const auto& [trait, value] : new_comp_traits)
                        total += std::min(value, 10.0);
                    candidate_strength = total / new_comp_traits.size();
                }

                // If the current direction has lost viability but still wins the
                // stale focus-weighted ranking, select the best different trait
                // instead of repeatedly proposing the direction we are abandoning.
                if (!current_direction_viable &&
                    candidate_direction == info.composition_direction) {
                    const auto alternative = std::find_if(
                        sorted_traits.begin(), sorted_traits.end(),
                        [&](const auto& item) {
                            return item.first != info.composition_direction;
                        });
                    if (alternative != sorted_traits.end()) {
                        candidate_direction = alternative->first;
                        candidate_strength = alternative->second * 1.4;
                    }
                }

                static const strategy::CompositionDirectionPolicy direction_policy{
                    strategy::CompositionDirectionConfig{
                        TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE,
                        TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE,
                        TFT_COMP_DIRECTION_REQUIRED_UPDATES
                    }
                };
                const std::string direction_before = info.composition_direction;
                const double strength_before = info.composition_strength;
                const bool locked_before = info.composition_locked;
                const auto direction_result = direction_policy.update(
                    direction_state,
                    strategy::CompositionDirectionInput{
                        candidate_direction,
                        candidate_strength,
                        current_direction_viable,
                        false
                    });

                if (direction_result.changed) {
                    info.previous_composition_direction = direction_before;
                    info.previous_composition_strength = strength_before;
                    info.composition_direction_change_was_locked = locked_before;
                    info.composition_direction_change_round = current_round;
                    const auto focus_weight = focus.find(direction_state.direction);
                    info.composition_direction_change_focus_weight =
                        focus_weight == focus.end() ? 0.0 : focus_weight->second;
                    info.composition_direction_change_reason = direction_state.change_reason;
                }
                composition_transition_unlocked_this_update = direction_result.unlocked;
                info.composition_direction = direction_state.direction;
                info.composition_strength = direction_state.strength;
                info.composition_candidate_direction = direction_state.candidate_direction;
                info.composition_candidate_strength = direction_state.candidate_strength;
                info.composition_candidate_streak = direction_state.candidate_streak;
                info.previous_composition_direction = direction_state.previous_direction;
                info.composition_direction_change_count = direction_state.change_count;

                // A successful strategic transition must not be re-locked by the
                // generic stage/board-stability heuristic in the same update.
                if (direction_result.unlocked)
                    info.composition_locked = false;
            }
        }

        // Python focus decay runs every decision step; direction refresh has its own
        // 0.95 decay every two steps above.
        const double focus_decay = current_stage <= 3 ? 0.88 : 0.92;
        for (auto& [trait, weight] : focus) weight *= focus_decay;
        std::string main_focus_trait;
        if (!new_comp_traits.empty()) {
            const auto main = std::max_element(new_comp_traits.begin(), new_comp_traits.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
            main_focus_trait = main->first;
        } else if (!info.composition_direction.empty()) main_focus_trait = info.composition_direction;
        for (const auto& [trait, count] : board_trait_counts) {
            const int level = trait_level(trait, count);
            const double base = count * 0.2 + level * 0.9;
            double gain = trait == main_focus_trait ? base * 2.5
                : (new_comp_traits.find(trait) != new_comp_traits.end() ? base * 0.9 : base * 0.25);
            if (current_stage <= 2) gain *= 0.6;
            focus[trait] += gain;
        }
        for (const auto& [trait, count] : trait_counts) {
            const auto it = board_trait_counts.find(trait);
            const int deployed = it == board_trait_counts.end() ? 0 : it->second;
            const int partial = std::max(0, count - deployed);
            if (partial == 0) continue;
            const double base = partial * 0.08 + 0.35;
            focus[trait] += trait == main_focus_trait ? base * 1.6
                : (new_comp_traits.find(trait) != new_comp_traits.end() ? base * 0.7 : base * 0.15);
        }
        if (info.composition_locked && !main_focus_trait.empty()) focus[main_focus_trait] *= 1.25;
        if (!main_focus_trait.empty()) {
            const double main_value = focus[main_focus_trait];
            for (auto& [trait, weight] : focus)
                if (trait != main_focus_trait && weight < main_value * 0.3) weight *= 0.75;
            focus[main_focus_trait] += 1.2;
        }
        if (focus.size() > 5) {
            std::vector<std::pair<std::string, double>> ordered(focus.begin(), focus.end());
            std::stable_sort(ordered.begin(), ordered.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
            focus.clear();
            for (std::size_t index = 0; index < 5; ++index)
                focus[ordered[index].first] = ordered[index].second;
        }
        double focus_total = 0.0;
        for (const auto& [trait, weight] : focus) focus_total += std::max(0.0, weight);
        if (focus_total > 0.0) {
            for (auto& [trait, weight] : focus)
                weight = std::min(10.0, weight * 10.0 / focus_total);
        }

        // _should_lock_comp(): stage 4+ requires an active offensive synergy;
        // STABILIZE can release the lock after three rounds without a win.
        const bool stabilize_lock_suppressed =
            info.strategy == 1 && info.comp_stabilize_start_round >= 0 &&
            current_round - info.comp_stabilize_start_round >= 3 &&
            economy.win_streak == 0;
        if (!stabilize_lock_suppressed && !composition_transition_unlocked_this_update) {
            bool has_offensive_synergy = false;
            if (current_stage >= 4) {
                for (const auto& [trait, count] : board_trait_counts) {
                    if (trait_level(trait, count) < 2) continue;
                    double avg_off = 0.0;
                    int matching = 0;
                    const auto inspect = [&](const auto& slots) {
                        for (const auto& unit : slots) {
                            if (!unit) continue;
                            const auto* def = catalog->find_by_id(unit->champion_internal_id);
                            if (def && std::find(def->traits.begin(), def->traits.end(), trait) != def->traits.end()) {
                                avg_off += offensive_factor(*def);
                                ++matching;
                            }
                        }
                    };
                    inspect(player_envs[i].board);
                    inspect(player_envs[i].bench);
                    if (matching > 0 && avg_off / matching > 1.0) {
                        has_offensive_synergy = true;
                        break;
                    }
                }
            }
            if (current_stage < 4 || has_offensive_synergy) {
                double avg_board_power = 0.0;
                int alive_count = 0;
                for (std::size_t other = 0; other < kPlayersPerLobby; ++other) {
                    if (!player_envs[other].info.alive) continue;
                    avg_board_power += board_power(other);
                    ++alive_count;
                }
                if (alive_count > 0) avg_board_power /= alive_count;
                const bool board_stable = power >= avg_board_power * 0.9;
                if (economy.win_streak >= 3 ||
                    (current_stage >= 4 && board_stable) ||
                    current_stage >= 5) {
                    info.composition_locked = true;
                }
            }
        }
        info.composition_focus_weights.assign(focus.begin(), focus.end());

        for (const auto& unit_id : unit_order) {
            info.unit_counts.emplace_back(unit_names[unit_id], copies_by_unit.at(unit_id));
            info.unit_counts_by_id.emplace_back(unit_id, copies_by_unit.at(unit_id));
        }

        if (!focus_scores.empty()) {
            auto best_score = -std::numeric_limits<double>::infinity();
            std::int32_t best_id = -1;
            for (const auto& unit_id : unit_order) {
                const auto found = focus_scores.find(unit_id);
                if (found != focus_scores.end() && found->second > best_score) {
                    best_score = found->second;
                    best_id = unit_id;
                }
            }
            const auto previous = focus_scores.find(previous_focus_id);
            // A challenger must be materially stronger (20% and at least 0.5
            // points) before replacing a still-owned focus unit.
            if (previous != focus_scores.end() &&
                (best_id == previous_focus_id ||
                 best_score < previous->second * 1.20 + 0.5)) {
                best_id = previous_focus_id;
            }
            if (best_id >= 0) {
                info.composition_focus_id = best_id;
                info.composition_focus = unit_names[best_id];
            }
        }

        std::vector<std::pair<std::int32_t, double>> ordered_units;
        ordered_units.reserve(focus_scores.size());
        for (const auto& unit_id : unit_order) {
            const auto found = focus_scores.find(unit_id);
            if (found != focus_scores.end()) ordered_units.emplace_back(unit_id, found->second);
        }
        std::stable_sort(ordered_units.begin(), ordered_units.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
        // Preserve viable core IDs first; fill vacancies with the strongest
        // newly observed units. This avoids core churn on near-tied scores.
        for (const auto id : previous_core_unit_ids) {
            if (info.core_unit_ids.size() >= 3) break;
            if (focus_scores.find(id) == focus_scores.end()) continue;
            if (std::find(info.core_unit_ids.begin(), info.core_unit_ids.end(), id) !=
                info.core_unit_ids.end()) continue;
            info.core_unit_ids.push_back(id);
            info.core_units.push_back(unit_names[id]);
        }
        for (const auto& [id, score] : ordered_units) {
            (void)score;
            if (info.core_unit_ids.size() >= 3) break;
            if (std::find(info.core_unit_ids.begin(), info.core_unit_ids.end(), id) !=
                info.core_unit_ids.end()) continue;
            info.core_unit_ids.push_back(id);
            info.core_units.push_back(unit_names[id]);
        }

        std::vector<std::pair<std::string, int>> ordered_traits;
        ordered_traits.reserve(trait_order.size());
        for (const auto& trait : trait_order)
            ordered_traits.emplace_back(trait, trait_counts.at(trait));
        std::stable_sort(ordered_traits.begin(), ordered_traits.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.second > rhs.second;
            });
        for (const auto& [trait, count] : ordered_traits) {
            if (count >= 2 && info.core_traits.size() < 3)
                info.core_traits.push_back(trait);
        }

        // Build a concrete target lineup from the entire champion pool once a
        // direction exists. The anchor trait guides selection, but the planner
        // deliberately reserves room for a carry, frontline and bridge/support
        // traits instead of forcing every slot into one trait.
        if (current_round >= 9 && !info.composition_direction.empty()) {
            strategy::CompositionPlanner::OwnedCopies owned_copies;
            for (const auto& [unit_id, copies] : copies_by_unit)
                owned_copies[unit_id] = copies;
            const strategy::CompositionPlanner::FocusWeights plan_focus(
                focus.begin(), focus.end());
            // Plan the complete level-8 core even while the player is below
            // level 8. At levels 9/10, recompute a larger plan from the latest
            // direction, focus weights, and owned copies so the extra slots can
            // adapt to the live state instead of being fixed in the early game.
            const auto planning_slots =
                strategy::CompositionPlanner::target_slots_for_level(economy.level);
            strategy::CompositionPlanner::AvailableCopies available_copies;
            std::unordered_set<std::int32_t> currently_offered;
            for (const auto& champion : catalog->all())
                available_copies[champion.internal_id] = pool
                    ? pool->available(champion.internal_id) : 0;
            const auto& current_shop = player_envs[i].shop;
            if (current_shop) {
                for (const auto& offer : current_shop->offers()) {
                    if (!offer) continue;
                    currently_offered.insert(offer->champion_internal_id);
                    available_copies[offer->champion_internal_id] =
                        std::max(1, available_copies[offer->champion_internal_id]);
                }
            }
            strategy::CompositionPlanner::LiveUnitContext live_context;
            const auto& live_player = player_envs[i];
            if (live_player.item_engine) {
                for (const auto& item_id : live_player.item_engine->inventory())
                    live_context.inventory_item_ids.insert(item_id);
                // Count a recommended item as feasible when the player already
                // owns it or currently has both components for its active recipe.
                if (item_catalog) {
                    for (const auto& item : item_catalog->items) {
                        const auto& first = item.recipe_components[0];
                        const auto& second = item.recipe_components[1];
                        if (!first.empty() && !second.empty() &&
                            live_player.item_engine->can_craft(first, second))
                            live_context.inventory_item_ids.insert(item.id);
                    }
                }
                const auto collect_live_unit = [&](const UnitInstance& unit, int board_slot) {
                    auto& equipped = live_context.equipped_items_by_champion[unit.champion_internal_id];
                    for (const auto& item_id : live_player.item_engine->equipped(unit.uuid))
                        equipped.push_back(item_id);
                    for (const auto& item : unit.items)
                        if (!item.api_name.empty()) equipped.push_back(item.api_name);
                    if (board_slot >= 0)
                        live_context.board_slot_by_champion[unit.champion_internal_id] = board_slot;
                };
                for (std::size_t slot = 0; slot < live_player.board.size(); ++slot)
                    if (live_player.board[slot])
                        collect_live_unit(*live_player.board[slot], static_cast<int>(slot));
                for (const auto& unit : live_player.bench)
                    if (unit) collect_live_unit(*unit, -1);
            }
            const auto plan = strategy::CompositionPlanner::build(
                catalog->all(), trait_milestones, info.composition_direction,
                plan_focus, owned_copies, planning_slots, economy.level,
                available_copies, currently_offered, &live_context);
            const bool direction_transitioned =
                info.composition_direction != previous_direction;
            const bool keep_previous_plan = !direction_transitioned &&
                !previous_planned_unit_ids.empty() &&
                !strategy::should_replace_composition_plan(
                    previous_planned_unit_ids, plan.unit_ids,
                    previous_planned_score, plan.score,
                    previous_composition_locked);
            if (keep_previous_plan) {
                info.planned_units = previous_planned_units;
                info.planned_unit_ids = previous_planned_unit_ids;
                info.planned_traits = previous_planned_traits;
                info.planned_main_carry_id = previous_planned_main_carry_id;
                info.planned_composition_score = previous_planned_score;
                info.planned_acquisition_notes.clear();
                for (const auto& note : previous_planned_notes) {
                    if (note.rfind("OPGG_META_CANDIDATE:", 0) == 0)
                        info.planned_acquisition_notes.push_back(note);
                }
                for (const auto& note : plan.acquisition_notes) {
                    if (note.rfind("OPGG_META_AUDIT:", 0) == 0 ||
                        note.rfind("OPGG_META_CANDIDATE:", 0) == 0)
                        info.planned_acquisition_notes.push_back(note);
                }
                info.planned_acquisition_notes.push_back(
                    "COMPOSITION_HYSTERESIS: kept_previous_plan (previous_score=" +
                    std::to_string(previous_planned_score) + ", candidate_score=" +
                    std::to_string(plan.score) + ", direction_locked=" +
                    std::string(previous_composition_locked ? "true" : "false") + ")");
            } else {
                info.planned_units = plan.unit_names;
                info.planned_unit_ids = plan.unit_ids;
                info.planned_traits = plan.core_traits;
                info.planned_main_carry_id = plan.main_carry_id;
                info.planned_composition_score = plan.score;
                info.planned_acquisition_notes = plan.acquisition_notes;
            }
        }

        // Paridade com StrategyManager.compute_strategy_confidence().
        double state_confidence = 0.5;
        if (player.hp <= 20.0f) state_confidence += 0.25;
        else if (player.hp >= 80.0f) state_confidence += 0.10;
        if (economy.gold >= 40) state_confidence += 0.15;
        else if (economy.gold < 10) state_confidence -= 0.10;
        if (power >= 70.0) state_confidence += 0.15;
        else if (power <= 20.0) state_confidence -= 0.15;
        if (economy.win_streak >= 4) state_confidence += 0.10;
        if (economy.loss_streak >= 4) state_confidence += 0.10;
        if (economy.level >= 8) state_confidence += 0.10;
        info.state_confidence = static_cast<float>(std::clamp(state_confidence, 0.0, 1.0));

        // O diagnóstico usa exatamente os scores que escolheram o modo ativo.
        // Não existe mais um segundo seletor divergente do modo executado.
        info.strategy_scores.clear();
        for (std::int32_t strategy_id = 0; strategy_id < 7; ++strategy_id) {
            info.strategy_scores.emplace_back(
                strategy_id,
                strategy_decision.scores[static_cast<std::size_t>(strategy_id)]);
        }
        info.best_scored_strategy = chosen;
        info.active_traits_count = static_cast<std::int32_t>(info.active_traits.size());

        if (list.empty()) list.push_back(info);
        else list.back() = info;
    }

    double state_reward(std::size_t i, double action_reward) {
        const double power = board_power(i);
        const double synergy = synergy_score(i);
        const auto board_eval = board_evaluation(i);
        const double positioning = board_eval.total_score;
        auto& player_info = player_envs[i].info;
        player_info.board_quality_delta =
            positioning - player_envs[i].previous_positioning_score;
        player_info.board_quality_score = positioning;
        player_info.board_positional_score = board_eval.positional_score;
        player_info.board_structural_score = board_eval.structural_score;
        player_info.board_exposed_carries = board_eval.exposed_carry_count;
        player_info.board_frontline_count = board_eval.frontline_count;
        player_info.board_isolated_frontline_count =
            board_eval.isolated_frontline_count;
        player_info.board_important_bench_units_unplayed =
            board_eval.important_bench_units_unplayed;
        double reward = action_reward;

        reward += reward_model::power(power, player_envs[i].previous_board_power);
        player_envs[i].previous_board_power = power;

        reward += reward_model::synergy(synergy, player_envs[i].previous_synergy_score);
        player_envs[i].previous_synergy_score = synergy;

        reward += reward_model::positioning(positioning, player_envs[i].previous_positioning_score);
        player_envs[i].previous_positioning_score = positioning;

        const auto current_level = player_envs[i].economy.level;
        if (current_level > player_envs[i].previous_reward_level) {
            reward += reward_model::level(current_level, player_envs[i].previous_reward_level);
            player_envs[i].previous_reward_level = current_level;
        }

        reward += reward_model::economy(player_envs[i].economy.gold, player_envs[i].info.hp);
        reward += reward_model::streak(
            player_envs[i].economy.win_streak, player_envs[i].economy.loss_streak);

        int occupied = 0;
        for (const auto& unit : player_envs[i].board) if (unit) ++occupied;
        reward += reward_model::board(occupied, static_cast<int>(kBoardSlots));
        return reward;
    }

    void sync_player_economy(std::size_t player_index) {
        if (player_index >= kPlayersPerLobby) {
            throw std::out_of_range(
                "Indice de jogador invalido ao sincronizar economia."
            );
        }

        const auto& economy = player_envs[player_index].economy;
        auto& player = player_envs[player_index].info;

        player.gold = economy.gold;
        player.level = economy.level;
        player.xp = economy.xp;
    }

    bool all_alive_players_finished_round() const {
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (player_envs[i].info.alive && !round_finished[i]) {
                return false;
            }
        }
        return true;
    }

    void mark_round_completion(std::size_t player_index) {
        if (!player_envs[player_index].info.alive) {
            return;
        }

        ++round_actions[player_index];
        if (round_actions[player_index] >= kMaxActionsPerRound) {
            round_finished[player_index] = true;
        }
    }

    void reset_round_completion() {
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (player_envs[i].info.alive) {
                round_actions[i] = 0;
                player_envs[i].actions_taken_this_round = 0;
                round_finished[i] = false;
            } else {
                round_finished[i] = true;
            }
        }
    }

    // Converte a formação atual da lobby para a entrada da engine de combate.
    // A engine de combate permanece independente: recebe equipes e devolve o resultado.
    void append_combat_team(
        std::size_t player_index,
        UnitTeam team,
        CombatTeam& output,
        int& next_unit_id
    ) const {
        output.team = team;
        output.player_id = static_cast<std::int32_t>(player_index);
        output.player_hp = player_envs[player_index].info.hp;

        // Build team-wide trait counts once, including emblems. Counts are
        // unique per champion instance, matching lobby breakpoint semantics.
        std::unordered_map<std::string, int> combat_trait_counts;
        for (const auto& candidate : player_envs[player_index].board) {
            if (!candidate) continue;
            const auto* candidate_def = catalog->find_by_id(candidate->champion_internal_id);
            if (!candidate_def) continue;
            std::unordered_set<std::string> candidate_traits(
                candidate_def->traits.begin(), candidate_def->traits.end());
            for (const auto& item : candidate->items) {
                if (!item.grants_trait.empty()) candidate_traits.insert(item.grants_trait);
            }
            for (const auto& trait : candidate_traits) ++combat_trait_counts[trait];
        }

        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            if (!player_envs[player_index].board[slot]) {
                continue;
            }

            const auto& instance = *player_envs[player_index].board[slot];
            const auto* definition =
                catalog->find_by_id(instance.champion_internal_id);
            if (!definition) {
                continue;
            }

            const auto& stats = definition->stats;
            const auto& profile = definition->combat_profile;

            CombatUnit unit{};
            // Same deterministic sequential IDs as TFTLobby._build_cpp_combat_input().
            unit.id = next_unit_id++;
            unit.api_name = definition->api_name;
            unit.name = definition->name;
            unit.star = std::clamp(instance.star, 1, 3);
            unit.team = team;

            const auto role_value = std::clamp(profile.role, 0, 4);
            unit.role = static_cast<UnitRole>(role_value);

            unit.position.x = static_cast<int>(slot % 7);
            unit.position.y = static_cast<int>(slot / 7);
            // Os tabuleiros ficam frente a frente durante o combate.
            if (team == UnitTeam::PLAYER_B) {
                // Python mirrors columns (x), not rows (y), for team B.
                unit.position.x = 6 - unit.position.x;
            }
            unit.source_board_position = static_cast<int>(slot);

            const double max_hp =
                profile.max_hp > 0.0 ? profile.max_hp : stats.max_hp;
            unit.max_hp = static_cast<float>(std::max(1.0, max_hp > 0.0 ? max_hp : 100.0));
            unit.hp = unit.max_hp;

            const double attack_damage =
                profile.attack_damage > 0.0
                    ? profile.attack_damage
                    : stats.attack_damage;
            // Match TFTLobby._build_cpp_combat_input() fallbacks.
            unit.attack_damage = static_cast<float>(
                std::max(1.0, attack_damage > 0.0 ? attack_damage : 30.0));

            const double attack_speed =
                profile.attack_speed > 0.0
                    ? profile.attack_speed
                    : stats.attack_speed;
            unit.attack_speed = static_cast<float>(
                std::max(0.1, attack_speed > 0.0 ? attack_speed : 1.0));

            const double attack_range =
                profile.attack_range > 0.0
                    ? profile.attack_range
                    : stats.attack_range;
            unit.attack_range = static_cast<float>(
                std::max(1.0, attack_range > 0.0 ? attack_range : 1.0));

            unit.armor = static_cast<float>(
                profile.armor > 0.0 ? profile.armor : stats.armor);
            unit.magic_resist = static_cast<float>(
                profile.magic_resist > 0.0
                    ? profile.magic_resist
                    : stats.magic_resist);

            unit.items = instance.items;

            std::unordered_set<std::string> unit_traits(
                definition->traits.begin(), definition->traits.end());
            for (const auto& item : instance.items) {
                if (!item.grants_trait.empty()) unit_traits.insert(item.grants_trait);
            }
            for (const auto& trait_name : unit_traits) {
                CombatTrait trait;
                trait.api_name = trait_name;
                trait.name = trait_name;
                const auto count = combat_trait_counts.find(trait_name);
                trait.active_count = count == combat_trait_counts.end() ? 0 : count->second;
                trait.active = trait.active_count > 0;
                unit.traits.push_back(std::move(trait));
            }

            unit.alive = true;
            unit.can_move = true;
            unit.can_attack = true;
            unit.can_cast = true;

            output.units.push_back(std::move(unit));
        }
    }

    void return_eliminated_player_units(std::size_t player_index) {
        if (player_envs[player_index].shop) player_envs[player_index].shop->return_current_shop();

        auto return_unit_copies = [this](const std::optional<UnitInstance>& unit) {
            if (!unit) return;
            for (std::int32_t copy = 0; copy < star_copies(unit->star); ++copy)
                (void)pool->give_back(unit->champion_internal_id);
        };
        for (auto& unit : player_envs[player_index].board) {
            return_unit_copies(unit);
            unit.reset();
        }
        for (auto& unit : player_envs[player_index].bench) {
            return_unit_copies(unit);
            unit.reset();
        }
        register_position_state(player_index);
    }

    void eliminate_players(std::vector<std::string>& events) {
        // Python define as colocações após todos os combates, em ordem de ID.
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            auto& player = player_envs[i].info;
            if (!player.alive || player.hp > 0.0f) continue;
            player.hp = 0.0f;
            player.alive = false;
            player.eliminated = true;
            player.placement = alive_players;
            ranking.push_back(static_cast<std::int32_t>(i));
            --alive_players;
            round_finished[i] = true;
            return_eliminated_player_units(i);
            events.emplace_back("ELIMINATED: jogador " + std::to_string(i));
        }
    }

    void finish_if_one_player_left() {
        if (alive_players > 1) return;
        done = true;
        winner_id.reset();
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (player_envs[i].info.alive) {
                winner_id = static_cast<std::int32_t>(i);
                player_envs[i].info.placement = 1;
                if (std::find(ranking.begin(), ranking.end(),
                              static_cast<std::int32_t>(i)) == ranking.end())
                    ranking.insert(ranking.begin(), static_cast<std::int32_t>(i));
                break;
            }
        }
        if (!winner_id && ranking.size() < kPlayersPerLobby) {
            for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
                if (std::find(ranking.begin(), ranking.end(),
                              static_cast<std::int32_t>(i)) == ranking.end()) {
                    player_envs[i].info.placement = static_cast<std::int32_t>(
                        kPlayersPerLobby - ranking.size());
                    ranking.push_back(static_cast<std::int32_t>(i));
                }
            }
        }

        // Public ranking is ordered best-to-worst, matching Python's
        // TFTLobby._finalize_ranking(). Eliminations are appended in the
        // order they happen (worst placement first), so insertion order is
        // not a valid final ranking once the winner is prepended.
        const auto placement_for = [this](std::int32_t player_id) {
            const auto& placement =
                player_envs[static_cast<std::size_t>(player_id)].info.placement;
            return placement.value_or(
                static_cast<std::int32_t>(kPlayersPerLobby + 1));
        };
        std::stable_sort(
            ranking.begin(),
            ranking.end(),
            [&](std::int32_t lhs, std::int32_t rhs) {
                const auto lhs_placement = placement_for(lhs);
                const auto rhs_placement = placement_for(rhs);
                if (lhs_placement != rhs_placement) {
                    return lhs_placement < rhs_placement;
                }
                return lhs < rhs;
            });
    }

    void resolve_combats(
        std::vector<std::string>& events,
        std::array<float, kPlayersPerLobby>& rewards,
        std::array<float, kPlayersPerLobby>& combat_rewards
    ) {
        std::vector<std::size_t> alive;
        alive.reserve(kPlayersPerLobby);
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            player_envs[i].info.opponent_id.reset();
            if (player_envs[i].info.alive) alive.push_back(i);
        }
        if (alive.size() < 2) {
            finish_if_one_player_left();
            return;
        }

        std::shuffle(alive.begin(), alive.end(), matchmaking_rng);
        std::vector<std::pair<std::size_t, std::size_t>> matchups;
        for (std::size_t i = 0; i + 1 < alive.size(); i += 2) {
            const auto a = alive[i];
            auto b = alive[i + 1];
            if (alive.size() > 2 && previous_matchups.contains(matchup_key(a, b))) {
                for (std::size_t j = i + 2; j < alive.size(); ++j) {
                    if (!previous_matchups.contains(matchup_key(a, alive[j]))) {
                        std::swap(alive[i + 1], alive[j]);
                        b = alive[i + 1];
                        break;
                    }
                }
            }
            matchups.emplace_back(a, b);
            player_envs[a].info.opponent_id = static_cast<std::int32_t>(b);
            player_envs[b].info.opponent_id = static_cast<std::int32_t>(a);
            previous_matchups.insert(matchup_key(a, b));
        }

        auto apply_damage = [&](std::size_t player_index, std::int32_t damage) {
            auto& player = player_envs[player_index].info;
            if (!player.alive || damage <= 0) return;
            const float applied = std::min(player.hp, static_cast<float>(damage));
            player.hp = std::max(0.0f, player.hp - applied);
            player.damage_taken += applied;
        };

        for (const auto& [a, b] : matchups) {
            if (!player_envs[a].info.alive || !player_envs[b].info.alive) continue;
            CombatInput input{};
            input.context = CombatContext::PVP;
            // Python TFTLobby sends the lobby seed unchanged for each
            // deterministic combat; unit IDs are sequential across both teams.
            input.seed = seed;
            input.config.seed = seed;
            input.config.board_rows = 4;
            input.config.board_cols = 7;
            input.config.max_duration = 30.0f;
            input.config.max_events = 100000;
            input.config.deterministic = true;
            int next_unit_id = 0;
            append_combat_team(a, UnitTeam::PLAYER_A, input.player_a, next_unit_id);
            append_combat_team(b, UnitTeam::PLAYER_B, input.player_b, next_unit_id);
            const auto record_combat_item_inputs = [&](std::size_t player_id,
                                                        const auto& team) {
                for (const auto& unit : team.units) {
                    for (const auto& item : unit.items) {
                        events.emplace_back(
                            "COMBAT_ITEM_INPUT: player=" + std::to_string(player_id) +
                            ", unit_id=" + std::to_string(unit.id) +
                            ", item=" + item.api_name);
                    }
                }
            };
            record_combat_item_inputs(a, input.player_a);
            record_combat_item_inputs(b, input.player_b);

            const bool empty_a = input.player_a.units.empty();
            const bool empty_b = input.player_b.units.empty();
            if (empty_a && empty_b) {
                // Uma partida sem unidades dos dois lados não pode ficar em
                // empate infinito. Não há vencedor nem bônus de streak, mas
                // o dano de rodada ainda avança a partida até a classificação.
                events.emplace_back("COMBAT_DRAW_EMPTY: jogadores " +
                    std::to_string(a) + " e " + std::to_string(b) +
                    "; dano de rodada aplicado aos dois");
                const auto draw_damage = stage_damage(stage);
                apply_damage(a, draw_damage);
                apply_damage(b, draw_damage);
                sync_player_economy(a);
                sync_player_economy(b);
                continue;
            }

            std::size_t winner = 0, loser = 0;
            std::int32_t winner_survivors = 0;
            std::int32_t loser_survivors = 0;
            if (empty_a || empty_b) {
                winner = empty_a ? b : a;
                loser = empty_a ? a : b;
                winner_survivors = static_cast<std::int32_t>(
                    empty_a ? input.player_b.units.size() : input.player_a.units.size());
                events.emplace_back("AUTO_WIN_EMPTY_BOARD: jogador " +
                    std::to_string(winner) + " venceu jogador " + std::to_string(loser));
            } else {
                const auto combat = combat_engine.simulate(input);
                if (!combat.success || !combat.completed)
                    throw std::runtime_error("CombatEngine falhou no confronto " +
                        std::to_string(a) + " vs " + std::to_string(b) +
                        ": " + combat.error_code + " - " + combat.error_message);
                events.emplace_back("COMBAT: jogador " + std::to_string(a) +
                    " vs " + std::to_string(b) + ", eventos=" +
                    std::to_string(combat.events_processed));
                if (combat.timeout) {
                    events.emplace_back(
                        "COMBAT_TIMEOUT: resultado parcial aceito para " +
                        std::to_string(a) + " vs " + std::to_string(b));
                }
                if (combat.winner == CombatWinner::PLAYER_A) {
                    winner = a; loser = b;
                    winner_survivors = combat.player_a_survivors;
                    loser_survivors = combat.player_b_survivors;
                } else if (combat.winner == CombatWinner::PLAYER_B) {
                    winner = b; loser = a;
                    winner_survivors = combat.player_b_survivors;
                    loser_survivors = combat.player_a_survivors;
                } else {
                    // Um empate do simulador (por exemplo, timeout simultâneo)
                    // não pode bloquear a lobby indefinidamente. Sem vencedor
                    // e sem bônus de streak, ambos recebem o dano de rodada
                    // acrescido dos sobreviventes adversários.
                    const auto damage_to_a = stage_damage(stage) +
                        std::max(0, combat.player_b_survivors);
                    const auto damage_to_b = stage_damage(stage) +
                        std::max(0, combat.player_a_survivors);
                    apply_damage(a, damage_to_a);
                    apply_damage(b, damage_to_b);
                    sync_player_economy(a);
                    sync_player_economy(b);
                    events.emplace_back("COMBAT_DRAW_DAMAGE: jogadores " +
                        std::to_string(a) + " e " + std::to_string(b) +
                        ", dano=" + std::to_string(damage_to_a) +
                        "/" + std::to_string(damage_to_b));
                    continue;
                }
            }

            ++player_envs[winner].info.wins;
            ++player_envs[loser].info.losses;
            economy_engine.register_pvp_win(player_envs[winner].economy);
            economy_engine.register_loss(player_envs[loser].economy);
            const auto damage = stage_damage(stage) + std::max(0, winner_survivors);
            apply_damage(loser, damage);
            const double winner_reward = combat_reward(
                true, damage, 0, loser_survivors, winner_survivors);
            const double loser_reward = combat_reward(
                false, 0, damage, winner_survivors, loser_survivors);
            player_envs[winner].last_combat_reward = winner_reward;
            player_envs[loser].last_combat_reward = loser_reward;
            combat_rewards[winner] += static_cast<float>(winner_reward);
            combat_rewards[loser] += static_cast<float>(loser_reward);
            rewards[winner] += static_cast<float>(winner_reward);
            rewards[loser] += static_cast<float>(loser_reward);
            events.emplace_back("COMBAT_WIN: jogador " + std::to_string(winner) +
                ", dano=" + std::to_string(damage) +
                ", sobreviventes=" + std::to_string(winner_survivors));
            sync_player_economy(winner);
            sync_player_economy(loser);
        }

        eliminate_players(events);
        finish_if_one_player_left();
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) refresh_masks(i);
    }
};

TFTEngine::Impl::PlayerTransition TFTEngine::Impl::PlayerEnv::step(
    TFTEngine::Impl& lobby,
    std::size_t player_id,
    const PlayerAction& player_action
) {

        const auto& action = player_action.values;

        const auto type = action[0];
        if (!movement_sequence_round_initialized ||
            movement_sequence_round != lobby.round) {
            movement_sequence_round = lobby.round;
            movement_sequence_round_initialized = true;
            movement_sequence_length = 0;
        }
        last_item_action = {};

        const auto shop_slot = action[1];
        actions_taken_this_round = std::min<std::uint32_t>(
            kActionMemoryHorizon, actions_taken_this_round + 1);
        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            if (board[slot]) {
                board_move_age[slot] = std::min<std::uint32_t>(
                    kActionMemoryHorizon, board_move_age[slot] + 1);
            } else {
                board_move_age[slot] = kActionMemoryHorizon;
            }
        }

        auto& economy = this->economy;
        auto& shop = *this->shop;
        ++this->action_serial;
        this->recent_purchases.erase(
            std::remove_if(this->recent_purchases.begin(), this->recent_purchases.end(),
                [this](const RecentPurchase& purchase) {
                    return this->action_serial - purchase.action_serial >
                        reward_model::kRecentPurchaseHistoryActions;
                }),
            this->recent_purchases.end());
        const auto old_gold = economy.gold;
        const auto old_level = economy.level;
        const auto old_xp = economy.xp;
        int old_board_count = 0, old_bench_count = 0;
        const double old_synergy_score = lobby.synergy_score(player_id);
        const double old_positioning_score = lobby.positioning_score(player_id);
        bool sold_core_unit = false;
        bool sold_unit_protects_upgrade = false;
        double sold_unit_upgrade_strength = 0.0;
        double sold_unit_fit_score = 0.0;
        bool displaced_core_unit = false;
        bool buy_completes_upgrade = false;
        bool buy_builds_pair = false;
        bool buy_strategy_approved = true;
        double candidate_board_impact = 0.0;
        double item_assignment_reward = 0.0;
        double reroll_old_best_fit = 0.0;
        double reroll_new_best_fit = 0.0;
        bool bought_unit_successfully = false;
        bool bought_upgrade = false;
        bool purchase_was_recently_reversed = false;
        bool sale_has_legitimate_context = false;
        bool sale_bench_was_full = false;
        double reversed_purchase_prior_reward = 0.0;
        bool board_capacity_violation = false;
        bool anti_loop_blocked = false;
        std::array<std::string, kBoardSlots> old_board_layout{};
        for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
            if (this->board[slot]) {
                ++old_board_count;
                old_board_layout[slot] = this->board[slot]->uuid;
            }
        }
        for (const auto& unit : this->bench) if (unit) ++old_bench_count;

        // Compare a candidate against the current composition before mutating
        // the board. This makes a replacement's reward relative, not absolute.
        const StrategyInfo strategy_before = this->strategy.empty()
            ? StrategyInfo{} : this->strategy.back();
        double board_comp_score_sum = 0.0;
        int board_comp_score_count = 0;
        for (const auto& unit : this->board) {
            if (!unit) continue;
            const auto* def = lobby.catalog->find_by_id(unit->champion_internal_id);
            if (!def) continue;
            board_comp_score_sum += unit_composition_score(strategy_before, *def, unit->star, lobby.trait_milestones);
            ++board_comp_score_count;
        }
        double candidate_comp_score = 0.0;
        double displaced_comp_score = 0.0;
        bool has_candidate_comp_score = false;
        bool has_displaced_comp_score = false;
        if (type == 0) {
            // BUY: avalia a oferta contra a direção atual da composição,
            // para que comprar qualquer unidade não tenha sempre a mesma recompensa.
            const auto* offer = shop.get_unit(shop_slot);
            if (offer) {
                const auto* def = lobby.catalog->find_by_id(
                    offer->champion_internal_id);
                if (def) {
                    candidate_comp_score = unit_composition_score(strategy_before, *def, offer->star, lobby.trait_milestones);
                    has_candidate_comp_score = true;
                    int same_star_copies = 0;
                    for (const auto& unit : this->board) {
                        if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                            unit->star == offer->star) ++same_star_copies;
                    }
                    for (const auto& unit : this->bench) {
                        if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                            unit->star == offer->star) ++same_star_copies;
                    }
                    buy_completes_upgrade = same_star_copies >= 2;
                    candidate_board_impact = unit_board_impact_score(
                        strategy_before, *def, offer->star, lobby.trait_milestones,
                        old_board_count, economy.level, static_cast<int>(stage_for_round(lobby.round + 1)));
                    const double upgrade_value = same_star_copies >= 3 ? 1.0
                        : (same_star_copies == 2 ? 0.6 : (same_star_copies == 1 ? 0.2 : 0.0));
                    const bool in_top_trait = (!strategy_before.composition_direction.empty() &&
                        std::find(def->traits.begin(), def->traits.end(),
                            strategy_before.composition_direction) != def->traits.end()) ||
                        std::any_of(def->traits.begin(), def->traits.end(), [&](const auto& trait) {
                            return std::find(strategy_before.core_traits.begin(),
                                strategy_before.core_traits.end(), trait) != strategy_before.core_traits.end();
                        });
                    // A unit explicitly selected by the current plan (including
                    // an OP.GG-derived plan) is strategically relevant even when
                    // it does not share the single anchor trait.
                    const bool in_planned_comp =
                        std::find(strategy_before.core_unit_ids.begin(),
                            strategy_before.core_unit_ids.end(), def->internal_id) !=
                                strategy_before.core_unit_ids.end() ||
                        std::find(strategy_before.planned_unit_ids.begin(),
                            strategy_before.planned_unit_ids.end(), def->internal_id) !=
                                strategy_before.planned_unit_ids.end();
                    const int offer_stage =
                        static_cast<int>(stage_for_round(lobby.round + 1));
                    const bool early_flex_purchase =
                        offer_stage <= 2 && def->cost <= 2 &&
                        old_bench_count < 6 && candidate_comp_score >= -2.0;
                    buy_strategy_approved = should_buy_unit(candidate_board_impact, upgrade_value,
                        in_top_trait || in_planned_comp, old_bench_count,
                        offer_stage, def->cost) || early_flex_purchase;
                    // A second matching copy has real upgrade option value
                    // even if temporary comp fit is negative. Fit still scales
                    // the pair reward; it does not gate pair recognition.
                    buy_builds_pair = reward_model::is_pair_purchase(same_star_copies, offer->star);
                }
            }
        } else if (type == 3) {
            const auto source = static_cast<std::size_t>(action[2]);
            const auto target = static_cast<std::size_t>(action[3]);
            if (source < this->bench.size() && this->bench[source]) {
                const auto* def = lobby.catalog->find_by_id(
                    this->bench[source]->champion_internal_id);
                if (def) {
                    candidate_comp_score = unit_composition_score(strategy_before, *def, this->bench[source]->star, lobby.trait_milestones);
                    has_candidate_comp_score = true;
                }
            }
            if (target < this->board.size() && this->board[target]) {
                const auto* def = lobby.catalog->find_by_id(
                    this->board[target]->champion_internal_id);
                if (def) {
                    displaced_comp_score = unit_composition_score(strategy_before, *def, this->board[target]->star, lobby.trait_milestones);
                    has_displaced_comp_score = true;
                    displaced_core_unit =
                        std::find(strategy_before.core_unit_ids.begin(),
                            strategy_before.core_unit_ids.end(), def->internal_id) !=
                                strategy_before.core_unit_ids.end() ||
                        std::find(strategy_before.planned_unit_ids.begin(),
                            strategy_before.planned_unit_ids.end(), def->internal_id) !=
                                strategy_before.planned_unit_ids.end();
                }
            }
        }
        double composition_quality_delta = 0.0;
        if (has_candidate_comp_score) {
            if (has_displaced_comp_score) {
                composition_quality_delta =
                    candidate_comp_score - displaced_comp_score;
            } else if (board_comp_score_count > 0) {
                composition_quality_delta = candidate_comp_score -
                    board_comp_score_sum / board_comp_score_count;
            } else {
                composition_quality_delta = candidate_comp_score;
            }
        }

        if (!this->info.alive) {
            lobby.refresh_masks(player_id);
            return PlayerTransition{0.0f, false};
        }

        if (type == 0) {
            // BUY
            const auto* offer = shop.get_unit(shop_slot);

            std::size_t free_slot = kBenchSlots;

            for (std::size_t slot = 0;
                 slot < kBenchSlots;
                 ++slot) {
                if (!this->bench[slot]) {
                    free_slot = slot;
                    break;
                }
            }

            if (offer && free_slot < kBenchSlots) {
                const auto* definition =
                    lobby.catalog->find_by_id(
                        offer->champion_internal_id
                    );

                if (definition &&
                    economy.gold >= definition->cost &&
                    lobby.economy_engine.spend_gold(
                        economy,
                        definition->cost
                    )) {
                    auto purchased =
                        shop.take_unit(shop_slot);

                    if (purchased) {
                        bought_unit_successfully = true;
                        this->bench[free_slot] = std::move(*purchased);

                        // Auto-upgrade 3x1★ -> 2★ e 3x2★ -> 3★.
                        // O pool não muda: as cópias continuam pertencendo ao jogador.
                        auto& bought_unit = this->bench[free_slot];
                        while (bought_unit && bought_unit->star < 3) {
                            const auto champion_id = bought_unit->champion_internal_id;
                            const auto star = bought_unit->star;
                            std::vector<std::pair<bool, std::size_t>> matches;
                            matches.emplace_back(false, free_slot);
                            for (std::size_t slot = 0; slot < kBenchSlots; ++slot) {
                                if (slot != free_slot && this->bench[slot] &&
                                    this->bench[slot]->champion_internal_id == champion_id &&
                                    this->bench[slot]->star == star)
                                    matches.emplace_back(false, slot);
                            }
                            for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
                                if (this->board[slot] &&
                                    this->board[slot]->champion_internal_id == champion_id &&
                                    this->board[slot]->star == star)
                                    matches.emplace_back(true, slot);
                            }
                            if (matches.size() < 3) break;
                            bought_upgrade = true;
                            std::size_t removed = 0;
                            for (const auto& [on_board, slot] : matches) {
                                if (!on_board && slot == free_slot) continue;
                                auto& consumed = on_board ? this->board[slot] : this->bench[slot];
                                if (consumed && this->item_engine) {
                                    (void)this->item_engine->transfer_equipped_on_merge(
                                        consumed->uuid, bought_unit->uuid);
                                }
                                if (on_board) this->board[slot].reset();
                                else this->bench[slot].reset();
                                if (++removed == 2) break;
                            }
                            bought_unit->star = star + 1;
                            if (this->item_engine)
                                lobby.sync_unit_items_from_engine(player_id, bought_unit->uuid);
                        }
                        if (bought_unit && !bought_upgrade) {
                            this->recent_purchases.push_back(RecentPurchase{
                                bought_unit->uuid,
                                bought_unit->champion_internal_id,
                                bought_unit->star,
                                this->action_serial,
                                definition->cost,
                                buy_builds_pair,
                                false,
                                false,
                                std::clamp(std::max(0.0, candidate_comp_score) / 40.0, 0.0, 1.0),
                                0.0,
                                strategy_before.composition_direction,
                                strategy_before.composition_focus
                            });
                        }
                        lobby.register_position_state(player_id);
                    } else {
                        lobby.economy_engine.add_gold(
                            economy,
                            definition->cost
                        );
                    }
                }
            }
        } else if (type == 1) {
            // REROLL: compare the best composition-fit offer before and after.
            // The spend is justified by improved options, not by rerolling itself.
            const auto best_shop_fit = [&](int available_gold) {
                double best = 0.0;
                for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
                    const auto* offer = shop.get_unit(static_cast<std::int32_t>(slot));
                    if (!offer) continue;
                    const auto* def = lobby.catalog->find_by_id(offer->champion_internal_id);
                    if (!def || available_gold < def->cost) continue;
                    double value = unit_composition_score(
                        strategy_before, *def, offer->star, lobby.trait_milestones);
                    int owned_same_star = 0;
                    for (const auto& unit : this->board)
                        if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                            unit->star == offer->star) ++owned_same_star;
                    for (const auto& unit : this->bench)
                        if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                            unit->star == offer->star) ++owned_same_star;
                    // A reroll must recognize upgrade progress, not only the
                    // highest single-unit fit. The third matching copy is much
                    // more valuable than an isolated offer with a similar score.
                    if (offer->star == 1 && owned_same_star == 1) value += 3.0;
                    else if (offer->star == 1 && owned_same_star == 2) value += 7.0;
                    best = std::max(best, value);
                }
                return best;
            };
            const bool fast8_before_level_eight =
                !this->strategy.empty() && this->strategy.back().strategy == 2 &&
                economy.level < 8;
            if (economy.gold >= EconomyEngine::REROLL_COST &&
                !fast8_before_level_eight) {
                // Compare both shops with the same post-reroll cash balance.
                // Otherwise the new shop is unfairly downgraded just because
                // the reroll itself removed two gold before its score is computed.
                const int comparison_gold = old_gold - EconomyEngine::REROLL_COST;
                reroll_old_best_fit = best_shop_fit(comparison_gold);
                if (lobby.economy_engine.spend_gold(economy, EconomyEngine::REROLL_COST)) {
                    shop.reroll(economy.level);
                    reroll_new_best_fit = best_shop_fit(comparison_gold);
                }
            }
        } else if (type == 2) {
            // BUY XP
            (void)lobby.economy_engine.buy_xp(economy);
        } else if (type == 3) {
            // MOVE_BENCH: implantação ou substituição de uma unidade.
            auto& anti_loop = this->anti_loop;
            anti_loop.record_execution_attempt();

            const auto source = action[2];
            const auto target = action[3];
            const auto state = lobby.build_decision_state(player_id);
            const auto validation =
                decision::MoveValidator::validate_bench_to_board_move(
                    state.board,
                    state.bench,
                    source,
                    target
                );

            if (validation.is_valid()) {
                const auto source_index = static_cast<std::size_t>(source);
                const auto target_index = static_cast<std::size_t>(target);

                // Uma casa vazia acrescenta uma unidade à equipe. Se o limite
                // do nível já foi atingido, rejeita a ação e aplica -0.25.
                if (
                    !state.board[target_index].has_value()
                    && old_board_count >= economy.level
                ) {
                    board_capacity_violation = true;
                } else {
                    const auto unit_identity =
                        std::optional<decision::UnitIdentity>{
                            state.bench[source_index]->identity
                        };
                    const auto displaced_unit_identity =
                        state.board[target_index].has_value()
                            ? std::optional<decision::UnitIdentity>{
                                  state.board[target_index]->identity
                              }
                            : std::nullopt;
                    const std::string move_type = "bench_to_board";
                    std::string block_reason;
                    const bool move_allowed =
                        lobby.action_mask_engine.is_bench_to_board_move_allowed(
                            state, anti_loop, source, target, &block_reason
                        );

                    if (!move_allowed) {
                        anti_loop_blocked = true;
                        (void)anti_loop.block_move(
                            move_type,
                            source,
                            target,
                            unit_identity
                        );
                        anti_loop.record_execution_block(
                            block_reason.empty() ? "cycle" : block_reason
                        );
                    } else {
                        const std::string moved_uuid = this->bench[source_index]
                            ? this->bench[source_index]->uuid : std::string{};
                        for (auto& purchase : this->recent_purchases) {
                            if (!moved_uuid.empty() && purchase.uuid == moved_uuid)
                                purchase.was_deployed = true;
                        }
                        (void)anti_loop.register_move(
                            move_type,
                            source,
                            target,
                            unit_identity,
                            displaced_unit_identity
                        );

                        if (this->board[target_index].has_value()) {
                            // Substituição: a unidade retirada volta ao slot
                            // do banco que acabou de ser liberado.
                            std::swap(
                                this->board[target_index],
                                this->bench[source_index]
                            );
                        } else {
                            this->board[target_index] =
                                std::move(this->bench[source_index]);
                            this->bench[source_index].reset();
                        }

                        anti_loop.record_successful_move(move_type);
                        board_move_age[target_index] = 0;
                        lobby.register_position_state(player_id);
                    }
                }
            }

        } else if (type == 4) {
            // MOVE_BOARD: movimento ou troca
            auto& anti_loop = this->anti_loop;

            anti_loop.record_execution_attempt();

            const auto source = action[4];
            const auto target = action[5];

            const auto state =
                lobby.build_decision_state(player_id);

            const auto validation =
                decision::MoveValidator::validate_board_move(
                    state.board,
                    source,
                    target
                );

            if (validation.is_valid() &&
                validation.move_type.has_value()) {
                const auto source_index =
                    static_cast<std::size_t>(source);

                const auto target_index =
                    static_cast<std::size_t>(target);

                const auto unit_identity =
                    std::optional<decision::UnitIdentity>{
                        state.board[source_index]->identity
                    };

                const std::string move_type =
                    *validation.move_type ==
                        decision::MoveType::BOARD_SWAP
                        ? "board_swap"
                        : "board_move";

                std::string block_reason;
                const bool move_allowed =
                    lobby.action_mask_engine.is_board_move_allowed(
                        state, anti_loop, source, target, &block_reason
                    );

                if (!move_allowed) {
                    anti_loop_blocked = true;
                    (void)anti_loop.block_move(
                        move_type,
                        source,
                        target,
                        unit_identity
                    );

                    anti_loop.record_execution_block(
                        block_reason.empty() ? "cycle" : block_reason
                    );
                } else {
                    (void)anti_loop.register_move(
                        move_type,
                        source,
                        target,
                        unit_identity
                    );

                    if (move_type == "board_swap") {
                        std::swap(
                            this->board[source_index],
                            this->board[target_index]
                        );
                    } else {
                        this->board[target_index] =
                            std::move(
                                this->board[source_index]
                            );

                        this->board[source_index].reset();
                    }

                    anti_loop.record_successful_move(move_type);
                    board_move_age[target_index] = 0;
                    if (move_type == "board_swap") {
                        board_move_age[source_index] = 0;
                    } else {
                        board_move_age[source_index] = kActionMemoryHorizon;
                    }

                    lobby.register_position_state(player_id);
                }
            }
        } else if (type == 7 || type == 8) {
            if (!this->item_engine) throw std::invalid_argument("Catalogo de itens nao configurado.");
            if (type == 7 && this->item_engine->has_pending_anvil()) {
                // The option index is the only meaningful argument while an
                // anvil is pending; target-head index zero is a masked sentinel.
                const auto* pending = this->item_engine->pending_anvil();
                const auto option = static_cast<std::size_t>(action[1]);
                if (!pending || option >= pending->options.size())
                    throw std::invalid_argument("Opcao de bigorna fora das opcoes oferecidas.");
                last_item_action.kind = pending->kind == items::ItemAnvilKind::COMPONENT
                    ? "anvil_component_choice" : "anvil_completed_item_choice";
                last_item_action.item_id = pending->options[option];
                if (!this->item_engine->choose_anvil_option(option))
                    throw std::invalid_argument("Escolha de opcao da bigorna rejeitada.");
                // Anvil selection is a grant, not an equip or craft operation.
            } else {
                const auto target_index = static_cast<std::size_t>(type == 7 ? action[2] : action[3]);
                auto* target_unit = lobby.item_target_unit(player_id, target_index);
                if (!target_unit) throw std::invalid_argument("Alvo de item nao existe.");
                const auto profiles = lobby.item_unit_profiles(player_id);
                items::ItemActionCommand command;
                command.operation = type == 7 ? items::ItemOperation::EQUIP_FROM_INVENTORY
                                               : items::ItemOperation::CRAFT_AND_EQUIP;
                command.target_unit_uuid = target_unit->uuid;
                last_item_action.kind = type == 7 ? "equip_inventory_item" : "craft_and_equip";
                last_item_action.target_uuid = target_unit->uuid;
                last_item_action.target_slot = static_cast<std::int32_t>(target_index < kBoardSlots
                    ? target_index : target_index - kBoardSlots);
                last_item_action.target_on_board = target_index < kBoardSlots;
                if (type == 7) {
                    const auto slot = static_cast<std::size_t>(action[1]);
                    if (slot >= this->item_engine->inventory().size()) throw std::invalid_argument("Indice de item fora do inventario.");
                    command.item_id = this->item_engine->inventory()[slot];
                    last_item_action.item_id = command.item_id;
                } else {
                    const auto first = static_cast<std::size_t>(action[1]);
                    const auto second = static_cast<std::size_t>(action[2]);
                    if (!lobby.item_catalog || first >= lobby.item_catalog->components.size() ||
                        second >= lobby.item_catalog->components.size())
                        throw std::invalid_argument("Indice de tipo de componente fora do catalogo.");
                    command.first_component_id = lobby.item_catalog->components[first].first;
                    command.second_component_id = lobby.item_catalog->components[second].first;
                    last_item_action.first_component = command.first_component_id;
                    last_item_action.second_component = command.second_component_id;
                }
                const auto result = this->item_engine->apply_action(command, profiles);
                if (!result.success) throw std::invalid_argument("Acao de item rejeitada: " + result.reason);

                // Score the actual catalog item and selected unit after the action
                // succeeds. Combat remains the source of truth for the long-term value.
                const auto& equipped_items = this->item_engine->equipped(target_unit->uuid);
                const auto target_profile = std::find_if(
                    profiles.begin(), profiles.end(),
                    [&](const items::UnitProfile& profile) { return profile.uuid == target_unit->uuid; });
                if (!equipped_items.empty() && target_profile != profiles.end()) {
                    const auto* item_definition = this->item_engine->find(equipped_items.back());
                    if (item_definition) {
                        double role_alignment = 0.0;
                        switch (target_profile->role) {
                            case 0: role_alignment = item_definition->tank_weight; break;
                            case 1: role_alignment = item_definition->carry_weight; break;
                            case 2: role_alignment = item_definition->support_weight; break;
                            case 3: role_alignment = std::max(
                                item_definition->carry_weight, item_definition->tank_weight); break;
                            case 4: role_alignment = item_definition->carry_weight; break;
                            default: role_alignment = item_definition->utility_weight; break;
                        }
                        const double numeric_strength = reward_model::item_immediate_strength(
                            item_definition->attack_damage,
                            item_definition->attack_speed,
                            item_definition->max_health,
                            item_definition->armor,
                            item_definition->critical_strike_chance,
                            item_definition->critical_strike_damage,
                            item_definition->omnivamp,
                            item_definition->damage_amplification,
                            item_definition->damage_reduction,
                            item_definition->ability_power,
                            item_definition->magic_resist,
                            item_definition->mana);
                        const double supported_effect_strength =
                            this->item_engine->item_effect_strength(
                                item_definition->id, *target_profile);
                        // Combine numeric stats and explicitly simulated passives
                        // without counting the same effect additively twice.
                        const double immediate_strength = 1.0 -
                            (1.0 - std::clamp(numeric_strength, 0.0, 1.0)) *
                            (1.0 - std::clamp(supported_effect_strength, 0.0, 1.0));
                        item_assignment_reward = reward_model::item_assignment_quality(
                            immediate_strength, target_profile->composition_fit,
                            role_alignment, static_cast<int>(lobby.stage));
                        if (type == 8) {
                            item_assignment_reward +=
                                reward_model::item_crafting_bonus(item_assignment_reward);
                        }
                        last_item_action.item_id = item_definition->id;
                        last_item_action.fit = this->item_engine->item_fit(
                            item_definition->id, *target_profile);
                        last_item_action.immediate_strength = immediate_strength;
                        last_item_action.role_alignment = role_alignment;
                        last_item_action.assignment_reward = item_assignment_reward;
                    }
                }
                lobby.sync_unit_items_from_engine(player_id, target_unit->uuid);
            }
        } else if (type == 5) {
            // SELL: vender unidade do banco
            const auto bench_slot =
                static_cast<std::size_t>(action[2]);

            if (bench_slot < kBenchSlots &&
                this->bench[bench_slot]) {
                const auto& unit =
                    *this->bench[bench_slot];

                const auto* definition =
                    lobby.catalog->find_by_id(
                        unit.champion_internal_id
                    );

                if (definition) {
                    const std::string sold_uuid = unit.uuid;
                    const auto sell_value = EconomyEngine::get_sell_value(
                        definition->cost, unit.star);
                    sale_bench_was_full =
                        old_bench_count >= static_cast<int>(kBenchSlots);
                    const int gold_after_sale = old_gold + sell_value;
                    const bool sale_economic_need =
                        (old_gold < EconomyEngine::REROLL_COST &&
                         gold_after_sale >= EconomyEngine::REROLL_COST) ||
                        (old_gold < EconomyEngine::BUY_XP_COST &&
                         gold_after_sale >= EconomyEngine::BUY_XP_COST);
                    // Crossing a spending threshold is not, by itself, a
                    // strategic reason to discard a core unit or an upgrade path.
                    // It remains available to the recent-purchase reversal check,
                    // but does not weaken the unit-retention signal.
                    sale_has_legitimate_context = sale_bench_was_full;
                    const auto recent = std::find_if(
                        this->recent_purchases.rbegin(), this->recent_purchases.rend(),
                        [&sold_uuid](const RecentPurchase& purchase) {
                            return !sold_uuid.empty() && purchase.uuid == sold_uuid;
                        });
                    if (recent != this->recent_purchases.rend()) {
                        const auto& current_strategy = this->strategy.empty()
                            ? StrategyInfo{} : this->strategy.back();
                        const bool still_core_or_planned =
                            std::find(current_strategy.core_unit_ids.begin(),
                                      current_strategy.core_unit_ids.end(),
                                      unit.champion_internal_id) != current_strategy.core_unit_ids.end() ||
                            std::find(current_strategy.planned_unit_ids.begin(),
                                      current_strategy.planned_unit_ids.end(),
                                      unit.champion_internal_id) != current_strategy.planned_unit_ids.end();
                        const bool composition_changed =
                            (recent->comp_direction != current_strategy.composition_direction ||
                             recent->comp_focus != current_strategy.composition_focus) &&
                            (!recent->comp_direction.empty() || !recent->comp_focus.empty()) &&
                            !still_core_or_planned;
                        sale_has_legitimate_context = sale_has_legitimate_context ||
                            composition_changed || recent->was_deployed;

                        // An experiment is justified when a later, better-fit
                        // purchase is still owned. This preserves flexibility:
                        // discovering a stronger option is not a buy/sell mistake.
                        const auto is_still_owned = [this](const std::string& uuid) {
                            if (uuid.empty()) return false;
                            const auto in_board = std::any_of(
                                this->board.begin(), this->board.end(),
                                [&uuid](const auto& candidate) {
                                    return candidate && candidate->uuid == uuid;
                                });
                            const auto in_bench = std::any_of(
                                this->bench.begin(), this->bench.end(),
                                [&uuid](const auto& candidate) {
                                    return candidate && candidate->uuid == uuid;
                                });
                            return in_board || in_bench;
                        };
                        const bool better_alternative_found = std::any_of(
                            this->recent_purchases.begin(), this->recent_purchases.end(),
                            [&](const RecentPurchase& candidate) {
                                return candidate.uuid != sold_uuid &&
                                    candidate.action_serial > recent->action_serial &&
                                    candidate.fit_score >= recent->fit_score + 0.10 &&
                                    is_still_owned(candidate.uuid);
                            });

                        // Selling to cross a meaningful spending threshold is an
                        // economic decision, not an impulsive reversal. The sale
                        // must make a 2g reroll or 4g XP purchase affordable.
                        const int gold_after_sale = old_gold + sell_value;
                        sale_has_legitimate_context = sale_has_legitimate_context ||
                            better_alternative_found;
                        if (reward_model::should_penalize_reversed_purchase(
                                recent->fit_score,
                                recent->completed_upgrade,
                                recent->was_deployed,
                                sale_bench_was_full,
                                composition_changed,
                                recent->built_pair,
                                better_alternative_found,
                                sale_economic_need)) {
                            purchase_was_recently_reversed = true;
                            reversed_purchase_prior_reward = recent->reward;
                        }
                    }
                    const auto& current_strategy = strategy_before;
                    if (!this->strategy.empty()) {
                        const auto& core_unit_ids = current_strategy.core_unit_ids;
                        const auto& planned_unit_ids = current_strategy.planned_unit_ids;
                        sold_core_unit =
                            std::find(core_unit_ids.begin(), core_unit_ids.end(),
                                unit.champion_internal_id) != core_unit_ids.end() ||
                            std::find(planned_unit_ids.begin(), planned_unit_ids.end(),
                                unit.champion_internal_id) != planned_unit_ids.end();
                        sold_unit_fit_score = std::clamp(
                            unit_composition_score(current_strategy, *definition, unit.star, lobby.trait_milestones) / 18.0,
                            0.0, 1.0);
                    }
                    int same_star_copies = 0;
                    int one_star_copies = 0;
                    const auto count_upgrade_copies = [&](const auto& candidate) {
                        if (!candidate ||
                            candidate->champion_internal_id != unit.champion_internal_id ||
                            &*candidate == &unit)
                            return;
                        if (candidate->star == unit.star) ++same_star_copies;
                        if (candidate->star == 1) ++one_star_copies;
                    };
                    for (const auto& candidate : this->board)
                        count_upgrade_copies(candidate);
                    for (const auto& candidate : this->bench)
                        count_upgrade_copies(candidate);
                    // Only same-star copies form a pair. For a 2-star sale, three
                    // 1-star copies also represent a concrete route to another 2-star.
                    sold_unit_upgrade_strength = reward_model::upgrade_path_strength(
                        unit.star, same_star_copies, one_star_copies);
                    sold_unit_protects_upgrade = sold_unit_upgrade_strength >= 0.45;

                    // A genuinely stronger owned option can justify releasing a
                    // planned/core slot. Compare only against units that fit the
                    // active plan materially better, not merely any owned unit.
                    if (sold_core_unit && !this->strategy.empty()) {
                        const auto has_better_owned_fit = [&](const auto& candidate) {
                            if (!candidate ||
                                candidate->champion_internal_id == unit.champion_internal_id)
                                return false;
                            const auto* candidate_def = lobby.catalog->find_by_id(
                                candidate->champion_internal_id);
                            if (!candidate_def) return false;
                            const double candidate_fit = std::clamp(
                                unit_composition_score(current_strategy, *candidate_def,
                                    candidate->star, lobby.trait_milestones) / 18.0,
                                0.0, 1.0);
                            return candidate_fit >= sold_unit_fit_score + 0.10;
                        };
                        const bool better_owned_substitute =
                            std::any_of(this->board.begin(), this->board.end(),
                                has_better_owned_fit) ||
                            std::any_of(this->bench.begin(), this->bench.end(),
                                has_better_owned_fit);
                        sale_has_legitimate_context =
                            sale_has_legitimate_context || better_owned_substitute;
                    }
                    // Só confirma a venda se a devolução ao pool
                    // for aceita.
                    bool returned_all_copies = true;
                    const auto copies = star_copies(unit.star);
                    std::int32_t returned = 0;
                    for (; returned < copies; ++returned) {
                        if (!lobby.pool->give_back(unit.champion_internal_id)) {
                            returned_all_copies = false;
                            break;
                        }
                    }
                    if (returned_all_copies) {
                        // Equipment belongs to the player, not to the unit.
                        // Return it to the bag before removing the unit so it
                        // can be equipped on another unit without loss/duplication.
                        if (this->item_engine &&
                            !this->item_engine->return_equipped_to_inventory(sold_uuid)) {
                            for (std::int32_t copy = 0; copy < returned; ++copy)
                                (void)lobby.pool->take(unit.champion_internal_id);
                            returned_all_copies = false;
                        } else {
                            this->bench[bench_slot].reset();
                            (void)lobby.economy_engine.add_gold(economy, sell_value);
                            lobby.register_position_state(player_id);
                        }
                    } else {
                        for (std::int32_t copy = 0; copy < returned; ++copy)
                            (void)lobby.pool->take(unit.champion_internal_id);
                    }
                }
            }
        } else if (type == 6) {
            // PASS
        }

        lobby.sync_player_economy(player_id);
        int new_board_count = 0, new_bench_count = 0;
        for (const auto& unit : this->board) if (unit) ++new_board_count;
        for (const auto& unit : this->bench) if (unit) ++new_bench_count;

        // Shared shop-opportunity check: buy meaningful pairs/upgrades/fits
        // before converting surplus gold into XP or passing.
        const auto has_useful_shop_purchase = [&]() {
            if (this->strategy.empty()) return false;
            const bool has_free_bench_slot = std::any_of(
                this->bench.begin(), this->bench.end(),
                [](const auto& unit) { return !unit.has_value(); });
            const auto& current_strategy = this->strategy.back();
            for (std::size_t slot = 0; slot < kShopSlots; ++slot) {
                const auto* offer = shop.get_unit(static_cast<std::int32_t>(slot));
                if (!offer) continue;
                const auto* definition = lobby.catalog->find_by_id(offer->champion_internal_id);
                if (!definition || economy.gold < definition->cost) continue;
                int same_star_copies = 0;
                for (const auto& unit : this->board)
                    if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                        unit->star == offer->star) ++same_star_copies;
                for (const auto& unit : this->bench)
                    if (unit && unit->champion_internal_id == offer->champion_internal_id &&
                        unit->star == offer->star) ++same_star_copies;
                const double upgrade_value = same_star_copies >= 3 ? 1.0
                    : (same_star_copies == 2 ? 0.6
                    : (same_star_copies == 1 ? 0.2 : 0.0));
                const bool completes_upgrade =
                    same_star_copies >= 2 && offer->star == 1;
                // A full bench can still buy a copy that immediately merges
                // into a higher-star unit and releases capacity.
                if (!has_free_bench_slot && !completes_upgrade) continue;
                // A matching copy is genuine upgrade progress even when
                // temporary comp-fit scores are negative. Fit still scales
                // purchase quality; it must not erase the pair opportunity.
                const bool builds_pair =
                    same_star_copies == 1 && offer->star == 1;
                const bool in_top_trait =
                    (!current_strategy.composition_direction.empty() &&
                     std::find(definition->traits.begin(), definition->traits.end(),
                        current_strategy.composition_direction) != definition->traits.end()) ||
                    std::any_of(definition->traits.begin(), definition->traits.end(),
                        [&](const auto& trait) {
                            return std::find(current_strategy.core_traits.begin(),
                                current_strategy.core_traits.end(), trait) !=
                                current_strategy.core_traits.end();
                        });
                const bool in_planned_comp =
                    std::find(current_strategy.core_unit_ids.begin(),
                        current_strategy.core_unit_ids.end(), definition->internal_id) !=
                            current_strategy.core_unit_ids.end() ||
                    std::find(current_strategy.planned_unit_ids.begin(),
                        current_strategy.planned_unit_ids.end(), definition->internal_id) !=
                            current_strategy.planned_unit_ids.end();
                const double board_impact = unit_board_impact_score(
                    current_strategy, *definition, offer->star, lobby.trait_milestones,
                    new_board_count, economy.level,
                    static_cast<int>(stage_for_round(lobby.round + 1)));
                const int offer_stage =
                    static_cast<int>(stage_for_round(lobby.round + 1));
                const bool early_flex_purchase =
                    offer_stage <= 2 && definition->cost <= 2 &&
                    new_bench_count < 6;
                const bool strategy_approved = should_buy_unit(
                    board_impact, upgrade_value, in_top_trait || in_planned_comp,
                    new_bench_count, offer_stage, definition->cost);
                // Early low-cost units create options before the comp is known.
                // Keep this bounded by cost and bench capacity so it cannot
                // turn into indiscriminate shop clearing.
                if (strategy_approved || early_flex_purchase ||
                    completes_upgrade || builds_pair)
                    return true;
            }
            return false;
        };
        const bool useful_shop_purchase_available = has_useful_shop_purchase();

        // Alinhado a RewardManager.calculate_action_reward() e aos valores
        // usados em env.py para compra, reroll e XP.
        double action_reward = 0.0;
        bool success = false;
        if (type == 0) {
            // The unit may be bought successfully while immediately consuming
            // two copies in an automatic upgrade, so slot counts are not a
            // reliable signal of success. Match MiniTFTEnv._execute_buy():
            // No unconditional bonus for a normal purchase; +0.30 for a completed upgrade.
            success = bought_unit_successfully;
            action_reward = !success ? -0.10 : (bought_upgrade ? 0.30 : 0.0);
            if (success && !buy_strategy_approved &&
                !buy_completes_upgrade && !buy_builds_pair) {
                // Keep the rejection signal for genuinely low-value clutter, but
                // don't punish pair progress or a merge because a board heuristic
                // undervalued a unit that is not deployed yet.
                action_reward -= 0.10;
            }
            if (success && has_candidate_comp_score) {
                // Bench acquisitions are valuable even if their raw score is below
                // the current board average. Blend absolute plan fit with marginal
                // board fit instead of making board-relative delta the only signal.
                // Early comp plans can be incomplete or overconfident. Keep
                // a modest positive floor for an affordable bench acquisition
                // instead of letting the current direction erase its value.
                // The early-game floor is for actual upgrade/pair progress.
                // Modest-fit early acquisitions preserve option value; the
                // recent-purchase sale guard prevents rewarding buy/sell loops.
                const bool early_purchase_has_progress =
                    buy_completes_upgrade || buy_builds_pair;
                const double effective_candidate_score =
                    lobby.stage <= 2 && early_purchase_has_progress
                        ? std::max(candidate_comp_score, 2.0)
                        : candidate_comp_score;
                const double acquisition_value = 0.75 * effective_candidate_score +
                    0.25 * composition_quality_delta;
                action_reward += reward_model::purchase_quality(
                    acquisition_value, false);
            }
            if (success && buy_completes_upgrade) {
                action_reward += 0.25;
            } else if (success && buy_builds_pair) {
                action_reward += reward_model::purchase_pair_quality(
                    candidate_comp_score);
            }
            if (success && economy.gold < old_gold) {
                const double normalized_fit = has_candidate_comp_score
                    ? std::clamp((lobby.stage <= 2 &&
                        (buy_completes_upgrade || buy_builds_pair)
                            ? std::max(candidate_comp_score, 2.0)
                            : candidate_comp_score) / 32.5, 0.0, 1.0)
                    : 0.0;
                action_reward += reward_model::spending_timing_reward(
                    old_gold, this->info.hp, lobby.stage, normalized_fit,
                    buy_completes_upgrade, false);
                // The offer is scoped to the execution branch above; the
                // actual gold delta is the authoritative purchase cost here.
                const int purchase_cost = old_gold - economy.gold;
                action_reward += reward_model::early_purchase_reward(
                    lobby.stage, old_gold, purchase_cost, old_bench_count,
                    normalized_fit, buy_completes_upgrade, buy_builds_pair);
                action_reward += reward_model::unit_purchase_timing_reward(
                    lobby.stage, old_gold, purchase_cost, old_bench_count,
                    normalized_fit, this->info.hp,
                    buy_completes_upgrade, buy_builds_pair);
            }
        } else if (type == 1) {
            success = economy.gold == old_gold - EconomyEngine::REROLL_COST;
            action_reward = success ? -0.01 : -0.10;
            if (success) {
                // Only SLOWROLL receives positive shop-quality feedback.
                // Other strategies use planned XP progress and useful purchases;
                // a better shop alone must not teach them to reroll.
                const std::int32_t active_strategy_id =
                    this->strategy.empty() ? 0 : this->strategy.back().strategy;
                const double tactical_reroll_scale =
                    reward_model::tactical_reroll_quality_scale(
                        active_strategy_id, this->info.hp, economy.gold,
                        static_cast<int>(lobby.stage));
                if (tactical_reroll_scale > 0.0) {
                    const double quality_reward = reward_model::reroll_quality_reward(
                        reroll_old_best_fit, reroll_new_best_fit,
                        economy.gold, this->info.hp, static_cast<int>(lobby.stage));
                    action_reward += tactical_reroll_scale * quality_reward;
                }
                // Reroll quality already includes liquidity and urgency. Do not
                // add the generic spending-timing bonus here, which could make
                // rolling below 50 more profitable than preserving interest.
            }
        } else if (type == 2) {
            success = economy.gold == old_gold - EconomyEngine::BUY_XP_COST;
            // Do not give a positive reward merely for spending on XP. Early
            // levels are less valuable than building a playable board and pairs.
            action_reward = success ? (lobby.stage <= 2 ? -0.03 : 0.0) : -0.10;
            const std::int32_t xp_strategy_id =
                this->strategy.empty() ? 0 : this->strategy.back().strategy;
            if (success) {
                double unlocked_board_power = 0.0;
                // A level-up can unlock a board slot. Credit only a plausible
                // immediate strength gain when the old board was full and a
                // deployable bench unit is already available.
                if (economy.level > old_level && old_board_count >= old_level) {
                    for (const auto& unit : this->bench) {
                        if (!unit) continue;
                        const auto* definition = lobby.catalog->find_by_id(
                            unit->champion_internal_id);
                        if (!definition) continue;
                        unlocked_board_power = std::max(
                            unlocked_board_power,
                            static_cast<double>(definition->cost) *
                                static_cast<double>(unit->star) * 2.0);
                    }
                }
                action_reward += reward_model::xp_level_transition_value(
                    old_gold, economy.gold, this->info.hp, lobby.stage,
                    old_level, economy.level, unlocked_board_power,
                    xp_strategy_id);
                const double phase_xp_reward =
                    reward_model::phase_xp_progress_reward(
                        old_gold, economy.gold, old_level, economy.level,
                        old_xp, economy.xp, this->info.hp,
                        static_cast<int>(lobby.stage), xp_strategy_id,
                        useful_shop_purchase_available);
                action_reward += phase_xp_reward;
                // Do not stack the generic surplus fallback on top of the
                // stronger phase/target-aware signal. Retain it for cases
                // where no phase progress signal applies.
                if (phase_xp_reward <= 0.0) {
                    action_reward += reward_model::surplus_xp_spending_reward(
                        old_gold, economy.gold, static_cast<int>(lobby.stage),
                        xp_strategy_id, useful_shop_purchase_available);
                }
                action_reward += reward_model::xp_progress_reward(
                    old_gold, economy.gold, old_level, economy.level,
                    old_xp, economy.xp, this->info.hp, static_cast<int>(lobby.stage),
                    xp_strategy_id, useful_shop_purchase_available);
            }
            // RewardManager tracks the last rewarded level, initialized to 1,
            // not merely the level at the start of this action.
            if (success && economy.level > old_level &&
                economy.level > this->previous_reward_level) {
                // A level-up is not automatically worth more than economy.
                // Scale its progress signal by liquidity and urgency so the
                // early policy builds pairs and protects interest before routine XP.
                const double level_scale = this->info.hp <= 30.0 ? 0.65
                    : (old_gold >= 54 ? 0.30
                    : (old_gold >= 50 ? 0.12
                    : (old_gold >= 40 ? 0.10
                    : (old_gold >= 30 ? 0.08
                    : (old_gold >= 20 ? 0.04 : 0.02)))));
                // Trim the extra level-transition shaping by the same
                // modest factor as the XP-specific marginal reward.
                action_reward += 0.90 * level_scale *
                    reward_model::xp_strategy_reward_scale(xp_strategy_id) *
                    reward_model::level(economy.level, this->previous_reward_level);
                this->previous_reward_level = economy.level;
                // Early XP should not receive the rich-spending bonus unless
                // the player is already under meaningful health pressure.
                // Do not stack a generic spending bonus onto an XP action
                // that breaks the interest floor. Preserve the exception for
                // critical HP and for spending surplus while staying at 50+.
                if (this->info.hp <= 30.0 ||
                    (old_gold >= EconomyEngine::BUY_XP_COST + 50 &&
                     economy.gold >= 50)) {
                    action_reward += 0.90 * reward_model::spending_timing_reward(
                        old_gold, this->info.hp, lobby.stage, 0.0, false, true);
                }
            }
            (void)old_level;
            (void)old_xp;
        } else if (type == 3 || type == 4) {
            bool board_layout_changed = false;
            for (std::size_t slot = 0; slot < kBoardSlots; ++slot) {
                const std::string current_uuid = this->board[slot]
                    ? this->board[slot]->uuid : std::string{};
                if (current_uuid != old_board_layout[slot]) {
                    board_layout_changed = true;
                    break;
                }
            }
            // Tanto a implantação como a substituição alteram o layout.
            // Capacidade excedida é a única falha com penalidade específica.
            success = board_layout_changed;
            action_reward = board_capacity_violation
                ? -0.25
                : (anti_loop_blocked
                    ? reward_model::blocked_move()
                    : (success ? 0.0 : -0.10));

            if (success && !board_capacity_violation && !anti_loop_blocked) {
                const double positioning_delta =
                    lobby.positioning_score(player_id) - old_positioning_score;
                const double synergy_delta =
                    lobby.synergy_score(player_id) - old_synergy_score;

                // Potential-based shaping makes consecutive movement rewards
                // telescope: intermediate improvements cannot be farmed and a
                // sequence is judged by its net final board state.
                action_reward += reward_model::movement_sequence_reward(
                    old_positioning_score,
                    old_synergy_score,
                    lobby.positioning_score(player_id),
                    lobby.synergy_score(player_id));

                if (type == 3) {
                    action_reward += 0.10 * std::tanh(
                        composition_quality_delta / 5.0);

                    const bool composition_improved =
                        old_board_count == 0 ||
                        composition_quality_delta > 1e-6 ||
                        synergy_delta > 1e-6 ||
                        positioning_delta > 1e-6;

                    if (new_board_count > old_board_count && composition_improved) {
                        action_reward += 0.25;
                        const int board_capacity = std::min(
                            economy.level, static_cast<int>(kBoardSlots));
                        if (new_board_count == board_capacity) {
                            // Capacity bonus only applies when the deployment
                            // improves composition, synergy, or positioning.
                            action_reward += reward_model::board_capacity_completion();
                        }
                    } else if (new_board_count <= old_board_count && !composition_improved) {
                        // A replacement that neither grows the team nor improves
                        // fit/synergy/positioning must not be a free action.
                        action_reward += reward_model::movement_no_progress_penalty(
                            composition_quality_delta, synergy_delta, positioning_delta);
                    }
                    // Do not casually bench a planned/core unit to make room for
                    // a weaker or merely equivalent candidate. Allow replacement
                    // when the new unit has a clear composition-fit advantage.
                    if (has_displaced_comp_score) {
                        action_reward += reward_model::core_displacement_penalty(
                            displaced_core_unit, composition_quality_delta);
                    }
                }
            }
        } else if (type == 5) {
            success = economy.gold > old_gold;
            const double sale_synergy_delta =
                lobby.synergy_score(player_id) - old_synergy_score;
            const double sale_positioning_delta =
                lobby.positioning_score(player_id) - old_positioning_score;
            // A negligible score fluctuation must not justify selling a planned
            // unit. Require a material, measurable improvement to the active board.
            const bool sale_compensated_by_board_gain =
                sale_synergy_delta >= 0.20 || sale_positioning_delta >= 0.10;
            action_reward = success
                ? reward_model::sale_quality(
                    sold_unit_fit_score, sold_core_unit,
                    sold_unit_protects_upgrade,
                    sale_bench_was_full,
                    sale_compensated_by_board_gain,
                    sale_has_legitimate_context,
                    sold_unit_upgrade_strength)
                : -0.10;
            if (success && !this->strategy.empty()) {
                action_reward += reward_model::critical_all_in_sale_penalty(
                    this->strategy.back().strategy,
                    static_cast<double>(this->info.hp),
                    sold_core_unit,
                    sold_unit_protects_upgrade,
                    sale_compensated_by_board_gain);
            }
            if (success && purchase_was_recently_reversed &&
                old_bench_count < static_cast<int>(kBenchSlots)) {
                // Do not exempt a unit merely because update_strategy promoted
                // it to core after the purchase. That would let the buy itself
                // manufacture the exception and hide the immediate reversal.
                action_reward += reward_model::reversed_purchase_penalty();
            }
        } else if (type == 7 || type == 8) {
            // Item actions use the normal environment/PPO reward path, with a
            // bounded strategic signal balancing immediate combat value and
            // long-term composition fit.
            success = true;
            action_reward = item_assignment_reward;
        } else {
            success = true;
            if (type == 6 && !this->strategy.empty()) {
                const std::int32_t pass_strategy_id =
                    this->strategy.back().strategy;
                const bool useful_xp_spending_available =
                    reward_model::phase_xp_opportunity_available(
                        economy.gold, economy.level, this->info.hp,
                        static_cast<int>(lobby.stage), pass_strategy_id);
                action_reward += reward_model::pass_opportunity_penalty(
                    economy.gold, this->info.hp, lobby.stage,
                    useful_shop_purchase_available,
                    useful_xp_spending_available);
            }
        }

        // Give spending decisions immediate feedback for the opportunity cost
        // of liquid gold and the interest breakpoints. This is a potential delta,
        // not a per-step bonus, so repeatedly PASSing cannot farm reward.
        // Small, bounded strategy shaping: rewards the intended action, not
        // merely the mode label. The active mode is the one used to choose this action.
        const std::int32_t active_strategy_id =
            this->strategy.empty() ? 0 : this->strategy.back().strategy;
        action_reward += reward_model::strategy_action_reward(
            active_strategy_id,
            type,
            old_gold,
            economy.gold,
            static_cast<double>(this->info.hp),
            static_cast<int>(lobby.stage),
            success && (buy_strategy_approved || buy_completes_upgrade || buy_builds_pair),
            success && reroll_new_best_fit > reroll_old_best_fit,
            success && economy.level > old_level,
            active_strategy_id == 2 || active_strategy_id == 3,
            lobby.board_power(player_id),
            useful_shop_purchase_available);

        // A policy-specific bonus must not erase the shared economic objective:
        // preserve the path to 50 gold, except when the board is already full
        // and leveling unlocks a real slot, or health pressure is critical.
        if (type == 2 && success && old_gold < 50 &&
            this->info.hp > 30.0 && old_board_count < old_level) {
            // Interest loss is already represented by gold_change_reward().
            // Keep XP slightly costly below 50, but do not stack a large
            // penalty that makes reroll the less-bad choice outside SLOWROLL.
            action_reward -= active_strategy_id == 3 ? 0.12 : 0.025;
        }
        // Even a better shop is usually not worth sacrificing interest below
        // 50. Critical health is the explicit exception; at/above 50 the
        // liquidity-scaled reroll quality signal decides whether to spend.
        if (type == 1 && success && old_gold < 50 && this->info.hp > 40.0) {
            // Small additional opportunity cost: discourage marginal rolls,
            // without suppressing genuinely useful rolls under pressure.
            action_reward -= 0.07;
        }

        // Charge a small, bounded action cost only for extra consecutive
        // movements. This preserves useful multi-step repositioning while
        // discouraging long shuffling sequences. A non-move or failed move
        // closes the sequence; the next round starts a fresh one.
        if (type == 3 || type == 4) {
            if (success && !board_capacity_violation && !anti_loop_blocked) {
                action_reward += reward_model::movement_sequence_action_cost(
                    movement_sequence_length);
                movement_sequence_length = std::min<std::uint32_t>(
                    movement_sequence_length + 1, 15);
            } else {
                movement_sequence_length = 0;
            }
        } else {
            movement_sequence_length = 0;
        }

        const double action_gold_reward =
            reward_model::gold_change_reward(old_gold, economy.gold);
        const std::int32_t gold_reward_strategy_id =
            this->strategy.empty() ? 0 : this->strategy.back().strategy;
        // ALL_IN is the explicit exception to interest preservation: retain
        // some opportunity-cost signal, but do not let crossing below 50
        // overwhelm the action's survival-oriented spending reward.
        const double liquidity_reward_scale =
            gold_reward_strategy_id == 4 && (type == 0 || type == 2) ? 0.35 : 1.0;
        action_reward += action_gold_reward * liquidity_reward_scale;

        // Persist the complete buy reward so a later proven reversal can be
        // prevented from making the aggregate purchase->sale sequence positive.
        if (type == 0 && success && !bought_upgrade) {
            for (auto it = this->recent_purchases.rbegin();
                 it != this->recent_purchases.rend(); ++it) {
                if (it->action_serial == this->action_serial) {
                    it->reward = action_reward;
                    break;
                }
            }
        }
        if (type == 5 && success && purchase_was_recently_reversed) {
            action_reward += reward_model::reversed_sequence_adjustment(
                reversed_purchase_prior_reward, action_reward);
        }

        // MiniTFTEnv.step() returns the action-specific reward directly.
        // RewardManager's broader state components are not called by the
        // Python environment, so applying them here would change PPO targets.
        lobby.update_strategy(player_id);
        // Record the direction *after* this purchase has refreshed strategy.
        // Otherwise a direction change caused by the purchase itself is mistaken
        // for a later legitimate transition and exempts an immediate buy/sell loop.
        if (type == 0 && success && !this->strategy.empty()) {
            for (auto it = this->recent_purchases.rbegin();
                 it != this->recent_purchases.rend(); ++it) {
                if (it->action_serial == this->action_serial) {
                    it->comp_direction = this->strategy.back().composition_direction;
                    it->comp_focus = this->strategy.back().composition_focus;
                    break;
                }
            }
        }
        // Capture actual action economics before round income is resolved.
        // Net gold after TFTEngine::step may include income and cannot be used
        // to infer the action's true cost. This trace is audit-only and never
        // enters the policy observation or PPO reward calculation.
        const auto actual_action_cost = type == 5
            ? 0 : std::max<std::int32_t>(0, old_gold - economy.gold);
        const auto actual_sale_proceeds = type == 5
            ? std::max<std::int32_t>(0, economy.gold - old_gold) : 0;
        return PlayerTransition{
            static_cast<float>(action_reward),
            true,
            board_capacity_violation,
            actual_action_cost,
            actual_sale_proceeds,
            old_gold,
            economy.gold,
            lobby.economy_engine.get_interest(old_gold),
            lobby.economy_engine.get_interest(economy.gold),
            old_level,
            economy.level,
            old_xp,
            economy.xp
        };
}

// ============================================================
// CONSTRUTOR E DESTRUTOR
// ============================================================

TFTEngine::TFTEngine(
    std::uint64_t seed,
    std::string catalog_path,
    std::string item_catalog_path
)
    : impl_(
        std::make_unique<Impl>(seed, catalog_path, item_catalog_path)
    ) {
}

TFTEngine::~TFTEngine() = default;

TFTEngine::TFTEngine(TFTEngine&&) noexcept = default;

TFTEngine& TFTEngine::operator=(TFTEngine&&) noexcept = default;

// ============================================================
// RESET
// ============================================================

ResetResult TFTEngine::reset(
    std::optional<std::uint64_t> seed
) {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    if (seed.has_value()) {
        impl_->seed = *seed;
    }

    ++impl_->episode_id;

    impl_->round = 0;
    impl_->stage = 1;
    impl_->done = false;
    impl_->winner_id = std::nullopt;
    impl_->ranking.clear();
    impl_->previous_matchups.clear();
    impl_->matchmaking_rng.seed(impl_->seed + impl_->episode_id);

    // Devolve as ofertas antes de restaurar o pool.
    for (auto& player_env : impl_->player_envs) {
        auto& shop = player_env.shop;
        if (shop) {
            shop->return_current_shop();
        }

        shop.reset();
    }

    impl_->pool->reset();

    impl_->unit_factory =
        std::make_unique<UnitInstanceFactory>(
            *impl_->catalog,
            impl_->seed
        );

    impl_->initialize_players();
    for (auto& player_env : impl_->player_envs) player_env.previous_board_power = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.previous_synergy_score = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.previous_positioning_score = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.previous_reward_level = 1;
    for (auto& player_env : impl_->player_envs) player_env.last_combat_reward = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.round_reward_total = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.total_reward = 0.0;
    for (auto& player_env : impl_->player_envs) player_env.placement_reward_given = false;

    ResetResult result{};

    result.observations = impl_->build_observations();
    result.observation_dim = impl_->observation_dim();
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) result.action_masks[i] = impl_->player_envs[i].action_masks;
    result.episode_id = impl_->episode_id;

    return result;
}

// ============================================================
// STEP
// ============================================================

StepResult TFTEngine::step(
    const std::array<PlayerAction, kPlayersPerLobby>& actions,
    bool timer_enabled,
    bool timer_expired
) {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    if (impl_->done) {
        throw std::runtime_error(
            "A partida ja foi encerrada. Execute reset()."
        );
    }

    // No treinamento, um jogador só encerra suas decisões após 10 ações.
    // PASS conta como ação, mas não encerra a preparação antecipadamente.
    // No viewer, o temporizador controla a fase e o limite de ações fica desativado.
    auto effective_actions = actions;
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        // A finished player normally idles with PASS, but a pending anvil is
        // a forced decision that must be resolved before preparation can end.
        // Do not overwrite that legal choice with PASS: the action mask has
        // intentionally disabled PASS while the anvil is pending.
        const auto* pending_anvil = impl_->player_envs[i].item_engine
            ? impl_->player_envs[i].item_engine->pending_anvil() : nullptr;
        if (impl_->round_finished[i] && pending_anvil == nullptr) {
            effective_actions[i].values = {6, 0, 0, 0, 0, 0};
        }
    }

    // Preflight em duas fases: valida limites e disponibilidade semântica
    // de todas as ações antes de executar a primeira. Se qualquer ação for
    // impossível, o lote falha sem alterar ouro, unidades ou posições.
    for (const auto& action : effective_actions) {
        impl_->validate_action(action);
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
        impl_->validate_action_against_masks(i, effective_actions[i]);
    }

    // ========================================================
    // AÇÕES DE LOJA, ECONOMIA E MOVIMENTAÇÃO
    // ========================================================

    std::array<float, kPlayersPerLobby> round_rewards{};
    std::array<float, kPlayersPerLobby> action_rewards{};
    std::array<float, kPlayersPerLobby> combat_rewards{};
    std::array<float, kPlayersPerLobby> placement_rewards{};
    std::array<bool, kPlayersPerLobby> capacity_violations{};
    std::array<std::int32_t, kPlayersPerLobby> action_costs{};
    std::array<std::int32_t, kPlayersPerLobby> sale_proceeds{};
    std::array<std::int32_t, kPlayersPerLobby> gold_before_actions{};
    std::array<std::int32_t, kPlayersPerLobby> gold_after_actions{};
    std::array<std::int32_t, kPlayersPerLobby> interest_before_actions{};
    std::array<std::int32_t, kPlayersPerLobby> interest_after_actions{};
    std::array<std::int32_t, kPlayersPerLobby> level_before_actions{};
    std::array<std::int32_t, kPlayersPerLobby> level_after_actions{};
    std::array<std::int32_t, kPlayersPerLobby> xp_before_actions{};
    std::array<std::int32_t, kPlayersPerLobby> xp_after_actions{};
    std::vector<std::string> item_action_events;
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        const auto transition = impl_->player_envs[i].step(*impl_, i, effective_actions[i]);
        if (!transition.acted) {
            continue;
        }
        const auto& item_action = impl_->player_envs[i].last_item_action;
        if (item_action.kind == "anvil_component_choice" ||
            item_action.kind == "anvil_completed_item_choice") {
            item_action_events.emplace_back(
                "ITEM_ANVIL_CHOSEN: player=" + std::to_string(i) +
                ", kind=" + item_action.kind +
                ", item=" + item_action.item_id +
                ", stage=" + std::to_string(impl_->stage));
        } else if (item_action.kind == "equip_inventory_item" ||
                   item_action.kind == "craft_and_equip") {
            item_action_events.emplace_back(
                "ITEM_ACTION_APPLIED: player=" + std::to_string(i) +
                ", kind=" + item_action.kind +
                ", item=" + item_action.item_id +
                ", target_uuid=" + item_action.target_uuid +
                ", fit=" + std::to_string(item_action.fit) +
                ", reward=" + std::to_string(item_action.assignment_reward) +
                ", stage=" + std::to_string(impl_->stage));
        }
        action_rewards[i] = transition.action_reward;
        capacity_violations[i] = transition.board_capacity_violation;
        action_costs[i] = transition.action_cost;
        sale_proceeds[i] = transition.sale_proceeds;
        gold_before_actions[i] = transition.gold_before_action;
        gold_after_actions[i] = transition.gold_after_action;
        interest_before_actions[i] = transition.interest_before_action;
        interest_after_actions[i] = transition.interest_after_action;
        level_before_actions[i] = transition.level_before_action;
        level_after_actions[i] = transition.level_after_action;
        xp_before_actions[i] = transition.xp_before_action;
        xp_after_actions[i] = transition.xp_after_action;
        round_rewards[i] += transition.action_reward;
        impl_->player_envs[i].round_reward_total = round_rewards[i];
        impl_->player_envs[i].total_reward += round_rewards[i];
        impl_->sync_player_economy(i);
        impl_->refresh_masks(i);
        if (!timer_enabled) {
            impl_->mark_round_completion(i);
        }
    }

    // ========================================================
    // RESOLUÇÃO CONDICIONAL DA RODADA
    // ========================================================

    std::vector<std::string> round_events;
    round_events.insert(round_events.end(),
                        item_action_events.begin(), item_action_events.end());
    bool round_resolved = false;

    // In viewer/runtime mode the wall-clock timer owns the preparation phase.
    // PASS does not resolve combat early; only the timer ends this phase.
    if (timer_enabled && timer_expired) {
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (impl_->player_envs[i].info.alive) {
                impl_->round_finished[i] = true;
            }
        }
    }

    const bool preparation_complete = timer_enabled
        ? timer_expired
        : impl_->all_alive_players_finished_round();

    if (preparation_complete) {
        round_resolved = true;

        // Primeiro resolve o PvP quando esta rodada possui combate entre
        // jogadores. PvE/carrossel não concedem bônus de vitória nem alteram
        // sequências; eles avançam a economia, mas não simulamos seus drops.
        const auto rewards_before_combat = round_rewards;
        const auto current_round_number = impl_->round + 1;
        if (is_pvp_round(current_round_number)) {
            impl_->resolve_combats(round_events, round_rewards, combat_rewards);
        } else {
            for (auto& player_env : impl_->player_envs) {
                player_env.info.opponent_id.reset();
            }
            round_events.emplace_back(
                "ROUND_NO_PVP: fase de abertura/PvE/carrossel; "
                "nenhum bônus PvP ou streak foi aplicado."
            );
        }

        // Resolve item-anvil rewards from the completed PvE encounter. The
        // choices are generated once from the active catalog and then selected
        // by the policy during the next preparation phase.
        impl_->grant_round_pve_item_rewards(current_round_number, round_events);

        // A renda/XP calculada é para a próxima preparação; o estado começa
        // em round=0 (1-1), então a próxima rodada é current_round_number + 1.
        const auto next_round_number =
            static_cast<std::int32_t>(current_round_number + 1);

        if (!impl_->done && impl_->alive_players > 1) {
            for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
                auto& player_env = impl_->player_envs[i];
                if (!player_env.info.alive || player_env.info.hp <= 0.0f) {
                    continue;
                }

                (void)impl_->economy_engine.resolve_round(
                    player_env.economy,
                    next_round_number
                );
                impl_->sync_player_economy(i);

                // A loja é renovada para o próximo turno após atualizar o nível.
                player_env.shop->roll(player_env.economy.level);
                impl_->refresh_masks(i);
            }
        }

        // TFTLobby aplica a recompensa de colocação uma única vez assim que
        // a posição do jogador é conhecida (inclusive para eliminados).
        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            if (impl_->player_envs[i].placement_reward_given ||
                !impl_->player_envs[i].info.placement.has_value()) {
                continue;
            }
            const auto placement = *impl_->player_envs[i].info.placement;
            if (placement < 1 ||
                placement > static_cast<std::int32_t>(kPlayersPerLobby)) {
                throw std::logic_error("Colocacao invalida ao calcular recompensa terminal.");
            }
            const double placement_reward = 1.0 - 2.0 *
                (static_cast<double>(placement - 1) /
                 static_cast<double>(kPlayersPerLobby - 1));
            placement_rewards[i] = static_cast<float>(placement_reward);
            round_rewards[i] += placement_rewards[i];
            impl_->player_envs[i].placement_reward_given = true;
            round_events.emplace_back(
                "PLACEMENT_REWARD: jogador " + std::to_string(i) +
                ", colocacao=" + std::to_string(placement) +
                ", recompensa=" + std::to_string(placement_reward));
        }

        for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
            // Python clamps the per-player action transition inside env.step(),
            // then TFTLobby adds combat and placement rewards without clamping
            // that combined value a second time.
            impl_->player_envs[i].round_reward_total = round_rewards[i];
            impl_->player_envs[i].total_reward += round_rewards[i] - rewards_before_combat[i];
            if (impl_->player_envs[i].info.alive) {
                impl_->update_strategy(i);
            }
        }

        // O bônus de vitória já foi creditado no ouro pelo combate.
        // Limpa apenas o acumulador do relatório desta rodada.
        for (auto& player_env : impl_->player_envs) {
        auto& economy = player_env.economy;
            economy.round_win_gold = 0;
        }

        // A lobby Python só avança se ainda houver pelo menos dois
        // jogadores vivos. A partida encerrada mantém a rodada final.
        if (!impl_->done && impl_->alive_players > 1) {
            ++impl_->round;

            const std::uint32_t public_round =
                impl_->round + 1;

            impl_->stage = stage_for_round(public_round);

            // Start a fresh anti-loop local history for the new preparation,
            // while retaining only signatures from the previous four rounds.
            for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
                auto& player_env = impl_->player_envs[i];
                player_env.anti_loop.begin_round(public_round);
                if (player_env.info.alive) {
                    impl_->register_position_state(i);
                    // The old-round masks were generated before begin_round;
                    // rebuild them now so the first next-round action sees the
                    // cleared local history and the fresh visited-state window.
                    impl_->refresh_masks(i);
                }
            }
            impl_->reset_round_completion();
        }
    }

    // ========================================================
    // RESULTADO
    // ========================================================

    StepResult result{};

    result.observations = impl_->build_observations();
    result.observation_dim = impl_->observation_dim();

    result.rewards = round_rewards;
    result.action_rewards = action_rewards;
    result.combat_rewards = combat_rewards;
    result.placement_rewards = placement_rewards;
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        result.dones[i] = (!impl_->player_envs[i].info.alive || impl_->done) ? 1 : 0;
    }

    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) result.action_masks[i] = impl_->player_envs[i].action_masks;
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) result.players[i] = impl_->player_envs[i].info;

    result.lobby_done = impl_->done;
    result.episode_id = impl_->episode_id;
    result.round = impl_->round + 1;
    result.stage = impl_->stage;
    result.alive_players = impl_->alive_players;

    result.events.emplace_back(
        "Acoes de loja, economia, venda e movimentacao "
        "executadas pelo TFTEngine."
    );
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        result.events.emplace_back(
            "ECONOMIC_ACTION player " + std::to_string(i) +
            " action_cost=" + std::to_string(action_costs[i]) +
            " sale_proceeds=" + std::to_string(sale_proceeds[i]) +
            " gold_before=" + std::to_string(gold_before_actions[i]) +
            " gold_after=" + std::to_string(gold_after_actions[i]) +
            " interest_before=" + std::to_string(interest_before_actions[i]) +
            " interest_after=" + std::to_string(interest_after_actions[i]) +
            " level_before=" + std::to_string(level_before_actions[i]) +
            " level_after=" + std::to_string(level_after_actions[i]) +
            " xp_before=" + std::to_string(xp_before_actions[i]) +
            " xp_after=" + std::to_string(xp_after_actions[i])
        );
        if (capacity_violations[i]) {
            result.events.emplace_back(
                "RULE_VIOLATION: jogador " + std::to_string(i) +
                " tentou exceder a capacidade do nivel; movimento recusado."
            );
        }
    }

    if (!round_resolved) {
        result.events.emplace_back(
            "ROUND_WAITING: fase de preparacao ainda ativa; "
            "aguardando o temporizador ou PASS de todos os jogadores vivos."
        );
    } else {
        result.events.emplace_back("ROUND_RESOLVED");
    }
    result.events.insert(
        result.events.end(),
        round_events.begin(),
        round_events.end()
    );

    return result;
}

// ============================================================
// MÁSCARAS DE AÇÃO
// ============================================================

std::array<PlayerActionMasks, kPlayersPerLobby>
TFTEngine::get_action_masks() const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    std::array<PlayerActionMasks, kPlayersPerLobby> result{};
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) result[i] = impl_->player_envs[i].action_masks;
    return result;
}

AuditSnapshot TFTEngine::get_audit_snapshot() const {
    if (!impl_) {
        throw std::runtime_error("TFTEngine nao possui estado interno.");
    }
    impl_->ensure_open();

    AuditSnapshot snapshot{};
    snapshot.round = impl_->round + 1;
    snapshot.stage = impl_->stage;
    snapshot.finished = impl_->done;

    const auto describe_unit = [this](const UnitInstance* unit) {
        AuditUnitSnapshot result{};
        if (!unit) return result;
        result.occupied = true;
        result.champion_id = unit->champion_id;
        result.uuid = unit->uuid;
        result.star = unit->star;
        const auto* definition = impl_->catalog->find_by_id(
            unit->champion_internal_id
        );
        result.cost = definition ? definition->cost : 0;
        result.item_ids.reserve(unit->items.size());
        for (const auto& item : unit->items) result.item_ids.push_back(item.api_name);
        return result;
    };

    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        const auto& source = impl_->player_envs[i];
        auto& target = snapshot.players[i];
        target.player_id = static_cast<std::int32_t>(i);
        target.gold = source.economy.gold;
        target.level = source.economy.level;
        target.xp = source.economy.xp;
        target.win_streak = source.economy.win_streak;
        target.loss_streak = source.economy.loss_streak;
        target.gold_before_interest = source.economy.gold_before_interest;
        target.interest_if_resolved_now = impl_->economy_engine.get_interest(source.economy.gold);
        target.board_power = impl_->board_power(i);
        target.synergy_score = impl_->synergy_score(i);
        // Recompute board diagnostics from the current board. PlayerInfo's
        // cached fields are not guaranteed to have been refreshed on every
        // action and may remain at their zero-initialized defaults.
        const auto board_eval = impl_->board_evaluation(i);
        target.positioning_score = board_eval.total_score;
        target.board_quality_score = board_eval.total_score;
        target.board_positional_score = board_eval.positional_score;
        target.board_structural_score = board_eval.structural_score;
        // Per-action deltas are derived by comparing consecutive snapshots.
        target.board_quality_delta = 0.0;
        target.board_exposed_carries = board_eval.exposed_carry_count;
        target.board_frontline_count = board_eval.frontline_count;
        target.board_isolated_frontline_count = board_eval.isolated_frontline_count;
        target.board_important_bench_units_unplayed =
            board_eval.important_bench_units_unplayed;
        target.round_gold = source.economy.round_gold;
        target.round_base_gold = source.economy.round_base_gold;
        target.round_interest = source.economy.round_interest;
        target.round_streak_gold = source.economy.round_streak_gold;
        target.round_win_gold = source.economy.last_round_win_gold;
        target.masks = source.action_masks;

        for (std::size_t slot = 0; slot < kBoardSlots; ++slot)
            target.board[slot] = describe_unit(
                source.board[slot] ? &*source.board[slot] : nullptr
            );
        for (std::size_t slot = 0; slot < kBenchSlots; ++slot)
            target.bench[slot] = describe_unit(
                source.bench[slot] ? &*source.bench[slot] : nullptr
            );
        for (std::size_t slot = 0; slot < kShopSlots; ++slot)
            target.shop[slot] = describe_unit(source.shop->get_unit(
                static_cast<std::int32_t>(slot)
            ));
        if (source.item_engine) {
            target.item_components = source.item_engine->components();
            target.item_inventory = source.item_engine->inventory();
            if (impl_->item_catalog) {
                for (const auto& component : impl_->item_catalog->components)
                    target.item_component_catalog.push_back(component.first);
            }
            if (const auto* pending = source.item_engine->pending_anvil()) {
                target.pending_anvil = true;
                target.pending_anvil_kind = static_cast<std::int32_t>(pending->kind);
                target.pending_anvil_options = pending->options;
            }
        }
        target.last_item_action_kind = source.last_item_action.kind;
        target.last_item_id = source.last_item_action.item_id;
        target.last_item_first_component = source.last_item_action.first_component;
        target.last_item_second_component = source.last_item_action.second_component;
        target.last_item_target_uuid = source.last_item_action.target_uuid;
        target.last_item_target_slot = source.last_item_action.target_slot;
        target.last_item_target_on_board = source.last_item_action.target_on_board;
        target.last_item_fit = source.last_item_action.fit;
        target.last_item_immediate_strength = source.last_item_action.immediate_strength;
        target.last_item_role_alignment = source.last_item_action.role_alignment;
        target.last_item_assignment_reward = source.last_item_action.assignment_reward;
    }
    return snapshot;
}

bool TFTEngine::set_unit_for_testing(
    std::int32_t player_id,
    bool on_board,
    std::int32_t slot,
    const std::string& champion_api_name,
    std::int32_t star
) {
    if (!impl_) {
        throw std::runtime_error("TFTEngine nao possui estado interno.");
    }
    impl_->ensure_open();
    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        star < 1 || star > 3) {
        return false;
    }

    const auto player_index = static_cast<std::size_t>(player_id);
    const auto* definition =
        impl_->catalog->find_by_api_name(champion_api_name);
    if (!definition) return false;

    auto unit = impl_->unit_factory->create(definition->internal_id);
    unit.star = star;

    if (on_board) {
        if (slot < 0 || static_cast<std::size_t>(slot) >= kBoardSlots)
            return false;
        impl_->player_envs[player_index].board[static_cast<std::size_t>(slot)] =
            std::move(unit);
        impl_->player_envs[player_index].board_move_age[static_cast<std::size_t>(slot)] = kActionMemoryHorizon;
    } else {
        if (slot < 0 || static_cast<std::size_t>(slot) >= kBenchSlots)
            return false;
        impl_->player_envs[player_index].bench[static_cast<std::size_t>(slot)] =
            std::move(unit);
    }

    impl_->register_position_state(player_index);
    // Keep derived strategy/composition data coherent with test-only fixture
    // mutations. SELL reward reads the current core-unit set before transition.
    impl_->update_strategy(player_index);
    impl_->refresh_masks(player_index);
    return true;
}

CombatTeam TFTEngine::get_combat_team_for_testing(
    std::int32_t player_id, UnitTeam team
) const {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby)
        throw std::out_of_range("Player ID invalido para snapshot de combate.");
    if (team != UnitTeam::PLAYER_A && team != UnitTeam::PLAYER_B)
        throw std::invalid_argument("Time invalido para snapshot de combate.");
    CombatTeam output{};
    int next_unit_id = 0;
    impl_->append_combat_team(static_cast<std::size_t>(player_id), team, output, next_unit_id);
    return output;
}

bool TFTEngine::set_unit_items_for_testing(
    std::int32_t player_id,
    bool on_board,
    std::int32_t slot,
    const std::vector<CombatItem>& items
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        slot < 0 || items.size() > 3) {
        return false;
    }
    const auto valid_bonus = [](float value) {
        return std::isfinite(value) && value >= 0.0f;
    };
    for (const auto& item : items) {
        if (item.api_name.empty() || item.name.empty() ||
            !valid_bonus(item.attack_damage) || !valid_bonus(item.ability_power) ||
            !valid_bonus(item.attack_speed) ||
            !valid_bonus(item.critical_strike_chance) ||
            !valid_bonus(item.critical_strike_damage) ||
            !valid_bonus(item.health) || !valid_bonus(item.armor) ||
            !valid_bonus(item.magic_resist) || !valid_bonus(item.mana) ||
            !valid_bonus(item.omnivamp) ||
            !valid_bonus(item.damage_amplification) ||
            !valid_bonus(item.damage_reduction)) return false;
    }
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    if (on_board) {
        if (static_cast<std::size_t>(slot) >= kBoardSlots ||
            !player.board[static_cast<std::size_t>(slot)]) return false;
        player.board[static_cast<std::size_t>(slot)]->items = items;
    } else {
        if (static_cast<std::size_t>(slot) >= kBenchSlots ||
            !player.bench[static_cast<std::size_t>(slot)]) return false;
        player.bench[static_cast<std::size_t>(slot)]->items = items;
    }
    impl_->refresh_masks(static_cast<std::size_t>(player_id));
    return true;
}

bool TFTEngine::grant_item_component(
    std::int32_t player_id, const std::string& component_id
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby)
        return false;
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    if (!player.item_engine || !player.item_engine->add_component(component_id))
        return false;
    impl_->refresh_masks(static_cast<std::size_t>(player_id));
    return true;
}

bool TFTEngine::grant_completed_item(
    std::int32_t player_id, const std::string& item_id
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby)
        return false;
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    if (!player.item_engine || !player.item_engine->add_completed_item(item_id))
        return false;
    impl_->refresh_masks(static_cast<std::size_t>(player_id));
    return true;
}

bool TFTEngine::add_item_component_for_testing(
    std::int32_t player_id, const std::string& component_id
) {
    return grant_item_component(player_id, component_id);
}

bool TFTEngine::add_completed_item_for_testing(
    std::int32_t player_id, const std::string& item_id
) {
    return grant_completed_item(player_id, item_id);
}

bool TFTEngine::set_player_economy_for_testing(
    std::int32_t player_id, std::int32_t gold,
    std::int32_t level, std::int32_t xp
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        gold < 0 || level < 1 || level > EconomyEngine::MAX_LEVEL || xp < 0)
        return false;
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    player.economy.gold = gold;
    player.economy.level = level;
    player.economy.xp = xp;
    impl_->refresh_masks(static_cast<std::size_t>(player_id));
    return true;
}

bool TFTEngine::set_shop_offer_for_testing(
    std::int32_t player_id, std::int32_t slot,
    const std::string& champion_api_name
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        slot < 0 || static_cast<std::size_t>(slot) >= kShopSlots)
        return false;
    const auto* definition = impl_->catalog->find_by_api_name(champion_api_name);
    if (!definition) return false;
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    auto unit = impl_->unit_factory->create(definition->internal_id);
    if (!player.shop->set_offer_for_testing(slot, std::move(unit))) return false;
    impl_->refresh_masks(static_cast<std::size_t>(player_id));
    return true;
}

bool TFTEngine::set_next_shop_roll_for_testing(
    std::int32_t player_id,
    const std::vector<std::string>& champion_api_names
) {
    if (!impl_) throw std::runtime_error("TFTEngine nao possui estado interno.");
    impl_->ensure_open();
    if (player_id < 0 || static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        champion_api_names.size() != kShopSlots)
        return false;
    std::vector<std::int32_t> ids;
    ids.reserve(kShopSlots);
    for (const auto& name : champion_api_names) {
        const auto* definition = impl_->catalog->find_by_api_name(name);
        if (!definition) return false;
        ids.push_back(definition->internal_id);
    }
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)];
    return player.shop->set_next_roll_for_testing(ids);
}

bool TFTEngine::set_player_hp_for_testing(
    std::int32_t player_id,
    float hp
) {
    if (!impl_) {
        throw std::runtime_error("TFTEngine nao possui estado interno.");
    }
    impl_->ensure_open();
    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby ||
        !std::isfinite(hp) || hp <= 0.0f) {
        return false;
    }
    auto& player = impl_->player_envs[static_cast<std::size_t>(player_id)].info;
    player.hp = hp;
    player.alive = true;
    player.eliminated = false;
    player.placement.reset();
    impl_->alive_players = 0;
    for (const auto& env : impl_->player_envs) {
        if (env.info.alive) ++impl_->alive_players;
    }
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        impl_->refresh_masks(i);
    }
    return true;
}

bool TFTEngine::set_player_alive_for_testing(
    std::int32_t player_id,
    bool alive
) {
    if (!impl_) {
        throw std::runtime_error("TFTEngine nao possui estado interno.");
    }
    impl_->ensure_open();
    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby) {
        return false;
    }

    const auto index = static_cast<std::size_t>(player_id);
    auto& player = impl_->player_envs[index].info;
    player.alive = alive;
    player.eliminated = !alive;
    if (!alive) {
        player.hp = 0.0f;
        player.placement.reset();
        impl_->round_finished[index] = true;
    } else if (player.hp <= 0.0f) {
        player.hp = 100.0f;
    }

    impl_->alive_players = 0;
    for (const auto& env : impl_->player_envs) {
            const auto& candidate = env.info;
        if (candidate.alive) ++impl_->alive_players;
    }
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) {
        impl_->refresh_masks(i);
    }
    return true;
}

bool TFTEngine::set_shop_locked(
    std::int32_t player_id,
    bool locked
) {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby) {
        throw std::out_of_range(
            "player_id invalido ao alterar bloqueio da loja."
        );
    }

    return impl_->player_envs[
        static_cast<std::size_t>(player_id)
    ].shop->set_locked(locked);
}

bool TFTEngine::toggle_shop_lock(
    std::int32_t player_id
) {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby) {
        throw std::out_of_range(
            "player_id invalido ao alternar bloqueio da loja."
        );
    }

    return impl_->player_envs[
        static_cast<std::size_t>(player_id)
    ].shop->toggle_lock();
}

bool TFTEngine::is_shop_locked(
    std::int32_t player_id
) const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    if (player_id < 0 ||
        static_cast<std::size_t>(player_id) >= kPlayersPerLobby) {
        throw std::out_of_range(
            "player_id invalido ao consultar bloqueio da loja."
        );
    }

    return impl_->player_envs[
        static_cast<std::size_t>(player_id)
    ].shop->is_locked();
}

// ============================================================
// OBSERVAÇÕES
// ============================================================

std::vector<float> TFTEngine::get_observations() const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    return impl_->build_observations();
}

// ============================================================
// ESTRATÉGIAS
// ============================================================

std::array<std::vector<StrategyInfo>, kPlayersPerLobby>
TFTEngine::get_strategies() const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    std::array<std::vector<StrategyInfo>, kPlayersPerLobby> result{};
    for (std::size_t i = 0; i < kPlayersPerLobby; ++i) result[i] = impl_->player_envs[i].strategy;
    return result;
}

RewardState TFTEngine::get_reward_state(std::int32_t player_id) const {
    if (!impl_ || player_id < 0 ||
        player_id >= static_cast<std::int32_t>(kPlayersPerLobby)) {
        throw std::out_of_range("player_id fora do intervalo.");
    }
    impl_->ensure_open();
    const auto i = static_cast<std::size_t>(player_id);
    RewardState state{};
    state.previous_board_power = impl_->player_envs[i].previous_board_power;
    state.previous_positioning_score = impl_->player_envs[i].previous_positioning_score;
    state.previous_synergy_score = impl_->player_envs[i].previous_synergy_score;
    state.previous_level = impl_->player_envs[i].previous_reward_level;
    // RewardManager.py declares previous_gold but does not update it.
    // Keep the same observable value until the Python contract changes.
    state.previous_gold = 0;
    state.last_combat_reward = impl_->player_envs[i].last_combat_reward;
    state.round_reward = impl_->player_envs[i].round_reward_total;
    state.total_reward = impl_->player_envs[i].total_reward;
    return state;
}

// ============================================================
// MÉTRICAS ANTI-LOOP
// ============================================================

double TFTEngine::get_unit_comp_score(
    std::int32_t player_id,
    const std::string& champion_api_name,
    std::int32_t star
) const {
    if (!impl_ || player_id < 0 ||
        player_id >= static_cast<std::int32_t>(kPlayersPerLobby)) {
        throw std::out_of_range("player_id fora do intervalo.");
    }
    impl_->ensure_open();
    const auto* def = impl_->catalog->find_by_api_name(champion_api_name);
    if (!def || star < 1 || star > 3) return 0.0;
    const auto& list =
        impl_->player_envs[static_cast<std::size_t>(player_id)].strategy;
    if (list.empty()) return 0.0;
    return unit_composition_score(list.back(), *def, star, impl_->trait_milestones);
}
bool TFTEngine::is_core_unit(std::int32_t player_id, const std::string& champion_api_name) const {
    if (!impl_ || player_id < 0 || player_id >= static_cast<std::int32_t>(kPlayersPerLobby)) return false; impl_->ensure_open(); const auto* def = impl_->catalog->find_by_api_name(champion_api_name); if (!def) return false;
    const auto& list = impl_->player_envs[static_cast<std::size_t>(player_id)].strategy; return !list.empty() && std::find(list.back().core_unit_ids.begin(), list.back().core_unit_ids.end(), def->internal_id) != list.back().core_unit_ids.end();
}
bool TFTEngine::is_core_trait(std::int32_t player_id, const std::string& trait) const {
    if (!impl_ || player_id < 0 || player_id >= static_cast<std::int32_t>(kPlayersPerLobby)) return false; impl_->ensure_open(); const auto& list = impl_->player_envs[static_cast<std::size_t>(player_id)].strategy;
    return !list.empty() && std::find(list.back().core_traits.begin(), list.back().core_traits.end(), trait) != list.back().core_traits.end();
}

std::array<AntiLoopMetrics, kPlayersPerLobby>
TFTEngine::get_anti_loop_metrics() const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    std::array<AntiLoopMetrics, kPlayersPerLobby> result{};

    for (std::size_t i = 0;
         i < kPlayersPerLobby;
         ++i) {
        const auto metrics =
            impl_->player_envs[i].anti_loop.get_metrics();

        result[i].execution_attempts =
            metrics.execution_attempts;
        result[i].execution_blocks =
            metrics.execution_blocks;
        result[i].blocked_reverse =
            metrics.blocked_reverse;
        result[i].blocked_oscillation =
            metrics.blocked_oscillation;
        result[i].blocked_explicit =
            metrics.blocked_explicit;
        result[i].blocked_bench_board_loop =
            metrics.blocked_bench_board_loop;
        result[i].blocked_cycle =
            metrics.blocked_cycle;
        result[i].board_moves_executed =
            metrics.board_moves_executed;
        result[i].bench_to_board_executed =
            metrics.bench_to_board_executed;

        result[i].mask_board_move_candidates =
            metrics.mask.board_move_candidates;
        result[i].mask_board_move_valid =
            metrics.mask.board_move_valid;
        result[i].mask_board_move_blocked_anti_loop =
            metrics.mask.board_move_blocked_anti_loop;
        result[i].mask_board_move_blocked_seen_state =
            metrics.mask.board_move_blocked_seen_state;
        result[i].mask_bench_move_candidates =
            metrics.mask.bench_move_candidates;
        result[i].mask_bench_move_valid =
            metrics.mask.bench_move_valid;
        result[i].mask_bench_move_blocked_anti_loop =
            metrics.mask.bench_move_blocked_anti_loop;
        result[i].mask_bench_move_blocked_seen_state =
            metrics.mask.bench_move_blocked_seen_state;
        result[i].mask_blocked_reverse = metrics.mask.blocked_reverse;
        result[i].mask_blocked_oscillation = metrics.mask.blocked_oscillation;
        result[i].mask_blocked_explicit = metrics.mask.blocked_explicit;
        result[i].mask_blocked_bench_board_loop =
            metrics.mask.blocked_bench_board_loop;
        result[i].mask_blocked_cycle = metrics.mask.blocked_cycle;
    }

    return result;
}

// ============================================================
// ESTADO PÚBLICO
// ============================================================

PublicState TFTEngine::get_public_state() const {
    if (!impl_) {
        throw std::runtime_error(
            "TFTEngine nao possui estado interno."
        );
    }

    impl_->ensure_open();

    PublicState state{};

    state.stage = impl_->stage;
    state.round = impl_->round + 1;
    state.round_index = impl_->round;
    state.finished = impl_->done;
    state.winner_id = impl_->winner_id;
    state.ranking = impl_->ranking;

    state.players.reserve(kPlayersPerLobby);

    state.alive_players.reserve(
        static_cast<std::size_t>(impl_->alive_players)
    );

    for (const auto& env : impl_->player_envs) {
        const auto& player = env.info;
        state.players.push_back(player);

        if (player.alive) {
            state.alive_players.push_back(player.player_id);
        }
    }

    return state;
}

// ============================================================
// DIMENSÃO DAS OBSERVAÇÕES
// ============================================================

std::size_t TFTEngine::observation_dim() const noexcept {
    return impl_ ? impl_->observation_dim() : 0;
}

// ============================================================
// IDENTIFICADOR DO EPISÓDIO
// ============================================================

std::uint64_t TFTEngine::episode_id() const noexcept {
    return impl_ ? impl_->episode_id : 0;
}

// ============================================================
// RODADA
// ============================================================

std::uint32_t TFTEngine::round() const noexcept {
    return impl_ ? impl_->round : 0;
}

// ============================================================
// ESTÁGIO
// ============================================================

std::uint32_t TFTEngine::stage() const noexcept {
    return impl_ ? impl_->stage : 1;
}

// ============================================================
// JOGADORES VIVOS
// ============================================================

std::int32_t TFTEngine::alive_players() const noexcept {
    return impl_ ? impl_->alive_players : 0;
}

// ============================================================
// ENCERRAMENTO
// ============================================================

bool TFTEngine::is_done() const noexcept {
    return !impl_ || impl_->done || impl_->closed;
}

// ============================================================
// CONSULTAS AO POOL
// ============================================================

std::int32_t TFTEngine::pool_available(
    std::int32_t champion_internal_id
) const {
    if (!impl_ || !impl_->pool) {
        throw std::runtime_error(
            "UnitPool indisponivel."
        );
    }

    return impl_->pool->available(champion_internal_id);
}

std::int32_t TFTEngine::pool_initial_count(
    std::int32_t champion_internal_id
) const {
    if (!impl_ || !impl_->pool) {
        throw std::runtime_error(
            "UnitPool indisponivel."
        );
    }

    return impl_->pool->initial_count(champion_internal_id);
}

std::int64_t TFTEngine::pool_total_available() const {
    if (!impl_ || !impl_->pool) {
        throw std::runtime_error(
            "UnitPool indisponivel."
        );
    }

    return impl_->pool->total_available();
}

// ============================================================
// FECHAMENTO
// ============================================================

void TFTEngine::close() noexcept {
    if (!impl_) {
        return;
    }

    impl_->closed = true;
}

} // namespace tft
