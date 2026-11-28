#pragma once

#include "ai/coordinator/unifiedteamvalue.h"
#include <QString>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace Coordinator
{
    inline constexpr int UNIFIED_DIMINISHING_ACTOR_PIVOT = 50;
    inline constexpr int UNIFIED_SEARCH_MAX_SWEEPS = 4;
    inline constexpr int UNIFIED_SEARCH_LARGE_ARMY_SWEEPS = 2;
    inline constexpr int UNIFIED_SEARCH_BASE_EVALS = 10000;
    inline constexpr int UNIFIED_EVALS_PER_ACTOR = 1000;
    inline constexpr int UNIFIED_LARGE_ARMY_MAX_EVALS = 4500;
    inline constexpr int UNIFIED_SEARCH_HARD_MAX_EVALS =
        UNIFIED_SEARCH_BASE_EVALS + UNIFIED_DIMINISHING_ACTOR_PIVOT * UNIFIED_EVALS_PER_ACTOR;
    inline constexpr int UNIFIED_SEARCH_MAX_EVALS = UNIFIED_SEARCH_BASE_EVALS;
    inline constexpr int UNIFIED_PAIR_PASS_EVAL_RESERVE = 400;
    inline constexpr int UNIFIED_PAIR_PASS_RESERVE_DIVISOR = 6;
    inline constexpr int UNIFIED_WORK_RESERVE_DIVISOR = 4;
    inline constexpr std::int64_t UNIFIED_PAIR_PASS_WORK_RESERVE = 60'000'000;
    inline constexpr std::size_t UNIFIED_MAX_FIRE_CANDIDATES = 10;
    inline constexpr std::size_t UNIFIED_MAX_POSITIONAL_CANDIDATES = 10;
    inline constexpr std::size_t UNIFIED_TIGHT_FIRE_CANDIDATES = 5;
    inline constexpr std::size_t UNIFIED_TIGHT_POSITIONAL_CANDIDATES = 4;
    inline constexpr std::size_t UNIFIED_LARGE_ARMY_FIRE_CANDIDATES = UNIFIED_TIGHT_FIRE_CANDIDATES;
    inline constexpr std::size_t UNIFIED_LARGE_ARMY_POSITIONAL_CANDIDATES = UNIFIED_TIGHT_POSITIONAL_CANDIDATES;
    inline constexpr int UNIFIED_TIGHT_EVALS_PER_REMAINING = 14;
    inline constexpr std::int64_t UNIFIED_TIGHT_WORK_FACTOR = 4;
    inline constexpr std::size_t UNIFIED_VACATE_PARTNER_LIMIT = 8;
    inline constexpr std::size_t UNIFIED_LARGE_ARMY_VACATE_PARTNER_LIMIT = 4;
    inline constexpr std::size_t UNIFIED_SCREEN_CANDIDATE_LIMIT = 12;
    inline constexpr std::size_t UNIFIED_LARGE_ARMY_SCREEN_CANDIDATE_LIMIT = 6;
    inline constexpr std::size_t UNIFIED_MAX_BLOCKED_ADVANCE_OPTS = 3;
    inline constexpr std::size_t UNIFIED_MAX_BREAKTHROUGH_TRIALS_PER_ACTOR = 4;
    inline constexpr std::size_t UNIFIED_MAX_JOINT_BREAKTHROUGH_TARGETS = 6;
    inline constexpr std::size_t UNIFIED_MAX_MOVERS_PER_BREAKTHROUGH_TARGET = 3;
    inline constexpr std::size_t UNIFIED_MAX_ADDITIONAL_WALLBREAK_KILLERS = 3;
    inline constexpr std::size_t UNIFIED_MAX_WALLBREAK_OPTS_PER_KILLER = 4;
    inline constexpr std::size_t UNIFIED_MAX_WALLBREAK_KILLER_POOL = 6;
    inline constexpr std::size_t UNIFIED_MAX_WALLBREAK_COMBOS_PER_MOVER = 3;

    // Generous eval budget up to 50 own actors; sharp cliff past 50 so massive maps stay fast.
    inline constexpr int unifiedDynamicEvalBudget(int actorCount)
    {
        const int clamped = std::max(0, actorCount);
        if (clamped > UNIFIED_DIMINISHING_ACTOR_PIVOT)
            return UNIFIED_LARGE_ARMY_MAX_EVALS;
        const int scaled = UNIFIED_SEARCH_BASE_EVALS + clamped * UNIFIED_EVALS_PER_ACTOR;
        return std::min(UNIFIED_SEARCH_HARD_MAX_EVALS, scaled);
    }

    using UnifiedAllowedFn = std::function<bool(const std::vector<int> &)>;

    struct UnifiedTeamSearchResult
    {
        std::vector<int> bestTeam;
        std::vector<CombatFireOrder> bestFireOrders;
        UnifiedTeamScore bestScore;
        UnifiedTeamScore baselineScore;
        bool improved{false};
        int sweeps{0};
        int evals{0};
        int pruned{0};
        int pairImprovements{0};
        QString winningSeed;
    };

    inline int findInPlaceWaitCandidate(const MasterActor & member)
    {
        for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
        {
            const auto & c = member.candidates[static_cast<std::size_t>(opt)];
            if (c.plannable && c.actionKind == PlanBundleKind::Wait && c.destination == c.origin)
                return opt;
        }
        return NO_CANDIDATE;
    }

    inline int findInPlaceHoldOrCaptureCandidate(const MasterActor & member)
    {
        for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
        {
            const auto & c = member.candidates[static_cast<std::size_t>(opt)];
            if (c.plannable && c.capture.has_value() && c.destination == c.origin)
                return opt;
        }
        return findInPlaceWaitCandidate(member);
    }

    inline bool unifiedCanChangeSlot(const MasterAssignmentProblem & problem, const MasterActor & member)
    {
        return problem.canChangeActor(member.canonicalSlot) || problem.canChangeCombatHelper(member);
    }

    // Builds a clean seed: stock-proposed cappers + in-place Wait for combat units (no Greedy panic).
    std::vector<int> buildCleanStockSeed(
        const MasterAssignmentProblem & problem, std::span<const int> baseline,
        std::span<const int> stockProposed);


    std::vector<int> buildAllWaitSeed(
        const MasterAssignmentProblem & problem, std::span<const int> baseline);


    std::vector<int> buildSafeStockAndClearFactoriesSeed(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        std::span<const PropertyFacts> properties, std::span<const int> baseline,
        std::span<const int> stockProposed,
        const UnifiedTeamEvaluator & evaluator);


    std::vector<int> buildDirectUnifiedSeed(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        std::span<const PropertyFacts> properties, std::span<const int> baseline,
        const UnifiedTeamEvaluator & evaluator);


    UnifiedTeamSearchResult optimizeUnifiedTeam(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const PropertyStockField & field, std::span<const PropertyFacts> properties,
        CombatTerrain & local, const ReplyBoard & actingBoard,
        std::span<const int> startingTeam, std::span<const int> stockProposed,
        std::int64_t replayTariff, std::int64_t & work,
        UnifiedHqRankFn hqRankFn, UnifiedBuildSlotsFn buildSlotsFn, UnifiedAllowedFn allowedByPrior,
        const UnifiedProductionModel * productionModel = nullptr);

}
