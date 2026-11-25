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
    inline constexpr int UNIFIED_SEARCH_MAX_SWEEPS = 3;
    inline constexpr int UNIFIED_SEARCH_MAX_EVALS = 1800;
    inline constexpr int UNIFIED_EVALS_PER_ACTOR = 60;
    inline constexpr int UNIFIED_PAIR_PASS_EVAL_RESERVE = 300;
    inline constexpr std::int64_t UNIFIED_PAIR_PASS_WORK_RESERVE = 40'000'000;
    inline constexpr std::size_t UNIFIED_MAX_FIRE_CANDIDATES = 8;
    inline constexpr std::size_t UNIFIED_MAX_POSITIONAL_CANDIDATES = 8;
    inline constexpr std::size_t UNIFIED_TIGHT_FIRE_CANDIDATES = 4;
    inline constexpr std::size_t UNIFIED_TIGHT_POSITIONAL_CANDIDATES = 3;
    inline constexpr std::size_t UNIFIED_CARDINAL_DIRECTIONS = 4;
    inline constexpr int UNIFIED_TIGHT_EVALS_PER_REMAINING = 14;
    inline constexpr std::size_t UNIFIED_VACATE_PARTNER_LIMIT = 6;
    inline constexpr std::size_t UNIFIED_SCREEN_CANDIDATE_LIMIT = 10;

    struct UnifiedTeamSearchResult
    {
        std::vector<int> bestTeam;
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
            if (prop.visible && prop.canProduce && prop.ownerId == knowledge.observerId())
                friendlyFactories.push_back({prop.x, prop.y});
        }
        const auto isSafeCandidate = [](const MasterCandidate & c) {
            return !c.source.replyFacts || !c.source.replyFacts->tile ||
                c.source.replyFacts->tile->shooters.empty();
        };
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (!unifiedCanChangeSlot(problem, member) || member.stockRow == NO_STOCK_ROW) continue;
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

    template <class HqRankFn, class BuildSlotsFn, class AllowedFn>
    inline UnifiedTeamSearchResult optimizeUnifiedTeam(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const PropertyStockField & field, std::span<const PropertyFacts> properties,
        CombatTerrain & local, const ReplyBoard & actingBoard,
        std::span<const int> startingTeam, std::span<const int> stockProposed,
        std::int64_t replayTariff, std::int64_t & work,
        HqRankFn hqRankFn, BuildSlotsFn buildSlotsFn, AllowedFn allowedByPrior)
    {
        UnifiedTeamSearchResult out;
        out.bestTeam.assign(startingTeam.begin(), startingTeam.end());
        if (out.bestTeam.size() != problem.actors().size()) return out;

        UnifiedTeamEvaluator evaluator(problem, knowledge, field, properties, local, actingBoard,
                                       replayTariff, work, std::move(hqRankFn), std::move(buildSlotsFn));

        const std::int64_t workBeforeSeeds = work;
        ReplyPassEvaluation bestEconEval;
        out.baselineScore = evaluator.evaluate(out.bestTeam, nullptr, &bestEconEval);
        out.bestScore = out.baselineScore;
        out.winningSeed = QStringLiteral("STARTING_TEAM");

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
        trySeed(buildCleanStockSeed(problem, startingTeam, stockProposed), "CLEAN_STOCK_AND_WAIT");
        trySeed(buildAllWaitSeed(problem, startingTeam), "ALL_WAIT");
        if (!out.bestScore.valid) return out;

        std::vector<TilePoint> friendlyFactories;
        std::vector<TilePoint> unownedProps;
        std::vector<TilePoint> unownedFactories;
        std::vector<TilePoint> friendlyRepairSites;
        for (const auto & prop : properties)
        {
            if (prop.visible && prop.canProduce && prop.ownerId == knowledge.observerId())
                friendlyFactories.push_back({prop.x, prop.y});
            if (prop.visible && prop.capturable && prop.ownerId != knowledge.observerId())
            {
                unownedProps.push_back({prop.x, prop.y});
                if (prop.canProduce)
                    unownedFactories.push_back({prop.x, prop.y});
            }
            if (prop.visible && prop.ownerId == knowledge.observerId() && !prop.canProduce)
                friendlyRepairSites.push_back({prop.x, prop.y});
        }
        std::vector<TilePoint> enemyPositions;
        for (const auto & pawn : local.units)
        {
            if (pawn.present && pawn.enemy && pawn.tile != INVALID_TILE)
                enemyPositions.push_back(pawn.tile);
        }

        // Order slots: factory blockers & active/damaged cappers, exposed combat, other cappers, safe combat.
        std::vector<int> order;
        order.reserve(problem.actors().size());
        for (const auto & member : problem.actors())
            if (unifiedCanChangeSlot(problem, member)) order.push_back(member.canonicalSlot);
        const auto slotPriority = [&](int slot) {
            const auto & member = problem.actors()[static_cast<std::size_t>(slot)];
            const int curOpt = out.bestTeam[static_cast<std::size_t>(slot)];
            const bool onFactory = curOpt >= 0 && curOpt < static_cast<int>(member.candidates.size()) &&
                std::ranges::find(friendlyFactories, member.candidates[static_cast<std::size_t>(curOpt)].origin) !=
                    friendlyFactories.end();
            if (onFactory) return 0;
            if (member.stockRow != NO_STOCK_ROW)
            {
                const bool hasKnown = member.knowledgeUnitIndex >= 0 &&
                    member.knowledgeUnitIndex < static_cast<int>(knowledge.units().size());
                const bool hasProgress = hasKnown &&
                    knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)].capturePoints > 0;
                if (hasProgress) return 0;
                return 2;
            }
            const bool exposedNow = curOpt >= 0 && curOpt < static_cast<int>(member.candidates.size()) &&
                member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts &&
                member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts->tile &&
                !member.candidates[static_cast<std::size_t>(curOpt)].source.replyFacts->tile->shooters.empty();
            return exposedNow ? 1 : UNIFIED_SEARCH_MAX_SWEEPS;
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return slotPriority(a) < slotPriority(b); });

        // Scale eval budget with actor count and shortlist high-signal candidates for 50v50 scaling.
        const int actorCount = static_cast<int>(order.size());
        const int maxEvals = std::max(UNIFIED_SEARCH_MAX_EVALS, actorCount * UNIFIED_EVALS_PER_ACTOR);
        const std::int64_t seedEvals = std::max(1, evaluator.evalCount());
        const std::int64_t perEvalWork = std::max<std::int64_t>(replayTariff, (workBeforeSeeds - work) / seedEvals);
        const std::int64_t pairWorkReserve = std::min(work / 4, perEvalWork * UNIFIED_PAIR_PASS_EVAL_RESERVE);
        const int sweepEvalCap = std::max(1, maxEvals - UNIFIED_PAIR_PASS_EVAL_RESERVE);
        const std::int64_t sweepWorkFloor = replayTariff + pairWorkReserve;

        const auto isSafeCand = [](const MasterCandidate & c) {
            return !c.source.replyFacts || !c.source.replyFacts->tile ||
                c.source.replyFacts->tile->shooters.empty();
        };
        const auto shortlistForActor = [&](const MasterActor & member, int slot, int currentOpt,
                                           std::size_t maxFires, std::size_t maxPositional) {
            std::vector<int> picked;
            std::vector<bool> seen(member.candidates.size(), false);
            const auto addOpt = [&](int opt) {
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size()) || opt == currentOpt || seen[static_cast<std::size_t>(opt)])
                    return;
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                if (!c.plannable || openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                    return;
                seen[static_cast<std::size_t>(opt)] = true;
                picked.push_back(opt);
            };
            // Always keep stock proposal, in-place wait, and every capture option.
            if (!stockProposed.empty() && slot < static_cast<int>(stockProposed.size()))
                addOpt(stockProposed[static_cast<std::size_t>(slot)]);
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
                if (!c.plannable || openRefinementDestinationTaken(problem, out.bestTeam, slot, c.destination))
                    continue;
                if (c.capture.has_value())
                    addOpt(opt);
                else if (c.targetUnitId != NO_UNIT)
                    fires.push_back(opt);
                else if (std::ranges::find(capturableDestinations, c.destination) == capturableDestinations.end())
                    positionals.push_back(opt);
            }
            std::stable_sort(fires.begin(), fires.end(), [&](int a, int b) {
                const auto & ca = member.candidates[static_cast<std::size_t>(a)];
                const auto & cb = member.candidates[static_cast<std::size_t>(b)];
                if (ca.immediateValue.economicValue != cb.immediateValue.economicValue)
                    return ca.immediateValue.economicValue > cb.immediateValue.economicValue;
                return isSafeCand(ca) > isSafeCand(cb);
            });
            for (std::size_t i = 0; i < fires.size() && i < maxFires; ++i)
                addOpt(fires[i]);

            const int ki = member.knowledgeUnitIndex;
            const int capPts = field.capturePointsToCapture();
            const int fullCapTurns = (capPts + UNIT_HP_STEPS - 1) / UNIT_HP_STEPS;
            const bool hasKnownCapper = member.stockRow != NO_STOCK_ROW && ki >= 0 &&
                ki < static_cast<int>(knowledge.units().size());
            const int capRate = hasKnownCapper ? knowledge.units()[static_cast<std::size_t>(ki)].captureRate : 0;
            const bool heavilyDamagedCapper = hasKnownCapper && !friendlyRepairSites.empty() &&
                (capRate <= 0 || (capPts + capRate - 1) / capRate > fullCapTurns + 1);
            const std::span<const TilePoint> goals = member.stockRow == NO_STOCK_ROW
                ? std::span<const TilePoint>(enemyPositions)
                : (heavilyDamagedCapper
                       ? std::span<const TilePoint>(friendlyRepairSites)
                       : (!unownedProps.empty() ? std::span<const TilePoint>(unownedProps)
                                                : std::span<const TilePoint>(enemyPositions)));
            const auto posRank = [&](int opt) {
                const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                const int safeRank = isSafeCand(c) ? 1 : 0;
                const int offFactory =
                    std::ranges::find(friendlyFactories, c.destination) == friendlyFactories.end() ? 1 : 0;
                int minDist = 0;
                if (member.stockRow == NO_STOCK_ROW && !enemyPositions.empty())
                {
                    minDist = evaluator.enemyPathDistance(ki, c.destination);
                }
                else if (!goals.empty())
                {
                    minDist = std::numeric_limits<int>::max();
                    for (const TilePoint g : goals)
                        minDist = std::min(minDist, manhattanDistance(c.destination, g));
                }
                const int stars = (local.defense && ki >= 0 && ki < static_cast<int>(local.units.size()))
                    ? local.defense(ki, c.destination).stars : 0;
                return std::tuple{safeRank, offFactory, -minDist, stars};
            };
            std::stable_sort(positionals.begin(), positionals.end(), [&](int a, int b) {
                return posRank(a) > posRank(b);
            });
            // Keep directional diversity (factory approach + 4 cardinal extremes) so shortlisting never blinds secondary fronts.
            if (member.stockRow != NO_STOCK_ROW && !unownedFactories.empty())
            {
                int bestFactoryOpt = NO_CANDIDATE;
                int bestFactoryDist = std::numeric_limits<int>::max();
                for (const int opt : positionals)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (!isSafeCand(c)) continue;
                    int d = std::numeric_limits<int>::max();
                    for (const TilePoint f : unownedFactories)
                        d = std::min(d, manhattanDistance(c.destination, f));
                    if (d < bestFactoryDist)
                    {
                        bestFactoryDist = d;
                        bestFactoryOpt = opt;
                    }
                }
                if (bestFactoryOpt != NO_CANDIDATE)
                    addOpt(bestFactoryOpt);
            }
            if (maxPositional >= UNIFIED_TIGHT_POSITIONAL_CANDIDATES)
            {
                std::array<int, UNIFIED_CARDINAL_DIRECTIONS> bestDirOpt{NO_CANDIDATE, NO_CANDIDATE, NO_CANDIDATE, NO_CANDIDATE};
                std::array<int, UNIFIED_CARDINAL_DIRECTIONS> bestDirScore{
                    std::numeric_limits<int>::min(), std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
                for (const int opt : positionals)
                {
                    const auto & c = member.candidates[static_cast<std::size_t>(opt)];
                    if (!isSafeCand(c)) continue;
                    const std::array<int, UNIFIED_CARDINAL_DIRECTIONS> proj{
                        c.destination.x, -c.destination.x, c.destination.y, -c.destination.y};
                    for (std::size_t dir = 0; dir < UNIFIED_CARDINAL_DIRECTIONS; ++dir)
                    {
                        if (proj[dir] > bestDirScore[dir])
                        {
                            bestDirScore[dir] = proj[dir];
                            bestDirOpt[dir] = opt;
                        }
                    }
                }
                for (const int opt : bestDirOpt)
                    addOpt(opt);
            }
            const std::size_t safeKeep = (member.stockRow == NO_STOCK_ROW && maxPositional > 1)
                ? (maxPositional - 1) : maxPositional;
            for (std::size_t i = 0; i < positionals.size() && i < safeKeep; ++i)
                addOpt(positionals[i]);
            if (maxPositional > 1 && !enemyPositions.empty())
            {
                // Keep one forward exposed tile so armor screening and infantry meat-shielding remain searchable.
                int bestForwardExposed = NO_CANDIDATE;
                int bestForwardDist = std::numeric_limits<int>::max();
                const bool allowExposedCapper = member.stockRow != NO_STOCK_ROW && !heavilyDamagedCapper;
                if (member.stockRow == NO_STOCK_ROW || allowExposedCapper)
                {
                    for (const int opt : positionals)
                    {
                        if (seen[static_cast<std::size_t>(opt)] || isSafeCand(member.candidates[static_cast<std::size_t>(opt)]))
                            continue;
                        const TilePoint dest = member.candidates[static_cast<std::size_t>(opt)].destination;
                        int d = std::numeric_limits<int>::max();
                        for (const TilePoint e : enemyPositions)
                            d = std::min(d, manhattanDistance(dest, e));
                        if (d < bestForwardDist)
                        {
                            bestForwardDist = d;
                            bestForwardExposed = opt;
                        }
                    }
                    if (bestForwardExposed != NO_CANDIDATE)
                        addOpt(bestForwardExposed);
                }
            }
            return picked;
        };

        for (int sweep = 0; sweep < UNIFIED_SEARCH_MAX_SWEEPS; ++sweep)
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
                const bool tight = remainingEvals < remainingActors * UNIFIED_TIGHT_EVALS_PER_REMAINING ||
                    work < sweepWorkFloor + perEvalWork * static_cast<std::int64_t>(remainingActors) * 4;
                const std::size_t maxFires = (sweep == 0 && remainingEvals <= 0)
                    ? 1 : (tight ? UNIFIED_TIGHT_FIRE_CANDIDATES : UNIFIED_MAX_FIRE_CANDIDATES);
                const std::size_t maxPos = (sweep == 0 && remainingEvals <= 0)
                    ? 1 : (tight ? UNIFIED_TIGHT_POSITIONAL_CANDIDATES : UNIFIED_MAX_POSITIONAL_CANDIDATES);
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
            for (int optA = 0; optA < static_cast<int>(actorA.candidates.size()); ++optA)
            {
                if (optA == out.bestTeam[static_cast<std::size_t>(slotA)]) continue;
                if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                const auto & candA = actorA.candidates[static_cast<std::size_t>(optA)];
                if (!candA.plannable || (!candA.capture && candA.targetUnitId == NO_UNIT)) continue;
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
                const auto partnerRankB = [&](int optB) {
                    const auto & cb = actorB.candidates[static_cast<std::size_t>(optB)];
                    const int safe = isSafeCand(cb) ? 1 : 0;
                    const int offFactory =
                        std::ranges::find(friendlyFactories, cb.destination) == friendlyFactories.end() ? 1 : 0;
                    return std::pair{safe, offFactory};
                };
                std::stable_sort(partnerOpts.begin(), partnerOpts.end(), [&](int a, int b) {
                    return partnerRankB(a) > partnerRankB(b);
                });
                std::size_t triedB = 0;
                bool improvedPair = false;
                for (const int optB : partnerOpts)
                {
                    if (triedB >= UNIFIED_VACATE_PARTNER_LIMIT ||
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

        // Screen/support pair pass: if a capper has an unchosen capture blocked by threat, pair with a combat unit.
        if (evaluator.evalCount() < maxEvals && work >= replayTariff)
        {
            for (const int capSlot : order)
            {
                const auto & capper = problem.actors()[static_cast<std::size_t>(capSlot)];
                if (capper.stockRow == NO_STOCK_ROW) continue;
                int desiredOpt = NO_CANDIDATE;
                if (!stockProposed.empty() && capSlot < static_cast<int>(stockProposed.size()))
                {
                    const int p = stockProposed[static_cast<std::size_t>(capSlot)];
                    if (p >= 0 && p < static_cast<int>(capper.candidates.size()) &&
                        p != out.bestTeam[static_cast<std::size_t>(capSlot)] &&
                        capper.candidates[static_cast<std::size_t>(p)].plannable &&
                        capper.candidates[static_cast<std::size_t>(p)].capture.has_value())
                        desiredOpt = p;
                }
                if (desiredOpt == NO_CANDIDATE)
                {
                    for (int opt = 0; opt < static_cast<int>(capper.candidates.size()); ++opt)
                    {
                        if (opt == out.bestTeam[static_cast<std::size_t>(capSlot)]) continue;
                        const auto & c = capper.candidates[static_cast<std::size_t>(opt)];
                        if (c.plannable && c.capture.has_value() &&
                            !openRefinementDestinationTaken(problem, out.bestTeam, capSlot, c.destination))
                        {
                            desiredOpt = opt;
                            break;
                        }
                    }
                }
                if (desiredOpt == NO_CANDIDATE) continue;
                const TilePoint capDest = capper.candidates[static_cast<std::size_t>(desiredOpt)].destination;
                if (openRefinementDestinationTaken(problem, out.bestTeam, capSlot, capDest)) continue;
                for (const int helperSlot : order)
                {
                    if (evaluator.evalCount() >= maxEvals || work < replayTariff) break;
                    const auto & helper = problem.actors()[static_cast<std::size_t>(helperSlot)];
                    if (helperSlot == capSlot || helper.stockRow != NO_STOCK_ROW) continue;
                    std::size_t tried = 0;
                    for (int optH = 0; optH < static_cast<int>(helper.candidates.size()) && tried < UNIFIED_SCREEN_CANDIDATE_LIMIT; ++optH)
                    {
                        if (optH == out.bestTeam[static_cast<std::size_t>(helperSlot)]) continue;
                        const auto & candH = helper.candidates[static_cast<std::size_t>(optH)];
                        if (!candH.plannable || candH.destination == capDest) continue;
                        if (openRefinementDestinationTaken(problem, out.bestTeam, helperSlot, candH.destination)) continue;
                        if (candH.targetUnitId == NO_UNIT && manhattanDistance(candH.destination, capDest) > 2) continue;
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
                            break;
                        }
                    }
                }
            }
        }

        // Fire-handoff pair pass: let a stronger finisher replace a shooter holding target damage capacity.
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
                    if (blocker >= 0) continue;
                    int weakestShooterSlot = -1;
                    std::int32_t weakestDmg = std::numeric_limits<std::int32_t>::max();
                    for (const int slotB : order)
                    {
                        if (slotB == slotA) continue;
                        const auto & actorB = problem.actors()[static_cast<std::size_t>(slotB)];
                        const auto & candB = actorB.candidates[static_cast<std::size_t>(out.bestTeam[static_cast<std::size_t>(slotB)])];
                        if (candB.targetUnitId != candA.targetUnitId) continue;
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
                    const auto & actorB = problem.actors()[static_cast<std::size_t>(weakestShooterSlot)];
                    const int waitB = findInPlaceWaitCandidate(actorB);
                    if (waitB == NO_CANDIDATE ||
                        openRefinementDestinationTaken(problem, out.bestTeam, weakestShooterSlot,
                                                       actorB.candidates[static_cast<std::size_t>(waitB)].destination))
                        continue;
                    std::vector<int> trial = out.bestTeam;
                    trial[static_cast<std::size_t>(slotA)] = optA;
                    trial[static_cast<std::size_t>(weakestShooterSlot)] = waitB;
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

        // Capture-over-wait dominance: never idle on an unowned capturable tile when capturing on it is legal.
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
                const auto score = evaluator.evaluate(trial, nullptr, &econEval);
                if (score.valid && score.worstHqRank >= out.bestScore.worstHqRank &&
                    score.hqCaptureStartSafe >= out.bestScore.hqCaptureStartSafe &&
                    score.worstEconomicValue >= out.bestScore.worstEconomicValue)
                {
                    out.bestTeam = std::move(trial);
                    out.bestScore = score;
                    bestEconEval = std::move(econEval);
                }
                break;
            }
        }

        out.evals = evaluator.evalCount();
        out.pruned = evaluator.prunedCount();
        out.improved = out.bestTeam != std::vector<int>(startingTeam.begin(), startingTeam.end()) &&
                       out.bestScore.strictlyBetterThan(out.baselineScore);
        return out;
    }
}
