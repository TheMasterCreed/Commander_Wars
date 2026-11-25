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
    inline constexpr MilliFunds UNIFIED_FACTORY_SLOT_VALUE = 1'500'000;
    inline constexpr MilliFunds UNIFIED_NEUTRAL_PROPERTY_INCOME = 1'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_BUDGET = 240'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_PER_ACTOR = 15'000'000;
    inline constexpr std::int64_t UNIFIED_FINAL_REPLAY_WORK_FLOOR = 2'000'000;
    inline constexpr std::int32_t UNIFIED_INFANTRY_BASE_MOVE = 3;
    inline constexpr std::int32_t UNIFIED_STOCK_TUNE_ROUNDS = 40;
    inline constexpr std::int64_t UNIFIED_STAGING_ENEMY_WEIGHT = 100;
    inline constexpr std::int64_t UNIFIED_STAGING_PROPERTY_WEIGHT = 40;
    inline constexpr std::int64_t UNIFIED_STAGING_THREAT_BONUS = 250;
    inline constexpr std::int64_t UNIFIED_STAGING_TERRAIN_STAR_WEIGHT = 15;
    inline constexpr std::int64_t UNIFIED_STAGING_CAPTURE_BONUS = 200;
    inline constexpr std::int64_t UNIFIED_STAGING_REPAIR_BONUS = 180;
    inline constexpr std::int32_t UNIFIED_TRAP_FOLLOWUP_TURNS = 2;
    inline constexpr std::int32_t UNIFIED_ENEMY_FACTORY_GUARD_RADIUS = 4;
    inline constexpr double UNIFIED_TRAP_MIN_BASE_DAMAGE = 1.0;
    inline constexpr std::int32_t UNIFIED_CRIPPLED_UNIT_HP = 6;
    inline constexpr double UNIFIED_CRIPPLED_DEFENDER_HP = 4.0;
    inline constexpr double UNIFIED_COVER_MIN_DAMAGE = 30.0;
    inline constexpr std::int32_t UNIFIED_VIABLE_ENEMY_CAPPER_HP = 7;
    inline constexpr std::int32_t UNIFIED_STAGING_DESTROYED_FLAG = 32;
    inline constexpr std::int32_t UNIFIED_STAGING_CAPTURE_FLAG = 1;

    struct UnifiedTeamScore
    {
        bool valid{false};
        std::tuple<TerminalClass, int, int, MilliFunds> worstHqRank{
            TerminalClass::ForcedLoss, std::numeric_limits<int>::min(),
            std::numeric_limits<int>::min(), std::numeric_limits<MilliFunds>::min()};
        MilliFunds worstEconomicValue{std::numeric_limits<MilliFunds>::min()};
        std::int64_t stagingScore{std::numeric_limits<std::int64_t>::min()};
        MilliFunds realizedMaterial{0};
        MilliFunds postReplyStock{0};
        MilliFunds enemyDenial{0};
        MilliFunds factoryValue{0};
        int affordableBuildSlots{0};
        TacticalReply bindingPolicy{TacticalReply::Economic};
        int bindingTargetUnit{NO_UNIT};

        bool strictlyBetterThan(const UnifiedTeamScore & other) const
        {
            if (!valid) return false;
            if (!other.valid) return true;
            return std::tie(worstHqRank, worstEconomicValue, stagingScore) >
                   std::tie(other.worstHqRank, other.worstEconomicValue, other.stagingScore);
        }

        bool notBetterThan(const UnifiedTeamScore & floor) const
        {
            if (!valid || !floor.valid) return false;
            return std::tie(worstHqRank, worstEconomicValue, stagingScore) <=
                   std::tie(floor.worstHqRank, floor.worstEconomicValue, floor.stagingScore);
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
                    m_unownedProperties.push_back({prop.x, prop.y});
                if (prop.visible && prop.canProduce && prop.ownerId != NO_OWNER && prop.ownerId != knowledge.observerId())
                    m_enemyFactories.push_back({prop.x, prop.y});
                if (prop.visible && prop.ownerId == knowledge.observerId() && !prop.canProduce)
                    m_friendlyRepairSites.push_back({prop.x, prop.y});
            }
        }

        std::int64_t stagingScore(std::span<const int> choices) const
        {
            std::vector<TilePoint> claimedProperties;
            for (const auto & member : m_problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
                const int opt = choices[static_cast<std::size_t>(slot)];
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
                if (cand.capture.has_value() ||
                    (member.stockRow != NO_STOCK_ROW &&
                     std::ranges::find(m_unownedProperties, cand.destination) != m_unownedProperties.end()))
                    claimedProperties.push_back(cand.destination);
            }
            std::int64_t total = 0;
            for (const auto & member : m_problem.actors())
            {
                const int slot = member.canonicalSlot;
                if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
                const int opt = choices[static_cast<std::size_t>(slot)];
                if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
                const TilePoint dest = cand.destination;
                const int ki = member.knowledgeUnitIndex;
                int minEnemyDist = m_maxDist;
                for (const TilePoint e : m_enemyPositions)
                    minEnemyDist = std::min(minEnemyDist, manhattanDistance(dest, e));
                int minPropDist = m_maxDist;
                bool foundUnclaimed = false;
                for (const TilePoint p : m_unownedProperties)
                {
                    if (member.stockRow != NO_STOCK_ROW && p != dest &&
                        std::ranges::find(claimedProperties, p) != claimedProperties.end())
                        continue;
                    foundUnclaimed = true;
                    minPropDist = std::min(minPropDist, manhattanDistance(dest, p));
                }
                if (!foundUnclaimed && member.stockRow != NO_STOCK_ROW)
                    minPropDist = minEnemyDist;
                int stars = 0;
                if (m_local.defense && ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                    stars = m_local.defense(ki, dest).stars;
                if (member.stockRow == NO_STOCK_ROW)
                {
                    const auto & unit = m_knowledge.units()[static_cast<std::size_t>(ki)];
                    const int nextTurnThreatReach = unit.movementPoints + std::max(1, unit.maxRange);
                    const std::int64_t threatBonus =
                        (!m_enemyPositions.empty() && minEnemyDist <= nextTurnThreatReach)
                            ? UNIFIED_STAGING_THREAT_BONUS : 0;
                    total += static_cast<std::int64_t>(m_maxDist - minEnemyDist) * UNIFIED_STAGING_ENEMY_WEIGHT;
                    total += static_cast<std::int64_t>(m_maxDist - minPropDist) * UNIFIED_STAGING_PROPERTY_WEIGHT;
                    total += threatBonus;
                    total += static_cast<std::int64_t>(stars) * UNIFIED_STAGING_TERRAIN_STAR_WEIGHT;
                }
                else
                {
                    const bool damaged = ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()) &&
                        m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded < UNIT_HP_STEPS;
                    const bool heavilyDamaged = ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()) &&
                        m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded <= UNIFIED_CRIPPLED_UNIT_HP;
                    if (heavilyDamaged && !cand.capture.has_value() && !m_friendlyRepairSites.empty())
                    {
                        int minRepairDist = m_maxDist;
                        for (const TilePoint r : m_friendlyRepairSites)
                            minRepairDist = std::min(minRepairDist, manhattanDistance(dest, r));
                        total += static_cast<std::int64_t>(m_maxDist - minRepairDist) * UNIFIED_STAGING_PROPERTY_WEIGHT;
                    }
                    else
                    {
                        total += static_cast<std::int64_t>(m_maxDist - minPropDist) * UNIFIED_STAGING_PROPERTY_WEIGHT;
                    }
                    total += static_cast<std::int64_t>(stars) * UNIFIED_STAGING_TERRAIN_STAR_WEIGHT;
                    if (cand.capture.has_value())
                        total += UNIFIED_STAGING_CAPTURE_BONUS;
                    if (damaged &&
                        std::ranges::find(m_friendlyRepairSites, dest) != m_friendlyRepairSites.end())
                        total += UNIFIED_STAGING_REPAIR_BONUS;
                }
            }
            return total;
        }

        UnifiedTeamScore evaluate(std::span<const int> choices, const UnifiedTeamScore * pruneFloor = nullptr,
                                  ReplyPassEvaluation * economicOut = nullptr)
        {
            UnifiedTeamScore summary;
            const std::int64_t staging = stagingScore(choices);
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
                const auto eval = evaluateReplyPassPlan(m_problem, choices, m_actingBoard);
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
                const int buildSlots = m_buildSlotsFn(eval);
                const MilliFunds factoryVal = static_cast<MilliFunds>(buildSlots) * UNIFIED_FACTORY_SLOT_VALUE;

                MilliFunds econTotal = realized;
                if (!RelativeStockDetail::checkedAdd(econTotal, postStock) ||
                    !RelativeStockDetail::checkedAdd(econTotal, denial) ||
                    !RelativeStockDetail::checkedAdd(econTotal, factoryVal))
                    return UnifiedTeamScore{};

                UnifiedTeamScore point;
                point.valid = true;
                point.worstHqRank = *hqRank;
                point.worstEconomicValue = econTotal;
                point.stagingScore = staging;
                point.realizedMaterial = realized;
                point.postReplyStock = postStock;
                point.enemyDenial = denial;
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
                int reach{0};
                int strikeRange{1};
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
                        const bool indirect = knownF.minRange > 1;
                        const int maxRange = std::max(1, knownF.maxRange);
                        const int reach = indirect ? maxRange : (std::max(0, fp.movement) + maxRange);
                        double hpF = static_cast<double>(actor.stepsAfterReply);
                        if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                            hpF = it->second;
                        if (reach > 0 && hpF > 0.0)
                            coverUnits.push_back({ki, cand.destination, hpF, reach, std::max(1, knownF.minRange)});
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
                int reach{0};
                int strikeRange{1};
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
                // Keep pre-E0 position and full initial HP unless critically crippled in F0 to avoid lure/chip cliffs.
                const double effectiveHp = hpOwn <= UNIFIED_CRIPPLED_DEFENDER_HP ? hpOwn : initialHp;
                const auto & knownE = m_knowledge.units()[static_cast<std::size_t>(e)];
                const bool indirect = knownE.minRange > 1;
                const int maxRange = std::max(1, knownE.maxRange);
                const int reach = indirect ? maxRange : (std::max(0, ep.movement) + maxRange);
                if (reach <= 0) continue;
                defenders.push_back({e, ep.tile, effectiveHp, reach, std::max(1, knownE.minRange)});
            }
            for (std::size_t n = 0; n < nodeCount; ++n)
            {
                if (finishingNextTurn[n]) continue;
                const TilePoint tile = m_stockContext.nodeTile(n);
                if (tile == INVALID_TILE) continue;
                const bool nearEnemyFactory = std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                    return manhattanDistance(f, tile) <= UNIFIED_ENEMY_FACTORY_GUARD_RADIUS;
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
                    if (manhattanDistance(d.pos, tile) > d.reach) continue;
                    const bool coveredByFriendlyArmor = std::ranges::any_of(coverUnits, [&](const FriendlyCover & fc) {
                        if (manhattanDistance(fc.pos, tile) > fc.reach) return false;
                        const double scaledDmg =
                            maxWeaponBaseDamage(fc.ki, d.ki, fc.strikeRange) * (fc.hp / static_cast<double>(UNIT_HP_STEPS));
                        return scaledDmg >= UNIFIED_COVER_MIN_DAMAGE;
                    });
                    if (coveredByFriendlyArmor) continue;
                    const double baseDmg = maxWeaponBaseDamage(d.ki, m_sampleCapperKi, d.strikeRange);
                    if (baseDmg < UNIFIED_TRAP_MIN_BASE_DAMAGE) continue;
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
                    key, m_baselineFloor.status == ReplyStockStatus::Valid ? &m_baselineFloor : nullptr, blocked);
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

        // Prices E1 follow-up loss when a combat unit or crippled capper takes damage and has no safe F1 escape.
        MilliFunds computeFollowupTrapLoss(const ReplyPassEvaluation & eval) const
        {
            if (m_local.width <= 0 || m_local.height <= 0 || (!m_local.weapon && !m_local.weaponSlots))
                return 0;
            struct DamagedUnit
            {
                int ki{NO_UNIT};
                TilePoint tile{INVALID_TILE};
                double hpAfterE0{0.0};
                int stepsAfterE0{0};
            };
            std::vector<DamagedUnit> victims;
            for (const auto & actor : eval.actors)
            {
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                if (ki < 0 || ki >= static_cast<int>(m_local.units.size())) continue;
                const int initialSteps = m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded;
                if (actor.stepsAfterReply <= 0 || actor.stepsAfterReply >= initialSteps) continue;
                if (member.stockRow != NO_STOCK_ROW && actor.stepsAfterReply > UNIFIED_CRIPPLED_UNIT_HP) continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                double hp = static_cast<double>(actor.stepsAfterReply);
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hp = it->second;
                if (hp <= 0.0) continue;
                victims.push_back({ki, cand.destination, hp, actor.stepsAfterReply});
            }
            if (victims.empty()) return 0;

            struct EnemyThreat
            {
                int ki{NO_UNIT};
                TilePoint pos{INVALID_TILE};
                double hp{0.0};
                int reach{0};
                int strikeRange{1};
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
                const bool indirect = knownE.minRange > 1;
                const int maxRange = std::max(1, knownE.maxRange);
                const int moveE = std::max(0, ep.movement);
                const int followupMoves = (!firedE0 && !ep.capper) ? UNIFIED_TRAP_FOLLOWUP_TURNS : 1;
                const int reachE = indirect
                    ? (firedE0 ? maxRange : moveE + maxRange)
                    : (followupMoves * moveE + maxRange);
                if (reachE <= 0) continue;
                threats.push_back({e, posE, hpE, reachE, std::max(1, knownE.minRange), ep.replacementCost});
            }
            if (threats.empty()) return 0;

            MilliFunds totalTrapLoss = 0;
            const std::size_t cellCount = static_cast<std::size_t>(m_local.width) * static_cast<std::size_t>(m_local.height);
            for (const auto & v : victims)
            {
                const auto & pawn = m_local.units[static_cast<std::size_t>(v.ki)];
                if (pawn.grid < 0 || pawn.grid >= static_cast<int>(m_local.grids.size()) || pawn.movement <= 0)
                    continue;
                const auto & grid = m_local.grids[static_cast<std::size_t>(pawn.grid)];
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
                    const TerrainDefenseRule defRule = m_local.defense ? m_local.defense(v.ki, q) : TerrainDefenseRule{};
                    MilliFunds worstAtQ = 0;
                    for (const auto & th : threats)
                    {
                        if (manhattanDistance(th.pos, q) > th.reach) continue;
                        const double baseDmg = maxWeaponBaseDamage(th.ki, v.ki, th.strikeRange);
                        if (baseDmg < UNIFIED_TRAP_MIN_BASE_DAMAGE) continue;
                        const double rawHit = terrainReducedDamage(defRule, baseDmg, th.hp, v.stepsAfterE0);
                        const double hit = engineShotDamage(rawHit);
                        if (hit <= 0.0) continue;
                        const double afterU = engineHpAfterReply(v.hpAfterE0, hit);
                        const MilliFunds capLoss = combatCapitalLoss(pawn.replacementCost, v.hpAfterE0, afterU);
                        MilliFunds counterLoss = 0;
                        if (th.strikeRange == 1 && afterU > 0.0)
                        {
                            const double cBase = maxWeaponBaseDamage(v.ki, th.ki, 1);
                            if (cBase >= UNIFIED_TRAP_MIN_BASE_DAMAGE)
                            {
                                const TerrainDefenseRule eDef = m_local.defense ? m_local.defense(th.ki, q) : TerrainDefenseRule{};
                                const double rawC = sameActionCounterDamage(eDef, cBase, v.hpAfterE0, rawHit, ceilHpSteps(th.hp));
                                const double eAfter = engineHpAfterReply(th.hp, engineShotDamage(rawC));
                                counterLoss = combatCapitalLoss(th.cost, th.hp, eAfter);
                            }
                        }
                        const MilliFunds netEnemyGain = capLoss - counterLoss;
                        if (netEnemyGain > worstAtQ)
                            worstAtQ = netEnemyGain;
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
                    const bool viableCapper = hasKnown &&
                        (capturing ||
                         m_knowledge.units()[static_cast<std::size_t>(targetKi)].hpRounded >= UNIFIED_VIABLE_ENEMY_CAPPER_HP);
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
            return total;
        }

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
        std::vector<TilePoint> m_enemyFactories;
        std::vector<TilePoint> m_friendlyRepairSites;
        std::map<std::pair<ContinuationKey, std::vector<std::uint8_t>>, MilliFunds> m_seqStockCache;
        int m_evals{0};
        int m_pruned{0};
    };
}
