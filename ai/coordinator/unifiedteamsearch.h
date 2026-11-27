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
    inline constexpr std::size_t UNIFIED_MAX_ADDITIONAL_WALLBREAK_KILLERS = 2;

    // Generous eval budget up to 50 own actors; sharp cliff past 50 so massive maps stay fast.
    inline constexpr int unifiedDynamicEvalBudget(int actorCount)
    {
        const int clamped = std::max(0, actorCount);
        if (clamped > UNIFIED_DIMINISHING_ACTOR_PIVOT)
            return UNIFIED_LARGE_ARMY_MAX_EVALS;
        const int scaled = UNIFIED_SEARCH_BASE_EVALS + clamped * UNIFIED_EVALS_PER_ACTOR;
        return std::min(UNIFIED_SEARCH_HARD_MAX_EVALS, scaled);
    }

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
    inline std::vector<int> buildCleanStockSeed(
        const MasterAssignmentProblem & problem, std::span<const int> baseline,
        std::span<const int> stockProposed)
    {
        std::vector<int> seed(baseline.begin(), baseline.end());
        if (seed.size() != problem.actors().size()) return seed;
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member)) continue;
            if (member.stockRow != NO_STOCK_ROW && !stockProposed.empty() &&
                slot < static_cast<int>(stockProposed.size()))
            {
                const int prop = stockProposed[static_cast<std::size_t>(slot)];
                if (prop >= 0 && prop < static_cast<int>(member.candidates.size()) &&
                    member.candidates[static_cast<std::size_t>(prop)].plannable)
                    seed[static_cast<std::size_t>(slot)] = prop;
            }
            else if (member.stockRow == NO_STOCK_ROW)
            {
                const int waitOpt = findInPlaceWaitCandidate(member);
                if (waitOpt != NO_CANDIDATE)
                    seed[static_cast<std::size_t>(slot)] = waitOpt;
            }
        }
        // Resolve any destination collision between a stationary combat unit and a moving capper.
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member) || member.stockRow != NO_STOCK_ROW) continue;
            const TilePoint dest = member.candidates[static_cast<std::size_t>(seed[static_cast<std::size_t>(slot)])].destination;
            if (!openRefinementDestinationTaken(problem, seed, slot, dest)) continue;
            const int orig = baseline[static_cast<std::size_t>(slot)];
            if (!openRefinementDestinationTaken(problem, seed, slot, member.candidates[static_cast<std::size_t>(orig)].destination))
                seed[static_cast<std::size_t>(slot)] = orig;
            else
            {
                for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (c.plannable && c.targetUnitId == NO_UNIT &&
                        !openRefinementDestinationTaken(problem, seed, slot, c.destination))
                    {
                        seed[static_cast<std::size_t>(slot)] = opt;
                        break;
                    }
                }
            }
        }
        return seed;
    }

    inline std::vector<int> buildAllWaitSeed(
        const MasterAssignmentProblem & problem, std::span<const int> baseline)
    {
        std::vector<int> seed(baseline.begin(), baseline.end());
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member)) continue;
            const int waitOpt = findInPlaceWaitCandidate(member);
            if (waitOpt != NO_CANDIDATE &&
                !openRefinementDestinationTaken(problem, seed, slot, member.candidates[static_cast<std::size_t>(waitOpt)].destination))
                seed[static_cast<std::size_t>(slot)] = waitOpt;
        }
        return seed;
    }

    inline std::vector<int> buildSafeStockAndClearFactoriesSeed(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        std::span<const PropertyFacts> properties, std::span<const int> baseline,
        std::span<const int> stockProposed)
    {
        std::vector<int> seed = buildAllWaitSeed(problem, baseline);
        if (seed.size() != problem.actors().size()) return seed;
        std::vector<TilePoint> friendlyFactories;
        for (const auto & prop : properties)
        {
            if (prop.canProduce && prop.ownerId == knowledge.observerId())
                friendlyFactories.push_back({prop.x, prop.y});
        }
        const auto isSafeCandidate = [](const MasterCandidate & c) {
            return !c.source.replyFacts || !c.source.replyFacts->tile ||
                c.source.replyFacts->tile->shooters.empty();
        };
        // Ready captures go first even if stock-control was skipped or proposed something else.
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member) || member.stockRow == NO_STOCK_ROW) continue;
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (c.plannable && c.capture.has_value() && c.capture->turnsUntilOwned <= 0 &&
                    !openRefinementDestinationTaken(problem, seed, slot, c.destination))
                {
                    seed[static_cast<std::size_t>(slot)] = opt;
                    break;
                }
            }
        }
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member) || member.stockRow == NO_STOCK_ROW) continue;
            if (seed[static_cast<std::size_t>(slot)] != baseline[static_cast<std::size_t>(slot)] &&
                member.candidates[static_cast<std::size_t>(seed[static_cast<std::size_t>(slot)])].capture.has_value())
                continue;
            if (stockProposed.empty() || slot >= static_cast<int>(stockProposed.size())) continue;
            const int prop = stockProposed[static_cast<std::size_t>(slot)];
            if (prop < 0 || prop >= static_cast<int>(member.candidates.size())) continue;
            const auto & c = member.candidates[static_cast<std::size_t>(prop)];
            const bool readyCap = c.capture.has_value() && c.capture->turnsUntilOwned <= 0;
            if (c.plannable && (readyCap || isSafeCandidate(c)) &&
                !openRefinementDestinationTaken(problem, seed, slot, c.destination))
                seed[static_cast<std::size_t>(slot)] = prop;
        }
        // Clear any unit still blocking a friendly production building onto a safe open tile.
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member)) continue;
            const TilePoint curDest = member.candidates[static_cast<std::size_t>(seed[static_cast<std::size_t>(slot)])].destination;
            if (std::ranges::find(friendlyFactories, curDest) == friendlyFactories.end()) continue;
            int bestOpt = NO_CANDIDATE;
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || c.targetUnitId != NO_UNIT) continue;
                if (std::ranges::find(friendlyFactories, c.destination) != friendlyFactories.end()) continue;
                if (openRefinementDestinationTaken(problem, seed, slot, c.destination)) continue;
                if (!isSafeCandidate(c)) continue;
                if (c.capture.has_value())
                {
                    bestOpt = opt;
                    break;
                }
                if (bestOpt == NO_CANDIDATE)
                    bestOpt = opt;
            }
            if (bestOpt != NO_CANDIDATE)
                seed[static_cast<std::size_t>(slot)] = bestOpt;
        }
        return seed;
    }

    inline std::vector<int> buildDirectUnifiedSeed(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        std::span<const PropertyFacts> properties, std::span<const int> baseline)
    {
        std::vector<int> seed = buildAllWaitSeed(problem, baseline);
        if (seed.size() != problem.actors().size()) return seed;
        std::vector<TilePoint> friendlyFactories;
        for (const auto & prop : properties)
        {
            if (prop.canProduce && prop.ownerId == knowledge.observerId())
                friendlyFactories.push_back({prop.x, prop.y});
        }
        const auto isSafeCandidate = [](const MasterCandidate & c) {
            return !c.source.replyFacts || !c.source.replyFacts->tile ||
                c.source.replyFacts->tile->shooters.empty();
        };
        const auto assignCapturePass = [&](bool readyOnly, bool safeOnly) {
            for (const auto & member : problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (!unifiedCanChangeSlot(problem, member) || member.stockRow == NO_STOCK_ROW) continue;
                const auto & curCand = member.candidates[static_cast<std::size_t>(seed[static_cast<std::size_t>(slot)])];
                if (curCand.capture.has_value()) continue;
                for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (!c.plannable || !c.capture.has_value()) continue;
                    if (readyOnly && c.capture->turnsUntilOwned > 0) continue;
                    if (safeOnly && !isSafeCandidate(c)) continue;
                    if (!openRefinementDestinationTaken(problem, seed, slot, c.destination))
                    {
                        seed[static_cast<std::size_t>(slot)] = opt;
                        break;
                    }
                }
            }
        };
        assignCapturePass(true, false);
        assignCapturePass(false, true);
        // Clear any unit still blocking a friendly production building onto a safe open tile.
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member)) continue;
            const TilePoint curDest = member.candidates[static_cast<std::size_t>(seed[static_cast<std::size_t>(slot)])].destination;
            if (std::ranges::find(friendlyFactories, curDest) == friendlyFactories.end()) continue;
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || c.targetUnitId != NO_UNIT) continue;
                if (std::ranges::find(friendlyFactories, c.destination) != friendlyFactories.end()) continue;
                if (openRefinementDestinationTaken(problem, seed, slot, c.destination)) continue;
                if (!isSafeCandidate(c)) continue;
                seed[static_cast<std::size_t>(slot)] = opt;
                break;
            }
        }
        return seed;
    }

    template <class HqRankFn, class BuildSlotsFn, class AllowedFn>
    inline UnifiedTeamSearchResult optimizeUnifiedTeam(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const PropertyStockField & field, std::span<const PropertyFacts> properties,
        CombatTerrain & local, const ReplyBoard & actingBoard,
        std::span<const int> startingTeam, std::span<const int> stockProposed,
        std::int64_t replayTariff, std::int64_t & work,
        HqRankFn hqRankFn, BuildSlotsFn buildSlotsFn, AllowedFn allowedByPrior,
        const UnifiedProductionModel * productionModel = nullptr)
    {
        UnifiedTeamSearchResult out;
        out.bestTeam.assign(startingTeam.begin(), startingTeam.end());
        if (out.bestTeam.size() != problem.actors().size()) return out;

        UnifiedTeamEvaluator evaluator(problem, knowledge, field, properties, local, actingBoard,
                                       replayTariff, work, std::move(hqRankFn), std::move(buildSlotsFn),
                                       productionModel);

        const std::int64_t workBeforeSeeds = work;
        ReplyPassEvaluation bestEconEval;
        out.baselineScore = evaluator.evaluate(out.bestTeam, nullptr, &bestEconEval, {}, false);
        out.bestScore = out.baselineScore;
        out.winningSeed = QStringLiteral("STARTING_TEAM");
        {
            const auto startOrders = evaluator.buildDefaultFireOrders(out.bestTeam);
            if (!startOrders.empty())
            {
                ReplyPassEvaluation orderedEval;
                const auto orderedScore = evaluator.evaluate(
                    out.bestTeam, out.bestScore.valid ? &out.bestScore : nullptr, &orderedEval, startOrders);
                if (orderedScore.strictlyBetterThan(out.bestScore))
                {
                    out.bestScore = orderedScore;
                    bestEconEval = std::move(orderedEval);
                    out.bestFireOrders = startOrders;
                }
            }
        }

        const auto trySeed = [&](std::vector<int> candidate, const char * label) {
            if (candidate == out.bestTeam || !allowedByPrior(candidate)) return;
            ReplyPassEvaluation econEval;
            const auto score = evaluator.evaluate(candidate, out.bestScore.valid ? &out.bestScore : nullptr, &econEval);
            if (score.strictlyBetterThan(out.bestScore))
            {
                out.bestTeam = std::move(candidate);
                out.bestScore = score;
                bestEconEval = std::move(econEval);
                out.winningSeed = QString::fromLatin1(label);
            }
        };

        trySeed(buildSafeStockAndClearFactoriesSeed(problem, knowledge, properties, startingTeam, stockProposed),
                "SAFE_STOCK_AND_CLEAR_FACTORIES");
        trySeed(buildDirectUnifiedSeed(problem, knowledge, properties, startingTeam), "DIRECT_UNIFIED_SEED");
        trySeed(buildCleanStockSeed(problem, startingTeam, stockProposed), "CLEAN_STOCK_AND_WAIT");
        trySeed(buildAllWaitSeed(problem, startingTeam), "ALL_WAIT");
        if (!out.bestScore.valid)
        {
            out.evals = evaluator.evalCount();
            out.pruned = evaluator.prunedCount();
            return out;
        }

        std::vector<TilePoint> friendlyFactories;
        std::vector<TilePoint> enemyFactories;
        std::vector<TilePoint> unownedProps;
        std::vector<TilePoint> unownedFactories;
        std::vector<TilePoint> friendlyRepairSites;
        for (const auto & prop : properties)
        {
            const Relation rel = knowledge.relation(prop.ownerId);
            const bool isEnemyProp = prop.ownerId != NO_OWNER &&
                (knowledge.observerId() == NO_OWNER ? true : rel == Relation::Enemy);
            if (prop.canProduce && prop.ownerId == knowledge.observerId())
                friendlyFactories.push_back({prop.x, prop.y});
            if (prop.canProduce && isEnemyProp)
                enemyFactories.push_back({prop.x, prop.y});
            if (prop.capturable && (prop.ownerId == NO_OWNER || isEnemyProp))
            {
                unownedProps.push_back({prop.x, prop.y});
                if (prop.canProduce)
                    unownedFactories.push_back({prop.x, prop.y});
            }
            if (prop.ownerId == knowledge.observerId())
            {
                const TilePoint site{prop.x, prop.y};
                const bool repairable = local.canRepairOnTerrain
                    ? std::ranges::any_of(problem.actors(), [&](const MasterActor & m) {
                          return local.canRepairOnTerrain(m.knowledgeUnitIndex, site);
                      })
                    : (!prop.canProduce || prop.repairTypeMask != 0);
                if (repairable)
                    friendlyRepairSites.push_back(site);
            }
        }
        std::vector<TilePoint> enemyPositions;
        for (const auto & pawn : local.units)
        {
            if (pawn.present && pawn.enemy && pawn.tile != INVALID_TILE)
                enemyPositions.push_back(pawn.tile);
        }

        // Order slots: factory blockers & active cappers, exposed combat, capturing cappers & safe combat, surplus cappers.
        std::vector<int> order;
        order.reserve(problem.actors().size());
        for (const auto & member : problem.actors())
            if (unifiedCanChangeSlot(problem, member)) order.push_back(member.canonicalSlot);
        const auto slotPriority = [&](int slot) {
            const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
            const int ki = member.knowledgeUnitIndex;
            const MilliFunds cost = (ki >= 0 && ki < static_cast<int>(local.units.size()))
                ? local.units[static_cast<std::size_t>(ki)].replacementCost : 0;
            const bool armed = ki >= 0 && ki < static_cast<int>(knowledge.units().size())
                ? knowledge.units()[static_cast<std::size_t>(ki)].hasWeapons
                : true;
            const int unarmedOrder = armed ? 0 : 1;
            const int curOpt = out.bestTeam[static_cast<std::size_t>(slot)];
            const bool onFactory = curOpt >= 0 && curOpt < static_cast<int>(member.candidates.size()) &&
                std::ranges::find(friendlyFactories, member.candidates[static_cast<std::size_t>(curOpt)].origin) !=
                    friendlyFactories.end();
            if (onFactory) return std::tuple{0, unarmedOrder, -cost};
            if (member.stockRow != NO_STOCK_ROW)
            {
                const bool canCaptureNow = std::ranges::any_of(member.candidates, [](const MasterCandidate & c) {
                    return c.plannable && c.capture.has_value();
                });
                return std::tuple{canCaptureNow ? 3 : UNIFIED_SEARCH_MAX_SWEEPS, unarmedOrder, -cost};
            }
            const bool exposedNow = curOpt >= 0 && curOpt < static_cast<int>(member.candidates.size()) &&
                member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts &&
                member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts->tile &&
                !member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts->tile->shooters.empty();
            return std::tuple{exposedNow ? 1 : 2, unarmedOrder, -cost};
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return slotPriority(a) < slotPriority(b); });

        // Generous budget up to 50 own actors; sharp cliff past 50.
        const int actorCount = static_cast<int>(order.size());
        const bool largeArmy = actorCount > UNIFIED_DIMINISHING_ACTOR_PIVOT;
        const int maxEvals = unifiedDynamicEvalBudget(actorCount);
        const int pairEvalReserve = std::max(UNIFIED_PAIR_PASS_EVAL_RESERVE, maxEvals / UNIFIED_PAIR_PASS_RESERVE_DIVISOR);
        const std::int64_t seedEvals = std::max(1, evaluator.evalCount());
        const std::int64_t perEvalWork = std::max<std::int64_t>(replayTariff, (workBeforeSeeds - work) / seedEvals);
        const std::int64_t pairWorkReserve = std::min(work / UNIFIED_WORK_RESERVE_DIVISOR, perEvalWork * pairEvalReserve);
        const int sweepEvalCap = std::max(1, maxEvals - pairEvalReserve);
        const std::int64_t sweepWorkFloor = replayTariff + pairWorkReserve;
        const int maxSweeps = !largeArmy
            ? UNIFIED_SEARCH_MAX_SWEEPS : UNIFIED_SEARCH_LARGE_ARMY_SWEEPS;
        const std::size_t fullFireCap = !largeArmy
            ? UNIFIED_MAX_FIRE_CANDIDATES : UNIFIED_LARGE_ARMY_FIRE_CANDIDATES;
        const std::size_t fullPosCap = !largeArmy
            ? UNIFIED_MAX_POSITIONAL_CANDIDATES : UNIFIED_LARGE_ARMY_POSITIONAL_CANDIDATES;
        const std::size_t vacatePartnerLimit = !largeArmy
            ? UNIFIED_VACATE_PARTNER_LIMIT : UNIFIED_LARGE_ARMY_VACATE_PARTNER_LIMIT;
        const std::size_t screenCandidateLimit = !largeArmy
            ? UNIFIED_SCREEN_CANDIDATE_LIMIT : UNIFIED_LARGE_ARMY_SCREEN_CANDIDATE_LIMIT;

        std::map<std::int32_t, std::int32_t> targetHpByEngineId;
        for (std::size_t ki = 0; ki < local.units.size(); ++ki)
        {
            const auto & pawn = local.units[ki];
            if (!pawn.present || !pawn.enemy || pawn.engineUnitId == NO_UNIT) continue;
            const std::int32_t hpSteps = ki < knowledge.units().size()
                ? knowledge.units()[ki].hpRounded : UNIT_HP_STEPS;
            targetHpByEngineId[pawn.engineUnitId] = std::max(1, hpSteps);
        }
        const auto targetHpStepsFor = [&](std::int32_t engineId) -> std::int32_t {
            const auto it = targetHpByEngineId.find(engineId);
            return it != targetHpByEngineId.end() ? it->second : UNIT_HP_STEPS;
        };
        const auto isEnemyKilledByTeam = [&](std::span<const int> team, int skipSlot, int enemyKi) -> bool {
            if (enemyKi < 0 || enemyKi >= static_cast<int>(local.units.size()))
                return false;
            const std::int32_t reqEngineId = local.units[static_cast<std::size_t>(enemyKi)].engineUnitId;
            if (reqEngineId == NO_UNIT)
                return false;
            std::int32_t dmgSum = 0;
            for (const auto & other : problem.actors())
            {
                const int s = other.canonicalSlot;
                if (s == skipSlot || s < 0 || s >= static_cast<int>(team.size())) continue;
                const int oOpt = team[static_cast<std::size_t>(s)];
                if (oOpt < 0 || oOpt >= static_cast<int>(other.candidates.size())) continue;
                const auto & oc = other.candidates[static_cast<std::size_t>(oOpt)];
                if (oc.targetUnitId != reqEngineId) continue;
                if (const auto * comp = singleComponentOf(oc.source.bundle);
                    comp && comp->kind == ComponentKind::Fire)
                    dmgSum += comp->fire.damageSteps;
            }
            return dmgSum >= targetHpStepsFor(reqEngineId);
        };
        const auto isSafeForTeam = [&](const MasterCandidate & c, std::span<const int> team, int skipSlot) {
            if (!c.source.replyFacts || !c.source.replyFacts->tile)
                return true;
            for (const auto & sh : c.source.replyFacts->tile->shooters)
            {
                if (!isEnemyKilledByTeam(team, skipSlot, sh.shooterIndex))
                    return false;
            }
            return true;
        };
        const auto isSafeCand = [&](const MasterCandidate & c) {
            return isSafeForTeam(c, out.bestTeam, NO_REPLY_PASS_ACTOR);
        };
        const auto breakthroughDepsMet = [&](std::span<const int> team, int skipSlot,
                                             const MasterCandidate & c) -> bool {
            if (c.requiredKilledEnemyIds.empty()) return true;
            for (const std::int32_t reqId : c.requiredKilledEnemyIds)
            {
                std::int32_t dmgSum = 0;
                for (const auto & other : problem.actors())
                {
                    const int s = other.canonicalSlot;
                    if (s == skipSlot) continue;
                    const int oOpt = team[static_cast<std::size_t>(s)];
                    if (oOpt < 0 || oOpt >= static_cast<int>(other.candidates.size())) continue;
                    const auto & oc = other.candidates[static_cast<std::size_t>(oOpt)];
                    if (oc.targetUnitId != reqId) continue;
                    if (std::ranges::find(oc.requiredKilledEnemyIds, reqId) != oc.requiredKilledEnemyIds.end())
                        continue;
                    if (const auto * comp = singleComponentOf(oc.source.bundle);
                        comp && comp->kind == ComponentKind::Fire)
                        dmgSum += comp->fire.damageSteps;
                }
                if (dmgSum < targetHpStepsFor(reqId)) return false;
            }
            return true;
        };
        const auto shortlistForActor = [&](const MasterActor & member, int slot, int currentOpt,
                                           std::size_t maxFires, std::size_t maxPositional) {
            std::vector<int> picked;
            std::vector<bool> seen(member.candidates.size(), false);
            const auto addOpt = [&](int opt) {
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size()) || opt == currentOpt || seen[static_cast<std::size_t>(opt)])
                    return;
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || !breakthroughDepsMet(out.bestTeam, slot, c) ||
                    openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                    return;
                seen[static_cast<std::size_t>(opt)] = true;
                picked.push_back(opt);
            };
            // Always keep stock proposal, in-place capture (or wait), and every capture option.
            if (!stockProposed.empty() && slot < static_cast<int>(stockProposed.size()))
                addOpt(stockProposed[static_cast<std::size_t>(slot)]);
            const int holdOpt = findInPlaceHoldOrCaptureCandidate(member);
            addOpt(holdOpt);
            if (holdOpt == NO_CANDIDATE || !member.candidates[static_cast<std::size_t>(holdOpt)].capture.has_value() ||
                !isSafeCand(member.candidates[static_cast<std::size_t>(holdOpt)]))
                addOpt(findInPlaceWaitCandidate(member));
            std::vector<int> fires;
            std::vector<int> positionals;
            std::vector<TilePoint> capturableDestinations;
            if (member.stockRow != NO_STOCK_ROW)
            {
                for (const auto & c : member.candidates)
                {
                    if (c.plannable && c.capture.has_value())
                        capturableDestinations.push_back(c.destination);
                }
            }
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                if (opt == currentOpt || seen[static_cast<std::size_t>(opt)]) continue;
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || !breakthroughDepsMet(out.bestTeam, slot, c) ||
                    openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                    continue;
                if (c.capture.has_value())
                    addOpt(opt);
                else if (c.targetUnitId != NO_UNIT)
                    fires.push_back(opt);
                else if (std::ranges::find(enemyFactories, c.destination) != enemyFactories.end())
                    addOpt(opt);
                else if (std::ranges::find(capturableDestinations, c.destination) == capturableDestinations.end())
                    positionals.push_back(opt);
            }
            const auto fireKey = [&](const MasterCandidate & c) -> MilliFunds {
                if (isTerrainTargetId(c.targetUnitId))
                    return std::max(c.immediateValue.economicValue, evaluator.breakableTerrainStake(c.targetUnitId));
                return c.immediateValue.economicValue;
            };
            std::stable_sort(fires.begin(), fires.end(), [&](int a, int b) {
                const auto & ca = member.candidates[static_cast<std::size_t>(a)];
                const auto & cb = member.candidates[static_cast<std::size_t>(b)];
                const MilliFunds ka = fireKey(ca);
                const MilliFunds kb = fireKey(cb);
                if (ka != kb)
                    return ka > kb;
                return isSafeCand(ca) > isSafeCand(cb);
            });
            std::vector<int> seenFireTargets;
            std::size_t firesPicked = 0;
            for (const int opt : fires)
            {
                if (firesPicked >= maxFires) break;
                const int tid = member.candidates[static_cast<std::size_t>(opt)].targetUnitId;
                if (std::ranges::find(seenFireTargets, tid) != seenFireTargets.end()) continue;
                seenFireTargets.push_back(tid);
                const std::size_t beforeSz = picked.size();
                addOpt(opt);
                if (picked.size() > beforeSz)
                    ++firesPicked;
            }
            for (const int opt : fires)
            {
                if (firesPicked >= maxFires) break;
                const std::size_t beforeSz = picked.size();
                addOpt(opt);
                if (picked.size() > beforeSz)
                    ++firesPicked;
            }

            const int ki = member.knowledgeUnitIndex;
            const auto [uMinR, uMaxR] = evaluator.unitRangePublic(ki);
            const bool indirectActor = member.stockRow == NO_STOCK_ROW && uMinR > 1;
            const auto actorEnemies = evaluator.enemyPositionsFor(ki);
            const auto posRank = [&](int opt) {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                const int offFactory =
                    std::ranges::find(friendlyFactories, c.destination) == friendlyFactories.end() ? 1 : 0;
                int inRing = 0;
                int screenContacts = 0;
                if (indirectActor && !actorEnemies.empty())
                {
                    const bool ring = std::ranges::any_of(actorEnemies, [&](TilePoint e) {
                        const int d = manhattanDistance(c.destination, e);
                        return d >= uMinR && d <= uMaxR;
                    });
                    const bool jammed = evaluator.isJammedInReachableBlindSpot(ki, c.destination, uMinR);
                    inRing = jammed ? -1 : (ring ? 1 : 0);
                    if (!jammed)
                        screenContacts = evaluator.indirectScreenContactCount(ki, c.destination);
                }
                int minDist = 0;
                if (member.stockRow == NO_STOCK_ROW)
                {
                    if (!actorEnemies.empty())
                        minDist = evaluator.enemyPathDistance(ki, c.destination);
                    else if (!unownedProps.empty())
                        minDist = evaluator.combatObjectivePathDistance(ki, c.destination);
                }
                else if (!unownedProps.empty())
                {
                    minDist = evaluator.unownedPropertyPathDistance(ki, c.destination);
                }
                else if (!actorEnemies.empty())
                {
                    minDist = evaluator.enemyPathDistance(ki, c.destination);
                }
                const int stars = (local.defense && ki >= 0 && ki < static_cast<int>(local.units.size()))
                    ? local.defense(ki, c.destination).stars : 0;
                return std::tuple{offFactory, inRing, screenContacts, -minDist, stars};
            };
            std::vector<int> safePositionals;
            std::vector<int> exposedPositionals;
            for (const int opt : positionals)
            {
                if (isSafeCand(member.candidates[static_cast<std::size_t>(opt)]))
                    safePositionals.push_back(opt);
                else
                    exposedPositionals.push_back(opt);
            }
            std::stable_sort(safePositionals.begin(), safePositionals.end(), [&](int a, int b) {
                return posRank(a) > posRank(b);
            });
            std::stable_sort(exposedPositionals.begin(), exposedPositionals.end(), [&](int a, int b) {
                return posRank(a) > posRank(b);
            });
            // Keep directional diversity (factory approach, repair retreat for any damaged unit, F1 backup staging).
            if (!friendlyRepairSites.empty() && !safePositionals.empty() &&
                ki >= 0 && ki < static_cast<int>(knowledge.units().size()) &&
                knowledge.units()[static_cast<std::size_t>(ki)].hpRounded < UNIT_HP_STEPS)
            {
                int bestRepairOpt = NO_CANDIDATE;
                auto bestRepairKey = std::tuple{std::numeric_limits<int>::max(), std::numeric_limits<int>::min(),
                                                std::numeric_limits<int>::max()};
                for (const int opt : safePositionals)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    const int d = evaluator.repairPathDistance(ki, c.destination);
                    const int stars = (local.defense && ki >= 0 && ki < static_cast<int>(local.units.size()))
                        ? local.defense(ki, c.destination).stars : 0;
                    const int eDist = !actorEnemies.empty() ? evaluator.enemyPathDistance(ki, c.destination) : 0;
                    const auto key = std::tuple{d, -stars, eDist};
                    if (key < bestRepairKey)
                    {
                        bestRepairKey = key;
                        bestRepairOpt = opt;
                    }
                }
                if (bestRepairOpt != NO_CANDIDATE)
                    addOpt(bestRepairOpt);
            }
            if (member.stockRow == NO_STOCK_ROW && uMinR <= 1 && !safePositionals.empty() &&
                !bestEconEval.shots.empty() && ki >= 0 && ki < static_cast<int>(local.units.size()))
            {
                const int uMove = std::max(0, local.units[static_cast<std::size_t>(ki)].movement);
                if (uMove > 0)
                {
                    std::vector<std::pair<MilliFunds, TilePoint>> threatTiles;
                    for (const auto & s : bestEconEval.shots)
                    {
                        const MilliFunds stake = std::max<MilliFunds>(0, s.targetCapital - s.counterCapital);
                        if (s.firingTile != INVALID_TILE)
                            threatTiles.emplace_back(stake, s.firingTile);
                        if (s.target < bestEconEval.actors.size())
                        {
                            const auto & hitAct = bestEconEval.actors[s.target];
                            if (hitAct.actorSlot != slot && hitAct.actorSlot >= 0 &&
                                hitAct.actorSlot < static_cast<int>(problem.actors().size()))
                            {
                                const auto & hitMem = problem.actors()[static_cast<std::size_t>(hitAct.actorSlot)];
                                if (hitAct.candidateIndex >= 0 &&
                                    hitAct.candidateIndex < static_cast<int>(hitMem.candidates.size()))
                                    threatTiles.emplace_back(
                                        stake, hitMem.candidates[static_cast<std::size_t>(hitAct.candidateIndex)].destination);
                            }
                        }
                    }
                    std::stable_sort(threatTiles.begin(), threatTiles.end(), [](const auto & a, const auto & b) {
                        return a.first > b.first;
                    });
                    std::vector<TilePoint> uniqueThreats;
                    for (const auto & [stake, t] : threatTiles)
                    {
                        if (std::ranges::find(uniqueThreats, t) == uniqueThreats.end())
                            uniqueThreats.push_back(t);
                        if (uniqueThreats.size() >= 2)
                            break;
                    }
                    for (const TilePoint t : uniqueThreats)
                    {
                        int bestBackupOpt = NO_CANDIDATE;
                        auto bestBackupKey = std::tuple{std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
                        for (const int opt : safePositionals)
                        {
                            const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                            if (evaluator.attackRingPathCostPublic(ki, c.destination, t, uMinR, uMaxR) > uMove)
                                continue;
                            const int stars = local.defense ? local.defense(ki, c.destination).stars : 0;
                            const int eDist = !actorEnemies.empty() ? evaluator.enemyPathDistance(ki, c.destination) : 0;
                            const auto key = std::tuple{stars, -eDist};
                            if (key > bestBackupKey)
                            {
                                bestBackupKey = key;
                                bestBackupOpt = opt;
                            }
                        }
                        if (bestBackupOpt != NO_CANDIDATE)
                            addOpt(bestBackupOpt);
                    }
                }
            }
            if (member.stockRow != NO_STOCK_ROW && !unownedFactories.empty())
            {
                int bestFactoryOpt = NO_CANDIDATE;
                int bestFactoryDist = std::numeric_limits<int>::max();
                for (const int opt : safePositionals)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    const int d = evaluator.unownedFactoryPathDistance(ki, c.destination);
                    if (d < bestFactoryDist)
                    {
                        bestFactoryDist = d;
                        bestFactoryOpt = opt;
                    }
                }
                if (bestFactoryOpt != NO_CANDIDATE)
                    addOpt(bestFactoryOpt);
            }
            // Balance safe retreat/staging tiles with forward/firing-ring tiles so indirects and tanks explore both.
            const std::size_t safeKeep = (member.stockRow == NO_STOCK_ROW && maxPositional > 1 && !exposedPositionals.empty())
                ? (maxPositional + 1) / 2
                : maxPositional;
            const std::size_t exposedKeep = maxPositional / 2;
            for (std::size_t i = 0; i < safePositionals.size() && i < safeKeep; ++i)
                addOpt(safePositionals[i]);
            for (std::size_t i = 0; i < exposedPositionals.size() && i < exposedKeep; ++i)
                addOpt(exposedPositionals[i]);
            for (const int opt : safePositionals)
            {
                if (picked.size() >= maxFires + maxPositional) break;
                addOpt(opt);
            }
            return picked;
        };

        for (int sweep = 0; sweep < maxSweeps; ++sweep)
        {
            if (work < replayTariff) break;
            if (sweep > 0 && (evaluator.evalCount() >= sweepEvalCap || work < sweepWorkFloor)) break;
            bool sweepChanged = false;
            out.sweeps = sweep + 1;
            for (std::size_t idx = 0; idx < order.size(); ++idx)
            {
                if (work < replayTariff) break;
                if (sweep > 0 && (evaluator.evalCount() >= sweepEvalCap || work < sweepWorkFloor)) break;
                const int slot = order[idx];
                const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                const int currentOpt = out.bestTeam[static_cast<std::size_t>(slot)];
                const int remainingActors = std::max(1, static_cast<int>(order.size() - idx));
                const int remainingEvals = std::max(0, sweepEvalCap - evaluator.evalCount());
                const bool tight = largeArmy &&
                    (remainingEvals < remainingActors * UNIFIED_TIGHT_EVALS_PER_REMAINING ||
                     work < sweepWorkFloor + perEvalWork * static_cast<std::int64_t>(remainingActors) * UNIFIED_TIGHT_WORK_FACTOR);
                const std::size_t maxFires = (sweep == 0 && remainingEvals <= 0)
                    ? 1 : (tight ? UNIFIED_TIGHT_FIRE_CANDIDATES : fullFireCap);
                const std::size_t maxPos = (sweep == 0 && remainingEvals <= 0)
                    ? 1 : (tight ? UNIFIED_TIGHT_POSITIONAL_CANDIDATES : fullPosCap);
                const std::vector<int> candidates = shortlistForActor(member, slot, currentOpt, maxFires, maxPos);
                std::vector<int> trial = out.bestTeam;
                int chosenOpt = currentOpt;
                for (const int opt : candidates)
                {
                    if (work < replayTariff) break;
                    if (sweep > 0 && (evaluator.evalCount() >= sweepEvalCap || work < sweepWorkFloor)) break;
                    trial[static_cast<std::size_t>(slot)] = opt;
                    if (!allowedByPrior(trial)) continue;
                    ReplyPassEvaluation econEval;
                    const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                    if (score.strictlyBetterThan(out.bestScore))
                    {
                        out.bestScore = score;
                        chosenOpt = opt;
                        bestEconEval = std::move(econEval);
                    }
                }
                if (chosenOpt != currentOpt)
                {
                    out.bestTeam[static_cast<std::size_t>(slot)] = chosenOpt;
                    sweepChanged = true;
                }
            }
            if (!sweepChanged) break;
        }

        // Targeted 2-unit pair pass: vacate chains (A wants B's tile) and capper screening.
        const auto occupantSlotAt = [&](TilePoint tile, int skipSlot) -> int {
            for (const auto & other : problem.actors())
            {
                const int s = other.canonicalSlot;
                if (s == skipSlot) continue;
                const int opt = out.bestTeam[static_cast<std::size_t>(s)];
                if (other.candidates[static_cast<std::size_t>(opt)].destination == tile)
                    return s;
            }
            return -1;
        };

        for (const int slotA : order)
        {
            if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
            const auto & actorA = problem.actors()[static_cast<std::size_t>(slotA)];
            std::vector<int> blockedAdvanceOpts;
            if (actorA.stockRow == NO_STOCK_ROW && !enemyPositions.empty())
            {
                const int curOptA = out.bestTeam[static_cast<std::size_t>(slotA)];
                const auto & curCandA = actorA.candidates[static_cast<std::size_t>(curOptA)];
                if (curCandA.targetUnitId == NO_UNIT)
                {
                    const auto [uMinRA, uMaxRA] = evaluator.unitRangePublic(actorA.knowledgeUnitIndex);
                    const bool indirectA = uMinRA > 1;
                    auto advanceRank = [&](TilePoint dest) {
                        int inRing = 0;
                        if (indirectA)
                        {
                            const bool ring = std::ranges::any_of(enemyPositions, [&](TilePoint e) {
                                const int d = manhattanDistance(dest, e);
                                return d >= uMinRA && d <= uMaxRA;
                            });
                            inRing = ring ? 1 : 0;
                        }
                        const int d = evaluator.enemyPathDistance(actorA.knowledgeUnitIndex, dest);
                        return std::pair{inRing, -d};
                    };
                    const auto curRankA = advanceRank(curCandA.destination);
                    std::vector<std::pair<std::pair<int, int>, int>> rankedBlocked;
                    for (int opt = 0; opt < static_cast<int>(actorA.candidates.size()); ++opt)
                    {
                        if (opt == curOptA) continue;
                        const auto & c = actorA.candidates[static_cast<std::size_t>(opt)];
                        if (!c.plannable || c.targetUnitId != NO_UNIT || !isSafeForTeam(c, out.bestTeam, slotA)) continue;
                        if (std::ranges::find(friendlyFactories, c.destination) != friendlyFactories.end()) continue;
                        const int occ = occupantSlotAt(c.destination, slotA);
                        if (occ < 0) continue;
                        const auto & occActor = problem.actors()[static_cast<std::size_t>(occ)];
                        const auto & occCand = occActor.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(occ)])];
                        if (occCand.capture.has_value() || occCand.targetUnitId != NO_UNIT) continue;
                        const auto candRank = advanceRank(c.destination);
                        if (candRank > curRankA)
                            rankedBlocked.emplace_back(candRank, opt);
                    }
                    std::stable_sort(rankedBlocked.begin(), rankedBlocked.end(), [](const auto & a, const auto & b) {
                        return a.first > b.first;
                    });
                    for (std::size_t bi = 0; bi < rankedBlocked.size() && bi < UNIFIED_MAX_BLOCKED_ADVANCE_OPTS; ++bi)
                        blockedAdvanceOpts.push_back(rankedBlocked[bi].second);
                }
            }
            else if (actorA.stockRow != NO_STOCK_ROW && !unownedProps.empty())
            {
                const int curOptA = out.bestTeam[static_cast<std::size_t>(slotA)];
                const auto & curCandA = actorA.candidates[static_cast<std::size_t>(curOptA)];
                if (!curCandA.capture.has_value() && curCandA.targetUnitId == NO_UNIT)
                {
                    const int curDistA = evaluator.unownedPropertyPathDistance(
                        actorA.knowledgeUnitIndex, curCandA.destination);
                    std::vector<std::pair<int, int>> rankedBlocked;
                    for (int opt = 0; opt < static_cast<int>(actorA.candidates.size()); ++opt)
                    {
                        if (opt == curOptA) continue;
                        const auto & c = actorA.candidates[static_cast<std::size_t>(opt)];
                        if (!c.plannable || c.targetUnitId != NO_UNIT || !isSafeForTeam(c, out.bestTeam, slotA)) continue;
                        if (std::ranges::find(friendlyFactories, c.destination) != friendlyFactories.end()) continue;
                        const int occ = occupantSlotAt(c.destination, slotA);
                        if (occ < 0) continue;
                        const auto & occActor = problem.actors()[static_cast<std::size_t>(occ)];
                        const auto & occCand = occActor.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(occ)])];
                        if (occCand.capture.has_value() || occCand.targetUnitId != NO_UNIT) continue;
                        const int candDist = evaluator.unownedPropertyPathDistance(
                            actorA.knowledgeUnitIndex, c.destination);
                        if (candDist < curDistA)
                            rankedBlocked.emplace_back(-candDist, opt);
                    }
                    std::stable_sort(rankedBlocked.begin(), rankedBlocked.end(), [](const auto & a, const auto & b) {
                        return a.first > b.first;
                    });
                    for (std::size_t bi = 0; bi < rankedBlocked.size() && bi < UNIFIED_MAX_BLOCKED_ADVANCE_OPTS; ++bi)
                        blockedAdvanceOpts.push_back(rankedBlocked[bi].second);
                }
            }
            for (int optA = 0; optA < static_cast<int>(actorA.candidates.size()); ++optA)
            {
                if (optA == out.bestTeam[static_cast<std::size_t>(slotA)]) continue;
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & candA = actorA.candidates[static_cast<std::size_t>(optA)];
                if (!candA.plannable) continue;
                if (!candA.capture && candA.targetUnitId == NO_UNIT &&
                    std::ranges::find(blockedAdvanceOpts, optA) == blockedAdvanceOpts.end())
                    continue;
                const int slotB = occupantSlotAt(candA.destination, slotA);
                if (slotB < 0 || !unifiedCanChangeSlot(problem, problem.actors()[static_cast<std::size_t>(slotB)]))
                    continue;
                const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                std::vector<int> partnerOpts;
                partnerOpts.reserve(actorB.candidates.size());
                for (int optB = 0; optB < static_cast<int>(actorB.candidates.size()); ++optB)
                {
                    if (optB == out.bestTeam[static_cast<std::size_t>(slotB)]) continue;
                    const auto & candB = actorB.candidates[static_cast<std::size_t>(optB)];
                    if (!candB.plannable || candB.destination == candA.destination) continue;
                    if (openRefinementDestinationTaken(problem, out.bestTeam, slotB, candB.destination) &&
                        candB.destination != actorA.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotA)])].destination)
                        continue;
                    partnerOpts.push_back(optB);
                }
                const int kiB = actorB.knowledgeUnitIndex;
                const auto [minRB, maxRB] = evaluator.unitRangePublic(kiB);
                const auto enemiesB = evaluator.enemyPositionsFor(kiB);
                const auto partnerRankB = [&](int optB) {
                    const auto & cb = actorB.candidates[static_cast<std::size_t>(optB)];
                    const int isCaptureB = cb.capture.has_value() ? 1 : 0;
                    const int isFireB = cb.targetUnitId != NO_UNIT ? 1 : 0;
                    const int safe = isSafeForTeam(cb, out.bestTeam, slotB) ? 1 : 0;
                    const int offFactory =
                        std::ranges::find(friendlyFactories, cb.destination) == friendlyFactories.end() ? 1 : 0;
                    int inRingB = 0;
                    if (minRB > 1 && !enemiesB.empty())
                    {
                        const bool ring = std::ranges::any_of(enemiesB, [&](TilePoint e) {
                            const int d = manhattanDistance(cb.destination, e);
                            return d >= minRB && d <= maxRB;
                        });
                        const bool jammed = evaluator.isJammedInReachableBlindSpot(
                            kiB, cb.destination, minRB);
                        inRingB = (ring && !jammed) ? 1 : 0;
                    }
                    int distB = 0;
                    if (actorB.stockRow == NO_STOCK_ROW && !enemiesB.empty())
                        distB = evaluator.enemyPathDistance(kiB, cb.destination);
                    else if (!unownedProps.empty())
                        distB = evaluator.unownedPropertyPathDistance(kiB, cb.destination);
                    return std::tuple{isCaptureB, isFireB, safe, offFactory, inRingB, -distB};
                };
                std::stable_sort(partnerOpts.begin(), partnerOpts.end(), [&](int a, int b) {
                    return partnerRankB(a) > partnerRankB(b);
                });
                std::size_t triedB = 0;
                bool improvedPair = false;
                for (const int optB : partnerOpts)
                {
                    if (triedB >= vacatePartnerLimit ||
                        evaluator.evalCount() >= maxEvals || work < replayTariff)
                        break;
                    ++triedB;
                    std::vector<int> trial = out.bestTeam;
                    trial[static_cast<std::size_t>(slotA)] = optA;
                    trial[static_cast<std::size_t>(slotB)] = optB;
                    if (!allowedByPrior(trial)) continue;
                    ReplyPassEvaluation econEval;
                    const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                    if (score.strictlyBetterThan(out.bestScore))
                    {
                        out.bestTeam = std::move(trial);
                        out.bestScore = score;
                        bestEconEval = std::move(econEval);
                        improvedPair = true;
                    }
                }
                if (improvedPair)
                    ++out.pairImprovements;
            }
        }

        // Screen/support and vacated-tile swap pair pass for unchosen open captures.
        if (evaluator.evalCount() < maxEvals && work >= replayTariff)
        {
            for (const int capSlot : order)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & capper = problem.actors()[static_cast<std::size_t>(capSlot)];
                if (capper.stockRow == NO_STOCK_ROW) continue;
                std::vector<int> desiredOpts;
                const auto addDesired = [&](int opt) {
                    if (opt < 0 || opt >= static_cast<int>(capper.candidates.size())) return;
                    if (opt == out.bestTeam[static_cast<std::size_t>(capSlot)]) return;
                    const auto & c = capper.candidates[static_cast<std::size_t>(opt)];
                    if (!c.plannable || !c.capture.has_value()) return;
                    if (openRefinementDestinationTaken(problem, out.bestTeam, capSlot, c.destination)) return;
                    if (std::ranges::find(desiredOpts, opt) == desiredOpts.end())
                        desiredOpts.push_back(opt);
                };
                if (!stockProposed.empty() && capSlot < static_cast<int>(stockProposed.size()))
                    addDesired(stockProposed[static_cast<std::size_t>(capSlot)]);
                for (int opt = 0; opt < static_cast<int>(capper.candidates.size()); ++opt)
                    addDesired(opt);
                if (desiredOpts.empty()) continue;
                for (const int desiredOpt : desiredOpts)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    if (desiredOpt == out.bestTeam[static_cast<std::size_t>(capSlot)]) continue;
                    const TilePoint capDest = capper.candidates[static_cast<std::size_t>(desiredOpt)].destination;
                    if (openRefinementDestinationTaken(problem, out.bestTeam, capSlot, capDest)) continue;
                    const TilePoint curCapDest =
                        capper.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(capSlot)])].destination;
                    bool capImproved = false;
                    for (const int helperSlot : order)
                    {
                        if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                        const auto & helper = problem.actors()[static_cast<std::size_t>(helperSlot)];
                        if (helperSlot == capSlot) continue;
                        std::vector<int> helperOpts;
                        for (int optH = 0; optH < static_cast<int>(helper.candidates.size()); ++optH)
                        {
                            if (optH == out.bestTeam[static_cast<std::size_t>(helperSlot)]) continue;
                            const auto & candH = helper.candidates[static_cast<std::size_t>(optH)];
                            if (!candH.plannable || candH.destination == capDest) continue;
                            const bool takesVacatedCapTile = candH.destination == curCapDest && curCapDest != capDest;
                            if (!takesVacatedCapTile &&
                                openRefinementDestinationTaken(problem, out.bestTeam, helperSlot, candH.destination))
                                continue;
                            if (!takesVacatedCapTile && candH.targetUnitId == NO_UNIT &&
                                !evaluator.canScreenCaptureTile(helper.knowledgeUnitIndex, candH.destination, capDest))
                                continue;
                            helperOpts.push_back(optH);
                        }
                        std::stable_sort(helperOpts.begin(), helperOpts.end(), [&](int a, int b) {
                            const bool vacA = helper.candidates[static_cast<std::size_t>(a)].destination == curCapDest;
                            const bool vacB = helper.candidates[static_cast<std::size_t>(b)].destination == curCapDest;
                            return vacA > vacB;
                        });
                        std::size_t tried = 0;
                        for (const int optH : helperOpts)
                        {
                            if (tried >= screenCandidateLimit ||
                                evaluator.evalCount() >= maxEvals || work < replayTariff)
                                break;
                            ++tried;
                            std::vector<int> trial = out.bestTeam;
                            trial[static_cast<std::size_t>(capSlot)] = desiredOpt;
                            trial[static_cast<std::size_t>(helperSlot)] = optH;
                            if (!allowedByPrior(trial)) continue;
                            ReplyPassEvaluation econEval;
                            const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                            if (score.strictlyBetterThan(out.bestScore))
                            {
                                out.bestTeam = std::move(trial);
                                out.bestScore = score;
                                bestEconEval = std::move(econEval);
                                ++out.pairImprovements;
                                capImproved = true;
                                break;
                            }
                        }
                        if (capImproved) break;
                    }
                    if (capImproved) break;
                }
            }
        }

        // Coordinated 2-unit E0 evacuation & cheaper-screen plug swap to break meatshield local minima.
        if (evaluator.evalCount() < maxEvals && work >= replayTariff && bestEconEval.valued())
        {
            const auto actorTakesE0Damage = [&](int slot) {
                for (const auto & ae : bestEconEval.actors)
                {
                    if (ae.actorSlot == slot)
                        return ae.stepsAfterReply < ae.stepsAfterOwnActions;
                }
                return false;
            };
            const auto safeRetreatOptsFor = [&](int slot, std::span<const int> team) {
                std::vector<int> safeOpts;
                const auto & actor = problem.actors()[static_cast<std::size_t>(slot)];
                const int ki = actor.knowledgeUnitIndex;
                const auto [uMinR, uMaxR] = evaluator.unitRangePublic(ki);
                for (int opt = 0; opt < static_cast<int>(actor.candidates.size()); ++opt)
                {
                    const auto & c = actor.candidates[static_cast<std::size_t>(opt)];
                    if (!c.plannable || !isSafeForTeam(c, team, slot)) continue;
                    if (openRefinementDestinationTaken(problem, team, slot, c.destination)) continue;
                    safeOpts.push_back(opt);
                }
                std::stable_sort(safeOpts.begin(), safeOpts.end(), [&](int a, int b) {
                    const auto & ca = actor.candidates[static_cast<std::size_t>(a)];
                    const auto & cb = actor.candidates[static_cast<std::size_t>(b)];
                    const int offA = std::ranges::find(friendlyFactories, ca.destination) == friendlyFactories.end() ? 1 : 0;
                    const int offB = std::ranges::find(friendlyFactories, cb.destination) == friendlyFactories.end() ? 1 : 0;
                    const int starsA = (local.defense && ki >= 0 && ki < static_cast<int>(local.units.size()))
                        ? local.defense(ki, ca.destination).stars : 0;
                    const int starsB = (local.defense && ki >= 0 && ki < static_cast<int>(local.units.size()))
                        ? local.defense(ki, cb.destination).stars : 0;
                    const auto & enemiesForKi = evaluator.enemyPositionsFor(ki);
                    int ringA = 0;
                    int ringB = 0;
                    if (uMinR > 1 && !enemiesForKi.empty())
                    {
                        ringA = std::ranges::any_of(enemiesForKi, [&](TilePoint e) {
                            const int d = manhattanDistance(ca.destination, e);
                            return d >= uMinR && d <= uMaxR;
                        }) ? 1 : 0;
                        ringB = std::ranges::any_of(enemiesForKi, [&](TilePoint e) {
                            const int d = manhattanDistance(cb.destination, e);
                            return d >= uMinR && d <= uMaxR;
                        }) ? 1 : 0;
                    }
                    const int dA = !enemiesForKi.empty() ? evaluator.enemyPathDistance(ki, ca.destination) : 0;
                    const int dB = !enemiesForKi.empty() ? evaluator.enemyPathDistance(ki, cb.destination) : 0;
                    return std::tuple{offA, ringA, -dA, starsA} > std::tuple{offB, ringB, -dB, starsB};
                });
                if (safeOpts.size() > 2)
                    safeOpts.resize(2);
                return safeOpts;
            };

            for (const int slotA : order)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & actorA = problem.actors()[static_cast<std::size_t>(slotA)];
                const auto & curCandA = actorA.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotA)])];
                if (curCandA.capture.has_value() || !actorTakesE0Damage(slotA))
                    continue;
                const std::vector<int> safeOptsA = safeRetreatOptsFor(slotA, out.bestTeam);
                if (safeOptsA.empty()) continue;

                // Pass A: simultaneous 2-unit retreat when A's solo retreat exposed B (or B shielded behind A).
                std::size_t triedPartners = 0;
                for (const int slotB : order)
                {
                    if (slotB == slotA || triedPartners >= UNIFIED_VACATE_PARTNER_LIMIT ||
                        evaluator.evalCount() >= maxEvals || work < replayTariff)
                        break;
                    const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                    const auto & curCandB = actorB.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotB)])];
                    if (curCandB.capture.has_value() || isSafeForTeam(curCandB, out.bestTeam, slotB))
                        continue;
                    const std::vector<int> safeOptsB = safeRetreatOptsFor(slotB, out.bestTeam);
                    if (safeOptsB.empty()) continue;
                    ++triedPartners;
                    bool improved = false;
                    for (const int optA : safeOptsA)
                    {
                        const TilePoint destA = actorA.candidates[static_cast<std::size_t>(optA)].destination;
                        for (const int optB : safeOptsB)
                        {
                            if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                            const TilePoint destB = actorB.candidates[static_cast<std::size_t>(optB)].destination;
                            if (destA == destB) continue;
                            std::vector<int> trial = out.bestTeam;
                            trial[static_cast<std::size_t>(slotA)] = optA;
                            trial[static_cast<std::size_t>(slotB)] = optB;
                            if (!allowedByPrior(trial)) continue;
                            ReplyPassEvaluation econEval;
                            const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                            if (score.strictlyBetterThan(out.bestScore))
                            {
                                out.bestTeam = std::move(trial);
                                out.bestScore = score;
                                bestEconEval = std::move(econEval);
                                ++out.pairImprovements;
                                improved = true;
                                break;
                            }
                        }
                        if (improved) break;
                    }
                    if (improved) break;
                }

                // Pass B: if slotA is still taking E0 damage on plugTile, swap a cheaper direct teammate into plugTile.
                const auto & latestCurA = actorA.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotA)])];
                const int kiA = actorA.knowledgeUnitIndex;
                const MilliFunds costA = (kiA >= 0 && kiA < static_cast<int>(local.units.size()))
                    ? local.units[static_cast<std::size_t>(kiA)].replacementCost : 0;
                if (!latestCurA.capture.has_value() && actorTakesE0Damage(slotA) && costA > 0)
                {
                    const TilePoint plugTile = latestCurA.destination;
                    for (const int slotB : order)
                    {
                        if (slotB == slotA || evaluator.evalCount() >= maxEvals || work < replayTariff)
                            break;
                        const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                        const int kiB = actorB.knowledgeUnitIndex;
                        const MilliFunds costB = (kiB >= 0 && kiB < static_cast<int>(local.units.size()))
                            ? local.units[static_cast<std::size_t>(kiB)].replacementCost : 0;
                        const auto [uMinRB, uMaxRB] = evaluator.unitRangePublic(kiB);
                        (void)uMaxRB;
                        if (costB <= 0 || costB > costA || uMinRB > 1)
                            continue;
                        int swapOptB = NO_CANDIDATE;
                        for (int optB = 0; optB < static_cast<int>(actorB.candidates.size()); ++optB)
                        {
                            const auto & cB = actorB.candidates[static_cast<std::size_t>(optB)];
                            if (cB.plannable && !cB.capture.has_value() && cB.destination == plugTile)
                            {
                                swapOptB = optB;
                                break;
                            }
                        }
                        if (swapOptB == NO_CANDIDATE)
                            continue;
                        const std::vector<int> safeForA = safeRetreatOptsFor(slotA, out.bestTeam);
                        for (const int optA : safeForA)
                        {
                            if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                            const TilePoint destA = actorA.candidates[static_cast<std::size_t>(optA)].destination;
                            if (destA == plugTile) continue;
                            std::vector<int> trial = out.bestTeam;
                            trial[static_cast<std::size_t>(slotA)] = optA;
                            trial[static_cast<std::size_t>(slotB)] = swapOptB;
                            if (!allowedByPrior(trial)) continue;
                            ReplyPassEvaluation econEval;
                            const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                            if (score.strictlyBetterThan(out.bestScore))
                            {
                                out.bestTeam = std::move(trial);
                                out.bestScore = score;
                                bestEconEval = std::move(econEval);
                                ++out.pairImprovements;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Fire-handoff pair pass: let a stronger finisher replace weaker shooter(s) holding target damage capacity.
        std::vector<int> releasedByHandoff;
        if (evaluator.evalCount() < maxEvals && work >= replayTariff)
        {
            for (const int slotA : order)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & actorA = problem.actors()[static_cast<std::size_t>(slotA)];
                const int curA = out.bestTeam[static_cast<std::size_t>(slotA)];
                if (actorA.candidates[static_cast<std::size_t>(curA)].targetUnitId != NO_UNIT) continue;
                for (int optA = 0; optA < static_cast<int>(actorA.candidates.size()); ++optA)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    const auto & candA = actorA.candidates[static_cast<std::size_t>(optA)];
                    if (!candA.plannable || candA.targetUnitId == NO_UNIT) continue;
                    const int blocker = occupantSlotAt(candA.destination, slotA);
                    std::vector<int> sameTargetShooters;
                    int weakestShooterSlot = -1;
                    std::int32_t weakestDmg = std::numeric_limits<std::int32_t>::max();
                    for (const int slotB : order)
                    {
                        if (slotB == slotA) continue;
                        const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                        const auto & candB = actorB.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotB)])];
                        if (candB.targetUnitId != candA.targetUnitId) continue;
                        sameTargetShooters.push_back(slotB);
                        const auto * compB = singleComponentOf(candB.source.bundle);
                        const std::int32_t dmgB = (compB && compB->kind == ComponentKind::Fire)
                            ? compB->fire.damageSteps : 0;
                        if (dmgB < weakestDmg)
                        {
                            weakestDmg = dmgB;
                            weakestShooterSlot = slotB;
                        }
                    }
                    if (weakestShooterSlot < 0) continue;
                    // Skip if a non-shooter occupies candA.destination; shooter blockers vacate with the handoff.
                    if (blocker >= 0 &&
                        std::ranges::find(sameTargetShooters, blocker) == sameTargetShooters.end())
                        continue;

                    const auto tryReleaseSet = [&](std::span<const int> releaseSlots) -> bool {
                        std::vector<int> trial = out.bestTeam;
                        trial[static_cast<std::size_t>(slotA)] = optA;
                        for (const int s : releaseSlots)
                        {
                            const auto & actorB = problem.actors()[static_cast<std::size_t>(s)];
                            const int waitB = findInPlaceHoldOrCaptureCandidate(actorB);
                            if (waitB == NO_CANDIDATE)
                                return false;
                            trial[static_cast<std::size_t>(s)] = waitB;
                        }
                        for (const int s : releaseSlots)
                        {
                            const auto & actorB = problem.actors()[static_cast<std::size_t>(s)];
                            const TilePoint dB = actorB.candidates[static_cast<std::size_t>(trial[static_cast<std::size_t>(s)])].destination;
                            if (openRefinementDestinationTaken(problem, trial, s, dB))
                                return false;
                        }
                        if (openRefinementDestinationTaken(problem, trial, slotA, candA.destination))
                            return false;
                        if (!allowedByPrior(trial))
                            return false;
                        ReplyPassEvaluation econEval;
                        const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                        if (score.strictlyBetterThan(out.bestScore))
                        {
                            out.bestTeam = std::move(trial);
                            out.bestScore = score;
                            bestEconEval = std::move(econEval);
                            ++out.pairImprovements;
                            for (const int s : releaseSlots)
                            {
                                if (std::ranges::find(releasedByHandoff, s) == releasedByHandoff.end())
                                    releasedByHandoff.push_back(s);
                            }
                            return true;
                        }
                        return false;
                    };

                    bool handedOff = false;
                    if (sameTargetShooters.size() >= 2 && evaluator.evalCount() < maxEvals && work >= replayTariff)
                        handedOff = tryReleaseSet(sameTargetShooters);
                    if (!handedOff && evaluator.evalCount() < maxEvals && work >= replayTariff)
                    {
                        const std::array<int, 1> single{blocker >= 0 ? blocker : weakestShooterSlot};
                        handedOff = tryReleaseSet(single);
                    }
                    if (handedOff)
                        break;
                }
            }
        }

        // Re-optimize any unit released to WAIT by a fire-handoff so it picks its best remaining attack or move.
        for (const int slot : releasedByHandoff)
        {
            if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
            const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
            const int currentOpt = out.bestTeam[static_cast<std::size_t>(slot)];
            const std::vector<int> candidates = shortlistForActor(member, slot, currentOpt, fullFireCap, fullPosCap);
            std::vector<int> trial = out.bestTeam;
            int chosenOpt = currentOpt;
            for (const int opt : candidates)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                trial[static_cast<std::size_t>(slot)] = opt;
                if (!allowedByPrior(trial)) continue;
                ReplyPassEvaluation econEval;
                const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                if (score.strictlyBetterThan(out.bestScore))
                {
                    out.bestScore = score;
                    chosenOpt = opt;
                    bestEconEval = std::move(econEval);
                }
            }
            if (chosenOpt != currentOpt)
            {
                out.bestTeam[static_cast<std::size_t>(slot)] = chosenOpt;
                ++out.pairImprovements;
            }
        }

        // Linear K-unit focus-fire wave pass: evaluates cumulative k=2..m attacker waves per enemy target (no cap of 2).
        if (evaluator.evalCount() < maxEvals && work >= replayTariff)
        {
            struct WaveAttacker
            {
                int slot{0};
                int opt{NO_CANDIDATE};
                TilePoint dest{INVALID_TILE};
                MilliFunds counterCap{0};
                std::int32_t dmgSteps{0};
                bool diesToCounter{false};
            };
            std::map<int, std::vector<WaveAttacker>> attackersByTarget;
            for (const int slot : order)
            {
                const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                const int ki = member.knowledgeUnitIndex;
                const bool hasKnown = ki >= 0 && ki < static_cast<int>(knowledge.units().size());
                const bool indirect = hasKnown && knowledge.units()[static_cast<std::size_t>(ki)].minRange > 1;
                const int actorHp = hasKnown ? knowledge.units()[static_cast<std::size_t>(ki)].hpRounded : UNIT_HP_STEPS;
                const MilliFunds actorCost = ki >= 0 && ki < static_cast<int>(local.units.size())
                    ? local.units[static_cast<std::size_t>(ki)].replacementCost : 0;
                std::map<int, WaveAttacker> bestOptForTarget;
                for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (!c.plannable || c.targetUnitId == NO_UNIT) continue;
                    const auto * comp = singleComponentOf(c.source.bundle);
                    if (!comp || comp->kind != ComponentKind::Fire) continue;
                    const MilliFunds cCap = (!indirect && comp->fire.counterSteps > 0)
                        ? bookDamage(actorCost > 0 ? actorCost : c.source.actor.replacementCost, comp->fire.counterSteps)
                        : 0;
                    const bool dies = !indirect && actorHp > 0 && comp->fire.counterSteps >= actorHp;
                    const WaveAttacker cand{slot, opt, c.destination, cCap, comp->fire.damageSteps, dies};
                    const bool candOffFactory =
                        std::ranges::find(friendlyFactories, c.destination) == friendlyFactories.end();
                    auto it = bestOptForTarget.find(c.targetUnitId);
                    if (it == bestOptForTarget.end() ||
                        std::tuple{candOffFactory, isSafeCand(c), cand.dmgSteps, -cand.counterCap} >
                            std::tuple{std::ranges::find(friendlyFactories, it->second.dest) == friendlyFactories.end(),
                                       isSafeCand(member.candidates[static_cast<std::size_t>(it->second.opt)]),
                                       it->second.dmgSteps, -it->second.counterCap})
                    {
                        bestOptForTarget[c.targetUnitId] = cand;
                    }
                }
                for (const auto & [tgtId, att] : bestOptForTarget)
                    attackersByTarget[tgtId].push_back(att);
            }
            struct TargetPriority
            {
                int targetUnitId{NO_UNIT};
                bool capturingOurHq{false};
                MilliFunds stake{0};
                std::int32_t hpSteps{UNIT_HP_STEPS};
            };
            const int waveHorizon = std::max(1, field.horizonTurns());
            std::vector<TargetPriority> targets;
            for (const auto & [tgtId, list] : attackersByTarget)
            {
                if (list.size() < 2) continue;
                bool capturingOurHq = false;
                MilliFunds stake = 0;
                std::int32_t hp = UNIT_HP_STEPS;
                if (isTerrainTargetId(tgtId))
                {
                    stake = evaluator.breakableTerrainStake(tgtId);
                    hp = evaluator.breakableTerrainHpSteps(tgtId);
                    if (stake <= 0) continue;
                }
                else
                {
                    for (std::size_t ki = 0; ki < local.units.size() && ki < knowledge.units().size(); ++ki)
                    {
                        if (local.units[ki].engineUnitId == tgtId)
                        {
                            stake = local.units[ki].replacementCost;
                            hp = knowledge.units()[ki].hpRounded;
                            if (knowledge.units()[ki].capturePoints > 0)
                            {
                                for (const auto & prop : properties)
                                {
                                    if (prop.capturable && prop.capturerIndex == static_cast<int>(ki) && prop.capturePoints > 0)
                                    {
                                        const Relation ownerRel = knowledge.relation(prop.ownerId);
                                        if (prop.objective == ObjectiveKind::Headquarters &&
                                            (prop.ownerId == knowledge.observerId() || ownerRel == Relation::Allied))
                                            capturingOurHq = true;
                                        const int colSlot = field.columnSlotAt({prop.x, prop.y});
                                        if (colSlot != NO_STOCK_COLUMN)
                                        {
                                            const auto col = mirroredColumn(field.enemyColumn(colSlot));
                                            const MilliFunds swing = incomeSwing(
                                                col.income, waveHorizon, col.ownerBefore, OwnerSign::Enemy);
                                            RelativeStockDetail::checkedAdd(stake, swing < 0 ? -swing : swing);
                                        }
                                        break;
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                targets.push_back({tgtId, capturingOurHq, stake, hp});
            }
            std::stable_sort(targets.begin(), targets.end(), [](const TargetPriority & a, const TargetPriority & b) {
                return std::tuple{a.capturingOurHq, a.stake, -a.targetUnitId} >
                       std::tuple{b.capturingOurHq, b.stake, -b.targetUnitId};
            });
            for (auto & [tgtId, list] : attackersByTarget)
            {
                std::stable_sort(list.begin(), list.end(), [](const WaveAttacker & a, const WaveAttacker & b) {
                    const bool aDraws = a.counterCap > 0;
                    const bool bDraws = b.counterCap > 0;
                    if (aDraws != bDraws) return !aDraws;
                    if (a.diesToCounter != b.diesToCounter) return !a.diesToCounter;
                    if (aDraws && bDraws)
                    {
                        const std::int64_t aEff = static_cast<std::int64_t>(a.dmgSteps) * b.counterCap;
                        const std::int64_t bEff = static_cast<std::int64_t>(b.dmgSteps) * a.counterCap;
                        if (aEff != bEff) return aEff > bEff;
                    }
                    if (a.dmgSteps != b.dmgSteps) return a.dmgSteps > b.dmgSteps;
                    if (a.counterCap != b.counterCap) return a.counterCap < b.counterCap;
                    return a.slot < b.slot;
                });
            }
            // Best-first wave rounds: every target competes head-to-head against the same clean baseline by UnifiedTeamScore.
            while (evaluator.evalCount() < maxEvals && work >= replayTariff)
            {
                const std::vector<int> baseTeam = out.bestTeam;
                UnifiedTeamScore roundBestScore = out.bestScore;
                std::vector<int> roundBestTeam;
                ReplyPassEvaluation roundBestEconEval;
                bool roundImproved = false;
                for (const auto & tp : targets)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    const auto & list = attackersByTarget[tp.targetUnitId];
                    std::vector<int> trial = baseTeam;
                    std::vector<int> waveSlots;
                    std::int32_t cumulativeDmg = 0;
                    bool targetAlreadyFocused = true;
                    for (const auto & att : list)
                    {
                        const auto & member = problem.actors()[static_cast<std::size_t>(att.slot)];
                        int pickedOpt = NO_CANDIDATE;
                        const auto destFree = [&](TilePoint d) {
                            for (const auto & other : problem.actors())
                            {
                                const int s = other.canonicalSlot;
                                if (s == att.slot) continue;
                                if (other.candidates[static_cast<std::size_t>(trial[static_cast<std::size_t>(s)])].destination == d)
                                    return false;
                            }
                            return true;
                        };
                        if (destFree(att.dest))
                            pickedOpt = att.opt;
                        else
                        {
                            for (int pass = 0; pass < 2 && pickedOpt == NO_CANDIDATE; ++pass)
                            {
                                for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                                {
                                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                                    if (!c.plannable || c.targetUnitId != tp.targetUnitId || !destFree(c.destination))
                                        continue;
                                    const bool onFac =
                                        std::ranges::find(friendlyFactories, c.destination) != friendlyFactories.end();
                                    if (pass == 0 && onFac) continue;
                                    pickedOpt = opt;
                                    break;
                                }
                            }
                        }
                        if (pickedOpt == NO_CANDIDATE) continue;
                        if (trial[static_cast<std::size_t>(att.slot)] != pickedOpt)
                            targetAlreadyFocused = false;
                        trial[static_cast<std::size_t>(att.slot)] = pickedOpt;
                        waveSlots.push_back(att.slot);
                        cumulativeDmg += att.dmgSteps;
                        if (waveSlots.size() >= 2 && !targetAlreadyFocused)
                        {
                            if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                            if (allowedByPrior(trial))
                            {
                                ReplyPassEvaluation econEval;
                                const auto score = evaluator.evaluate(trial, &roundBestScore, &econEval);
                                if (score.strictlyBetterThan(roundBestScore))
                                {
                                    roundBestScore = score;
                                    roundBestTeam = trial;
                                    roundBestEconEval = std::move(econEval);
                                    roundImproved = true;
                                }
                            }
                        }
                        if (cumulativeDmg >= tp.hpSteps)
                            break;
                    }
                }
                if (!roundImproved)
                    break;
                out.bestTeam = std::move(roundBestTeam);
                out.bestScore = roundBestScore;
                bestEconEval = std::move(roundBestEconEval);
                ++out.pairImprovements;
            }
        }

        // Post-kill breakthrough pass: test blocked movers through enemies killed this turn (or with joint killers).
        if (evaluator.evalCount() < maxEvals && work >= replayTariff)
        {
            const auto computeDmgByTarget = [&](std::span<const int> team) {
                std::map<std::int32_t, std::int32_t> dmg;
                for (const auto & member : problem.actors())
                {
                    const int s = member.canonicalSlot;
                    const int oOpt = team[static_cast<std::size_t>(s)];
                    if (oOpt < 0 || oOpt >= static_cast<int>(member.candidates.size())) continue;
                    const auto & oc = member.candidates[static_cast<std::size_t>(oOpt)];
                    if (oc.targetUnitId == NO_UNIT) continue;
                    if (const auto * comp = singleComponentOf(oc.source.bundle);
                        comp && comp->kind == ComponentKind::Fire)
                        dmg[oc.targetUnitId] += comp->fire.damageSteps;
                }
                return dmg;
            };
            auto dmgByTarget = computeDmgByTarget(out.bestTeam);
            // Case A: movers whose required kills are already satisfied by out.bestTeam.
            for (const int slot : order)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                std::vector<int> readyOpts;
                for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (!c.plannable || c.requiredKilledEnemyIds.empty()) continue;
                    if (!breakthroughDepsMet(out.bestTeam, slot, c) ||
                        openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                        continue;
                    readyOpts.push_back(opt);
                }
                if (readyOpts.empty()) continue;
                std::stable_sort(readyOpts.begin(), readyOpts.end(), [&](int a, int b) {
                    const auto & ca = member.candidates[static_cast<std::size_t>(a)];
                    const auto & cb = member.candidates[static_cast<std::size_t>(b)];
                    const int pa = ca.capture.has_value() ? 2 : (ca.targetUnitId != NO_UNIT ? 1 : 0);
                    const int pb = cb.capture.has_value() ? 2 : (cb.targetUnitId != NO_UNIT ? 1 : 0);
                    if (pa != pb) return pa > pb;
                    return ca.immediateValue.economicValue > cb.immediateValue.economicValue;
                });
                const std::size_t cap = std::min(readyOpts.size(), UNIFIED_MAX_BREAKTHROUGH_TRIALS_PER_ACTOR);
                for (std::size_t i = 0; i < cap; ++i)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    std::vector<int> trial = out.bestTeam;
                    trial[static_cast<std::size_t>(slot)] = readyOpts[i];
                    if (!allowedByPrior(trial)) continue;
                    ReplyPassEvaluation econEval;
                    const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                    if (score.strictlyBetterThan(out.bestScore))
                    {
                        out.bestTeam = std::move(trial);
                        out.bestScore = score;
                        bestEconEval = std::move(econEval);
                        ++out.pairImprovements;
                        dmgByTarget = computeDmgByTarget(out.bestTeam);
                        break;
                    }
                }
            }
            // Case B: pair unkilled blocking enemies with available killers to unlock single-target breakthroughs.
            std::vector<std::int32_t> pendingTargets;
            for (const int slot : order)
            {
                const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                for (const auto & c : member.candidates)
                {
                    if (!c.plannable || c.requiredKilledEnemyIds.size() != 1) continue;
                    const std::int32_t reqId = c.requiredKilledEnemyIds.front();
                    if (dmgByTarget[reqId] >= targetHpStepsFor(reqId)) continue;
                    if (std::ranges::find(pendingTargets, reqId) == pendingTargets.end())
                    {
                        pendingTargets.push_back(reqId);
                        if (pendingTargets.size() >= UNIFIED_MAX_JOINT_BREAKTHROUGH_TARGETS) break;
                    }
                }
                if (pendingTargets.size() >= UNIFIED_MAX_JOINT_BREAKTHROUGH_TARGETS) break;
            }
            for (const std::int32_t reqId : pendingTargets)
            {
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const std::int32_t reqHp = targetHpStepsFor(reqId);
                if (dmgByTarget[reqId] >= reqHp) continue;
                struct BreakMover { int slot{0}; int opt{NO_CANDIDATE}; int prio{0}; MilliFunds val{0}; };
                std::vector<BreakMover> movers;
                for (const int slot : order)
                {
                    const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                    int bestOpt = NO_CANDIDATE;
                    int bestPrio = -1;
                    MilliFunds bestVal = std::numeric_limits<MilliFunds>::min();
                    for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
                    {
                        const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                        if (!c.plannable || c.requiredKilledEnemyIds.size() != 1 ||
                            c.requiredKilledEnemyIds.front() != reqId)
                            continue;
                        if (openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                            continue;
                        const int prio = c.capture.has_value() ? 2 : (c.targetUnitId != NO_UNIT ? 1 : 0);
                        if (bestOpt == NO_CANDIDATE || std::tuple{prio, c.immediateValue.economicValue} >
                            std::tuple{bestPrio, bestVal})
                        {
                            bestOpt = opt;
                            bestPrio = prio;
                            bestVal = c.immediateValue.economicValue;
                        }
                    }
                    if (bestOpt != NO_CANDIDATE)
                        movers.push_back({slot, bestOpt, bestPrio, bestVal});
                }
                if (movers.empty()) continue;
                std::stable_sort(movers.begin(), movers.end(), [](const BreakMover & a, const BreakMover & b) {
                    return std::tuple{a.prio, a.val} > std::tuple{b.prio, b.val};
                });
                const std::size_t moverCap = std::min(movers.size(), UNIFIED_MAX_MOVERS_PER_BREAKTHROUGH_TARGET);
                for (std::size_t mi = 0; mi < moverCap; ++mi)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    const BreakMover & bm = movers[mi];
                    const auto & breakCand = problem.actors()[static_cast<std::size_t>(bm.slot)].candidates[static_cast<std::size_t>(bm.opt)];
                    struct KillerChoice { int slot{0}; int opt{NO_CANDIDATE}; std::int32_t dmgSteps{0}; TilePoint dest{INVALID_TILE}; };
                    std::vector<KillerChoice> killers;
                    for (const int attSlot : order)
                    {
                        if (attSlot == bm.slot) continue;
                        const auto & attMember = problem.actors()[static_cast<std::size_t>(attSlot)];
                        const int curOpt = out.bestTeam[static_cast<std::size_t>(attSlot)];
                        if (curOpt >= 0 && curOpt < static_cast<int>(attMember.candidates.size()) &&
                            attMember.candidates[static_cast<std::size_t>(curOpt)].capture.has_value())
                            continue;
                        int bestKOpt = NO_CANDIDATE;
                        std::int32_t bestDmg = 0;
                        TilePoint bestDest = INVALID_TILE;
                        for (int opt = 0; opt < static_cast<int>(attMember.candidates.size()); ++opt)
                        {
                            const auto & c = attMember.candidates[static_cast<std::size_t>(opt)];
                            if (!c.plannable || c.targetUnitId != reqId || !c.requiredKilledEnemyIds.empty())
                                continue;
                            if (c.destination == breakCand.destination ||
                                openRefinementDestinationTaken(problem, out.bestTeam, attSlot, c.destination))
                                continue;
                            const auto * comp = singleComponentOf(c.source.bundle);
                            if (!comp || comp->kind != ComponentKind::Fire || comp->fire.damageSteps <= 0)
                                continue;
                            if (comp->fire.damageSteps > bestDmg)
                            {
                                bestDmg = comp->fire.damageSteps;
                                bestKOpt = opt;
                                bestDest = c.destination;
                            }
                        }
                        if (bestKOpt != NO_CANDIDATE)
                            killers.push_back({attSlot, bestKOpt, bestDmg, bestDest});
                    }
                    if (killers.empty()) continue;
                    std::stable_sort(killers.begin(), killers.end(), [](const KillerChoice & a, const KillerChoice & b) {
                        return a.dmgSteps > b.dmgSteps;
                    });
                    std::vector<int> trial = out.bestTeam;
                    trial[static_cast<std::size_t>(bm.slot)] = bm.opt;
                    std::int32_t totalDmg = 0;
                    for (const auto & other : problem.actors())
                    {
                        const int s = other.canonicalSlot;
                        if (s == bm.slot) continue;
                        const auto & oc = other.candidates[static_cast<std::size_t>(trial[static_cast<std::size_t>(s)])];
                        if (oc.targetUnitId != reqId) continue;
                        if (const auto * comp = singleComponentOf(oc.source.bundle);
                            comp && comp->kind == ComponentKind::Fire)
                            totalDmg += comp->fire.damageSteps;
                    }
                    std::size_t added = 0;
                    for (const KillerChoice & kc : killers)
                    {
                        if (totalDmg >= reqHp || added >= UNIFIED_MAX_ADDITIONAL_WALLBREAK_KILLERS) break;
                        if (trial[static_cast<std::size_t>(kc.slot)] == kc.opt) continue;
                        bool collides = false;
                        for (const auto & other : problem.actors())
                        {
                            const int s = other.canonicalSlot;
                            if (s == kc.slot) continue;
                            if (other.candidates[static_cast<std::size_t>(trial[static_cast<std::size_t>(s)])].destination == kc.dest)
                            {
                                collides = true;
                                break;
                            }
                        }
                        if (collides) continue;
                        std::int32_t prevOnReq = 0;
                        const auto & prevC = problem.actors()[static_cast<std::size_t>(kc.slot)]
                            .candidates[static_cast<std::size_t>(trial[static_cast<std::size_t>(kc.slot)])];
                        if (prevC.targetUnitId == reqId)
                        {
                            if (const auto * comp = singleComponentOf(prevC.source.bundle);
                                comp && comp->kind == ComponentKind::Fire)
                                prevOnReq = comp->fire.damageSteps;
                        }
                        if (kc.dmgSteps <= prevOnReq) continue;
                        trial[static_cast<std::size_t>(kc.slot)] = kc.opt;
                        totalDmg += (kc.dmgSteps - prevOnReq);
                        ++added;
                    }
                    if (totalDmg < reqHp || !allowedByPrior(trial)) continue;
                    ReplyPassEvaluation econEval;
                    const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                    if (score.strictlyBetterThan(out.bestScore))
                    {
                        out.bestTeam = std::move(trial);
                        out.bestScore = score;
                        bestEconEval = std::move(econEval);
                        ++out.pairImprovements;
                        dmgByTarget = computeDmgByTarget(out.bestTeam);
                        break;
                    }
                }
            }
        }

        // Evaluate in-place captures strictly under the Section 13 ranking key.
        for (const int slot : order)
        {
            if (work < replayTariff) break;
            const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
            if (member.stockRow == NO_STOCK_ROW) continue;
            const int curOpt = out.bestTeam[static_cast<std::size_t>(slot)];
            const auto & curCand = member.candidates[static_cast<std::size_t>(curOpt)];
            if (curCand.capture.has_value() || curCand.targetUnitId != NO_UNIT) continue;
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || !c.capture.has_value() || c.destination != curCand.destination) continue;
                std::vector<int> trial = out.bestTeam;
                trial[static_cast<std::size_t>(slot)] = opt;
                if (!allowedByPrior(trial)) break;
                ReplyPassEvaluation econEval;
                const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
                if (score.strictlyBetterThan(out.bestScore))
                {
                    out.bestTeam = std::move(trial);
                    out.bestScore = score;
                    bestEconEval = std::move(econEval);
                }
                break;
            }
        }

        // Evaluate same-tile zero-counter shots strictly under the Section 13 ranking key.
        for (const int slot : order)
        {
            if (work < replayTariff) break;
            const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
            const int curOpt = out.bestTeam[static_cast<std::size_t>(slot)];
            const auto & curCand = member.candidates[static_cast<std::size_t>(curOpt)];
            if (curCand.targetUnitId != NO_UNIT || curCand.capture.has_value()) continue;
            const int actorKi = member.knowledgeUnitIndex;
            const bool indirect = actorKi >= 0 && actorKi < static_cast<int>(knowledge.units().size()) &&
                knowledge.units()[static_cast<std::size_t>(actorKi)].minRange > 1;
            int bestFreeOpt = NO_CANDIDATE;
            std::int32_t bestFreeDmg = 0;
            for (int opt = 0; opt < static_cast<int>(member.candidates.size()); ++opt)
            {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || c.destination != curCand.destination || c.targetUnitId == NO_UNIT) continue;
                const auto * comp = singleComponentOf(c.source.bundle);
                if (!comp || comp->kind != ComponentKind::Fire || comp->fire.damageSteps <= 0) continue;
                if (!indirect && comp->fire.counterSteps > 0) continue;
                if (!isTerrainTargetId(c.targetUnitId))
                {
                    int targetKi = NO_UNIT;
                    for (int ei = 0; ei < static_cast<int>(local.units.size()); ++ei)
                    {
                        if (local.units[static_cast<std::size_t>(ei)].engineUnitId == c.targetUnitId)
                        {
                            targetKi = ei;
                            break;
                        }
                    }
                    if (targetKi != NO_UNIT)
                    {
                        if (const auto it = bestEconEval.enemyAfterOwn.find(targetKi);
                            it != bestEconEval.enemyAfterOwn.end() && it->second <= 0.0)
                            continue;
                    }
                }
                if (bestFreeOpt == NO_CANDIDATE || comp->fire.damageSteps > bestFreeDmg)
                {
                    bestFreeOpt = opt;
                    bestFreeDmg = comp->fire.damageSteps;
                }
            }
            if (bestFreeOpt == NO_CANDIDATE) continue;
            std::vector<int> trial = out.bestTeam;
            trial[static_cast<std::size_t>(slot)] = bestFreeOpt;
            if (!allowedByPrior(trial)) continue;
            ReplyPassEvaluation econEval;
            const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval);
            if (score.strictlyBetterThan(out.bestScore))
            {
                out.bestTeam = std::move(trial);
                out.bestScore = score;
                bestEconEval = std::move(econEval);
            }
        }

        // Same-target firing-tile swap pass: let focus-fire partners trade tiles (e.g. put first shooter on a city).
        if (work >= replayTariff)
        {
            std::map<int, std::vector<int>> shootersByTarget;
            for (const int slot : order)
            {
                const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
                const int curOpt = out.bestTeam[static_cast<std::size_t>(slot)];
                const auto & curCand = member.candidates[static_cast<std::size_t>(curOpt)];
                if (curCand.targetUnitId != NO_UNIT)
                    shootersByTarget[curCand.targetUnitId].push_back(slot);
            }
            for (const auto & [tgtId, shooterSlots] : shootersByTarget)
            {
                if (shooterSlots.size() < 2) continue;
                for (std::size_t i = 0; i < shooterSlots.size() && work >= replayTariff; ++i)
                {
                    for (std::size_t j = i + 1; j < shooterSlots.size() && work >= replayTariff; ++j)
                    {
                        const int slotA = shooterSlots[i];
                        const int slotB = shooterSlots[j];
                        const auto & actorA = problem.actors()[static_cast<std::size_t>(slotA)];
                        const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                        const TilePoint destA = actorA.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotA)])].destination;
                        const TilePoint destB = actorB.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotB)])].destination;
                        if (destA == destB) continue;
                        int swapOptA = NO_CANDIDATE;
                        int swapOptB = NO_CANDIDATE;
                        for (int opt = 0; opt < static_cast<int>(actorA.candidates.size()); ++opt)
                        {
                            const auto & c = actorA.candidates[static_cast<std::size_t>(opt)];
                            if (c.plannable && c.targetUnitId == tgtId && c.destination == destB)
                            {
                                swapOptA = opt;
                                break;
                            }
                        }
                        for (int opt = 0; opt < static_cast<int>(actorB.candidates.size()); ++opt)
                        {
                            const auto & c = actorB.candidates[static_cast<std::size_t>(opt)];
                            if (c.plannable && c.targetUnitId == tgtId && c.destination == destA)
                            {
                                swapOptB = opt;
                                break;
                            }
                        }
                        if (swapOptA == NO_CANDIDATE || swapOptB == NO_CANDIDATE) continue;
                        std::vector<int> trial = out.bestTeam;
                        trial[static_cast<std::size_t>(slotA)] = swapOptA;
                        trial[static_cast<std::size_t>(slotB)] = swapOptB;
                        if (!allowedByPrior(trial)) continue;
                        const std::vector<CombatFireOrder> trialDefOrders = evaluator.buildDefaultFireOrders(trial);
                        const std::vector<CombatFireOrder> altOrders{
                            CombatFireOrder{actorA.engineUnitId, actorB.engineUnitId}};
                        const std::array<std::vector<CombatFireOrder>, 2> orderChoices{trialDefOrders, altOrders};
                        for (const std::size_t k : {std::size_t{0}, std::size_t{1}})
                        {
                            if (k == 1 && orderChoices[1] == orderChoices[0]) break;
                            if (work < replayTariff) break;
                            ReplyPassEvaluation econEval;
                            const auto score = evaluator.evaluate(trial, &out.bestScore, &econEval, orderChoices[k]);
                            if (score.strictlyBetterThan(out.bestScore))
                            {
                                out.bestTeam = trial;
                                out.bestScore = score;
                                bestEconEval = std::move(econEval);
                                ++out.pairImprovements;
                            }
                        }
                    }
                }
            }
        }

        // Value-kept multi-attacker fire-order pass: test every focus-fire sequence under the Section 13 ranking key.
        out.bestFireOrders = evaluator.buildDefaultFireOrders(out.bestTeam);
        if (!out.bestFireOrders.empty() && work >= replayTariff)
        {
            for (std::size_t i = 0; i < out.bestFireOrders.size(); ++i)
            {
                if (work < replayTariff) break;
                std::vector<CombatFireOrder> trialOrders = out.bestFireOrders;
                std::swap(trialOrders[i].first, trialOrders[i].second);
                ReplyPassEvaluation econEval;
                const auto score = evaluator.evaluate(out.bestTeam, &out.bestScore, &econEval, trialOrders);
                if (score.strictlyBetterThan(out.bestScore))
                {
                    out.bestFireOrders = std::move(trialOrders);
                    out.bestScore = score;
                    bestEconEval = std::move(econEval);
                }
            }
        }

        out.evals = evaluator.evalCount();
        out.pruned = evaluator.prunedCount();
        const bool teamOrOrderChanged = out.bestTeam != std::vector<int>(startingTeam.begin(), startingTeam.end()) ||
                                        !out.bestFireOrders.empty();
        out.improved = teamOrOrderChanged && out.bestScore.strictlyBetterThan(out.baselineScore);
        return out;
    }
}
