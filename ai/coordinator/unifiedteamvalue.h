#pragma once

#include "ai/coordinator/opentacticalrefinement.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replystockcontext.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <tuple>
#include <vector>

namespace Coordinator
{
    // Temporary production placeholders until Unified Production replaces them with exact build valuation.
    inline constexpr MilliFunds UNIFIED_FACTORY_SLOT_VALUE = 1'500'000;
    inline constexpr std::int32_t UNIFIED_FACTORY_COUNTER_REACH = 6;
    inline constexpr MilliFunds UNIFIED_FACTORY_COUNTER_COST = 16'000'000;
    inline constexpr double UNIFIED_FACTORY_COUNTER_BASE_DAMAGE = 55.0;
    inline constexpr double UNIFIED_FACTORY_COUNTER_RETURN_DAMAGE = 35.0;

    // Exact game rules and search work budgets.
    inline constexpr MilliFunds UNIFIED_NEUTRAL_PROPERTY_INCOME = 1'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_BUDGET = 240'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_PER_ACTOR = 15'000'000;
    inline constexpr std::int64_t UNIFIED_FINAL_REPLAY_WORK_FLOOR = 2'000'000;
    inline constexpr std::int32_t UNIFIED_INFANTRY_BASE_MOVE = 3;
    inline constexpr std::int32_t UNIFIED_STOCK_TUNE_ROUNDS = 40;
    inline constexpr std::int32_t UNIFIED_TRAP_FOLLOWUP_TURNS = 2;

    struct UnifiedTeamScore
    {
        bool valid{false};
        std::tuple<TerminalClass, int, int, MilliFunds> worstHqRank{
            TerminalClass::ForcedLoss, std::numeric_limits<int>::min(),
            std::numeric_limits<int>::min(), std::numeric_limits<MilliFunds>::min()};
        bool hqCaptureStartSafe{false};
        MilliFunds worstEconomicValue{std::numeric_limits<MilliFunds>::min()};
        std::int32_t activeCaptures{std::numeric_limits<std::int32_t>::min()};
        std::int32_t unitsRepairing{std::numeric_limits<std::int32_t>::min()};
        std::int32_t combatUnitsInThreatRange{std::numeric_limits<std::int32_t>::min()};
        std::int64_t stagingScore{std::numeric_limits<std::int64_t>::min()};
        std::int32_t negativeClumpPairs{std::numeric_limits<std::int32_t>::min()};
        std::int64_t terrainCoverScore{std::numeric_limits<std::int64_t>::min()};
        MilliFunds realizedMaterial{0};
        MilliFunds postReplyStock{0};
        MilliFunds enemyDenial{0};
        MilliFunds ownCaptureRisk{0};
        MilliFunds factoryValue{0};
        int affordableBuildSlots{0};
        TacticalReply bindingPolicy{TacticalReply::Economic};
        int bindingTargetUnit{NO_UNIT};

        auto rankingKey() const
        {
            return std::tie(worstHqRank, hqCaptureStartSafe, worstEconomicValue,
                            activeCaptures, unitsRepairing, combatUnitsInThreatRange,
                            stagingScore, negativeClumpPairs, terrainCoverScore);
        }

        bool strictlyBetterThan(const UnifiedTeamScore & other) const
        {
            if (!valid) return false;
            if (!other.valid) return true;
            return rankingKey() > other.rankingKey();
        }

        bool notBetterThan(const UnifiedTeamScore & floor) const
        {
            if (!valid || !floor.valid) return false;
            return rankingKey() <= floor.rankingKey();
        }
    };

    inline std::vector<PlanRowAction> buildPostReplyStockActions(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const ReplyPassEvaluation & eval)
    {
        std::vector<PlanRowAction> actions;
        actions.reserve(eval.actors.size());
        for (const auto & actor : eval.actors)
        {
            const auto & member = problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            if (member.stockRow == NO_STOCK_ROW) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            const int initialSteps = knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)].hpRounded;
            const int afterSteps = std::max(0, actor.stepsAfterReply);
            const int lostSteps = std::max(0, initialSteps - afterSteps);
            actions.push_back(PlanRowAction{
                .row = member.stockRow,
                .destination = cand.destination,
                .captures = cand.capture.has_value(),
                .stepsLost = lostSteps,
                .destroyed = afterSteps <= 0,
            });
        }
        return actions;
    }

    inline int mostValuableExposedUnit(
        const MasterAssignmentProblem & problem, const CombatTerrain & local, std::span<const int> choices)
    {
        int bestUnit = NO_UNIT;
        MilliFunds bestCost = -1;
        for (const auto & member : problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
            const int opt = choices[static_cast<std::size_t>(slot)];
            if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
            if (!cand.source.replyFacts || !cand.source.replyFacts->tile ||
                cand.source.replyFacts->tile->shooters.empty())
                continue;
            const int ki = member.knowledgeUnitIndex;
            if (ki < 0 || ki >= static_cast<int>(local.units.size())) continue;
            const MilliFunds cost = local.units[static_cast<std::size_t>(ki)].replacementCost;
            if (cost > bestCost || (cost == bestCost && (bestUnit == NO_UNIT || member.engineUnitId < bestUnit)))
            {
                bestCost = cost;
                bestUnit = member.engineUnitId;
            }
        }
        return bestUnit;
    }

    inline int mostValuableExposedCapper(
        const MasterAssignmentProblem & problem, const CombatTerrain & local, std::span<const int> choices)
    {
        int bestUnit = NO_UNIT;
        bool bestCapturing = false;
        MilliFunds bestCost = -1;
        for (const auto & member : problem.actors())
        {
            if (member.stockRow == NO_STOCK_ROW) continue;
            const int slot = member.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
            const int opt = choices[static_cast<std::size_t>(slot)];
            if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
            if (!cand.source.replyFacts || !cand.source.replyFacts->tile ||
                cand.source.replyFacts->tile->shooters.empty())
                continue;
            const int ki = member.knowledgeUnitIndex;
            if (ki < 0 || ki >= static_cast<int>(local.units.size())) continue;
            const bool capturing = cand.capture.has_value();
            const MilliFunds cost = local.units[static_cast<std::size_t>(ki)].replacementCost;
            const bool better = bestUnit == NO_UNIT ||
                (capturing && !bestCapturing) ||
                (capturing == bestCapturing && (cost > bestCost || (cost == bestCost && member.engineUnitId < bestUnit)));
            if (better)
            {
                bestCapturing = capturing;
                bestCost = cost;
                bestUnit = member.engineUnitId;
            }
        }
        return bestUnit;
    }

    template <class HqRankFn, class BuildSlotsFn>
    class UnifiedTeamEvaluator
    {
    public:
        UnifiedTeamEvaluator(const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
                             const PropertyStockField & field, std::span<const PropertyFacts> properties,
                             CombatTerrain & local, const ReplyBoard & actingBoard, std::int64_t replayTariff,
                             std::int64_t & work, HqRankFn hqRankFn, BuildSlotsFn buildSlotsFn)
            : m_problem(problem),
              m_knowledge(knowledge),
              m_stockField(field),
              m_stockContext(field),
              m_properties(properties),
              m_local(local),
              m_actingBoard(actingBoard),
              m_replayTariff(replayTariff),
              m_work(work),
              m_hqRankFn(std::move(hqRankFn)),
              m_buildSlotsFn(std::move(buildSlotsFn)),
              m_maxDist(std::max(1, local.width + local.height))
        {
            for (std::size_t i = 0; i < local.units.size(); ++i)
            {
                const auto & pawn = local.units[i];
                if (pawn.present && pawn.enemy && pawn.tile != INVALID_TILE)
                    m_enemyPositions.push_back(pawn.tile);
                if (m_sampleCapperKi == NO_UNIT && pawn.present && !pawn.enemy && pawn.capper)
                    m_sampleCapperKi = static_cast<int>(i);
            }
            for (const auto & prop : properties)
            {
                if (prop.visible && prop.capturable && prop.ownerId != knowledge.observerId())
                {
                    m_unownedProperties.push_back({prop.x, prop.y});
                    if (prop.ownerId == NO_OWNER)
                        m_unownedNeutralProperties.push_back({prop.x, prop.y});
                    if (prop.canProduce)
                        m_unownedFactories.push_back({prop.x, prop.y});
                }
                if (prop.visible && prop.canProduce && prop.ownerId != NO_OWNER && prop.ownerId != knowledge.observerId())
                    m_enemyFactories.push_back({prop.x, prop.y});
                if (prop.visible && prop.ownerId == knowledge.observerId() && !prop.canProduce)
                    m_friendlyRepairSites.push_back({prop.x, prop.y});
                if (prop.visible && prop.capturable && prop.ownerId == knowledge.observerId() && prop.capturePoints <= 0)
                {
                    m_ownCapturableProperties.push_back(OwnCapturableProperty{
                        .tile = {prop.x, prop.y},
                        .income = captureIncome(prop),
                        .isHq = prop.objective == ObjectiveKind::Headquarters,
                    });
                }
            }
            if (m_stockContext.valid())
            {
                for (std::size_t n = 0; n < m_stockContext.nodeCount(); ++n)
                {
                    if (m_stockContext.nodeOwner(n) != OwnerSign::Neutral) continue;
                    const TilePoint tile = m_stockContext.nodeTile(n);
                    if (tile == INVALID_TILE) continue;
                    const bool nearEnemyFactory = std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                        return manhattanDistance(f, tile) <= UNIFIED_INFANTRY_BASE_MOVE + 1;
                    });
                    if (!nearEnemyFactory && !m_stockContext.enemyClaimsNeutralBeforeUs(n))
                        m_deliverableNeutralProperties.push_back(tile);
                }
            }
            else
            {
                m_deliverableNeutralProperties = m_unownedNeutralProperties;
            }
            buildPathDistances();
        }

        static int pawnTravelGrid(const CombatPawn & pawn)
        {
            return pawn.travelGrid >= 0 ? pawn.travelGrid : pawn.grid;
        }

        int enemyPathDistance(int ki, TilePoint dest) const
        {
            if (ki >= 0 && ki < static_cast<int>(m_local.units.size()) &&
                m_local.width > 0 && m_local.height > 0 &&
                dest.x >= 0 && dest.x < m_local.width && dest.y >= 0 && dest.y < m_local.height)
            {
                const int gridId = pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]);
                const auto [minR, maxR] = unitAttackRange(ki);
                if (const auto it = m_enemyPathDistByProfile.find({gridId, minR, maxR});
                    it != m_enemyPathDistByProfile.end())
                {
                    const std::size_t idx = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(dest.x);
                    if (idx < it->second.size() && it->second[idx] < std::numeric_limits<std::int32_t>::max())
                        return it->second[idx];
                }
            }
            int minDist = m_maxDist;
            for (const TilePoint e : m_enemyPositions)
                minDist = std::min(minDist, manhattanDistance(dest, e));
            return m_maxDist * 2 + minDist;
        }

        int repairPathDistance(int ki, TilePoint dest) const
        {
            return cachedSetPathDistance(m_repairPathDistByGrid, m_friendlyRepairSites, ki, dest);
        }

        int unownedPropertyPathDistance(int ki, TilePoint dest) const
        {
            return cachedSetPathDistance(m_unownedPropPathDistByGrid, m_unownedProperties, ki, dest);
        }

        int unownedFactoryPathDistance(int ki, TilePoint dest) const
        {
            return cachedSetPathDistance(m_unownedFactoryPathDistByGrid, m_unownedFactories, ki, dest);
        }

        int tilePathDistance(int ki, TilePoint from, TilePoint goal, bool allowAdjacent = false) const
        {
            if (from == goal) return 0;
            const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) ||
                m_local.width <= 0 || m_local.height <= 0 ||
                from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height)
                return manhattanDistance(from, goal);
            const auto key = std::tuple{gridId, goal.x, goal.y, allowAdjacent};
            auto it = m_tilePathDistCache.find(key);
            if (it == m_tilePathDistCache.end())
            {
                const int range = allowAdjacent ? 1 : 0;
                it = m_tilePathDistCache.emplace(
                    key, buildReversePathDistances(gridId, std::span<const TilePoint>(&goal, 1), range, range)).first;
            }
            const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(from.x);
            if (idx < it->second.size() && it->second[idx] < std::numeric_limits<std::int32_t>::max())
                return it->second[idx];
            return m_maxDist * 2 + manhattanDistance(from, goal);
        }

        struct TieBreakerScores
        {
            std::int32_t activeCaptures{0};
            std::int32_t unitsRepairing{0};
            std::int32_t combatUnitsInThreatRange{0};
            std::int64_t stagingScore{0};
            std::int32_t negativeClumpPairs{0};
            std::int64_t terrainCoverScore{0};
        };

        TieBreakerScores computeTieBreakers(std::span<const int> choices) const
        {
            struct CapperPos
            {
                int slot{0};
                int ki{NO_UNIT};
                TilePoint dest{INVALID_TILE};
            };
            std::vector<TilePoint> claimedProperties;
            std::vector<TilePoint> friendlyArmorPositions;
            std::vector<CapperPos> nonCapturingCappers;
            for (const auto & member : m_problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
                const int opt = choices[static_cast<std::size_t>(slot)];
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
                if (member.stockRow == NO_STOCK_ROW)
                    friendlyArmorPositions.push_back(cand.destination);
                else if (!cand.capture.has_value())
                    nonCapturingCappers.push_back({slot, member.knowledgeUnitIndex, cand.destination});
                if (cand.capture.has_value() ||
                    (member.stockRow != NO_STOCK_ROW &&
                     std::ranges::find(m_unownedProperties, cand.destination) != m_unownedProperties.end()))
                    claimedProperties.push_back(cand.destination);
            }
            // Match each unclaimed deliverable neutral 1-to-1 to its nearest healthy capper; surplus cappers support armor.
            std::map<int, TilePoint> assignedNeutralBySlot;
            std::vector<TilePoint> unclaimedDeliverables;
            for (const TilePoint p : m_deliverableNeutralProperties)
            {
                if (std::ranges::find(claimedProperties, p) == claimedProperties.end())
                    unclaimedDeliverables.push_back(p);
            }
            std::vector<std::uint8_t> capperAssigned(nonCapturingCappers.size(), 0);
            for (const TilePoint neutral : unclaimedDeliverables)
            {
                int bestIdx = -1;
                int bestDist = std::numeric_limits<int>::max();
                for (std::size_t i = 0; i < nonCapturingCappers.size(); ++i)
                {
                    if (capperAssigned[i] != 0) continue;
                    const int d = tilePathDistance(nonCapturingCappers[i].ki, nonCapturingCappers[i].dest, neutral);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        bestIdx = static_cast<int>(i);
                    }
                }
                if (bestIdx >= 0)
                {
                    capperAssigned[static_cast<std::size_t>(bestIdx)] = 1;
                    assignedNeutralBySlot.emplace(nonCapturingCappers[static_cast<std::size_t>(bestIdx)].slot, neutral);
                }
            }
            TieBreakerScores out;
            const int capPts = m_stockField.capturePointsToCapture();
            const int fullCapTurns = (capPts + UNIT_HP_STEPS - 1) / UNIT_HP_STEPS;
            for (const auto & member : m_problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
                const int opt = choices[static_cast<std::size_t>(slot)];
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
                const TilePoint dest = cand.destination;
                const int ki = member.knowledgeUnitIndex;
                int minPropDist = std::numeric_limits<int>::max();
                bool foundUnclaimed = false;
                for (const TilePoint p : m_unownedProperties)
                {
                    if (member.stockRow != NO_STOCK_ROW && p != dest &&
                        std::ranges::find(claimedProperties, p) != claimedProperties.end())
                        continue;
                    foundUnclaimed = true;
                    minPropDist = std::min(minPropDist, tilePathDistance(ki, dest, p));
                }
                if (!foundUnclaimed && member.stockRow != NO_STOCK_ROW)
                    minPropDist = enemyPathDistance(ki, dest);
                int stars = 0;
                if (m_local.defense && ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                    stars = m_local.defense(ki, dest).stars;
                out.terrainCoverScore += static_cast<std::int64_t>(stars);
                if (member.stockRow == NO_STOCK_ROW)
                {
                    const auto & unit = m_knowledge.units()[static_cast<std::size_t>(ki)];
                    const bool indirect = unit.minRange > 1;
                    const int maxRange = std::max(1, unit.maxRange);
                    const int pathDist = !m_enemyPositions.empty()
                        ? enemyPathDistance(ki, dest)
                        : unownedPropertyPathDistance(ki, dest);
                    if (indirect)
                    {
                        const bool inFiringRing = !m_enemyPositions.empty() &&
                            std::ranges::any_of(m_enemyPositions, [&](TilePoint e) {
                                const int d = manhattanDistance(dest, e);
                                return d >= unit.minRange && d <= maxRange;
                            });
                        if (inFiringRing)
                            ++out.combatUnitsInThreatRange;
                        out.stagingScore -= static_cast<std::int64_t>(pathDist);
                    }
                    else
                    {
                        const int nextTurnThreatReach = unit.movementPoints + maxRange;
                        if (!m_enemyPositions.empty() && pathDist <= nextTurnThreatReach)
                            ++out.combatUnitsInThreatRange;
                        out.stagingScore -= static_cast<std::int64_t>(pathDist);
                    }
                }
                else
                {
                    const bool hasKnown = ki >= 0 && ki < static_cast<int>(m_knowledge.units().size());
                    const bool damaged = hasKnown &&
                        m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded < UNIT_HP_STEPS;
                    const int capRate = hasKnown ? m_knowledge.units()[static_cast<std::size_t>(ki)].captureRate : 0;
                    const bool heavilyDamaged = hasKnown &&
                        (capRate <= 0 || (capPts + capRate - 1) / capRate > fullCapTurns + 1);
                    const auto assignedIt = assignedNeutralBySlot.find(slot);
                    const bool isSurplusCapper = !cand.capture.has_value() && assignedIt == assignedNeutralBySlot.end();
                    if (cand.capture.has_value())
                    {
                        ++out.activeCaptures;
                    }
                    else if (heavilyDamaged && !m_friendlyRepairSites.empty())
                    {
                        out.stagingScore -= static_cast<std::int64_t>(repairPathDistance(ki, dest));
                    }
                    else if (assignedIt != assignedNeutralBySlot.end())
                    {
                        out.stagingScore -= static_cast<std::int64_t>(tilePathDistance(ki, dest, assignedIt->second));
                    }
                    else if (isSurplusCapper && !friendlyArmorPositions.empty())
                    {
                        int minArmorDist = std::numeric_limits<int>::max();
                        for (const TilePoint a : friendlyArmorPositions)
                            minArmorDist = std::min(minArmorDist, tilePathDistance(ki, dest, a, true));
                        out.stagingScore -= static_cast<std::int64_t>(std::max(2, minArmorDist) + minPropDist);
                    }
                    else
                    {
                        out.stagingScore -= static_cast<std::int64_t>(minPropDist);
                    }
                    if (isSurplusCapper)
                    {
                        for (const auto & peer : nonCapturingCappers)
                        {
                            if (peer.slot > slot && manhattanDistance(dest, peer.dest) == 1)
                                --out.negativeClumpPairs;
                        }
                    }
                    if (damaged &&
                        std::ranges::find(m_friendlyRepairSites, dest) != m_friendlyRepairSites.end())
                        ++out.unitsRepairing;
                }
            }
            return out;
        }

        // Orders same-target attackers by counter-capital exposure (zero-counter / indirect first, then lowest counter loss).
        std::vector<CombatFireOrder> buildDefaultFireOrders(std::span<const int> choices) const
        {
            struct AttackerInfo
            {
                int unitId{NO_UNIT};
                MilliFunds counterCapital{0};
                std::int32_t damageSteps{0};
            };
            std::map<int, std::vector<AttackerInfo>> byTarget;
            for (const auto & member : m_problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
                const int opt = choices[static_cast<std::size_t>(slot)];
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
                if (cand.targetUnitId == NO_UNIT) continue;
                const auto * comp = singleComponentOf(cand.source.bundle);
                MilliFunds counterCap = 0;
                std::int32_t dmgSteps = 0;
                if (comp && comp->kind == ComponentKind::Fire)
                {
                    dmgSteps = comp->fire.damageSteps;
                    const bool indirect = member.knowledgeUnitIndex >= 0 &&
                        member.knowledgeUnitIndex < static_cast<int>(m_knowledge.units().size()) &&
                        m_knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)].minRange > 1;
                    if (!indirect && comp->fire.counterSteps > 0)
                    {
                        const MilliFunds cost = member.knowledgeUnitIndex >= 0 &&
                            member.knowledgeUnitIndex < static_cast<int>(m_local.units.size())
                            ? m_local.units[static_cast<std::size_t>(member.knowledgeUnitIndex)].replacementCost
                            : cand.source.actor.replacementCost;
                        counterCap = bookDamage(cost, comp->fire.counterSteps);
                    }
                }
                byTarget[cand.targetUnitId].push_back({member.engineUnitId, counterCap, dmgSteps});
            }
            std::vector<CombatFireOrder> orders;
            for (auto & [targetId, attackers] : byTarget)
            {
                if (attackers.size() < 2) continue;
                std::stable_sort(attackers.begin(), attackers.end(), [](const AttackerInfo & a, const AttackerInfo & b) {
                    const bool aDraws = a.counterCapital > 0;
                    const bool bDraws = b.counterCapital > 0;
                    if (aDraws != bDraws) return !aDraws;
                    if (a.counterCapital != b.counterCapital) return a.counterCapital < b.counterCapital;
                    if (a.damageSteps != b.damageSteps) return a.damageSteps > b.damageSteps;
                    return a.unitId < b.unitId;
                });
                for (std::size_t i = 0; i + 1 < attackers.size(); ++i)
                    orders.push_back(CombatFireOrder{attackers[i].unitId, attackers[i + 1].unitId});
            }
            return orders;
        }

        UnifiedTeamScore evaluate(std::span<const int> choices, const UnifiedTeamScore * pruneFloor = nullptr,
                                  ReplyPassEvaluation * economicOut = nullptr,
                                  std::span<const CombatFireOrder> fireOrdersOverride = {},
                                  bool useDefaultFireOrders = true)
        {
            UnifiedTeamScore summary;
            const std::vector<CombatFireOrder> defaultOrders = (fireOrdersOverride.empty() && useDefaultFireOrders)
                ? buildDefaultFireOrders(choices) : std::vector<CombatFireOrder>{};
            const std::span<const CombatFireOrder> activeFireOrders = !fireOrdersOverride.empty()
                ? fireOrdersOverride : std::span<const CombatFireOrder>(defaultOrders);
            const TieBreakerScores ties = computeTieBreakers(choices);
            const auto priorPolicy = m_local.policy;
            const int priorTarget = m_local.replyTarget;
            const int priorShooter = m_local.containShooter;
            const auto priorFrom = m_local.containFrom;
            auto * const priorWorkLeft = m_local.workLeft;
            const auto restore = qScopeGuard([&] {
                m_local.policy = priorPolicy;
                m_local.replyTarget = priorTarget;
                m_local.containShooter = priorShooter;
                m_local.containFrom = priorFrom;
                m_local.workLeft = priorWorkLeft;
            });
            m_local.containShooter = NO_UNIT;
            m_local.containFrom = INVALID_TILE;
            m_local.workLeft = &m_work;

            std::vector<std::pair<TacticalReply, int>> policies{
                {TacticalReply::Economic, NO_UNIT},
            };
            const int topExposed = mostValuableExposedUnit(m_problem, m_local, choices);
            const int topCapper = mostValuableExposedCapper(m_problem, m_local, choices);
            if (topExposed != NO_UNIT)
            {
                if (topCapper != NO_UNIT)
                    policies.emplace_back(TacticalReply::InterruptCapture, NO_UNIT);
                policies.emplace_back(TacticalReply::ArmorAndInfantry, NO_UNIT);
                policies.emplace_back(TacticalReply::TargetUnit, topExposed);
                if (topCapper != NO_UNIT && topCapper != topExposed)
                    policies.emplace_back(TacticalReply::TargetUnit, topCapper);
            }

            bool hasAny = false;
            for (const auto & [policy, targetId] : policies)
            {
                if (m_work < m_replayTariff) return UnifiedTeamScore{};
                m_work -= m_replayTariff;
                ++m_evals;
                m_local.policy = policy;
                m_local.replyTarget = targetId;
                const auto eval = evaluateReplyPassPlan(m_problem, choices, m_actingBoard, {}, activeFireOrders);
                if (!eval.valued() || !eval.replay.valid() || !eval.replay.complete())
                    return UnifiedTeamScore{};
                if (policy == TacticalReply::Economic && economicOut)
                    *economicOut = eval;

                const auto hqRank = m_hqRankFn(eval);
                if (!hqRank) return UnifiedTeamScore{};
                const auto baseRealized = openRefinementRealizedMaterial(eval, m_local, m_knowledge);
                if (!baseRealized) return UnifiedTeamScore{};
                const MilliFunds trapLoss = computeFollowupTrapLoss(eval);
                MilliFunds realized = *baseRealized;
                if (!RelativeStockDetail::checkedSubtract(realized, trapLoss))
                    return UnifiedTeamScore{};

                const MilliFunds postStock = cachedPostReplyStock(eval);
                const MilliFunds denial = computeEnemyDenial(eval);
                const auto [hqCaptureStartSafe, ownRisk] = computeOwnPropertyCaptureRisk(eval);
                const int buildSlots = m_buildSlotsFn(eval);
                const MilliFunds factoryVal = static_cast<MilliFunds>(buildSlots) * UNIFIED_FACTORY_SLOT_VALUE;

                MilliFunds econTotal = realized;
                if (!RelativeStockDetail::checkedAdd(econTotal, postStock) ||
                    !RelativeStockDetail::checkedAdd(econTotal, denial) ||
                    !RelativeStockDetail::checkedSubtract(econTotal, ownRisk) ||
                    !RelativeStockDetail::checkedAdd(econTotal, factoryVal))
                    return UnifiedTeamScore{};

                UnifiedTeamScore point;
                point.valid = true;
                point.worstHqRank = *hqRank;
                point.hqCaptureStartSafe = hqCaptureStartSafe;
                point.worstEconomicValue = econTotal;
                point.activeCaptures = ties.activeCaptures;
                point.unitsRepairing = ties.unitsRepairing;
                point.combatUnitsInThreatRange = ties.combatUnitsInThreatRange;
                point.stagingScore = ties.stagingScore;
                point.negativeClumpPairs = ties.negativeClumpPairs;
                point.terrainCoverScore = ties.terrainCoverScore;
                point.realizedMaterial = realized;
                point.postReplyStock = postStock;
                point.enemyDenial = denial;
                point.ownCaptureRisk = ownRisk;
                point.factoryValue = factoryVal;
                point.affordableBuildSlots = buildSlots;
                point.bindingPolicy = policy;
                point.bindingTargetUnit = targetId;

                // Worst-case over policies is <= every individual policy; exact pruning.
                if (pruneFloor && pruneFloor->valid && point.notBetterThan(*pruneFloor))
                {
                    ++m_pruned;
                    return UnifiedTeamScore{};
                }
                if (!hasAny || summary.strictlyBetterThan(point))
                {
                    summary = point;
                    hasAny = true;
                }
            }
            return summary;
        }

        int evalCount() const { return m_evals; }
        int prunedCount() const { return m_pruned; }

    private:
        // Masks future nodes a lone capper cannot deliver without creating E0 lure or 1-HP chip cliffs.
        std::vector<std::uint8_t> computeBlockedStockNodes(const ReplyPassEvaluation & eval) const
        {
            const std::size_t nodeCount = m_stockContext.nodeCount();
            std::vector<std::uint8_t> blocked(nodeCount, 0);
            if (nodeCount == 0 || m_sampleCapperKi == NO_UNIT || (!m_local.weapon && !m_local.weaponSlots))
                return blocked;
            const std::int32_t capPts = m_stockField.capturePointsToCapture();
            std::vector<bool> finishingNextTurn(nodeCount, false);
            struct FriendlyCover
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int moveBudget{0};
                int strikeRange{1};
                int maxRange{1};
                MilliFunds cost{0};
            };
            std::vector<FriendlyCover> coverUnits;
            for (const auto & actor : eval.actors)
            {
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                if (actor.stepsAfterReply <= 0) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                if (member.stockRow == NO_STOCK_ROW)
                {
                    const int ki = member.knowledgeUnitIndex;
                    if (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                    {
                        const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
                        const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                        const int minR = std::max(1, knownF.minRange);
                        const int maxR = std::max(1, knownF.maxRange);
                        const int moveB = minR > 1 ? 0 : std::max(0, fp.movement);
                        double hpF = static_cast<double>(actor.stepsAfterReply);
                        if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                            hpF = it->second;
                        if (hpF > 0.0)
                            coverUnits.push_back({ki, cand.destination, hpF, moveB, minR, maxR, fp.replacementCost});
                    }
                    continue;
                }
                if (!cand.capture.has_value()) continue;
                const auto & known = m_knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)];
                const int carried = (cand.destination == cand.origin ? known.capturePoints : 0) + known.captureRate;
                const int nextRate = replyCaptureRate(known.captureRate,
                    std::max(0, known.hpRounded - actor.stepsAfterReply), false);
                if (carried < capPts && carried + nextRate >= capPts)
                {
                    for (std::size_t n = 0; n < nodeCount; ++n)
                    {
                        if (m_stockContext.nodeTile(n) == cand.destination)
                        {
                            finishingNextTurn[n] = true;
                            break;
                        }
                    }
                }
            }
            struct Defender
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int moveBudget{0};
                int strikeRange{1};
                int maxRange{1};
                MilliFunds cost{0};
            };
            std::vector<Defender> defenders;
            std::vector<std::int32_t> killedInF0;
            for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
            {
                const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                if (!ep.present || !ep.enemy) continue;
                const double initialHp = ep.realHp >= 0.0
                    ? ep.realHp
                    : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded);
                double hpOwn = initialHp;
                if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                    hpOwn = static_cast<double>(itOwn->second);
                if (hpOwn <= 0.0)
                {
                    killedInF0.push_back(e);
                    continue;
                }
                const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                const int minR = std::max(1, knownE.minRange);
                const int maxR = std::max(1, knownE.maxRange);
                const int moveB = minR > 1 ? 0 : std::max(0, ep.movement);
                // Keep pre-E0 position and initial HP for surviving defenders to avoid E0 lure and chip cliffs.
                defenders.push_back({e, ep.tile, initialHp, moveB, minR, maxR, ep.replacementCost});
            }
            for (std::size_t n = 0; n < nodeCount; ++n)
            {
                if (finishingNextTurn[n]) continue;
                const TilePoint tile = m_stockContext.nodeTile(n);
                if (tile == INVALID_TILE) continue;
                const bool nearEnemyFactory = std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                    return manhattanDistance(f, tile) <= UNIFIED_INFANTRY_BASE_MOVE + 1;
                });
                const OwnerSign owner = m_stockContext.nodeOwner(n);
                if (owner == OwnerSign::Neutral)
                {
                    if (nearEnemyFactory || m_stockContext.enemyClaimsNeutralBeforeUs(n, killedInF0))
                        blocked[n] = 1;
                    continue;
                }
                if (owner != OwnerSign::Enemy) continue;
                if (nearEnemyFactory)
                {
                    blocked[n] = 1;
                    continue;
                }
                const TerrainDefenseRule defRule = m_local.defense
                    ? m_local.defense(m_sampleCapperKi, tile) : TerrainDefenseRule{};
                for (const auto & d : defenders)
                {
                    if (!canUnitReachAttack(d.ki, d.pos, tile, d.moveBudget, d.strikeRange, d.maxRange)) continue;
                    const bool coveredByFriendlyArmor = std::ranges::any_of(coverUnits, [&](const FriendlyCover & fc) {
                        if (!canUnitReachAttack(fc.ki, fc.pos, tile, fc.moveBudget, fc.strikeRange, fc.maxRange))
                            return false;
                        const double coverBase = maxWeaponBaseDamage(fc.ki, d.ki, fc.strikeRange);
                        if (coverBase <= 0.0) return false;
                        const TerrainDefenseRule dDef = m_local.defense
                            ? m_local.defense(d.ki, d.pos) : TerrainDefenseRule{};
                        const double rawHit = terrainReducedDamage(dDef, coverBase, fc.hp, ceilHpSteps(d.hp));
                        const double hit = engineShotDamage(rawHit);
                        if (hit <= 0.0) return false;
                        const double afterD = engineHpAfterReply(d.hp, hit);
                        if (afterD <= 0.0) return true;
                        const MilliFunds capLoss = combatCapitalLoss(d.cost, d.hp, afterD);
                        MilliFunds counterLoss = 0;
                        if (fc.strikeRange == 1)
                        {
                            const double cBase = maxWeaponBaseDamage(d.ki, fc.ki, 1);
                            if (cBase > 0.0)
                            {
                                const TerrainDefenseRule fDef = m_local.defense
                                    ? m_local.defense(fc.ki, fc.pos) : TerrainDefenseRule{};
                                const double rawC = sameActionCounterDamage(
                                    fDef, cBase, d.hp, rawHit, ceilHpSteps(fc.hp));
                                const double afterF = engineHpAfterReply(fc.hp, engineShotDamage(rawC));
                                counterLoss = combatCapitalLoss(fc.cost, fc.hp, afterF);
                            }
                        }
                        return (d.cost == 0 && fc.cost == 0) || capLoss > counterLoss;
                    });
                    if (coveredByFriendlyArmor) continue;
                    const double baseDmg = maxWeaponBaseDamage(d.ki, m_sampleCapperKi, d.strikeRange);
                    if (baseDmg <= 0.0) continue;
                    const double hit1 = engineShotDamage(terrainReducedDamage(defRule, baseDmg, d.hp, UNIT_HP_STEPS));
                    const double hp1 = engineHpAfterReply(static_cast<double>(UNIT_HP_STEPS), hit1);
                    const int s1 = ceilHpSteps(hp1);
                    if (s1 <= 0)
                    {
                        blocked[n] = 1;
                        break;
                    }
                    if (UNIT_HP_STEPS + s1 >= capPts) continue;
                    const double hit2 = engineShotDamage(terrainReducedDamage(defRule, baseDmg, d.hp, s1));
                    const double hp2 = engineHpAfterReply(hp1, hit2);
                    const int s2 = ceilHpSteps(hp2);
                    if (s2 <= 0 || UNIT_HP_STEPS + s1 + s2 < capPts)
                    {
                        blocked[n] = 1;
                        break;
                    }
                }
            }
            return blocked;
        }

        MilliFunds cachedPostReplyStock(const ReplyPassEvaluation & eval)
        {
            const ContinuationKey & key = eval.afterReply;
            const std::vector<std::uint8_t> blocked = computeBlockedStockNodes(eval);
            const auto cacheKey = std::pair{key, blocked};
            if (const auto it = m_seqStockCache.find(cacheKey); it != m_seqStockCache.end())
                return it->second;
            if (m_stockContext.valid())
            {
                if (!m_stockTuned)
                {
                    const auto tuned = m_stockContext.tune(key, UNIFIED_STOCK_TUNE_ROUNDS);
                    m_stockTuned = true;
                    if (tuned.status == ReplyStockStatus::Valid && tuned.floor.status == ReplyStockStatus::Valid)
                        m_baselineFloor = tuned.floor;
                    if (m_stockContext.priceVersion() == 0)
                    {
                        const std::vector<MilliFunds> zeroPrices(m_stockContext.nodeCount(), 0);
                        m_stockContext.installPrices(zeroPrices);
                    }
                }
                const auto owned = m_stockContext.owned(key);
                const auto floor = m_stockContext.floor(
                    key, m_baselineFloor.status == ReplyStockStatus::Valid ? &m_baselineFloor : nullptr, blocked, true);
                if (owned.status == ReplyStockStatus::Valid && floor.status == ReplyStockStatus::Valid)
                {
                    const MilliFunds spawnGain = m_stockContext.futureFactorySpawnGain(
                        key, floor, m_properties, m_knowledge.observerId(), blocked);
                    MilliFunds value = owned.owned;
                    if (RelativeStockDetail::checkedAdd(value, floor.floor) &&
                        RelativeStockDetail::checkedAdd(value, spawnGain) &&
                        RelativeStockDetail::checkedSubtract(value, m_stockContext.enemyOptimum()))
                    {
                        m_seqStockCache.emplace(cacheKey, value);
                        return value;
                    }
                }
            }
            const auto actions = buildPostReplyStockActions(m_problem, m_knowledge, eval);
            const MilliFunds fallback = m_stockField.jointStock(actions);
            m_seqStockCache.emplace(cacheKey, fallback);
            return fallback;
        }

        double maxWeaponBaseDamage(int attackerKi, int defenderKi, int range) const
        {
            const auto & pawn = m_local.units[static_cast<std::size_t>(attackerKi)];
            if (m_local.weaponSlots)
            {
                const auto weaponDamages = m_local.weaponSlots(attackerKi, defenderKi, range);
                double best = 0.0;
                for (int n = 0; n < 2; ++n)
                {
                    if (pawn.ammoByWeapon[static_cast<std::size_t>(n)] != 0 &&
                        weaponDamages[static_cast<std::size_t>(n)] > best)
                        best = weaponDamages[static_cast<std::size_t>(n)];
                }
                return best;
            }
            return m_local.weapon ? std::max(0.0, m_local.weapon(attackerKi, defenderKi, range)) : 0.0;
        }

        // Prices E1 follow-up loss for damaged units and boxed-in full-HP armor with no safe F1 escape.
        MilliFunds computeFollowupTrapLoss(const ReplyPassEvaluation & eval) const
        {
            if (m_local.width <= 0 || m_local.height <= 0 || (!m_local.weapon && !m_local.weaponSlots))
                return 0;
            struct EscapeVictim
            {
                int ki{NO_UNIT};
                TilePoint tile{INVALID_TILE};
                double hpAfterE0{0.0};
                int stepsAfterE0{0};
                bool damaged{false};
            };
            struct FriendlyCoverUnit
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int moveBudget{0};
                int strikeRange{1};
                int maxRange{1};
            };
            std::vector<EscapeVictim> victims;
            std::vector<FriendlyCoverUnit> friendlyCover;
            std::vector<TilePoint> friendlyTiles;
            for (const auto & actor : eval.actors)
            {
                if (actor.stepsAfterReply <= 0) continue;
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                if (ki < 0 || ki >= static_cast<int>(m_local.units.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                friendlyTiles.push_back(cand.destination);
                const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
                double hp = static_cast<double>(actor.stepsAfterReply);
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hp = it->second;
                if (hp <= 0.0) continue;
                if (member.stockRow == NO_STOCK_ROW)
                {
                    const int minR = std::max(1, knownF.minRange);
                    const int maxR = std::max(1, knownF.maxRange);
                    const int moveB = minR > 1 ? 0 : std::max(0, fp.movement);
                    friendlyCover.push_back({ki, cand.destination, hp, moveB, minR, maxR});
                }
                const int initialSteps = knownF.hpRounded;
                if (actor.stepsAfterReply < initialSteps)
                {
                    if (member.stockRow != NO_STOCK_ROW && cand.capture.has_value()) continue;
                    victims.push_back({ki, cand.destination, hp, actor.stepsAfterReply, true});
                }
                else if (member.stockRow == NO_STOCK_ROW)
                {
                    const bool nearEnemyFactory = std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                        return manhattanDistance(f, cand.destination) <= fp.movement + UNIFIED_FACTORY_COUNTER_REACH;
                    });
                    if (nearEnemyFactory)
                        victims.push_back({ki, cand.destination, hp, actor.stepsAfterReply, false});
                }
            }
            if (victims.empty()) return 0;

            struct EnemyThreat
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int damagedMove{0};
                int oneTurnMove{0};
                int strikeRange{1};
                int maxRange{1};
                bool capper{false};
                MilliFunds cost{0};
            };
            std::vector<EnemyThreat> threats;
            std::vector<TilePoint> enemyTiles;
            for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
            {
                const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                if (!ep.present || !ep.enemy) continue;
                double hpE = ep.realHp >= 0.0
                    ? ep.realHp
                    : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded);
                if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                    hpE = static_cast<double>(it->second);
                else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                    hpE = static_cast<double>(itOwn->second);
                if (hpE <= 0.0) continue;
                const bool firedE0 = std::ranges::any_of(eval.shots, [&](const ReplyPassShot & s) {
                    return s.shooterIndex == e;
                });
                const TilePoint posE = (firedE0 && e < static_cast<int>(eval.combatLocations.size()) &&
                                        eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                    ? eval.combatLocations[static_cast<std::size_t>(e)]
                    : ep.tile;
                enemyTiles.push_back(posE);
                const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                const int minR = std::max(1, knownE.minRange);
                const int maxR = std::max(1, knownE.maxRange);
                const int moveE = std::max(0, ep.movement);
                const int followupMoves = (!firedE0 && !ep.capper) ? UNIFIED_TRAP_FOLLOWUP_TURNS : 1;
                const int damagedMove = minR > 1 ? (firedE0 ? 0 : moveE) : (followupMoves * moveE);
                const int oneTurnMove = minR > 1 ? 0 : moveE;
                threats.push_back({e, posE, hpE, damagedMove, oneTurnMove, minR, maxR,
                                   ep.capper, ep.replacementCost});
            }
            if (threats.empty()) return 0;

            std::vector<TilePoint> openEnemyFactories;
            for (const TilePoint f : m_enemyFactories)
            {
                const bool busy =
                    std::ranges::any_of(enemyTiles, [&](TilePoint t) { return t == f; }) ||
                    std::ranges::any_of(friendlyTiles, [&](TilePoint t) { return t == f; });
                if (!busy) openEnemyFactories.push_back(f);
            }
            const auto isEnemyZoc = [&](TilePoint t) {
                return std::ranges::any_of(enemyTiles, [&](TilePoint et) { return manhattanDistance(et, t) == 1; });
            };

            MilliFunds totalTrapLoss = 0;
            const std::size_t cellCount = static_cast<std::size_t>(m_local.width) * static_cast<std::size_t>(m_local.height);
            for (const auto & v : victims)
            {
                const auto & pawn = m_local.units[static_cast<std::size_t>(v.ki)];
                const int gridId = pawnTravelGrid(pawn);
                if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) || pawn.movement <= 0)
                    continue;
                const auto & grid = m_local.grids[static_cast<std::size_t>(gridId)];
                if (!grid.onGrid(v.tile.x, v.tile.y)) continue;

                std::vector<std::int32_t> dist(cellCount, std::numeric_limits<std::int32_t>::max());
                using Node = std::pair<std::int32_t, TilePoint>;
                const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
                std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
                const auto idxOf = [&](TilePoint t) {
                    return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) + static_cast<std::size_t>(t.x);
                };
                dist[idxOf(v.tile)] = 0;
                pq.push({0, v.tile});
                std::vector<TilePoint> escapeTiles;
                while (!pq.empty())
                {
                    const auto [d, cur] = pq.top();
                    pq.pop();
                    if (d > dist[idxOf(cur)]) continue;
                    escapeTiles.push_back(cur);
                    if (cur != v.tile && isEnemyZoc(cur)) continue;
                    for (const EntryDirection dir : ENTRY_DIRECTIONS)
                    {
                        const StepVector step = entryStep(dir);
                        const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                        if (!grid.onGrid(next.x, next.y)) continue;
                        const bool occupiedByEnemy = std::ranges::any_of(enemyTiles, [&](TilePoint et) { return et == next; });
                        if (occupiedByEnemy) continue;
                        const std::int32_t c = grid.cost(next.x, next.y, dir);
                        if (c == IMPASSABLE_COST) continue;
                        const std::int32_t nd = d + c;
                        if (nd <= pawn.movement && nd < dist[idxOf(next)])
                        {
                            dist[idxOf(next)] = nd;
                            pq.push({nd, next});
                        }
                    }
                }
                MilliFunds minEscapeLoss = std::numeric_limits<MilliFunds>::max();
                for (const TilePoint q : escapeTiles)
                {
                    if (!v.damaged)
                    {
                        const bool coveredByFriendly = std::ranges::any_of(friendlyCover, [&](const FriendlyCoverUnit & fc) {
                            return fc.ki != v.ki &&
                                canUnitReachAttack(fc.ki, fc.pos, q, fc.moveBudget, fc.strikeRange, fc.maxRange);
                        });
                        if (coveredByFriendly)
                        {
                            minEscapeLoss = 0;
                            break;
                        }
                    }
                    const TerrainDefenseRule defRule = m_local.defense ? m_local.defense(v.ki, q) : TerrainDefenseRule{};
                    struct ShotPlan
                    {
                        double baseDmg{0.0};
                        double attackerHp{0.0};
                        double counterBase{0.0};
                        int strikeRange{1};
                        MilliFunds attackerCost{0};
                        int attackerKi{NO_UNIT};
                    };
                    std::vector<ShotPlan> shotsAtQ;
                    bool hasCombatUnitThreat = false;
                    for (const auto & th : threats)
                    {
                        const int moveB = v.damaged ? th.damagedMove : th.oneTurnMove;
                        if (!canUnitReachAttack(th.ki, th.pos, q, moveB, th.strikeRange, th.maxRange)) continue;
                        const double baseDmg = maxWeaponBaseDamage(th.ki, v.ki, th.strikeRange);
                        if (baseDmg <= 0.0) continue;
                        if (!th.capper) hasCombatUnitThreat = true;
                        const double cBase = th.strikeRange == 1 ? maxWeaponBaseDamage(v.ki, th.ki, 1) : 0.0;
                        shotsAtQ.push_back({baseDmg, th.hp, cBase, th.strikeRange, th.cost, th.ki});
                    }
                    if (!v.damaged && hasCombatUnitThreat)
                    {
                        const bool openFactoryNearQ = std::ranges::any_of(openEnemyFactories, [&](TilePoint f) {
                            return manhattanDistance(f, q) <= UNIFIED_FACTORY_COUNTER_REACH;
                        });
                        if (openFactoryNearQ)
                        {
                            shotsAtQ.push_back({UNIFIED_FACTORY_COUNTER_BASE_DAMAGE,
                                                static_cast<double>(UNIT_HP_STEPS),
                                                UNIFIED_FACTORY_COUNTER_RETURN_DAMAGE, 1,
                                                UNIFIED_FACTORY_COUNTER_COST, NO_UNIT});
                        }
                    }
                    std::stable_sort(shotsAtQ.begin(), shotsAtQ.end(), [](const ShotPlan & a, const ShotPlan & b) {
                        return (a.baseDmg * a.attackerHp) > (b.baseDmg * b.attackerHp);
                    });
                    MilliFunds worstAtQ = 0;
                    double remHp = v.hpAfterE0;
                    MilliFunds combinedGain = 0;
                    for (const auto & sp : shotsAtQ)
                    {
                        if (remHp <= 0.0) break;
                        const double rawHit = terrainReducedDamage(defRule, sp.baseDmg, sp.attackerHp, ceilHpSteps(remHp));
                        const double hit = engineShotDamage(rawHit);
                        if (hit <= 0.0) continue;
                        const double afterU = engineHpAfterReply(remHp, hit);
                        const MilliFunds capLoss = combatCapitalLoss(pawn.replacementCost, remHp, afterU);
                        MilliFunds counterLoss = 0;
                        if (sp.strikeRange == 1 && afterU > 0.0 && sp.counterBase > 0.0)
                        {
                            const TerrainDefenseRule eDef = (sp.attackerKi != NO_UNIT && m_local.defense)
                                ? m_local.defense(sp.attackerKi, q) : TerrainDefenseRule{};
                            const double rawC = sameActionCounterDamage(
                                eDef, sp.counterBase, remHp, rawHit, ceilHpSteps(sp.attackerHp));
                            const double eAfter = engineHpAfterReply(sp.attackerHp, engineShotDamage(rawC));
                            counterLoss = combatCapitalLoss(sp.attackerCost, sp.attackerHp, eAfter);
                        }
                        const MilliFunds netGain = capLoss - counterLoss;
                        if (netGain > worstAtQ) worstAtQ = netGain;
                        if (netGain > 0 || afterU <= 0.0)
                        {
                            combinedGain += netGain;
                            remHp = afterU;
                            if (combinedGain > worstAtQ) worstAtQ = combinedGain;
                        }
                    }
                    if (worstAtQ < minEscapeLoss)
                        minEscapeLoss = worstAtQ;
                    if (minEscapeLoss == 0)
                        break;
                }
                if (minEscapeLoss > 0 && minEscapeLoss < std::numeric_limits<MilliFunds>::max())
                    RelativeStockDetail::checkedAdd(totalTrapLoss, minEscapeLoss);
            }
            return totalTrapLoss;
        }

        MilliFunds computeEnemyDenial(const ReplyPassEvaluation & eval)
        {
            std::map<int, double> finalEnemyHp;
            for (const auto & actor : eval.actors)
            {
                if (actor.ownShot.target == NO_UNIT) continue;
                const int targetKi = actor.ownShot.target;
                auto [it, inserted] = finalEnemyHp.emplace(targetKi, actor.ownShot.targetAfter);
                if (!inserted)
                    it->second = std::min(it->second, actor.ownShot.targetAfter);
            }
            const int horizon = m_stockField.horizonTurns();
            const int capPts = m_stockField.capturePointsToCapture();
            const auto isKilledInF0 = [&](int ki) {
                const auto it = finalEnemyHp.find(ki);
                return it != finalEnemyHp.end() && it->second <= 0.0;
            };
            // Bound killed-capper interrupt delay by the fastest surviving backup capper or factory replacement.
            const auto boundedKillInterruptCredit = [&](int targetKi, const OpenCaptureInterruptInfo & raw) -> MilliFunds {
                if (raw.delayCredit <= 0 || targetKi < 0 ||
                    targetKi >= static_cast<int>(m_knowledge.units().size()))
                    return raw.delayCredit;
                const auto prop = std::ranges::find_if(m_properties, [&](const PropertyFacts & p) {
                    return p.capturable && p.capturerIndex == targetKi && p.capturePoints > 0;
                });
                if (prop == m_properties.end()) return raw.delayCredit;
                const int slot = m_stockField.columnSlotAt({prop->x, prop->y});
                if (slot == NO_STOCK_COLUMN) return raw.delayCredit;
                const auto & killedUnit = m_knowledge.units()[static_cast<std::size_t>(targetKi)];
                const int beforeTurns = static_cast<int>(widenedCaptureTurns(
                    prop->capturePoints, killedUnit.captureRate, capPts));
                int bestReplaceFinish = std::numeric_limits<int>::max();
                const TilePoint propTile{prop->x, prop->y};
                for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
                {
                    if (e == targetKi || isKilledInF0(e)) continue;
                    const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                    if (!ep.present || !ep.enemy || !ep.capper) continue;
                    const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                    if (knownE.captureRate <= 0 || knownE.movementPoints <= 0) continue;
                    const int dist = manhattanDistance(ep.tile, propTile);
                    const int arriveTurns = (dist + knownE.movementPoints - 1) / knownE.movementPoints;
                    const int capTurns = (capPts + knownE.captureRate - 1) / knownE.captureRate;
                    bestReplaceFinish = std::min(bestReplaceFinish, arriveTurns + capTurns);
                }
                for (const TilePoint f : m_enemyFactories)
                {
                    const int dist = manhattanDistance(f, propTile);
                    const int arriveTurns = 1 + (dist + UNIFIED_INFANTRY_BASE_MOVE - 1) / UNIFIED_INFANTRY_BASE_MOVE;
                    const int capTurns = (capPts + UNIT_HP_STEPS - 1) / UNIT_HP_STEPS;
                    bestReplaceFinish = std::min(bestReplaceFinish, arriveTurns + capTurns);
                }
                if (bestReplaceFinish >= std::numeric_limits<int>::max()) return raw.delayCredit;
                const int delayTurns = std::max(0, std::min(horizon, bestReplaceFinish - beforeTurns));
                const auto col = mirroredColumn(m_stockField.enemyColumn(slot));
                const MilliFunds swing = incomeSwing(col.income, delayTurns, col.ownerBefore, OwnerSign::Enemy);
                const MilliFunds bounded = swing < 0 ? -swing : swing;
                return std::min(raw.delayCredit, bounded);
            };
            std::vector<std::int32_t> killedEnemies;
            MilliFunds woundedInterruptCredit = 0;
            MilliFunds killedInterruptFloor = 0;
            MilliFunds nonCaptureReplacementCap = 0;
            bool hasUnboundedNonCapture = m_enemyFactories.empty();
            for (const auto & [targetKi, hpAfter] : finalEnemyHp)
            {
                const auto interrupt = openRefinementCaptureInterruptCredit(
                    m_knowledge, m_stockField, m_properties, targetKi, hpAfter);
                if (hpAfter <= 0.0)
                {
                    const bool hasKnown = targetKi >= 0 &&
                        targetKi < static_cast<int>(m_knowledge.units().size());
                    const bool capturing = hasKnown &&
                        m_knowledge.units()[static_cast<std::size_t>(targetKi)].capturePoints > 0;
                    const int rateE = hasKnown
                        ? m_knowledge.units()[static_cast<std::size_t>(targetKi)].captureRate : 0;
                    const bool viableCapper = hasKnown &&
                        (capturing || (rateE > 0 && (capPts + rateE - 1) / rateE <= horizon));
                    if (viableCapper)
                    {
                        killedEnemies.push_back(targetKi);
                        if (!capturing && !hasUnboundedNonCapture)
                        {
                            const auto & ku = m_knowledge.units()[static_cast<std::size_t>(targetKi)];
                            const int moveE = std::max(1, ku.movementPoints);
                            int minDist = m_maxDist;
                            for (const TilePoint f : m_enemyFactories)
                                minDist = std::min(minDist, manhattanDistance({ku.x, ku.y}, f));
                            const int delayTurns = std::min(horizon, 1 + (minDist + moveE - 1) / moveE);
                            RelativeStockDetail::checkedAdd(
                                nonCaptureReplacementCap,
                                static_cast<MilliFunds>(delayTurns) * UNIFIED_NEUTRAL_PROPERTY_INCOME);
                        }
                    }
                    RelativeStockDetail::checkedAdd(
                        killedInterruptFloor, boundedKillInterruptCredit(targetKi, interrupt));
                }
                else if (interrupt.delayCredit > 0)
                {
                    RelativeStockDetail::checkedAdd(woundedInterruptCredit, interrupt.delayCredit);
                }
            }
            MilliFunds killedStockLoss = m_stockField.enemyStockLoss(killedEnemies);
            if (!hasUnboundedNonCapture)
            {
                MilliFunds totalCap = killedInterruptFloor;
                RelativeStockDetail::checkedAdd(totalCap, nonCaptureReplacementCap);
                killedStockLoss = std::min(killedStockLoss, totalCap);
            }
            MilliFunds total = std::max(killedStockLoss, killedInterruptFloor);
            RelativeStockDetail::checkedAdd(total, woundedInterruptCredit);
            // Credit exact capture delay + counter-damage slowdown when an active enemy capper shoots on E0.
            std::vector<std::uint8_t> replyShooterSeen(m_local.units.size(), 0);
            for (const ReplyPassShot & shot : eval.shots)
            {
                const int e = shot.shooterIndex;
                if (e < 0 || e >= static_cast<int>(m_local.units.size()) ||
                    e >= static_cast<int>(m_knowledge.units().size()) ||
                    replyShooterSeen[static_cast<std::size_t>(e)] != 0)
                    continue;
                replyShooterSeen[static_cast<std::size_t>(e)] = 1;
                const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                if (!ep.present || !ep.enemy || !ep.capper) continue;
                const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                if (knownE.captureRate <= 0 || knownE.capturePoints <= 0) continue;
                const TilePoint shotPos = (e < static_cast<int>(eval.combatLocations.size()) &&
                                           eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                    ? eval.combatLocations[static_cast<std::size_t>(e)]
                    : ep.tile;
                const auto replyHpIt = eval.enemyReplyHp.find(e);
                const double afterHp = replyHpIt != eval.enemyReplyHp.end()
                    ? static_cast<double>(replyHpIt->second)
                    : static_cast<double>(knownE.hpRounded);
                if (shotPos != ep.tile)
                {
                    const auto abandoned = openRefinementCaptureInterruptCredit(
                        m_knowledge, m_stockField, m_properties, e, 0.0);
                    RelativeStockDetail::checkedAdd(total, boundedKillInterruptCredit(e, abandoned));
                }
                else
                {
                    // Shooting in place preserves progress but skips this turn's capture step.
                    RelativeStockDetail::checkedAdd(total, UNIFIED_NEUTRAL_PROPERTY_INCOME);
                    const auto wounded = openRefinementCaptureInterruptCredit(
                        m_knowledge, m_stockField, m_properties, e, afterHp);
                    RelativeStockDetail::checkedAdd(total, wounded.delayCredit);
                }
                if (!m_unownedNeutralProperties.empty() && replyHpIt != eval.enemyReplyHp.end())
                {
                    const int afterSteps = ceilHpSteps(afterHp);
                    const int beforeRate = knownE.captureRate;
                    const int afterRate = replyCaptureRate(
                        beforeRate, std::max(0, knownE.hpRounded - afterSteps), afterSteps <= 0);
                    const int beforeCapTurns = (capPts + beforeRate - 1) / beforeRate;
                    const int slowdownTurns = afterRate <= 0
                        ? horizon
                        : std::max(0, (capPts + afterRate - 1) / afterRate - beforeCapTurns);
                    if (slowdownTurns > 0)
                    {
                        int maxDelay = horizon;
                        if (!m_enemyFactories.empty())
                        {
                            const int moveE = std::max(1, knownE.movementPoints);
                            int minDist = m_maxDist;
                            for (const TilePoint f : m_enemyFactories)
                                minDist = std::min(minDist, manhattanDistance({knownE.x, knownE.y}, f));
                            maxDelay = std::min(horizon, 1 + (minDist + moveE - 1) / moveE);
                        }
                        RelativeStockDetail::checkedAdd(
                            total, static_cast<MilliFunds>(std::min(maxDelay, slowdownTurns)) *
                                UNIFIED_NEUTRAL_PROPERTY_INCOME);
                    }
                }
            }
            return total;
        }

        std::pair<int, int> unitAttackRange(int ki) const
        {
            if (ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()))
            {
                const auto & known = m_knowledge.units()[static_cast<std::size_t>(ki)];
                return {std::max(1, known.minRange), std::max(1, known.maxRange)};
            }
            return {1, 1};
        }

        static bool tileEnterable(const MobilityCostGrid & grid, TilePoint t)
        {
            if (!grid.onGrid(t.x, t.y)) return false;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                if (grid.cost(t.x, t.y, dir) != IMPASSABLE_COST)
                    return true;
            }
            return false;
        }

        std::vector<std::int32_t> buildReversePathDistances(
            int gridId, std::span<const TilePoint> targets, int minRange = 0, int maxRange = 0) const
        {
            const std::size_t cellCount = static_cast<std::size_t>(m_local.width) * static_cast<std::size_t>(m_local.height);
            std::vector<std::int32_t> dist(cellCount, std::numeric_limits<std::int32_t>::max());
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) || targets.empty())
                return dist;
            const auto & grid = m_local.grids[static_cast<std::size_t>(gridId)];
            const auto idxOf = [&](TilePoint t) {
                return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) + static_cast<std::size_t>(t.x);
            };
            using Node = std::pair<std::int32_t, TilePoint>;
            const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
            std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
            for (const TilePoint t : targets)
            {
                if (!grid.onGrid(t.x, t.y)) continue;
                if (minRange > 1)
                {
                    const int x0 = std::max(0, t.x - maxRange);
                    const int x1 = std::min(m_local.width - 1, t.x + maxRange);
                    const int y0 = std::max(0, t.y - maxRange);
                    const int y1 = std::min(m_local.height - 1, t.y + maxRange);
                    for (int y = y0; y <= y1; ++y)
                    {
                        for (int x = x0; x <= x1; ++x)
                        {
                            const TilePoint p{x, y};
                            const int d = manhattanDistance(p, t);
                            if (d < minRange || d > maxRange || !tileEnterable(grid, p)) continue;
                            if (dist[idxOf(p)] > 0)
                            {
                                dist[idxOf(p)] = 0;
                                pq.push({0, p});
                            }
                        }
                    }
                }
                else
                {
                    if (minRange == 0 || tileEnterable(grid, t))
                    {
                        if (dist[idxOf(t)] > 0)
                        {
                            dist[idxOf(t)] = 0;
                            pq.push({0, t});
                        }
                    }
                    if (maxRange >= 1)
                    {
                        for (const EntryDirection dir : ENTRY_DIRECTIONS)
                        {
                            const StepVector step = entryStep(dir);
                            const TilePoint adj{t.x + step.dx, t.y + step.dy};
                            if (tileEnterable(grid, adj) && dist[idxOf(adj)] > 1)
                            {
                                dist[idxOf(adj)] = 1;
                                pq.push({1, adj});
                            }
                        }
                    }
                }
            }
            while (!pq.empty())
            {
                const auto [d, cur] = pq.top();
                pq.pop();
                if (d > dist[idxOf(cur)]) continue;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint prev{cur.x - step.dx, cur.y - step.dy};
                    if (!grid.onGrid(prev.x, prev.y)) continue;
                    const std::int32_t c = grid.cost(cur.x, cur.y, dir);
                    if (c == IMPASSABLE_COST) continue;
                    const std::int32_t nd = d + c;
                    if (nd < dist[idxOf(prev)])
                    {
                        dist[idxOf(prev)] = nd;
                        pq.push({nd, prev});
                    }
                }
            }
            return dist;
        }

        int cachedSetPathDistance(const std::map<int, std::vector<std::int32_t>> & table,
                                  std::span<const TilePoint> fallbackTargets, int ki, TilePoint dest) const
        {
            if (ki >= 0 && ki < static_cast<int>(m_local.units.size()) &&
                m_local.width > 0 && m_local.height > 0 &&
                dest.x >= 0 && dest.x < m_local.width && dest.y >= 0 && dest.y < m_local.height)
            {
                const int gridId = pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]);
                if (const auto it = table.find(gridId); it != table.end())
                {
                    const std::size_t idx = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(dest.x);
                    if (idx < it->second.size() && it->second[idx] < std::numeric_limits<std::int32_t>::max())
                        return it->second[idx];
                }
            }
            int minDist = m_maxDist;
            for (const TilePoint g : fallbackTargets)
                minDist = std::min(minDist, manhattanDistance(dest, g));
            return m_maxDist * 2 + minDist;
        }

        bool canUnitReachAttack(int ki, TilePoint from, TilePoint target, int moveBudget,
                                int minRange, int maxRange) const
        {
            const int manh = manhattanDistance(from, target);
            if (minRange > 1)
            {
                if (manh >= minRange && manh <= maxRange) return true;
                if (moveBudget <= 0 || manh > moveBudget + maxRange) return false;
                const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                    ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
                if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) ||
                    m_local.width <= 0 || m_local.height <= 0 ||
                    from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height)
                    return manh <= moveBudget + maxRange;
                const auto key = std::tuple{gridId, target.x, target.y, minRange, maxRange};
                auto it = m_indirectTilePathCache.find(key);
                if (it == m_indirectTilePathCache.end())
                {
                    it = m_indirectTilePathCache.emplace(
                        key, buildReversePathDistances(gridId, std::span<const TilePoint>(&target, 1), minRange, maxRange)).first;
                }
                const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(from.x);
                return idx < it->second.size() && it->second[idx] <= moveBudget;
            }
            if (manh > moveBudget + maxRange) return false;
            const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()))
                return manh <= moveBudget + maxRange;
            return tilePathDistance(ki, from, target, true) <= moveBudget + maxRange;
        }

        void buildPathDistances()
        {
            if (m_local.width <= 0 || m_local.height <= 0)
                return;
            for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
            {
                const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
                if (!pawn.present || pawn.enemy) continue;
                const int gridId = pawnTravelGrid(pawn);
                if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size())) continue;
                if (!m_enemyPositions.empty())
                {
                    const auto [minR, maxR] = unitAttackRange(ki);
                    const auto profKey = std::tuple{gridId, minR, maxR};
                    if (!m_enemyPathDistByProfile.contains(profKey))
                        m_enemyPathDistByProfile.emplace(
                            profKey, buildReversePathDistances(gridId, m_enemyPositions, minR, maxR));
                }
                if (!m_friendlyRepairSites.empty() && !m_repairPathDistByGrid.contains(gridId))
                    m_repairPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_friendlyRepairSites));
                if (!m_unownedProperties.empty() && !m_unownedPropPathDistByGrid.contains(gridId))
                    m_unownedPropPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_unownedProperties));
                if (!m_unownedFactories.empty() && !m_unownedFactoryPathDistByGrid.contains(gridId))
                    m_unownedFactoryPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_unownedFactories));
            }
        }

        // Flags uninterruptible HQ capture starts and prices exact income swings on other own properties.
        std::pair<bool, MilliFunds> computeOwnPropertyCaptureRisk(const ReplyPassEvaluation & eval) const
        {
            if (m_ownCapturableProperties.empty() || m_local.width <= 0 || m_local.height <= 0 ||
                (!m_local.weapon && !m_local.weaponSlots))
                return {true, 0};
            const std::int32_t capPts = m_stockField.capturePointsToCapture();
            const std::int32_t horizon = m_stockField.horizonTurns();
            if (horizon <= 1) return {true, 0};

            struct FriendlyResponder
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int moveBudget{0};
                int strikeRange{1};
                int maxRange{1};
            };
            std::vector<TilePoint> friendlyOccupied;
            std::vector<FriendlyResponder> responders;
            std::vector<std::uint8_t> actorKiSeen(m_local.units.size(), 0);
            const auto addFriendlyResponder = [&](int ki, TilePoint pos, double hpF) {
                if (hpF <= 0.0 || pos == INVALID_TILE) return;
                friendlyOccupied.push_back(pos);
                if (ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                    ki >= static_cast<int>(m_knowledge.units().size()))
                    return;
                const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
                const int minR = std::max(1, knownF.minRange);
                const int maxR = std::max(1, knownF.maxRange);
                const int moveB = minR > 1 ? 0 : std::max(0, fp.movement);
                responders.push_back({ki, pos, hpF, moveB, minR, maxR});
            };
            for (const auto & actor : eval.actors)
            {
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                if (ki >= 0 && ki < static_cast<int>(actorKiSeen.size()))
                    actorKiSeen[static_cast<std::size_t>(ki)] = 1;
                if (actor.stepsAfterReply <= 0) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                double hpF = static_cast<double>(actor.stepsAfterReply);
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hpF = it->second;
                addFriendlyResponder(ki, cand.destination, hpF);
            }
            for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
            {
                if (actorKiSeen[static_cast<std::size_t>(ki)] != 0) continue;
                const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                if (!fp.present || fp.enemy || fp.tile == INVALID_TILE) continue;
                double hpF = fp.realHp >= 0.0
                    ? fp.realHp
                    : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded);
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hpF = it->second;
                addFriendlyResponder(ki, fp.tile, hpF);
            }

            struct EnemyCapper
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int steps{0};
                int rate{0};
                int movement{0};
                int grid{-1};
            };
            std::vector<EnemyCapper> enemyCappers;
            for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
            {
                const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                if (!ep.present || !ep.enemy || !ep.capper || ep.movement <= 0) continue;
                const bool firedE0 = std::ranges::any_of(eval.shots, [&](const ReplyPassShot & s) {
                    return s.shooterIndex == e;
                });
                if (firedE0) continue;
                double hpE = ep.realHp >= 0.0
                    ? ep.realHp
                    : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded);
                if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                    hpE = static_cast<double>(it->second);
                else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                    hpE = static_cast<double>(itOwn->second);
                if (hpE <= 0.0) continue;
                const int stepsE = ceilHpSteps(hpE);
                const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                const int rateE = replyCaptureRate(knownE.captureRate, std::max(0, knownE.hpRounded - stepsE), false);
                if (rateE <= 0 || rateE * 2 < capPts) continue;
                enemyCappers.push_back({e, ep.tile, hpE, stepsE, rateE, ep.movement, pawnTravelGrid(ep)});
            }
            if (enemyCappers.empty()) return {true, 0};

            const auto isFriendlyZoc = [&](TilePoint t) {
                return std::ranges::any_of(friendlyOccupied, [&](TilePoint f) {
                    return manhattanDistance(f, t) == 1;
                });
            };
            const auto isFriendlyOccupied = [&](TilePoint t) {
                return std::ranges::any_of(friendlyOccupied, [&](TilePoint f) { return f == t; });
            };
            const std::size_t cellCount = static_cast<std::size_t>(m_local.width) * static_cast<std::size_t>(m_local.height);
            const auto idxOf = [&](TilePoint t) {
                return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) + static_cast<std::size_t>(t.x);
            };
            const auto enemyCapperCanReach = [&](const EnemyCapper & ec, TilePoint goal) {
                if (ec.pos == goal) return true;
                if (manhattanDistance(ec.pos, goal) > ec.movement) return false;
                if (ec.grid < 0 || ec.grid >= static_cast<int>(m_local.grids.size())) return true;
                const auto & grid = m_local.grids[static_cast<std::size_t>(ec.grid)];
                if (!grid.onGrid(ec.pos.x, ec.pos.y) || !grid.onGrid(goal.x, goal.y)) return false;
                std::vector<std::int32_t> dist(cellCount, std::numeric_limits<std::int32_t>::max());
                using Node = std::pair<std::int32_t, TilePoint>;
                const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
                std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
                dist[idxOf(ec.pos)] = 0;
                pq.push({0, ec.pos});
                while (!pq.empty())
                {
                    const auto [d, cur] = pq.top();
                    pq.pop();
                    if (cur == goal) return true;
                    if (d > dist[idxOf(cur)]) continue;
                    if (cur != ec.pos && isFriendlyZoc(cur)) continue;
                    for (const EntryDirection dir : ENTRY_DIRECTIONS)
                    {
                        const StepVector step = entryStep(dir);
                        const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                        if (!grid.onGrid(next.x, next.y) || isFriendlyOccupied(next)) continue;
                        const std::int32_t c = grid.cost(next.x, next.y, dir);
                        if (c == IMPASSABLE_COST) continue;
                        const std::int32_t nd = d + c;
                        if (nd <= ec.movement && nd < dist[idxOf(next)])
                        {
                            dist[idxOf(next)] = nd;
                            pq.push({nd, next});
                        }
                    }
                }
                return false;
            };

            bool hqCaptureStartSafe = true;
            MilliFunds totalRisk = 0;
            for (const auto & prop : m_ownCapturableProperties)
            {
                if (isFriendlyOccupied(prop.tile)) continue;
                const EnemyCapper * strongestThreat = nullptr;
                for (const auto & ec : enemyCappers)
                {
                    if (!enemyCapperCanReach(ec, prop.tile)) continue;
                    if (!strongestThreat || ec.rate > strongestThreat->rate ||
                        (ec.rate == strongestThreat->rate && ec.hp > strongestThreat->hp))
                        strongestThreat = &ec;
                }
                if (!strongestThreat) continue;
                const TerrainDefenseRule defRule = m_local.defense
                    ? m_local.defense(strongestThreat->ki, prop.tile) : TerrainDefenseRule{};
                bool canInterrupt = false;
                for (const auto & fr : responders)
                {
                    if (!canUnitReachAttack(fr.ki, fr.pos, prop.tile, fr.moveBudget, fr.strikeRange, fr.maxRange))
                        continue;
                    const double baseDmg = maxWeaponBaseDamage(fr.ki, strongestThreat->ki, fr.strikeRange);
                    if (baseDmg <= 0.0) continue;
                    const double hit = engineShotDamage(terrainReducedDamage(
                        defRule, baseDmg, fr.hp, strongestThreat->steps));
                    if (hit <= 0.0) continue;
                    const double remHp = engineHpAfterReply(strongestThreat->hp, hit);
                    const int remSteps = ceilHpSteps(remHp);
                    const int nextRate = replyCaptureRate(
                        strongestThreat->rate, std::max(0, strongestThreat->steps - remSteps), remSteps <= 0);
                    if (strongestThreat->rate + nextRate < capPts)
                    {
                        canInterrupt = true;
                        break;
                    }
                }
                if (!canInterrupt)
                {
                    if (prop.isHq)
                    {
                        hqCaptureStartSafe = false;
                    }
                    else
                    {
                        const MilliFunds swing = incomeSwing(
                            prop.income, std::max(1, horizon - 1), OwnerSign::Ours, OwnerSign::Enemy);
                        const MilliFunds loss = swing < 0 ? -swing : swing;
                        RelativeStockDetail::checkedAdd(totalRisk, loss);
                    }
                }
            }
            return {hqCaptureStartSafe, totalRisk};
        }

        struct OwnCapturableProperty
        {
            TilePoint tile{INVALID_TILE};
            PropertyIncome income{};
            bool isHq{false};
        };

        const MasterAssignmentProblem & m_problem;
        const BattlefieldKnowledge & m_knowledge;
        PropertyStockField m_stockField;
        ReplyStockContext m_stockContext;
        bool m_stockTuned{false};
        ReplyStockFloor m_baselineFloor;
        std::span<const PropertyFacts> m_properties;
        CombatTerrain & m_local;
        const ReplyBoard & m_actingBoard;
        std::int64_t m_replayTariff{0};
        std::int64_t & m_work;
        HqRankFn m_hqRankFn;
        BuildSlotsFn m_buildSlotsFn;
        int m_maxDist{1};
        int m_sampleCapperKi{NO_UNIT};
        std::vector<TilePoint> m_enemyPositions;
        std::vector<TilePoint> m_unownedProperties;
        std::vector<TilePoint> m_unownedNeutralProperties;
        std::vector<TilePoint> m_unownedFactories;
        std::vector<TilePoint> m_deliverableNeutralProperties;
        std::vector<TilePoint> m_enemyFactories;
        std::vector<TilePoint> m_friendlyRepairSites;
        std::vector<OwnCapturableProperty> m_ownCapturableProperties;
        std::map<std::tuple<int, int, int>, std::vector<std::int32_t>> m_enemyPathDistByProfile;
        std::map<int, std::vector<std::int32_t>> m_repairPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_unownedPropPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_unownedFactoryPathDistByGrid;
        mutable std::map<std::tuple<int, int, int, bool>, std::vector<std::int32_t>> m_tilePathDistCache;
        mutable std::map<std::tuple<int, int, int, int, int>, std::vector<std::int32_t>> m_indirectTilePathCache;
        std::map<std::pair<ContinuationKey, std::vector<std::uint8_t>>, MilliFunds> m_seqStockCache;
        int m_evals{0};
        int m_pruned{0};
    };
}
